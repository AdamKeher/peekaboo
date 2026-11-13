"use strict";

const assert = require("node:assert/strict");
const test = require("node:test");
const { EventEmitter } = require("node:events");
const {
  PeekabooSession,
  quote,
  normalisePath,
  decodeSr,
  assignTarget,
} = require("../adapter/session");

/** A fake MiClient: records commands, answers from a scripted table. */
class FakeMi extends EventEmitter {
  constructor(replies = {}) {
    super();
    this.sent = [];
    this.consoled = [];
    this.replies = replies;
  }
  async send(command) {
    this.sent.push(command);
    for (const [prefix, reply] of Object.entries(this.replies)) {
      if (command.startsWith(prefix)) {
        if (reply instanceof Error) throw reply;
        return { class: "done", results: reply, streams: [] };
      }
    }
    return { class: "done", results: {}, streams: [] };
  }
  async console(command) {
    this.consoled.push(command);
    for (const [prefix, text] of Object.entries(this.consoleReplies || {})) {
      if (command.startsWith(prefix)) return text;
    }
    return "";
  }
  stop() {}
}

function makeSession(replies, consoleReplies) {
  const messages = [];
  const session = new PeekabooSession((m) => messages.push(m));
  session.mi = new FakeMi(replies);
  session.mi.consoleReplies = consoleReplies;
  session.wireMi(session.mi); // the same wiring onLaunch installs
  return { session, messages, mi: session.mi };
}

const req = (command, args, seq = 1) => ({ type: "request", seq, command, arguments: args });
const lastResponse = (messages) => [...messages].reverse().find((m) => m.type === "response");
const eventsOf = (messages, name) => messages.filter((m) => m.type === "event" && m.event === name);

test("initialize advertises what is actually implemented", async () => {
  const { session, messages } = makeSession();
  await session.handleMessage(req("initialize", {}));
  const body = lastResponse(messages).body;
  assert.equal(body.supportsConfigurationDoneRequest, true);
  assert.equal(body.supportsDisassembleRequest, true);
  assert.equal(body.supportsEvaluateForHovers, true);
  // Reverse execution is offered here, but Peekaboo only advertises it over RSP when frame
  // history is recording, and gdb only exposes it when the target did -- so the buttons are
  // live exactly when they will work.
  assert.equal(body.supportsStepBack, true);
  // "initialized" must NOT be sent yet: it tells VS Code to replay its saved breakpoints, and
  // until launch has run there is no gdb to send them to. onLaunch sends it instead.
  assert.equal(eventsOf(messages, "initialized").length, 0);
});

test("the stop gdb reports while attaching is not shown as a pause", async () => {
  // Connecting to a halted target makes gdb announce a stop. Forwarding it would paint the
  // session as paused before VS Code has even been told it exists, at a ROM address that means
  // nothing to the user.
  const { session, messages, mi } = makeSession();
  session.attaching = true;
  mi.emit("stopped", { reason: "signal-received", "signal-name": "SIGTRAP" });
  assert.equal(eventsOf(messages, "stopped").length, 0);

  session.attaching = false;
  mi.emit("stopped", { reason: "breakpoint-hit", bkptno: "1" });
  assert.equal(eventsOf(messages, "stopped").length, 1, "real stops still get through");
});

test("configurationDone resumes a plain launch", async () => {
  // Attaching halts the machine (the `?` packet interrupts it), which is right for gdb and
  // wrong for F5: without resuming here the session sits stopped in Kickstart, seconds before
  // the program has even loaded, and the user has to press Continue once before any breakpoint
  // can be hit. Resuming is also what makes gdb send its Z0 packets at all.
  const { session, messages, mi } = makeSession();
  session.launchArgs = {};
  await session.handleMessage(req("configurationDone", {}));
  assert.equal(lastResponse(messages).success, true);
  assert.ok(mi.sent.includes("-exec-continue"), "must resume the target");
  assert.equal(eventsOf(messages, "stopped").length, 0, "and must not claim it is stopped");
  assert.equal(session.running, true);
});

test("configurationDone stays stopped for stopOnEntry and for attach", async () => {
  for (const args of [{ stopOnEntry: true }, { attach: true }]) {
    const { session, messages, mi } = makeSession();
    session.launchArgs = args;
    await session.handleMessage(req("configurationDone", {}));
    assert.ok(!mi.sent.includes("-exec-continue"), JSON.stringify(args));
    const stopped = eventsOf(messages, "stopped");
    assert.equal(stopped.length, 1, JSON.stringify(args));
    assert.equal(stopped[0].body.reason, "entry");
    assert.equal(session.running, false);
  }
});

test("threads reports the single 68000 context", async () => {
  const { session, messages } = makeSession();
  await session.handleMessage(req("threads", {}));
  assert.deepEqual(lastResponse(messages).body.threads, [{ id: 1, name: "68000" }]);
});

