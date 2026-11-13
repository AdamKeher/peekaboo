#!/usr/bin/env python3
"""Build ``tosec/tosec.json`` (and ``tosec.zip``) from TOSEC DAT files.

This is the DAT-to-JSON converter for the TOSEC browser. It replaces the archive.org
scraper in ``archive/fetch_categories.py``, which derived categories and download URLs
by parsing an HTML listing of a 2016 archive.org snapshot.

Sources, in priority order
--------------------------
1. **Preview Pack** — ``TOSEC - Commodore Amiga - Preview Pack.zip``, 32 per-set DATs
   dated 2025-03 to 2026-08. Each DAT names its own set in the header, so categories
   come straight from the data instead of being scraped.
2. **Latest Public DATS Combined (TOSEC-v2020-10-31)** — one flat DAT with no per-set
   information. Used only to fill sets the Preview Pack omits: it drops ~29 sets
   (Docs, Packmags, most Collections, Emerald Mine, the [EXE] sets, Operating
   Systems), worth ~11k hashes, so taking the Preview Pack alone would *lose*
   identification coverage.

Successive "Latest Public DATS Combined" releases are each complete standalone
snapshots and are near-cumulative, so only the newest is needed. The Preview Pack is
not a snapshot of the same kind and must be merged rather than swapped in.

Categories and download URLs both come from ``turran_index.json`` where possible
(see ``fetch_turran_index.py``), which points downloads at the Turran FTP rather
than the archive.org 2016 mirror.

Usage:
    python tosec/fetch_turran_index.py      # once, to build the FTP index
    python tosec/build_tosec_json.py        # then this
    python tosec/build_tosec_json.py --no-zip
"""

import argparse
import hashlib
import io
import json
import os
import re
import sys
import urllib.parse
import xml.etree.ElementTree as ET
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

PREVIEW_ZIP = "TOSEC - Commodore Amiga - Preview Pack.zip"
LEGACY_DAT = "Commodore Amiga - Latest Public DATS Combined (TOSEC-v2020-10-31).dat"

SET_PREFIX = "Commodore Amiga - "


# ─── TOSEC name parser ────────────────────────────────────────────────────────
# Carried over from the previous pipeline (archive/fetch_categories.py) so the
# JSON schema the C++ TosecManager reads stays byte-for-byte compatible.

def _load_known_hack_types():
    """Lowercase hack-type prefixes harvested from hacked.json.

    Entries look like ``[h filter off SomeGroup]``; the type is the leading run of
    lowercase words and the scene group starts at the first capitalised word.
    """
    path = os.path.join(SCRIPT_DIR, "hacked.json")
    try:
        with open(path, "r", encoding="utf-8") as f:
            entries = json.load(f)
    except Exception:
        return []

    types = set()
    for entry in entries:
        m = re.match(r"^\[h\s+(.*?)\]$", entry.strip())
        if not m:
            continue
        type_words = []
        for word in m.group(1).split():
            if word[0].islower():
                type_words.append(word)
            else:
                break
        if type_words:
            types.add(" ".join(type_words))
    # Longest first so "filter off" matches before "filter".
    return sorted(types, key=len, reverse=True)


KNOWN_HACK_TYPES = _load_known_hack_types()

HARDWARE_TAGS = {
    "A500", "A500+", "A600", "A1000", "A1200", "A2000", "A3000", "A4000",
    "CD32", "CDTV", "OCS", "ECS", "AGA",
    "KS1.3", "KS2.0", "KS3.0", "KS3.1",
    "68000", "68020", "68030", "68040", "68060",
}

VIDEO_TAGS = {"NTSC", "PAL", "SECAM", "PAL-60"}

REGION_TAGS = {
    "US", "USA", "EU", "Europe", "Japan", "UK", "Germany", "France",
    "Spain", "Italy", "Australia", "Brazil", "Canada", "Netherlands",
    "Sweden", "Denmark", "Norway", "Finland", "Belgium", "Switzerland",
    "Portugal", "Asia", "World", "International",
}

DEMO_STRINGS = {
    "demo", "demo disk", "demo version", "rolling demo",
    "interactive demo", "playable demo", "magazine demo", "preview",
}

