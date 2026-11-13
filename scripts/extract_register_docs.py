#!/usr/bin/env python3
"""
Extract chip register documentation from ChipRegisterDocs.xml and convert
to markdown, producing two files:
  - assets/custom_registers_docs.json   (doc_anchor -> markdown string)
  - assets/custom_registers.json (updated with "doc_name" per register)
"""

import json
import re
import os
import sys
from html.parser import HTMLParser
from io import StringIO

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
XML_PATH = os.path.join(PROJECT_ROOT, "assets", "ChipRegisterDocs.xml")
CUSTOM_REGS_PATH = os.path.join(PROJECT_ROOT, "assets", "custom_registers.json")
OUTPUT_DOCS_PATH = os.path.join(PROJECT_ROOT, "assets", "custom_registers_docs.json")
OUTPUT_BITFIELDS_PATH = os.path.join(PROJECT_ROOT, "assets", "custom_registers_bitfields.json")


# ---------------------------------------------------------------------------
# HTML → Markdown converter (specialised for this XML structure)
# ---------------------------------------------------------------------------

class HTMLToMarkdown(HTMLParser):
    """Converts a chunk of HTML (one doc section) to markdown."""

    def __init__(self):
        super().__init__()
        self.output = StringIO()
        self._in_table = False
        self._in_thead = False
        self._in_th = False
        self._in_td = False
        self._in_tr = False
        self._in_blockquote = False
        self._in_p = False
        self._row_cells = []
        self._cell_text = ""
        self._header_row = []
        self._table_rows = []
        self._p_text = ""
        self._bq_text = ""
        self._first_table = True

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        if tag == "table":
            self._in_table = True
            self._header_row = []
            self._table_rows = []
        elif tag == "th":
            self._in_th = True
            self._cell_text = ""
        elif tag == "td":
            self._in_td = True
            self._cell_text = ""
        elif tag == "tr":
            self._in_tr = True
            self._row_cells = []
        elif tag == "blockquote":
            self._in_blockquote = True
            self._bq_text = ""
        elif tag == "p":
            self._in_p = True
            self._p_text = ""
        elif tag in ("br",):
            if self._in_td or self._in_th:
                self._cell_text += " "
            elif self._in_p:
                self._p_text += " "
            elif self._in_blockquote:
                self._bq_text += " "

    def handle_endtag(self, tag):
        tag = tag.lower()
        if tag == "table":
            self._in_table = False
            self._flush_table()
        elif tag == "th":
            self._in_th = False
            self._row_cells.append(self._cell_text.strip())
        elif tag == "td":
            self._in_td = False
            self._row_cells.append(self._cell_text.strip())
        elif tag == "tr":
            self._in_tr = False
            if any(self._in_th for _ in []):
                pass
            # Detect header row: first row with cells is the header if it came from <th>
            if not self._header_row and self._row_cells:
                # We detect header vs data by checking if this row is from <th>
                # Since we track per-cell, we use a simpler heuristic:
                # the first row in each table is the header
                if not self._table_rows:
                    self._header_row = list(self._row_cells)
                else:
                    self._table_rows.append(list(self._row_cells))
            else:
                self._table_rows.append(list(self._row_cells))
        elif tag == "blockquote":
            self._in_blockquote = False
            text = self._collapse_whitespace(self._bq_text)
            if text:
                self.output.write("\n> " + text + "\n")
        elif tag == "p":
            self._in_p = False
            text = self._collapse_whitespace(self._p_text)
            if text:
                if self._in_blockquote:
                    self._bq_text += text + " "
                else:
                    self.output.write("\n" + text + "\n")

    def handle_data(self, data):
        if self._in_th or self._in_td:
            self._cell_text += data
        elif self._in_p:
            self._p_text += data
        elif self._in_blockquote:
            self._bq_text += data

    def _flush_table(self):
        if not self._header_row:
            return

        num_cols = len(self._header_row)

        # Pad rows to same number of columns
        for row in self._table_rows:
            while len(row) < num_cols:
                row.append("")

        # Build markdown table
        header_line = "| " + " | ".join(self._header_row) + " |"
        separator = "| " + " | ".join(["---"] * num_cols) + " |"

        lines = ["\n" + header_line, separator]
        for row in self._table_rows:
            # Collapse whitespace in cells
            cleaned = [self._collapse_whitespace(c) for c in row]
            lines.append("| " + " | ".join(cleaned) + " |")

        self.output.write("\n".join(lines) + "\n")

    @staticmethod
    def _collapse_whitespace(text):
        """Collapse runs of whitespace into single spaces."""
        return re.sub(r'\s+', ' ', text).strip()

    def get_markdown(self):
        return self.output.getvalue().strip()