test("stackTrace maps MI frames and keeps source paths", async () => {
  const { session, messages } = makeSession({
    "-stack-list-frames": {
      // Real MI key, captured from gdb: "stack", not "frames".
      stack: [
        { level: "0", addr: "0x00c0ee0a", func: "crass_vector_render_frame",
          file: "crass_vector.c", fullname: "C:\\a\\crass_vector.c", line: "830" },
        { level: "1", addr: "0x00c0d19a", func: "main", file: "main.c",
          fullname: "C:\\a\\main.c", line: "81" },
      ],
    },
  });
  await session.handleMessage(req("stackTrace", { threadId: 1 }));
  const frames = lastResponse(messages).body.stackFrames;
  assert.equal(frames.length, 2);
  assert.equal(frames[0].name, "crass_vector_render_frame");
  assert.equal(frames[0].line, 830);
  assert.equal(frames[0].source.name, "crass_vector.c");
  assert.equal(frames[0].instructionPointerReference, "0x00c0ee0a");
  assert.notEqual(frames[0].id, frames[1].id); // distinct handles
});

test("a frame with no source still produces a usable name", async () => {
  // ROM and library addresses have no symbols; the stack must not break there.
  const { session, messages } = makeSession({
    "-stack-list-frames": { stack: [{ level: "0", addr: "0x00f0ff60" }] },
  });
  await session.handleMessage(req("stackTrace", { threadId: 1 }));
  const frame = lastResponse(messages).body.stackFrames[0];
  assert.equal(frame.name, "0x00f0ff60");
  assert.equal(frame.source, undefined);
});

test("scopes offers Locals, Globals and Registers", async () => {
  const { session, messages } = makeSession();
  await session.handleMessage(req("scopes", { frameId: 1 }));
  const scopes = lastResponse(messages).body.scopes;
  assert.deepEqual(scopes.map((s) => s.name), ["Locals", "Globals", "Registers"]);
  assert.ok(scopes.every((s) => s.variablesReference > 0));
});

test("locals preserve <optimized out> rather than hiding it", async () => {
  // The honest answer on -Ofast builds, and the user needs to see it.
  const { session, messages } = makeSession({
    "-stack-list-variables": {
      variables: [
        { name: "i", value: "0", type: "int" },
        { name: "draw", value: "<optimized out>", type: "UBYTE *" },
      ],
    },
  });
  await session.handleMessage(req("scopes", { frameId: 1 }));
  const localsRef = lastResponse(messages).body.scopes[0].variablesReference;
  await session.handleMessage(req("variables", { variablesReference: localsRef }, 2));
  const vars = lastResponse(messages).body.variables;
  assert.equal(vars[0].name, "i");
  assert.equal(vars[0].value, "0");
  assert.equal(vars[1].value, "<optimized out>");
});

test("registers are named, not numbered", async () => {
  const { session, messages } = makeSession({
    "-data-list-register-names": { "register-names": ["d0", "d1", "sp", "pc"] },
    "-data-list-register-values": {
      "register-values": [
        { number: "0", value: "0x11110000" },
        { number: "3", value: "0x00c0cb68" },
      ],
    },
  });
  await session.handleMessage(req("scopes", { frameId: 1 }));
  const regRef = lastResponse(messages).body.scopes[2].variablesReference;
  await session.handleMessage(req("variables", { variablesReference: regRef }, 2));
  const vars = lastResponse(messages).body.variables;
  assert.deepEqual(vars, [
    { name: "d0", value: "0x11110000", variablesReference: 0 },
    { name: "pc", value: "0x00c0cb68", variablesReference: 0 },
  ]);
});

// The MI shape here is copied from a real -symbol-info-variables reply against
// tests/images/a.elf; inventing one is how a parser ends up agreeing with a fixture and
// nothing else.
const GLOBALS_REPLY = {
  symbols: {
    debug: [
      {
        filename: "C:/src/voxel.c",
        fullname: "C:/src/voxel.c",
        symbols: [
          { line: "97", name: "vox_map64", type: "UBYTE *" },
          { line: "95", name: "vox_s_alloc", type: "UBYTE *[2]" },
        ],
      },
      {
        filename: "C:/src/printf.c",
        fullname: "C:/src/printf.c",
        symbols: [{ line: "26", name: "stdout_putp", type: "void *" }],
      },
    ],
    nondebug: [
      { address: "0x00030ee6", name: "scroll_fp" },
      { address: "0x00030edb", name: "_edata" },
      { address: "0x00030edc", name: "__bss_start" },
    ],
  },
};

async function openGlobals(session, messages, seq = 2) {
  await session.handleMessage(req("scopes", { frameId: 1 }));
  const ref = lastResponse(messages).body.scopes[1].variablesReference;
  await session.handleMessage(req("variables", { variablesReference: ref }, seq));
  return lastResponse(messages).body.variables;
}

