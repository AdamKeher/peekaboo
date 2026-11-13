<#
.SYNOPSIS
    Packages a built Peekaboo into a self-contained folder + zip that runs on another Windows PC.

.DESCRIPTION
    Copies the executable, MinGW runtime DLLs, the patched PuAE core, Kickstart ROMs, all the
    runtime data folders, the workspaces, the licences, and a set of Workbench floppies /
    hardfiles / hard-drive directories from Amiga Forever's Shared folder (-Public: none of the
    ROMs or Workbench; -Private: the bundled Python and Exocet's tools as well). -Sign signs the
    exe and the core with Authenticode before zipping. The
    user_prefs.json is a sanitised copy of the build's: machine-specific paths, recent files, the
    MCP token and window geometry are reset, and the hard-drive mounts point at the bundled media
    with paths relative to the exe (the app sets its working directory to the exe folder at start).

    Rerun after any rebuild. Nothing in the repo or build folder is modified.

.EXAMPLE
    .\scripts\package_peekaboo.ps1
    .\scripts\package_peekaboo.ps1 -BuildDir build\default -NoTosec
    .\scripts\package_peekaboo.ps1 -AmigaFilesDir "D:\Amiga Files\Shared" -NoZip
    .\scripts\package_peekaboo.ps1 -NoHardfiles
    .\scripts\package_peekaboo.ps1 -Public -Sign -CertThumbprint 0123...ABCD
#>
[CmdletBinding()]
param(
    # Build to package, relative to the repo root.
    [string]$BuildDir = "build\release",
    # Where the staging folder and the zip are written, relative to the repo root.
    [string]$OutDir = "dist",
    # Amiga Forever's Shared folder (adf\, hdf\, dir\, rom\). Default: $env:PEEKABOO_AMIGA_FILES,
    # else Amiga Forever's own place in Public or the user's Documents. A package other than
    # -Public stops if none is there, rather than ship without Workbench.
    [string]$AmigaFilesDir = "",
    # Leave out the bundled Python (138 MB; only the Exocet sync tracker needs it).
    [switch]$NoPython,
    # Leave out the TOSEC database (161 MB; only the TOSEC browser / identify needs it).
    [switch]$NoTosec,
    # Also ship the build's save states (states\*.pkst). Off by default: they are dev sessions.
    [switch]$IncludeStates,
    # Leave out the Workbench .hdf hardfiles (media\hdf). The host-directory drives in harddisks\
    # are still shipped, so a hard-drive boot keeps working.
    [switch]$NoHardfiles,
    # Stage the folder but do not zip it.
    [switch]$NoZip,
    # A download for the public: no Kickstart ROMs, no Workbench media (Amiga Forever's or the
    # repo's harddisks\) and no hard drives configured. Those are copyrighted; users bring their own,
    # and the setup wizard helps them find them. emulation\ holds only the core and its own files,
    # and the package fails if anything ROM-, key-, hardfile- or Workbench-like is staged.
    [switch]$Public,
    # Adam's own build: Exocet and the crAss engine's tools shown (privateFeatures in the prefs),
    # with the bundled Python and exocet-tools\ they need. Without it neither ships and the prefs
    # hide them.
    [switch]$Private,
    # Sign peekaboo.exe and emulation\puae_libretro.dll in the staged folder (the build's
    # own copies are left unsigned) with signtool from the Windows SDK. The certificate is either
    # one in the Windows certificate store, by thumbprint - where a hardware token's or Certum
    # SimplySign's appears - or a .pfx file (a test certificate).
    [switch]$Sign,
    [string]$CertThumbprint = $env:PEEKABOO_SIGN_THUMBPRINT,
    [string]$PfxFile = "",
    [string]$PfxPassword = "",
    # RFC 3161 timestamp server, so the signature outlives the certificate. "" to leave it out.
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = (Resolve-Path (Join-Path $repo $BuildDir)).Path
$exeName = "peekaboo.exe"

. (Join-Path $PSScriptRoot "PeekabooSystemFiles.ps1")

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Warn($msg) { Write-Host "WARNING: $msg" -ForegroundColor Yellow }

# Copies a directory tree. robocopy exit codes below 8 are success.
function Copy-Tree([string]$src, [string]$dst, [string[]]$excludeFiles = @(),
                   [string[]]$excludeDirs = @()) {
    if (-not (Test-Path $src)) { Warn "missing, skipped: $src"; return }
    $rcArgs = @($src, $dst, "/E", "/NFL", "/NDL", "/NJH", "/NJS", "/NP", "/R:1", "/W:1")
    if ($excludeFiles) { $rcArgs += "/XF"; $rcArgs += $excludeFiles }
    if ($excludeDirs) { $rcArgs += "/XD"; $rcArgs += $excludeDirs }
    & robocopy @rcArgs | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE): $src -> $dst" }
    $global:LASTEXITCODE = 0
}

