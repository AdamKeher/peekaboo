# Build the ADM PuAE core (Windows).
#
# Everything this script does to external\libretro-uae between extracting it and running make
# is listed, in order, in patches\series.txt - which build_puae.sh reads too. There is no
# inline source surgery left here: a change to what the core does is a change to a patch file
# or to a file under patches\, and it reaches every platform at once.
#
# The series holds two kinds of entry:
#   <path>    a patch file under patches\, applied with `git apply`
#   @<phase>  a named step this script implements below, for the handful of things a patch
#             cannot express (fetching a second repository, copying a whole file in, appending
#             one file to another, substituting the build version string)
#
# Set strict error handling.
# ErrorActionPreference alone is NOT enough: powershell.exe -File has been observed exiting 0
# after a terminating error, which is how a failed extract once produced "build succeeded" with
# a stale DLL still in place. The trap makes the exit code follow the failure.
$ErrorActionPreference = "Stop"
trap {
    Write-Host "BUILD FAILED: $_" -ForegroundColor Red
    Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray
    exit 1
}

# Define paths
$ProjectRoot = Get-Location
$ExternalDir = Join-Path $ProjectRoot "external"
$EmulationDir = Join-Path $ProjectRoot "emulation"
$PatchesDir = Join-Path $ProjectRoot "patches"
$SeriesFile = Join-Path $PatchesDir "series.txt"
$BuildEmulationDirs = @(
    (Join-Path $ProjectRoot "build\default\emulation"),
    (Join-Path $ProjectRoot "build\release\emulation")
)
$RepoUrl = "https://github.com/libretro/libretro-uae.git"
# Pinned upstream commit — must stay identical in build_puae.sh so every platform
# builds the same core. Update deliberately, then rebuild on ALL platforms.
# The patch series carries exact context against this commit, so moving the pin means
# regenerating the patches that no longer apply.
$UpstreamSha = "2b0ed42fe565fb997a0627aaa8f44e0948b527f8"
$UpstreamSha7 = $UpstreamSha.Substring(0, 7)
$RepoDir = Join-Path $ExternalDir "libretro-uae"
# The real FloppyBridge driver sources (Greaseweazle / DrawBridge / SuperCard Pro).
# Pinned the same way and for the same reason as $UpstreamSha - must stay identical
# in build_puae.sh. 710fa15 is upstream v1.6.0.5.
$FloppyBridgeUrl = "https://github.com/RobSmithDev/FloppyDriveBridge.git"
$FloppyBridgeSha = "710fa15cb200303f8c4bde1c931786175f301a68"
$FloppyBridgeSha7 = $FloppyBridgeSha.Substring(0, 7)
$FloppyBridgeDir = Join-Path $ExternalDir "FloppyDriveBridge"
# WinUAE, for its Picasso96 module only. libretro-uae ships the UAE 0.8.x picasso96.c while
# everything around it is WinUAE 5.3.1, and the two do not meet - see
# patches/series/1310-picasso96-winuae-port.patch. This is the commit libretro-uae's own
# README.md says it is based on, so the module matches the tree it lands in. Pinned the same way
# and for the same reason as $UpstreamSha - must stay identical in build_puae.sh.
$WinUaeUrl = "https://raw.githubusercontent.com/tonioni/WinUAE"
$WinUaeSha = "51b7be351f359dc97f967f051da390283bc4b409"
$WinUaeSha7 = $WinUaeSha.Substring(0, 7)
$WinUaeDir = Join-Path $ExternalDir "winuae-picasso96"

# The ADM patch revision, reported by adm_get_version() and logged by the host on core load.
# Bump this on every rebuild so a stale DLL is visible. Keep it identical in build_puae.sh.
$AdmVersion = "1.102.0+$UpstreamSha7"

# Ensure directories exist
if (-not (Test-Path $ExternalDir)) { New-Item -ItemType Directory -Path $ExternalDir | Out-Null }
if (-not (Test-Path $EmulationDir)) { New-Item -ItemType Directory -Path $EmulationDir | Out-Null }
foreach ($BuildEmulationDir in $BuildEmulationDirs) {
    if (-not (Test-Path $BuildEmulationDir)) {
        New-Item -ItemType Directory -Path $BuildEmulationDir -Force | Out-Null
    }
}