test("globals are grouped by file, with linker symbols left out", async () => {
  const { session, messages } = makeSession({
    "-symbol-info-variables": GLOBALS_REPLY,
  });
  const groups = await openGlobals(session, messages);
  assert.deepEqual(
    groups.map((g) => g.name),
    ["printf.c", "voxel.c", "(no debug info)"]
  );
  assert.equal(groups[0].value, "1 variable");
  assert.equal(groups[1].value, "2 variables");
  // _edata and __bss_start are addresses the linker invented, not variables anyone wants.
  assert.equal(groups[2].value, "1 variable");
});

test("a global is read through its file-qualified name, not the bare one", async () => {
  // Two translation units can each hold a `static int count`; the bare name resolves to
  // whichever gdb happens to see first, which is the wrong one half the time.
  const { session, messages, mi } = makeSession({
    "-symbol-info-variables": GLOBALS_REPLY,
    "-var-create": { name: "pv0", numchild: "0", value: "0x4d8fe", type: "UBYTE *" },
  });
  const groups = await openGlobals(session, messages);
  const voxel = groups.find((g) => g.name === "voxel.c");
  await session.handleMessage(req("variables", { variablesReference: voxel.variablesReference }, 3));
  assert.ok(mi.sent.some((c) => c.includes("'voxel.c'::vox_map64")));
});

test("a global LTO stripped of its DWARF location is recovered from .symtab", async () => {
  // The whole reason the Globals list is worth having on this toolchain: the compiler drops
  // the location and leaves the storage exactly where it was. Address plus declared type
  // reads a real value out of a row that would otherwise just say <optimized out>.
  const { session, messages, mi } = makeSession(
    {
      "-symbol-info-variables": GLOBALS_REPLY,
      "-var-create pv0": { name: "pv0", numchild: "0", value: "<optimized out>" },
      "-var-create pv1": { name: "pv1", numchild: "1", value: "0xc00020", type: "void *" },
    },
    { "info address stdout_putp": 'Symbol "stdout_putp" is static storage at address 0x4d9a4.\n' }
  );
  const groups = await openGlobals(session, messages);
  const printf = groups.find((g) => g.name === "printf.c");
  await session.handleMessage(req("variables", { variablesReference: printf.variablesReference }, 3));
  const vars = lastResponse(messages).body.variables;
  assert.equal(vars[0].name, "stdout_putp");
  assert.equal(vars[0].value, "0xc00020");
  assert.equal(vars[0].memoryReference, "0x4d9a4");
  assert.ok(mi.sent.some((c) => c.includes("{void *} 0x4d9a4")));
});

test("a global with no location and no symbol stays honestly optimized out", async () => {
  const { session, messages } = makeSession(
    {
      "-symbol-info-variables": GLOBALS_REPLY,
      "-var-create": { name: "pv0", numchild: "0", value: "<optimized out>" },
    },
    { "info address": 'Symbol "stdout_putp" is optimized out.\n' }
  );
  const groups = await openGlobals(session, messages);
  const printf = groups.find((g) => g.name === "printf.c");
  await session.handleMessage(req("variables", { variablesReference: printf.variablesReference }, 3));
  assert.equal(lastResponse(messages).body.variables[0].value, "<optimized out>");
});

test("a symtab-only global still offers its bytes", async () => {
  const { session, messages } = makeSession({ "-symbol-info-variables": GLOBALS_REPLY });
  const groups = await openGlobals(session, messages);
  const bare = groups.find((g) => g.name === "(no debug info)");
  await session.handleMessage(req("variables", { variablesReference: bare.variablesReference }, 3));
  const vars = lastResponse(messages).body.variables;
  assert.equal(vars[0].name, "scroll_fp");
  assert.match(vars[0].value, /no debug info/);
  assert.equal(vars[0].memoryReference, "0x00030ee6");
});

test("a single-file program shows its globals without a folder to open first", async () => {
  const { session, messages } = makeSession({
    "-symbol-info-variables": {
      symbols: {
        debug: [
          { filename: "main.c", fullname: "main.c", symbols: [{ name: "frame", type: "int" }] },
        ],
      },
    },
    "-var-create": { name: "pv0", numchild: "0", value: "42" },
  });
  const vars = await openGlobals(session, messages);
  assert.equal(vars.length, 1);
  assert.equal(vars[0].name, "frame");
  assert.equal(vars[0].value, "42");
});

test("a gdb too old for -symbol-info-variables leaves Globals empty, not broken", async () => {
  const { session, messages } = makeSession({
    "-symbol-info-variables": new Error("Undefined MI command"),
  });
  assert.deepEqual(await openGlobals(session, messages), []);
});