def html_to_markdown(html_fragment):
    """Convert an HTML fragment to markdown."""
    parser = HTMLToMarkdown()
    parser.feed(html_fragment)
    return parser.get_markdown()


# ---------------------------------------------------------------------------
# Split XML into doc sections by <a name="..."> anchors
# ---------------------------------------------------------------------------

def extract_doc_sections(xml_text):
    """
    Split the XML by <a name="..."> anchors.
    Returns dict: anchor_name -> html_fragment
    """
    # Find all anchor positions
    pattern = re.compile(r'<a\s+name="([^"]+)"\s*>\s*</a>', re.IGNORECASE)
    matches = list(pattern.finditer(xml_text))

    sections = {}
    for i, match in enumerate(matches):
        anchor_name = match.group(1).lower()
        start = match.end()
        if i + 1 < len(matches):
            # End at next anchor (or actually at the <hr> before it)
            end = matches[i + 1].start()
        else:
            # Last section: go to </body>
            body_end = xml_text.find("</body>", start)
            end = body_end if body_end != -1 else len(xml_text)

        fragment = xml_text[start:end]
        # Strip trailing <hr>, <br/> etc
        fragment = re.sub(r'[\s]*<hr[^>]*/?>[\s]*$', '', fragment, flags=re.IGNORECASE)
        fragment = re.sub(r'[\s]*<br\s*/?>[\s]*$', '', fragment, flags=re.IGNORECASE)

        sections[anchor_name] = fragment.strip()

    return sections


# ---------------------------------------------------------------------------
# Build the doc_name mapping for custom_registers.json
# ---------------------------------------------------------------------------

# Maps register name patterns to their XML doc anchor names.
# Order matters: more specific patterns should come first.

DOC_NAME_MAP = {
    # Exact matches (uppercase register name -> lowercase anchor)
    "BLTDDAT": "bltddat",
    "DMACONR": "dmaconr",
    "DMACON": "dmaconr",
    "VPOSR": "vposr",
    "VHPOSR": "vhposr",
    "DSKDATR": "dskdatr",
    "JOY0DAT": "joy0dat",
    "JOY1DAT": "joy0dat",
    "CLXDAT": "clxdat",
    "ADKCONR": "adkconr",
    "ADKCON": "adkconr",
    "POT0DAT": "pot0dat",
    "POT1DAT": "pot0dat",
    "POTINP": "potinp",
    "POTGO": "potgo",
    "SERDATR": "serdatr",
    "DSKBYTR": "dskbytr",
    "INTENAR": "intenar",
    "INTREQR": "intreqr",
    "INTENA": "intenar",
    "INTREQ": "intreqr",
    "DSKLEN": "dsklen",
    "DSKDAT": "dskdatr",
    "REFPTR": "refptr",
    "VPOSW": "vposr",
    "VHPOSW": "vhposr",
    "COPCON": "copcon",
    "SERDAT": "serdat",
    "SERPER": "serper",
    "JOYTEST": "joytest",
    "STREQU": "strequ",
    "STRVBL": "strequ",
    "STRHOR": "strequ",
    "STRLONG": "strequ",
    "BLTCON0": "bltcon0",
    "BLTCON1": "bltcon0",
    "BLTCON0L": "bltcon0",
    "BLTAFWM": "bltafwm",
    "BLTALWM": "bltafwm",
    "BLTSIZE": "bltsize",
    "BLTSIZV": "bltsizv",
    "BLTSIZH": "bltsizh",
    "DENISEID": "deniseid",
    "DSKSYNC": "dsksync",
    "COPJMP1": "copjmp",
    "COPJMP2": "copjmp",
    "COPINS": "copins",
    "DIWSTRT": "diwstrt",
    "DIWSTOP": "diwstrt",
    "DDFSTRT": "ddfstrt",
    "DDFSTOP": "ddfstrt",
    "CLXCON": "clxcon",
    "CLXCON2": "clxcon2",
    "BPLCON0": "bplcon0",
    "BPLCON1": "bplcon1",
    "BPLCON2": "bplcon2",
    "BPLCON3": "bplcon3",
    "BPLCON4": "bplcon4",
    "BPL1MOD": "bplhmod",
    "BPL2MOD": "bplhmod",
    "BEAMCON0": "beamcon0",
    "HTOTAL": "htotal",
    "HSSTOP": "hsstop",
    "HSSTRT": "hsstrt",
    "HBSTRT": "hbstrt",
    "HBSTOP": "hbstrt",
    "VTOTAL": "vtotal",
    "VSSTOP": "vtotal",
    "VSSTRT": "vsstrt",
    "VBSTRT": "vbstrt",
    "VBSTOP": "vbstrt",
    "HCENTER": "hcenter",
    "DIWHIGH": "diwhigh",
    "FMODE": "fmode",
    "HHPOSW": "hhposw",
    "HHPOSR": "hhposw",
    "SPRHSTRT": "sprhstrt",
    "SPRHSTOP": "sprhstop",
    "BPLHSTRT": "bplhstrt",
    "BPLHSTOP": "bplhstop",
    "NOOP": "noop",
}

