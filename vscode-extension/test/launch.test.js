"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const net = require("node:net");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");
const launch = require("../adapter/launch");

function tempDir(context) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "peekaboo-launch-"));
  context.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

test("toGdbPath uses forward slashes", () => {
  // gdb treats a backslash inside a command string as an escape, so a Windows path handed to
  // add-symbol-file arrives mangled unless it is converted first. This cost a debugging session.
  assert.equal(launch.toGdbPath("C:\\a\\b.elf"), "C:/a/b.elf");
  assert.equal(launch.toGdbPath("/already/posix"), "/already/posix");
});

test("findSiblingElf prefers the matching basename", (context) => {
  const dir = tempDir(context);
  fs.writeFileSync(path.join(dir, "demo.exe"), "HUNK");
  fs.writeFileSync(path.join(dir, "demo.elf"), "ELF");
  fs.writeFileSync(path.join(dir, "other.elf"), "ELF");
  assert.equal(launch.findSiblingElf(path.join(dir, "demo.exe")), path.join(dir, "demo.elf"));
});

test("findSiblingElf accepts a lone elf but refuses to guess between several", (context) => {
  const dir = tempDir(context);
  fs.writeFileSync(path.join(dir, "demo.exe"), "HUNK");
  fs.writeFileSync(path.join(dir, "unrelated.elf"), "ELF");
  assert.equal(launch.findSiblingElf(path.join(dir, "demo.exe")), path.join(dir, "unrelated.elf"));

  fs.writeFileSync(path.join(dir, "second.elf"), "ELF");
  // Two candidates and no name match: guessing would put every symbol at a wrong address.
  assert.equal(launch.findSiblingElf(path.join(dir, "demo.exe")), null);
});

test("findSiblingElf returns null when there is nothing to find", (context) => {
  const dir = tempDir(context);
  fs.writeFileSync(path.join(dir, "demo.exe"), "HUNK");
  assert.equal(launch.findSiblingElf(path.join(dir, "demo.exe")), null);
  assert.equal(launch.findSiblingElf(path.join(dir, "nope", "x.exe")), null);
});

test("findBundledGdb picks the highest version, not the alphabetically last", (context) => {
  const root = tempDir(context);
  const platform = process.platform === "win32" ? "win32" : process.platform;
  const exe = process.platform === "win32" ? "m68k-amiga-elf-gdb.exe" : "m68k-amiga-elf-gdb";
  for (const version of ["1.8.2", "1.10.0", "1.9.1"]) {
    const dir = path.join(root, `BartmanAbyss.amiga-debug-${version}`, "bin", platform, "opt", "bin");
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, exe), "gdb");
  }
  const found = launch.findBundledGdb(root);
  // "1.9.1" sorts after "1.10.0" as a string; numeric comparison must win.
  assert.match(found, /amiga-debug-1\.10\.0/);
});

test("findBundledGdb returns null when the extension is absent", (context) => {
  assert.equal(launch.findBundledGdb(tempDir(context)), null);
  assert.equal(launch.findBundledGdb(path.join(os.tmpdir(), "definitely-not-here-12345")), null);
});

test("parseSectionArgs collapses monitor output onto one line", () => {
  const text = "-s .text 0xc0cb68 -s .rodata 0xc18e40 -s .MEMF_CHIP 0x15060\n";
  assert.equal(
    launch.parseSectionArgs(text),
    "-s .text 0xc0cb68 -s .rodata 0xc18e40 -s .MEMF_CHIP 0x15060"
  );
});

test("parseSectionArgs rejects anything that is not a section list", () => {
  // "no section map: launch through Peekaboo's VS Code target..." is a normal answer, not a
  // section list, and must not be pasted into add-symbol-file.
  assert.equal(launch.parseSectionArgs("no section map: launch through ...\n"), null);
  assert.equal(launch.parseSectionArgs(""), null);
  assert.equal(launch.parseSectionArgs(null), null);
});

test("waitForPort resolves once something listens", async () => {
  const server = net.createServer(() => {});
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const port = server.address().port;
  assert.equal(await launch.waitForPort(port, "127.0.0.1", 3000, 20), true);
  server.close();
});

test("waitForPort gives up rather than hanging", async () => {
  // Port 1 is reserved and nothing will be listening on it.
  assert.equal(await launch.waitForPort(1, "127.0.0.1", 250, 40), false);
});

test("attach relocates when monitor offsets answers", async () => {
  const sent = [];
  const consoled = [];
  const mi = {
    send: async (cmd) => {
      sent.push(cmd);
      return { class: "done", results: {}, streams: [] };
    },
    console: async (cmd) => {
      consoled.push(cmd);
      if (cmd === "monitor offsets") return "-s .text 0xc0cb68 -s .bss 0xc216c8\n";
      return "";
    },
  };
  const result = await launch.attach(mi, { elfPath: "C:\\a\\demo.elf", port: 2346 });
  assert.equal(result.relocated, true);
  assert.ok(sent.includes("-target-select remote 127.0.0.1:2346"));
  // Without this gdb pulls every breakpoint out of the target on each stop and pushes them back
  // on each resume -- which, when the target's breakpoint list is a window the user is watching,
  // makes their breakpoints blink in and out of Peekaboo all session.
  assert.ok(sent.includes("-gdb-set breakpoint always-inserted on"));
  // Symbols must be added at the reported bases, with a gdb-safe path.
  assert.ok(
    consoled.some((c) => c === "add-symbol-file C:/a/demo.elf -s .text 0xc0cb68 -s .bss 0xc216c8")
  );
});