# ---------------------------------------------------------------------------
# Byte-exact text helpers
# ---------------------------------------------------------------------------
# Latin-1 maps every byte 0x00-0xFF to a distinct char and back, so reading and writing
# through it round-trips a file's bytes whatever its real encoding is. Get-Content /
# Set-Content do not: they decode as UTF-8 (pwsh) or ANSI (powershell.exe) and re-encode on
# write, which is how sources\src\expansion.c ended up with U+FFFD where a Latin-1 "ü" used
# to be.
$Latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Read-TextExact([string]$Path) {
    return [System.IO.File]::ReadAllText($Path, $Latin1)
}

function Write-TextExact([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, $Latin1)
}

# Copy a file from patches\ into the core tree, normalising CRLF to LF. The repository is
# checked out with core.autocrlf=true on Windows, so everything under patches\ arrives with
# CRLF; the core tree is LF throughout and mixing the two makes later patch context miss.
function Copy-AdmFile([string]$From, [string]$To) {
    if (-not (Test-Path $From)) {
        Write-Host "Error: $From is missing." -ForegroundColor Red
        exit 1
    }
    Write-TextExact $To ((Read-TextExact $From) -replace "`r`n", "`n")
}

# ---------------------------------------------------------------------------
# Download and extract the core
# ---------------------------------------------------------------------------
if (Test-Path $RepoDir) {
    Write-Host "Directory $RepoDir already exists. Skipping download."
}
else {
    Write-Host "Downloading libretro-uae @ $UpstreamSha7 from $RepoUrl..."
    $ZipUrl = "https://github.com/libretro/libretro-uae/archive/$UpstreamSha.zip"
    $ZipFile = Join-Path $ExternalDir "libretro-uae.zip"

    Invoke-WebRequest -Uri $ZipUrl -OutFile $ZipFile

    Write-Host "Unzipping..."
    Expand-Archive -Path $ZipFile -DestinationPath $ExternalDir -Force

    # Rename extracted folder
    Rename-Item -Path (Join-Path $ExternalDir "libretro-uae-$UpstreamSha") -NewName "libretro-uae"

    # Cleanup
    Remove-Item -Path $ZipFile

    # Verify the outcome, not the absence of an error. A rename can fail because something
    # still holds the extracted directory (an indexer, an editor, or a `git add` running against
    # the tree mid-build), and without this the script would carry on to patch and "build"
    # nothing.
    if (-not (Test-Path $RepoDir)) {
        Write-Host "Error: $RepoDir does not exist after extraction. The rename most likely failed because another process was holding the extracted folder - close anything reading external\ and re-run." -ForegroundColor Red
        exit 1
    }
    $SentinelSource = Join-Path $RepoDir "sources\src\newcpu.c"
    if (-not (Test-Path $SentinelSource)) {
        Write-Host "Error: $SentinelSource missing. The extracted tree is incomplete; delete external\libretro-uae* and re-run." -ForegroundColor Red
        exit 1
    }

    # Ensure Makefile exists
    $Makefile = Join-Path $RepoDir "Makefile"
    if (-not (Test-Path $Makefile)) {
        Write-Host "Makefile missing, downloading..."
        Invoke-WebRequest -Uri "https://raw.githubusercontent.com/libretro/libretro-uae/$UpstreamSha/Makefile" -OutFile $Makefile
    }
}

# ---------------------------------------------------------------------------
# Patch application
# ---------------------------------------------------------------------------
# git apply must run from the project root with --directory, NOT from inside $RepoDir.
# Invoked from a subdirectory of a git work tree, git prepends the cwd-relative prefix
# to every path in the patch, concludes the patch touches nothing, prints
# "Skipped patch '...'" — and exits 0.
$PatchDirectory = "external/libretro-uae"

# core.autocrlf is forced off for every git apply. The project root is not itself a git work
# tree as far as `git apply --no-index` is concerned, so git falls back to the *global* config
# — and with the usual Windows core.autocrlf=true it rewrites every patched file to CRLF as it
# writes it. That silently converted whole files, made later patch context miss, and left this
# script producing a different tree on Windows than build_puae.sh produced on macOS.
$GitApplyArgs = @("-c", "core.autocrlf=false", "-c", "core.eol=lf", "apply", "--no-index",
    "--ignore-space-change", "--ignore-whitespace", "--directory=$PatchDirectory")