function Copy-One([string]$src, [string]$dstDir, [string]$newName = "") {
    if (-not (Test-Path $src)) { Warn "missing, skipped: $src"; return }
    New-Item -ItemType Directory -Force $dstDir | Out-Null
    $name = if ($newName) { $newName } else { Split-Path $src -Leaf }
    Copy-Item -LiteralPath $src -Destination (Join-Path $dstDir $name) -Force
}

# The script runs under Windows PowerShell 5.1 as well as 7, so no utf8NoBOM encoding and no
# ConvertFrom-Json -AsHashtable - both are 7-only.
function Write-Utf8([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding $false))
}

# ConvertFrom-Json output (PSCustomObject) as ordered dictionaries, so keys can be set freely.
function ConvertTo-Dictionary($value) {
    if ($value -is [Management.Automation.PSCustomObject]) {
        $d = [ordered]@{}
        foreach ($prop in $value.PSObject.Properties) { $d[$prop.Name] = ConvertTo-Dictionary $prop.Value }
        return $d
    }
    if ($value -is [Array]) {
        # The leading comma stops PowerShell unrolling a one-element or empty array.
        return , @($value | ForEach-Object { ConvertTo-Dictionary $_ })
    }
    return $value
}

# --- Preconditions --------------------------------------------------------------------------

if (-not (Test-Path (Join-Path $build $exeName))) { throw "no $exeName in $build - build first" }
if (-not (Test-Path (Join-Path $build "emulation\puae_libretro.dll"))) {
    throw "no emulation\puae_libretro.dll in $build - run scripts\build_puae.ps1 and rebuild"
}

# The version the exe was built with (its version resource, from CMakeLists.txt's project()).
$version = (Get-Item (Join-Path $build $exeName)).VersionInfo.ProductVersion
if (-not $version) { throw "$exeName has no version resource - rebuild it" }
$sha = (& git -C $repo rev-parse --short HEAD).Trim()
$dirty = (& git -C $repo status --porcelain --untracked-files=no)
$pkgName = "Peekaboo-$version-$sha" + $(if ($dirty) { "-dirty" } else { "" }) + $(if ($Public) { "-public" } else { "" })

