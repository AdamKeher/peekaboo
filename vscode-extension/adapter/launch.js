"use strict";
// @ts-check

const fs = require("node:fs");
const net = require("node:net");
const os = require("node:os");
const path = require("node:path");

/**
 * Locating gdb, waiting for Peekaboo to be ready, and the attach handshake.
 * Kept out of the session so each piece can be tested without a debug adapter around it.
 */

/**
 * Find Bartman's bundled m68k gdb.
 *
 * Globbed rather than hardcoded so a version bump of his extension does not break us; the
 * extensionDependency guarantees the extension is present but says nothing about its version.
 * @param {string} [extensionsRoot] defaults to the user's VS Code extensions directory
 * @returns {string|null}
 */
function findBundledGdb(extensionsRoot) {
  const root = extensionsRoot || path.join(os.homedir(), ".vscode", "extensions");
  let entries;
  try {
    entries = fs.readdirSync(root);
  } catch {
    return null;
  }
  const candidates = entries
    .filter((name) => /^bartmanabyss\.amiga-debug-/i.test(name))
    // Newest version last alphabetically is not reliable across 1.9 vs 1.10, so compare the
    // version parts numerically and take the highest.
    .sort((a, b) => compareVersions(versionOf(a), versionOf(b)));

  for (let i = candidates.length - 1; i >= 0; i -= 1) {
    const exe = path.join(
      root, candidates[i], "bin", process.platform === "win32" ? "win32" : process.platform,
      "opt", "bin",
      process.platform === "win32" ? "m68k-amiga-elf-gdb.exe" : "m68k-amiga-elf-gdb"
    );
    if (fs.existsSync(exe)) return exe;
  }
  return null;
}

function versionOf(dirName) {
  const m = /-(\d+(?:\.\d+)*)$/.exec(dirName);
  return m ? m[1] : "0";
}

function compareVersions(a, b) {
  const pa = a.split(".").map(Number);
  const pb = b.split(".").map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i += 1) {
    const d = (pa[i] || 0) - (pb[i] || 0);
    if (d !== 0) return d;
  }
  return 0;
}

/**
 * Find the ELF that carries debug info for a HUNK executable.
 * Mirrors DebugSymbols::FindSiblingElf on the C++ side so both agree: prefer the same basename
 * (a.exe -> a.elf), otherwise accept a lone .elf in the directory.
 * @param {string} programPath
 * @returns {string|null}
 */
function findSiblingElf(programPath) {
  const dir = path.dirname(programPath);
  const base = path.basename(programPath, path.extname(programPath));
  const sameName = path.join(dir, `${base}.elf`);
  if (fs.existsSync(sameName)) return sameName;

  let elves;
  try {
    elves = fs.readdirSync(dir).filter((f) => f.toLowerCase().endsWith(".elf"));
  } catch {
    return null;
  }
  // Only when it is unambiguous: guessing between several would put symbols at wrong addresses.
  return elves.length === 1 ? path.join(dir, elves[0]) : null;
}

/**
 * Wait until something accepts connections on the port.
 *
 * This proves Peekaboo is up and nothing more. It is NOT a signal that the program has loaded:
 * the listener opens when Peekaboo starts, so if it was already running this returns instantly
 * while the emulated machine is still booting. attach() waits for the seglist separately.
 * @returns {Promise<boolean>}
 */
function waitForPort(port, host = "127.0.0.1", timeoutMs = 20000, intervalMs = 100) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolve) => {
    const attempt = () => {
      const socket = new net.Socket();
      const done = (ok) => {
        socket.destroy();
        if (ok) return resolve(true);
        if (Date.now() >= deadline) return resolve(false);
        setTimeout(attempt, intervalMs);
      };
      socket.setTimeout(Math.min(1000, intervalMs * 5));
      socket.once("connect", () => done(true));
      socket.once("timeout", () => done(false));
      socket.once("error", () => done(false));
      socket.connect(port, host);
    };
    attempt();
  });
}

/**
 * Turn `monitor offsets` output into the argument string for add-symbol-file.
 * Returns null when the target has no section map (nothing was launched through Peekaboo's
 * VS Code target), which is a normal state and not an error.
 * @param {string} text
 * @returns {string|null}
 */
function parseSectionArgs(text) {
  const trimmed = (text || "").trim();
  if (!trimmed.startsWith("-s ")) return null;
  // Collapse the newline gdb appends; add-symbol-file wants it all on one line.
  return trimmed.replace(/\s+/g, " ");
}

/**
 * Connect gdb to Peekaboo and load symbols at the addresses the program is actually loaded at.
 *
 * Deliberately uses -file-exec-file rather than -file-exec-and-symbols: the latter would load
 * symbols at their link addresses first, leaving two copies to disagree about.
 *
 * @param {import("./miClient").MiClient} mi
 * @param {{elfPath:string, port:number, host?:string, loadTimeout?:number,
 *          onProgress?:(t:string)=>void, setQuiet?:(quiet:boolean)=>void}} options
 * @returns {Promise<{relocated:boolean, sections:string|null, reason:string, detail:string}>}
 */