# Run git apply and hand back its exit code. $ErrorActionPreference is dropped to Continue for
# the duration: with it left at Stop, powershell.exe turns anything a native command writes to
# stderr into a terminating error, so a --check that is *expected* to fail would abort the
# build instead of answering the question.
function Invoke-GitApply([string[]]$ExtraArgs, [switch]$Quiet) {
    $Previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        if ($Quiet) {
            & git @GitApplyArgs @ExtraArgs *> $null
        }
        else {
            & git @GitApplyArgs @ExtraArgs 2>&1 | ForEach-Object { Write-Host "    $_" }
        }
        return $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $Previous
    }
}

# Returns $true when the patch is present in the tree exactly as it was applied: a patch that
# reverse-applies cleanly is, by definition, applied. Used to verify an apply, not to decide
# whether to skip one — several patches append at end-of-file, and once a later patch has
# appended after them their "\ No newline at end of file" no longer holds, so this goes back
# to answering false even though the change is there. What is applied is tracked in $StampFile.
function Test-PatchApplied([string]$TmpPatch) {
    return ((Invoke-GitApply @("--check", "--reverse", $TmpPatch) -Quiet) -eq 0)
}

# Records which series entries this tree already carries, one per line. It is the tree's own
# record, so re-running the script over a built tree is cheap and exact, and adding a patch to
# the end of the series picks it up without a fresh extract.
$StampFile = Join-Path $RepoDir ".adm-series-applied"
$Stamped = @{}
if (Test-Path $StampFile) {
    foreach ($Entry in (Get-Content -Path $StampFile)) {
        if ($Entry.Trim() -ne "") { $Stamped[$Entry.Trim()] = $true }
    }
}
elseif ((Test-Path (Join-Path $RepoDir "libretro\libretro-core.c")) -and
        ((Read-TextExact (Join-Path $RepoDir "libretro\libretro-core.c")) -match "adm_puae_version_str")) {
    # Patched, but by a build script from before the series existed. Its tree is not the one
    # the series produces (line endings differ, for one), so there is nothing safe to resume.
    Write-Host "Error: external\libretro-uae is already patched, but by an older build script." -ForegroundColor Red
    Write-Host "       Remove external\libretro-uae and re-run this script to rebuild it from" -ForegroundColor Red
    Write-Host "       the pinned upstream commit." -ForegroundColor Red
    exit 1
}

