"use strict";
// @ts-check

const { MiClient } = require("./miClient");
const launch = require("./launch");
const client = require("../peekabooClient");

/**
 * The Debug Adapter Protocol side of the bridge.
 *
 * Everything about the m68k program -- breakpoints, stack, variables, stepping -- goes through
 * gdb via MI. Everything about the machine -- copper, blitter, sprites, the frame scrubber --
 * stays in Peekaboo's own windows, which are richer than anything a tree view here could be.
 * The one deliberate overlap is disassembly: it is genuinely source-anchored, and on -Ofast code
 * "what did the compiler actually emit for this line" is often the whole answer.
 *
 * Variables use gdb's varobj interface (-var-create / -var-list-children) rather than a flat
 * -stack-list-variables dump, because that is what gives VS Code expandable structs and arrays.
 */

const THREAD_ID = 1;

/** Reference kinds packed into a DAP variablesReference. */
const REF_LOCALS = "locals";
const REF_REGISTERS = "registers";
const REF_VAROBJ = "varobj";
const REF_GLOBALS = "globals";
const REF_GLOBALS_FILE = "globals-file";
const REF_SR = "sr";

/** Linker-defined symbols that are addresses, not variables anyone wants to inspect. */
const LINKER_SYMBOLS = /^(_edata|_end|_etext|__bss_start|__bss_end|_sdata|_stext)$/;
/** How many untyped .symtab symbols to list before giving up and saying how many were dropped. */
const MAX_BARE_GLOBALS = 200;
/** Group heading for globals gdb knows only from .symtab. */
const BARE_GROUP = "(no debug info)";
/** What gdb prints for an aggregate instead of a value: `{...}` for a struct, `[12]` for an
 *  array. It says nothing about whether the thing has any storage behind it. */
const AGGREGATE_PLACEHOLDER = /^(\{\.\.\.\}|\[\d+\])$/;

class PeekabooSession {
  /**
   * @param {(message:any)=>void} send  writes a DAP message back to VS Code
   * @param {{log?:(t:string)=>void}} [hooks]
   */
  constructor(send, hooks = {}) {
    this._send = send;
    this._seq = 1;
    this.mi = /** @type {MiClient|null} */ (null);
    this.log = hooks.log || (() => {});

    /** @type {Map<number, any>} */
    this.refs = new Map();
    this.nextRef = 1000;
    // Varobj names owned by the current stop, deleted wholesale when handles are invalidated.
    // A set rather than a map keyed by expression: two frames can both have a local called
    // `buf`, and keying by expression silently dropped one of the two varobjs on the floor.
    /** @type {Set<string>} */
    this.varObjects = new Set();
    this.varCounter = 0;
    // The program-wide variable sweep. Symbols do not change while a program is loaded, so this
    // survives across stops; addresses are re-resolved each time because they do not.
    /** @type {{groups:any[], truncated:number}|null} */
    this.globalsCache = null;

    this.launchArgs = /** @type {any} */ ({});
    // "launch" or "attach": attach leaves the target where it is, launch runs it.
    this.launchRequest = "launch";
    this.running = false;
    this.terminated = false;
    // Suppresses gdb's console echo; see wireMi.
    this.consoleQuiet = false;
    // Raised for the whole attach handshake. gdb reports a stop when it connects to a halted
    // target, and forwarding that to VS Code paints the session as paused before it has even
    // been configured -- a pause the user never asked for, at an address that means nothing.
    this.attaching = false;
    // Whether Peekaboo confirmed over HTTP that the program is loaded, and whether this build
    // of Peekaboo can answer that at all.
    this.programReady = false;
    this.readinessSupported = false;
    // Set while a step/continue we issued is in flight, so the stopped event that answers it
    // is reported to VS Code rather than swallowed as an unsolicited stop.
    this.expectStop = false;
    // Latched when Peekaboo cannot audit line placement, so we stop asking.
    this.lineAuditUnavailable = false;
    // Set while we interrupt the target purely to modify it. The resulting stop is ours, not
    // the user's, and must not surface in VS Code as a spurious pause.
    this.suppressStops = 0;
    /** @type {Array<(v:any)=>void>} */
    this.stopWaiters = [];
  }

  // ---------------------------------------------------------------- DAP plumbing

  send(message) {
    message.seq = this._seq++;
    this._send(message);
  }

  respond(request, body) {
    this.send({
      type: "response",
      request_seq: request.seq,
      success: true,
      command: request.command,
      body: body || {},
    });
  }

  fail(request, message) {
    this.send({
      type: "response",
      request_seq: request.seq,
      success: false,
      command: request.command,
      message,
    });
  }

  event(event, body) {
    this.send({ type: "event", event, body: body || {} });
  }

  output(text, category = "console") {
    if (!text) return;
    this.event("output", { output: text, category });
  }

  allocRef(payload) {
    const ref = this.nextRef++;
    this.refs.set(ref, payload);
    return ref;
  }

  async handleMessage(message) {
    if (message.type !== "request") return;
    try {
      await this.dispatch(message);
    } catch (err) {
      this.fail(message, err instanceof Error ? err.message : String(err));
    }
  }

  async dispatch(request) {
    switch (request.command) {
      case "initialize": return this.onInitialize(request);
      case "launch": return this.onLaunch(request);
      case "attach": return this.onLaunch(request);
      case "configurationDone": return this.onConfigurationDone(request);
      case "setBreakpoints": return this.onSetBreakpoints(request);
      case "setFunctionBreakpoints": return this.onSetFunctionBreakpoints(request);
      case "setExceptionBreakpoints": return this.respond(request);
      case "setDataBreakpoints": return this.onSetDataBreakpoints(request);
      case "threads": return this.onThreads(request);
      case "stackTrace": return this.onStackTrace(request);
      case "scopes": return this.onScopes(request);
      case "variables": return this.onVariables(request);
      case "setVariable": return this.onSetVariable(request);
      case "setExpression": return this.onSetExpression(request);
      case "evaluate": return this.onEvaluate(request);
      case "continue": return this.onContinue(request);
      case "next": return this.onStep(request, "-exec-next");
      case "stepIn": return this.onStep(request, "-exec-step");
      case "stepOut": return this.onStep(request, "-exec-finish");
      case "stepBack": return this.onStep(request, "-exec-step --reverse");
      case "reverseContinue": return this.onReverseContinue(request);
      case "pause": return this.onPause(request);
      case "disassemble": return this.onDisassemble(request);
      case "readMemory": return this.onReadMemory(request);
      case "writeMemory": return this.onWriteMemory(request);
      case "disconnect": return this.onDisconnect(request);
      case "terminate": return this.onDisconnect(request);
      default: return this.respond(request);
    }
  }

