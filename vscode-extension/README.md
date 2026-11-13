# Peekaboo Compile & Run for VS Code

This companion extension adds a **Peekaboo** entry to VS Code's Run and Debug target list. It
uses Bartman's existing `compile` task, then sends the newly built Amiga HUNK executable to a
running Peekaboo instance. It does not modify Bartman's extension.

## Install from this repository

Package and install the extension:

```powershell
cd vscode-extension
npx @vscode/vsce package --out peekaboo-run.vsix
code --install-extension .\peekaboo-run.vsix
```

Restart VS Code after installing it.

## Add the target to a Bartman project

Open the project and run **Peekaboo: Add Compile & Run Target** from the Command Palette. The
command adds this entry to `.vscode/launch.json` without changing the existing AROS and Amiga
targets:

```json
{
  "type": "peekaboo",
  "request": "launch",
  "name": "Peekaboo",
  "preLaunchTask": "compile",
  "program": "${workspaceFolder}/${config:amiga.program}"
}
```

Start Peekaboo, select **Peekaboo** in Run and Debug, and press F5. The debug console reports the
handoff and the debug session then ends; debugging and profiling continue inside Peekaboo.

The default connection is `http://127.0.0.1:7474`. Add a `server` property to the launch target
only if Peekaboo has been configured differently.