if (-not $AmigaFilesDir) {
    $AmigaFilesDir = @($env:PEEKABOO_AMIGA_FILES,
                       $(if ($env:PUBLIC) { Join-Path $env:PUBLIC "Documents\Amiga Files\Shared" }),
                       $(if ($env:USERPROFILE) { Join-Path $env:USERPROFILE "Documents\Amiga Files\Shared" })) |
        Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (-not $Public -and -not ($AmigaFilesDir -and (Test-Path $AmigaFilesDir))) {
    throw ("no Amiga Forever Shared folder ($(if ($AmigaFilesDir) { $AmigaFilesDir } else { 'none found' }))" +
           " - pass -AmigaFilesDir, set PEEKABOO_AMIGA_FILES, or build -Public")
}

# Before anything is staged, so a missing signtool or certificate stops the run at once.
if ($Sign) {
    if (-not $CertThumbprint -and -not $PfxFile) {
        throw "-Sign needs -CertThumbprint (or PEEKABOO_SIGN_THUMBPRINT) or -PfxFile"
    }
    $signtool = @(Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
                  Sort-Object { [version]$_.Directory.Parent.Name } -Descending |
                  Select-Object -ExpandProperty FullName) +
                @((Get-Command signtool -ErrorAction SilentlyContinue).Source) |
        Where-Object { $_ } | Select-Object -First 1
    if (-not $signtool) { throw "signtool.exe not found - install the Windows SDK's signing tools" }
}

$exeTime = (Get-Item (Join-Path $build $exeName)).LastWriteTime
$headTime = [DateTime]::Parse((& git -C $repo log -1 --format=%cI).Trim())
if ($exeTime -lt $headTime) {
    Warn "$exeName ($exeTime) is older than HEAD ($headTime) - is the build current?"
}

$outRoot = Join-Path $repo $OutDir
$stage = Join-Path $outRoot $pkgName
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Step "Staging $stage"

# --- Program --------------------------------------------------------------------------------

Step "Executable and runtime DLLs"
foreach ($f in @($exeName, "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")) {
    Copy-One (Join-Path $build $f) $stage
}

Step "Data folders"
$dataDirs = @("assets", "bootblocks", "datatypes", "images")
if ($Private) { $dataDirs += "exocet-tools" }
foreach ($d in $dataDirs) {
    Copy-Tree (Join-Path $build $d) (Join-Path $stage $d)
}
# The build does not deploy definitions\ - the app only finds it via ..\..\definitions from
# inside the build tree. Next to the exe is the first place it looks.
Copy-Tree (Join-Path $repo "definitions") (Join-Path $stage "definitions")
# uae\configs: the sample, not this machine's own custom configs (they hold its paths). The
# sample is a copy of Adam's WinUAE setup - his ROM path and devices - so not for the public.
Copy-Tree (Join-Path $build "uae") (Join-Path $stage "uae") -excludeDirs @("custom") `
    -excludeFiles $(if ($Public) { @("sample.uae") } else { @() })
# The licences of everything Peekaboo is built on, and its own.
Copy-One (Join-Path $repo "THIRD_PARTY_NOTICES.md") $stage
Copy-One (Join-Path $repo "COPYING") $stage

if (-not $NoTosec) {
    Step "TOSEC database"
    foreach ($f in @("tosec.json", "categories.json", "hacked.json", "modified.json")) {
        Copy-One (Join-Path $build "tosec\$f") (Join-Path $stage "tosec")
    }
}

if ($Private -and -not $NoPython) {
    Step "Bundled Python (Exocet)"
    Copy-Tree (Join-Path $build "python") (Join-Path $stage "python") -excludeFiles @("*.pdb") `
        -excludeDirs @("__pycache__")
}

if ($IncludeStates) {
    Step "Save states"
    Copy-Tree (Join-Path $build "states") (Join-Path $stage "states")
}

# --- Emulator core + ROMs -------------------------------------------------------------------