  // ---------------------------------------------------------------- lifecycle

  onInitialize(request) {
    this.respond(request, {
      supportsConfigurationDoneRequest: true,
      supportsEvaluateForHovers: true,
      supportsSetVariable: true,
      // Without this the Watch pane is read-only: VS Code only offers to edit a watched
      // expression when the adapter says it can assign to one.
      supportsSetExpression: true,
      supportsConditionalBreakpoints: true,
      supportsHitConditionalBreakpoints: true,
      supportsFunctionBreakpoints: true,
      supportsDataBreakpoints: true,
      supportsDisassembleRequest: true,
      supportsReadMemoryRequest: true,
      supportsWriteMemoryRequest: true,
      supportsSteppingGranularity: true,
      supportsTerminateRequest: true,
      supportsLogPoints: true,
      // Reverse execution. Peekaboo only advertises it over RSP when frame history is actually
      // recording, and gdb only exposes it when the target advertised it -- so the buttons are
      // live exactly when they will work.
      supportsStepBack: true,
      supportsRestartRequest: false,
    });
    // Deliberately NOT sending "initialized" here. It tells VS Code to replay its saved
    // breakpoints, and until launch has run there is no gdb to send them to -- every one fails
    // and the user sees a breakpoint in the gutter that was never really set. onLaunch sends it
    // once the session can actually accept them.
  }

  async onLaunch(request) {
    const args = request.arguments || {};
    this.launchArgs = args;
    // A new program means new symbols; anything cached from the last one is a lie.
    this.globalsCache = null;
    this.launchRequest = request.command;

    const gdbPath = args.gdbPath || launch.findBundledGdb();
    if (!gdbPath) {
      return this.fail(
        request,
        "Could not find m68k-amiga-elf-gdb. Install Bartman's amiga-debug extension, or set " +
          '"gdbPath" in the launch configuration.'
      );
    }

    const program = args.program;
    const elfPath = args.elfPath || (program ? launch.findSiblingElf(program) : null);
    const port = args.gdbPort || 2346;

    // Wait for the program to be in memory BEFORE gdb attaches.
    //
    // gdb's first act on connecting is to halt the target, and a machine halted partway through
    // booting Kickstart never gets round to loading the program -- so waiting for the program
    // after attaching waits forever, on a machine we froze ourselves. Peekaboo reports readiness
    // over the same HTTP channel the launch request went out on, which leaves the emulator
    // running while we watch.
    if (!args.attach) {
      const ready = await client.waitForProgramLoaded({
        server: args.server,
        timeoutMs: args.loadTimeout || 180000,
        onProgress: (text) => this.output(text),
      });
      if (ready.timedOut) {
        this.output(
          "The program has not loaded yet; attaching anyway. If symbols look wrong, let it " +
            'reach the demo and press F5 again, or raise "loadTimeout".\n',
          "stderr"
        );
      }
      this.programReady = ready.loaded;
      this.readinessSupported = ready.supported;
    }

    this.output(`Connecting to Peekaboo on 127.0.0.1:${port}...\n`);
    if (!(await launch.waitForPort(port, "127.0.0.1", args.timeout || 20000))) {
      return this.fail(
        request,
        `Nothing is listening on 127.0.0.1:${port}. Start Peekaboo and enable ` +
          "Preferences -> GDB Server, then try again."
      );
    }

    this.mi = new MiClient(gdbPath);
    this.wireMi(this.mi);
    this.mi.start();

    this.attaching = true;
    const result = await launch.attach(this.mi, {
      elfPath,
      port,
      // Peekaboo already told us the program is in memory, so `monitor offsets` answers on the
      // first poll; the RSP wait only has to cover the gap between the two channels. Without
      // that signal it is the only mechanism there is, so give it the full budget.
      loadTimeout: this.readinessSupported ? 15000 : args.loadTimeout || 180000,
      onProgress: (text) => this.output(text),
      setQuiet: (quiet) => {
        this.consoleQuiet = quiet;
      },
    });
    this.attaching = false;
    if (!elfPath) {
      this.output(
        "No ELF with debug info was found next to the program; source-level debugging is " +
          "unavailable (registers, memory and disassembly still work).\n",
        "stderr"
      );
    } else if (!result.relocated) {
      // Distinguish the two failures. "No section map" means Peekaboo was never told which ELF
      // this is; a timeout means it was, but the machine had not finished booting it yet -- and
      // telling someone to launch through the VS Code target when that is exactly what they did
      // sends them looking in the wrong place.
      const shared =
        "Symbols are at their link addresses and will NOT match the running program, so " +
        "breakpoints will bind to the wrong code.\n";
      if (result.reason === "timeout") {
        this.output(
          `The program did not load within ${Math.round((args.loadTimeout || 180000) / 1000)}s. ` +
            `Peekaboo last reported: ${result.detail || "nothing"}. ` +
            'Raise "loadTimeout" in the launch configuration, or press F5 again once the ' +
            `program is on screen. ${shared}`,
          "stderr"
        );
      } else if (result.reason === "unsupported") {
        this.output(
          "This Peekaboo build has no `monitor offsets` command, so symbols cannot be " +
            `relocated. ${shared}`,
          "stderr"
        );
      } else {
        this.output(
          "Peekaboo reported no section map, so it does not know which ELF this is. Launch " +
            `through Peekaboo's VS Code target to fix this. ${shared}`,
          "stderr"
        );
      }
    } else {
      this.output(`Symbols relocated: ${result.sections}\n`);
    }

    this.respond(request);
    // Now that gdb is attached and symbols are relocated, invite VS Code to send its
    // breakpoints. Doing this earlier means they arrive before there is anything to send them
    // to; doing it after relocation means they resolve against real addresses.
    //
    // The target really is halted here: attaching sends `?`, which interrupts the machine so
    // RSP's "stopped on attach" contract is true rather than merely claimed. Whether it stays
    // halted is decided in configurationDone, once VS Code has sent the breakpoints.
    this.running = false;
    this.event("initialized");
  }

