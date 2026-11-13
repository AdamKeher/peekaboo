"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const http = require("node:http");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");
const client = require("../peekabooClient");

test("resolveProgram appends Bartman's .exe suffix", (context) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "peekaboo-vscode-"));
  context.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const executable = path.join(directory, "a.exe");
  fs.writeFileSync(executable, "HUNK");

  assert.equal(client.resolveProgram(path.join(directory, "a")), executable);
});

test("runInPeekaboo posts the absolute executable path", async (context) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "peekaboo-vscode-"));
  context.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const executable = path.join(directory, "demo.exe");
  fs.writeFileSync(executable, "HUNK");

  let received;
  const server = http.createServer((request, response) => {
    const chunks = [];
    request.on("data", (chunk) => chunks.push(chunk));
    request.on("end", () => {
      received = JSON.parse(Buffer.concat(chunks).toString("utf8"));
      response.writeHead(202, { "Content-Type": "application/json" });
      response.end('{"status":"accepted"}');
    });
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  context.after(() => server.close());
  const address = server.address();

  const result = await client.runInPeekaboo({
    program: executable,
    server: `http://127.0.0.1:${address.port}`,
    focus: false
  });

  assert.equal(result, executable);
  assert.deepEqual(received, { program: executable, focus: false });
});

test("runInPeekaboo reports a useful connection error", async () => {
  await assert.rejects(
    client.runInPeekaboo({ program: __filename, server: "http://127.0.0.1:1" }),
    /Cannot reach Peekaboo/
  );
});


/** A stub /api/status that answers `bodies[i]` to the i-th request, repeating the last. */
function statusServer(bodies) {
  let calls = 0;
  const server = http.createServer((req, res) => {
    const body = bodies[Math.min(calls, bodies.length - 1)];
    calls += 1;
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(body));
  });
  return { server, calls: () => calls };
}

test("waitForProgramLoaded waits for the program to reach memory", async (context) => {
  // The whole point of polling here rather than over RSP: attaching gdb halts the machine, and
  // a machine halted mid-boot never finishes loading the program being waited for.
  const { server } = statusServer([
    { status: "ok", programLoaded: false, sectionMap: true },
    { status: "ok", programLoaded: false, sectionMap: true },
    { status: "ok", programLoaded: true, sectionMap: true, segments: 6 }
  ]);
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  context.after(() => server.close());

  const progress = [];
  const result = await client.waitForProgramLoaded({
    server: `http://127.0.0.1:${server.address().port}`,
    intervalMs: 10,
    onProgress: (t) => progress.push(t)
  });

  assert.deepEqual(result, { supported: true, loaded: true, timedOut: false });
  assert.match(progress.join(""), /Waiting for the program to load/);
});

test("waitForProgramLoaded gives up rather than blocking the launch forever", async (context) => {
  const { server } = statusServer([{ status: "ok", programLoaded: false }]);
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  context.after(() => server.close());

  const result = await client.waitForProgramLoaded({
    server: `http://127.0.0.1:${server.address().port}`,
    timeoutMs: 300,
    intervalMs: 10
  });
  assert.deepEqual(result, { supported: true, loaded: false, timedOut: true });
});

test("waitForProgramLoaded detects an older Peekaboo instead of waiting it out", async (context) => {
  // Before this field existed /api/status answered a bare {status, application}. Waiting for a
  // readiness signal that build cannot send would stall every launch for the whole timeout.
  const { server } = statusServer([{ status: "ok", application: "Peekaboo" }]);
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  context.after(() => server.close());

  const started = Date.now();
  const result = await client.waitForProgramLoaded({
    server: `http://127.0.0.1:${server.address().port}`,
    timeoutMs: 5000,
    intervalMs: 10
  });
  assert.deepEqual(result, { supported: false, loaded: false, timedOut: false });
  assert.ok(Date.now() - started < 2000, "must not wait out the budget");
});
