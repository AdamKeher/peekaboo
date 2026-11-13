import urllib.request
import re
import json
import os
import sys
import html
import hashlib

url = "https://ia803202.us.archive.org/view_archive.php?archive=/28/items/tosec-20161111-commodore-amiga/TOSEC.2016.11.11.Commodore.Amiga.AlphaBot.zip"

print(f"Fetching URL: {url}")
req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
try:
    with urllib.request.urlopen(req) as response:
        content = response.read().decode('utf-8', errors='ignore')
except Exception as e:
    print(f"Error fetching URL: {e}")
    sys.exit(1)

prefixes_dict = {}

pattern = r'(?P<prefix>TOSEC\.2016\.11\.11\.Commodore\.Amiga\.AlphaBot/(?P<folder>[^/<]+)/)'

matches = re.finditer(pattern, content)
for match in matches:
    prefix = match.group('prefix')
    folder = match.group('folder')

    category = folder
    if category.startswith("Commodore Amiga - "):
        category = category[len("Commodore Amiga - "):]
    if category.endswith(" - [ADF]"):
        category = category[:-len(" - [ADF]")]

    category = category.strip()
    category = html.unescape(category)

    if category not in prefixes_dict:
        prefixes_dict[category] = prefix

result = []
for cat, pref in sorted(prefixes_dict.items()):
    result.append({
        "Category": cat,
        "Prefix": pref
    })

print(f"Found {len(result)} distinct categories.")

script_dir = os.path.dirname(os.path.abspath(__file__))
categories_output_file = os.path.join(script_dir, 'categories.json')

with open(categories_output_file, 'w', encoding='utf-8') as f:
    json.dump(result, f, indent=4)
print(f"Saved to {categories_output_file}")


import xml.etree.ElementTree as ET
import urllib.parse

dat_file_path = os.path.join(script_dir, "..", "build", "default", "tosec", "Commodore Amiga - Latest Public DATS Combined (TOSEC-v2020-10-31).dat")
dat_file_path = os.path.abspath(dat_file_path)

print(f"Parsing DAT file: {dat_file_path}")

try:
    tree = ET.parse(dat_file_path)
    root = tree.getroot()
except Exception as e:
    print(f"Failed to parse XML dat file: {e}")
    sys.exit(1)

file_pattern = r'(?P<prefix>TOSEC\.2016\.11\.11\.Commodore\.Amiga\.AlphaBot/(?P<folder>[^/<]+)/)(?P<filename>[^<]+?\.zip)'
file_matches = re.finditer(file_pattern, content)

filename_to_data = {}

for match in file_matches:
    folder = match.group('folder')
    prefix = match.group('prefix')
    filename = match.group('filename')
    filename = html.unescape(filename)
    prefix = html.unescape(prefix)

    if filename.endswith('.zip'):
        clean_filename = filename[:-4]
    else:
        clean_filename = filename

    category = folder
    if category.startswith("Commodore Amiga - "):
        category = category[len("Commodore Amiga - "):]
    if category.endswith(" - [ADF]"):
        category = category[:-len(" - [ADF]")]

    category = category.strip()
    category = html.unescape(category)

    path_in_zip = f"{prefix}{filename}"
    download_url = f"https://archive.org/download/tosec-20161111-commodore-amiga/TOSEC.2016.11.11.Commodore.Amiga.AlphaBot.zip/{urllib.parse.quote(path_in_zip)}"

    filename_to_data[clean_filename.lower()] = {
        "Category": category,
        "Download URL": download_url
    }


# ─── TOSEC name parser ────────────────────────────────────────────────────────

# Load known hack types from hacked.json (lowercase-word prefixes of each entry)
_hacked_json_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hacked.json')
KNOWN_HACK_TYPES = []
try:
    with open(_hacked_json_path, 'r', encoding='utf-8') as _f:
        _hacked_entries = json.load(_f)
    _hack_type_set = set()
    for _entry in _hacked_entries:
        _m = re.match(r'^\[h\s+(.*?)\]$', _entry.strip())
        if _m:
            _words = _m.group(1).split()
            _type_words = []
            for _w in _words:
                if _w[0].islower():
                    _type_words.append(_w)
                else:
                    break
            if _type_words:
                _hack_type_set.add(' '.join(_type_words))
    # Sort longest first so we match "filter off" before "filter"
    KNOWN_HACK_TYPES = sorted(_hack_type_set, key=len, reverse=True)
except Exception:
    pass

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

VERSION_RE = re.compile(r'^(v\d|Rev\b|r\d\b)', re.IGNORECASE)
YEAR_RE    = re.compile(r'^(19|20)\d{2}(-\d{2}(-\d{2})?)?$')
YEAR_FUZZY = re.compile(r'^(19|20)xx$', re.IGNORECASE)
LANG_RE    = re.compile(r'^([a-z]{2}(-[a-z]{2})*|M\d+)$')
MEDIA_RE   = re.compile(r'^(Disk|Side|Vol|Part|File|Tape|Cassette|Disk Set|HD)\b', re.IGNORECASE)