VERSION_RE = re.compile(r"^(v\d|Rev\b|r\d\b)", re.IGNORECASE)
YEAR_RE = re.compile(r"^(19|20)\d{2}(-\d{2}(-\d{2})?)?$")
YEAR_FUZZY = re.compile(r"^(19|20)xx$", re.IGNORECASE)
LANG_RE = re.compile(r"^([a-z]{2}(-[a-z]{2})*|M\d+)$")
MEDIA_RE = re.compile(r"^(Disk|Side|Vol|Part|File|Tape|Cassette|Disk Set|HD)\b", re.IGNORECASE)


def _split_type_group(content):
    """Split '[m content]' into (type, group) at the first capitalised word.

    Scanning starts at word index 1 so all-caps type abbreviations (BB, HD, FS)
    in first position stay part of the type rather than being read as a group.
    """
    if not content:
        return "", ""
    words = content.split()
    for i in range(1, len(words)):
        if words[i][0].isupper():
            return " ".join(words[:i]).strip(), " ".join(words[i:]).strip()
    return content, ""


def _parse_trainer(content, out):
    """Populate TrainerCount / TrainerLanguage / TrainerGroup from trainer content."""
    if not content:
        return

    count_match = re.search(r"\+(\d+)", content)
    if count_match:
        out["TrainerCount"] = int(count_match.group(1))
        content = content.replace(count_match.group(0), "").strip()

    if not content:
        return

    words = content.split()
    lang_words = []
    rest_start = 0
    for i, w in enumerate(words):
        if w[0].islower():
            lang_words.append(w)
            rest_start = i + 1
        else:
            break

    if lang_words:
        out["TrainerLanguage"] = " ".join(lang_words)

    group_words = words[rest_start:]
    if group_words:
        out["TrainerGroup"] = " ".join(group_words)