test("one source file split across CUs by LTO is one entry, not several", async () => {
  // Real behaviour, not a hypothetical: -symbol-info-variables against tests/images/a.elf
  // reports goth_scroll.c twice, with its 22 variables arbitrarily divided 5/17 between them.
  const { session, messages } = makeSession({
    "-symbol-info-variables": {
      symbols: {
        debug: [
          { filename: "C:/src/scroll.c", fullname: "C:/src/scroll.c", symbols: [{ name: "a" }] },
          {
            filename: "C:/src/scroll.c",
            fullname: "C:/src/scroll.c",
            symbols: [{ name: "b" }, { name: "a" }],
          },
          { filename: "C:/src/other.c", fullname: "C:/src/other.c", symbols: [{ name: "c" }] },
        ],
      },
    },
  });
  const groups = await openGlobals(session, messages);
  assert.deepEqual(groups.map((g) => g.name), ["other.c", "scroll.c"]);
  assert.equal(groups[1].value, "2 variables", "a and b, with the repeated a counted once");
});

test("an aggregate LTO hollowed out does not get to look live", async () => {
  // gdb prints `{...}` for a struct straight off the type, whether or not there is any storage
  // behind it -- so the row reads as a real value while every field inside says <optimized
  // out>. The missing address is the only tell.
  const { session, messages } = makeSession(
    {
      "-symbol-info-variables": {
        symbols: {
          debug: [
            {
              filename: "sys.c",
              fullname: "sys.c",
              symbols: [{ name: "backup", type: "struct SystemBackup" }],
            },
          ],
        },
      },
      "-var-create": { name: "pv0", numchild: "8", value: "{...}", type: "struct SystemBackup" },
      "-data-evaluate-expression": new Error(
        'Can\'t take address of "backup" which isn\'t an lvalue.'
      ),
    },
    { "info address": 'Symbol "backup" is optimized out.\n' }
  );
  const vars = await openGlobals(session, messages);
  assert.equal(vars[0].value, "{...} <optimized out>");
  assert.equal(vars[0].memoryReference, undefined);
});

test("an aggregate that does have storage keeps its plain value and its address", async () => {
  const { session, messages } = makeSession({
    "-symbol-info-variables": {
      symbols: {
        debug: [
          { filename: "sys.c", fullname: "sys.c", symbols: [{ name: "lut", type: "WORD [1024]" }] },
        ],
      },
    },
    "-var-create": { name: "pv0", numchild: "1024", value: "[1024]", type: "WORD [1024]" },
    "-data-evaluate-expression": { value: "0x4d000 <lut>" },
  });
  const vars = await openGlobals(session, messages);
  assert.equal(vars[0].value, "[1024]");
  assert.equal(vars[0].memoryReference, "0x4d000");
  assert.ok(vars[0].variablesReference > 0);
});

test("a pointer carries its address so the byte view can open it", async () => {
  const { session, messages } = makeSession({
    "-stack-list-variables": {
      variables: [
        { name: "chip", value: "0x00c00020", type: "UWORD *" },
        { name: "i", value: "3", type: "int" },
      ],
    },
  });
  await session.handleMessage(req("scopes", { frameId: 1 }));
  const localsRef = lastResponse(messages).body.scopes[0].variablesReference;
  await session.handleMessage(req("variables", { variablesReference: localsRef }, 2));
  const vars = lastResponse(messages).body.variables;
  assert.equal(vars[0].memoryReference, "0x00c00020");
  // A scalar has no address worth a round-trip to find, and the byte view of an int is noise.
  assert.equal(vars[1].memoryReference, undefined);
});

test("sr expands into flags instead of hex to decode in your head", async () => {
  const { session, messages } = makeSession({
    "-data-list-register-names": { "register-names": ["d0", "sr"] },
    "-data-list-register-values": {
      "register-values": [
        { number: "0", value: "0x11110000" },
        { number: "1", value: "0x2704" },
      ],
    },
  });
  await session.handleMessage(req("scopes", { frameId: 1 }));
  const regRef = lastResponse(messages).body.scopes[2].variablesReference;
  await session.handleMessage(req("variables", { variablesReference: regRef }, 2));
  const vars = lastResponse(messages).body.variables;
  assert.equal(vars[0].variablesReference, 0, "d0 is just a number");
  assert.ok(vars[1].variablesReference > 0, "sr expands");

  await session.handleMessage(req("variables", { variablesReference: vars[1].variablesReference }, 3));
  const flags = lastResponse(messages).body.variables;
  const byName = Object.fromEntries(flags.map((f) => [f.name, f.value]));
  // 0x2704: supervisor, IPL 7, Z set.
  assert.equal(byName["S (supervisor)"], "1");
  assert.equal(byName["IPL (interrupt mask)"], "7");
  assert.equal(byName["Z (zero)"], "1");
  assert.equal(byName["C (carry)"], "0");
  assert.equal(byName["T (trace)"], "0");
});

test("decodeSr refuses a value that is not a number", () => {
  assert.equal(decodeSr("<unavailable>"), null);
});

