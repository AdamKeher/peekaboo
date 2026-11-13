#!/usr/bin/env python3
"""One-off sync of the bootblocks/ .bin library with the brainfile database.

For every bootblocks/*.bin file (1024 bytes = block 0 followed by block 1) this:
  * computes its CRC32 signature (identical to BrainFile::CalculateCRC32),
  * matches it against virus/brainfile.json by CRC, then by recognition pattern,
  * for matches: renames the .bin to the canonical brainfile Name and records
    the relative path in the entry's "File" field (backfilling "CRC" if absent),
  * for non-matches: appends a new entry built from the filename + generated CRC,
  * writes the merged database to bootblocks/brainfile.json (indent=4) so the
    library and its metadata live together.

This is a one-time bootstrap: it reads the original hand-curated seed at
virus/brainfile.json (kept in the repo as the reproducible input) and writes the
live database to bootblocks/brainfile.json, which the app reads/writes from then
on. Run from the repository root:

    python3 scripts/sync_bootblocks.py

Existing entries keep all their fields (Class, KS, Recog, Notes, CRC); the script
only adds a "File" path to matched entries and appends new entries for .bin files
not present in the seed. Renaming only ever touches matched "keeper" files, so
re-running is idempotent (zero renames, byte-identical output).
"""

import json
import sys
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_BRAINFILE = REPO_ROOT / "virus" / "brainfile.json"
BOOTBLOCKS_DIR = REPO_ROOT / "bootblocks"
DST_BRAINFILE = BOOTBLOCKS_DIR / "brainfile.json"

# Field order mirrors BrainFile::SaveDatabase so future C++ saves diff minimally.
FIELD_ORDER = ["Name", "Recog", "Class", "Notes", "KS", "CRC", "File", "Bootable", "Data"]


def sanitize_name(name: str) -> str:
    """Port of BootBlockLibrary::SanitizeName (src/core/BootBlockLibrary.cpp:12)."""
    invalid = set('/\\:*?"<>|')
    out = []
    for ch in name:
        if ch in invalid:
            out.append("_")
        elif ord(ch) < 0x20:
            continue  # skip control characters
        else:
            out.append(ch)
    cleaned = "".join(out).strip(" .")
    return cleaned if cleaned else "bootblock"


def crc32_hex(data: bytes) -> str:
    """Uppercase 8-hex CRC32 (reflected IEEE-802.3) over the full 1024 bytes.

    Equivalent to BrainFile::CalculateCRC32 which folds sector0 then sector1.
    """
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"


def normalize_crc(value) -> str:
    """Normalize a brainfile CRC field to an uppercase hex string, or '' if none."""
    if value is None:
        return ""
    if isinstance(value, int):
        return f"{value:08X}"
    s = str(value).strip()
    if not s:
        return ""
    if s.lower().startswith("0x"):
        s = s[2:]
    try:
        return f"{int(s, 16):08X}"
    except ValueError:
        return s.upper()


def match_recog(recog: str, sector0: bytes, sector1: bytes) -> bool:
    """Port of BrainFile::MatchRecognitionPattern (src/core/BrainFile.cpp:219).

    recog is a comma-separated list of offset,value pairs. Offsets < 512 index
    sector0; offsets >= 512 index sector1[offset-512].
    """
    if not recog:
        return False
    parts = [p.strip() for p in recog.split(",") if p.strip() != ""]
    if len(parts) < 2:
        return False
    # Walk pairs; a trailing unpaired token is ignored (mirrors the C++ loop).
    for i in range(0, len(parts) - 1, 2):
        try:
            offset = int(parts[i])
            value = int(parts[i + 1]) & 0xFF
        except ValueError:
            return False
        if offset < 512:
            if offset >= len(sector0):
                return False
            actual = sector0[offset]
        else:
            idx = offset - 512
            if idx >= len(sector1):
                return False
            actual = sector1[idx]
        if actual != value:
            return False
    return True


