# PUAE Libretro Patches

`patches/` holds everything the `libretro-uae` core is changed by to become Peekaboo's PuAE
core, and this file explains why each change exists. [Every entry in the series](#every-entry-in-the-series)
lists all of them in order; the sections after it explain the larger pieces of work.

## How the patches are applied

[`patches/series.txt`](patches/series.txt) is the single ordered list of everything done to
`external/libretro-uae` between extracting it and running `make`. Both `scripts/build_puae.ps1`
(Windows) and `scripts/build_puae.sh` (macOS/Linux) read it, so a change reaches every platform
at once and the two scripts cannot drift apart. Verified: run either script's patch phase over a
fresh extract and the resulting trees are byte-identical.

A series entry is one of two things:

| Entry | Meaning |
| --- | --- |
| `<path>` | A patch file under `patches/`, applied with `git apply` |
| `@<phase>` | A named step implemented in both build scripts |

There are only eight phases, for the handful of things a patch cannot express: fetching a second
repository and vendoring files out of it (`@vendor-floppybridge`, `@vendor-picasso96`), copying
whole files we own into the tree (`@install-floppybridge-lib`, `@install-adm-includes`,
`@install-picasso-host`, `@install-link-t`), appending `adm_blitvis.c` to `libretro-core.c`
(`@append-blitvis`), and stamping the build version string (`@set-version`). Everything else is a
patch file.

### Patch files

* `patches/*.patch` — hand-written patches with their own preambles, referenced by name from the
  docs (the FloppyBridge ones especially).
* `patches/series/NNNN-name.patch` — the rest, one per logical change, numbered by position in
  the series. These were generated from the tree the old scripts produced, so each carries exact
  context against upstream `2b0ed42`.

Patches apply with exact context, so **order matters**: a patch's context includes every earlier
patch's output. Add new work at the end of `series.txt` unless it has to interleave; the numeric
prefixes leave gaps for that.

### Changing a patch

Edit the `.patch` file (or the file under `patches/` a phase installs), then rebuild:

```powershell
rm -r -force .\external\libretro-uae\ ; .\scripts\build_puae.ps1     # Windows
rm -rf ./external/libretro-uae && ./scripts/build_puae.sh              # macOS / Linux
```

A re-run over an existing tree is cheap and safe: each applied entry is recorded in
`external/libretro-uae/.adm-series-applied`, so only new entries are applied, while the phases
re-run unconditionally (editing an `.inc`, `link.T` or a `floppybridge/` source and re-running
picks the change up without a fresh extract). A patch that will not apply aborts the build and
says so — it never half-applies and carries on.

### Adding a new `adm_*` export

Add the symbol to [`patches/link.T`](patches/link.T). That file *is* the linker version script,
copied over upstream's by `@install-link-t`; without an entry there `--version-script` hides the
symbol however correctly it was compiled.

### libretro-uae-multidrive.patch

This patch modifies the disk control interface to support arbitrary disk insertion into drives
DF0-DF3. By default, the core treats the disk index as a swap list for DF0. This patch changes it
so that index 0 -> DF0, index 1 -> DF1, index 2 -> DF2, index 3 -> DF3.

### The second and third pinned upstream commits

Two of the eight phases fetch source from somewhere other than libretro-uae, and each pins its
own commit. Both must stay identical in `build_puae.ps1` and `build_puae.sh`, for the same
reason `$UpstreamSha` must: otherwise the two platforms build different cores.

| Phase | Repository | Pin | Why |
| --- | --- | --- | --- |
| `@vendor-floppybridge` | RobSmithDev/FloppyDriveBridge | `710fa15` (v1.6.0.5) | The real Greaseweazle / DrawBridge / SCP drivers, compiled in. |
| `@vendor-picasso96` | tonioni/WinUAE | `51b7be3` | The Picasso96 module. See below. |

## Every entry in the series

In `series.txt` order, under the group headings it uses. Each `series/` patch starts with a
preamble that explains it in full; the newer ones (from 1160 on) say why as well as what.

### Upstream fixes and host-integration patches

| Entry | What it does |
| --- | --- |
| `libretro-uae-multidrive.patch` | libretro-core.c: the disk-control index selects a drive (0-3 = DF0-DF3), not a DF0 swap list |
| `libretro-uae-filesys-comm-pipe.patch` | filesys.c: unit pipes start zeroed, so a re-initialisation never frees indeterminate handles |
| `libretro-uae-vfs-readonly.patch` | fsdb_host.c: open a host file read-only unless it is opened for writing (every open asked for write access) |
| `libretro-uae-hd-unit-activity.patch` | gui.c: per-unit hard-drive read/write activity (`adm_drain_hd_activity`) |
| `libretro-uae-active-filesystems.patch` | filesys.c: which directory units AmigaOS has actually started (`adm_get_active_filesystem_units`) |
| `libretro-uae-kickstart-path.patch` | libretro-core.c: a full-length Kickstart path (`opt_kickstart` was 64 bytes) |

### Debugger, chipset telemetry and disk instrumentation

| Entry | What it does |
| --- | --- |
| `series/0080-drawing-sprite-mask.patch` | drawing.c: bitplane/sprite mask visibility control |
| `series/0090-cpu-state-export.patch` | libretro-core.c: adm_get_cpu_state() |
| `series/0100-disasm-export.patch` | libretro-core.c: adm_disassemble() |
| `series/0110-assembler-export.patch` | libretro-core.c: adm_assemble() |
| `series/0120-display-mask-exports.patch` | libretro-core.c: adm_set_bitplane_mask / adm_set_sprite_mask |
| `series/0130-palette-exports.patch` | libretro-core.c: palette read/write exports |
| `series/0140-blitter-writes-exports.patch` | libretro-core.c: blitter write-log exports |
| `series/0150-redraw-frame-export.patch` | libretro-core.c: adm_redraw_frame() |
| `series/0160-bplpt-ring-buffer-events.patch` | libretro-core.c: bplpt event ring buffer |
| `series/0170-sprpt-events.patch` | libretro-core.c: sprite pointer (sprpt) event capture |
| `series/0180-paula-audio-telemetry.patch` | libretro-core.c: Paula audio telemetry ring |
| `series/0190-audio-paula-tap.patch` | audio.c: Paula per-channel telemetry tap |
| `series/0200-custom-sprpt-capture.patch` | custom.c: SPRxPT write capture |
| `series/0210-custom-bplpt-capture.patch` | custom.c: BPLxPT write capture |
| `series/0220-custom-blit-event-capture.patch` | custom.c: blit start event capture |
| `series/0230-cia-state-export.patch` | cia.c: adm_get_cia_state() |
| `series/0240-adm-get-version.patch` | libretro-core.c: adm_get_version() |
| `series/0250-serial-host-capture.patch` | serial_host.c: capture emulated serial output |
| `series/0260-uaelib-kprintf-capture.patch` | uaelib.c: capture uaelib KPrintF output |
| `series/0270-serial-output-ring.patch` | libretro-core.c: serial/debug output ring buffer |
| `series/0280-sysdeps-debug-capture.patch` | sysdeps.h: route write_log through the ADM capture |
| `series/0290-writelog-debug-capture.patch` | writelog.c: route write_log through the ADM capture |
| `series/0300-adm-debug-command.patch` | libretro-core.c: adm_debug_command() |
| `series/0310-adm-debug-break-arm.patch` | libretro-core.c: adm_get_debug_break_pending / adm_arm_debug |
| `series/0320-oneshot-checkpoint-exports.patch` | libretro-core.c: bounded one-shot checkpoint exports |
| `series/0330-bootblock-break-exports.patch` | libretro-core.c: break-on-bootblock-entry exports |
| `series/0340-expansion-force-boot-rom.patch` | expansion.c: force the UAE boot ROM on (Bartman gate) |
| `series/0350-bartman-resource-registry.patch` | libretro-core.c: Bartman debug resource registry |
| `series/0360-uaelib-bartman-case88.patch` | uaelib.c: Bartman uaelib function 88 |
| `series/0370-uaelib-bartman-gate.patch` | uaelib.c: Bartman JSR gate + diagnostic |
| `series/0380-debug-step-over-nextpc.patch` | debug.c: step-over (z) stale nextpc fix |
| `series/0390-debug-console-out-format-safety.patch` | debug.c: console_out format-string safety |
| `series/0400-debug-arm-trace.patch` | debug.c: breakpoint trace arming + libretro early return |
| `series/0410-debug-bpnode-skip.patch` | debug.c: adm_skip_bp_check in the bpnode loop |
| `series/0420-debug-early-return-trace-mode.patch` | debug.c: early return when trace_mode is 0 |
| `series/0430-blit-events.patch` | libretro-core.c: blit event ring buffer |
| `series/0440-frame-start-cycle-anchor.patch` | libretro-core.c: frame-start cycle anchor |
| `series/0450-frame-start-state-capture.patch` | libretro-core.c: frame-start save-state capture |
| `series/0460-newcpu-instruction-boundary-capture.patch` | newcpu.c: instruction-boundary capture hook |
| `series/0470-disk-dma-savestate-chunk.patch` | savestate.c: disk DMA engine state (ADSK chunk) |
| `series/0480-idle-events.patch` | libretro-core.c: Bartman idle-time profiling events |
| `series/0490-copper-exec-events.patch` | libretro-core.c: copper execution event ring |
| `series/0500-register-write-events.patch` | libretro-core.c: custom register write log |
| `series/0510-cpu-trace-events.patch` | libretro-core.c: CPU trace ring buffer |
| `series/0520-blitter-record-blit-end.patch` | blitter.c: adm_record_blit_end hooks |
| `series/0530-custom-frame-start-cycle.patch` | custom.c: frame-start cycle capture |
| `series/0540-custom-beam-break.patch` | custom.c: break-on-beam check |
| `series/0550-debug-exception-break.patch` | debug.c: break-on-exception hook |
| `series/0560-exception-beam-break-exports.patch` | libretro-core.c: break-on-exception / break-on-beam exports |
| `series/0570-newcpu-cpu-trace.patch` | newcpu.c: CPU trace capture in every m68k_run variant |
| `series/0580-newcpu-bootblock-break.patch` | newcpu.c: break-on-bootblock-entry |
| `series/0590-newcpu-oneshot-checkpoint.patch` | newcpu.c: bounded one-shot checkpoints |
| `series/0600-newcpu-skip-bp-check.patch` | newcpu.c: adm_skip_bp_check (breakpoint re-fire fix) |
| `series/0610-newcpu-m68k-go-no-preloop-debug.patch` | newcpu.c: skip the pre-loop debug() in m68k_go() |
| `series/0620-newcpu-exact-restore.patch` | newcpu.c: exact state restore flag and guard |
| `series/0630-core-exact-restore-unserialize.patch` | libretro-core.c: set the exact-restore flag in retro_unserialize() |
| `series/0640-custom-copper-exec-capture.patch` | custom.c: copper execution event capture |
| `series/0650-adm-get-display-geometry.patch` | libretro-core.c: adm_get_display_geometry() |
| `series/0660-debug-dma-exports.patch` | debug.c: DMA debugger exports |
| `series/0670-disk-write-events.patch` | disk.c: write-event capture and raw track read-back |
| `series/0680-disk-write-event-exports.patch` | libretro-core.c: disk write event exports |
| `series/0690-disk-dma-log-exports.patch` | libretro-core.c: disk DMA transfer log |
| `series/0700-disk-dma-log-hooks.patch` | disk.c: disk DMA transfer logging hooks |
| `series/0710-custom-disk-trigger-capture.patch` | custom.c: disk trigger PC capture |
| `series/0720-makefile-caps-ipf.patch` | Makefile: enable CAPS/IPF support |
| `series/0730-caps-capsimg-path.patch` | caps.c: honour ADM_CAPSIMG_PATH |

### FloppyBridge: real drivers compiled in

| Entry | What it does |
| --- | --- |
| `@vendor-floppybridge` | Fetch FloppyDriveBridge at its pinned commit and vendor the driver sources |
| `libretro-uae-floppybridge-portability.patch` | FloppyBridge driver sources: portability fixes for the GCC/MinGW libretro build. |
| `libretro-uae-floppybridge-latency.patch` | FloppyBridge driver sources: two buffered-path fixes. |
| `libretro-uae-floppybridge-overlap.patch` | FloppyBridge RotationExtractor: a runtime overlap search window. |
| `@install-floppybridge-lib` | Install `patches/floppybridge/` (the static driver factory and Peekaboo's own drivers) |
| `series/0790-disk-floppybridge-linkage.patch` | disk.c: keep C linkage when built as C++ for FloppyBridge |
| `series/0800-zfile-drop-z-stream-decl.patch` | zfile.h: drop the dead z_stream forward declaration |
| `series/0810-makefile-floppybridge-sources.patch` | Makefile.common: add the FloppyBridge sources |
| `series/0820-makefile-floppybridge-flags.patch` | Makefile: enable FLOPPYBRIDGE |
| `series/0830-disk-floppybridge-config.patch` | disk.c: FloppyBridge host configuration |
| `series/0840-core-floppybridge-config-exports.patch` | libretro-core.c: FloppyBridge configuration exports |
| `series/0850-disk-floppybridge-teardown.patch` | disk.c: a dropped bridge closes its port |
| `series/0860-core-deinit-bridge-teardown.patch` | libretro-core.c: retro_deinit() stops live floppy drives |
| `series/0870-disk-write-protect.patch` | disk.c: per-drive write protect |
| `series/0880-core-write-protect-exports.patch` | libretro-core.c: write-protect exports |
| `series/0890-debug-memwatch-capture.patch` | debug.c: memory watchpoint hit capture |
| `series/0900-debug-memwatch-accessor.patch` | debug.c: memory watchpoint hit accessor |
| `series/0910-debug-memwatch-accessor-ext.patch` | debug.c: extended memory watchpoint hit accessor |
| `series/0920-thread-sem-init-fd-leak.patch` | thread.c: uae_sem_init fd-leak fix (macOS named semaphores) |
| `series/0930-thread-sem-destroy-zero.patch` | thread.h: uae_sem_destroy zeroes sem->sem after close |
| `series/0940-commpipe-init-fd-leak.patch` | commpipe.h: init_comm_pipe destroys sems before memset |
| `series/0950-thread-sem-unpost-drain.patch` | thread.h: uae_sem_unpost drains instead of dropping the handle |

### Blitter destination provenance (blitter-vis)

| Entry | What it does |
| --- | --- |
| `@append-blitvis` | Append `patches/adm_blitvis.c` to `libretro-core.c` |
| `series/0970-blitvis-hooks-blitter.patch` | blitter.c: blitter-vis write hooks |
| `series/0980-blitvis-hooks-custom.patch` | custom.c: blitter-vis fetch hooks |
| `series/0990-blitvis-hooks-drawing.patch` | drawing.c: blitter-vis span projection |

### Chipset debug toggles that #include a shared .inc

| Entry | What it does |
| --- | --- |
| `@install-adm-includes` | Install the shared `.inc` files the chipset debug toggles include |
| `series/1010-palette-vis-hooks-drawing.patch` | drawing.c: palette-vis hooks |
| `series/1020-bplcon1-scroll-hooks-custom.patch` | custom.c: bplcon1 scroll hooks |
| `series/1040-chipset-blocks-hooks-custom.patch` | custom.c: chipset write-block hooks |
| `series/1050-blitter-writes-hook-blitter.patch` | blitter.c: blitter write-log hook |
| `series/1060-register-log-hook-custom.patch` | custom.c: custom register write-log hook |
| `series/1080-memory-protect-hooks-memory.patch` | memory.c: memory protection (trainer lock) hooks |
| `series/1090-memory-protect-hook-custom.patch` | custom.c: memory protection copper hook |

### SuperCard Pro (.scp) flux images

| Entry | What it does |
| --- | --- |
| `series/1100-disk-crc32-memoise.patch` | disk.c: memoise the disk-image CRC32 |
| `series/1110-scp-header.patch` | scp.h: SuperCard Pro flux image header |
| `series/1120-scp-sysconfig.patch` | sysconfig.h: enable SCP support |
| `series/1130-makefile-scp-source.patch` | Makefile.common: add scp.c to SOURCES_C |
| `series/1140-libretro-dc-scp-extension.patch` | libretro-dc.c: classify .scp as floppy media |
| `series/1150-core-scp-valid-extension.patch` | libretro-core.c: add scp to valid_extensions |

### CPU trace fidelity

| Entry | What it does |
| --- | --- |
| `series/1160-cpu-trace-stop-spin.patch` | newcpu.c: do not trace the STOP spin |
| `series/1170-cpu-trace-run-loops.patch` | newcpu.c: trace the remaining non-JIT run loops |

### Host-generated media boots (WHDLoad, CD)

| Entry | What it does |
| --- | --- |
| `series/1180-peekaboo-media-exports.patch` | libretro-core.c: WHDLoad, CD and archive exports for host-generated boots |

### Machine configuration below the model preset

| Entry | What it does |
| --- | --- |
| `series/1190-chipset-revision-options.patch` | libretro-core.c: chipset revision options below the model preset |
| `series/1200-mmu-model-option.patch` | libretro-core.c: MMU model option |

### Host serial and parallel ports

| Entry | What it does |
| --- | --- |
| `series/1210-serial-parallel-host-backend.patch` | Host serial and parallel ports: TCP, host serial devices, a parallel capture file |
| `series/1220-midi-host-devices.patch` | MIDI in and out through host devices (Windows) |

### Networking

| Entry | What it does |
| --- | --- |
| `series/1230-bsdsocket-library.patch` | bsdsocket.library through the host's TCP/IP stack (POSIX and Winsock) |
| `series/1240-a2065-uaenet-slirp.patch` | A2065 Ethernet and uaenet.device on WinUAE's built-in SLIRP (Windows) |

### Sound cards

| Entry | What it does |
| --- | --- |
| `series/1250-sound-cards.patch` | Expansion sound cards: Toccata, Prelude, Prelude 1200 and UAESND |

### Robustness

| Entry | What it does |
| --- | --- |
| `series/1260-kickstart-cpu-mismatch.patch` | A Kickstart the CPU cannot run no longer kills the host process |
| `series/1270-serial-poll-batching-midi-sysex-pool.patch` | Serial port: poll the host once every 16 idle asks and send in frame-sized writes; MIDI SysEx |

### ROM identification

| Entry | What it does |
| --- | --- |
| `series/1280-adm-identify-rom.patch` | Kickstart ROM identification by content, exported to the host |
| `series/1290-kickstart-ext-option.patch` | An extended ROM slot the host can set: puae_kickstart_ext |

### RTG / Picasso96 (uaegfx)

| Entry | What it does |
| --- | --- |
| `series/1300-picasso96-enable.patch` | Turn Picasso96 (uaegfx) RTG on |
| `@vendor-picasso96` | Fetch WinUAE's Picasso96 module at its pinned commit |
| `series/1310-picasso96-winuae-port.patch` | Port WinUAE 5.3.1's Picasso96 module onto this tree |
| `series/1320-misc-rtg-display-list.patch` | The RTG display list: struct ScreenResolution, and one Displays[] |
| `series/1330-gfxboard-uaegfx-only.patch` | uaegfx only: keep gfxboard.c's hardware boards out of the build |
| `series/1340-mman-rtg-write-watch.patch` | RTG write-watch, and a Zorro III ceiling that was zero |
| `@install-picasso-host` | Install `patches/picasso96/`, the libretro half of the RTG platform layer |
| `series/1350-libretro-rtg.patch` | The libretro front end's RTG path: options, geometry and adm_get_rtg_state |

### Video geometry

| Entry | What it does |
| --- | --- |
| `series/1360-horizontal-crop-clamp.patch` | Horizontal centring can no longer push the crop window past the end of the row |
| `series/1370-dma-record-hdiw-units.patch` | DMA record: horizontal display-window events land at the clock they happen |
| `series/1380-cpu-ext-state-export.patch` | newcpu.c: adm_get_cpu_ext_state() - every register beyond D/A/PC/SR |

### Finishing steps

| Entry | What it does |
| --- | --- |
| `@install-link-t` | Install `patches/link.T`, the linker version script that exports every `adm_*` symbol |
| `@set-version` | Stamp the core version string `adm_get_version()` reports |

## RTG / Picasso96 (`uaegfx`)

`sources/src/picasso96.c` as libretro-uae ships it is the **UAE 0.8.x** module, while every
file around it — `gfxboard.c`, `expansion.c`, `drawing.c`, `custom.c` — is WinUAE 5.3.1, which
is what libretro-uae's own `README.md` says the core is based on. They do not meet: the old
module has a different memory bank, a scalar `picasso96_state` where the others index an array,
and a one-argument `gfx_set_picasso_state()` where `drawing.c` calls the two-argument one.
Defining `PICASSO96` over it does not compile, which is very likely why it never was.

`@vendor-picasso96` therefore fetches the module the rest of the tree was written against, from
WinUAE at the commit the README pins:

| WinUAE `51b7be3` | lands as |
| --- | --- |
| `od-win32/picasso96_win.cpp` | `sources/src/picasso96.c` |
| `od-win32/picasso96_win.h` | `sources/src/include/picasso96.h` |
| `p96_blit.cpp` | `sources/src/p96_blit.inc` |

`series/1310-picasso96-winuae-port.patch` adapts them — od-win32 headers, the Windows mouse
pointer, Windows types, the `natmem_offset` idiom — and its preamble lists every category. The
RTG logic itself is untouched. `@install-picasso-host` then installs `patches/picasso96/`, which
is the libretro half of the platform layer (what WinUAE keeps in `win32gfx.cpp` and
`direct3d.cpp`), written here.

The copy is skipped once `sources/src/p96_blit.inc` exists: a re-run must not put the unpatched
originals back under a patch the stamp file already records as applied.

The hardware boards in `gfxboard.c` stay out — its guard is now
`PICASSO96 && WITH_GFXBOARD_HARDWARE` and nothing defines the second.

## Blitter destination provenance ("blitter-vis")

Adapted from the **ENGINE9000** debugger by alpine9000
(<https://github.com/alpine9000/engine9000-public>), commit `0d8db93`, files
`ami9000/sources/src/{blitter,custom,drawing}.c` and `e9k-debugger/amiga_blittervis.c`.
Copyright (C) alpine9000, GPL-2 — the same licence this project is distributed under
(see [COPYING](COPYING)).

### Why

Locating a blit on screen by reconstructing it from `bltdpt` / `bltdmod` / `bplpt` geometry (what
`src/core/BlitScreenMapper.h` does) has to guess the target plane and the buffer base, and knows
nothing about `BPLxMOD`, fetch mode or x-scroll delay. Each guess is a chance to put the box in the
wrong place, which is why the emulator-window overlay was unreliable on scrolling playfields.

This sidesteps the geometry problem rather than solving it:

1. **Write time** (`blitter.c`) — every word the blitter writes stamps a slot in a table indexed
   purely by address, `(addr & chipmem_bank.mask) >> 1`. No row/column/stride concept exists at
   this layer, so modulos and offscreen buffers are not its problem.
2. **Fetch time** (`custom.c`) — the real Agnus bitplane fetch asks "did the blitter last write
   this word?" and tags the pixel coordinates the fetch itself just computed. Those are already
   correct for `BPLxMOD`, fetch mode, scroll delay and plane number, because they *are* the
   emulator's display logic.
3. **Projection** (`drawing.c`) — those source coordinates are projected onto native output
   coordinates during the normal line-to-screen decode, post-crop and post-scale, and emitted as
   `(x, y, xEnd, blitId)` spans.

**Known limit, inherent to the technique:** a blit whose destination is never pointed at by an
active `BPLxPT` produces no spans, because nothing ever fetches those words. This is the provenance
of what is on screen, not a general offscreen-buffer viewer. The geometry-based *Blit BBoxes*
overlay is retained for that case.

### How it is applied

The implementation lives in [`patches/adm_blitvis.c`](patches/adm_blitvis.c) and is appended
verbatim to `libretro/libretro-core.c`. Only twelve small call hooks are patched into `blitter.c`,
`custom.c` and `drawing.c`; each carries exact context, so the build **fails loudly** if an anchor
moved upstream rather than silently producing a core with half the hooks in it.

The twelve hooks are three patches in `patches/series/` (`0970`-`0990`, one per file), applied in
series order. `tests/AdmBlitVisTest.cpp` compiles `adm_blitvis.c` directly and tests the algorithm
without a core rebuild, so a failure there means the algorithm is wrong, while a
correct-tests-but-wrong-picture result points at the hook sites instead.

Exports added (also listed in `patches/link.T`):
`adm_set_blitter_vis_enabled`, `adm_get_blitter_vis_spans`, `adm_get_blitter_vis_stats`.

`adm_blit_event_t` also gained a `blitId` field so spans join onto the blit records the profiler
already collects. That is an ABI change, so `adm_get_blitter_vis_spans` is in
`kRequiredCoreSymbols` — an older DLL is rejected at load rather than read at the wrong stride.

### Blitter destination data ("what the blit actually wrote")

Provenance answers *where* a blit landed. This answers *what it drew*: the same
`blit_chipmem_agnus_wput` choke point also copies each written word, with its address, into a
4 MB ring, and `adm_blit_event_t` carries a `(data_off, data_words)` window into it. The host
drains a frame's windows in `BlitProfiler::Update()`, before the next frame's blits recycle them,
and `src/core/BlitDestImage.h` rebuilds a one-bitplane bitmap for the Blitter Operations window's
*Data* column and the Frame Scrubber's blit tooltip.

Storing the **address** per word rather than relying on write order is what makes descending
blits, fill mode and line mode reconstruct without re-deriving their geometry — a word's position
is where it was written, not where the count says it should be. Pixels no write touched (the
modulo gap, a skipped row) stay marked untouched rather than reading as background.

The ring drops the oldest data rather than stalling: a window the core has already recycled comes
back empty, and a partial one is reported as truncated. Capture is off by default and armed from
the *Data* checkbox in the Blitter Operations window; the arena is only allocated while it is on.

Exports added (also listed in `patches/link.T`): `adm_set_blit_data_enabled`,
`adm_get_blit_data_enabled`, `adm_get_blit_data`. This is another `adm_blit_event_t` ABI change,
so `adm_get_blit_data` is in `kRequiredCoreSymbols` for the same reason as above.

The `blitter.c` write hook is shared with both blitter-vis and the kill switch below, so all
three anchors move together — change one and the other two must be updated with it.

## Copper / DMA raster overlay ("beam view")

Adapted from the **ENGINE9000** debugger by alpine9000
(<https://github.com/alpine9000/engine9000-public>), files
`e9k-debugger/emu_ami.c` (`emu_ami_copperColorForRecord`, `emu_ami_copperColorForMoveOffset`,
`emu_ami_getCopperSlotTextureRect`, `emu_ami_dmaDebugNormalizeLineDhpos`, `emu_ami_adjustVideoDst`,
`emu_ami_getDmaDebugOverlayDst`, `emu_ami_buildCopperTooltip`) and
`ami9000/libretro/e9k_debug.c` (`e9k_debug_amiga_dma_debug_get_frame_view`).
Copyright (C) alpine9000, GPL-2 — the same licence this project is distributed under
(see [COPYING](COPYING)).

### Why

The Copper window tells you *what* the Copper did and the Frame Scrubber tells you *when* in the
frame. Neither shows *where on the raster*, which is the dimension that actually explains an Amiga
effect: a colour split is a horizontal line, a stalled `WAIT` is a horizontal bar, and a Copper that
misses its slot has the culprit sitting in the neighbouring colour clock.

### What was taken

Two ideas, both alpine9000's:

1. **The adjusted window.** The emulator picture is shrunk and inset inside a larger rect standing
   for the whole beam, so the offscreen colour clocks — roughly 48 of the 227 per line and 26 of the
   313 lines, none of which exist in the framebuffer — get real estate to be drawn in.
2. **Classifying a Copper instruction by its destination register** for colour: WAIT/SKIP, and MOVEs
   to sprite / blitter / bitplane-pointer / colour / other registers. ENGINE9000 draws WAIT and SKIP
   in one red; they are split here because the trace distinguishes them at no cost and mistaking a
   skipped branch for a stalled wait is the confusion the view exists to remove.

### What was NOT taken

The DMA channel palette. ENGINE9000 uses WinUAE's `set_debug_colors()` values; this app already had
its own in `src/ui/DmaColors.h`, shared by `DmaStatsView` and the Frame Scrubber's DMA row, and the
raster reuses that so all three views agree. No palette is copied from WinUAE.

### Where it lives

- [`src/core/RasterFrameMap.h`](src/core/RasterFrameMap.h) — the beam ↔ picture ↔ screen geometry.
- [`src/core/CopperRaster.{h,cpp}`](src/core/CopperRaster.h) — pure, unit-tested rasteriser:
  instruction classification, WAIT stall runs, the `evt`-flag annotations, channel filtering.
- [`src/ui/views/CopperRasterOverlay.{h,cpp}`](src/ui/views/CopperRasterOverlay.h) — the GL/ImGui
  side: texture, markers, hit-testing.
- Controls in Debugger → Display → *Raster (Beam View)*; settings in `src/ui/RasterOverlaySettings.h`.

**No PuAE rebuild was needed.** PuAE already carries WinUAE's full visual-DMA-debugger record
(`struct dma_rec`), already exported as `adm_get_dma_record` and already mirrored host-side as
`AmigaEmulator::DmaSlotRecord`. Beyond the channel type it holds `DMA_EVENT_COPPERWAKE`,
`COPPERWANTED`, `COPPERSKIP`, `DDFSTRT`/`DDFSTOP`, `HDIWS`/`HDIWE`, `HBS`/`HBE`, `HSS`/`HSE` and
`VB`/`VS` — every one of which was declared but never read anywhere in `src/` before this feature.

**One trap worth recording:** the record buffer is `NR_DMA_REC_VPOS = 1000` rows tall, which is
nothing like a frame (PAL is 313 lines, NTSC 263). Taking `vposCount` at face value stretches the
raster to three times the real frame and squashes the picture into a corner of it. `DeriveFrameHeight`
recovers the true height from the record — refresh DMA runs on every scanline including blanking, so
the highest row carrying activity is a stable proxy for `maxvpos`. `CopperRasterTest` guards this.

## Palette visualiser and BPLCON1 scroll override ("palette-vis")

Adapted from the **ENGINE9000** debugger by alpine9000
(<https://github.com/alpine9000/engine9000-public>), files
`ami9000/sources/src/{drawing,custom}.c` — specifically `drawing_applyPaletteVisToScanline` /
`drawing_paletteVisEncodeDualPfColor` and the `compute_toscr_delay` override behind
`custom_setBplcon1DelayScrollEnabled`.
Copyright (C) alpine9000, GPL-2 — the same licence this project is distributed under
(see [COPYING](COPYING)).

### Why

Two questions the existing windows could not answer.

*What is the copper actually doing to the palette?* The Copper window lists the writes and the
raster overlay shows when they land, but neither shows the resulting colours. The **palette
visualiser** replaces every scanline's decoded pen data with a left-to-right ramp through all
`2^BPU` pens. Because the substitution happens after `adjust_drawing_colors()` has latched the
line's colour table but before colour lookup, the picture becomes a per-scanline palette chart:
X is pen index, Y is scanline, and every colour split, fade or raster bar shows up as a change
down the bars. Sprites still composite on top.

*Where is the bitplane data really being fetched?* **BPLCON1 scroll**, when switched off, forces
the BPLCON1 delay and sub-pixel scroll fields to zero, so the shifter stops applying fine
horizontal scroll and the raw fetch alignment — and any per-playfield scrolling — becomes obvious.

### How it is applied

The palette visualiser implementation lives in
[`patches/adm_palette_vis.inc`](patches/adm_palette_vis.inc), copied into `sources/src/` and
`#include`d by `drawing.c`. It is an `.inc` rather than a `.c` because it reads `drawing.c` file
statics (`pixdata`, `src_pixel`, `playfield_start`/`playfield_end`, `bplplanecnt`, `bpldualpf`),
so it cannot be its own translation unit; keeping it in a file rather than inside the build
scripts keeps the C reviewable and byte-identical on both platforms.

Five exact-string hooks patch `drawing.c` and `custom.c` (two and three respectively). Each
asserts an expected match count and **fails the build loudly** if its anchor moved upstream. Note
the `custom.c/bplcon1_shdelay` anchor deliberately includes the preceding `toscr_delay_shifter[1]`
assignment: the bare `shdelay1`/`shdelay2` lines appear twice, and the second copy is inside an
`#if 0` block.

The hooks live in `patches/series/`, applied in series order.

Exports added (also listed in `patches/link.T`):
`adm_set_palette_vis`, `adm_set_bplcon1_scroll`.

`adm_set_bplcon1_scroll` is defined in `custom.c` rather than appended to `libretro-core.c` like
every other `adm_*` setter, because it has to set `bplcon1_written` — a file static — to force a
recompute; without that nothing changes until the program next writes BPLCON1 itself. It takes an
`int` rather than a `uint8_t` for the same reason: `uint8_t` is not in scope there.

Both exports are optional, not in `kRequiredCoreSymbols`: an older DLL resolves them to null and
`AmigaEmulator::HasPaletteVis()` / `HasBplcon1Scroll()` grey the controls out, which is more useful
than rejecting the core over two debug toggles.

**Worth knowing when using them:** the palette ramp is applied in the *drawing* stage, so
`adm_redraw_frame` makes a change visible immediately, even while paused (and it lands in MCP
screenshots). The BPLCON1 override is applied during bitplane *fetch*, so it only takes effect once
the emulator runs another frame.

## Copper and bitplane-pointer write blocks ("chipset-blocks")

Adapted from the **ENGINE9000** debugger by alpine9000
(<https://github.com/alpine9000/engine9000-public>), file
`ami9000/sources/src/custom.c` — specifically `custom_isCopperLineLimited`,
`custom_isCopperBitplanePointerWriteBlocked` and their setters, surfaced there as the
*Copper Block* and *Bitplane Ptr Block* controls.
Copyright (C) alpine9000, GPL-2 — the same licence this project is distributed under
(see [COPYING](COPYING)).

### Why

The Copper window lists what a copper list writes and the raster overlay shows when each MOVE
lands, but neither answers "what would the picture look like without this". These two do, by
bisection:

- **Copper Block** drops every register write the copper makes on a band of scanlines. Sweep the
  band down the screen and whatever stops changing inside it is what the copper was driving there.
- **Bitplane Ptr Block** drops copper writes to selected `BPLxPT` pointers on a band, pinning them
  at whatever the last permitted write left. Mid-screen re-pointing is how split screens, hardware
  scrollers and copper-driven buffer swaps work, so the region a pinned plane feeds visibly
  repeats — which identifies which MOVE drives which plane, and where.

Both are **copper-only**: a CPU write to the same register still lands, so what disappears is
exactly the copper's contribution. Neither changes timing — the copper program still runs, WAIT
and SKIP still execute, copper DMA cycles are still consumed and the copper PC still advances.
Only the effect of the write is dropped, which is what makes them a bisection tool rather than a
perturbation of the thing being measured.

### How it is applied

The implementation lives in [`patches/adm_chipset_blocks.inc`](patches/adm_chipset_blocks.inc),
copied into `sources/src/` and `#include`d by `custom.c` immediately after
`static int copper_access;`. It is an `.inc` rather than a `.c` because it reads `custom.c` file
statics (`copper_access`, `vpos`), so it cannot be its own translation unit — the same arrangement
as `adm_palette_vis.inc`.

Three exact-string hooks, each asserting an expected match count and failing the build loudly if
its anchor moved upstream: the include, an early return at the top of `custom_wput_copper` (placed
ahead of `debug_putpeekdma_chipset`, so a blocked write is invisible to DMA debug and memory
watchpoints too), and one replacement covering all twelve contiguous `BPLxPT` cases `$0E0`–`$0F6`
in the register dispatch.

The hooks live in `patches/series/`, applied in series order.

Exports added (also listed in `patches/link.T`):
`adm_set_copper_block`, `adm_set_bplptr_block`. Both are defined in `custom.c` next to the state
they gate rather than appended to `libretro-core.c`, and both take plain `int`s for the same
reason `adm_set_bplcon1_scroll` does. Both are optional, not in `kRequiredCoreSymbols`; an older
DLL resolves them to null and `AmigaEmulator::HasChipsetBlocks()` greys the section out.

Only planes 1-6 have blockable pointers — `BPL7PT`/`BPL8PT` are AGA-only and outside the hooked
range — so the host-side mask (`core/DisplayBlocks.h`) is six bits wide.

**Worth knowing when using them:** both act at register-write time, so a change only takes effect
once the emulator runs another frame — unlike the palette visualiser, re-rendering a paused frame
shows nothing new.

### Blitter kill switch

The same part of the series also adds a third, simpler suppression: `adm_set_blitter_writes`
gates the blitter's destination (D channel) write in `blit_chipmem_agnus_wput` — the single choke
point every blit path funnels through (`blitter_dofast`, the descending variant, fill, line mode and
the cycle-exact path; it is the same function the blitter-vis provenance hook uses, which is why
both anchors below match the *post*-blitter-vis text and this hook must stay after that block).

Off means nothing the blitter draws reaches chip RAM, while the blit itself still runs: BLTSIZE
still starts it, its DMA cycles are still consumed, and BUSY and the completion interrupt still
behave. Whatever stops updating on screen is what the blitter was drawing. The provenance tag is
skipped along with the write, and `regs.chipset_latch_rw` is still updated, both matching upstream.

Two hooks in `blitter.c`: the `int adm_blitter_writes_enabled = 1;` definition (kept next to the
choke point it gates rather than in the `.inc`, since it needs nothing from `custom.c`) and the
guard itself. The setter is appended to `libretro-core.c` in the usual style, reaching the flag
through an `extern` inside the function body.

Export added (also listed in `patches/link.T`): `adm_set_blitter_writes`.

Adapted from ENGINE9000's *Blitter* checkbox — `blitter_setDestinationWriteEnabled` in
`ami9000/sources/src/blitter.c`, there gated behind its `E9K_HACK_BLITTER_CONTROL` build flag.

## Custom-register write log ("Register Operations")

Adapted from the **ENGINE9000** debugger by alpine9000
(<https://github.com/alpine9000/engine9000-public>), files
`e9k-debugger/amiga_custom_log.{c,h}` — there the *Chipset Log*, with its
LINE / NAME / SRC / ADDR / VALUE / DESC columns and per-column substring filters.
Copyright (C) alpine9000, GPL-2 — the same licence this project is distributed under
(see [COPYING](COPYING)).

### Why

Three windows each answer part of "what is the chipset doing" and none answers this part.
The Copper window lists the *instructions* a copper list executes, not their effect on a
register over the frame. The Custom Registers window shows a register's value *now*, with no
history and no attribution. The Blitter Operations window does exactly this job, but only for
the blitter. Nothing said **who wrote this register, and when** — which is the question behind
a colour split, a mid-screen BPLCON change, and any register the CPU and copper are both
driving.

### What was taken, and what was added

Taken: the idea of a per-frame log of custom-register writes attributed to CPU or Copper, and
its column set — line, register name, source, address, value, description.

Added: **time**. ENGINE9000 records `LINE` (vpos) only and its rows sync nothing. This app
already has a frame-wide L-Ref timeline (`vpos * 228 + hpos`) that the CPU trace, Copper trace,
Frame Scrubber, disassembly and beam crosshair all follow, so each row carries its L-Ref and
selecting one drives `DebuggerView::JumpToLRef` — moving every one of those views to the instant
of the write. The jump is then made source-aware from the row's own `fromCopper` flag: a copper
write additionally selects the MOVE that made it in the Copper trace
(`JumpToCopperTrace`), a CPU write points the disassembly at the writing instruction.

Also added: a `Was` column (the value the register held before this write, or `first`/`same`),
which is what separates the write that changed something from the hundreds that re-wrote the
same value; and a "changed only" filter built on it.

### How it is applied

`custom_wput_1` in `custom.c` is the single chokepoint **every** custom-register write funnels
through — CPU writes arrive via `custom_wput`/`custom_bput`/`custom_lput`, copper writes via
`custom_wput_copper`, which sets `copper_access` around the call. One hook therefore captures
both sources already discriminated, with `hpos` in scope as a parameter, `vpos` as a file
static and `get_cycles()/512` for the absolute CCK.

Two exact-string hooks, each asserting an expected match count and **failing the build loudly**
if its anchor moved upstream: the `extern` declaration ahead of the function, and the record
call placed immediately after the two `custom_storage[addr >> 1]` assignments, where `addr` and
`value` have already been masked.

For the PC, the copper case records `cop_state.ip - 4` — the address of the MOVE itself, which
is what `adm_record_copper_exec` stores, so the two logs join on address. The CPU case records
`regs.instruction_pc` rather than `M68K_GETPC`, because the latter has already advanced past
the writer on a multi-word move (the same reasoning as the DSKLEN trigger hook).

The hooks live in `patches/series/`, applied in series order.

Exports added (also listed in `patches/link.T`):
`adm_get_reg_write_events`, `adm_clear_reg_write_events`. Capture is always on, into a
16384-entry ring — the same size and always-on arrangement as the copper exec events, and a
typical frame runs to one to three thousand writes.

Both exports are **optional**, not in `kRequiredCoreSymbols`: an older DLL resolves them to null,
`AmigaEmulator::HasRegWriteLog()` reports false and the window says the core needs rebuilding,
which is more useful than rejecting the core outright over one debug view.

**Known limit, inherent to the hook site:** this sees CPU and Copper writes only. Chipset
activity that bypasses the register-write path — sprite DMA loading SPRxPOS/CTL/DATA, bitplane
pointer auto-increment — never reaches `custom_wput_1` and so never appears. ENGINE9000 has the
same boundary (its SRC column is only CPU/COP); the Sprites and Bitplanes windows cover that side.

### Where it lives

- [`src/core/RegWriteProfiler.{h,cpp}`](src/core/RegWriteProfiler.h) — pure, unit-tested
  enrichment: L-Ref, per-register previous value, `changed`/`firstWrite`, frame stats. It takes
  an already-fetched event buffer rather than an `EmulatorView`, which is what keeps it free of
  any emulator/UI dependency and testable (`tests/RegWriteProfilerTest.cpp`).
- [`src/ui/views/RegisterOpsView.{h,cpp}`](src/ui/views/RegisterOpsView.h) — the window.
- [`src/core/RegLogSnapshot.h`](src/core/RegLogSnapshot.h) + `Application::MakeRegLogSnapshot`
  — the self-contained POD mirror behind the AR console `REGLOG` command and the MCP
  `get_register_log` tool.

## Memory write protection ("trainer locks")

Command grammar mirrors the **ENGINE9000** debugger by alpine9000
(<https://github.com/alpine9000/engine9000-public>) and its `protect <addr> block|set=0x...`
console command. The implementation here is our own; ENGINE9000 publishes no file format.

### Why

Peekaboo could already *find* an address (`TS`/`T`/`TF`, the AR-style trainer) and *watch* one
(`MS` watchpoints), but nothing could hold a value there, so a found address could never become
a live cheat. This closes that: a lock either drops guest writes to a range, or forces a value
into them.

The choice worth recording is **where** it holds. A per-frame host-side clamp would rewrite the
value 50 times a second, which loses to anything that reads the variable between frames, and
shows up as a visible flicker on a HUD. Doing it inside the core's write path instead means the
guest never observes the unprotected value at all — and "force a value" falls out of the same
hook for free, which is why there is no separate set-and-hold code anywhere.

### How it is applied

The implementation lives in [`patches/adm_protect.inc`](patches/adm_protect.inc), copied into
`sources/src/` and `#include`d by `memory.c` immediately before `memory_put_long()`, so the
table is defined before the three functions that consult it. Unlike `adm_palette_vis.inc` and
`adm_chipset_blocks.inc` it needs no file statics from its host translation unit; it is an
`.inc` for the same practical reason they are — the C stays reviewable in one place and
byte-identical on both platforms rather than being spelled out twice inside the build scripts.

**Four** exact-string hooks. Three in `memory.c`, one inserted line each, at
`memory_put_long`/`_word`/`_byte`:

```c
if (adm_protect_count && adm_protect_check(addr, N, &v)) return;   /* N = 4 / 2 / 1 */
```

Each asserts an expected match count and fails the build loudly if its anchor moved upstream.
The `adm_protect_count &&` short-circuit is what keeps the cost of the feature to one
predictable not-taken branch when nothing is protected.

**Why three hooks cover every CPU write.** Every CPU write path in UAE funnels through
`memory_put_{long,word,byte}`: `put_byte`/`put_word`/`put_long` call them directly, so do the
`_compatible` and `_jit` variants, and the cycle-exact 68000 path
(`put_*_ce000` -> `mem_access_delay_*_write` at `newcpu.c` -> `wait_cpu_cycle_write` at
`custom.c`) ends up calling them too. This is the opposite situation to the CPU-loop hooks
above, where fourteen separate loops each need their own anchor.

### The copper needs its own hook

The fourth hook is in `custom.c`, at the top of `custom_wput_copper`:

```c
{
	extern int adm_protect_count;
	extern int adm_protect_check(uaecptr addr, int width, uae_u32 *v);
	if (adm_protect_count && adm_protect_check(0xdff000 + addr, 2, &value)) {
		return 0;
	}
}
```

**Why it is needed, and why this was not obvious.** The copper does not write registers through
the memory map. `custom_wput_copper` calls `custom_wput_1` directly, so a copper MOVE never goes
anywhere near `memory_put_word`. Without this hook a lock on a `$DFFxxx` register catches CPU
writes only — which is no use at all for the registers a copper list actually drives: BPLCON1,
the colour registers, the bitplane pointers. That is exactly how the gap was found, by locking
BPLCON1 and watching the copper carry on updating it while `PR` cheerfully listed the lock as
`on` and `resolved`.

`extern` rather than a header because `adm_protect.inc` is `#include`d by `memory.c`, not by
`custom.c`; the same technique the blitter kill switch uses to reach `adm_blitter_writes_enabled`.

Placement matches the copper-block hook immediately above it — ahead of
`debug_putpeekdma_chipset`, so a blocked write is invisible to DMA debug and memory watchpoints
too. And like the chipset blocks, **it changes no timing**: the copper program still runs, WAIT
and SKIP still execute, copper DMA cycles are still consumed and the copper PC still advances.
Only the effect of the write is caught.

**Ordering trap.** The anchor matches the text *after* the chipset-blocks patch has inserted its
own early return at the top of `custom_wput_copper`, so this hook must stay after that patch in
`series.txt` — the same constraint the blitter kill switch has with the blitter-vis hook.

There is deliberately **no** hook in `custom_wput_1` itself. That would double-apply: a CPU write
to `$DFFxxx` already passes `memory_put_word` on its way to `custom_wput`, so it would be spliced
twice. `custom_wput_1`'s other callers are DMA data paths (bitplane fetch writing `BPLxDAT`, the
sprite/bitplane conflict case), which are not writes a user means to protect.

The hooks live in `patches/series/`, applied in series order.

Exports added (also listed in `patches/link.T`): `adm_protect_set`,
`adm_protect_clear`, `adm_protect_count`, and `cia_bank`.

`cia_bank` is an existing upstream symbol (`cia.c`, declared in `include/memory.h`) rather than
anything new; it is exported so the host can reach `cia_bank.bput` and write a CIA register at
all. Before that, a CIA address failed in `ArMemoryAccess::Write` as `UNMAPPED ADDRESS`, which
is why `W`/`PR` on `$BFExxx` could not work. `AmigaEmulator` static_asserts the `addrbank`
field order it depends on (`bput` at +40).

All three `adm_protect_*` exports are optional, not in `kRequiredCoreSymbols`: an older DLL
resolves them to null, `AmigaEmulator::SupportsProtections()` returns false, and the Trainer
window and `PR` both say so rather than letting the user believe a lock is live when nothing
enforces it.

**KNOWN LIMIT — DMA writes are not blocked.** The blitter's D channel and disk DMA reach chip
RAM through their own paths, which pass through neither hook. This is irrelevant for game
variables (CPU-written) and for chipset registers (CPU or copper), so what it actually costs is
protecting a *buffer* the blitter draws into. If that is ever wanted, `blit_chipmem_agnus_wput`
is the single choke point every blit path funnels through — it is already hooked twice, by the
blitter kill switch and the blitter-vis provenance tag. It is stated in the console `HELP` text
so the limit is discoverable without reading this file.

**The byte-lane splice is the part to be careful with** if `adm_protect_check` is ever touched.
Values are big-endian: for a `width`-byte access at `addr`, the byte at address `b` sits at bit
`(width - 1 - (b - addr)) * 8`. A `Set` entry rewrites only its own bytes inside a wider write,
so a longword store that straddles a forced byte still lands with the forced byte surviving
inside it. Every lane is pinned by `AdmProtectTest`, which compiles the `.inc` directly (same
arrangement as `AdmBlitVisTest`) — so this is testable without a core rebuild.

## CPU-loop hooks and CPU-model coverage

Three features hang off a single injected token in `sources/src/newcpu.c`:

```c
adm_cpu_trace_pre(r);   /* CPU trace                */
/* + adm_fb_check()        bootblock trap           */
/* + adm_oneshot_check()   one-shot PC breakpoint   */
```

`series/0570` injects `adm_cpu_trace_pre(r);` into CPU interpreter loops, and `series/0580` and
`0590` attach the bootblock and one-shot checks beside **every** occurrence of that token that
exists at that point in the series. `series/1170` later added the trace hook alone to three more
loops, so those trace but do not carry the other two checks.

### Why this needs saying

UAE does not have one CPU loop — it has fourteen, and picks between them by CPU model and by the
cycle-exact / compatible settings. The selection is a single chained ternary in `m68k_go()` (`newcpu.c`, search `run_func =`), and
reading it is the only reliable way to know which loop a machine uses — the mapping is not
simply "one loop per CPU". In evaluation order:

| condition | loop | status |
|---|---|---|
| `cpu_cycle_exact && model <= 68010` | `m68k_run_1_ce` | **hooked** |
| `cpu_compatible && model <= 68010` | `m68k_run_1` | **hooked** |
| `model >= 68020 && cachesize` (JIT) | `m68k_run_jit` | *not hooked* (a JIT block has no per-instruction dispatch) |
| `68030/040/060 && mmu_model` | `m68k_run_mmu030/040/060` | *not hooked* (an MMU fault restarts an instruction, so a hook would record it twice) |
| `model >= 68040 && cycle_exact` | `m68k_run_3ce` | trace only (`1170`) |
| `model >= 68020 && cycle_exact` | `m68k_run_2ce` | **hooked** |
| `model <= 68020 && cpu_compatible` | `m68k_run_2p` | **hooked** |
| `68030 && cpu_compatible` | `m68k_run_2p` | **hooked** (same loop) |
| `model >= 68040 && cpu_compatible` | `m68k_run_3p` | trace only (`1170`) |
| `model < 68020` (fallback) | `m68k_run_2_000` | trace only (`1170`) |
| otherwise | `m68k_run_2_020` | **hooked** |

`m68k_run_2` is an empty stub upstream.

**Note the ordering trap.** `m68k_run_2_020` is the *last* branch — it only runs when the machine
is neither cycle-exact, nor compatible, nor JIT. Hooking it alone looks like "68020 support" and
is not: an A1200 under Peekaboo's documented CPU setting ("Approximate A500/A1200 or Cycle
Exact") lands on `m68k_run_2ce` or `m68k_run_2p` instead. That mistake was made and caught here,
by the runtime diagnosis below.

So an A1200 (68EC020) is covered in every non-JIT configuration. A 68040/060 and a 68000 on
"Fastest possible" (`m68k_run_2_000`) have the CPU trace but not the bootblock or one-shot
checks, and an MMU configuration has neither.

On an unhooked loop the three features **silently do nothing**: the code compiles, the exports
resolve, `adm_arm_oneshot_breakpoint` returns success, and the breakpoint simply never fires.
That is exactly how the gap was found — the one-shot sat `Armed` with its executed count climbing
past 10M instructions while the program demonstrably loaded.

Because silence is the failure mode, `Application::ServiceStopOnEntry` detects it: a one-shot
that stays `Armed` with `executed == 0` can only mean the loop has no hook, and it says so by
name rather than blaming the program. If you add a loop and it still does not fire, that
message is how you will know the pattern matched something other than the live dispatch.

### What still works on an unhooked loop

Most of the debugger, because it does not go through this token:

- **UAE bpnode breakpoints** (`Z0`) and **memwatch watchpoints** — these run through
  `check_debugger()` / `do_specialties()`, which every loop calls.
- Registers, memory, disassembly, the seglist walk, ELF relocation, the DWARF line audit, and the
  whole GDB/DAP surface — all host-side.

Lost are: the CPU trace (so the frame scrubber's function bands and any trace-first analysis), the
bootblock trap, and the one-shot breakpoint — which **stop-on-entry depends on**.

### Adding another loop

1. Find the dispatch in `sources/src/newcpu.c`. The shape varies: `m68k_run_1` masks with
   `& 0xffff` and has a `loop_mode` fixup, `m68k_run_2_020` shifts `>> 16` and has none — which is
   why each loop needs its own pattern rather than one shared regex.
2. Insert `adm_cpu_trace_pre(r);` immediately **before** the dispatch and **after** the opcode
   fetch: the helper reads `cpudatatbl[r->opcode].length`, so it needs a valid opcode.
3. Regenerate `patches/series/0570-newcpu-cpu-trace.patch` against the pinned upstream commit, or
   add a new patch at the end of `patches/series.txt`. A patch that no longer applies aborts the
   build, which is the difference between a loud failure and a core that quietly lacks the hooks.
4. Bump `AdmVersion` / `ADM_VERSION` in both scripts.
5. Rebuild: `rm -r -force .\external\libretro-uae\ && .\scripts\build_puae.ps1`.

`m68k_run_jit` is the exception: it compiles blocks rather than dispatching per instruction, so a
per-instruction hook does not fit. Either force the interpreter when debugging, or accept that the
trace and one-shot are unavailable under JIT and say so in the UI.

## FloppyBridge — live physical floppy drives

Real-floppy support (Greaseweazle, DrawBridge, SuperCard Pro) driven by
**RobSmithDev/FloppyDriveBridge** (<https://github.com/RobSmithDev/FloppyDriveBridge>),
pinned at **`710fa15`** (upstream v1.6.0.5, 2024-08-07). The pin lives in both
`scripts/build_puae.ps1` (`$FloppyBridgeSha`) and `scripts/build_puae.sh`
(`FLOPPYBRIDGE_SHA`) and must be updated in both together, exactly like `$UpstreamSha`.


### Why there is anything to do at all

libretro-uae already carries the *whole* FloppyBridge integration in `sources/src/disk.c`,
behind `#ifdef FLOPPYBRIDGE` — the `ADF_FLOPPYBRIDGE` filetype, `DRV_FB`, the DMA-engine
branch in `DISK_update`, `floppybridge_init2()`'s config parsing, the runtime hot-attach
path. Upstream defines `FLOPPYBRIDGE` nowhere, so all of it compiles to nothing. And what
it compiles *against* is `floppybridge_lib.cpp`: a shim that `LoadLibrary()`s a separate
`FloppyBridge.dll` which nobody ships with a libretro core.

So this is two jobs: turn the define on, and replace that shim with the real drivers
compiled straight in.

### What is vendored, and under which licence

Fetched into `external/FloppyDriveBridge/` (download + extract + sentinel-file check,
mirroring how `libretro-uae` itself is fetched), then copied into
`external/libretro-uae/sources/src/floppybridge/`:

| Purpose | Files | Licence |
|---|---|---|
| Shared machinery | `CommonBridgeTemplate`, `RotationExtractor`, `SerialIO`, `pll`, `ftdi` (`.cpp` + `.h`) | MPL-2.0 **or** GPL-2.0-or-later |
| Greaseweazle | `GreaseWeazleBridge`, `GreaseWeazleInterface` (`.cpp` + `.h`) | MPL-2.0 **or** GPL-2.0-or-later |
| DrawBridge | `ArduinoFloppyBridge`, `ArduinoInterface` (`.cpp` + `.h`) | MPL-2.0 **or** GPL-2.0-or-later |
| SuperCard Pro | `SuperCardProBridge`, `SuperCardProInterface` (`.cpp` + `.h`) | MPL-2.0 **or** GPL-2.0-or-later |
| Interface headers | `floppybridge_abstract.h`, `floppybridge_common.h`, `floppybridge_config.h` | Unlicense (public domain) |
| Licence texts | `GPL2.txt`, `MPL2.txt` | — |

The driver implementations are **multi-licensed MPL-2.0 or GPL-2.0-or-later**, at the
recipient's choice. This project is distributed under GPL-2 (see [COPYING](COPYING)), so
they are taken under the GPL-2 arm and no licence conflict arises.

The three interface headers are RobSmithDev's public-domain (Unlicense) release. Two of
them — `floppybridge_abstract.h` and an older `floppybridge_lib.h` — were **already**
vendored in libretro-uae before any of this work; `floppybridge_abstract.h` is overwritten
with the 2024 version because the drivers declare `override` on four methods the 2021 copy
does not have.

Deliberately **not** taken: `ADFBridge` (a file-backed bridge this app has no use for) and
the entire `windows/` directory (the FloppyBridge DLL entry points, the MFC-ish profile
editor dialogs, bitmaps and resources).

`ftdi.cpp`/`.h` **is not optional**, despite being the FTDI-adapter path. It is not scoped
to DrawBridge: `SerialIO.h` holds an `FTDI::FTDIInterface m_ftdi` member and every driver
talks to hardware through `SerialIO`. It costs nothing to include — it `LoadLibrary()`s
`FTD2XX.DLL` at runtime and degrades gracefully when it is absent, so it adds no link-time
dependency.

### `patches/libretro-uae-floppybridge-portability.patch`

Applied to the vendored sources through the same
`git apply --no-index --ignore-space-change --ignore-whitespace --directory=external/libretro-uae`
mechanism as the other patches, and — deliberately — shared by both build scripts so the
edit text exists in exactly one place. Five problems, all the same root cause: these
sources have only ever been compiled by MSVC with `_UNICODE` defined. The patch file
carries its own detailed preamble; in short:

| File | Fix |
|---|---|
| `floppybridge_config.h` | `ROMTYPE_*` map to WinUAE's `ROMTYPE_FLOPYBRDGEn`, which PuAE has no equivalent of. They are only ever `#ifdef`-tested, so they become plain enable switches — which is what upstream's own `windows/FloppyBridge.cpp` does. |
| `ftdi.h` | Upstream includes `<Windows.h>` from *inside* `namespace FTDI`. Compiling `ftdi.cpp` first with GCC drags the Win32 SDK and libstdc++ into the namespace; `cstdlib` then fails with `'abs' has not been declared in '::'`. Pulled in at global scope first. |
| `ftdi.cpp` | `LoadLibrary(L"FTD2XX.DLL")` → `LoadLibraryA("FTD2XX.DLL")`. |
| `GreaseWeazleBridge.cpp`, `ArduinoFloppyBridge.cpp` | Six `RegQueryValueEx`/`RegSetValueEx` calls passing wide literals to the TCHAR macro → explicit `-A` variants with narrow literals. Upstream already calls `RegCreateKeyExA` on the adjacent line; the registry values are unchanged. |
| `SerialIO.cpp` | `SetupDiGetDeviceProperty` has no TCHAR mapping macro in MinGW's `SetupAPI.h`. The buffer is `WCHAR[128]`, so `-W` is what MSVC resolved it to anyway. |

**Do not "fix" these by defining `UNICODE`/`_UNICODE` instead.** `floppybridge_lib.h`'s
`TCharString` fields are shared with `disk.c`, which is compiled without it, so `TCHAR`
must stay `char` in every translation unit or those structs disagree about their own size.

### `patches/floppybridge/floppybridge_lib.{h,cpp}`

Copied over the vendored files rather than patched, because the `.cpp` is ours outright:
a **static factory** over the concrete drivers, replacing upstream's DLL shim. `.h` is
libretro-uae's own declaration with four methods added (the ones the 2024
`floppybridge_abstract.h` introduced) and one extra `ConfigOption_` constant.

The class keeps its name, its base (`FloppyDiskBridge`) and its entire public surface, so
**`disk.c` needs no modification at all** for this. What changed underneath:

- `isAvailable()` is now simply `true` — there is no DLL to find.
- `createDriverFromString()` parses upstream's own pipe-delimited config format
  (`[bridgeIndex|flags|comPort|bridgeMode|bridgeDensity]`, optional profile-name prefix)
  and `new`s the matching driver; round-trips byte-identically with WinUAE's format.
  Driver indices are 0 = DrawBridge, 1 = Greaseweazle, 2 = SuperCard Pro (all upstream),
  3 = Greaseweazle streaming, 4 = XCopy Standalone streaming, 5 = XCopy Standalone buffered
  (all three ours — see below).
- `enumCOMPorts()` goes straight to the vendored `SerialIO`'s platform enumeration.
- Every `FloppyDiskBridge` virtual forwards to the held `CommonBridgeTemplate`.

The whole **profile-ID system is stubbed** (`createDriverFromProfileID`, `getAllProfiles`,
`importProfilesFromString`, `exportProfilesToString`, `getProfileConfigAsString`,
`setProfileConfigFromString`, `createNewProfile`, `setProfileName`, `deleteProfile`) — it
exists only for RobSmithDev's Windows configuration GUI, `disk.c` reaches it only as a
fallback *after* `createDriverFromString()` has failed, and this app always supplies a
fully-specified config string. `showProfileConfigDialog()` is an `HWND` Win32 dialog and is
stubbed too; the host app has its own UI. The DNS version check in
`getBridgeDriverInformation()` is dropped: we are pinned to a commit, so it could only ever
be a surprise network call from an emulator core.

**Every forwarder is null-safe.** `floppybridge_init2()` assigns the bridge to
`floppy[dr].bridge` *even when `initialise()` failed*, and then keeps polling it — so the
no-driver answers matter: `maxMFMBitPosition()` returns 100000 (it is divided by),
`getMFMSpeed()` returns 1000 (= 100%), `isWriteProtected()` returns true, `hasDiskChanged()`
returns true. `getDriverInfo()` answers from the static driver table rather than the
instance, because `disk.c` calls it to *name the drive in its failure message*.

### `patches/floppybridge/Adm*Bridge.{h,cpp}`, `XCopyLink.{h,cpp}`, `XCopyLiveProtocol.h`

Ours outright, not derived from upstream, copied into the vendored directory alongside it
and compiled into the core. Two additional **streaming** drivers: they hold one track at a
time, read from the platter when the guest asks for it, and never cache or read ahead. The
three upstream `CommonBridgeTemplate` drivers are untouched and remain the fallback.

| File | Driver | Notes |
|---|---|---|
| `AdmStreamingBridge.{h,cpp}` | 3, Greaseweazle | Needs `patches/libretro-uae-floppybridge-latency.patch` for its incremental-flux API. |
| `AdmXCopyBridge.{h,cpp}` | 4, XCopy Standalone (streaming) | |
| `AdmXCopyBufferedBridge.{h,cpp}` | 5, XCopy Standalone (buffered) | The exception to the paragraph below: a `CommonBridgeTemplate` subclass shaped like the Greaseweazle driver, on the same link and board as driver 4. Bounded reads (`XCL_CMD_READ_TRACK`, firmware v718+), read-only. |
| `XCopyLink.{h,cpp}` | — | Transport for drivers 4 and 5, on the vendored `SerialIO`. |
| `XCopyLiveProtocol.h` | — | The wire contract. **Also lives in the XCopyStandalone firmware repo as `shared/XCopyLiveProtocol.h` and the two must stay byte-identical** — the same rule as the two build scripts. Both ends carry `static_assert`s on the struct sizes so drift cannot go unnoticed. |

Both drivers subclass `FloppyDiskBridge` directly rather than `CommonBridgeTemplate`, which
is what keeps `disk.c` unmodified: the template's whole job is capture-a-revolution-then-
replay-it, and that serialisation is precisely what these avoid. Both are read-only —
`isWriteProtected()` returns true and the write methods refuse — so writing stays with the
buffered drivers.

### Build-system changes

- `Makefile.common` — `SOURCES_CXX` gains all twelve `.cpp` files. It was already consumed
  by the Makefile but had never been populated by anything.
- `Makefile` — `-DFLOPPYBRIDGE`; `disk.o` built with `$(CXX)`; the C++ standard headers
  pre-parsed with `-include` so none land inside `disk.c`'s `extern "C"` block; and the
  link line.
- **`CXXFLAGS` carries no optimisation level at all** in this Makefile — the `.cpp` rule
  uses `CXXFLAGS` without `CFLAGS`. Survivable for a stub; not for a real-time MFM decoder
  on a worker thread. `-O3` (or `-O0 -g` under `DEBUG=1`) is now added explicitly.
- Windows link gains `-lsetupapi -lws2_32 -luuid`. The MSVC `#pragma comment(lib, ...)` in
  those sources does nothing under GCC. `uuid` is the non-obvious one:
  `SerialIO.cpp`'s guarded `DEFINE_GUID(GUID_DEVINTERFACE_COMPORT, ...)` only *declares*
  the symbol (no `INITGUID`), so the definition has to come from the library.

**Measured effect on the DLL** (before = §3b, define on but drivers absent):

| Measure | Before | After |
|---|---|---|
| Named exports | 204 | **204 (identical, diff-verified)** |
| Import DLLs | 13 | 16 — adds `ADVAPI32`, `SETUPAPI`, `WS2_32` |
| C++ translation units | 2 | 13 |

The three new imports are core Windows system DLLs present on every install; the C++
runtime is still linked statically, so it adds no import of its own.

### Attaching a drive

`adm_set_drive_bridge_config` (`series/0840`) switches a drive slot to `DRV_FB` with a driver,
its port and its settings; the host's *Floppy Drives* window and the console's `LIVE` command
call it. A slot can be attached while the machine runs (a hot attach through the prefs) or while
it is stopped, in which case the attachment is applied at the next start. Several drives can be
live at once, each on its own driver. Reading and writing both work, protected disks included.

Nothing in UAE ever freed a bridge, so a dropped one kept its serial port and its thread:
`series/0850` closes the port when a bridge is dropped, and `series/0860` stops every live drive
in `retro_deinit()`, before the DLL can be unloaded under a running thread.

## Per-drive floppy write protect

Two exports, `adm_set_floppy_write_protect(drive, wp)` and `adm_get_floppy_write_protect(drive)`,
so the host can protect one emulated floppy at a time.

### Why

PuAE already has everything needed; the libretro layer just never exposes it.
`drive_writeprotected()` (`sources/src/disk.c`) reads `drv->forcedwrprot` on every access and
`DISK_status_ciaa()` feeds the result to the CIAA PRA `/WPRO` bit the guest polls, so a change
takes effect **without ejecting the disk** — the same as flipping the tab on a real floppy while
it is in the drive. What libretro offers instead is `puae_floppy_write_protection`, which is
all four drives at once and reinserts every disk when it changes.

Worse, the disk-control path works against a per-drive flag: `set_eject_state(false)` runs
`floppy_open_redirect()`, which calls `disk_setwriteprotect(&currprefs, i, ..., 0)` — so every
insert the host performs actively clears the slot's protection. The host side therefore
re-applies the flag at the end of `AmigaEmulator::InsertDisk()`, and `PollDiskState()` compares
the request against `gui_data.drives[i].floppy_protected` each frame and re-pushes if the core
has lost it. That read-back needed no new export: the field was already in the `gui_data` mirror.

### Why not `disk_setwriteprotect()`

It is the obvious-looking entry point and it is the wrong one. It chmods the host file
(`diskfile_readonly()`) — which is exactly why the host app carries `ClearReadOnlyAttribute()`
to undo it on temp images — and it sets `floppy[num].forcedwrprot = false` as its last act, so
calling it to protect a drive un-protects it. The helper here writes `forcedwrprot` directly and
touches no file.

### How it is applied

In the patch series:

- `sources/src/disk.c` gains `adm_floppy_set_write_protect()` / `adm_floppy_get_write_protect()`,
  inserted before `DISK_init()`. They live in `disk.c` because `floppy[]` is static there.
  The setter writes `floppy[n].forcedwrprot`, `floppy[n].newnamewriteprotected` **and** both
  `currprefs`/`changed_prefs` `floppyslots[n].forcedwriteprotect`, because the re-insert paths
  do not all read the same field. `DISK_vsync()`'s df-diff re-insert reads the prefs; the
  *delayed* insert in `DISK_hsync()` reads `drv->newnamewriteprotected`. That delayed one is
  not an edge case — `set_eject_state(false)` ends in `DISK_reinsert()`, so **every** host
  insert schedules it, two seconds out. Setting only the prefs left the slot writable from
  that moment on.
  The getter returns `drive_writeprotected()`, which is broader than the forced flag — an empty
  drive and the global `floppy_read_only` option both read as protected.
- `libretro/libretro-core.c` gains the two `RETRO_API` wrappers, appended after the FloppyBridge
  config exports (which must therefore be patched first; the script fails loudly if they are not).
- `libretro/link.T` lists both symbols. Without that the `--version-script` hides them however
  they are declared.

Core version `1.79.0`.

### Host side

`AmigaEmulator` resolves both optionally, so a core predating them still loads and
`HasWriteProtectSupport()` reports false — protection is then enforced host-side only (every
`AdfHandler` mutator checks its own flag) and the guest still sees a writable disk.

### Why the host re-asserts it every frame

Setting the flag once is not enough, and the failure is silent data loss rather than a refused
write. Kickstart samples the line immediately before **every** track write — `trackdisk.device`
does `btst #3,$BFE001` and returns `TDERR_WriteProt` on a low bit (verified by disassembly:
`kick34005.A500` offset `0x2a286`, error path at `0x2a36e` = `moveq #28,d2`). So a single frame
with `forcedwrprot` dropped is one whole track written to a disk the user had locked. PuAE then
throws that track away in `drive_write_data()` once protection is back, AmigaDOS reads its own
write back as garbage, and the user gets the repeating *"read/write error"* requester and a file
that is gone after a reboot — instead of a clean *"disk is write protected"*.

Four core paths clear `forcedwrprot` without asking: `drive_eject()`, the delayed
`drive_insert()` above, `restore_disk()` on every save-state restore (it re-inserts with
`forcedwriteprotect = false`, so a rewind drops protection), and `disk_setwriteprotect()` itself.
`AmigaEmulator::AssertFloppyWriteProtect()` therefore pushes the requested state for every
non-bridged slot before each `retro_run()` — four stores a frame. `PollDiskState()` keeps the
old `gui_data` comparison, now as a *detector*: a hit there means a path inside that frame
cleared the flag, so it counts into `GetFloppyWriteProtectDrift()` and warns to the log. The
console's `LIVE` table shows the three values side by side (`Prot` requested, `Core` what the
machine sees, `Drift`); on a locked slot holding a disk they must read `on on 0`.

### And the toggle re-inserts the disk

Asserting the flag is still not the whole story, and measurement said so: with the assert in
place a locked disk *still* took writes mid-session, drift climbed, and the files were gone
after a reset — while the very same disk refused writes correctly once the machine had been
reset. The difference is `drive_insert()`, which sets `drv->wrprot` **as well as**
`drv->forcedwrprot`; that pair survives everything in the core that clears the forced flag on
its own. AmigaDOS also only re-reads a disk's protection on a disk change, and it holds
filesystem buffers for a volume it believes is writable — buffers that are then written to a
disk PuAE now refuses, which is what turns a locked disk into a corrupt one.

So `EmulatorView::SetDriveWriteProtect()` calls `AmigaEmulator::ReinsertDisk()` whenever the
value actually changes on a running machine with an image in the slot, in **both** directions.
That is also the physical truth: no stock drive lets you reach the tab with a disk in it. The
guest sees the volume leave and return after UAE's own two-second `setdskchangetime()` delay.
`ReinsertDisk()` re-inserts the **temp image the core already holds**, never the host's copy —
the guest's writes since it was mounted live in that file, and re-inserting the document's bytes
would silently roll the disk back.

## Machines, expansions and host ports

Upstream libretro-uae builds much of WinUAE's hardware with its switch turned off, or with the host
end of it stubbed out. The patches from 1190 to 1290 turn those on and give them a host. Each
patch's preamble has the detail; in short:

| Patch | What it adds | Platforms |
| --- | --- | --- |
| `1190-chipset-revision-options` | `puae_chipset`, `puae_chipset_compatible`, `puae_agnus_model`, `puae_agnus_size`, `puae_denise_model`, which survive the model preset's rewrite of the chipset fields | all |
| `1200-mmu-model-option` | `puae_mmu_model` (auto, full, ec, none), so a 68030/040/060 can run without MMU emulation or as an EC part | all |
| `1210-serial-parallel-host-backend` | The host end of the serial port (`TCP://host:port` to listen, `.../connect` to dial out, or a host serial device) and of the parallel port (bytes appended to a file), in `retrodep/adm_hostio.c` | all |
| `1220-midi-host-devices` | MIDI in and out through winmm devices, parsed from the serial byte stream at 31400 baud | Windows |
| `1230-bsdsocket-library` | `bsdsocket.library` through the host's TCP/IP stack: Amiberry's host layer (GPL-2.0-or-later), WinUAE's Winsock code on Windows and the POSIX one elsewhere | all |
| `1240-a2065-uaenet-slirp` | The A2065 Ethernet card and `uaenet.device` on WinUAE's built-in SLIRP NAT (the Amiga is 10.0.2.15; no host driver or privileges) | Windows |
| `1250-sound-cards` | Toccata, Prelude, Prelude 1200 and UAESND (the board WinUAE's AHI driver uses), mixed into Paula's output | all |
| `1260-kickstart-cpu-mismatch` | A Kickstart the CPU cannot run (an A1200 ROM on a 68000) no longer crashes the host: the mismatch is logged as an error and the machine fails as the real hardware would | all |
| `1270-serial-poll-batching-midi-sysex-pool` | The serial port polls the host once every 16 idle asks and writes a frame's bytes at once; MIDI SysEx buffers are handed back lazily instead of slept for | all |
| `1280-adm-identify-rom` | `adm_identify_rom()`: a Kickstart identified by its contents against WinUAE's ROM database, `rom.key`-aware, for the setup wizard and *System Files* | all |
| `1290-kickstart-ext-option` | `puae_kickstart_ext`, an extended ROM slot (CD32, CDTV) the host can set on every launch path | all |