async function attach(mi, options) {
  const host = options.host || "127.0.0.1";
  // Async, deliberately. In synchronous all-stop mode gdb cannot touch the target while it
  // runs, so a breakpoint set after you hit Continue is held by gdb and never sent -- VS Code
  // shows it in the gutter, Peekaboo's breakpoint list stays empty, and it never fires. With
  // async on, gdb interrupts the target, inserts the breakpoint and resumes by itself. The
  // stub already supports what that needs: vCont, Ctrl-C interrupt, and unsolicited stop
  // replies.
  await mi.send("-gdb-set mi-async on").catch(async () => {
    // Older gdb, or a target that refuses async: fall back rather than failing to attach.
    await mi.send("-gdb-set mi-async off").catch(() => {});
  });
  await mi.send("-gdb-set confirm off");
  // Keep breakpoints in the target instead of pulling them out on every stop.
  //
  // By default gdb removes every breakpoint from the target each time it stops and reinserts
  // them on each resume. It is invisible with a normal debugger, but here the target's
  // breakpoint list IS a window the user is looking at, so each stop made every GDB breakpoint
  // blink out of Peekaboo's Breakpoints window and back in. With this on, a Z0 packet means
  // "the user created a breakpoint" and a z0 means "the user deleted one", which is the only
  // reading under which the two lists stay honestly in sync.
  await mi.send("-gdb-set breakpoint always-inserted on").catch(() => {});
  await mi.send("-gdb-set print pretty on");
  // The architecture has to be set before connecting, or gdb guesses from an empty target.
  await mi.console("set architecture m68k");

  if (options.elfPath) {
    // Forward slashes: gdb treats a backslash inside a command string as an escape, which
    // silently mangles Windows paths into nonsense.
    await mi.console(`file-exec-file ${toGdbPath(options.elfPath)}`).catch(() => {});
  }
  await mi.send(`-target-select remote ${host}:${options.port}`);

  // Wait for the program to be loaded, not merely for Peekaboo to be up.
  //
  // A successful TCP connect proves nothing: Peekaboo opens the RSP listener when it starts, so
  // if it was already running the connect returns instantly -- long before the emulated machine
  // has booted Kickstart and loaded the program. Attaching then leaves gdb looking at ROM with
  // symbols at their link addresses, which is worse than useless because every address is
  // plausible and wrong.
  //
  // The real readiness signal is the seglist existing, which is exactly what `monitor offsets`
  // reports. Poll it until the program is there.
  // A cold start is the slow case and the normal one: Peekaboo has to load the media, boot
  // Kickstart and let AmigaDOS run the program before there is a seglist to find. The old 30 s
  // budget expired mid-boot, which looked exactly like a failure and left symbols at their link
  // addresses -- every one plausible and wrong. Wait long enough for a cold boot instead.
  const timeoutMs = options.loadTimeout || 180000;
  const started = Date.now();
  const sectionsDeadline = started + timeoutMs;
  let sections = null;
  let reason = "";
  let detail = "";
  let announced = false;
  let lastProgress = 0;

  // gdb echoes every `monitor` reply to the console, and polling four times a second turns one
  // unremarkable "still booting" into a hundred identical lines. Silence that stream while
  // polling and report progress ourselves.
  if (options.setQuiet) options.setQuiet(true);
  try {
    for (;;) {
      let reply;
      try {
        reply = await mi.console("monitor offsets");
      } catch {
        reason = "unsupported"; // an older Peekaboo without the monitor command
        break;
      }
      detail = (reply || "").trim();
      sections = parseSectionArgs(reply);
      if (sections) break;
      // "no section map" means Peekaboo was never told which ELF this is, and no amount of
      // waiting changes that -- only relaunching through the VS Code target does. Waiting out
      // the whole budget would just delay a message the user needs immediately.
      if (detail.startsWith("no section map")) {
        reason = "no-map";
        break;
      }

      const now = Date.now();
      if (now >= sectionsDeadline) {
        reason = "timeout";
        break;
      }
      if (!announced) {
        announced = true;
        lastProgress = now;
        // Booting Kickstart and loading takes several seconds; say so rather than appearing hung.
        if (options.onProgress) options.onProgress("Waiting for the program to load...\n");
      } else if (now - lastProgress >= 5000) {
        lastProgress = now;
        // The reason changes as the machine boots -- no seglist, then the wrong process, then
        // ours -- so showing it is what tells a slow boot apart from a hang.
        if (options.onProgress) {
          options.onProgress(
            `  still waiting (${Math.round((now - started) / 1000)}s): ${detail}\n`
          );
        }
      }
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
  } finally {
    if (options.setQuiet) options.setQuiet(false);
  }

  if (sections && options.elfPath) {
    await mi.console(`add-symbol-file ${toGdbPath(options.elfPath)} ${sections}`);
    return { relocated: true, sections, reason: "", detail };
  }
  // Still usable without relocation -- registers, memory and disassembly all work; only
  // source-level symbols are missing. Say so rather than pretending.
  if (options.elfPath) {
    await mi.console(`add-symbol-file ${toGdbPath(options.elfPath)}`).catch(() => {});
  }
  return { relocated: false, sections: null, reason: reason || "no-map", detail };
}

/** gdb parses backslashes as escapes, so paths handed to it must use forward slashes. */
function toGdbPath(p) {
  return String(p).replace(/\\/g, "/");
}

module.exports = {
  attach,
  findBundledGdb,
  findSiblingElf,
  parseSectionArgs,
  toGdbPath,
  waitForPort,
};