Step "PuAE core and Kickstart ROMs"
if ($Public) {
    # Only what may be published goes in: the core and its own files. Everything else in the
    # build's emulation\ - ROMs under any name, rom.key, NVRAM, saves - is named and left out.
    $allowed = @("puae_libretro.dll", "puae_libretro.dylib", "required_files.md", "WHDLoad.prefs")
    $emuOut = Join-Path $stage "emulation"
    New-Item -ItemType Directory -Force $emuOut | Out-Null
    $leftOut = @()
    foreach ($item in Get-ChildItem -Force (Join-Path $build "emulation")) {
        if (-not $item.PSIsContainer -and $allowed -contains $item.Name) {
            Copy-One $item.FullName $emuOut
        } else {
            $leftOut += $item.Name
        }
    }
    if ($leftOut) { Step "Left out of emulation\ ($($leftOut.Count)): $($leftOut -join ', ')" }
} else {
    # PuAE's per-session *_save.adf.gz redirects and the WHDLoad prefs backup are local leftovers.
    Copy-Tree (Join-Path $build "emulation") (Join-Path $stage "emulation") `
        -excludeFiles @("*_save.adf.gz", "*.prefs_backup")
}

$af = $AmigaFilesDir
$haveAf = -not $Public

if ($haveAf) {
    # Optional ROMs from required_files.md, under the names the core resolves.
    $romDir = Join-Path $af "rom"
    $emu = Join-Path $stage "emulation"
    if (-not (Test-Path (Join-Path $emu "kick34005.CDTV"))) {
        Copy-One (Join-Path $romDir "amiga-os-130-cdtv-ext.rom") $emu "kick34005.CDTV"
    }
    Copy-One (Join-Path $romDir "amiga-boot-a590.rom") $emu
    Copy-One (Join-Path $romDir "amiga-boot-a4091.rom") $emu
}

# --- Media ----------------------------------------------------------------------------------

Step "Workbench media"
$media = Join-Path $stage "media"
# The small Workbench 1.3 / 3.1 host directories used as DH0 for running executables.
if (-not $Public) { Copy-Tree (Join-Path $repo "harddisks") (Join-Path $stage "harddisks") }
if ($haveAf) {
    # Workbench / Extras / Install floppies (OS 1.x - 3.1) and AROS.
    Copy-Tree (Join-Path $af "adf") (Join-Path $media "adf")
    if (-not $NoHardfiles) {
        foreach ($h in @("workbench-135.hdf", "workbench-211.hdf", "workbench-311.hdf")) {
            Copy-One (Join-Path $af "hdf\$h") (Join-Path $media "hdf")
        }
    }
    # Amiga Forever's System: / Work: directories (a full installed 3.x System disk).
    Copy-Tree (Join-Path $af "dir\System") (Join-Path $stage "harddisks\System")
    Copy-Tree (Join-Path $af "dir\Work") (Join-Path $stage "harddisks\Work")
}

# --- Settings -------------------------------------------------------------------------------

Step "Workspaces and sanitised preferences"
Copy-One (Join-Path $build "table_layout.ini") $stage
# Exocet is hidden unless this is Adam's own package, so its workspace, its panel and its windows'
# open states come out of the shipped workspaces - otherwise an empty "Exocet" panel and workspace
# are left behind for everyone else.
$wsSrc = Join-Path $build "workspaces.json"
if (Test-Path $wsSrc) {
    $wsList = ConvertTo-Dictionary (Get-Content -Raw -Encoding UTF8 -LiteralPath $wsSrc | ConvertFrom-Json)
    if (-not $Private) {
        $kept = @()
        foreach ($ws in $wsList) {
            if ($ws["name"] -eq "Exocet") { continue }
            if ($ws.Contains("panels") -and $ws["panels"]) {
                $ws["panels"] = @($ws["panels"] | Where-Object { $_["name"] -ne "Exocet" })
            }
            if ($ws.Contains("windowStates") -and $ws["windowStates"]) {
                foreach ($k in @($ws["windowStates"].Keys)) {
                    if ($k -like "Exocet*") { $ws["windowStates"].Remove($k) }
                }
            }
            $kept += , $ws
        }
        $wsList = $kept
    }
    Write-Utf8 (Join-Path $stage "workspaces.json") (ConvertTo-Json -InputObject @($wsList) -Depth 32)
}
# The Lores profile's column widths - only there once the Lores profile has been used.
if (Test-Path (Join-Path $build "table_layout_lores.ini")) {
    Copy-One (Join-Path $build "table_layout_lores.ini") $stage
}
foreach ($f in @("memory_annotations.json", "memory_locks.json")) {
    Write-Utf8 (Join-Path $stage $f) "{}"
}

$prefsSrc = Join-Path $build "user_prefs.json"
if (Test-Path $prefsSrc) {
    $p = ConvertTo-Dictionary (Get-Content -Raw -Encoding UTF8 -LiteralPath $prefsSrc | ConvertFrom-Json)

    # Machine-specific or personal.
    $p["kickstartRomDir"] = ""            # empty = <exe>\emulation
    $p["recentFiles"] = @()
    $p["recentDirectories"] = @()
    $p["mcpToken"] = ""                   # a fresh one is generated at first start
    # The demoscene.au login is gone from the app; a build's prefs written before then still
    # carry its session.
    foreach ($k in @("demosceneAuthState", "demosceneSessionCookie", "demosceneUserRole",
                     "showDemosceneAuth")) { [void]$p.Remove($k) }
    $p["exocetPython"] = ""
    $p["exocetTools"] = ""
    # Exocet and the crAss engine's tools are hidden unless this is Adam's own package.
    $p["privateFeatures"] = [bool]$Private
    if (-not $Private) {
        $p["showExocet"] = $false
    }
    # Every package starts in the Tosec workspace (pick a disk, run it), not in whichever one this
    # build last had open.
    $p["activeWorkspace"] = "Tosec"
    # A Kickstart picked by path on this machine means nothing on another: the model's own.
    if ($p.Contains("emulatorSettings") -and $null -ne $p["emulatorSettings"]) {
        foreach ($k in @("puae_kickstart", "puae_kickstart_ext")) {
            if ($p["emulatorSettings"].Contains($k) -and @("auto", "aros") -notcontains $p["emulatorSettings"][$k]) {
                $p["emulatorSettings"][$k] = "auto"
            }
        }
    }
    # The setup wizard runs on the first start (it skips itself when the ROMs are already there).
    $p["onboardingDone"] = $false
    $p["cdImagePath"] = ""
    $p["parallelOutputPath"] = ""
    $p["serialPort"] = ""
    if ($p.Contains("videoCapture")) { $p["videoCapture"]["path"] = ""; $p["videoCapture"]["ffmpegPath"] = "" }
    if ($p.Contains("source")) { $p["source"]["searchRoots"] = @() }
    if ($p.Contains("fbSlots")) { foreach ($s in $p["fbSlots"]) { $s["comPort"] = "" } }

    # Screen profiles: ship every profile's settings stored and none
    # chosen, so the first run asks which suits its screen and takes that profile's text sizes.
    if (-not $p.Contains("screenProfiles") -or $null -eq $p["screenProfiles"]) {
        $p["screenProfiles"] = [ordered]@{}
    }
    if ($p.Contains("screenProfile") -and $p["screenProfile"]) {
        $live = [ordered]@{}
        foreach ($k in @("fonts", "fontName", "spritesScanlinesPanelW", "spritesDetailPanelH",
                         "memMapLeftColumnW", "memMapPreviewH", "memoryBytesPerLine")) {
            if ($p.Contains($k)) { $live[$k] = $p[$k] }
        }
        $p["screenProfiles"][$p["screenProfile"]] = $live
    }
    $p["screenProfile"] = ""

    # A 4K maximised window becomes a sensible default the first run can resize - in every
    # profile, since this machine's window positions mean nothing on another.
    $p["maximized"] = $true
    $p["windowX"] = 100; $p["windowY"] = 100
    $p["windowWidth"] = 1600; $p["windowHeight"] = 900
    foreach ($key in @($p["screenProfiles"].Keys)) {
        $sp = $p["screenProfiles"][$key]
        $sp["maximized"] = $true
        $sp["windowX"] = 100; $sp["windowY"] = 100
        $sp["windowWidth"] = 1600; $sp["windowHeight"] = 900
    }

    $dh0Hardfile = if ($haveAf -and -not $NoHardfiles) { "media/hdf/workbench-311.hdf" } else { "" }

    # Hard drives: bundled media, exe-relative, left disabled so the default boot is floppy.
    $p["hostDirectoryMounts"] = @(
        [ordered]@{
            backingType = "directory"; bootPriority = 0; controller = "uae"; controllerUnit = 0
            device = "DH0"; directory = "harddisks/System"; enabled = $false
            geometry = [ordered]@{ blockSize = 512; mode = "auto"; reserved = 2; sectors = 32; surfaces = 1 }
            hardfile = $dh0Hardfile; label = "System"; readOnly = $false
        },
        [ordered]@{
            backingType = "directory"; bootPriority = -128; controller = "uae"; controllerUnit = 0
            device = "DH1"; directory = "harddisks/Work"; enabled = $false
            geometry = [ordered]@{ blockSize = 512; mode = "auto"; reserved = 2; sectors = 32; surfaces = 1 }
            hardfile = ""; label = "Work"; readOnly = $true
        }
    )
    # Nothing to mount in a public package: the drives above are Workbench installs.
    if ($Public) { $p["hostDirectoryMounts"] = @() }
    if ($p.Contains("hardDriveControllerRoms")) {
        $p["hardDriveControllerRoms"]["a590A2091"] = if ($haveAf) { "emulation/amiga-boot-a590.rom" } else { "" }
        $p["hardDriveControllerRoms"]["a4091"] = if ($haveAf) { "emulation/amiga-boot-a4091.rom" } else { "" }
    }

    $json = $p | ConvertTo-Json -Depth 32
    # Any absolute path that survived is a leak from this machine - report it, do not ship silently.
    $leaks = [regex]::Matches($json, '"[A-Za-z]:(\\\\|/)[^"]*"') | ForEach-Object Value
    if ($leaks) { Warn "absolute paths still in user_prefs.json:`n  $($leaks -join "`n  ")" }
    Write-Utf8 (Join-Path $stage "user_prefs.json") $json
} else {
    Warn "no user_prefs.json in the build - the package starts with defaults"
}

