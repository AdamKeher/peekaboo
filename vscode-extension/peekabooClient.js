"use strict";

const fs = require("fs");
const http = require("http");
const https = require("https");
const path = require("path");

const DEFAULT_SERVER = "http://127.0.0.1:7474";

function resolveProgram(configuredProgram, workspaceFolder) {
  if (typeof configuredProgram !== "string" || configuredProgram.trim() === "") {
    throw new Error("The Peekaboo launch target has no program path.");
  }

  let candidate = configuredProgram;
  if (!path.isAbsolute(candidate)) {
    if (!workspaceFolder) {
      throw new Error(`Program path is relative and no workspace folder is available: ${candidate}`);
    }
    candidate = path.resolve(workspaceFolder, candidate);
  }

  const candidates = path.extname(candidate).toLowerCase() === ".exe"
    ? [candidate]
    : [`${candidate}.exe`, candidate];
  const program = candidates.find((entry) => {
    try {
      return fs.statSync(entry).isFile();
    } catch {
      return false;
    }
  });

  if (!program) {
    throw new Error(`Unable to find Bartman's compiled executable at ${candidates.join(" or ")}.`);
  }
  return path.resolve(program);
}

function apiUrl(server = DEFAULT_SERVER, route = "/api/run") {
  const url = new URL(server);
  if (url.protocol !== "http:" && url.protocol !== "https:") {
    throw new Error(`Unsupported Peekaboo server protocol: ${url.protocol}`);
  }
  if (url.pathname === "/" || url.pathname === "") {
    url.pathname = route;
  }
  return url;
}

function requestJson(url, method, body, timeoutMs = 3000) {
  const payload = body === undefined ? undefined : Buffer.from(JSON.stringify(body), "utf8");
  const transport = url.protocol === "https:" ? https : http;

  return new Promise((resolve, reject) => {
    const request = transport.request({
      protocol: url.protocol,
      hostname: url.hostname,
      port: url.port,
      path: `${url.pathname}${url.search}`,
      method,
      headers: payload ? {
        "Content-Type": "application/json",
        "Content-Length": payload.length
      } : undefined
    }, (response) => {
      const chunks = [];
      let length = 0;
      response.on("data", (chunk) => {
        length += chunk.length;
        if (length <= 64 * 1024) {
          chunks.push(chunk);
        }
      });
      response.on("end", () => {
        const text = Buffer.concat(chunks).toString("utf8");
        let responseBody;
        try {
          responseBody = text ? JSON.parse(text) : {};
        } catch {
          responseBody = { message: text };
        }

        if (response.statusCode >= 200 && response.statusCode < 300) {
          resolve(responseBody);
          return;
        }
        const detail = responseBody.error || responseBody.message || `HTTP ${response.statusCode}`;
        reject(new Error(`Peekaboo rejected the request: ${detail}`));
      });
    });

    request.setTimeout(timeoutMs, () => {
      request.destroy(new Error(`Timed out connecting to Peekaboo at ${url.origin}.`));
    });
    request.on("error", (error) => {
      const connectionError = error.code === "ECONNREFUSED"
        ? new Error(`Cannot reach Peekaboo at ${url.origin}. Is Peekaboo running?`)
        : error;
      reject(connectionError);
    });
    if (payload) {
      request.write(payload);
    }
    request.end();
  });
}

function runInPeekaboo(options) {
  const program = resolveProgram(options.program, options.workspaceFolder);
  const url = apiUrl(options.server || DEFAULT_SERVER);
  const body = { program, focus: options.focus !== false };
  // Only send stopOnEntry when it was asked for: older Peekaboo builds reject unknown fields.
  if (options.stopOnEntry) {
    body.stopOnEntry = true;
  }
  return requestJson(url, "POST", body).then(() => program);
}

function checkConnection(server = DEFAULT_SERVER) {
  return requestJson(apiUrl(server, "/api/status"), "GET");
}

/**
 * Wait until Peekaboo reports the launched program is actually in memory.
 *
 * This has to happen before gdb attaches, not after. Attaching halts the machine -- RSP's
 * contract is that the target is stopped when gdb connects -- and a machine halted midway
 * through booting Kickstart never gets round to loading the program the debugger is waiting
 * for. Polling over HTTP leaves the emulator running, so the boot can finish.
 *
 * @param {{server?:string, timeoutMs?:number, intervalMs?:number,
 *          onProgress?:(t:string)=>void}} [options]
 * @returns {Promise<{supported:boolean, loaded:boolean, timedOut:boolean}>}
 */
async function waitForProgramLoaded(options = {}) {
  const url = apiUrl(options.server || DEFAULT_SERVER, "/api/status");
  const timeoutMs = options.timeoutMs || 180000;
  const intervalMs = options.intervalMs || 250;
  const started = Date.now();
  const deadline = started + timeoutMs;
  let announced = false;
  let lastProgress = 0;

  for (;;) {
    let status = null;
    try {
      status = await requestJson(url, "GET", undefined, 3000);
    } catch {
      // A transient failure while the machine is busy loading; keep trying until the deadline.
    }
    if (status && typeof status.programLoaded !== "boolean") {
      // An older Peekaboo with no readiness field. Say so rather than waiting out the whole
      // budget for an answer that will never come.
      return { supported: false, loaded: false, timedOut: false };
    }
    if (status && status.programLoaded) {
      return { supported: true, loaded: true, timedOut: false };
    }

    const now = Date.now();
    if (now >= deadline) return { supported: true, loaded: false, timedOut: true };
    if (options.onProgress) {
      if (!announced) {
        announced = true;
        lastProgress = now;
        options.onProgress("Waiting for the program to load...\n");
      } else if (now - lastProgress >= 5000) {
        lastProgress = now;
        options.onProgress(`  still booting (${Math.round((now - started) / 1000)}s)...\n`);
      }
    }
    await new Promise((resolve) => setTimeout(resolve, intervalMs));
  }
}

module.exports = {
  DEFAULT_SERVER,
  apiUrl,
  checkConnection,
  requestJson,
  resolveProgram,
  runInPeekaboo,
  waitForProgramLoaded
};