test("editing a struct field assigns to the field, not to something of that name", async () => {
  // The child arrives as "y" against the parent's handle. `set var y=5` finds whatever y means
  // in the current frame -- which is not this field.
  const { session, messages, mi } = makeSession({
    "-stack-list-variables": { variables: [{ name: "pos", type: "Point" }] },
    "-var-create": { name: "pv0", numchild: "2", value: "{...}", type: "Point" },
    "-var-list-children": {
      children: [
        { name: "pv0.x", exp: "x", value: "1", type: "int", numchild: "0" },
        { name: "pv0.y", exp: "y", value: "2", type: "int", numchild: "0" },
      ],
    },
    "-var-assign": { value: "5" },
  });
  await session.handleMessage(req("scopes", { frameId: 1 }));
  const localsRef = lastResponse(messages).body.scopes[0].variablesReference;
  await session.handleMessage(req("variables", { variablesReference: localsRef }, 2));
  const posRef = lastResponse(messages).body.variables[0].variablesReference;
  await session.handleMessage(req("variables", { variablesReference: posRef }, 3));

  await session.handleMessage(
    req("setVariable", { variablesReference: posRef, name: "y", value: "5" }, 4)
  );
  assert.ok(mi.sent.includes('-var-assign pv0.y "5"'));
  assert.equal(lastResponse(messages).body.value, "5");
});

test("editing a register writes the register, not a symbol that happens to share its name", () => {
  assert.equal(assignTarget({ kind: "registers" }, "d0"), "$d0");
  assert.equal(assignTarget({ kind: "locals" }, "d0"), "d0");
  const group = { entries: [{ name: "frame", expression: "'main.c'::frame" }] };
  assert.equal(assignTarget({ kind: "globals-file", group }, "frame"), "'main.c'::frame");
});

test("editing a local selects its frame first", async () => {
  // Without this, editing a local while a caller is selected in the Call Stack pane writes to
  // the innermost frame's variable of that name instead.
  const { session, messages, mi } = makeSession({
    "-data-evaluate-expression": { value: "9" },
  });
  await session.handleMessage(req("scopes", { frameId: 1 }));
  const localsRef = lastResponse(messages).body.scopes[0].variablesReference;
  session.refs.get(localsRef).level = 2;
  await session.handleMessage(
    req("setVariable", { variablesReference: localsRef, name: "i", value: "9" }, 2)
  );
  assert.ok(mi.sent.includes("-stack-select-frame 2"));
  assert.ok(mi.sent.includes("-gdb-set var i=9"));
});

test("setExpression makes the Watch pane editable", async () => {
  const { session, messages, mi } = makeSession({
    "-data-evaluate-expression": { value: "7" },
  });
  await session.handleMessage(req("initialize", {}));
  assert.equal(lastResponse(messages).body.supportsSetExpression, true);

  await session.handleMessage(
    req("setExpression", { expression: "vox_map64[3]", value: "7" }, 2)
  );
  assert.ok(mi.sent.includes("-gdb-set var vox_map64[3]=7"));
  assert.equal(lastResponse(messages).body.value, "7");
});

test("setBreakpoints reports when gdb moved the line", async () => {
  // The whole point of this project: a breakpoint that silently lands elsewhere is a lie.
  const { session, messages } = makeSession({
    "-break-insert": {
      bkpt: { number: "1", line: "836", fullname: "C:\\a\\copper.c", addr: "0x00c0ee0a" },
    },
  });
  await session.handleMessage(
    req("setBreakpoints", { source: { path: "C:\\a\\copper.c" }, breakpoints: [{ line: 830 }] })
  );
  const bp = lastResponse(messages).body.breakpoints[0];
  assert.equal(bp.verified, true);
  assert.equal(bp.line, 836);
  assert.match(bp.message, /generated no code/);
  assert.match(bp.message, /836/);
});

test("setBreakpoints says nothing extra when the line bound exactly", async () => {
  const { session, messages } = makeSession({
    "-break-insert": { bkpt: { number: "1", line: "830", fullname: "C:\\a\\copper.c" } },
  });
  await session.handleMessage(
    req("setBreakpoints", { source: { path: "C:\\a\\copper.c" }, breakpoints: [{ line: 830 }] })
  );
  const bp = lastResponse(messages).body.breakpoints[0];
  assert.equal(bp.line, 830);
  assert.equal(bp.message, undefined);
});

test("a pending breakpoint is reported unverified, not as a solid red dot", async () => {
  // Captured from real gdb: a line that generated no code comes back as a SUCCESS with
  // addr="<PENDING>" and no line field. Trusting that paints a verified breakpoint in the
  // editor for something that can never fire.
  const { session, messages } = makeSession({
    "-break-insert": {
      bkpt: {
        number: "1", type: "breakpoint", enabled: "y", addr: "<PENDING>",
        pending: "C:/a/crass_vector.c:3", times: "0",
      },
    },
  });
  await session.handleMessage(
    req("setBreakpoints", { source: { path: "C:/a/crass_vector.c" }, breakpoints: [{ line: 3 }] })
  );
  const bp = lastResponse(messages).body.breakpoints[0];
  assert.equal(bp.verified, false);
  assert.equal(bp.line, 3);
  assert.match(bp.message, /never fire/);
});

