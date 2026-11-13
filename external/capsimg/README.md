# capsimg (SPS/CAPS Decoder Library)

This directory holds the **public API headers** of the SPS/CAPS decoder library,
vendored from the redistributable spsdeclib / capsimg sources. They describe the
ABI we decode IPF images against. The library itself is **not** linked at build
time — it is loaded at runtime via `dlopen`/`LoadLibrary` (see
`src/core/CapsImageLoader.cpp`), exactly like the PuAE emulator core.

## Why a runtime plugin?

IPF reading has no open-source implementation; every Amiga emulator delegates to
this SPS library. It is freely redistributable as a binary plugin but is not
open source, so it is kept as a separable, optional runtime dependency rather
than a hard build dependency. When the plugin is absent, IPF files simply fail
to open with a friendly message instead of crashing.

## Installing the plugin

Place the platform plugin in the `emulation/` folder next to the app binary (it
is copied to the build output alongside the PuAE core):

- Windows: `capsimg.dll`
- Linux:   `capsimg.so` (or `libcapsimg.so`)
- macOS:   `capsimg.dylib` (or `libcapsimg.dylib`)

`CapsImageLoader` also falls back to the OS library search path if the plugin is
not found in `emulation/`.

Compatible builds:

- https://www.softpres.org/download
- https://fs-uae.net/download#plugins
- https://github.com/rsn8887/capsimg/releases (multi-platform)
