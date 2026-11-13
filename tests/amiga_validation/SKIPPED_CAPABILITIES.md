# Skipped target capabilities

The fixture lane intentionally advertises only `fixture-oracle`, so every target capability is
reported as skipped. The real-core adapter currently implements mailbox/symbol
relocation, CPU snapshots, Copper events, blitter events, DMA/timeline normalization, idle events,
CIA snapshots, save states, profile selection, and capability negotiation.

The following capabilities are still absent from the real lane:

| Capability | Required implementation |
|---|---|
| `framebuffer-golden` | Headless framebuffer capture that does not require an OpenGL context, an independent planar/Copper reference renderer, checked profile-specific crops and hashes, and an explicit reviewed golden-update command. |
| `alignment-workload` | Target code that emits two Bartman idle spans, CPU/Copper/blitter markers at fixed beam positions, CPU- and Copper-launched blits, and independent PAL/NTSC cycle expectations. |
| `callstack` | Deterministic nested/recursive/tail/interrupt guest functions plus integration of `CallStackProfiler` output into the runner snapshot. |
| `dwarf` | C target objects built at pinned `-O0`/`-O2`/inline settings, linked sibling ELF DWARF line/inline data, and normalized source/inline-frame serializers. |
| `breakpoints` | The bounded one-shot PUAE API is implemented and proven by the phase-1 CPU/blitter CTest. Enabling this broader capability still needs phase-3 guest cases for conditional breakpoints, step/resume behavior, cancellation/budget-expiry diagnostics, and normalized breakpoint snapshots. |
| `watchpoints` | Guest read/write/custom-register cases, core watchpoint arming without UI state, and serialization of PC/address/value/access source/size. |
| `bitplanes` | Deterministic 1–8-plane target assets, pure bitplane/display-layout snapshots consumed by the UI and runner, and profile-aware OCS/ECS/AGA expectations. |
| `sprites` | Eight-sprite and attached-sprite workloads, sprite pointer/event view models, collision/priority assertions, and reference pixels. |
| `display-masks` | Off-screen display composition with each plane/playfield/sprite mask and assertions that only output pixels—not emulated registers or execution—change. |
| `resources` | Bartman bitmap/palette/Copper resource registration workloads plus pure metadata/preview serializers and lifecycle/reset checks. |
| `panel-render` | Deterministic off-screen ImGui rendering at pinned fonts/DPI/size, curated panel PNGs, thresholded diff images, and artifact links. |
| `mcp` | An in-process MCP vertical slice that invokes the public handlers against the same paused emulator and compares normalized JSON with direct snapshots. |
| `provenance-hashes` | A bundled SHA-256 implementation (or existing trusted hash service) for the loaded core binary plus a core export identifying the selected Kickstart ROM image without reading or distributing ROM contents. |

The blocking real phase-1 CTest now proves `cpu`, `mailbox`, `symbols`, and `blitter` across repeated
clean launches. The fixture report still lists every target capability as skipped by design. The
capture/control capabilities below exist in the real adapter but still need dedicated workloads,
expectations, and passing real-core evidence before they can be treated as complete:

`copper`, `timeline`, `dma`, `idle`, `partial-frame`, `cia`, `save-state`,
`capability-negotiation`, and `matrix`.

In addition, full phase acceptance needs more scenarios behind capabilities already present in the
adapter: exception recovery, trace overflow, all CIA timer/TOD/ICR modes, isolated DMA channels,
advanced Copper and blitter modes, ADF semantic parity, save-state continuation parity, clean reset
history, and the complete PAL/NTSC/ECS/AGA matrix. These are workloads and expectations, not new
adapter capability names.