def _split_type_group(content):
    """Split '[h/m content]' into (type_str, group_str).

    The modification type is described by lowercase words; the scene group
    name starts with an uppercase letter.  We scan from word index 1 onward
    so that all-uppercase type abbreviations (BB, HD, FS …) at position 0
    are kept as part of the type rather than mistaken for a group.
    """
    if not content:
        return '', ''
    words = content.split()
    for i in range(1, len(words)):
        if words[i][0].isupper():
            return ' '.join(words[:i]).strip(), ' '.join(words[i:]).strip()
    return content, ''


def _parse_trainer(content, out):
    """Populate TrainerCount / TrainerLanguage / TrainerGroup from trainer content."""
    if not content:
        return

    count_match = re.search(r'\+(\d+)', content)
    if count_match:
        out["TrainerCount"] = int(count_match.group(1))
        content = content.replace(count_match.group(0), '').strip()

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
        out["TrainerLanguage"] = ' '.join(lang_words)

    group_words = words[rest_start:]
    if group_words:
        out["TrainerGroup"] = ' '.join(group_words)


def parse_tosec_name(name):
    """Parse a TOSEC filename (without extension) into a flat metadata dict.

    Output schema (all keys always present):
        Title, Version, Demo, Year, Publisher, Language, System, Video,
        Region, Media (list),
        Verified, AlternateDump, BadDump, Overdump, VirusInfected, Fixed,
        Cracked, CrackGroup,
        Trained, TrainerCount, TrainerLanguage, TrainerGroup,
        Hacked, HackType, HackGroup,
        Modified, ModType, ModGroup
    """
    out = {
        "Title": "",
        "Version": "",
        "Demo": "",
        "Year": "",
        "Publisher": "",
        "ReleaseGroup": "",
        "Language": "",
        "System": "",
        "Video": "",
        "Region": "",
        "Media": [],
        # Dump integrity
        "Verified": False,
        "AlternateDump": False,
        "BadDump": False,
        "Overdump": False,
        "VirusInfected": False,
        "VirusName": "",
        "Fixed": False,
        # Crack
        "Cracked": False,
        "CrackGroup": "",
        # Trainer
        "Trained": False,
        "TrainerCount": 0,
        "TrainerLanguage": "",
        "TrainerGroup": "",
        # Hack (functional modification with optional group)
        "Hacked": False,
        "HackType": "",
        "HackGroup": "",
        # General modification (format change, data alteration, etc.)
        "Modified": False,
        "ModType": "",
        "ModGroup": "",
    }

    # ── Title ────────────────────────────────────────────────────────────────
    title_match = re.match(r'^([^(\[]+)', name)
    if title_match:
        out["Title"] = title_match.group(1).strip()

    # ── Collect all (...) and [...] blocks ───────────────────────────────────
    parens   = re.findall(r'\(([^)]*)\)', name)
    brackets = re.findall(r'\[([^\]]*)\]', name)

    # ── Parse parenthesised fields (positional TOSEC grammar) ────────────────
    year_found      = False
    publisher_found = False

    for p in parens:
        p = p.strip()
        if not p:
            continue

        p_up  = p.upper()
        p_low = p.lower()

        # Version: v1.2 / Rev A / r3
        if not out["Version"] and VERSION_RE.match(p):
            out["Version"] = p
            continue

        # Demo designation
        if not out["Demo"] and p_low in DEMO_STRINGS:
            out["Demo"] = p
            continue

        # Year / Date  (e.g. 1990, 1990-10, 1990-10-15, 19xx)
        if not year_found and (YEAR_RE.match(p) or YEAR_FUZZY.match(p)):
            out["Year"] = p
            year_found = True
            continue

        # Publisher — first unrecognised paren after the year
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

        # Language: en / de / en-de / M3
        if not out["Language"] and LANG_RE.match(p):
            out["Language"] = p
            continue

        # System / Hardware
        if p_up in HARDWARE_TAGS:
            out["System"] = p_up if not out["System"] else out["System"] + ", " + p_up
            continue

        # Video standard
        if p_up in VIDEO_TAGS:
            out["Video"] = p_up
            continue

        # Region
        if p in REGION_TAGS:
            out["Region"] = p
            continue

        # Media  (Disk X of Y, Side A, Vol 2, …)
        if MEDIA_RE.match(p):
            out["Media"].append(p)
            continue

        # Fallback: if we still haven't found a publisher and have no year yet,
        # treat this as the publisher (handles titles with no date field).
        if not year_found and not publisher_found and not out["Version"] and not out["Demo"]:
            out["Publisher"] = p
            publisher_found = True

    # ── Parse bracket flags ──────────────────────────────────────────────────
    for b in brackets:
        b = b.strip()
        if not b:
            continue

        if b == '!':
            out["Verified"] = True

        elif re.match(r'^a\d*$', b):
            out["AlternateDump"] = True

        elif b == 'b':
            out["BadDump"] = True

        elif b == 'o':
            out["Overdump"] = True

        elif b == 'v' or b.startswith('v '):
            out["VirusInfected"] = True
            if b.startswith('v '):
                out["VirusName"] = b[2:].strip()

        elif b == 'f' or b.startswith('f '):
            out["Fixed"] = True

        elif b == 'cr' or b.startswith('cr '):
            out["Cracked"] = True
            out["CrackGroup"] = b[2:].strip()

        elif re.match(r'^tr?\b', b):
            out["Trained"] = True
            t_content = re.sub(r'^tr?\s*', '', b).strip()
            _parse_trainer(t_content, out)

        elif b == 'h' or b.startswith('h '):
            out["Hacked"] = True
            h_content = b[1:].strip()
            if h_content:
                # Try to match the beginning against known hack types (from hacked.json).
                # If nothing matches, the entire content is a scene group name.
                matched_type = ''
                matched_group = ''
                for known_type in KNOWN_HACK_TYPES:
                    if h_content.lower().startswith(known_type):
                        matched_type = known_type
                        matched_group = h_content[len(known_type):].strip()
                        break
                if matched_type:
                    # Accumulate multiple [h] brackets with "; " separator
                    out["HackType"] = (out["HackType"] + "; " + matched_type) if out["HackType"] else matched_type
                    if matched_group and not out["HackGroup"]:
                        out["HackGroup"] = matched_group
                else:
                    # No known hack type — entire content is the group name
                    if not out["HackGroup"]:
                        out["HackGroup"] = h_content

        elif b == 'm' or b.startswith('m '):
            out["Modified"] = True
            m_content = b[1:].strip()
            m_type, m_group = _split_type_group(m_content)
            if m_type:
                out["ModType"] = (out["ModType"] + "; " + m_type) if out["ModType"] else m_type
            if m_group and not out["ModGroup"]:
                out["ModGroup"] = m_group

        # Other flags (p = pirated, etc.) are intentionally ignored for now.

    return out


