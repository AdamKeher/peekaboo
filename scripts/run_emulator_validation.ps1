param(
    [ValidateSet("a500-pal", "a500-ntsc", "a600-pal", "a1200-pal")]
    [string]$Model = "a500-pal",
    [string]$Filter = "1-5",
    [string]$Artifacts = "validation-artifacts",
    [int]$Repetitions = 1,
    [switch]$Real,
    [string]$Core = "emulation/puae_libretro.dll",
    [int]$FrameBudget = 900,
    [switch]$UpdateGoldens
)

$ErrorActionPreference = "Stop"
if ($UpdateGoldens) {
    throw "Golden updates require a reviewed image producer and are intentionally unavailable in the fixture lane."
}

$parts = $Filter -split "-", 2
$minPhase = [int]$parts[0]
$maxPhase = if ($parts.Count -eq 2) { [int]$parts[1] } else { $minPhase }
$runner = Join-Path $PSScriptRoot "..\build\default\emulator_validation_tests.exe"
if (-not (Test-Path $runner)) {
    throw "Validation runner not found. Build it with: cmake --build build/default"
}

$arguments = @(
    "--manifest", "tests/amiga_validation/fixtures/expected.json",
    "--profile", $Model,
    "--min-phase", $minPhase,
    "--max-phase", $maxPhase,
    "--repetitions", $Repetitions,
    "--artifacts", $Artifacts
)
if ($Real) {
    $arguments += @(
        "--real", "--core", $Core, "--system-dir", "emulation",
        "--target", "tests/amiga_validation/fixtures/peekaboo_validation.exe",
        "--elf", "tests/amiga_validation/fixtures/peekaboo_validation.elf",
        "--frame-budget", $FrameBudget
    )
}

& $runner @arguments
exit $LASTEXITCODE
