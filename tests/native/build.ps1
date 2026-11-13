# Build script for Amiga Debugger Test
# Compiles with vasm, links with vlink, and creates a bootable ADF

$ErrorActionPreference = "Stop"

# Set working directory to the script's location
Set-Location -Path $PSScriptRoot

$asmFile = "debugger_test.s"
$objDir = "build"
$objFile = "$objDir\debugger_test.o"
$exeDir = "uae\dh0"
$exeFile = "$exeDir\debugger_test"
$adfFile = "debugger_test.adf"

# Ensure directories exist
if (-not (Test-Path $objDir)) { New-Item -ItemType Directory -Path $objDir }
if (-not (Test-Path $exeDir)) { New-Item -ItemType Directory -Path $exeDir }

Write-Host "--- Building Amiga Debugger Test ---" -ForegroundColor Cyan

# 1. Compile assembly to object file
Write-Host "Compiling $asmFile..."
& vasmm68k_mot.exe -m68000 -Fhunk -linedebug -o $objFile $asmFile

if ($LASTEXITCODE -ne 0) {
    Write-Error "Compilation failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

# 2. Link object file to Amiga hunk executable
Write-Host "Linking $objFile..."
& vlink -bamigahunk -Bstatic -o $exeFile $objFile

if ($LASTEXITCODE -ne 0) {
    Write-Error "Linking failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

# 3. Create ADF from executable
Write-Host "Creating $adfFile..."
& exe2adf -i $exeFile -a $adfFile

if ($LASTEXITCODE -ne 0) {
    Write-Error "ADF creation failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "--- Build Successful ---" -ForegroundColor Green
Write-Host "Executable: $(Get-Item $exeFile | Select-Object -ExpandProperty FullName)"
Write-Host "ADF Output: $(Get-Item $adfFile | Select-Object -ExpandProperty FullName)"
