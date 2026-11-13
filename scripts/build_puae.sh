#!/bin/bash
# Build the ADM PuAE core (macOS / Linux).
#
# Everything this script does to external/libretro-uae between extracting it and running make
# is listed, in order, in patches/series.txt - which build_puae.ps1 reads too. There is no
# inline source surgery left here: a change to what the core does is a change to a patch file
# or to a file under patches/, and it reaches every platform at once.
#
# The series holds two kinds of entry:
#   <path>    a patch file under patches/, applied with `git apply`
#   @<phase>  a named step this script implements below, for the handful of things a patch
#             cannot express (fetching a second repository, copying a whole file in, appending
#             one file to another, substituting the build version string)
set -e

# Define paths
PROJECT_ROOT=$(pwd)
EXTERNAL_DIR="$PROJECT_ROOT/external"
EMULATION_DIR="$PROJECT_ROOT/emulation"
PATCHES_DIR="$PROJECT_ROOT/patches"
SERIES_FILE="$PATCHES_DIR/series.txt"
BUILD_EMULATION_DIRS=(
    "$PROJECT_ROOT/build/default/emulation"
    "$PROJECT_ROOT/build/release/emulation"
)
REPO_URL="https://github.com/libretro/libretro-uae.git"
# Pinned upstream commit — must stay identical in build_puae.ps1 so every platform
# builds the same core. Update deliberately, then rebuild on ALL platforms.
# The patch series carries exact context against this commit, so moving the pin means
# regenerating the patches that no longer apply.
UPSTREAM_SHA="2b0ed42fe565fb997a0627aaa8f44e0948b527f8"
UPSTREAM_SHA7="${UPSTREAM_SHA:0:7}"
REPO_DIR="$EXTERNAL_DIR/libretro-uae"
# The real FloppyBridge driver sources (Greaseweazle / DrawBridge / SuperCard Pro).
# Pinned the same way and for the same reason as $UPSTREAM_SHA - must stay identical
# in build_puae.ps1. 710fa15 is upstream v1.6.0.5.
FLOPPYBRIDGE_URL="https://github.com/RobSmithDev/FloppyDriveBridge.git"
FLOPPYBRIDGE_SHA="710fa15cb200303f8c4bde1c931786175f301a68"
FLOPPYBRIDGE_SHA7="${FLOPPYBRIDGE_SHA:0:7}"
FLOPPYBRIDGE_DIR="$EXTERNAL_DIR/FloppyDriveBridge"
# WinUAE, for its Picasso96 module only. libretro-uae ships the UAE 0.8.x picasso96.c while
# everything around it is WinUAE 5.3.1, and the two do not meet - see
# patches/series/1310-picasso96-winuae-port.patch. This is the commit libretro-uae's own
# README.md says it is based on, so the module matches the tree it lands in. Pinned the same way
# and for the same reason as $UPSTREAM_SHA - must stay identical in build_puae.ps1.
WINUAE_URL="https://raw.githubusercontent.com/tonioni/WinUAE"
WINUAE_SHA="51b7be351f359dc97f967f051da390283bc4b409"
WINUAE_SHA7="${WINUAE_SHA:0:7}"
WINUAE_DIR="$EXTERNAL_DIR/winuae-picasso96"

# The ADM patch revision, reported by adm_get_version() and logged by the host on core load.
# Bump this on every rebuild so a stale DLL is visible. Keep it identical in build_puae.ps1.
ADM_VERSION="1.102.0+$UPSTREAM_SHA7"

# Ensure directories exist
mkdir -p "$EXTERNAL_DIR"
mkdir -p "$EMULATION_DIR"
for BUILD_EMULATION_DIR in "${BUILD_EMULATION_DIRS[@]}"; do
    mkdir -p "$BUILD_EMULATION_DIR"
done

# Copy a file from patches/ into the core tree, normalising CRLF to LF. On a Windows checkout
# everything under patches/ arrives with CRLF; the core tree is LF throughout, and normalising
# here is what makes the tree these scripts build byte-identical on every platform.
copy_adm_file() {
    local from="$1" to="$2"
    if [ ! -f "$from" ]; then
        echo "Error: $from is missing." >&2
        exit 1
    fi
    perl -0777 -pe 's/\r\n/\n/g' "$from" > "$to"
}