# Pattern-based matches (regex on register name -> anchor)
DOC_PATTERN_MAP = [
    (r'^AUD\dLCH$', "audxlch"),
    (r'^AUD\dLCL$', "audxlch"),
    (r'^AUD\dLC$', "audxlch"),
    (r'^AUD\dLEN$', "audxlen"),
    (r'^AUD\dPER$', "audxper"),
    (r'^AUD\dVOL$', "audxvol"),
    (r'^AUD\dDAT$', "audxdat"),
    (r'^BPL\dPTH$', "bplxpth"),
    (r'^BPL\dPTL$', "bplxpth"),
    (r'^BPL\dPT$', "bplxpth"),
    (r'^BPL\dDAT$', "bplxdat"),
    (r'^SPR\dPTH$', "sprxpth"),
    (r'^SPR\dPTL$', "sprxpth"),
    (r'^SPR\dPT$', "sprxpth"),
    (r'^SPR\dPOS$', "sprxpos"),
    (r'^SPR\dCTL$', "sprxctl"),
    (r'^SPR\dDATA$', "sprxdata"),
    (r'^SPR\dDATB$', "sprxdata"),
    (r'^COLOR\d+$', "colorxx"),
    (r'^COP1LCH$', "cop1lch"),
    (r'^COP1LCL$', "cop1lch"),
    (r'^COP1LC$', "cop1lch"),
    (r'^COP2LCH$', "cop2lch"),
    (r'^COP2LCL$', "cop2lch"),
    (r'^COP2LC$', "cop2lch"),
    (r'^DSKPTH$', "dskpth"),
    (r'^DSKPTL$', "dskpth"),
    (r'^DSKPT$', "dskpth"),
    (r'^BLT[ABCD]PTH$', "bltxpth"),
    (r'^BLT[ABCD]PTL$', "bltxpth"),
    (r'^BLT[ABCD]PT$', "bltxpth"),
    (r'^BLT[ABCD]MOD$', "bltxmod"),
    (r'^BLT[ABCD]DAT$', "bltxdat"),
    (r'^SPRH?DAT$', "sprhdat"),
    (r'^SPRH?PTH$', "sprhpth"),
    (r'^SPRH?PTL$', "sprhpth"),
]


# ---------------------------------------------------------------------------
# Hand-authored bit-field overrides (keyed by register NAME, not doc anchor).
#
# Some registers share a single documentation section but have different bit
# layouts that the generic markdown parser cannot extract — notably BLTCON0 and
# BLTCON1, whose meaning also changes between the blitter's Area and Line modes
# (selected by bit 0 / LINE of BLTCON1). These are expressed as "variants" the
# decoder presents with a mode selector.
# ---------------------------------------------------------------------------

def _bf(hi, lo, name, desc=""):
    return {"hi": hi, "lo": lo, "name": name, "desc": desc}


