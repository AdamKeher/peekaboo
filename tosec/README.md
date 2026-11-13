# TOSEC data

`tosec.json` is the database behind the TOSEC browser (`TosecBrowserView`) and behind
automatic disk identification (`TosecManager`, `DiskIdentifier`). It maps every known
Amiga dump's MD5/SHA1 to a title, a category, parsed TOSEC metadata and a download URL.

It is **generated** — git-ignored, and rebuilt from the DAT files in this directory by
the two scripts here. Nothing edits it by hand.

## Regenerating

```bash
python tosec/fetch_turran_index.py     # crawl the FTP -> turran_index.json  (minutes)
python tosec/build_tosec_json.py       # merge + parse  -> tosec.json, tosec.zip, categories.json
cmake --build build/default            # redeploys tosec.json into the build dir
```

The crawl is cached; pass `--refresh` to redo it. `build_tosec_json.py --no-zip` skips
repacking `tosec.zip`.

## Files

**Generated — do not edit**

| File | |
|---|---|
| `tosec.json` | The database. ~97k entries, ~161 MB. Git-ignored (shipped as `tosec.zip`). |
| `tosec.zip` | `tosec.json` compressed; this is what is tracked in git. |
| `categories.json` | Category name → entry count. Reference only; the app does not read it. |
| `turran_index.json` | Cached FTP crawl: 65 set folders, 88,858 ROM filenames. Git-ignored. |

**Inputs**

| File | |
|---|---|
| `TOSEC - Commodore Amiga - Preview Pack.zip` | 32 per-set DATs, dated 2025-03 → 2026-08. Primary source. |
| `Commodore Amiga - Latest Public DATS Combined (TOSEC-v2020-10-31).dat` | One flat DAT. Secondary source — **still required**, see below. |

**Scripts**

| File | |
|---|---|
| `fetch_turran_index.py` | Crawls the Turran FTP for categories and download URLs. |
| `build_tosec_json.py` | The DAT→JSON converter. Merges the two sources and parses TOSEC names. |
| `extract_tosec_tags.py` | Reporting helper: dumps the distinct `[m ]`/`[cr ]`/`[h ]`/`[t ]` tag values in `tosec.json`, used to hand-maintain `hacked.json` / `modified.json`. |
| `fetch_categories.py` | ⚠️ **Superseded — do not run.** The pre-2026 converter. It still works, and running it would overwrite `tosec.json` with the old archive.org-sourced data. Kept in place only so existing references resolve; the archived copy is `archive/fetch_categories.py`. Safe to delete. |

**Support data (hand-maintained)**

| File | |
|---|---|
| `hacked.json` | Known `[h ...]` hack-type prefixes. Read by the name parser to split hack *type* from scene *group*. |
| `modified.json` | Known `[m ...]` modification types. Reference for the same split. |
| `legacy_categories.json` | 249 name→category entries that neither source can supply. See "Categories" below. |


## Where the data comes from

Upstream is the Turran FTP: <https://ftp2.grandis.nu/turran/FTP/TOSEC/>

Its TOSEC root publishes Amiga DATs as **two different kinds of artifact**, and they are
not interchangeable. This is the single thing to understand before touching this data.

**"Latest Public DATS Combined (TOSEC-vYYYY-MM-DD)"** — a single flat `.dat` with every
set merged into one file, one header, and no per-set information. Five exist, 2017–2020.
Each is a complete standalone snapshot, and they are near-cumulative, so only the newest
is ever needed — they are *not* deltas and must *not* be merged with each other:

| Release | Games | Distinct MD5 | vs previous |
|---|---:|---:|---|
| 2017-04-23 | 76,408 | 79,189 | — |
| 2018-04-01 | 79,477 | 82,211 | +3,466 / −444 |
| 2018-07-27 | 80,158 | 82,970 | +759 / −0 |
| 2019-05-10 | 81,493 | 84,447 | +1,482 / −5 |
| 2020-10-31 | 83,331 | 86,397 | +1,961 / −11 |

(The small drops are TOSEC retiring bad or renamed entries.)

**"TOSEC - Commodore Amiga - Preview Pack"** — 32 separate DATs, one per set, dated
2025-03 to 2026-08. Five years newer, and each DAT names its own set in its header,
which is where categories now come from.

## How it is merged, and why

### Why both sources are needed

The Preview Pack looks like a straight replacement for the combined DAT. It is not:
it **omits ~29 whole sets** — Docs, Packmags, most Collections, Games - Emerald Mine,
Operating Systems, Games - Save Disks, and every `[EXE]` set. Using it alone drops
**11,264 MD5 hashes**, i.e. it silently *reduces* identification coverage while looking
like an upgrade.

