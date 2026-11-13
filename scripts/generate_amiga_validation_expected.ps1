param(
    [string]$Output = "tests/amiga_validation/fixtures/expected.json"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$outputPath = Join-Path $repo $Output

# Source-level oracle constants. This script deliberately does not read emulator output.
$buildId = [uint32]0x20260812
$cpuD0 = [uint32]0x01020304
$cpuD1 = [uint32]0x11121314
$supervisorMask = [uint32]0x2000
$xorMinterm = [uint32]0x3c
$blitA = [uint32]0xaaaa
$blitB = [uint32]0xcccc
$blitResult = $blitA -bxor $blitB
$palLines = 313
$palCyclesPerLine = 228

function Exact($value) { return [ordered]@{ mode = "exact"; value = $value } }
function Masked($value, $mask) {
    return [ordered]@{ mode = "masked"; value = $value; mask = $mask }
}
function SequenceRule($value) { return [ordered]@{ mode = "sequence"; value = $value } }
function SetRule($value) { return [ordered]@{ mode = "set"; value = $value } }

$manifest = [ordered]@{
    schemaVersion = 1
    buildId = $buildId
    target = "peekaboo_validation.exe"
    profiles = @("a500-pal", "a500-ntsc", "a600-pal", "a1200-pal")
    scenarios = @(
        [ordered]@{
            id = 1; phase = 1; name = "CPU registers and status"
            capabilities = @("cpu", "mailbox", "symbols")
            checkpoints = @([ordered]@{
                id = 1; name = "distinctive register state"; symbol = "ptv_checkpoint_cpu"
                expectations = [ordered]@{
                    d0 = Exact $cpuD0
                    d1 = Exact $cpuD1
                    # AmigaDOS launches the validation executable in user mode.
                    sr = Masked 0 $supervisorMask
                    pc = Exact "ptv_checkpoint_cpu+0"
                }
            })
        },
        [ordered]@{
            id = 2; phase = 1; name = "Copper color split"
            capabilities = @("copper", "framebuffer-golden", "mailbox", "symbols")
            checkpoints = @([ordered]@{
                id = 2; name = "MOVE execution order"; symbol = "ptv_checkpoint_copper"
                expectations = [ordered]@{
                    operations = SequenceRule @("WAIT", "MOVE COLOR00 `$000F", "WAIT", "MOVE COLOR00 `$0F00")
                    lists = SetRule @(1)
                    framebuffer = [ordered]@{
                        mode = "image"; hash = "split-000f-0f00"; crop = @(0, 44, 320, 212)
                    }
                }
            })
        },
        [ordered]@{
            id = 3; phase = 1; name = "A XOR B blit"
            capabilities = @("blitter", "mailbox", "symbols")
            checkpoints = @([ordered]@{
                id = 3; name = "minterm and destination"; symbol = "ptv_checkpoint_blitter"
                expectations = [ordered]@{
                    minterm = Exact $xorMinterm
                    result = Exact $blitResult
                    trigger = Exact "cpu"
                }
            })
        },
        [ordered]@{
            id = 4; phase = 2; name = "Frame timing and alignment"
            capabilities = @("alignment-workload", "timeline", "dma", "idle", "partial-frame")
            checkpoints = @([ordered]@{
                id = 4; name = "PAL normalized frame"; symbol = "ptv_checkpoint_alignment"
                expectations = [ordered]@{
                    frameCycles = Exact ($palLines * $palCyclesPerLine)
                    idleSpans = Exact @(@(1000, 5000), @(8000, 12000))
                    dmaTotal = Exact ($palLines * $palCyclesPerLine)
                    breakLRef = [ordered]@{ mode = "range"; min = 19990; max = 20010 }
                }
            })
        },
        [ordered]@{
            id = 5; phase = 3; name = "Debugger depth and CIA"
            capabilities = @("callstack", "dwarf", "breakpoints", "watchpoints", "cia")
            checkpoints = @([ordered]@{
                id = 1; name = "nested stack and non-destructive CIA"; symbol = "ptv_checkpoint_cpu"
                expectations = [ordered]@{
                    stack = SequenceRule @("main", "alpha", "beta", "leaf")
                    ciaaCra = Masked 17 255
                    snapshotDestructive = Exact $false
                }
            })
        },
        [ordered]@{
            id = 6; phase = 4; name = "Chipset and visual model"
            capabilities = @("bitplanes", "sprites", "display-masks", "resources", "panel-render")
            checkpoints = @([ordered]@{
                id = 2; name = "isolated display and panel golden"; symbol = "ptv_checkpoint_copper"
                expectations = [ordered]@{
                    bitplanes = Exact 4
                    sprites = SetRule @(0, 1, 2, 3, 4, 5, 6, 7)
                    machineStateUnchanged = Exact $true
                    panel = [ordered]@{
                        mode = "image"; hash = "panel-alignment-v1"; crop = @(0, 0, 960, 240)
                    }
                }
            })
        },
        [ordered]@{
            id = 7; phase = 5; name = "Matrix and robustness"
            capabilities = @(
                "save-state", "capability-negotiation", "mcp", "matrix", "provenance-hashes"
            )
            checkpoints = @([ordered]@{
                id = 3; name = "save/reload and public API parity"; symbol = "ptv_checkpoint_blitter"
                expectations = [ordered]@{
                    restored = Exact $true
                    mcpMatchesDirect = Exact $true
                    profile = SetRule @("a500-pal", "a500-ntsc", "a600-pal", "a1200-pal")
                }
            })
        }
    )
}

$directory = Split-Path -Parent $outputPath
New-Item -ItemType Directory -Force $directory | Out-Null
$manifest | ConvertTo-Json -Depth 20 | Set-Content -Encoding utf8 $outputPath
Write-Host "Generated independent validation oracle at $outputPath"