# ---------------------------------------------------------------------------
# Download and extract the core
# ---------------------------------------------------------------------------
if [ -d "$REPO_DIR" ]; then
    echo "Directory $REPO_DIR already exists. Skipping download."
else
    echo "Downloading libretro-uae @ $UPSTREAM_SHA7 from $REPO_URL..."
    ZIP_URL="https://github.com/libretro/libretro-uae/archive/${UPSTREAM_SHA}.zip"
    ZIP_FILE="$EXTERNAL_DIR/libretro-uae.zip"

    curl -L -o "$ZIP_FILE" "$ZIP_URL"

    echo "Unzipping..."
    unzip -q "$ZIP_FILE" -d "$EXTERNAL_DIR"

    # Rename the extracted folder
    mv "$EXTERNAL_DIR/libretro-uae-${UPSTREAM_SHA}" "$REPO_DIR"

    # Cleanup
    rm "$ZIP_FILE"

    # Verify the outcome, not the absence of an error: a move can fail because something still
    # holds the extracted directory, and without this the script carries on to patch nothing.
    if [ ! -f "$REPO_DIR/sources/src/newcpu.c" ]; then
        echo "Error: $REPO_DIR/sources/src/newcpu.c missing after extraction. Delete external/libretro-uae* and re-run." >&2
        exit 1
    fi

    # Ensure Makefile exists (sometimes missing from zip or failed extraction)
    if [ ! -f "$REPO_DIR/Makefile" ]; then
        echo "Makefile missing, downloading..."
        curl -L -o "$REPO_DIR/Makefile" "https://raw.githubusercontent.com/libretro/libretro-uae/${UPSTREAM_SHA}/Makefile"
    fi
fi

# ---------------------------------------------------------------------------
# Patch application
# ---------------------------------------------------------------------------
# git apply must run from the project root with --directory, NOT from inside $REPO_DIR.
# Invoked from a subdirectory of a git work tree, git prepends the cwd-relative prefix to every
# path in the patch, concludes the patch touches nothing, prints "Skipped patch '...'" — and
# exits 0.
PATCH_DIRECTORY="external/libretro-uae"

# core.autocrlf is forced off for every git apply. The project root is not itself a git work
# tree as far as `git apply --no-index` is concerned, so git falls back to the *global* config
# — and with the Windows default core.autocrlf=true it rewrites every patched file to CRLF as
# it writes it, which silently converts whole files and makes later patch context miss.
GIT_APPLY=(git -c core.autocrlf=false -c core.eol=lf apply --no-index
           --ignore-space-change --ignore-whitespace "--directory=$PATCH_DIRECTORY")

TMP_PATCH="$(mktemp)"
trap 'rm -f "$TMP_PATCH"' EXIT

# A patch that reverse-applies cleanly is, by definition, applied. Used to verify an apply, not
# to decide whether to skip one — several patches append at end-of-file, and once a later patch
# has appended after them their "\ No newline at end of file" no longer holds, so this goes
# back to answering false even though the change is there. What is applied is tracked in
# $STAMP_FILE.
patch_is_applied() {
    (cd "$PROJECT_ROOT" && "${GIT_APPLY[@]}" --check --reverse "$TMP_PATCH") >/dev/null 2>&1
}

# Records which series entries this tree already carries, one per line. It is the tree's own
# record, so re-running the script over a built tree is cheap and exact, and adding a patch to
# the end of the series picks it up without a fresh extract.
STAMP_FILE="$REPO_DIR/.adm-series-applied"
if [ ! -f "$STAMP_FILE" ] && [ -f "$REPO_DIR/libretro/libretro-core.c" ] \
   && grep -q "adm_puae_version_str" "$REPO_DIR/libretro/libretro-core.c"; then
    # Patched, but by a build script from before the series existed. Its tree is not the one the
    # series produces (line endings differ, for one), so there is nothing safe to resume.
    echo "Error: external/libretro-uae is already patched, but by an older build script." >&2
    echo "       Remove external/libretro-uae and re-run this script to rebuild it from" >&2
    echo "       the pinned upstream commit." >&2
    exit 1
fi
touch "$STAMP_FILE"

is_stamped() {
    grep -Fxq "$1" "$STAMP_FILE"
}

