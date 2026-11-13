"use strict";
// @ts-check

const { spawn } = require("node:child_process");
const { EventEmitter } = require("node:events");
const { parseLine } = require("./miParser");

/**
 * Drives a gdb process over its machine interface.
 *
 * Commands are numbered so replies can be matched to them: MI interleaves asynchronous
 * notifications with command results, so "the next ^done" is not a safe assumption. Stream
 * records that arrive while a command is in flight are collected and handed back with its
 * result, which is how `monitor offsets` gets its text (the answer arrives as console output,
 * not as a result field).
 *
 * Events:
 *   "stopped"    (results)  the target halted; carries reason, frame, bkptno...
 *   "running"    (results)  the target resumed
 *   "console"    (text)     text a user would see, for the Debug Console
 *   "target"     (text)     output from the debuggee itself (KPrintF arrives here)
 *   "log"        (text)     gdb's own logging
 *   "notify"     (record)   other =async records
 *   "exit"       (code)     gdb terminated
 */
class MiClient extends EventEmitter {
  /**
   * @param {string} gdbPath
   * @param {string[]} [extraArgs]
   */
  constructor(gdbPath, extraArgs = []) {
    super();
    this.gdbPath = gdbPath;
    this.extraArgs = extraArgs;
    this.proc = null;
    this.buffer = "";
    this.nextToken = 1;
    /** @type {Map<number, {resolve:Function, reject:Function, streams:string[]}>} */
    this.pending = new Map();
    this.exited = false;
  }

  start() {
    // -nx keeps a user's ~/.gdbinit from changing behaviour under us; mi3 is what the parser
    // was written against.
    const args = ["-q", "--interpreter=mi3", "-nx", ...this.extraArgs];
    this.proc = spawn(this.gdbPath, args, { stdio: ["pipe", "pipe", "pipe"] });

    this.proc.stdout.setEncoding("utf8");
    this.proc.stdout.on("data", (chunk) => this._onData(chunk));
    this.proc.stderr.setEncoding("utf8");
    this.proc.stderr.on("data", (text) => this.emit("log", text));
    this.proc.on("exit", (code) => {
      this.exited = true;
      // Fail anything still in flight, or its caller waits forever.
      for (const [, entry] of this.pending) {
        entry.reject(new Error("gdb exited before answering"));
      }
      this.pending.clear();
      this.emit("exit", code);
    });
    this.proc.on("error", (err) => this.emit("log", `gdb failed to start: ${err.message}\n`));
  }

  /**
   * Send an MI command and wait for its result record.
   * @param {string} command e.g. "-stack-list-frames 0 20"
   * @returns {Promise<{class:string, results:Record<string,any>, streams:string[]}>}
   */
  send(command) {
    if (!this.proc || this.exited) {
      return Promise.reject(new Error("gdb is not running"));
    }
    const token = this.nextToken++;
    return new Promise((resolve, reject) => {
      this.pending.set(token, { resolve, reject, streams: [] });
      this.proc.stdin.write(`${token}${command}\n`);
    });
  }

  /**
   * Run a plain gdb console command (one gdb does not expose as an MI verb, such as
   * `monitor` or `add-symbol-file`) and return whatever it printed.
   * @param {string} command
   * @returns {Promise<string>}
   */
  async console(command) {
    // The command text is embedded in an MI string literal, so quotes and backslashes in it
    // must be escaped or the command is silently truncated at the first quote.
    const escaped = command.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
    const reply = await this.send(`-interpreter-exec console "${escaped}"`);
    return reply.streams.join("");
  }

  stop() {
    if (this.proc && !this.exited) {
      try {
        this.proc.kill();
      } catch {
        /* already gone */
      }
    }
  }

  _onData(chunk) {
    this.buffer += chunk;
    let index;
    while ((index = this.buffer.indexOf("\n")) >= 0) {
      const line = this.buffer.slice(0, index);
      this.buffer = this.buffer.slice(index + 1);
      this._onLine(line);
    }
  }

  _onLine(line) {
    const record = parseLine(line);
    if (!record) return;

    switch (record.type) {
      case "stream": {
        // Attach stream output to whichever command is in flight, so `console()` can return it.
        // There is at most one outstanding command in practice, because callers await.
        for (const [, entry] of this.pending) entry.streams.push(record.text);
        if (record.kind === "console") this.emit("console", record.text);
        else if (record.kind === "target") this.emit("target", record.text);
        else this.emit("log", record.text);
        return;
      }
      case "result": {
        const entry = record.token !== null ? this.pending.get(record.token) : undefined;
        if (!entry) return; // an unsolicited result, or one we already gave up on
        this.pending.delete(record.token);
        if (record.class === "error") {
          const err = new Error(record.results.msg || "gdb error");
          // Keep the structured form: callers sometimes want to treat "No registers." as an
          // expected state rather than a failure.
          // @ts-ignore -- attaching context to an Error is intentional
          err.miResults = record.results;
          entry.reject(err);
          return;
        }
        entry.resolve({ class: record.class, results: record.results, streams: entry.streams });
        return;
      }
      case "exec": {
        if (record.class === "stopped") this.emit("stopped", record.results);
        else if (record.class === "running") this.emit("running", record.results);
        return;
      }
      case "notify":
      case "status":
        this.emit("notify", record);
        return;
      default:
        return; // prompt
    }
  }
}

module.exports = { MiClient };