# ─── Build tosec.json ─────────────────────────────────────────────────────────

games_data = []

for game in root.findall('game'):
    game_dict = {}
    game_dict['name'] = game.get('name')

    desc = game.find('description')
    if desc is not None:
        game_dict['description'] = desc.text

    roms = []
    category_assigned = None
    download_url_assigned = None

    for rom in game.findall('rom'):
        rom_name = rom.get('name')
        rom_dict = {
            'name': rom_name,
            'size': rom.get('size'),
            'crc':  rom.get('crc'),
            'md5':  rom.get('md5'),
            'sha1': rom.get('sha1'),
        }

        if rom_name:
            rom_base = os.path.splitext(rom_name)[0]
            if rom_base.lower() in filename_to_data:
                category_assigned    = filename_to_data[rom_base.lower()]["Category"]
                download_url_assigned = filename_to_data[rom_base.lower()]["Download URL"]
            elif game_dict['name'].lower() in filename_to_data:
                category_assigned    = filename_to_data[game_dict['name'].lower()]["Category"]
                download_url_assigned = filename_to_data[game_dict['name'].lower()]["Download URL"]

        roms.append(rom_dict)

    game_dict['roms'] = roms
    if category_assigned:
        game_dict['Category'] = category_assigned
    if download_url_assigned:
        game_dict['Download URL'] = download_url_assigned

    # Flat parsed metadata merged into the top-level entry
    parsed = parse_tosec_name(game_dict['name'])
    game_dict.update(parsed)

    # Title group ID: MD5 hash of the normalized title, shared across all versions of a title.
    # Using a hash keeps it a fixed-length, filename-safe identifier computed once here.
    raw_title = game_dict.get("Title", "")
    normalized_title = re.sub(r'[^a-z0-9]+', '', raw_title.lower())
    game_dict["TitleGroupId"] = hashlib.md5(normalized_title.encode()).hexdigest()

    # For certain categories, the "Publisher" is actually the scene/release group
    if category_assigned:
        cat_lower = category_assigned.lower()
        if (cat_lower.startswith("demos") or 
            cat_lower.startswith("diskmags") or 
            cat_lower.startswith("packmags") or 
            cat_lower.startswith("docs") or 
            cat_lower.startswith("compilations")):
            if game_dict.get("Publisher"):
                game_dict["ReleaseGroup"] = game_dict["Publisher"]

    games_data.append(game_dict)


tosec_output_file = os.path.join(script_dir, 'tosec.json')
with open(tosec_output_file, 'w', encoding='utf-8') as f:
    json.dump(games_data, f, indent=4)

print(f"Saved to {tosec_output_file}")