test("setBreakpoints flags an inlined multi-location breakpoint", async () => {
  const { session, messages } = makeSession({
    "-break-insert": { bkpt: { number: "1", line: "830", addr: "<MULTIPLE>" } },
  });
  await session.handleMessage(
    req("setBreakpoints", { source: { path: "C:\\a\\c.c" }, breakpoints: [{ line: 830 }] })
  );
  assert.match(lastResponse(messages).body.breakpoints[0].message, /inlined into several/);
});

test("a breakpoint gdb rejects is reported unverified, not dropped", async () => {
  const { session, messages } = makeSession({
    "-break-insert": new Error("No line 9999 in the current file."),
  });
  await session.handleMessage(
    req("setBreakpoints", { source: { path: "C:\\a\\c.c" }, breakpoints: [{ line: 9999 }] })
  );
  const bp = lastResponse(messages).body.breakpoints[0];
  assert.equal(bp.verified, false);
  assert.match(bp.message, /No line 9999/);
});

test("setBreakpoints passes conditions and hit counts to gdb", async () => {
  const { session, mi } = makeSession({
    "-break-insert": { bkpt: { number: "1", line: "830" } },
  });
  await session.handleMessage(
    req("setBreakpoints", {
      source: { path: "/a/c.c" },
      breakpoints: [{ line: 830, condition: "i == 3", hitCondition: "5" }],
    })
  );
  const cmd = mi.sent.find((c) => c.startsWith("-break-insert"));
  assert.match(cmd, /-c "i == 3"/);
  assert.match(cmd, /-i 5/);
});

test("continue reports optimistically and emits continued", async () => {
  const { session, messages } = makeSession();
  await session.handleMessage(req("continue", { threadId: 1 }));
  assert.equal(lastResponse(messages).success, true);
  assert.equal(eventsOf(messages, "continued").length, 1);
});

test("instruction granularity picks the instruction-level MI verbs", async () => {
  const { session, mi } = makeSession();
  await session.handleMessage(req("next", { threadId: 1, granularity: "instruction" }));
  assert.ok(mi.sent.includes("-exec-next-instruction"));

  await session.handleMessage(req("next", { threadId: 1 }, 2));
  assert.ok(mi.sent.includes("-exec-next"));
});

test("stepOut maps to finish", async () => {
  const { session, mi } = makeSession();
  await session.handleMessage(req("stepOut", { threadId: 1 }));
  assert.ok(mi.sent.includes("-exec-finish"));
});

test("a stopped notification becomes a DAP stopped event with the right reason", async () => {
  const { session, messages, mi } = makeSession();
  mi.emit("stopped", { reason: "breakpoint-hit", bkptno: "2", frame: { line: "830" } });
  const stopped = eventsOf(messages, "stopped");
  assert.equal(stopped.length, 1);
  assert.equal(stopped[0].body.reason, "breakpoint");
  assert.deepEqual(stopped[0].body.hitBreakpointIds, [2]);
});

test("a SIGTRAP from a Peekaboo-side stop is not an exception", async () => {
  // Peekaboo answers every stop with T05. When gdb cannot match the PC to a breakpoint of its
  // own -- one set in Peekaboo, or a pause taken there -- it reports a plain signal, and VS Code
  // paints "Exception has occurred: Trace/breakpoint trap" over an ordinary stop.
  const { session, messages, mi } = makeSession();
  mi.emit("stopped", { reason: "signal-received", "signal-name": "SIGTRAP" });
  const stopped = eventsOf(messages, "stopped")[0];
  assert.equal(stopped.body.reason, "pause");
  assert.match(stopped.body.description, /Peekaboo/);
});

test("an interrupt reads as a pause, and real faults still raise an exception", async () => {
  const interrupted = makeSession();
  interrupted.mi.emit("stopped", { reason: "signal-received", "signal-name": "SIGINT" });
  assert.equal(eventsOf(interrupted.messages, "stopped")[0].body.reason, "pause");

  const faulted = makeSession();
  faulted.mi.emit("stopped", {
    reason: "signal-received",
    "signal-name": "SIGSEGV",
    "signal-meaning": "Segmentation fault",
  });
  const stopped = eventsOf(faulted.messages, "stopped")[0];
  assert.equal(stopped.body.reason, "exception");
  assert.equal(stopped.body.text, "Segmentation fault");
});

test("watchpoint and stepping stops map to their DAP reasons", async () => {
  for (const [miReason, dapReason] of [
    ["watchpoint-trigger", "data breakpoint"],
    ["end-stepping-range", "step"],
    ["function-finished", "step"],
    ["signal-received", "exception"],
  ]) {
    const { session, messages, mi } = makeSession();
    mi.emit("stopped", { reason: miReason });
    assert.equal(eventsOf(messages, "stopped")[0].body.reason, dapReason, miReason);
  }
});

