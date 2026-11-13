# Third-party notices

Peekaboo is licensed under the GNU General Public License, version 2 (see `COPYING`). It is built
on the work below, each under its own licence. This file ships in every Peekaboo package.

**Peekaboo includes no Kickstart ROMs, Workbench or AmigaOS disks.** Those are copyrighted; use your
own (Amiga Forever, AmigaOS 3.2, or ROMs and disks dumped from your own Amiga). Amiga is a
trademark of its owner; Peekaboo is an independent project and is not affiliated with it.

## The emulator core (`emulation/puae_libretro.dll`)

| Component | Licence | Where |
|---|---|---|
| PUAE / libretro-uae, patched for Peekaboo (`patches/`) | GPL-2.0 | https://github.com/libretro/libretro-uae |
| WinUAE (the core descends from it; ROM database, Picasso96) | GPL-2.0 | https://github.com/tonioni/WinUAE |
| AROS Kickstart replacement, built into the core | AROS Public License 1.1 | https://aros.sourceforge.io |
| HRTmon, built into the core | GPL | https://github.com/tonioni/WinUAE |
| WHDLoad and its support files, as libretro-uae bundles them | see whdload.de | https://www.whdload.de |
| FloppyBridge / FloppyDriveBridge | MPL-2.0 or GPL-2.0-or-later (Unlicense parts) | https://github.com/RobSmithDev/FloppyDriveBridge |
| Amiberry's bsdsocket.library host layer (`bsdsocket_host.cpp`) | GPL-2.0-or-later | https://github.com/BlitterStudio/amiberry |
| SLIRP user-mode networking by Danny Gasparovski, as WinUAE carries it (A2065 / uaenet.device, Windows) | BSD-style, notice below | https://github.com/tonioni/WinUAE |

The WHDLoad support files libretro-uae embeds also carry a few AmigaOS commands and libraries
(SetPatch, Assign, ...). They come unchanged from upstream libretro-uae, as in its RetroArch core.

## The application

| Component | Licence | Where |
|---|---|---|
| Dear ImGui | MIT | https://github.com/ocornut/imgui |
| GLFW | zlib | https://www.glfw.org |
| glad | MIT | https://github.com/Dav1dde/glad |
| {fmt} | MIT | https://github.com/fmtlib/fmt |
| nlohmann/json | MIT | https://github.com/nlohmann/json |
| Native File Dialog Extended | zlib | https://github.com/btzy/nativefiledialog-extended |
| cpr | MIT | https://github.com/libcpr/cpr |
| libcurl | curl licence | https://curl.se |
| Crc32c | BSD-3-Clause | https://github.com/google/crc32c |
| ADFlib | GPL-2.0 / LGPL | https://github.com/adflib/ADFlib |
| xdms | public domain | https://gitlab.com/heikkiorsila/xdms |
| RECOIL | GPL-2.0-or-later | https://recoil.sourceforge.net |
| libopenmpt | BSD-3-Clause | https://lib.openmpt.org |
| mpg123 | LGPL-2.1 | https://www.mpg123.de |
| libogg, libvorbis | BSD-3-Clause | https://xiph.org |
| libzip | BSD-3-Clause | https://libzip.org |
| zlib | zlib | https://zlib.net |
| bzip2 | BSD-style | https://sourceware.org/bzip2 |
| brotli | MIT | https://github.com/google/brotli |
| FreeType | FreeType Licence (FTL) | https://freetype.org |
| libpng | libpng licence | http://www.libpng.org |
| tinyxml2 | zlib | https://github.com/leethomason/tinyxml2 |
| md4c, imgui_md | MIT | https://github.com/mity/md4c |
| miniaudio | public domain / MIT-0 | https://miniaud.io |
| stb_image | public domain / MIT | https://github.com/nothings/stb |
| nanosvg | zlib | https://github.com/memononen/nanosvg |
| gif.h | public domain | https://github.com/charlietangora/gif-h |
| TinySHA1 | ISC-style | https://github.com/mohaps/TinySHA1 |
| SPS CAPS API headers (the library itself is not shipped) | freely redistributable | http://www.softpres.org |
| cpp-httplib | MIT | https://github.com/yhirose/cpp-httplib |
| libdisk (keirf/disk-utilities), copy-protection track formats | public domain (Unlicense) | https://github.com/keirf/disk-utilities |

## Data

| Component | Licence | Where |
|---|---|---|
| The boot block library (`bootblocks/`), from AmigaBootBlockReader by jasonthesmith79 | used with permission | https://github.com/jasonthesmith79/AmigaBootBlockReader |
| M68000 instruction reference (`assets/m68k_instruction_docs.json`), from the MOTOROLA M68000 FAMILY Programmer's Reference Manual | Copyright 1992 Motorola Inc./NXP, adapted with permission (as each entry states) | |
| Custom chip register reference (`assets/ChipRegisterDocs.xml`), converted from the Amiga memory map | as published | http://oscomp.hu/depot/amiga_memory_map.html |
| Amiga NDK 3.9 library function definitions (`scripts/fd/`) and include files (`tests/native/include/`), for building and naming system calls | Commodore / Amiga, Inc. | |
| TOSEC catalogue data (`tosec/`) | as published by TOSEC | https://www.tosecdev.org |

## Fonts and icons

| Component | Licence | Where |
|---|---|---|
| Topaz, TopazPlus, MicroKnight, P0T-NOoDLE, mO'sOul by dMG of Trueschool | CC BY-NC-SA 3.0, with the author's addition (`assets/fonts/LICENSE-dMG-Amiga-fonts.txt`) | https://www.trueschool.se |
| Font Awesome Free 6 | icons CC BY 4.0, font SIL OFL 1.1 | https://fontawesome.com |
| amiga-demo-pixel-type-8x8 | as published by its author | |

## Ideas and code adapted

- The copper raster overlay and the blitter overlay are adapted from **ENGINE9000** by alpine9000,
  GPL-2.0 (https://github.com/alpine9000/engine9000-public).
- The Bartman resource and profiler conventions follow **vscode-amiga-debug** by BartmanAbyss
  (https://github.com/BartmanAbyss/vscode-amiga-debug).
- The TOSEC data is the **TOSEC** project's (https://www.tosecdev.org).

## Not included, used if you provide them

- **FFmpeg**, for video capture in codecs other than the built-in AVI: Peekaboo runs an
  `ffmpeg` executable you point it at, and ships none.
- The **SPS/CAPS `capsimg` plugin**, for IPF images: freely redistributable, loaded from
  `emulation/` when present.
- **ScreenScraper.fr** (https://www.screenscraper.fr) is an online service the Disk Identifier
  queries; its data stays under its own terms.

## SLIRP copyright notice

The emulator core includes SLIRP. Its licence asks a binary distribution to reproduce this:

```text
 Copyright (c) 1995,1996 Danny Gasparovski.  All rights reserved.
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:
 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 DANNY GASPAROVSKI OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

Slirp uses code that is copyrighted by the following people/organizations:

Juha Pirkola.
Gregory M. Christy.
The Regents of the University of California.
Carnegie Mellon University.
The Australian National University.
RSA Data Security, Inc.

Please read the top of each source file for the details on the various
copyrights.