apply_series_patch() {
    local rel="$1"
    if is_stamped "$rel"; then
        echo "  = $rel (already applied)"
        return
    fi

    local path="$PATCHES_DIR/$rel"
    if [ ! -f "$path" ]; then
        echo "Patch file not found: $path" >&2
        exit 1
    fi

    # Normalise the patch to LF. The core tree is LF and patches are stored LF, but a Windows
    # checkout hands them over as CRLF, which breaks git apply's context matching.
    perl -0777 -pe 's/\r\n/\n/g' "$path" > "$TMP_PATCH"

    if ! (cd "$PROJECT_ROOT" && "${GIT_APPLY[@]}" "$TMP_PATCH"); then
        echo "  ! $rel FAILED to apply." >&2
        echo "    Its context does not match the tree as the earlier entries in patches/series.txt" >&2
        echo "    left it. Remove external/libretro-uae and re-run to rebuild from the pinned" >&2
        echo "    upstream commit; if it still fails, the patch needs regenerating against" >&2
        echo "    $UPSTREAM_SHA7." >&2
        exit 1
    fi

    # git apply can report success while having skipped the patch entirely, so confirm the
    # change is actually in the file before trusting it.
    if ! patch_is_applied; then
        echo "  ! $rel reported success but is not present in the tree." >&2
        echo "    Remove external/libretro-uae and re-run this script." >&2
        exit 1
    fi
    printf '%s\n' "$rel" >> "$STAMP_FILE"
    echo "  + $rel"
}

# ---------------------------------------------------------------------------
# Phases — the steps a patch file cannot express
# ---------------------------------------------------------------------------

# Fetch the pinned FloppyDriveBridge and copy its driver sources into the core tree.
#
# The vendored fork already carries the entire FloppyBridge integration behind
# #ifdef FLOPPYBRIDGE: the ADF_FLOPPYBRIDGE filetype, DRV_FB, the DMA-engine branch in
# DISK_update, floppybridge_init2()'s config parsing and the runtime hot-attach path. Upstream
# defines FLOPPYBRIDGE nowhere, so all of it compiles to nothing, and what it compiles against
# is a stub that LoadLibrary()s a separate FloppyBridge.dll nobody ships. This phase and the
# patches around it turn the define on AND replace that stub with the real drivers, compiled
# in.
#
# ADFBridge (a file-backed bridge we have no use for) and the whole windows/ GUI are
# deliberately left behind. ftdi.cpp/h is NOT optional: it is not just the DrawBridge FTDI
# adapter, SerialIO.h holds an FTDI member and every driver goes through SerialIO. It
# dynamically loads FTD2XX, so it adds no link-time dependency of its own.
phase_vendor_floppybridge() {
    local fb_src_dir="$REPO_DIR/sources/src/floppybridge"

    if [ -d "$FLOPPYBRIDGE_DIR" ]; then
        echo "  = FloppyDriveBridge already downloaded"
    else
        echo "  Downloading FloppyDriveBridge @ $FLOPPYBRIDGE_SHA7 from $FLOPPYBRIDGE_URL..."
        local fb_zip="$EXTERNAL_DIR/FloppyDriveBridge.zip"
        curl -L -o "$fb_zip" "https://github.com/RobSmithDev/FloppyDriveBridge/archive/${FLOPPYBRIDGE_SHA}.zip"
        unzip -q "$fb_zip" -d "$EXTERNAL_DIR"
        mv "$EXTERNAL_DIR/FloppyDriveBridge-${FLOPPYBRIDGE_SHA}" "$FLOPPYBRIDGE_DIR"
        rm -f "$fb_zip"

        # Verify the outcome, not the absence of an error - the same reasoning as the
        # libretro-uae extract above. A half-extracted tree here would produce a core that
        # silently lacks a driver.
        if [ ! -f "$FLOPPYBRIDGE_DIR/floppybridge/CommonBridgeTemplate.cpp" ]; then
            echo "Error: FloppyDriveBridge extract is incomplete; delete external/FloppyDriveBridge* and re-run." >&2
            exit 1
        fi
    fi

    if [ -f "$fb_src_dir/CommonBridgeTemplate.cpp" ]; then
        echo "  = FloppyBridge driver sources already vendored"
        return
    fi

    local fb_files=(
        # Shared machinery
        "CommonBridgeTemplate.cpp" "CommonBridgeTemplate.h"
        "RotationExtractor.cpp" "RotationExtractor.h"
        "SerialIO.cpp" "SerialIO.h"
        "pll.cpp" "pll.h"
        "ftdi.cpp" "ftdi.h"
        # Greaseweazle
        "GreaseWeazleBridge.cpp" "GreaseWeazleBridge.h"
        "GreaseWeazleInterface.cpp" "GreaseWeazleInterface.h"
        # DrawBridge
        "ArduinoFloppyBridge.cpp" "ArduinoFloppyBridge.h"
        "ArduinoInterface.cpp" "ArduinoInterface.h"
        # SuperCard Pro
        "SuperCardProBridge.cpp" "SuperCardProBridge.h"
        "SuperCardProInterface.cpp" "SuperCardProInterface.h"
        # Interface headers. floppybridge_abstract.h overwrites the 2021 copy libretro-uae
        # vendored: the 2024 one adds four methods the drivers declare 'override' on, so the
        # older header will not compile them.
        "floppybridge_abstract.h" "floppybridge_common.h" "floppybridge_config.h"
    )
    # libretro-uae ships this directory, so it normally already exists - but every file in it
    # is either overwritten from the vendored sources below or installed from
    # patches/floppybridge, so deleting it is a legitimate way to force a re-vendor without
    # refetching the whole core.
    mkdir -p "$fb_src_dir"
    local f
    for f in "${fb_files[@]}"; do
        if [ ! -f "$FLOPPYBRIDGE_DIR/floppybridge/$f" ]; then
            echo "Error: expected FloppyBridge source $FLOPPYBRIDGE_DIR/floppybridge/$f is missing." >&2
            exit 1
        fi
        cp -f "$FLOPPYBRIDGE_DIR/floppybridge/$f" "$fb_src_dir/$f"
    done
    # Licence texts alongside the code they cover - see PATCHES.md.
    for f in GPL2.txt MPL2.txt; do
        cp -f "$FLOPPYBRIDGE_DIR/$f" "$fb_src_dir/$f"
    done
    echo "  + FloppyBridge driver sources vendored (${#fb_files[@]} files)"
}

