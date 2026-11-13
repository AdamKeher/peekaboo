"use strict";
// @ts-check

/**
 * Parser for GDB's machine interface (MI3) output.
 *
 * Written against real output captured from m68k-amiga-elf-gdb rather than from the manual,
 * because the shapes that break naive parsers only show up in practice: tuples containing lists
 * (`bkpt={...,thread-groups=["i1"],...}`), lists whose elements are themselves name=value results
 * (`frames=[frame={...},frame={...}]`), and C-escaped Windows paths where every separator is a
 * doubled backslash (`fullname="C:\\src\\..."`).
 *
 * Record types, by leading character:
 *   ^  result        ^done / ^error,msg="..." / ^running / ^connected / ^exit
 *   *  exec-async    *stopped,reason="breakpoint-hit",...
 *   +  status-async
 *   =  notify-async  =thread-group-added,id="i1"
 *   ~  console stream    the text a user would see
 *   @  target stream     output from the program itself
 *   &  log stream        gdb's own logging
 *   (gdb)               prompt, marks the end of a response
 * A record may carry a numeric token prefix that matches it to the command that caused it.
 */

/** @typedef {{type:"result"|"exec"|"status"|"notify", token:number|null, class:string, results:Record<string,any>}} AsyncOrResultRecord */
/** @typedef {{type:"stream", kind:"console"|"target"|"log", text:string}} StreamRecord */
/** @typedef {{type:"prompt"}} PromptRecord */
/** @typedef {AsyncOrResultRecord|StreamRecord|PromptRecord|null} MiRecord */

const STREAM_KINDS = { "~": "console", "@": "target", "&": "log" };
const ASYNC_TYPES = { "*": "exec", "+": "status", "=": "notify" };

/**
 * Decode a C string literal, starting at the opening quote.
 * @returns {[string, number]} the text and the index just past the closing quote
 */
function parseCString(text, start) {
  let i = start + 1; // skip the opening quote
  let out = "";
  while (i < text.length) {
    const c = text[i];
    if (c === '"') {
      return [out, i + 1];
    }
    if (c !== "\\") {
      out += c;
      i += 1;
      continue;
    }
    const esc = text[i + 1];
    i += 2;
    switch (esc) {
      case "n": out += "\n"; break;
      case "t": out += "\t"; break;
      case "r": out += "\r"; break;
      case "\\": out += "\\"; break;
      case '"': out += '"'; break;
      case "a": out += "\x07"; break;
      case "b": out += "\b"; break;
      case "f": out += "\f"; break;
      case "v": out += "\v"; break;
      case "0": out += "\0"; break;
      case undefined: break; // trailing backslash: emit nothing rather than throw
      default: out += esc; break;
    }
  }
  // Unterminated string. Returning what we have keeps one malformed line from killing the
  // session; the caller sees a truncated value rather than an exception.
  return [out, i];
}

/** @returns {[any, number]} */
function parseValue(text, start) {
  const c = text[start];
  if (c === '"') return parseCString(text, start);
  if (c === "{") return parseTuple(text, start);
  if (c === "[") return parseList(text, start);
  // Bare token (rare, but MI emits it for some values). Read to the next delimiter.
  let i = start;
  while (i < text.length && !",}]".includes(text[i])) i += 1;
  return [text.slice(start, i), i];
}

/** Reads `name=value`. @returns {[string, any, number]} */
function parseResult(text, start) {
  let i = start;
  while (i < text.length && text[i] !== "=") i += 1;
  const name = text.slice(start, i);
  const [value, next] = parseValue(text, i + 1);
  return [name, value, next];
}

/** @returns {[Record<string,any>, number]} */
function parseTuple(text, start) {
  const out = /** @type {Record<string,any>} */ ({});
  let i = start + 1; // skip '{'
  while (i < text.length && text[i] !== "}") {
    if (text[i] === ",") {
      i += 1;
      continue;
    }
    const [name, value, next] = parseResult(text, i);
    out[name] = value;
    i = next;
  }
  return [out, i + 1];
}

/**
 * @returns {[any[], number]}
 * A list holds either plain values (`["i1"]`) or repeated results (`[frame={..},frame={..}]`).
 * In the second case the names are all identical and carry no information, so the values are
 * what the caller wants.
 */
function parseList(text, start) {
  const out = [];
  let i = start + 1; // skip '['
  while (i < text.length && text[i] !== "]") {
    if (text[i] === ",") {
      i += 1;
      continue;
    }
    // Distinguish `name=value` from a bare value by looking for an '=' before the next
    // delimiter that would end this element.
    let probe = i;
    let isResult = false;
    while (probe < text.length && !",]".includes(text[probe])) {
      if (text[probe] === "=") {
        isResult = true;
        break;
      }
      if ('"{['.includes(text[probe])) break;
      probe += 1;
    }
    if (isResult) {
      const [, value, next] = parseResult(text, i);
      out.push(value);
      i = next;
    } else {
      const [value, next] = parseValue(text, i);
      out.push(value);
      i = next;
    }
  }
  return [out, i + 1];
}

/**
 * Parse one line of MI output.
 * @param {string} rawLine
 * @returns {MiRecord} null for blank lines and anything unrecognised
 */
function parseLine(rawLine) {
  const line = rawLine.replace(/\r$/, "");
  if (line.length === 0) return null;
  // The prompt can arrive with trailing spaces, which is why this is not an equality test.
  if (line.startsWith("(gdb)")) return { type: "prompt" };

  let i = 0;
  let token = null;
  while (i < line.length && line[i] >= "0" && line[i] <= "9") i += 1;
  if (i > 0) token = Number(line.slice(0, i));

  const marker = line[i];
  if (marker in STREAM_KINDS) {
    const [text] = parseCString(line, i + 1);
    return { type: "stream", kind: STREAM_KINDS[marker], text };
  }

  const type = marker === "^" ? "result" : ASYNC_TYPES[marker];
  if (!type) return null;

  let j = i + 1;
  while (j < line.length && line[j] !== ",") j += 1;
  const klass = line.slice(i + 1, j);

  const results = /** @type {Record<string,any>} */ ({});
  let k = j;
  while (k < line.length) {
    if (line[k] === ",") {
      k += 1;
      continue;
    }
    const [name, value, next] = parseResult(line, k);
    results[name] = value;
    k = next;
  }
  return { type, token, class: klass, results };
}

module.exports = { parseLine, parseCString, parseValue };