function Invoke-SeriesPatch([string]$Rel) {
    if ($Stamped.ContainsKey($Rel)) {
        Write-Host "  = $Rel (already applied)"
        return
    }

    $Path = Join-Path $PatchesDir $Rel
    if (-not (Test-Path $Path)) {
        Write-Host "Patch file not found: $Path" -ForegroundColor Red
        exit 1
    }

    # Normalise the patch to LF. The core tree is LF and patches are stored LF, but a Windows
    # checkout hands them over as CRLF, which breaks git apply's context matching.
    $TmpPatch = Join-Path $env:TEMP ("adm-" + [System.IO.Path]::GetFileName($Rel))
    Write-TextExact $TmpPatch ((Read-TextExact $Path) -replace "`r`n", "`n")

    try {
        Push-Location $ProjectRoot

        if ((Invoke-GitApply @($TmpPatch)) -ne 0) {
            Write-Host "  ! $Rel FAILED to apply." -ForegroundColor Red
            Write-Host "    Its context does not match the tree as the earlier entries in" -ForegroundColor Red
            Write-Host "    patches\series.txt left it. Remove external\libretro-uae and re-run to" -ForegroundColor Red
            Write-Host "    rebuild from the pinned upstream commit; if it still fails, the patch" -ForegroundColor Red
            Write-Host "    needs regenerating against $UpstreamSha7." -ForegroundColor Red
            exit 1
        }

        # git apply can report success while having skipped the patch entirely, so confirm the
        # change is actually in the file before trusting it.
        if (-not (Test-PatchApplied $TmpPatch)) {
            Write-Host "  ! $Rel reported success but is not present in the tree." -ForegroundColor Red
            Write-Host "    Remove external\libretro-uae and re-run this script." -ForegroundColor Red
            exit 1
        }
        [System.IO.File]::AppendAllText($StampFile, "$Rel`n", $Latin1)
        $Stamped[$Rel] = $true
        Write-Host "  + $Rel"
    }
    finally {
        Pop-Location
        Remove-Item -Path $TmpPatch -ErrorAction SilentlyContinue
    }
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
# dynamically LoadLibrary()s FTD2XX.DLL, so it adds no link-time dependency of its own.
function Invoke-PhaseVendorFloppyBridge {
    $FbSrcDir = Join-Path $RepoDir "sources\src\floppybridge"

    if (Test-Path $FloppyBridgeDir) {
        Write-Host "  = FloppyDriveBridge already downloaded"
    }
    else {
        Write-Host "  Downloading FloppyDriveBridge @ $FloppyBridgeSha7 from $FloppyBridgeUrl..."
        $FbZipUrl = "https://github.com/RobSmithDev/FloppyDriveBridge/archive/$FloppyBridgeSha.zip"
        $FbZipFile = Join-Path $ExternalDir "FloppyDriveBridge.zip"

        Invoke-WebRequest -Uri $FbZipUrl -OutFile $FbZipFile
        Expand-Archive -Path $FbZipFile -DestinationPath $ExternalDir -Force
        Rename-Item -Path (Join-Path $ExternalDir "FloppyDriveBridge-$FloppyBridgeSha") -NewName "FloppyDriveBridge"
        Remove-Item -Path $FbZipFile

        # Verify the outcome, not the absence of an error - the same reasoning as the
        # libretro-uae extract above. A half-extracted tree here would produce a core that
        # silently lacks a driver.
        if (-not (Test-Path $FloppyBridgeDir)) {
            Write-Host "Error: $FloppyBridgeDir does not exist after extraction. The rename most likely failed because another process was holding the extracted folder - close anything reading external\ and re-run." -ForegroundColor Red
            exit 1
        }
        $FbSentinel = Join-Path $FloppyBridgeDir "floppybridge\CommonBridgeTemplate.cpp"
        if (-not (Test-Path $FbSentinel)) {
            Write-Host "Error: $FbSentinel missing. The extracted tree is incomplete; delete external\FloppyDriveBridge* and re-run." -ForegroundColor Red
            exit 1
        }
    }

    $FbVendorSentinel = Join-Path $FbSrcDir "CommonBridgeTemplate.cpp"
    if (Test-Path $FbVendorSentinel) {
        Write-Host "  = FloppyBridge driver sources already vendored"
        return
    }

    $FbFiles = @(
        # Shared machinery
        "CommonBridgeTemplate.cpp", "CommonBridgeTemplate.h",
        "RotationExtractor.cpp", "RotationExtractor.h",
        "SerialIO.cpp", "SerialIO.h",
        "pll.cpp", "pll.h",
        "ftdi.cpp", "ftdi.h",
        # Greaseweazle
        "GreaseWeazleBridge.cpp", "GreaseWeazleBridge.h",
        "GreaseWeazleInterface.cpp", "GreaseWeazleInterface.h",
        # DrawBridge
        "ArduinoFloppyBridge.cpp", "ArduinoFloppyBridge.h",
        "ArduinoInterface.cpp", "ArduinoInterface.h",
        # SuperCard Pro
        "SuperCardProBridge.cpp", "SuperCardProBridge.h",
        "SuperCardProInterface.cpp", "SuperCardProInterface.h",
        # Interface headers. floppybridge_abstract.h overwrites the 2021 copy
        # libretro-uae vendored: the 2024 one adds four methods the drivers
        # declare 'override' on, so the older header will not compile them.
        "floppybridge_abstract.h", "floppybridge_common.h", "floppybridge_config.h"
    )
    # libretro-uae ships this directory, so it normally already exists - but every file in it
    # is either overwritten from the vendored sources below or installed from
    # patches\floppybridge, so deleting it is a legitimate way to force a re-vendor without
    # refetching the whole core.
    if (-not (Test-Path $FbSrcDir)) {
        New-Item -ItemType Directory -Path $FbSrcDir -Force | Out-Null
    }
    foreach ($FbFile in $FbFiles) {
        $FbFrom = Join-Path $FloppyBridgeDir "floppybridge\$FbFile"
        if (-not (Test-Path $FbFrom)) {
            Write-Host "Error: expected FloppyBridge source $FbFrom is missing." -ForegroundColor Red
            exit 1
        }
        Copy-Item -Path $FbFrom -Destination (Join-Path $FbSrcDir $FbFile) -Force
    }
    # Licence texts alongside the code they cover - see PATCHES.md.
    foreach ($FbLicence in @("GPL2.txt", "MPL2.txt")) {
        Copy-Item -Path (Join-Path $FloppyBridgeDir $FbLicence) -Destination (Join-Path $FbSrcDir $FbLicence) -Force
    }
    Write-Host "  + FloppyBridge driver sources vendored ($($FbFiles.Count) files)"
}

# Our floppybridge_lib - a static factory over the drivers, replacing upstream's DLL shim, plus
# the two ADM XCopy bridges. Copied rather than patched: these files are ours outright. disk.c
# needs no change for the replacement: the class keeps its name, base and public surface.
# Copied unconditionally so editing one and re-running picks the change up without a fresh
# extract, and LF-normalised so the tree these scripts build is byte-identical on every
# platform (a Windows checkout hands these over as CRLF, a macOS one as LF).
function Invoke-PhaseInstallFloppyBridgeLib {
    $FbSrcDir = Join-Path $RepoDir "sources\src\floppybridge"
    $FbLibFiles = @("floppybridge_lib.h", "floppybridge_lib.cpp",
        "AdmXCopyBridge.h", "AdmXCopyBridge.cpp",
        "AdmXCopyBufferedBridge.h", "AdmXCopyBufferedBridge.cpp",
        "XCopyLink.h", "XCopyLink.cpp", "XCopyLiveProtocol.h")
    foreach ($FbLib in $FbLibFiles) {
        Copy-AdmFile (Join-Path $PatchesDir "floppybridge\$FbLib") (Join-Path $FbSrcDir $FbLib)
    }
    Write-Host "  + ADM floppybridge_lib installed ($($FbLibFiles.Count) files)"
}

# The three chipset-debug implementations that are #included by a core source rather than
# compiled on their own - each needs the including file's statics, so none of them can be its
# own translation unit. Copied unconditionally so editing an .inc and re-running picks the
# change up without a fresh extract, the same way the version string bumps in place.
function Invoke-PhaseInstallAdmIncludes {
    foreach ($Inc in @("adm_palette_vis.inc", "adm_chipset_blocks.inc", "adm_protect.inc")) {
        Copy-AdmFile (Join-Path $PatchesDir $Inc) (Join-Path $RepoDir "sources\src\$Inc")
    }
    Write-Host "  + adm_palette_vis.inc, adm_chipset_blocks.inc, adm_protect.inc installed"
}

# The blitter-vis implementation is appended to libretro-core.c rather than patched in: it is
# 750 lines of ours, and keeping it as a real .c file means it stays editable and diffable.
# Only the small call hooks in blitter.c / custom.c / drawing.c are patches.
function Invoke-PhaseAppendBlitVis {
    $CoreFile = Join-Path $RepoDir "libretro\libretro-core.c"
    $BlitVisSrc = Join-Path $PatchesDir "adm_blitvis.c"
    if (-not (Test-Path $BlitVisSrc)) {
        Write-Host "Error: patches\adm_blitvis.c not found." -ForegroundColor Red
        exit 1
    }
    if ((Read-TextExact $CoreFile) -match "adm_blitvis_record_write") {
        Write-Host "  = blitter-vis implementation already appended"
        return
    }
    $BlitVis = (Read-TextExact $BlitVisSrc) -replace "`r`n", "`n"
    Write-TextExact $CoreFile ((Read-TextExact $CoreFile) + $BlitVis)
    Write-Host "  + blitter-vis implementation appended to libretro-core.c"
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
function Invoke-PhaseVendorPicasso96 {
    $BlitTarget = Join-Path $RepoDir "sources\src\p96_blit.inc"
    if (Test-Path $BlitTarget) {
        Write-Host "  = WinUAE Picasso96 module already vendored"
        return
    }

    $Files = @(
        @{ From = "od-win32/picasso96_win.cpp"; To = "sources\src\picasso96.c" },
        @{ From = "od-win32/picasso96_win.h"; To = "sources\src\include\picasso96.h" },
        @{ From = "p96_blit.cpp"; To = "sources\src\p96_blit.inc" }
    )
    if (-not (Test-Path $WinUaeDir)) { New-Item -ItemType Directory -Path $WinUaeDir | Out-Null }

    foreach ($File in $Files) {
        $Cached = Join-Path $WinUaeDir ([System.IO.Path]::GetFileName($File.From))
        if (-not (Test-Path $Cached)) {
            Write-Host "  Downloading WinUAE $($File.From) @ $WinUaeSha7..."
            Invoke-WebRequest -Uri "$WinUaeUrl/$WinUaeSha/$($File.From)" -OutFile $Cached
        }
        # Verify the outcome, not the absence of an error: a proxy or a 404 page would otherwise
        # be copied in as source and fail hundreds of lines later with nothing pointing here.
        if ((Get-Item $Cached).Length -lt 4096) {
            Write-Host "Error: $Cached is too small to be $($File.From). Delete external\winuae-picasso96 and re-run." -ForegroundColor Red
            exit 1
        }
        Copy-AdmFile $Cached (Join-Path $RepoDir $File.To)
    }
    Write-Host "  + WinUAE Picasso96 module vendored ($($Files.Count) files @ $WinUaeSha7)"
}

# The libretro side of Picasso96's platform layer - the half WinUAE keeps in win32gfx.cpp and
# direct3d.cpp. Ours outright, so it is copied rather than patched, and copied unconditionally so
# editing it and re-running picks the change up without a fresh extract.
function Invoke-PhaseInstallPicassoHost {
    foreach ($File in @("picasso_host.c", "picasso_host.h")) {
        Copy-AdmFile (Join-Path $PatchesDir "picasso96\$File") (Join-Path $RepoDir "retrodep\$File")
    }
    Write-Host "  + retrodep\picasso_host.c, picasso_host.h installed"
}

# link.T is the linker version script that decides which symbols leave the DLL. It lives in
# patches\link.T and is copied over upstream's, so a new adm_* export is added in exactly one
# place instead of once per build script.
function Invoke-PhaseInstallLinkT {
    Copy-AdmFile (Join-Path $PatchesDir "link.T") (Join-Path $RepoDir "libretro\link.T")
    Write-Host "  + libretro\link.T installed"
}

# Stamp the build version. adm_get_version() is added by the patch series with a placeholder;
# this rewrites it every run so a rebuild bumps the string without a fresh extract, and the
# host can tell a stale DLL from a current one.
function Invoke-PhaseSetVersion {
    $CoreFile = Join-Path $RepoDir "libretro\libretro-core.c"
    $Text = Read-TextExact $CoreFile
    if ($Text -notmatch 'static const char adm_puae_version_str\[\] = "[^"]*";') {
        Write-Host "Error: adm_puae_version_str not found in libretro-core.c." -ForegroundColor Red
        exit 1
    }
    $Text = $Text -replace 'static const char adm_puae_version_str\[\] = "[^"]*";',
        "static const char adm_puae_version_str[] = `"$AdmVersion`";"
    Write-TextExact $CoreFile $Text
    Write-Host "  + adm_get_version set to $AdmVersion"
}

function Invoke-Phase([string]$Name) {
    switch ($Name) {
        "vendor-floppybridge" { Invoke-PhaseVendorFloppyBridge }
        "install-floppybridge-lib" { Invoke-PhaseInstallFloppyBridgeLib }
        "install-adm-includes" { Invoke-PhaseInstallAdmIncludes }
        "vendor-picasso96" { Invoke-PhaseVendorPicasso96 }
        "install-picasso-host" { Invoke-PhaseInstallPicassoHost }
        "append-blitvis" { Invoke-PhaseAppendBlitVis }
        "install-link-t" { Invoke-PhaseInstallLinkT }
        "set-version" { Invoke-PhaseSetVersion }
        default {
            Write-Host "Error: unknown phase '@$Name' in patches\series.txt." -ForegroundColor Red
            Write-Host "       Add it to Invoke-Phase in build_puae.ps1 AND to apply_phase in build_puae.sh." -ForegroundColor Red
            exit 1
        }
    }
}

# ---------------------------------------------------------------------------
# Walk the series
# ---------------------------------------------------------------------------
if (-not (Test-Path $SeriesFile)) {
    Write-Host "Error: $SeriesFile not found." -ForegroundColor Red
    exit 1
}

Write-Host "Applying the patch series from patches\series.txt..."
$StepCount = 0
foreach ($RawLine in Get-Content -Path $SeriesFile) {
    $Line = $RawLine.Trim()
    if ($Line -eq "" -or $Line.StartsWith("#")) { continue }
    $StepCount++
    if ($Line.StartsWith("@")) {
        Invoke-Phase $Line.Substring(1)
    }
    else {
        Invoke-SeriesPatch $Line
    }
}
Write-Host "Patch series complete: $StepCount steps."

Set-Location $RepoDir

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
Write-Host "Building libretro-uae..."
# Timestamped before make so the artifact can be proved new rather than merely present.
$BuildStartedAt = Get-Date
# Assuming 'make' is available (MinGW32-make or similar)
make -f Makefile platform=wincross64 LDFLAGS="-Wl,--export-all-symbols" -j($env:NUMBER_OF_PROCESSORS)
# A native command does not raise a PowerShell error, so ErrorActionPreference never sees a
# failed compile. Without this check a broken build falls through to the copy below.
if ($LASTEXITCODE -ne 0) {
    Write-Host "Error: make failed with exit code $LASTEXITCODE." -ForegroundColor Red
    exit 1
}

# Verify artifact
$TargetLib = "puae_libretro.dll"
if (Test-Path $TargetLib) {
    # Existence is not enough: a DLL left over from an earlier run would satisfy it, and the
    # copy below would then distribute a stale core while reporting success. Require that this
    # run actually wrote it.
    $ArtifactWrittenAt = (Get-Item $TargetLib).LastWriteTime
    if ($ArtifactWrittenAt -lt $BuildStartedAt) {
        Write-Host "Error: $TargetLib was last written at $ArtifactWrittenAt, before this build started at $BuildStartedAt. Nothing was rebuilt; refusing to copy a stale core." -ForegroundColor Red
        exit 1
    }
    $CoreDestinations = @($EmulationDir) + $BuildEmulationDirs
    Write-Host "Build successful! Copying $TargetLib to all emulation directories..."
    $Locked = @()
    foreach ($DestinationDir in $CoreDestinations) {
        try {
            Copy-Item -Path $TargetLib -Destination (Join-Path $DestinationDir $TargetLib) -Force
            Write-Host "  Copied to $DestinationDir"
        }
        catch {
            # Almost always a running Peekaboo holding the DLL open. Say so, keep copying the
            # rest, and fail at the end — a half-deployed core must not look like success.
            Write-Host "  LOCKED: $DestinationDir" -ForegroundColor Yellow
            $Locked += $DestinationDir
        }
    }
    if ($Locked.Count -gt 0) {
        Write-Host "Error: could not replace the core in $($Locked.Count) directory/ies - the file is" -ForegroundColor Red
        Write-Host "       open in another process. Close any running Peekaboo instance, then either" -ForegroundColor Red
        Write-Host "       copy external\libretro-uae\$TargetLib over each one by hand, or delete it" -ForegroundColor Red
        Write-Host "       and re-run this script (make relinks it; the patch series is already" -ForegroundColor Red
        Write-Host "       applied, and without deleting it the staleness check below refuses)." -ForegroundColor Red
        exit 1
    }
    Write-Host "Done."
}
else {
    Write-Host "Error: Build artifact $TargetLib not found." -ForegroundColor Red
    exit 1
}

Set-Location $ProjectRoot