# Our floppybridge_lib - a static factory over the drivers, replacing upstream's DLL shim, plus
# the two ADM XCopy bridges. Copied rather than patched: these files are ours outright. disk.c
# needs no change for the replacement: the class keeps its name, base and public surface.
# Copied unconditionally so editing one and re-running picks the change up without a fresh
# extract, and LF-normalised so every platform builds the same tree.
phase_install_floppybridge_lib() {
    local fb_src_dir="$REPO_DIR/sources/src/floppybridge"
    local fb_lib_files=(
        "floppybridge_lib.h" "floppybridge_lib.cpp"
        "AdmXCopyBridge.h" "AdmXCopyBridge.cpp"
        "AdmXCopyBufferedBridge.h" "AdmXCopyBufferedBridge.cpp"
        "XCopyLink.h" "XCopyLink.cpp" "XCopyLiveProtocol.h"
    )
    local f
    for f in "${fb_lib_files[@]}"; do
        copy_adm_file "$PATCHES_DIR/floppybridge/$f" "$fb_src_dir/$f"
    done
    echo "  + ADM floppybridge_lib installed (${#fb_lib_files[@]} files)"
}

# The three chipset-debug implementations that are #included by a core source rather than
# compiled on their own - each needs the including file's statics, so none of them can be its
# own translation unit. Copied unconditionally so editing an .inc and re-running picks the
# change up without a fresh extract, the same way the version string bumps in place.
phase_install_adm_includes() {
    local f
    for f in adm_palette_vis.inc adm_chipset_blocks.inc adm_protect.inc; do
        copy_adm_file "$PATCHES_DIR/$f" "$REPO_DIR/sources/src/$f"
    done
    echo "  + adm_palette_vis.inc, adm_chipset_blocks.inc, adm_protect.inc installed"
}

# The blitter-vis implementation is appended to libretro-core.c rather than patched in: it is
# 750 lines of ours, and keeping it as a real .c file means it stays editable and diffable.
# Only the small call hooks in blitter.c / custom.c / drawing.c are patches.
phase_append_blitvis() {
    local core_file="$REPO_DIR/libretro/libretro-core.c"
    local blitvis_src="$PATCHES_DIR/adm_blitvis.c"
    if [ ! -f "$blitvis_src" ]; then
        echo "Error: patches/adm_blitvis.c not found." >&2
        exit 1
    fi
    if grep -q "adm_blitvis_record_write" "$core_file"; then
        echo "  = blitter-vis implementation already appended"
        return
    fi
    perl -0777 -pe 's/\r\n/\n/g' "$blitvis_src" >> "$core_file"
    echo "  + blitter-vis implementation appended to libretro-core.c"
}