BITFIELD_OVERRIDES = {
    "BLTCON0": {
        "bits": 16,
        "variants": [
            {"mode": "Area Mode", "fields": [
                _bf(15, 12, "ASH3-0", "Shift value of A source"),
                _bf(11, 11, "USEA", "Mode control bit to use source A"),
                _bf(10, 10, "USEB", "Mode control bit to use source B"),
                _bf(9, 9, "USEC", "Mode control bit to use source C"),
                _bf(8, 8, "USED", "Mode control bit to use destination D"),
                _bf(7, 0, "LF7-0", "Logic function minterm select lines"),
            ]},
            {"mode": "Line Mode", "fields": [
                _bf(15, 12, "ASH3-0", "Shift value of A source (line texture start)"),
                _bf(11, 11, "USEA", "Must be 1 in line mode"),
                _bf(10, 10, "USEB", "Must be 0 in line mode"),
                _bf(9, 9, "USEC", "Must be 1 in line mode"),
                _bf(8, 8, "USED", "Must be 1 in line mode"),
                _bf(7, 0, "LF7-0", "Logic function minterm select lines"),
            ]},
        ],
    },
    "BLTCON1": {
        "bits": 16,
        "variants": [
            {"mode": "Area Mode", "fields": [
                _bf(15, 12, "BSH3-0", "Shift value of B source and line texture"),
                _bf(7, 7, "DOFF",
                    "Disables the D output for external ALUs (hires chips only)"),
                _bf(4, 4, "EFE", "Exclusive fill enable"),
                _bf(3, 3, "IFE", "Inclusive fill enable"),
                _bf(2, 2, "FCI", "Fill carry input"),
                _bf(1, 1, "DESC", "Descending (decreasing address) control bit"),
                _bf(0, 0, "LINE", "Line mode control bit (0 = area mode)"),
            ]},
            {"mode": "Line Mode", "fields": [
                _bf(15, 12, "BSH3-0", "Shift value of B source and line texture"),
                _bf(7, 7, "DPFF", ""),
                _bf(6, 6, "SIGN", "Line draw sign flag"),
                _bf(5, 5, "OVF", "Line/draw r/l word overflow flag"),
                _bf(4, 4, "SUD", "Line draw: sometimes up or down (=AUD)"),
                _bf(3, 3, "SUL", "Line draw: sometimes up or left"),
                _bf(2, 2, "AUL", "Line draw: always up or left"),
                _bf(1, 1, "SING", "Line draw: single bit per horizontal line"),
                _bf(0, 0, "LINE", "Line mode control bit (1 = line mode)"),
            ]},
        ],
    },
}


def get_doc_name(register_name):
    """Return the doc anchor name for a given register name."""
    upper = register_name.upper()

    # Try exact match first
    if upper in DOC_NAME_MAP:
        return DOC_NAME_MAP[upper]

    # Try pattern match
    for pattern, anchor in DOC_PATTERN_MAP:
        if re.match(pattern, upper):
            return anchor

    return None


# ---------------------------------------------------------------------------
# Parse structured bit-field definitions from the generated markdown
# ---------------------------------------------------------------------------

def _split_md_row(line):
    """Split a markdown table row '| a | b | c |' into ['a','b','c']."""
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    return [c.strip() for c in line.split("|")]


def _parse_bit_cell(cell):
    """Parse a Bit-column cell into (hi, lo). Returns None if not a bit spec.

    Accepts '15', '00', '14-12', '07-00'. Hyphen ranges are hi-lo.
    """
    cell = cell.strip()
    m = re.fullmatch(r'(\d{1,2})\s*-\s*(\d{1,2})', cell)
    if m:
        a, b = int(m.group(1)), int(m.group(2))
        return (max(a, b), min(a, b))
    if re.fullmatch(r'\d{1,2}', cell):
        v = int(cell)
        return (v, v)
    return None