  /**
   * VS Code has finished replaying its breakpoints; decide whether to run.
   *
   * Attaching halts the machine, which is right for gdb but wrong for F5: without this the
   * session sits stopped wherever Kickstart happened to be -- typically in ROM, several seconds
   * before the program even loads -- and the user has to press Continue once before anything
   * they set can be hit. Resuming here is also what pushes the breakpoints to Peekaboo at all,
   * since gdb only sends Z0 packets on resume.
   *
   * stopOnEntry and attach both mean "I want to look at this before it moves", so those stay
   * stopped and report it.
   */
  async onConfigurationDone(request) {
    this.respond(request);
    const stayStopped =
      Boolean(this.launchArgs.stopOnEntry) || Boolean(this.launchArgs.attach) ||
      this.launchRequest === "attach";
    if (stayStopped) {
      this.running = false;
      this.event("stopped", {
        reason: "entry",
        threadId: THREAD_ID,
        allThreadsStopped: true,
      });
      return;
    }
    if (!this.mi || this.terminated) return;
    this.running = true;
    this.mi.send("-exec-continue").catch((err) => this.output(`${err.message}
`, "stderr"));
  }

  async onDisconnect(request) {
    if (this.mi) {
      // Detach rather than kill: the Amiga program keeps running in Peekaboo, which is what a
      // user expects when they stop debugging but leave the emulator open.
      try {
        await this.mi.send("-target-detach");
      } catch {
        /* already gone */
      }
      this.mi.stop();
    }
    this.respond(request);
    this.terminate();
  }

  terminate() {
    if (this.terminated) return;
    this.terminated = true;
    this.event("terminated");
  }

  // ---------------------------------------------------------------- stop handling

  /**
   * Route an MiClient's events into DAP events. Extracted from onLaunch so the mapping can be
   * exercised without spawning gdb -- the stop-reason translation is the part most worth testing.
   * @param {import("./miClient").MiClient} mi
   */
  wireMi(mi) {
    // consoleQuiet is raised while attach() polls `monitor offsets`: gdb echoes every reply, and
    // four polls a second turn one "still booting" into a hundred identical lines.
    mi.on("console", (t) => {
      if (!this.consoleQuiet) this.output(t);
    });
    // KPrintF and serial output from the Amiga arrive on the target stream. Routing it to
    // stdout keeps it distinct from gdb's own chatter in the Debug Console.
    mi.on("target", (t) => this.output(t, "stdout"));
    mi.on("log", (t) => this.log(t));
    mi.on("stopped", (results) => this.onMiStopped(results));
    mi.on("exit", () => this.terminate());
  }

  onMiStopped(results) {
    this.running = false;
    this.invalidateHandles();

    // Wake anything waiting for the target to come to rest.
    const waiters = this.stopWaiters;
    this.stopWaiters = [];
    for (const resolve of waiters) resolve(results);

    if (this.attaching) {
      // The stop gdb reports on connecting. Waiters have been woken; VS Code has not been told
      // the session exists yet, so there is nothing to report a pause to.
      return;
    }

    if (this.suppressStops > 0) {
      // A stop we caused ourselves in order to insert or remove a breakpoint. Reporting it
      // would flash the editor into a paused state the user never asked for.
      this.suppressStops -= 1;
      return;
    }

    const reasonMap = {
      "breakpoint-hit": "breakpoint",
      "watchpoint-trigger": "data breakpoint",
      "read-watchpoint-trigger": "data breakpoint",
      "access-watchpoint-trigger": "data breakpoint",
      "end-stepping-range": "step",
      "function-finished": "step",
      "signal-received": "exception",
      "exited-normally": "exited",
    };
    let reason = reasonMap[results.reason] || "pause";

    // Not every SIGTRAP is an exception, and this stub sends nothing else.
    //
    // Peekaboo answers every stop with T05 (SIGTRAP) -- it has no way to describe an Amiga trap
    // any more precisely. When gdb can match the PC to one of its own breakpoints it reports
    // breakpoint-hit; when it cannot -- a breakpoint set in Peekaboo rather than VS Code, a
    // pause taken there, a stop gdb did not ask for -- it reports a plain signal, and VS Code
    // paints its big red "Exception has occurred: Trace/breakpoint trap" box over a perfectly
    // ordinary stop. SIGINT is the same story for an interrupt. Neither is an exception; call
    // them what they are and leave the red box for signals that really are faults.
    const signal = results["signal-name"] || "";
    if (results.reason === "signal-received" && (signal === "SIGTRAP" || signal === "SIGINT")) {
      reason = "pause";
    }

    if (results.reason === "exited-normally" || results.reason === "exited") {
      this.event("exited", { exitCode: Number(results.exitcode || 0) });
      this.terminate();
      return;
    }

    const body = { reason, threadId: THREAD_ID, allThreadsStopped: true };
    if (results.bkptno) body.hitBreakpointIds = [Number(results.bkptno)];
    if (results.reason === "signal-received" && signal) {
      if (reason === "pause") {
        // Say where it came from rather than repeating "SIGTRAP", which tells the user nothing
        // about a stop that Peekaboo, not gdb, decided to make.
        body.description =
          signal === "SIGINT"
            ? "Interrupted"
            : "Stopped by Peekaboo (a breakpoint or pause set outside VS Code)";
      } else {
        body.description = signal;
        body.text = results["signal-meaning"] || signal;
      }
    }
    this.event("stopped", body);
  }

  invalidateHandles() {
    // Frame and variable handles only mean anything for one stop.
    this.refs.clear();
    this.nextRef = 1000;
    // Delete gdb-side varobjs too, or they accumulate for the life of the session. Deleting a
    // root takes its children with it, which is why only roots are tracked here.
    for (const name of this.varObjects) {
      if (this.mi) this.mi.send(`-var-delete ${name}`).catch(() => {});
    }
    this.varObjects.clear();
  }

  requireMi() {
    if (!this.mi || this.terminated) throw new Error("no debug session is running");
    return this.mi;
  }

  // ---------------------------------------------------------------- breakpoints