def parse_tosec_name(name):
    """Parse a TOSEC filename (without extension) into a flat metadata dict."""
    out = {
        "Title": "", "Version": "", "Demo": "", "Year": "", "Publisher": "",
        "ReleaseGroup": "", "Language": "", "System": "", "Video": "", "Region": "",
        "Media": [],
        # Dump integrity
        "Verified": False, "AlternateDump": False, "BadDump": False,
        "Overdump": False, "VirusInfected": False, "VirusName": "", "Fixed": False,
        # Crack
        "Cracked": False, "CrackGroup": "",
        # Trainer
        "Trained": False, "TrainerCount": 0, "TrainerLanguage": "", "TrainerGroup": "",
        # Hack (functional modification with optional group)
        "Hacked": False, "HackType": "", "HackGroup": "",
        # General modification (format change, data alteration, etc.)
        "Modified": False, "ModType": "", "ModGroup": "",
    }

    title_match = re.match(r"^([^(\[]+)", name)
    if title_match:
        out["Title"] = title_match.group(1).strip()

    parens = re.findall(r"\(([^)]*)\)", name)
    brackets = re.findall(r"\[([^\]]*)\]", name)

    year_found = False
    publisher_found = False

    for p in parens:
        p = p.strip()
        if not p:
            continue

        p_up = p.upper()
        p_low = p.lower()

        if not out["Version"] and VERSION_RE.match(p):
            out["Version"] = p
            continue

        if not out["Demo"] and p_low in DEMO_STRINGS:
            out["Demo"] = p
            continue

        if not year_found and (YEAR_RE.match(p) or YEAR_FUZZY.match(p)):
            out["Year"] = p
            year_found = True
            continue

        # Publisher — first unrecognised paren after the year.
        if year_found and not publisher_found:
            if (p_up not in HARDWARE_TAGS
                    and p_up not in VIDEO_TAGS
                    and p not in REGION_TAGS
                    and not MEDIA_RE.match(p)
                    and not LANG_RE.match(p)
                    and not YEAR_RE.match(p)
                    and not YEAR_FUZZY.match(p)):
                out["Publisher"] = p
                publisher_found = True
                continue

        if not out["Language"] and LANG_RE.match(p):
            out["Language"] = p
            continue

        if p_up in HARDWARE_TAGS:
            out["System"] = p_up if not out["System"] else out["System"] + ", " + p_up
            continue

        if p_up in VIDEO_TAGS:
            out["Video"] = p_up
            continue

        if p in REGION_TAGS:
            out["Region"] = p
            continue

        if MEDIA_RE.match(p):
            out["Media"].append(p)
            continue

        # Fallback: a title with no date field still names its publisher first.
        if not year_found and not publisher_found and not out["Version"] and not out["Demo"]:
            out["Publisher"] = p
            publisher_found = True

    for b in brackets:
        b = b.strip()
        if not b:
            continue

        if b == "!":
            out["Verified"] = True

        elif re.match(r"^a\d*$", b):
            out["AlternateDump"] = True

        elif b == "b" or b.startswith("b "):
            out["BadDump"] = True

        elif b == "o" or b.startswith("o "):
            out["Overdump"] = True

        elif b == "v" or b.startswith("v "):
            out["VirusInfected"] = True
            if b.startswith("v "):
                out["VirusName"] = b[2:].strip()

        elif b == "f" or b.startswith("f "):
            out["Fixed"] = True

        elif b == "cr" or b.startswith("cr "):
            out["Cracked"] = True
            out["CrackGroup"] = b[2:].strip()

        elif re.match(r"^tr?\b", b):
            out["Trained"] = True
            _parse_trainer(re.sub(r"^tr?\s*", "", b).strip(), out)

        elif b == "h" or b.startswith("h "):
            out["Hacked"] = True
            h_content = b[1:].strip()
            if h_content:
                matched_type = ""
                matched_group = ""
                for known_type in KNOWN_HACK_TYPES:
                    if h_content.lower().startswith(known_type):
                        matched_type = known_type
                        matched_group = h_content[len(known_type):].strip()
                        break
                if matched_type:
                    out["HackType"] = (out["HackType"] + "; " + matched_type) if out["HackType"] else matched_type
                    if matched_group and not out["HackGroup"]:
                        out["HackGroup"] = matched_group
                else:
                    # No known hack type — the whole content is a scene group name.
                    if not out["HackGroup"]:
                        out["HackGroup"] = h_content

        elif b == "m" or b.startswith("m ") or re.match(r"^m\d+\b", b):
            out["Modified"] = True
            m_content = re.sub(r"^m\d*\s*", "", b).strip()
            m_type, m_group = _split_type_group(m_content)
            if m_type:
                out["ModType"] = (out["ModType"] + "; " + m_type) if out["ModType"] else m_type
            if m_group and not out["ModGroup"]:
                out["ModGroup"] = m_group

    return out


# ─── DAT reading ──────────────────────────────────────────────────────────────

def read_dat(blob, source_name):
    """Parse one Logiqx DAT into (set_name, version, [game elements])."""
    root = ET.parse(io.BytesIO(blob)).getroot()
    header = root.find("header")
    set_name = header.findtext("name", "").strip() if header is not None else ""
    version = header.findtext("version", "").strip() if header is not None else ""
    if not set_name:
        set_name = source_name
    return set_name, version, root.findall("game")


def category_from_set(set_name):
    """'Commodore Amiga - Games - [ADF]' -> 'Games - [ADF]'."""
    cat = set_name
    if cat.startswith(SET_PREFIX):
        cat = cat[len(SET_PREFIX):]
    return cat.strip()


# ─── Turran FTP index ─────────────────────────────────────────────────────────

def load_turran_index():
    """Return (base_url, {game name lowered: (set name, encoded href, set path)})."""
    path = os.path.join(SCRIPT_DIR, "turran_index.json")
    if not os.path.exists(path):
        print(f"WARNING: {path} not found — entries will have no download URL.")
        print("         Run 'python tosec/fetch_turran_index.py' first.")
        return None, {}

    with open(path, "r", encoding="utf-8") as f:
        index = json.load(f)

    lookup = {}
    for set_name, info in index["sets"].items():
        for stem, href in info["files"].items():
            # First set wins; sets are distinct so collisions are rare and benign.
            lookup.setdefault(stem, (set_name, href, info["path"]))
    return index["base"], lookup


