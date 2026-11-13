"use strict";

const assert = require("node:assert/strict");
const test = require("node:test");
const { MiClient } = require("../adapter/miClient");

// Drives the client against a fake gdb: a stand-in process object, so the tests cover the
// command/reply correlation and event routing without needing a real toolchain.
function makeFakeGdb() {
  const written = [];
  const listeners = { stdout: [], stderr: [], exit: [], error: [] };
  const proc = {
    stdin: { write: (s) => written.push(s) },
    stdout: { setEncoding() {}, on: (_e, cb) => listeners.stdout.push(cb) },
    stderr: { setEncoding() {}, on: (_e, cb) => listeners.stderr.push(cb) },
    on: (event, cb) => {
      if (listeners[event]) listeners[event].push(cb);
    },
    kill: () => {},
  };
  return {
    proc,
    written,
    emitStdout: (text) => listeners.stdout.forEach((cb) => cb(text)),
    emitExit: (code) => listeners.exit.forEach((cb) => cb(code)),
  };
}

function makeClient() {
  const fake = makeFakeGdb();
  const client = new MiClient("gdb");
  client.proc = fake.proc;
  // start() would spawn a real process; wire the fake in by hand instead.
  fake.proc.stdout.on("data", (chunk) => client._onData(chunk));
  fake.proc.on("exit", (code) => {
    client.exited = true;
    for (const [, entry] of client.pending) entry.reject(new Error("gdb exited before answering"));
    client.pending.clear();
    client.emit("exit", code);
  });
  return { client, fake };
}

test("numbers commands and resolves the matching reply", async () => {
  const { client, fake } = makeClient();
  const promise = client.send("-stack-list-frames 0 1");
  assert.equal(fake.written[0], "1-stack-list-frames 0 1\n");
  fake.emitStdout('1^done,frames=[frame={level="0",func="main"}]\n(gdb) \n');
  const reply = await promise;
  assert.equal(reply.class, "done");
  assert.equal(reply.results.frames[0].func, "main");
});

test("matches replies by token even when they arrive out of order", async () => {
  // MI interleaves async notifications with results, so "the next ^done" is not safe.
  const { client, fake } = makeClient();
  const first = client.send("-a");
  const second = client.send("-b");
  fake.emitStdout('2^done,which="second"\n');
  fake.emitStdout('1^done,which="first"\n');
  assert.equal((await second).results.which, "second");
  assert.equal((await first).results.which, "first");
});

test("rejects on ^error and carries the message", async () => {
  const { client, fake } = makeClient();
  const promise = client.send("-data-list-register-values x");
  fake.emitStdout('1^error,msg="No registers."\n');
  await assert.rejects(promise, /No registers\./);
});

test("collects stream output and returns it from console()", async () => {
  // `monitor offsets` answers as console output, not as a result field, so this path is how
  // the section bases actually reach the adapter.
  const { client, fake } = makeClient();
  const promise = client.console("monitor offsets");
  assert.equal(fake.written[0], '1-interpreter-exec console "monitor offsets"\n');
  fake.emitStdout('~"-s .text 0xc0cb68 -s .bss 0xc216c8\\n"\n');
  fake.emitStdout("1^done\n");
  assert.equal(await promise, "-s .text 0xc0cb68 -s .bss 0xc216c8\n");
});

test("escapes quotes and backslashes in console commands", async () => {
  // A Windows path in add-symbol-file would otherwise truncate the command at the first quote.
  const { client, fake } = makeClient();
  const promise = client.console('add-symbol-file "C:\\a\\b.elf"');
  assert.equal(
    fake.written[0],
    '1-interpreter-exec console "add-symbol-file \\"C:\\\\a\\\\b.elf\\""\n'
  );
  fake.emitStdout("1^done\n");
  await promise;
});

test("emits stopped with the reason and frame", async () => {
  const { client, fake } = makeClient();
  const seen = [];
  client.on("stopped", (r) => seen.push(r));
  fake.emitStdout(
    '*stopped,reason="breakpoint-hit",bkptno="1",frame={func="main",line="81"},thread-id="1"\n'
  );
  assert.equal(seen.length, 1);
  assert.equal(seen[0].reason, "breakpoint-hit");
  assert.equal(seen[0].frame.line, "81");
});

test("routes target output separately from console output", () => {
  // KPrintF from the Amiga arrives on the target stream; it must not be muddled with gdb's
  // own chatter in the Debug Console.
  const { client, fake } = makeClient();
  const console_ = [];
  const target = [];
  client.on("console", (t) => console_.push(t));
  client.on("target", (t) => target.push(t));
  fake.emitStdout('~"gdb says this\\n"\n');
  fake.emitStdout('@"the amiga says this\\n"\n');
  assert.deepEqual(console_, ["gdb says this\n"]);
  assert.deepEqual(target, ["the amiga says this\n"]);
});

test("handles a reply split across two chunks", async () => {
  // Nothing guarantees a record arrives in one read.
  const { client, fake } = makeClient();
  const promise = client.send("-x");
  fake.emitStdout('1^done,val');
  fake.emitStdout('ue="42"\n');
  assert.equal((await promise).results.value, "42");
});

test("fails outstanding commands when gdb exits", async () => {
  const { client, fake } = makeClient();
  const promise = client.send("-x");
  fake.emitExit(1);
  await assert.rejects(promise, /exited before answering/);
});

test("refuses to send once gdb has exited", async () => {
  const { client, fake } = makeClient();
  fake.emitExit(0);
  await assert.rejects(client.send("-x"), /not running/);
});
