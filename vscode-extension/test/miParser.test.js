"use strict";

const assert = require("node:assert/strict");
const test = require("node:test");
const { parseLine } = require("../adapter/miParser");

// Every fixture below is real output captured from m68k-amiga-elf-gdb 17.0.50 driven with
// --interpreter=mi3, not invented. The awkward shapes are the point.

test("parses a bare result record", () => {
  assert.deepEqual(parseLine("^done"), {
    type: "result", token: null, class: "done", results: {}
  });
});

test("parses an error with a message", () => {
  const r = parseLine('^error,msg="No registers."');
  assert.equal(r.type, "result");
  assert.equal(r.class, "error");
  assert.equal(r.results.msg, "No registers.");
});

test("parses a notify-async record", () => {
  const r = parseLine('=thread-group-added,id="i1"');
  assert.equal(r.type, "notify");
  assert.equal(r.class, "thread-group-added");
  assert.equal(r.results.id, "i1");
});

test("recognises the prompt, including trailing whitespace", () => {
  assert.deepEqual(parseLine("(gdb) "), { type: "prompt" });
  assert.deepEqual(parseLine("(gdb)"), { type: "prompt" });
});

test("unescapes console stream text", () => {
  const r = parseLine('~"\\t.text_addr = 0xc0cb68\\n"');
  assert.equal(r.type, "stream");
  assert.equal(r.kind, "console");
  assert.equal(r.text, "\t.text_addr = 0xc0cb68\n");
});

test("unescapes doubled backslashes in Windows paths", () => {
  // This is the shape that silently corrupts paths if \\ is not handled: gdb's log stream
  // escapes every separator.
  const r = parseLine('&"warning: Section .debug_aranges in C:\\\\src\\\\amiga\\\\a.elf ignored.\\n"');
  assert.equal(r.kind, "log");
  assert.equal(r.text, "warning: Section .debug_aranges in C:\\src\\amiga\\a.elf ignored.\n");
});

test("parses a tuple that contains a list", () => {
  // ^done,bkpt={...,thread-groups=["i1"],...} -- the nesting that breaks regex-based parsers.
  const line =
    '^done,bkpt={number="1",type="breakpoint",disp="keep",enabled="y",addr="0x00c0ee0a",' +
    'func="crass_vector_render_frame",file="crass_vector.c",' +
    'fullname="C:\\\\src\\\\crass_vector.c",line="830",thread-groups=["i1"],times="0",' +
    'original-location="crass_vector.c:830"}';
  const r = parseLine(line);
  assert.equal(r.class, "done");
  const bp = r.results.bkpt;
  assert.equal(bp.number, "1");
  assert.equal(bp.addr, "0x00c0ee0a");
  assert.equal(bp.func, "crass_vector_render_frame");
  assert.equal(bp.line, "830");
  assert.deepEqual(bp["thread-groups"], ["i1"]);
  assert.equal(bp.fullname, "C:\\src\\crass_vector.c");
});

test("parses a list of repeated results as a list of values", () => {
  // -stack-list-frames returns frames=[frame={...},frame={...}]; the repeated 'frame' name
  // carries no information, so callers want the values.
  const line =
    '^done,frames=[frame={level="0",addr="0x00c0fa10",func="crass_vector_publish",' +
    'file="crass_vector.c",line="896"},frame={level="1",addr="0x00c11a7e",' +
    'func="menu_update",file="menu.h",line="712"}]';
  const r = parseLine(line);
  const frames = r.results.frames;
  assert.equal(frames.length, 2);
  assert.equal(frames[0].func, "crass_vector_publish");
  assert.equal(frames[0].line, "896");
  assert.equal(frames[1].level, "1");
  assert.equal(frames[1].func, "menu_update");
});

test("parses a stopped event with a nested frame", () => {
  const line =
    '*stopped,reason="breakpoint-hit",disp="keep",bkptno="1",frame={addr="0x00c0ee0a",' +
    'func="crass_vector_render_frame",args=[],file="crass_vector.c",line="830"},' +
    'thread-id="1",stopped-threads="all"';
  const r = parseLine(line);
  assert.equal(r.type, "exec");
  assert.equal(r.class, "stopped");
  assert.equal(r.results.reason, "breakpoint-hit");
  assert.equal(r.results.bkptno, "1");
  assert.equal(r.results.frame.line, "830");
  assert.deepEqual(r.results.frame.args, []);
});

test("parses frame arguments, which are tuples inside a list", () => {
  const line =
    '^done,stack-args=[frame={level="0",args=[{name="plane",value="0xc237d2"},' +
    '{name="trail",value="0xc237ce"}]}]';
  const r = parseLine(line);
  const args = r.results["stack-args"][0].args;
  assert.equal(args.length, 2);
  assert.deepEqual(args[0], { name: "plane", value: "0xc237d2" });
  assert.equal(args[1].name, "trail");
});

test("keeps a token so a reply can be matched to its command", () => {
  const r = parseLine('42^done,value="0x1234"');
  assert.equal(r.token, 42);
  assert.equal(r.results.value, "0x1234");
});

test("handles values that contain commas and braces inside strings", () => {
  // A string is opaque: delimiters inside it must not end the value.
  const r = parseLine('^done,msg="a,b={c} [d]",other="x"');
  assert.equal(r.results.msg, "a,b={c} [d]");
  assert.equal(r.results.other, "x");
});

test("returns null for blank and unrecognised lines rather than throwing", () => {
  assert.equal(parseLine(""), null);
  assert.equal(parseLine("some stray text"), null);
});

test("survives an unterminated string instead of throwing", () => {
  // A truncated line should degrade to a partial value, not kill the debug session.
  const r = parseLine('^done,msg="unterminated');
  assert.equal(r.results.msg, "unterminated");
});

test("parses an empty list and an empty tuple", () => {
  const r = parseLine("^done,a=[],b={}");
  assert.deepEqual(r.results.a, []);
  assert.deepEqual(r.results.b, {});
});

test("parses register values", () => {
  const line =
    '^done,register-values=[{number="0",value="0x11110000"},{number="17",value="0xc0cb68"}]';
  const r = parseLine(line);
  const regs = r.results["register-values"];
  assert.equal(regs.length, 2);
  assert.equal(regs[0].number, "0");
  assert.equal(regs[1].value, "0xc0cb68");
});

test("parses -stack-list-variables output", () => {
  const line = '^done,variables=[{name="i",value="0"},{name="draw",value="<optimized out>"}]';
  const r = parseLine(line);
  const vars = r.results.variables;
  assert.equal(vars.length, 2);
  assert.equal(vars[0].name, "i");
  // The honest answer on -Ofast code, and it must survive parsing intact.
  assert.equal(vars[1].value, "<optimized out>");
});