def parse_bitfields_from_markdown(md):
    """Extract a vertical bit-field table from a register's markdown.

    Recognises the dominant shape: a table whose header has both a 'Bit' column
    and a 'Function' column (3-col Bit|Function|Description and 4-col
    Bit|Function|Level|Description forms). Irregular tables (horizontal bit
    rows, side-by-side comparative tables) have no 'Function' header and are
    skipped, returning None.
    """
    lines = md.splitlines()
    i = 0
    while i < len(lines):
        header = lines[i]
        if header.strip().startswith("|") and i + 1 < len(lines) and \
                set(lines[i + 1].replace("|", "").replace("-", "").split()) == set():
            cols = [c.lower() for c in _split_md_row(header)]
            if "bit" in cols and "function" in cols:
                bit_idx = cols.index("bit")
                name_idx = cols.index("function")
                desc_idx = len(cols) - 1  # description is always the last column
                fields = []
                j = i + 2  # skip header + separator
                while j < len(lines) and lines[j].strip().startswith("|"):
                    cells = _split_md_row(lines[j])
                    j += 1
                    if len(cells) <= max(bit_idx, name_idx, desc_idx):
                        continue
                    rng = _parse_bit_cell(cells[bit_idx])
                    if rng is None:
                        continue
                    name = cells[name_idx].strip()
                    if not name or name.upper() == "X":
                        continue
                    desc = cells[desc_idx].strip() if desc_idx != name_idx else ""
                    fields.append({"hi": rng[0], "lo": rng[1], "name": name, "desc": desc})
                if fields:
                    bits = 32 if max(f["hi"] for f in fields) >= 16 else 16
                    return {"bits": bits, "fields": fields}
            i += 2
            continue
        i += 1
    return None


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    # 1. Read XML
    print(f"Reading {XML_PATH}...")
    with open(XML_PATH, "r", encoding="utf-8") as f:
        xml_text = f.read()

    # 2. Extract doc sections
    print("Extracting doc sections...")
    sections = extract_doc_sections(xml_text)
    print(f"Found {len(sections)} doc sections")

    # 3. Convert each section to markdown
    docs = {}
    for anchor, html_fragment in sections.items():
        md = html_to_markdown(html_fragment)
        if md:
            docs[anchor] = md

    print(f"Converted {len(docs)} sections to markdown")

    # 4. Write custom_registers_docs.json
    print(f"Writing {OUTPUT_DOCS_PATH}...")
    with open(OUTPUT_DOCS_PATH, "w", encoding="utf-8") as f:
        json.dump(docs, f, indent=2, ensure_ascii=False)

    # 4b. Parse structured bit-field definitions from the markdown and write them.
    bitfields = {}
    skipped = []
    for anchor, md in docs.items():
        parsed = parse_bitfields_from_markdown(md)
        if parsed:
            bitfields[anchor] = parsed
        else:
            skipped.append(anchor)

    # Merge hand-authored overrides (keyed by register name, e.g. BLTCON0/BLTCON1).
    bitfields.update(BITFIELD_OVERRIDES)
    print(f"Added {len(BITFIELD_OVERRIDES)} hand-authored override(s): "
          f"{', '.join(sorted(BITFIELD_OVERRIDES))}")

    print(f"Writing {OUTPUT_BITFIELDS_PATH}...")
    with open(OUTPUT_BITFIELDS_PATH, "w", encoding="utf-8") as f:
        json.dump(bitfields, f, indent=2, ensure_ascii=False)
    print(f"Parsed bit fields for {len(bitfields)} registers; "
          f"{len(skipped)} without a vertical Bit|Function table")

    # 5. Update custom_registers.json with doc_name fields
    print(f"Updating {CUSTOM_REGS_PATH} with doc_name fields...")
    with open(CUSTOM_REGS_PATH, "r", encoding="utf-8") as f:
        custom_regs = json.load(f)

    mapped = 0
    unmapped = []
    for offset, reg_data in custom_regs.items():
        reg_name = reg_data.get("name", "")
        doc_name = get_doc_name(reg_name)
        if doc_name:
            reg_data["doc_name"] = doc_name
            mapped += 1
        else:
            unmapped.append(f"  {offset}: {reg_name}")

    with open(CUSTOM_REGS_PATH, "w", encoding="utf-8") as f:
        json.dump(custom_regs, f, indent=4, ensure_ascii=False)

    print(f"Mapped {mapped} registers to doc sections")
    if unmapped:
        print(f"Unmapped registers ({len(unmapped)}):")
        for u in unmapped:
            print(u)

    # 6. Print a sample for verification
    sample_key = "bplcon0"
    if sample_key in docs:
        print(f"\n--- Sample: {sample_key} (first 500 chars) ---")
        print(docs[sample_key][:500])

    print("\nDone!")


if __name__ == "__main__":
    main()