test("attach still connects when there is no section map", async () => {
  // Registers, memory and disassembly all work unrelocated; only source symbols are missing.
  const consoled = [];
  const mi = {
    send: async () => ({ class: "done", results: {}, streams: [] }),
    console: async (cmd) => {
      consoled.push(cmd);
      if (cmd === "monitor offsets") return "no section map: launch through ...\n";
      return "";
    },
  };
  // Short budget: this is the "never loads" path, and the real default now waits out a cold
  // Kickstart boot.
  const result = await launch.attach(mi, { elfPath: "/a/demo.elf", port: 2346, loadTimeout: 200 });
  assert.equal(result.relocated, false);
  assert.equal(result.sections, null);
  // "no section map" is Peekaboo saying it does not know the ELF, which is a different fault
  // from the program not having loaded yet, and gets a different message.
  assert.equal(result.reason, "no-map");
  assert.ok(consoled.some((c) => c === "add-symbol-file /a/demo.elf"));
});

test("attach survives an older Peekaboo with no monitor command", async () => {
  const mi = {
    send: async () => ({ class: "done", results: {}, streams: [] }),
    console: async (cmd) => {
      if (cmd === "monitor offsets") throw new Error("unsupported");
      return "";
    },
  };
  const result = await launch.attach(mi, { elfPath: "/a/demo.elf", port: 2346 });
  assert.equal(result.relocated, false);
  assert.equal(result.reason, "unsupported");
});

test("attach waits for the program to load rather than trusting the connect", async () => {
  // A TCP connect only proves Peekaboo is up. If it was already running the connect returns
  // instantly, while the emulated machine is still booting Kickstart -- attaching then leaves
  // gdb looking at ROM with symbols at their link addresses, every one plausible and wrong.
  let attempts = 0;
  const progress = [];
  const mi = {
    send: async () => ({ class: "done", results: {}, streams: [] }),
    console: async (cmd) => {
      if (cmd !== "monitor offsets") return "";
      attempts += 1;
      // The first few polls happen while the machine is still booting.
      // What a booting machine actually answers: the map is known, the program has not been
      // loaded by AmigaDOS yet. (A literal "no section map" is a different state -- Peekaboo
      // does not know the ELF at all -- and attach stops waiting on it, since it never clears.)
      return attempts < 4
        ? "not relocated: no AmigaDOS process seglist found (is the program running?)\n"
        : "-s .text 0xc0cb68 -s .bss 0xc216c8\n";
    },
  };

  const result = await launch.attach(mi, {
    elfPath: "/a/demo.elf",
    port: 2346,
    loadTimeout: 10000,
    onProgress: (t) => progress.push(t),
  });

  assert.equal(result.relocated, true);
  assert.ok(attempts >= 4, `polled ${attempts} times`);
  assert.match(progress.join(""), /Waiting for the program to load/);
});

test("attach gives up waiting rather than hanging forever", async () => {
  const mi = {
    send: async () => ({ class: "done", results: {}, streams: [] }),
    // Still booting for longer than the budget allows -- the timeout path, not the no-map one.
    console: async () => "not relocated: no AmigaDOS process seglist found\n",
  };
  const started = Date.now();
  const result = await launch.attach(mi, { elfPath: "/a/demo.elf", port: 2346, loadTimeout: 600 });
  assert.equal(result.relocated, false);
  // Reported as a timeout, not as "no section map": the difference is what the user is told to
  // do about it, and blaming their launch configuration for a slow boot sends them the wrong way.
  assert.equal(result.reason, "timeout");
  assert.ok(Date.now() - started < 5000, "must respect loadTimeout");
});

test("polling the seglist does not spam the console, and reports progress", async () => {
  // gdb echoes every `monitor` reply. At four polls a second a slow boot produced a hundred
  // identical "no seglist" lines in the Debug Console, which is how a working wait came to look
  // like a failure.
  const quiet = [];
  const progress = [];
  let attempts = 0;
  const mi = {
    send: async () => ({ class: "done", results: {}, streams: [] }),
    console: async (cmd) => {
      if (cmd !== "monitor offsets") return "";
      attempts += 1;
      return attempts < 30
        ? "not relocated: no AmigaDOS process seglist found\n"
        : "-s .text 0xc0cb68\n";
    },
  };
  const result = await launch.attach(mi, {
    elfPath: "/a/demo.elf",
    port: 2346,
    loadTimeout: 20000,
    onProgress: (t) => progress.push(t),
    setQuiet: (q) => quiet.push(q),
  });
  assert.equal(result.relocated, true);
  // Raised for the poll loop and lowered again, so add-symbol-file output is still shown.
  assert.deepEqual(quiet, [true, false]);
  const text = progress.join("");
  assert.match(text, /Waiting for the program to load/);
  // The elapsed-time line carries the current reason, so a slow boot is distinguishable from
  // a hang without turning the echo back on.
  assert.match(text, /still waiting \(\d+s\): not relocated: no AmigaDOS process seglist found/);
});
