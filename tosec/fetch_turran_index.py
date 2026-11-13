#!/usr/bin/env python3
"""Crawl the Turran FTP TOSEC tree and cache an index of every published ROM zip.

The index maps a TOSEC game name to the set folder that holds it and to the exact
URL-encoded filename on the server, which gives ``build_tosec_json.py`` two things
the DAT files cannot supply on their own:

  * the **category** for entries that come from the flat "Latest Public DATS
    Combined" DAT, which carries no per-set information at all, and
  * a **download URL on Turran** rather than the archive.org 2016 snapshot the
    previous pipeline scraped.

Only directory listings are fetched (no ROM downloads), and the server does not
count those against its 7-day transfer limit.

Output: ``tosec/turran_index.json`` (git-ignored; regenerate as needed).

Usage:
    python tosec/fetch_turran_index.py            # full crawl
    python tosec/fetch_turran_index.py --refresh  # ignore any existing cache
"""

import argparse
import html
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://ftp2.grandis.nu/turran/FTP/TOSEC/"
USER_AGENT = "Mozilla/5.0 (peekaboo tosec indexer)"

# Anchor rows in the Apache-style listing: href is URL-encoded, the text is not.
ANCHOR_RE = re.compile(r'<a href="([^"?][^"]*)">([^<]*)</a>')

# Sort links (?C=N;O=D) and the parent link are the only non-content anchors.
SKIP_HREFS = ("/", "..")


def fetch(url, retries=5, delay=3.0):
    """GET a directory listing, retrying through the server's 503 rate limiting."""
    last = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=120) as resp:
                return resp.read().decode("utf-8", errors="replace")
        except (urllib.error.HTTPError, urllib.error.URLError, OSError) as exc:
            last = exc
            if attempt < retries - 1:
                time.sleep(delay * (attempt + 1))
    raise RuntimeError(f"failed to fetch {url}: {last}")


def list_dir(url):
    """Return (subdirs, files) for a listing URL.

    Each entry is a ``(href, display_name)`` pair; ``href`` stays URL-encoded so it
    can be pasted straight back into a download URL.
    """
    body = fetch(url)
    subdirs, files = [], []
    for href, text in ANCHOR_RE.findall(body):
        # The listing HTML-escapes hrefs too, so "&" arrives as "&amp;" and would
        # 404 if pasted straight into a request.
        href = html.unescape(href)
        if href.startswith(SKIP_HREFS) or href.startswith("http"):
            continue
        name = html.unescape(text).strip()
        if href.endswith("/"):
            subdirs.append((href, name.rstrip("/")))
        else:
            files.append((href, name))
    return subdirs, files


def crawl(refresh=False):
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.join(script_dir, "turran_index.json")

    if os.path.exists(out_path) and not refresh:
        print(f"{out_path} already exists; pass --refresh to re-crawl.")
        return out_path

    print(f"Crawling {BASE}")
    top_dirs, top_files = list_dir(BASE)
    print(f"  {len(top_dirs)} top-level categories, {len(top_files)} loose files")

    sets = {}
    total_files = 0

    for top_href, top_name in top_dirs:
        top_url = urllib.parse.urljoin(BASE, top_href)
        sub_dirs, sub_files = list_dir(top_url)

        # Firmware/ and Kickstart-Disks/ hold their zips directly, with no set folder.
        if sub_files and not sub_dirs:
            key = f"Commodore Amiga - {top_name}"
            sets[key] = {
                "top": top_name,
                "path": top_href,
                "files": {os.path.splitext(n)[0].lower(): h for h, n in sub_files},
            }
            total_files += len(sub_files)
            print(f"  {top_name:<20} (flat)  {len(sub_files):>6} files")
            continue

        for set_href, set_name in sub_dirs:
            set_url = urllib.parse.urljoin(top_url, set_href)
            try:
                _, set_files = list_dir(set_url)
            except RuntimeError as exc:
                # One unreachable folder must not discard the whole crawl.
                print(f"  !! skipping {set_name}: {exc}")
                continue
            if not set_files:
                continue
            sets[set_name] = {
                "top": top_name,
                "path": top_href + set_href,
                "files": {os.path.splitext(n)[0].lower(): h for h, n in set_files},
            }
            total_files += len(set_files)
            print(f"  {set_name:<58} {len(set_files):>6} files")

    index = {
        "base": BASE,
        "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
        "set_count": len(sets),
        "file_count": total_files,
        "sets": sets,
    }

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(index, f, indent=1)

    print(f"\n{len(sets)} sets, {total_files} files -> {out_path}")
    return out_path


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--refresh", action="store_true", help="re-crawl even if a cache exists")
    args = ap.parse_args()
    try:
        crawl(refresh=args.refresh)
    except KeyboardInterrupt:
        sys.exit(130)
