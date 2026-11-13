# Amiga Debugger Test: Expected Outputs

This document details the expected behavior and register states for the `debugger_test.s` program. Use these values to verify the accuracy of the emulator's debugger, tracer, and profiler.

## 1. CPU Tracing & Disassembly Profiler

### Subroutine Hotspots
The Disassembly Profiler should identify the following hotspots:
- **`ProfileTask`**: This is the primary hotspot. It is called **16 times per frame**. Inside, it executes a `dbf` loop **101 times** (from 100 down to -1).
- **`WaitVBlank`**: Should show high "Wait" time as it spins on the `VHPOSR` ($DFF004) register until line 300.
- **`MainLoop`**: Should show a consistent execution frequency of **once per frame**.

### Register States
- **A6**: Should consistently point to `$DFF000` (Custom Registers) throughout the program.
- **D7**: Should hold the initial `DMACONR` value (with bit 15 set) for the duration of the program.

## 2. Copper Tracing

### Rainbow Effect
The Copper tracer should show a sequence of `WAIT` and `MOVE` instructions. Expected line transitions for `COLOR00` ($DFF180):
| Line (Hex) | Line (Dec) | Expected Color (RGB) |
|------------|------------|-----------------------|
| $3C        | 60         | $F00 (Red)            |
| $4C        | 76         | $FF0 (Yellow)         |
| $5C        | 92         | $0F0 (Green)          |
| $6C        | 108        | $0FF (Cyan)           |
| $7C        | 124        | $00F (Blue)           |
| $8C        | 140        | $F0F (Magenta)        |
| $AC        | 172        | Dynamic (Varies)      |

### Frame Scrubber (Dynamic Color)
- The color at line **172 ($AC)** should change every **4 frames** (`lsr.l #2, d0`).
- The pattern cycles through 4-bit grayscale/blue-ish values (`and.w #$000F, d0`).

## 3. Blitter Tracing

The Blitter tracer should capture one blit per frame with the following parameters:

### Control Registers
- **BLTCON0**: `$BC5A`
  - Channels enabled: **A, B, D**
  - Logic function (LF): **$5A** (A XOR B)
- **BLTCON1**: `$0000` (No shifts, no line mode)
- **BLTSIZE**: `$0802`
  - High 10 bits: **32** (Height)
  - Low 6 bits: **2** (Width in words)
  - Total: 64 words (128 bytes) processed.

### Modulo Registers
- **BLTAMOD / BLTBMOD / BLTDMOD**: All should be **0**.

### Address Tracing
- **BLTAPTH/L**: Should point to `SourceA` (Pattern: `$AAAA` repeated).
- **BLTBPTH/L**: Should point to `SourceB` (Pattern: `$CCCC` repeated).
- **BLTDPTH/L**: Should point to `BlitBuffer`.

### Logical Result
The resulting data in `BlitBuffer` (starting from the address in BLTDPTH) should be:
- `$AAAA XOR $CCCC = $6666`
- **Verification**: Check memory at `BlitBuffer`; it should be filled with `$6666` for the first 128 bytes.

## 4. System Integrity & Exit
- **Left Mouse Button**: The program polls `$BFE001` bit 6. Pulling this bit low (pressing the button) should trigger the cleanup code.
- **Cleanup**: The debugger should show `DMACON` being restored to the value saved in `D7` and `INTENA` being set to `$C000`.