# --- Extras ---------------------------------------------------------------------------------

Step "Extras (MCP bridge, VS Code extension, Chrome extension)"
$extras = Join-Path $stage "extras"
foreach ($f in @("README.md", "peekaboo_mcp_bridge.py", "requirements.txt")) {
    Copy-One (Join-Path $repo "mcp\$f") (Join-Path $extras "mcp")
}
Copy-One (Join-Path $repo "vscode-extension\peekaboo-run.vsix") (Join-Path $extras "vscode")
Copy-One (Join-Path $repo "vscode-extension\README.md") (Join-Path $extras "vscode")
Copy-Tree (Join-Path $repo "chrome-extension") (Join-Path $extras "chrome-extension")

# --- README ---------------------------------------------------------------------------------

$shipHdf = $haveAf -and -not $NoHardfiles
$hdfListing = if ($shipHdf) {
    "`n  media\hdf\     Pre-installed Workbench 1.3 / 2.1 / 3.1 hardfiles"
} else { "" }
$hdfHint = if ($shipHdf) {
    "`n    To use a hardfile instead, switch DH0's backing to Hardfile (media\hdf\workbench-311.hdf)."
} else { "" }

$pythonListing = if ($Private -and -not $NoPython) {
    "`n  python\        Private Python used only by the Exocet sync tracker"
} else { "" }
$contents = if ($Public) {
@"
  emulation\     Patched PuAE emulator core (Kickstart ROMs are not included - see below)$pythonListing
  extras\        Claude MCP bridge, VS Code "compile & run" extension, Chrome extension
  THIRD_PARTY_NOTICES.md   What Peekaboo is built on, and the licences

Kickstart ROMs and Workbench
----------------------------
Kickstart ROMs and Workbench disks are copyrighted, so they are not in this download. Use your
own: Amiga Forever, AmigaOS 3.2, or ROMs and disks dumped from your own Amiga. On the first start
the setup wizard looks for them (Amiga Forever's Shared folder, FS-UAE and WinUAE's ROM folders,
or any folder you pick), identifies them by their contents and copies the ROMs into emulation\.
Run it again from Help > Setup Wizard, or use Emulator > Preferences > System Files.
"@
} else {
@"
  emulation\     Patched PuAE emulator core + Kickstart ROMs (A1000/A500/A600/A1200/A4000/CD32)
  media\adf\     Workbench / Extras / Install floppies, OS 1.0 - 3.1, and AROS$hdfListing
  harddisks\     Host-directory hard drives: System and Work (a full 3.1 install),
                 workbench1.3 / workbench3.1 (small boot drives for running executables)$pythonListing
  extras\        Claude MCP bridge, VS Code "compile & run" extension, Chrome extension
  THIRD_PARTY_NOTICES.md   What Peekaboo is built on, and the licences
"@
}
$firstSteps = if ($Public) {
@"
  * System files: the setup wizard on the first start, or Help > Setup Wizard.
  * Machine / ROM: Emulator > Preferences. The ROM folder defaults to emulation\ next to the exe.
  * Floppy: File > Open an ADF, then run it. The TOSEC browser finds and downloads software.
"@
} else {
@"
  * Floppy: File > Open an ADF (e.g. media\adf\amiga-os-134-workbench.adf), then run it.
  * Hard drive boot: Emulator > Hard Drives - DH0 (System) and DH1 (Work) are configured but
    disabled; tick them, pick an A1200/A4000 model with Kickstart 3.1, Apply and Restart.$hdfHint
  * Machine / ROM: Emulator > Preferences. The ROM folder defaults to emulation\ next to the exe.
"@
}