def build_download_url(base, set_path, href):
    """Join the FTP base, the set folder and the filename.

    Both parts come out of the listing already percent-encoded, so they are
    concatenated as-is; re-quoting here would double-encode every space. The one
    exception is a literal '&' (from an HTML-unescaped '&amp;' in a folder such as
    "Unofficial Addons & Patches") which is encoded so the URL survives being
    handled as a query-bearing string downstream.
    """
    return (base + set_path + href).replace("&", "%26")


# ─── Entry construction ───────────────────────────────────────────────────────

RELEASE_GROUP_CATEGORIES = ("demos", "diskmags", "packmags", "docs", "compilations")


def make_entry(game, category, turran_base, turran_lookup):
    name = game.get("name") or ""
    entry = {"name": name}

    desc = game.find("description")
    entry["description"] = desc.text if desc is not None and desc.text else name

    roms = []
    for rom in game.findall("rom"):
        roms.append({
            "name": rom.get("name"),
            "size": rom.get("size"),
            "crc": rom.get("crc"),
            "md5": rom.get("md5"),
            "sha1": rom.get("sha1"),
        })
    entry["roms"] = roms

    # Category: the DAT header set name, or the Turran set folder for entries from
    # the flat combined DAT, which carries no per-set information.
    resolved_category = category
    download_url = ""

    stems = [os.path.splitext(r["name"])[0].lower() for r in roms if r.get("name")]
    stems.append(name.lower())
    for stem in stems:
        hit = turran_lookup.get(stem)
        if hit:
            set_name, href, set_path = hit
            download_url = build_download_url(turran_base, set_path, href)
            if not resolved_category:
                resolved_category = category_from_set(set_name)
            break

    entry["Category"] = resolved_category
    entry["Download URL"] = download_url

    entry.update(parse_tosec_name(name))

    # Shared across every dump of a title, so the browser can group versions.
    normalized_title = re.sub(r"[^a-z0-9]+", "", entry.get("Title", "").lower())
    entry["TitleGroupId"] = hashlib.md5(normalized_title.encode()).hexdigest()

    # In scene categories the parenthesised name is the release group, not a publisher.
    cat_lower = resolved_category.lower()
    if any(cat_lower.startswith(p) for p in RELEASE_GROUP_CATEGORIES):
        if entry.get("Publisher"):
            entry["ReleaseGroup"] = entry["Publisher"]

    return entry


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--no-zip", action="store_true", help="skip writing tosec.zip")
    ap.add_argument("--out", default=os.path.join(SCRIPT_DIR, "tosec.json"))
    args = ap.parse_args()

    turran_base, turran_lookup = load_turran_index()
    print(f"Turran index: {len(turran_lookup)} files\n")

    entries = []
    seen = {}          # lowered game name -> index into entries
    stats = []

    # ── Layer 1: the Preview Pack, newest and per-set ────────────────────────
    preview_path = os.path.join(SCRIPT_DIR, PREVIEW_ZIP)
    if not os.path.exists(preview_path):
        sys.exit(f"ERROR: {preview_path} not found.")

    with zipfile.ZipFile(preview_path) as z:
        for member in sorted(z.namelist()):
            if not member.lower().endswith(".dat"):
                continue
            set_name, version, games = read_dat(z.read(member), member)
            category = category_from_set(set_name)
            added = 0
            for game in games:
                key = (game.get("name") or "").lower()
                if key in seen:
                    continue
                seen[key] = len(entries)
                entries.append(make_entry(game, category, turran_base, turran_lookup))
                added += 1
            stats.append((category, version, added))
            print(f"  {added:>6}  {version}  {category}")

    preview_total = len(entries)
    print(f"\nPreview Pack: {preview_total} entries from {len(stats)} sets")

    # TOSEC renames entries between releases, so name-keyed dedup alone would let a
    # dump the Preview Pack already carries back in under its old name. Hash-keyed
    # dedup drops those; what survives is genuinely absent coverage.
    preview_md5 = {r["md5"].lower() for e in entries for r in e["roms"] if r.get("md5")}

    # ── Layer 2: the 2020 combined DAT, for the sets the Preview Pack omits ──
    legacy_path = os.path.join(SCRIPT_DIR, LEGACY_DAT)
    legacy_added = 0
    legacy_renamed = 0
    if os.path.exists(legacy_path):
        with open(legacy_path, "rb") as f:
            _, legacy_version, games = read_dat(f.read(), LEGACY_DAT)
        for game in games:
            key = (game.get("name") or "").lower()
            if key in seen:
                continue
            md5s = {r.get("md5", "").lower() for r in game.findall("rom") if r.get("md5")}
            if md5s and md5s <= preview_md5:
                legacy_renamed += 1
                continue
            seen[key] = len(entries)
            # No category in this DAT; make_entry resolves it from the Turran index.
            entries.append(make_entry(game, "", turran_base, turran_lookup))
            legacy_added += 1
        print(f"Combined {legacy_version}: {legacy_added} additional entries "
              f"({legacy_renamed} skipped as renames of Preview Pack dumps)")
    else:
        print(f"WARNING: {legacy_path} not found — omitted sets will be missing.")

    # A handful of sets exist in neither source's category information: Turran has no
    # "Operating Systems" folder and the Preview Pack ships no DAT for one, so those
    # entries arrive from the combined DAT with nowhere to get a category from.
    # legacy_categories.json carries them forward from the previous pipeline, which
    # derived them from a 2016 archive.org snapshot that did have those folders.
    overrides = {}
    override_path = os.path.join(SCRIPT_DIR, "legacy_categories.json")
    if os.path.exists(override_path):
        with open(override_path, "r", encoding="utf-8") as f:
            overrides = {k.lower(): v for k, v in json.load(f).items()}
    applied = 0
    for e in entries:
        if not e["Category"]:
            hit = overrides.get(e["name"].lower())
            if hit:
                e["Category"] = hit
                applied += 1
    if applied:
        print(f"Applied {applied} categories from legacy_categories.json")

    # Entries the Turran index could not place are mostly alternate dumps ([a10],
    # [b], …) that have since been pulled from the server. Every dump of a title
    # belongs to the same set, so borrow the category from a sibling.
    by_title = {}
    for e in entries:
        if e["Category"]:
            by_title.setdefault(e["TitleGroupId"], e["Category"])
    backfilled = 0
    for e in entries:
        if not e["Category"]:
            sibling = by_title.get(e["TitleGroupId"])
            if sibling:
                e["Category"] = sibling
                backfilled += 1
                cat_lower = sibling.lower()
                if any(cat_lower.startswith(p) for p in RELEASE_GROUP_CATEGORIES):
                    if e.get("Publisher"):
                        e["ReleaseGroup"] = e["Publisher"]
    if backfilled:
        print(f"Backfilled {backfilled} categories from title siblings")

    entries.sort(key=lambda e: e["name"].lower())

    # ── Report ───────────────────────────────────────────────────────────────
    md5s = {r["md5"].lower() for e in entries for r in e["roms"] if r.get("md5")}
    with_url = sum(1 for e in entries if e["Download URL"])
    with_cat = sum(1 for e in entries if e["Category"])
    print(f"\nTotal   : {len(entries)} entries, {len(md5s)} distinct md5")
    print(f"Category: {with_cat} ({100*with_cat/len(entries):.1f}%)")
    print(f"Download: {with_url} ({100*with_url/len(entries):.1f}%)")

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(entries, f, indent=4)
    print(f"\nWrote {args.out} ({os.path.getsize(args.out):,} bytes)")

    # categories.json: the set list, for reference and for tooling.
    cats = {}
    for e in entries:
        if e["Category"]:
            cats.setdefault(e["Category"], 0)
            cats[e["Category"]] += 1
    cat_path = os.path.join(SCRIPT_DIR, "categories.json")
    with open(cat_path, "w", encoding="utf-8") as f:
        json.dump([{"Category": c, "Count": n} for c, n in sorted(cats.items())], f, indent=4)
    print(f"Wrote {cat_path} ({len(cats)} categories)")

    if not args.no_zip:
        zip_path = os.path.join(SCRIPT_DIR, "tosec.zip")
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            z.write(args.out, "tosec.json")
        print(f"Wrote {zip_path} ({os.path.getsize(zip_path):,} bytes)")


if __name__ == "__main__":
    main()
