#!/usr/bin/env python3
"""Generate assets/amiga_lvos.json from Amiga NDK .fd (function definition) files.

The .fd files are the authoritative source for library vector offsets (LVOs): the
negative displacement off the library base register that a `JSR (a6,-$xxx)` call
uses. LibraryCallAnnotator reads the produced JSON, keyed by the library's Amiga
*node name* (e.g. "exec.library", "timer.device"), and resolves a call's target
library at runtime from the live base register (see LibraryCallAnnotator.cpp).

.fd format recap:
    ##base   _SysBase          ; base-variable name (ignored here)
    ##bias   30                ; magnitude of the first vector offset (-30)
    ##public / ##private       ; section markers (private vectors still占 a slot)
    Name(args)(regs)           ; one library function; each advances the offset 6
    * comment                  ; ignored
    ##end                      ; stop

Every function entry — public or private — occupies a 6-byte vector, so the offset
counter advances by 6 for each (honouring any mid-file `##bias` reset). We emit all
of them so offsets stay exact; private vectors keep their declared fd name.

The .fd files are vendored in scripts/fd/ (NDK 3.9, from the BartmanAbyss/
ghidra-amiga mirror) and are the default input, so regeneration is offline and
reproducible. Pass --fetch to (re)download them from GitHub into scripts/fd/, or
--fd-dir to read from a different local directory.

Usage:
    python3 scripts/gen_lvos.py                 # convert vendored scripts/fd/*.fd
    python3 scripts/gen_lvos.py --fetch         # refresh scripts/fd/ then convert
    python3 scripts/gen_lvos.py --fd-dir ./fd   # convert from another directory
"""

import argparse
import json
import os
import re
import sys
import urllib.request

RAW_BASE = "https://raw.githubusercontent.com/BartmanAbyss/ghidra-amiga/master/data/fd"

# .fd basename -> Amiga node name (the string LibraryCallAnnotator matches against
# the live library node). Default for anything not listed is "<prefix>.library"
# where prefix is the basename minus a trailing "_lib". Override the device /
# resource / oddly-named files here.
NODE_NAME_OVERRIDES = {
    "wb_lib.fd": "workbench.library",
    "timer_lib.fd": "timer.device",
    "console_lib.fd": "console.device",
    "input_lib.fd": "input.device",
    "ramdrive_lib.fd": "ramdrive.device",
    "disk_lib.fd": "disk.resource",
    "misc_lib.fd": "misc.resource",
    "potgo_lib.fd": "potgo.resource",
    "battclock_lib.fd": "battclock.resource",
    "battmem_lib.fd": "battmem.resource",
    "cardres_lib.fd": "card.resource",
    "cia_lib.fd": ["ciaa.resource", "ciab.resource"],
}

# .fd files to convert. Curated to the system libraries / devices / resources with
# unambiguous node names that turn up in real 68k code; BOOPSI gadget classes
# (button_lib, slider_lib, ...) are intentionally excluded.
FD_FILES = [
    "exec_lib.fd", "dos_lib.fd", "graphics_lib.fd", "intuition_lib.fd",
    "layers_lib.fd", "gadtools_lib.fd", "asl_lib.fd", "diskfont_lib.fd",
    "utility_lib.fd", "expansion_lib.fd", "keymap_lib.fd", "icon_lib.fd",
    "commodities_lib.fd", "iffparse_lib.fd", "datatypes_lib.fd", "locale_lib.fd",
    "lowlevel_lib.fd", "mathffp_lib.fd", "mathieeesingbas_lib.fd",
    "mathieeedoubbas_lib.fd", "mathtrans_lib.fd", "mathieeesingtrans_lib.fd",
    "mathieeedoubtrans_lib.fd", "wb_lib.fd", "bullet_lib.fd", "nonvolatile_lib.fd",
    "realtime_lib.fd", "rexxsyslib_lib.fd", "translator_lib.fd", "amigaguide_lib.fd",
    "arp_lib.fd", "timer_lib.fd", "console_lib.fd", "input_lib.fd",
    "ramdrive_lib.fd", "disk_lib.fd", "misc_lib.fd", "potgo_lib.fd",
    "battclock_lib.fd", "battmem_lib.fd", "cardres_lib.fd", "cia_lib.fd",
]