  /** Resolve when the target next stops, or after `timeoutMs`. */
  waitForStop(timeoutMs = 4000) {
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.stopWaiters = this.stopWaiters.filter((w) => w !== wake);
        resolve(null);
      }, timeoutMs);
      const wake = (results) => {
        clearTimeout(timer);
        resolve(results);
      };
      this.stopWaiters.push(wake);
    });
  }

  /**
   * Run `fn` with the target halted, resuming afterwards if it was running.
   *
   * gdb refuses to modify a running target -- "Cannot execute this command while the target is
   * running" -- even with mi-async on, because this stub is all-stop. Without this, a breakpoint
   * set after you press Continue is held by gdb and never reaches Peekaboo: the gutter shows it,
   * the target never stops on it, and nothing anywhere says why.
   */
  async withTargetStopped(fn) {
    const mi = this.requireMi();
    if (!this.running) return fn();

    this.suppressStops += 1;
    try {
      await mi.send("-exec-interrupt");
      await this.waitForStop();
    } catch {
      // Could not interrupt; undo the suppression and try the work anyway rather than
      // silently skipping it.
      this.suppressStops = Math.max(0, this.suppressStops - 1);
    }
    try {
      return await fn();
    } finally {
      this.running = true;
      mi.send("-exec-continue").catch((err) => this.output(`${err.message}\n`, "stderr"));
    }
  }

  async onSetBreakpoints(request) {
    return this.withTargetStopped(() => this.applySetBreakpoints(request));
  }

  async applySetBreakpoints(request) {
    const mi = this.requireMi();
    const source = request.arguments.source || {};
    const file = source.path || source.name;
    const wanted = request.arguments.breakpoints || [];

    // Clear the file's existing breakpoints: DAP sends the complete desired set every time.
    await this.clearBreakpointsForFile(file);

    const results = [];
    for (const bp of wanted) {
      results.push(await this.insertBreakpoint(mi, file, bp));
    }
    this.respond(request, { breakpoints: results });
  }

  async clearBreakpointsForFile(file) {
    const mi = this.requireMi();
    let list;
    try {
      list = await mi.send("-break-list");
    } catch {
      return;
    }
    const table = list.results.BreakpointTable;
    const rows = (table && table.body) || [];
    const target = normalisePath(file);
    for (const row of rows) {
      if (!row || !row.fullname) continue;
      if (normalisePath(row.fullname) === target) {
        await mi.send(`-break-delete ${row.number}`).catch(() => {});
      }
    }
  }

  async insertBreakpoint(mi, file, bp) {
    // -f inserts a pending breakpoint rather than failing when the location is not yet known.
    let cmd = "-break-insert -f";
    if (bp.condition) cmd += ` -c ${quote(bp.condition)}`;
    if (bp.hitCondition) {
      const n = Number(bp.hitCondition);
      if (Number.isFinite(n) && n > 0) cmd += ` -i ${n}`;
    }
    cmd += ` ${quote(`${launch.toGdbPath(file)}:${bp.line}`)}`;

    try {
      const reply = await mi.send(cmd);
      const info = reply.results.bkpt;
      if (!info) return { verified: false, line: bp.line, message: "gdb did not report a location" };

      // A pending breakpoint has no address and will never fire. gdb reports it as a success,
      // and reporting it back as verified paints a solid red dot in the editor for a breakpoint
      // that does nothing -- precisely the lie this project exists to remove. Under
      // -Ofast -flto -fwhole-program this is common: the line was folded into a caller, or
      // generated no code of its own at all.
      if (info.addr === "<PENDING>" || info.pending || !info.line) {
        const audit = await this.auditLine(file, bp.line);
        const nearby =
          audit && audit.nearbyLines.length
            ? ` Lines with code nearby: ${audit.nearbyLines.slice(0, 8).join(", ")}.`
            : "";
        return {
          verified: false,
          line: bp.line,
          message:
            `Line ${bp.line} generated no code, so this breakpoint can never fire. Heavy ` +
            "optimisation (-Ofast -flto -fwhole-program) folds many lines into their callers." +
            nearby,
        };
      }

      const boundLine = Number(info.line);
      const out = {
        id: Number(info.number),
        verified: true,
        line: boundLine,
        source: { path: info.fullname || file },
      };
      // Say so when the breakpoint did not land where it was asked for. Under -Ofast the
      // requested line often generated no code of its own, and silently moving the marker is
      // exactly the dishonesty this project set out to fix.
      if (boundLine !== bp.line) {
        out.message =
          `Line ${bp.line} generated no code; gdb placed this at line ${boundLine}. ` +
          "Heavy optimisation (-Ofast -flto -fwhole-program) often folds a line into its caller.";
      }
      if (info.addr === "<MULTIPLE>") {
        out.message =
          "This line was inlined into several places; the breakpoint covers all of them.";
      }

      // Audit gdb's placement against Peekaboo's own line table. gdb places the breakpoint --
      // fighting it would make next/finish/bt disagree with the marker -- but it reports a
      // single address even when the line produced several disjoint stretches of code, so a
      // breakpoint can silently cover only part of the line it is drawn on.
      const audit = await this.auditLine(file, bp.line);
      if (audit && audit.siteCount > 1 && info.addr !== "<MULTIPLE>") {
        // gdb reports the address it chose; find which stretch that falls inside rather than
        // only which one it starts, since gdb may pick a location past a site's first byte.
        const chosen = Number(info.addr);
        const covered =
          audit.sites.findIndex(
            (site) =>
              site.runtimeLo !== undefined &&
              chosen >= Number(site.runtimeLo) &&
              chosen < Number(site.runtimeLo) + (Number(site.hi) - Number(site.lo))
          ) + 1;
        out.message =
          `${audit.siteCount} separate stretches of code came from line ${bp.line} ` +
          "(inlined or unrolled by the optimiser); this breakpoint covers " +
          (covered > 0 ? `only stretch ${covered} of ${audit.siteCount}` : "only one of them") +
          ". Execution can reach this line without stopping here.";
      }
      return out;
    } catch (err) {
      return {
        verified: false,
        line: bp.line,
        message: err instanceof Error ? err.message : String(err),
      };
    }
  }

  /**
   * Ask Peekaboo where a source line's code actually is.
   *
   * Deliberately advisory: gdb owns placement, this only explains it. Returns null when the
   * command is unavailable (an older Peekaboo, or no line table), so callers degrade to gdb's
   * own answer rather than failing.
   */
  async auditLine(file, line) {
    if (!this.mi || this.lineAuditUnavailable) return null;
    try {
      const text = await this.mi.console(`monitor resolve-line ${launch.toGdbPath(file)}:${line}`);
      if (/^no debug line table/.test(text) || /^unknown monitor command/.test(text)) {
        this.lineAuditUnavailable = true;
        return null;
      }
      const sites = [];
      const SITE = /\$([0-9A-Fa-f]+)-\$([0-9A-Fa-f]+)(?:\s+runtime \$([0-9A-Fa-f]+))?/;
      for (const raw of text.split("\n")) {
        const m = SITE.exec(raw);
        if (!m) continue;
        sites.push({
          lo: `0x${m[1].toLowerCase()}`,
          hi: `0x${m[2].toLowerCase()}`,
          // gdb reports addresses unpadded, so strip leading zeros to compare them.
          runtimeLo: m[3] ? `0x${m[3].toLowerCase().replace(/^0+(?=.)/, "")}` : undefined,
          isStmt: /\(stmt\)/.test(raw),
        });
      }
      const nearby = /lines with code nearby:([\d ]+)/.exec(text);
      return {
        text,
        sites,
        siteCount: sites.length,
        noCode: /generated no code/.test(text),
        nearbyLines: nearby ? nearby[1].trim().split(/\s+/).map(Number) : [],
      };
    } catch {
      this.lineAuditUnavailable = true;
      return null;
    }
  }

  async onSetFunctionBreakpoints(request) {
    return this.withTargetStopped(() => this.applySetFunctionBreakpoints(request));
  }

  async applySetFunctionBreakpoints(request) {
    const mi = this.requireMi();
    const wanted = request.arguments.breakpoints || [];
    const results = [];
    for (const bp of wanted) {
      try {
        const reply = await mi.send(`-break-insert -f ${quote(bp.name)}`);
        const info = reply.results.bkpt;
        results.push({
          id: Number(info.number),
          verified: true,
          line: info.line ? Number(info.line) : undefined,
        });
      } catch (err) {
        results.push({ verified: false, message: err instanceof Error ? err.message : String(err) });
      }
    }
    this.respond(request, { breakpoints: results });
  }

  async onSetDataBreakpoints(request) {
    return this.withTargetStopped(() => this.applySetDataBreakpoints(request));
  }

  async applySetDataBreakpoints(request) {
    const mi = this.requireMi();
    const wanted = request.arguments.breakpoints || [];
    const results = [];
    for (const bp of wanted) {
      try {
        // dataId carries the expression to watch.
        const access = bp.accessType === "read" ? "-r" : bp.accessType === "readWrite" ? "-a" : "";
        await mi.send(`-break-watch ${access} ${bp.dataId}`.replace(/\s+/g, " ").trim());
        results.push({ verified: true });
      } catch (err) {
        results.push({ verified: false, message: err instanceof Error ? err.message : String(err) });
      }
    }
    this.respond(request, { breakpoints: results });
  }

  // ---------------------------------------------------------------- stack and variables

  onThreads(request) {
    // The 68000 has one context. Inventing more would confuse both gdb and VS Code.
    this.respond(request, { threads: [{ id: THREAD_ID, name: "68000" }] });
  }

  async onStackTrace(request) {
    const mi = this.requireMi();
    const start = request.arguments.startFrame || 0;
    const levels = request.arguments.levels || 20;
    const reply = await mi.send(`-stack-list-frames ${start} ${start + levels - 1}`);
    // MI names this "stack", not "frames" -- an invented fixture said otherwise and the tests
    // happily agreed with it while the real thing returned nothing.
    const frames = reply.results.stack || [];

    const out = frames.map((f) => {
      const level = Number(f.level);
      const frameId = this.allocRef({ kind: "frame", level });
      const frame = {
        id: frameId,
        name: f.func || `0x${(f.addr || "0").replace(/^0x/, "")}`,
        line: f.line ? Number(f.line) : 0,
        column: 0,
        instructionPointerReference: f.addr,
      };
      if (f.fullname || f.file) {
        frame.source = { name: baseName(f.file || f.fullname), path: f.fullname || f.file };
      }
      return frame;
    });
    this.respond(request, { stackFrames: out, totalFrames: out.length + start });
  }

  onScopes(request) {
    const frame = this.refs.get(request.arguments.frameId);
    const level = frame ? frame.level : 0;
    this.respond(request, {
      scopes: [
        {
          name: "Locals",
          variablesReference: this.allocRef({ kind: REF_LOCALS, level }),
          expensive: false,
        },
        // Globals belong to no frame at all -- they exist before main() and after it returns --
        // which is exactly why -stack-list-variables can never show them. They need a
        // program-wide symbol sweep instead. The frame still matters for reading them, because
        // gdb resolves a file-scoped static relative to whichever frame is selected.
        {
          name: "Globals",
          variablesReference: this.allocRef({ kind: REF_GLOBALS, level }),
          expensive: false,
        },
        {
          name: "Registers",
          variablesReference: this.allocRef({ kind: REF_REGISTERS, level }),
          expensive: false,
        },
      ],
    });
  }

  async onVariables(request) {
    const mi = this.requireMi();
    const ref = this.refs.get(request.arguments.variablesReference);
    if (!ref) return this.respond(request, { variables: [] });

    if (ref.kind === REF_REGISTERS) return this.respondRegisters(request, mi, ref.level);
    if (ref.kind === REF_LOCALS) return this.respondLocals(request, mi, ref.level);
    if (ref.kind === REF_GLOBALS) return this.respondGlobals(request, mi, ref);
    if (ref.kind === REF_GLOBALS_FILE) return this.respondGlobalsFile(request, mi, ref);
    if (ref.kind === REF_VAROBJ) return this.respondChildren(request, mi, ref);
    if (ref.kind === REF_SR) return this.respondStatusRegister(request, ref);
    return this.respond(request, { variables: [] });
  }

  async respondRegisters(request, mi, level) {
    await mi.send(`-stack-select-frame ${level}`).catch(() => {});
    const names = await mi.send("-data-list-register-names");
    const values = await mi.send("-data-list-register-values x");
    const nameList = names.results["register-names"] || [];
    const variables = (values.results["register-values"] || [])
      .map((rv) => {
        const idx = Number(rv.number);
        const name = nameList[idx];
        if (!name) return null;
        const variable = { name, value: rv.value, variablesReference: 0 };
        // sr is the one register you read bit by bit rather than as a number, so it expands
        // into named flags instead of making you decode the hex in your head.
        if (name === "sr" && decodeSr(rv.value)) {
          variable.variablesReference = this.allocRef({ kind: REF_SR, value: rv.value });
        }
        return variable;
      })
      .filter(Boolean);
    this.respond(request, { variables });
  }

  respondStatusRegister(request, ref) {
    this.respond(request, { variables: decodeSr(ref.value) || [] });
  }

  async respondLocals(request, mi, level) {
    await mi.send(`-stack-select-frame ${level}`).catch(() => {});
    // --simple-values gives names and scalar values without expanding aggregates, which is what
    // we want: aggregates get a varobj so VS Code can expand them lazily.
    const reply = await mi.send("-stack-list-variables --simple-values");
    const vars = reply.results.variables || [];
    const variables = [];
    for (const v of vars) {
      variables.push(await this.makeVariable(mi, v.name, v.name, v.value, v.type));
    }
    this.respond(request, { variables });
  }

  /**
   * Sweep the program for variables that are not locals.
   *
   * One MI round-trip, cached for the life of the loaded program. --include-nondebug matters
   * more than it looks: on the LTO builds this toolchain produces, plenty of real globals lose
   * their DWARF location and survive only as an entry in .symtab. Without it they would simply
   * be missing from the list. `globalsFilter` passes a regex straight through, for programs
   * where the whole list is too long to be useful.
   */
  async fetchGlobals(mi) {
    if (this.globalsCache) return this.globalsCache;

    const filter = this.launchArgs.globalsFilter;
    const args = filter ? ` --name ${quote(filter)}` : "";
    let reply;
    try {
      reply = await mi.send(`-symbol-info-variables --include-nondebug${args}`);
    } catch {
      // A gdb too old for -symbol-info-variables, or no symbols loaded. An empty Globals scope
      // is a better answer than failing the whole variables request.
      return { groups: [], truncated: 0 };
    }

    const symbols = reply.results.symbols || {};
    const named = new Set();
    // Keyed by file, not one group per CU: LTO splits a single source file across several
    // compilation units, and gdb duly reports it once per unit. Left alone that shows the same
    // filename twice in the tree, with the variables arbitrarily divided between them.
    /** @type {Map<string, any>} */
    const byFile = new Map();
    for (const cu of symbols.debug || []) {
      const file = baseName(cu.filename || cu.fullname);
      let group = byFile.get(file);
      if (!group) {
        group = { label: file, path: cu.fullname || cu.filename, entries: [], seen: new Set() };
        byFile.set(file, group);
      }
      for (const sym of cu.symbols || []) {
        if (group.seen.has(sym.name)) continue;
        group.seen.add(sym.name);
        named.add(sym.name);
        group.entries.push({
          name: sym.name,
          type: sym.type,
          line: sym.line ? Number(sym.line) : 0,
          // Qualifying by file is not decoration: two translation units can each hold a
          // `static int count`, and the bare name resolves to whichever gdb sees first.
          expression: `'${file}'::${sym.name}`,
        });
      }
    }
    const groups = [...byFile.values()].filter((g) => g.entries.length);
    for (const group of groups) {
      group.entries.sort((a, b) => a.name.localeCompare(b.name));
      delete group.seen;
    }
    groups.sort((a, b) => a.label.localeCompare(b.label));

    const bare = [];
    for (const sym of symbols.nondebug || []) {
      if (named.has(sym.name) || LINKER_SYMBOLS.test(sym.name) || sym.name.startsWith("__")) {
        continue;
      }
      bare.push({ name: sym.name, address: sym.address, expression: sym.name, bare: true });
    }
    bare.sort((a, b) => a.name.localeCompare(b.name));
    const truncated = Math.max(0, bare.length - MAX_BARE_GLOBALS);
    if (bare.length) {
      groups.push({ label: BARE_GROUP, path: "", entries: bare.slice(0, MAX_BARE_GLOBALS) });
    }

    this.globalsCache = { groups, truncated };
    return this.globalsCache;
  }

  async respondGlobals(request, mi, ref) {
    await mi.send(`-stack-select-frame ${ref.level}`).catch(() => {});
    const { groups, truncated } = await this.fetchGlobals(mi);
    if (!groups.length) return this.respond(request, { variables: [] });

    // One source file is the common case for a demo, and a folder the user has to open before
    // seeing anything is pure friction.
    if (groups.length === 1) {
      ref.group = groups[0];
      return this.listGlobals(request, mi, groups[0], truncated);
    }

    const variables = groups.map((group) => ({
      name: group.label,
      value: `${group.entries.length} variable${group.entries.length === 1 ? "" : "s"}`,
      variablesReference: this.allocRef({
        kind: REF_GLOBALS_FILE,
        level: ref.level,
        group,
        truncated: group.label === BARE_GROUP ? truncated : 0,
      }),
    }));
    this.respond(request, { variables });
  }

  async respondGlobalsFile(request, mi, ref) {
    await mi.send(`-stack-select-frame ${ref.level}`).catch(() => {});
    return this.listGlobals(request, mi, ref.group, ref.truncated || 0);
  }

  async listGlobals(request, mi, group, truncated) {
    const variables = [];
    for (const entry of group.entries) {
      variables.push(await this.makeGlobal(mi, entry));
    }
    if (truncated) {
      variables.push({
        name: `(${truncated} more)`,
        value: "list truncated -- narrow it with the globalsFilter launch setting",
        variablesReference: 0,
      });
    }
    this.respond(request, { variables });
  }

  /**
   * Read one global, falling back to its raw storage when DWARF has given up on it.
   *
   * On -Ofast + LTO most globals come back `<optimized out>`: the compiler dropped the DWARF
   * location while leaving the storage exactly where it was. .symtab still knows the address
   * and DWARF still knows the type, and putting those two together recovers a real live value
   * from what would otherwise be a dead row.
   */
  async makeGlobal(mi, entry) {
    if (entry.bare) {
      // Known only from .symtab: an address and a name, with no type to read it through. The
      // address is real though, and that is enough to open the byte view on it.
      return {
        name: entry.name,
        value: `${entry.address} <no debug info>`,
        variablesReference: 0,
        memoryReference: entry.address,
      };
    }

    const direct = await this.makeVariable(mi, entry.name, entry.expression, undefined, entry.type, {
      wantAddress: true,
    });
    // An aggregate gdb has no location for still prints as `{...}`, because that comes from the
    // type and not from any storage -- so the row reads as live while every field inside it
    // will say <optimized out>. The missing address is the tell, and the only one: no address,
    // no storage.
    const unlocated =
      !direct.memoryReference &&
      (isOptimisedOut(direct.value) || AGGREGATE_PLACEHOLDER.test(direct.value));
    if (!unlocated || !entry.type) return direct;

    const address = await this.symbolAddress(mi, entry.name);
    if (!address) {
      // Say so on the row itself rather than making the user expand it to find out.
      if (!isOptimisedOut(direct.value)) direct.value = `${direct.value} <optimized out>`;
      return direct;
    }
    // `{TYPE} ADDR` rather than a cast: it reads structs and arrays as cleanly as scalars,
    // where `*(UWORD [12] *)addr` is not even valid syntax.
    const recovered = await this.makeVariable(
      mi,
      entry.name,
      `{${entry.type}} ${address}`,
      undefined,
      entry.type
    );
    if (isOptimisedOut(recovered.value)) return direct;
    recovered.memoryReference = address;
    return recovered;
  }

  /** Ask gdb where a symbol lives, for symbols DWARF no longer has a location for. */
  async symbolAddress(mi, name) {
    const text = await mi.console(`info address ${name}`).catch(() => "");
    const match = /at address (0x[0-9a-fA-F]+)/.exec(text);
    return match ? match[1] : null;
  }

  /**
   * Build a DAP variable, creating a gdb varobj when the value looks expandable so the client
   * can drill into structs and arrays.
   */
  async makeVariable(mi, display, expression, value, type, opts = {}) {
    const out = { name: display, value: value === undefined ? "" : String(value), variablesReference: 0 };
    if (type) out.type = type;
    // A missing value means an aggregate gdb declined to print inline; a varobj can expand it.
    const looksAggregate = value === undefined || value === "{...}";
    if (looksAggregate) {
      try {
        const name = `pv${this.varCounter++}`;
        const created = await mi.send(`-var-create ${name} * ${quote(expression)}`);
        const children = Number(created.results.numchild || 0);
        this.varObjects.add(name);
        out.value = created.results.value || out.value || (created.results.type || "");
        if (created.results.type) out.type = created.results.type;
        if (children > 0) {
          out.variablesReference = this.allocRef({ kind: REF_VAROBJ, varName: name, expression });
        }
      } catch {
        // Leave it as a plain value; failing to expand is not worth failing the request over.
      }
    }
    await this.attachAddress(mi, out, expression, opts.wantAddress);
    return out;
  }

  /**
   * Give a variable an address, which is what lets VS Code offer "View Binary Data" on it.
   *
   * A pointer already prints as one, so that case is free. Anything else needs `&expr`, and
   * that is a round-trip per variable -- so it is only spent where the byte view is actually
   * wanted: aggregates (a chip-RAM buffer is the whole use case) and globals. Register-
   * allocated locals have no address at all and gdb says so; that is expected, and silent.
   */
  async attachAddress(mi, out, expression, force) {
    const pointer = /^(0x[0-9a-fA-F]+)/.exec(out.value || "");
    if (pointer) {
      out.memoryReference = pointer[1];
      return;
    }
    if (!force && !out.variablesReference) return;
    try {
      const reply = await mi.send(`-data-evaluate-expression ${quote(`&(${expression})`)}`);
      const match = /^(0x[0-9a-fA-F]+)/.exec(String(reply.results.value || ""));
      if (match) out.memoryReference = match[1];
    } catch {
      // No address: optimised out, or living in a register.
    }
  }

  async respondChildren(request, mi, ref) {
    const reply = await mi.send(`-var-list-children --all-values ${ref.varName}`);
    const kids = reply.results.children || [];
    // An edit to a child arrives as its display name against the *parent* handle, so the parent
    // has to remember which varobj each name belongs to or there is nothing to assign to.
    ref.childVars = new Map();
    const variables = [];
    for (const kid of kids) {
      ref.childVars.set(kid.exp, kid.name);
      const child = {
        name: kid.exp,
        value: kid.value === undefined ? (kid.type || "") : String(kid.value),
        variablesReference: 0,
      };
      if (kid.type) child.type = kid.type;
      if (Number(kid.numchild || 0) > 0) {
        child.variablesReference = this.allocRef({
          kind: REF_VAROBJ,
          varName: kid.name,
          expression: kid.exp,
        });
      }
      const pointer = /^(0x[0-9a-fA-F]+)/.exec(String(kid.value || ""));
      if (pointer) child.memoryReference = pointer[1];
      variables.push(child);
    }
    this.respond(request, { variables });
  }

  async onSetVariable(request) {
    const mi = this.requireMi();
    const { name, value } = request.arguments;
    const ref = this.refs.get(request.arguments.variablesReference);

    // A struct field or array element arrives as its display name -- "[0]", "y" -- and writing
    // `set var [0]=5` either fails or, worse, finds something else called y. The varobj gdb
    // already made for the child is the only thing that identifies it unambiguously.
    const childVar = ref && ref.childVars && ref.childVars.get(name);
    if (childVar) {
      const assigned = await mi.send(`-var-assign ${childVar} ${quote(value)}`);
      return this.respond(request, { value: assigned.results.value });
    }
    if (ref && ref.kind === REF_SR) {
      return this.fail(request, "Edit sr itself; its flags are a read-only decode of it.");
    }

    // The frame matters: without selecting it first, editing a local while a caller is selected
    // in the Call Stack pane writes to the innermost frame's variable of that name instead.
    if (ref && ref.level !== undefined) {
      await mi.send(`-stack-select-frame ${ref.level}`).catch(() => {});
    }

    const target = assignTarget(ref, name);
    await mi.send(`-gdb-set var ${target}=${value}`);
    const read = await mi.send(`-data-evaluate-expression ${quote(target)}`);
    this.respond(request, { value: read.results.value });
  }

  async onSetExpression(request) {
    const mi = this.requireMi();
    const { expression, value } = request.arguments;
    const frameRef = this.refs.get(request.arguments.frameId);
    if (frameRef) await mi.send(`-stack-select-frame ${frameRef.level}`).catch(() => {});
    await mi.send(`-gdb-set var ${expression}=${value}`);
    const read = await mi.send(`-data-evaluate-expression ${quote(expression)}`);
    const variable = await this.makeVariable(mi, expression, expression, read.results.value);
    this.respond(request, {
      value: variable.value,
      type: variable.type,
      variablesReference: variable.variablesReference,
      memoryReference: variable.memoryReference,
    });
  }

  async onEvaluate(request) {
    const mi = this.requireMi();
    const expression = request.arguments.expression || "";
    const frameRef = this.refs.get(request.arguments.frameId);

    // The Debug Console doubles as a gdb prompt: "-exec <cmd>" runs a raw gdb command, which is
    // how a user reaches anything MI does not model -- including Peekaboo's monitor commands.
    if (expression.startsWith("-exec ")) {
      const text = await mi.console(expression.slice(6));
      return this.respond(request, { result: text.trimEnd(), variablesReference: 0 });
    }
    if (frameRef) await mi.send(`-stack-select-frame ${frameRef.level}`).catch(() => {});

    try {
      const reply = await mi.send(`-data-evaluate-expression ${quote(expression)}`);
      const variable = await this.makeVariable(mi, expression, expression, reply.results.value);
      this.respond(request, {
        result: variable.value,
        type: variable.type,
        variablesReference: variable.variablesReference,
        memoryReference: variable.memoryReference,
      });
    } catch (err) {
      // Hovers evaluate every identifier under the cursor, most of which are not expressions,
      // so a failure here is routine and must not be shouted about.
      if (request.arguments.context === "hover") {
        return this.respond(request, { result: "", variablesReference: 0 });
      }
      throw err;
    }
  }

  // ---------------------------------------------------------------- execution

  async onContinue(request) {
    const mi = this.requireMi();
    this.running = true;
    this.respond(request, { allThreadsContinued: true });
    this.event("continued", { threadId: THREAD_ID, allThreadsContinued: true });
    mi.send("-exec-continue").catch((err) => this.output(`${err.message}\n`, "stderr"));
  }

  async onReverseContinue(request) {
    const mi = this.requireMi();
    this.running = true;
    this.respond(request);
    this.event("continued", { threadId: THREAD_ID, allThreadsContinued: true });
    // Running out of recorded history is a normal outcome, not a failure: gdb reports it as
    // "Reached end of recorded history" and the target stays where it is.
    mi.send("-exec-continue --reverse").catch((err) => this.output(`${err.message}
`, "stderr"));
  }

  async onStep(request, command) {
    const mi = this.requireMi();
    const granularity = request.arguments && request.arguments.granularity;
    let cmd = command;
    // VS Code's "step by instruction" toggle maps onto the -instruction variants.
    if (granularity === "instruction") {
      if (command === "-exec-next") cmd = "-exec-next-instruction";
      else if (command === "-exec-step") cmd = "-exec-step-instruction";
    }
    this.respond(request);
    this.running = true;
    mi.send(cmd).catch((err) => this.output(`${err.message}\n`, "stderr"));
  }

  async onPause(request) {
    const mi = this.requireMi();
    this.respond(request);
    // -exec-interrupt needs async mode; the RSP Ctrl-C path is what actually halts the machine,
    // so fall back to it when gdb refuses.
    try {
      await mi.send("-exec-interrupt");
    } catch {
      await mi.console("interrupt").catch(() => {});
    }
  }

  // ---------------------------------------------------------------- memory and disassembly

  async onDisassemble(request) {
    const mi = this.requireMi();
    const { memoryReference, instructionCount } = request.arguments;
    const offset = request.arguments.instructionOffset || 0;
    const start = `${memoryReference}${offset ? `+${offset * 2}` : ""}`;
    // Mode 5 is source-and-disassembly with opcode bytes: on -Ofast code, seeing which
    // instructions a source line actually produced is usually the answer.
    const reply = await mi.send(
      `-data-disassemble -s ${start} -e ${start}+${Math.max(instructionCount, 1) * 8} -- 5`
    );
    const instructions = [];
    const asmLines = reply.results.asm_insns || [];
    for (const entry of asmLines) {
      // In mode 5 the outer entries are src_and_asm_line tuples carrying a nested list.
      const inner = entry.line_asm_insn || [entry];
      for (const insn of inner) {
        if (!insn || !insn.address) continue;
        const item = {
          address: insn.address,
          instruction: insn.inst || "",
          instructionBytes: insn.opcodes,
        };
        if (entry.line) item.line = Number(entry.line);
        if (entry.fullname || entry.file) {
          item.location = { name: baseName(entry.file || entry.fullname), path: entry.fullname || entry.file };
        }
        instructions.push(item);
      }
    }
    this.respond(request, { instructions });
  }

  async onReadMemory(request) {
    const mi = this.requireMi();
    const { memoryReference, count } = request.arguments;
    const offset = request.arguments.offset || 0;
    const reply = await mi.send(
      `-data-read-memory-bytes ${memoryReference}${offset ? `+${offset}` : ""} ${count}`
    );
    const chunks = reply.results.memory || [];
    const hex = chunks.map((c) => c.contents || "").join("");
    const bytes = Buffer.from(hex, "hex");
    this.respond(request, {
      address: chunks.length ? chunks[0].begin : memoryReference,
      data: bytes.toString("base64"),
      unreadableBytes: bytes.length < count ? count - bytes.length : 0,
    });
  }

  async onWriteMemory(request) {
    const mi = this.requireMi();
    const { memoryReference, data } = request.arguments;
    const offset = request.arguments.offset || 0;
    const bytes = Buffer.from(data, "base64");
    await mi.send(
      `-data-write-memory-bytes ${memoryReference}${offset ? `+${offset}` : ""} ${bytes.toString("hex")}`
    );
    this.respond(request, { bytesWritten: bytes.length });
  }
}

