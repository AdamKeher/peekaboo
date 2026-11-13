"use strict";
// @ts-check

const vscode = require("vscode");
const client = require("./peekabooClient");
const { PeekabooSession } = require("./adapter/session");

/**
 * Peekaboo's VS Code integration.
 *
 * The "peekaboo" debug target compiles with Bartman's task, hands the executable to a running
 * Peekaboo, and then debugs it there: source-level breakpoints, variables, watches and call
 * stacks appear in VS Code's own debug panels, while the copper, blitter, sprite and
 * frame-scrubber windows stay in Peekaboo where they are far richer than any tree view here.
 * Bartman's own "amiga" target is untouched and still available.
 *
 * The adapter runs in-process (DebugAdapterInlineImplementation) rather than as a separate
 * executable, because it needs the vscode API from inside the session.
 */

class PeekabooDebugAdapter {
  constructor(workspaceFolder) {
    this.workspaceFolder = workspaceFolder;
    this.emitter = new vscode.EventEmitter();
    this.onDidSendMessage = this.emitter.event;
    this.session = new PeekabooSession((message) => this.emitter.fire(message), {
      log: (text) => console.log(`[peekaboo] ${text}`),
    });
  }

  handleMessage(message) {
    // launch is the one request that needs work before the session sees it: the program has to
    // reach Peekaboo first, and only this layer knows the workspace folder.
    if (message.type === "request" && message.command === "launch") {
      this.prepareLaunch(message).catch((error) => {
        this.emitter.fire({
          type: "response",
          request_seq: message.seq,
          success: false,
          command: "launch",
          message: error instanceof Error ? error.message : String(error),
        });
      });
      return;
    }
    this.session.handleMessage(message);
  }

  async prepareLaunch(message) {
    const args = message.arguments || {};
    // request:"attach" (or attach:true) debugs whatever Peekaboo already has running, which
    // matters when the interesting state took a while to reach and re-running would lose it.
    if (!args.attach) {
      const program = await client.runInPeekaboo({
        program: args.program,
        workspaceFolder: this.workspaceFolder,
        server: args.server,
        focus: args.focus,
        stopOnEntry: args.stopOnEntry,
      });
      message.arguments = { ...args, program };
    }
    await this.session.handleMessage(message);
  }

  dispose() {
    this.emitter.dispose();
  }
}

class PeekabooAdapterFactory {
  createDebugAdapterDescriptor(session) {
    const folder = session.workspaceFolder?.uri.fsPath;
    return new vscode.DebugAdapterInlineImplementation(new PeekabooDebugAdapter(folder));
  }
}

function targetConfiguration() {
  return {
    type: "peekaboo",
    request: "launch",
    name: "Peekaboo",
    preLaunchTask: "compile",
    program: "${workspaceFolder}/${config:amiga.program}",
  };
}

async function chooseWorkspaceFolder() {
  const folders = vscode.workspace.workspaceFolders || [];
  if (folders.length === 0) {
    throw new Error("Open a Bartman Amiga project before adding the Peekaboo target.");
  }
  if (folders.length === 1) {
    return folders[0];
  }
  const selected = await vscode.window.showWorkspaceFolderPick({
    placeHolder: "Select the Bartman Amiga project",
  });
  if (!selected) {
    throw new Error("No workspace folder selected.");
  }
  return selected;
}

async function addLaunchTarget() {
  try {
    const folder = await chooseWorkspaceFolder();
    const launch = vscode.workspace.getConfiguration("launch", folder.uri);
    const configurations = launch.get("configurations", []);
    if (configurations.some((entry) => entry && entry.type === "peekaboo")) {
      vscode.window.showInformationMessage("This project already has a Peekaboo launch target.");
      return;
    }
    await launch.update(
      "configurations",
      [...configurations, targetConfiguration()],
      vscode.ConfigurationTarget.WorkspaceFolder
    );
    vscode.window.showInformationMessage(
      "Added Peekaboo to Run and Debug. Start Peekaboo, select the target, and press F5."
    );
  } catch (error) {
    vscode.window.showErrorMessage(error instanceof Error ? error.message : String(error));
  }
}

async function checkConnection() {
  try {
    await client.checkConnection();
    vscode.window.showInformationMessage("Connected to Peekaboo on 127.0.0.1:7474.");
  } catch (error) {
    vscode.window.showErrorMessage(error instanceof Error ? error.message : String(error));
  }
}

function activate(context) {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("peekaboo", new PeekabooAdapterFactory()),
    vscode.commands.registerCommand("peekaboo.addLaunchTarget", addLaunchTarget),
    vscode.commands.registerCommand("peekaboo.checkConnection", checkConnection)
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