$readme = @"
Peekaboo ($pkgName)
===================

Windows 10/11 x64. No installer - unzip anywhere you can write to (not Program Files; Peekaboo
saves its settings, workspaces and ROM scan results next to the exe) and run:

    peekaboo.exe

If Windows SmartScreen warns about an unknown publisher: More info > Run anyway.
Needs an OpenGL 3.3 capable GPU driver.

Website: https://demoscene.au/peekaboo
Source and new releases: https://github.com/AdamKeher/peekaboo

What is in here
---------------
$contents
First steps
-----------
$firstSteps

Claude / MCP
------------
The MCP server is on (127.0.0.1:7475). A new token is generated on first start - see
Preferences > Claude / MCP Server, and extras\mcp\README.md for the Claude Desktop bridge.
"@
Write-Utf8 (Join-Path $stage "README.txt") $readme

# --- Nothing copyrighted in a public package --------------------------------------------------

if ($Public) {
    Step "Checking for Amiga system files"
    $found = Find-SystemFiles $stage
    if ($found) {
        throw "a public package must not carry Amiga system files:`n$($found -join "`n")"
    }
}

# --- Signing --------------------------------------------------------------------------------

if ($Sign) {
    $targets = @((Join-Path $stage $exeName), (Join-Path $stage "emulation\puae_libretro.dll"))
    $signArgs = @("sign", "/fd", "SHA256", "/d", "PEEKaboo",
                  "/du", "https://github.com/AdamKeher/peekaboo")
    if ($PfxFile) {
        $signArgs += @("/f", (Resolve-Path $PfxFile).Path)
        if ($PfxPassword) { $signArgs += @("/p", $PfxPassword) }
    } else {
        $signArgs += @("/sha1", $CertThumbprint)
    }
    if ($TimestampUrl) { $signArgs += @("/tr", $TimestampUrl, "/td", "SHA256") }
    Step "Signing with $signtool"
    & $signtool @signArgs @targets
    if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }
    foreach ($t in $targets) {
        $sig = Get-AuthenticodeSignature -LiteralPath $t
        if ($sig.Status -eq "NotSigned") { throw "$t is not signed" }
        $who = $sig.SignerCertificate.Subject
        if ($sig.Status -eq "Valid") {
            Step "Signed $(Split-Path $t -Leaf): $who"
        } else {
            # A test certificate: signed, but by no authority Windows trusts.
            Warn "$(Split-Path $t -Leaf) is signed by $who, but Windows says: $($sig.Status) - $($sig.StatusMessage)"
        }
    }
}

# --- Zip ------------------------------------------------------------------------------------

$sizeMB = [math]::Round(((Get-ChildItem -Recurse -File $stage | Measure-Object Length -Sum).Sum) / 1MB)
Step "Staged $sizeMB MB"

if (-not $NoZip) {
    $zip = Join-Path $outRoot "$pkgName.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    $sevenZip = @((Get-Command 7z -ErrorAction SilentlyContinue).Source,
                  "C:\Program Files\7-Zip\7z.exe") | Where-Object { $_ -and (Test-Path $_) } |
        Select-Object -First 1
    Step "Zipping to $zip"
    if ($sevenZip) {
        & $sevenZip a -tzip -mx=7 -bso0 -bsp0 $zip $stage
        if ($LASTEXITCODE -ne 0) { throw "7-Zip failed ($LASTEXITCODE)" }
    } else {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip,
            [IO.Compression.CompressionLevel]::Optimal, $true)
    }
    $zipMB = [math]::Round((Get-Item $zip).Length / 1MB)
    Step "Done: $zip ($zipMB MB)"
} else {
    Step "Done: $stage"
}