// ---------------------------------------------------------------- helpers

/** Wrap a value in the quotes MI expects, escaping what would otherwise end the string. */
function quote(text) {
  return `"${String(text).replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"`;
}

/** True for the several ways gdb says "the compiler threw this away". */
function isOptimisedOut(value) {
  return /<optimi[sz]ed out>/.test(String(value || ""));
}

/**
 * Split a 68000 status register into named flags.
 *
 * Bit 15 is T (trace), 13 is S (supervisor), 10-8 the interrupt priority mask, and 4-0 the
 * condition codes XNZVC. Returns null when the value is not a number, which is how the caller
 * decides whether sr is worth making expandable at all.
 */
function decodeSr(raw) {
  const sr = Number(raw);
  if (!Number.isFinite(sr)) return null;
  const bit = (n) => ({ value: String((sr >> n) & 1), variablesReference: 0 });
  return [
    { name: "C (carry)", ...bit(0) },
    { name: "V (overflow)", ...bit(1) },
    { name: "Z (zero)", ...bit(2) },
    { name: "N (negative)", ...bit(3) },
    { name: "X (extend)", ...bit(4) },
    { name: "IPL (interrupt mask)", value: String((sr >> 8) & 7), variablesReference: 0 },
    { name: "S (supervisor)", ...bit(13) },
    { name: "T (trace)", ...bit(15) },
  ];
}

/**
 * The expression that names a variable for assignment, given the scope it was listed under.
 */
function assignTarget(ref, name) {
  // Registers are gdb convenience variables, not program symbols: without the $, gdb goes
  // looking for something called "d0" and either misses or finds the wrong thing entirely.
  if (ref && ref.kind === REF_REGISTERS) return `$${name}`;
  // Globals were listed file-qualified for a reason; assigning to the bare name would put the
  // value in whichever same-named static gdb happens to resolve first.
  if (ref && ref.group) {
    const entry = ref.group.entries.find((e) => e.name === name);
    if (entry) return entry.expression;
  }
  return name;
}

function baseName(p) {
  if (!p) return "";
  const parts = String(p).split(/[\\/]/);
  return parts[parts.length - 1];
}

/** Compare paths without caring about separator style or case (Windows). */
function normalisePath(p) {
  return String(p || "").replace(/\\/g, "/").toLowerCase();
}

module.exports = { PeekabooSession, quote, baseName, normalisePath, decodeSr, assignTarget };
