param(
    [string]$Output = "tests/amiga_validation/fixtures"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo "tests/amiga_validation/target/validation.s"
$outputDir = Join-Path $repo $Output
New-Item -ItemType Directory -Force $outputDir | Out-Null

foreach ($tool in @("vasmm68k_mot.exe", "vlink.exe", "exe2adf.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required Amiga tool '$tool' was not found on PATH"
    }
}

$hunkObject = Join-Path $outputDir "peekaboo_validation.hunk.o"
$elfObject = Join-Path $outputDir "peekaboo_validation.elf.o"
$exe = Join-Path $outputDir "peekaboo_validation.exe"
$elf = Join-Path $outputDir "peekaboo_validation.elf"
$adf = Join-Path $outputDir "peekaboo_validation.adf"
$map = Join-Path $outputDir "peekaboo_validation.map"

& vasmm68k_mot.exe -m68000 -Fhunk -linedebug -o $hunkObject $source
if ($LASTEXITCODE) { throw "vasm HUNK build failed" }
$linkOutput = & vlink.exe -bamigahunk -Bstatic -M -o $exe $hunkObject 2>&1
if ($LASTEXITCODE) { throw "HUNK link failed" }
$linkOutput | Set-Content -Encoding utf8 $map

& vasmm68k_mot.exe -m68000 -Felf -dwarf=3 -o $elfObject $source
if ($LASTEXITCODE) { throw "vasm ELF build failed" }
# The relocatable ELF is intentionally retained as the sibling debug file. It contains the
# complete symbol table and DWARF, while avoiding old vlink builds that abort when linking DWARF
# sections into an ELF executable. Runtime relocation is resolved from the HUNK segment map.
Copy-Item -Force $elfObject $elf

& exe2adf.exe -i $exe -a $adf
if ($LASTEXITCODE) { throw "ADF build failed" }

Remove-Item -Force $hunkObject, $elfObject

& (Join-Path $PSScriptRoot "generate_amiga_validation_expected.ps1") `
    -Output (Join-Path $Output "expected.json")
if ($LASTEXITCODE) { throw "expected manifest generation failed" }

Write-Host "Built validation target in $outputDir"