# Fetch WinUAE's Picasso96 module and put it where libretro-uae's stale one was.
#
# libretro-uae is WinUAE 5.3.1 (its own README.md says so) everywhere except sources/src/
# picasso96.c, which is the UAE 0.8.x module with a different bank, a different state struct and
# a one-argument gfx_set_picasso_state(). Defining PICASSO96 over it does not compile. These
# three files are the ones the rest of the tree was written against, taken at the same commit:
#   od-win32/picasso96_win.cpp -> sources/src/picasso96.c
#   od-win32/picasso96_win.h   -> sources/src/include/picasso96.h
#   p96_blit.cpp               -> sources/src/p96_blit.inc  (#included 61 times to generate the
#                                                            RTG blitters; never compiled alone)
# 1310-picasso96-winuae-port.patch then adapts them, which is why the copy is skipped once
# p96_blit.inc exists: a re-run must not put the unpatched originals back under a patch the
# stamp file already records as applied.
phase_vendor_picasso96() {
    if [ -f "$REPO_DIR/sources/src/p96_blit.inc" ]; then
        echo "  = WinUAE Picasso96 module already vendored"
        return
    fi

    mkdir -p "$WINUAE_DIR"
    local pair from to cached
    for pair in \
        "od-win32/picasso96_win.cpp:sources/src/picasso96.c" \
        "od-win32/picasso96_win.h:sources/src/include/picasso96.h" \
        "p96_blit.cpp:sources/src/p96_blit.inc"
    do
        from="${pair%%:*}"
        to="${pair#*:}"
        cached="$WINUAE_DIR/$(basename "$from")"
        if [ ! -f "$cached" ]; then
            echo "  Downloading WinUAE $from @ $WINUAE_SHA7..."
            curl -L -o "$cached" "$WINUAE_URL/$WINUAE_SHA/$from"
        fi
        # Verify the outcome, not the absence of an error: a proxy or a 404 page would otherwise
        # be copied in as source and fail hundreds of lines later with nothing pointing here.
        if [ "$(wc -c < "$cached")" -lt 4096 ]; then
            echo "Error: $cached is too small to be $from. Delete external/winuae-picasso96 and re-run." >&2
            exit 1
        fi
        copy_adm_file "$cached" "$REPO_DIR/$to"
    done
    echo "  + WinUAE Picasso96 module vendored (3 files @ $WINUAE_SHA7)"
}

# The libretro side of Picasso96's platform layer - the half WinUAE keeps in win32gfx.cpp and
# direct3d.cpp. Ours outright, so it is copied rather than patched, and copied unconditionally so
# editing it and re-running picks the change up without a fresh extract.
phase_install_picasso_host() {
    local f
    for f in picasso_host.c picasso_host.h; do
        copy_adm_file "$PATCHES_DIR/picasso96/$f" "$REPO_DIR/retrodep/$f"
    done
    echo "  + retrodep/picasso_host.c, picasso_host.h installed"
}

# link.T is the linker version script that decides which symbols leave the DLL. It lives in
# patches/link.T and is copied over upstream's, so a new adm_* export is added in exactly one
# place instead of once per build script.
phase_install_link_t() {
    copy_adm_file "$PATCHES_DIR/link.T" "$REPO_DIR/libretro/link.T"
    echo "  + libretro/link.T installed"
}

# Stamp the build version. adm_get_version() is added by the patch series with a placeholder;
# this rewrites it every run so a rebuild bumps the string without a fresh extract, and the
# host can tell a stale DLL from a current one.
phase_set_version() {
    local core_file="$REPO_DIR/libretro/libretro-core.c"
    if ! grep -q "adm_puae_version_str" "$core_file"; then
        echo "Error: adm_puae_version_str not found in libretro-core.c." >&2
        exit 1
    fi
    perl -0777 -i -pe "s/static const char adm_puae_version_str\\[\\] = \"[^\"]*\";/static const char adm_puae_version_str[] = \"$ADM_VERSION\";/" "$core_file"
    echo "  + adm_get_version set to $ADM_VERSION"
}