def main() -> int:
    if not SRC_BRAINFILE.is_file():
        print(f"error: {SRC_BRAINFILE} not found", file=sys.stderr)
        return 1
    if not BOOTBLOCKS_DIR.is_dir():
        print(f"error: {BOOTBLOCKS_DIR} not found", file=sys.stderr)
        return 1

    with SRC_BRAINFILE.open("r", encoding="utf-8") as f:
        db = json.load(f)

    entries = db.get("Bootblocks")
    if not isinstance(entries, list):
        print("error: brainfile missing 'Bootblocks' array", file=sys.stderr)
        return 1

    # Index entries by CRC (skipping empty/0) and collect those with a recog pattern.
    crc_index = {}
    recog_entries = []
    for entry in entries:
        crc = normalize_crc(entry.get("CRC"))
        if crc and crc != "00000000":
            crc_index.setdefault(crc, entry)
        if entry.get("Recog"):
            recog_entries.append(entry)

    bin_files = sorted(BOOTBLOCKS_DIR.glob("*.bin"))
    stats = {"crc": 0, "recog": 0, "created": 0, "collisions": 0, "renamed": 0}

    # Pass 1 — content-based match for every .bin (independent of its filename).
    scanned = []  # {path, entry, by, crc, bootable, canonical}
    for path in bin_files:
        data = path.read_bytes()
        if len(data) < 1024:
            data = data + bytes(1024 - len(data))
        sector0 = data[0:512]
        sector1 = data[512:1024]
        crc = crc32_hex(data)
        bootable = "True" if sector0[0:3] == b"DOS" else "False"

        entry = crc_index.get(crc)
        by = "crc" if entry is not None else None
        if entry is None:
            for cand in recog_entries:
                if match_recog(cand.get("Recog", ""), sector0, sector1):
                    entry = cand
                    by = "recog"
                    break

        rec = {"path": path, "entry": entry, "by": by, "crc": crc, "bootable": bootable}
        if entry is not None:
            rec["canonical"] = sanitize_name(entry.get("Name") or path.stem)
            stats[by] += 1
        scanned.append(rec)

    # Pass 2 — choose one "keeper" per canonical name. A file already named
    # canonically wins (so reruns never rename); otherwise the first sorted file.
    groups = {}
    for rec in scanned:
        if rec["entry"] is not None:
            groups.setdefault(rec["canonical"], []).append(rec)
    keepers = set()
    for canonical, members in groups.items():
        keeper = next((m for m in members if m["path"].stem == canonical), members[0])
        keepers.add(id(keeper))

    # Pass 3 — apply renames + build/augment entries. Only matched keepers are
    # renamed; duplicates and unmatched files keep their name and get their own
    # entry, so the operation is idempotent.
    for rec in scanned:
        path = rec["path"]
        entry = rec["entry"]
        if entry is not None and id(rec) in keepers:
            canonical = rec["canonical"]
            target = BOOTBLOCKS_DIR / f"{canonical}.bin"
            if target != path:
                if target.exists():
                    # On a case-insensitive filesystem target.exists() is true for a
                    # case-only rename of this same file; bounce through a temp name.
                    same = False
                    try:
                        same = path.samefile(target)
                    except OSError:
                        same = False
                    if same:
                        tmp = path.with_name(path.name + ".tmp_rename")
                        path.rename(tmp)
                        tmp.rename(target)
                        stats["renamed"] += 1
                    else:
                        # Canonical name occupied by a different file; leave this one put.
                        stats["collisions"] += 1
                        canonical = path.stem
                else:
                    path.rename(target)
                    stats["renamed"] += 1
            entry["File"] = f"bootblocks/{canonical}.bin"
            if not normalize_crc(entry.get("CRC")):
                entry["CRC"] = rec["crc"]
        elif entry is not None:
            # Duplicate content of a kept boot block: record under its own name.
            entries.append({
                "Name": path.stem,
                "Class": entry.get("Class", ""),
                "Notes": entry.get("Notes", ""),
                "KS": entry.get("KS", ""),
                "CRC": rec["crc"],
                "File": f"bootblocks/{path.stem}.bin",
                "Bootable": "True" if entry.get("Bootable") in (True, "True") else rec["bootable"],
                "Data": "False",
            })
        else:
            stats["created"] += 1
            entries.append({
                "Name": path.stem,
                "Class": "",
                "KS": "",
                "CRC": rec["crc"],
                "File": f"bootblocks/{path.stem}.bin",
                "Bootable": rec["bootable"],
                "Data": "False",
            })

    no_bin = sum(1 for e in entries if not e.get("File"))

    # Reorder keys for stable, readable output.
    ordered = []
    for e in entries:
        node = {k: e[k] for k in FIELD_ORDER if k in e and e[k] not in (None, "")}
        # Preserve any unexpected extra keys at the end.
        for k, v in e.items():
            if k not in node and v not in (None, ""):
                node[k] = v
        ordered.append(node)
    db["Bootblocks"] = ordered

    with DST_BRAINFILE.open("w", encoding="utf-8") as f:
        json.dump(db, f, indent=4, ensure_ascii=False)
        f.write("\n")

    print(f"Scanned {len(bin_files)} .bin files")
    print(f"  matched by CRC:   {stats['crc']}")
    print(f"  matched by recog: {stats['recog']}")
    print(f"  newly created:    {stats['created']}")
    print(f"  files renamed:    {stats['renamed']}")
    print(f"  rename collisions:{stats['collisions']}")
    print(f"Brainfile entries without a .bin: {no_bin}")
    print(f"Total entries: {len(entries)}")
    print(f"Wrote {DST_BRAINFILE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