test("an exiting target terminates the session instead of reporting a stop", async () => {
  const { session, messages, mi } = makeSession();
  mi.emit("stopped", { reason: "exited-normally", exitcode: "0" });
  assert.equal(eventsOf(messages, "stopped").length, 0);
  assert.equal(eventsOf(messages, "exited").length, 1);
  assert.equal(eventsOf(messages, "terminated").length, 1);
});

test("stopping invalidates stale frame handles", async () => {
  const { session, messages, mi } = makeSession({
    "-stack-list-frames": { stack: [{ level: "0", func: "main", line: "81" }] },
  });
  await session.handleMessage(req("stackTrace", { threadId: 1 }));
  const staleId = lastResponse(messages).body.stackFrames[0].id;
  mi.emit("stopped", { reason: "end-stepping-range" });
  // Handles from the previous stop must not resolve, or scopes would describe the wrong frame.
  assert.equal(session.refs.has(staleId), false);
});

test("evaluate routes -exec to a raw gdb command", async () => {
  const { session, messages, mi } = makeSession();
  await session.handleMessage(req("evaluate", { expression: "-exec monitor sections" }));
  assert.deepEqual(mi.consoled, ["monitor sections"]);
  assert.equal(lastResponse(messages).success, true);
});

test("a failed hover evaluation is quiet", async () => {
  // Hovers fire on every identifier under the cursor; most are not expressions.
  const { session, messages } = makeSession({
    "-data-evaluate-expression": new Error("No symbol \"the\" in current context."),
  });
  await session.handleMessage(req("evaluate", { expression: "the", context: "hover" }));
  const response = lastResponse(messages);
  assert.equal(response.success, true);
  assert.equal(response.body.result, "");
});

test("a failed console evaluation reports the error", async () => {
  const { session, messages } = makeSession({
    "-data-evaluate-expression": new Error("No symbol \"nope\" in current context."),
  });
  await session.handleMessage(req("evaluate", { expression: "nope", context: "repl" }));
  const response = lastResponse(messages);
  assert.equal(response.success, false);
  assert.match(response.message, /No symbol/);
});

test("readMemory returns base64 and reports unreadable bytes", async () => {
  const { session, messages } = makeSession({
    "-data-read-memory-bytes": { memory: [{ begin: "0xc0cb68", contents: "48e73830" }] },
  });
  await session.handleMessage(req("readMemory", { memoryReference: "0xc0cb68", count: 4 }));
  const body = lastResponse(messages).body;
  assert.equal(Buffer.from(body.data, "base64").toString("hex"), "48e73830");
  assert.equal(body.unreadableBytes, 0);
});

test("disassemble flattens source-and-asm output", async () => {
  const { session, messages } = makeSession({
    "-data-disassemble": {
      asm_insns: [
        {
          line: "830", file: "crass_vector.c", fullname: "C:\\a\\crass_vector.c",
          line_asm_insn: [
            { address: "0x00c0ee0a", inst: "move.l d0,(a1)", opcodes: "2280" },
            { address: "0x00c0ee0c", inst: "addq.l #4,a1", opcodes: "5889" },
          ],
        },
      ],
    },
  });
  await session.handleMessage(
    req("disassemble", { memoryReference: "0x00c0ee0a", instructionCount: 2 })
  );
  const insns = lastResponse(messages).body.instructions;
  assert.equal(insns.length, 2);
  assert.equal(insns[0].instruction, "move.l d0,(a1)");
  assert.equal(insns[0].line, 830);
  assert.equal(insns[0].location.name, "crass_vector.c");
});

test("requests before a session exists fail cleanly", async () => {
  const messages = [];
  const session = new PeekabooSession((m) => messages.push(m)); // no mi attached
  await session.handleMessage(req("stackTrace", { threadId: 1 }));
  const response = lastResponse(messages);
  assert.equal(response.success, false);
  assert.match(response.message, /no debug session/);
});

test("quote escapes what would otherwise end an MI string", () => {
  assert.equal(quote("plain"), '"plain"');
  assert.equal(quote('say "hi"'), '"say \\"hi\\""');
  assert.equal(quote("C:\\a\\b.c"), '"C:\\\\a\\\\b.c"');
});

test("normalisePath ignores separator style and case", () => {
  assert.equal(normalisePath("C:\\A\\B.c"), normalisePath("c:/a/b.c"));
});

test("warns when a breakpoint covers only part of an unrolled line", async () => {
  // The case a single address hides: one source line, four disjoint stretches of code, so
  // execution can reach the line without ever passing the address gdb chose. Captured from the
  // real crass_vector.c:830, which the optimiser unrolled.
  const { session, messages } = makeSession(
    { "-break-insert": { bkpt: { number: "1", line: "830", addr: "0xc0ee14" } } },
    {
      "monitor resolve-line": [
        "line 830 has code at 4 site(s):",
        "  $000022a2-$000022a8  runtime $00C0EE0A",
        "  $000022a8-$000022ac  runtime $00C0EE10",
        "  $000022b8-$000022c0  runtime $00C0EE20",
        "  $000022d0-$000022d8  runtime $00C0EE38",
        "",
      ].join("\n"),
    }
  );
  await session.handleMessage(
    req("setBreakpoints", { source: { path: "/a/crass_vector.c" }, breakpoints: [{ line: 830 }] })
  );
  const bp = lastResponse(messages).body.breakpoints[0];
  assert.equal(bp.verified, true);
  assert.match(bp.message, /4 separate stretches/);
  assert.match(bp.message, /without stopping here/);
});