apply_phase() {
    case "$1" in
        vendor-floppybridge)      phase_vendor_floppybridge ;;
        install-floppybridge-lib) phase_install_floppybridge_lib ;;
        install-adm-includes)     phase_install_adm_includes ;;
        vendor-picasso96)         phase_vendor_picasso96 ;;
        install-picasso-host)     phase_install_picasso_host ;;
        append-blitvis)           phase_append_blitvis ;;
        install-link-t)           phase_install_link_t ;;
        set-version)              phase_set_version ;;
        *)
            echo "Error: unknown phase '@$1' in patches/series.txt." >&2
            echo "       Add it to apply_phase in build_puae.sh AND to Invoke-Phase in build_puae.ps1." >&2
            exit 1
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Walk the series
# ---------------------------------------------------------------------------
if [ ! -f "$SERIES_FILE" ]; then
    echo "Error: $SERIES_FILE not found." >&2
    exit 1
fi

echo "Applying the patch series from patches/series.txt..."
STEP_COUNT=0
while IFS= read -r RAW_LINE || [ -n "$RAW_LINE" ]; do
    LINE="${RAW_LINE%$'\r'}"
    LINE="$(printf '%s' "$LINE" | awk '{$1=$1};1')"
    case "$LINE" in
        ""|\#*) continue ;;
    esac
    STEP_COUNT=$((STEP_COUNT + 1))
    case "$LINE" in
        @*) apply_phase "${LINE#@}" ;;
        *)  apply_series_patch "$LINE" ;;
    esac
done < "$SERIES_FILE"
echo "Patch series complete: $STEP_COUNT steps."

cd "$REPO_DIR"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
echo "Building libretro-uae..."
# Timestamp marker written before make, so the artifact can be proved new rather than merely
# present. set -e catches a failed make; it cannot catch "make did nothing and an old DLL is
# still lying here".
BUILD_MARKER="$(mktemp)"
if [[ "$OSTYPE" == "darwin"* ]]; then
    make -f Makefile -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
else
    make -f Makefile LDFLAGS="-Wl,--export-all-symbols" -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
fi

# determine extension based on OS
if [[ "$OSTYPE" == "darwin"* ]]; then
    LIB_EXT="dylib"
else
    LIB_EXT="dll" # Assuming Windows for non-mac, simplifying for now
fi

TARGET_LIB="puae_libretro.${LIB_EXT}"

if [ -f "$TARGET_LIB" ]; then
    # Existence is not enough: a DLL left over from an earlier run would satisfy it, and the copy
    # below would then distribute a stale core while reporting success.
    if [ ! "$TARGET_LIB" -nt "$BUILD_MARKER" ]; then
        rm -f "$BUILD_MARKER"
        echo "Error: $TARGET_LIB is older than this build. Nothing was rebuilt; refusing to copy a stale core." >&2
        exit 1
    fi
    rm -f "$BUILD_MARKER"
    CORE_DESTINATIONS=("$EMULATION_DIR" "${BUILD_EMULATION_DIRS[@]}")
    echo "Build successful! Copying $TARGET_LIB to all emulation directories..."
    LOCKED=0
    for DESTINATION_DIR in "${CORE_DESTINATIONS[@]}"; do
        if cp "$TARGET_LIB" "$DESTINATION_DIR/$TARGET_LIB" 2>/dev/null; then
            echo "  Copied to $DESTINATION_DIR"
        else
            # Almost always a running Peekaboo holding the library open. Say so, keep copying
            # the rest, and fail at the end — a half-deployed core must not look like success.
            echo "  LOCKED: $DESTINATION_DIR"
            LOCKED=$((LOCKED + 1))
        fi
    done
    if [ "$LOCKED" -gt 0 ]; then
        echo "Error: could not replace the core in $LOCKED directory/ies - the file is open in" >&2
        echo "       another process. Close any running Peekaboo instance, then either copy" >&2
        echo "       external/libretro-uae/$TARGET_LIB over each one by hand, or delete it and" >&2
        echo "       re-run this script (make relinks it; the patch series is already applied," >&2
        echo "       and without deleting it the staleness check above refuses)." >&2
        exit 1
    fi
    echo "Done."
else
    echo "Error: Build artifact $TARGET_LIB not found."
    exit 1
fi