So `build_tosec_json.py` layers them:

1. **Preview Pack** — authoritative for the 32 sets it carries (86,345 entries).
2. **2020 combined DAT** — fills only the sets the Preview Pack omits (11,079 entries).

### Deduplication is by hash, not name

Layer 2 skips any entry whose MD5s are already present. This matters because TOSEC
renames entries between releases: keying only on the game name let **8,362** dumps the
Preview Pack already carried back in under their old names, as duplicate rows with stale
titles. Hash-keyed dedup drops those; what survives is genuinely absent coverage.

### Categories

The flat combined DAT carries no category at all, so it is resolved in four tiers,
first hit wins:

1. **DAT header set name** — for Preview Pack entries, e.g. `Commodore Amiga - Games - [ADF]` → `Games - [ADF]`.
2. **Turran set folder** — which FTP folder holds that ROM zip, from `turran_index.json`.
3. **`legacy_categories.json`** — Turran has no `Operating Systems` folder and the Preview
   Pack ships no DAT for one, so those 234 entries can only inherit the category the old
   archive.org pipeline had recorded. 249 entries total.
4. **Title sibling** — every dump of a title is in the same set, so alternate dumps
   (`[a10]`, `[b]`) that have since been pulled from the server borrow from a sibling.

Result: 100.0% category coverage, against 81.9% before.

### Download URLs

The previous pipeline scraped a **2016** archive.org snapshot for both categories and
URLs, which is why every `Download URL` pointed at archive.org rather than the Turran FTP
this project sources TOSEC from, and why 18% of entries had neither. `fetch_turran_index.py`
replaces that by crawling the FTP itself and recording the exact URL-encoded filename of
every ROM zip, so URLs are constructed from files that are known to exist.

Coverage is 85.8% — the Preview Pack DATs describe more dumps than are actually uploaded.

## Result

| | Old (archive.org) | New (Turran) |
|---|---:|---:|
| Entries | 83,331 | 97,424 |
| Distinct MD5 / SHA1 | 86,397 | 100,455 |
| Category coverage | 81.9% | 100.0% |
| Download coverage | 81.9% | 85.8% |
| Download host | archive.org | ftp2.grandis.nu |

99.86% of the old hash set is retained. The 119 MD5s not carried over are dumps TOSEC
itself superseded between 2020 and 2026 — the entry still exists under the same name with
a corrected hash, so the old hash is dead rather than missing.

Category names now match TOSEC set names exactly, so `Games` became `Games - [ADF]`: the
old pipeline stripped the ` - [ADF]` suffix but kept ` - [EXE]`, which collapsed sets that
are actually distinct. Nothing persists a category string — `TosecBrowserView` rebuilds its
filter list from the loaded data each time — so the rename needs no migration.

## Gotchas

**The published zip size is not a content measure.** The Preview Pack zip (6.0 MB) is
*smaller* than the 2020 combined zip (6.3 MB) but holds *more* data — 32.6 MB of DAT text
against 30.9 MB. They were packed by different tools (the combined releases declare
made-by 3.1 with NTFS extra fields; the Preview Pack declares 6.3 with none — 7-Zip).
Repacked at identical zlib settings the Preview Pack is the larger archive, 6.88 MB
against 6.53 MB.

**FTP transfer limits.** The server allows 100 GiB / 500 files per 7 days. Directory
listings do *not* count, so the crawl is free, but ROM downloads do — it starts returning
`503` after a handful, so back off rather than retrying hard.

**Listing hrefs are HTML-escaped.** A folder like `Unofficial Addons & Patches` arrives as
`&amp;` and 404s if used raw. `fetch_turran_index.py` unescapes hrefs as well as link text.

**Two FTP layout quirks.** `Firmware/` and `Kickstart-Disks/` hold their zips directly with
no set folder; everything else is `TOSEC/<Category>/Commodore Amiga - <Set>/<game>.zip`.

**Deployment.** `CMakeLists.txt` copies only the runtime files (`tosec.json`,
`categories.json`, `hacked.json`, `modified.json`) into the build directory — not this
whole folder, which also holds the source DATs, the FTP index and the backup, several
hundred MB the app never reads. The copy is a custom target with a real `DEPENDS` on each
file rather than a `POST_BUILD` step, because `POST_BUILD` only runs when the target
relinks — a regenerated `tosec.json` would otherwise never be redeployed.

## See also