test("says nothing extra when a line produced exactly one stretch", async () => {
  const { session, messages } = makeSession(
    { "-break-insert": { bkpt: { number: "1", line: "822", addr: "0xc0ed3a" } } },
    {
      "monitor resolve-line":
        "line 822 has code at 1 site(s):\n  $000021d2-$000021da  runtime $00C0ED3A  (stmt)\n",
    }
  );
  await session.handleMessage(
    req("setBreakpoints", { source: { path: "/a/crass_vector.c" }, breakpoints: [{ line: 822 }] })
  );
  assert.equal(lastResponse(messages).body.breakpoints[0].message, undefined);
});

test("an unbindable breakpoint suggests lines that do have code", async () => {
  const { session, messages } = makeSession(
    { "-break-insert": { bkpt: { number: "1", addr: "<PENDING>", pending: "/a/c.c:3" } } },
    { "monitor resolve-line": "line 3 generated no code\n  lines with code nearby: 12 18 22\n" }
  );
  await session.handleMessage(
    req("setBreakpoints", { source: { path: "/a/c.c" }, breakpoints: [{ line: 3 }] })
  );
  const bp = lastResponse(messages).body.breakpoints[0];
  assert.equal(bp.verified, false);
  assert.match(bp.message, /never fire/);
  assert.match(bp.message, /12, 18, 22/);
});

test("an older Peekaboo without the audit degrades quietly", async () => {
  // The audit is advisory. If it is unavailable, gdb's own answer still stands.
  const { session, messages } = makeSession(
    { "-break-insert": { bkpt: { number: "1", line: "830", addr: "0xc0ee14" } } },
    { "monitor resolve-line": "unknown monitor command: resolve-line\n" }
  );
  await session.handleMessage(
    req("setBreakpoints", { source: { path: "/a/c.c" }, breakpoints: [{ line: 830 }] })
  );
  const bp = lastResponse(messages).body.breakpoints[0];
  assert.equal(bp.verified, true);
  assert.equal(bp.message, undefined);
  assert.equal(session.lineAuditUnavailable, true);
});

test("stepBack and reverseContinue use gdb's reverse verbs", async () => {
  const { session, mi } = makeSession();
  await session.handleMessage(req("stepBack", { threadId: 1 }));
  assert.ok(mi.sent.includes("-exec-step --reverse"));

  await session.handleMessage(req("reverseContinue", { threadId: 1 }, 2));
  assert.ok(mi.sent.includes("-exec-continue --reverse"));
});

test("a breakpoint set while running is applied, not deferred", async () => {
  // gdb refuses to modify a running target. Without interrupt/insert/resume the breakpoint is
  // held by gdb and never reaches Peekaboo: the gutter shows it and it never fires.
  const { session, mi, messages } = makeSession({
    "-break-insert": { bkpt: { number: "1", line: "830", addr: "0xc0ee0a" } },
  });
  session.running = true; // as if Continue had been pressed

  const done = session.handleMessage(
    req("setBreakpoints", { source: { path: "/a/c.c" }, breakpoints: [{ line: 830 }] })
  );
  // The adapter interrupts; the stop arrives asynchronously, exactly as gdb would report it.
  await new Promise((r) => setTimeout(r, 10));
  mi.emit("stopped", { reason: "signal-received", "signal-name": "SIGINT" });
  await done;

  assert.ok(mi.sent.includes("-exec-interrupt"), "must interrupt before inserting");
  assert.ok(mi.sent.some((c) => c.startsWith("-break-insert")), "must insert the breakpoint");
  assert.ok(mi.sent.includes("-exec-continue"), "must resume afterwards");
  assert.equal(lastResponse(messages).body.breakpoints[0].verified, true);

  // The interrupt was ours, so VS Code must not be told the user's program paused.
  const stops = eventsOf(messages, "stopped");
  assert.equal(stops.length, 0, "the transient stop must be suppressed");
});

test("a breakpoint set while stopped does not interrupt", async () => {
  const { session, mi } = makeSession({
    "-break-insert": { bkpt: { number: "1", line: "830", addr: "0xc0ee0a" } },
  });
  session.running = false;
  await session.handleMessage(
    req("setBreakpoints", { source: { path: "/a/c.c" }, breakpoints: [{ line: 830 }] })
  );
  assert.ok(!mi.sent.includes("-exec-interrupt"), "no need to interrupt an already-stopped target");
  assert.ok(!mi.sent.includes("-exec-continue"), "and it must not start running by itself");
});
