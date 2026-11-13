# Peekaboo MCP bridge (Claude Desktop integration)

Lets [Claude Desktop](https://claude.ai/download) read and annotate the live state of a running
Peekaboo instance — registers, chipset/CIA registers, memory, disassembly, labels/comments/remarks,
screenshots — and drive execution, so you can have a natural-language reverse-engineering
conversation about Amiga games/demos or your own native 68k code.

## How it works

Peekaboo runs a small JSON-over-HTTP API on `127.0.0.1` (it can't be a stdio MCP server itself —
it's a long-lived GUI holding the emulator session). `peekaboo_mcp_bridge.py` is a thin stdio↔HTTP
shim using the official `mcp` SDK: Claude Desktop launches the bridge, and the bridge forwards tool
calls to Peekaboo. The tool list is fetched from Peekaboo at startup, so the bridge is generic.

```
Claude Desktop  ──stdio(MCP)──►  peekaboo_mcp_bridge.py  ──HTTP──►  Peekaboo (127.0.0.1:7475)
```

Every tool call Claude makes is logged in Peekaboo's **MCP Activity** window (Windows ▸ Debugger ▸
MCP Activity), and `exec_console` commands are echoed into the AR console — so you can watch how
Claude uses the system.

## Setup

1. **Enable the server in Peekaboo:** Preferences ▸ *Claude / MCP Server* ▸ enable it. Note the
   port (default `7475`) and token, then **restart Peekaboo**. Click *Copy Claude Desktop config* to
   put a ready-made snippet on your clipboard.

2. **Install the bridge dependencies** (Python 3.10–3.13; the `mcp` SDK may lag the newest
   release, and some Homebrew Python 3.14 builds have a broken `pyexpat`). A virtualenv keeps the
   deps isolated and gives Claude Desktop a stable interpreter to point at:

   ```bash
   python3.13 -m venv mcp/.venv          # or python3.12
   mcp/.venv/bin/python -m pip install -r mcp/requirements.txt
   ```

3. **Add the bridge to Claude Desktop.** Edit `claude_desktop_config.json`
   (macOS: `~/Library/Application Support/Claude/claude_desktop_config.json`). Point `command` at
   the **venv interpreter** so the `mcp` package is found:

   ```json
   {
     "mcpServers": {
       "peekaboo": {
         "command": "/absolute/path/to/peekaboo/mcp/.venv/bin/python",
         "args": ["/absolute/path/to/peekaboo/mcp/peekaboo_mcp_bridge.py"],
         "env": {
           "PEEKABOO_MCP_URL": "http://127.0.0.1:7475",
           "PEEKABOO_MCP_TOKEN": "<token from Preferences>"
         }
       }
     }
   }
   ```

   (The *Copy Claude Desktop config* button fills in the bridge path, port and token — adjust
   `command` to the venv interpreter shown above if you used a virtualenv.)

4. **Restart Claude Desktop.** The `peekaboo` tools should appear. Make sure Peekaboo is running
   with a disk/program booted before asking Claude to inspect state.

## Tools (summary)

- **Read:** `get_status`, `get_registers`, `get_custom_registers`, `get_cia_registers`,
  `list_memory_regions`, `read_memory`, `disassemble` (includes existing annotations),
  `disassemble_copper` (`COP1`, `COP2`, an address, or the current frame's `Trace`, with raw
  instruction bytes, disassembly text and register names), `get_annotations`, `screenshot`
  (honours the display masks — see below), `get_display_masks`.
- **Annotate:** `set_label`, `set_comment`, `set_block`, `remove_annotation`.
- **Control:** `pause`, `resume`, `step`, `step_over`, `step_frame` (step whole frames while
  paused), `run_until_frame` (free-run to a target frame, optional `turbo`, auto-pauses),
  `click_mouse` (left/right Amiga mouse click with configurable duration in emulated frames),
  `add_breakpoint`, `remove_breakpoint`, `list_breakpoints`, `add_watchpoint`,
  `remove_watchpoint`, `list_watchpoints`.
- **KPrintF traps:** `add_kprintf_breakpoint`, `remove_kprintf_breakpoint`,
  `list_kprintf_breakpoints`, `clear_kprintf_breakpoints` — break when the program's serial debug
  output (KPrintF) matches a string or regex. Matching is line-based, so execution stops at the
  end of the matching line; `get_status` then reports `lastKprintfMatch`. Same store as the
  Breakpoints window's *KPrintF text* section and the console `BK`/`BKS`/`BKD`/`BKDA` commands.
- **Write:** `write_memory`, `write_custom_register`, `load_document`, `run_document`,
  `list_documents`.
- **Profiling:** `set_cpu_trace` (arm the per-instruction trace buffer that feeds the profiler),
  `get_profile` (per-function / per-instruction / flat / flame / call-stack snapshot of the last
  traced frame — `view` selects which). Enable tracing and run a frame first; the named-function,
  flame and call-stack views also need debug symbols from a sibling `.elf`.
- **Display masking:** `get_display_masks`, `set_display_masks` (show/hide individual bitplanes
  BPL1-8, sprites SPR0-7 and playfields PF1/PF2 to isolate what each layer draws). Masking is
  cosmetic — the machine still executes and renders everything, only the final pixel composition
  is filtered — and it is applied inside the core's renderer, so `screenshot` captures the masked
  image: mask a layer, screenshot, and you see exactly what that layer contributes. Changing a
  mask re-renders the current frame, so this works while paused. Hiding a playfield also hides its
  planes (PF1 = BPL1/3/5, PF2 = BPL2/4/6); the composed result is reported as
  `effectiveBitplaneMask`. Same state as the debugger's Display window and the console
  `BPL`/`SPR`/`PF`/`DISPLAY` commands.
- **Blitter:** `get_blitter_log` (the current frame's blitter operations — mirrors the Blitter
  Operations window: per-blit timing, trigger, op, size, minterm, BLTCON0/1, DMA cost and the
  active A/B/C/D channel addresses+modulos, plus frame totals and bus usage). Needs the emulator
  running; captured fresh each frame (no CPU trace required).
- **Disk I/O:** `get_disk_log` (every disk DMA transfer this session — mirrors the Disk I/O
  window: direction, the PC that armed it and whether the CPU or Copper wrote DSKLEN, DSKPT
  target, DSKLEN, drive/cyl/side, byte count, a SHA-1 of the transferred bytes, and the AmigaDOS
  sectors decoded out of the MFM payload with per-sector hashes and checksum status). This is the
  emulated program's *own* floppy traffic, as opposed to `read_disk_track`/`read_disk_sectors`
  which read the image directly. The summary fields (`writes`, `bytesWritten`, `anyWrites`) answer
  "did this program write anything back to the disk?" in a single call.
  `add_disk_breakpoint` / `remove_disk_breakpoint` trap a transfer as it starts, so a disk write
  halts on the instruction that caused it; `access`/`track`/`cyl`/`side`/`addrLo`/`addrHi` are
  evaluated when DSKLEN is written, while a `sector` filter needs the MFM headers and so halts at
  transfer completion instead (the log still reports the arming PC). Needs the emulator running
  and a core built from PuAE >= 1.19.0 (`scripts/build_puae`).
- **Console:** `exec_console` remains available for free-form input. Every command family also has
  a discoverable, dedicated MCP tool (for example `inspect_memory_hex`, `set_cpu_register`,
  `manage_breakpoints`, `trainer_search`, `read_disk_track`, and `list_exec_tasks`). These tools
  route through the same `ArCommandInterpreter` instance as the UI, so console and MCP parsing,
  validation, side effects, and output cannot drift apart. Families with aliases expose a
  `variant` enum; `arguments` contains the normal console arguments without the command token.
  Interactive memory-edit and assembler tools additionally accept an `input` array of follow-up
  lines and return a transcript.

## Quick test without Claude

```bash
TOKEN=<token>
curl -s -H "X-Peekaboo-Token: $TOKEN" http://127.0.0.1:7475/tools | jq '.[].name'
curl -s -X POST -H "X-Peekaboo-Token: $TOKEN" http://127.0.0.1:7475/tool \
     -d '{"name":"get_registers","args":{}}' | jq
```

## Security

The server binds to `127.0.0.1` only and requires the token on every request, so other local
processes can't drive your emulator without it. Don't expose the port beyond localhost.