FUNC_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def node_names_for(basename):
    if basename in NODE_NAME_OVERRIDES:
        ov = NODE_NAME_OVERRIDES[basename]
        return ov if isinstance(ov, list) else [ov]
    prefix = basename[:-3]  # drop ".fd"
    if prefix.endswith("_lib"):
        prefix = prefix[:-4]
    return [prefix + ".library"]


def parse_fd(text):
    """Returns {"-30": "Open", ...} for one .fd file."""
    table = {}
    bias = None  # current vector magnitude
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("*"):
            continue
        if line.startswith("##"):
            directive = line[2:].strip()
            low = directive.lower()
            if low.startswith("bias"):
                parts = directive.split()
                if len(parts) >= 2:
                    bias = int(parts[1])
            elif low.startswith("end"):
                break
            # ##base/##public/##private: nothing to do (private vectors still count)
            continue
        m = FUNC_RE.match(line)
        if not m:
            continue
        if bias is None:
            bias = 30  # default first-vector magnitude
        table[str(-bias)] = m.group(1)
        bias += 6
    return table


def fetch_fd_files(fd_dir):
    """(Re)download the curated .fd set into fd_dir from GitHub."""
    os.makedirs(fd_dir, exist_ok=True)
    for basename in FD_FILES:
        url = f"{RAW_BASE}/{basename}"
        with urllib.request.urlopen(url) as resp:
            data = resp.read()
        with open(os.path.join(fd_dir, basename), "wb") as f:
            f.write(data)
        print(f"  fetched {basename}")


def load_fd(basename, fd_dir):
    with open(os.path.join(fd_dir, basename), "r", encoding="latin-1") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    default_fd_dir = os.path.join(repo_root, "scripts", "fd")
    ap.add_argument("--fd-dir", default=default_fd_dir,
                    help="directory of .fd files to convert (default: scripts/fd/)")
    ap.add_argument("--fetch", action="store_true",
                    help="(re)download the curated .fd set into --fd-dir first")
    ap.add_argument("-o", "--out", default=None,
                    help="output path (default: <repo>/assets/amiga_lvos.json)")
    args = ap.parse_args()

    if args.fetch:
        fetch_fd_files(args.fd_dir)

    out_path = args.out or os.path.join(repo_root, "assets", "amiga_lvos.json")

    result = {
        "_comment": ("Amiga library vector offset (LVO) tables, keyed by Amiga node "
                     "name. Keys are the negative displacement off the library base "
                     "register as shown in disassembly, e.g. 'JSR (a6,-$01c8)' -> -456 "
                     "-> exec/DoIO. Generated by scripts/gen_lvos.py from NDK 3.9 .fd "
                     "files (" + RAW_BASE + "). Do not edit by hand; rerun the script."),
    }

    total_funcs = 0
    for basename in FD_FILES:
        try:
            text = load_fd(basename, args.fd_dir)
        except Exception as e:  # noqa: BLE001 — report and continue
            print(f"  skip {basename}: {e}", file=sys.stderr)
            continue
        table = parse_fd(text)
        if not table:
            print(f"  skip {basename}: no functions parsed", file=sys.stderr)
            continue
        for node in node_names_for(basename):
            result[node] = table
        total_funcs += len(table)
        print(f"  {basename}: {len(table)} vectors -> "
              f"{', '.join(node_names_for(basename))}")

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=True)
        f.write("\n")

    libs = len([k for k in result if not k.startswith("_")])
    print(f"\nWrote {out_path}: {libs} library tables, {total_funcs} vectors")


if __name__ == "__main__":
    main()
