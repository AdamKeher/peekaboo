#pragma once

#include <cstdint>

namespace adm::core {

// Chipset write blocks: which copper register writes the core is allowed to perform, over which
// band of scanlines. See PATCHES.md ("chipset-blocks").
//
// Unlike DisplayMasks these are not cosmetic — a suppressed write really does not happen, so the
// emulated program's picture changes. What they do NOT change is timing: the copper program still
// runs, WAIT/SKIP still execute, copper DMA cycles are still consumed and the copper PC still
// advances. Only the effect of the write is dropped, which is what makes them usable as a
// bisection tool rather than a perturbation.
//
// Both are copper-only. A CPU write to the same register still lands, so what disappears when a
// block is armed is exactly the copper's contribution.
//
// The default line range spans roughly the PAL visible display window.
inline constexpr int kBlockDefaultStartLine = 52;
inline constexpr int kBlockDefaultEndLine = 308;

// Highest line the core will accept (MAXVPOS_LINES_ECS - 1 in custom.h). The core clamps too;
// this is here so the UI can clamp before it sends.
inline constexpr int kBlockMaxLine = 2047;

// Drop every copper register write on lines in [startLine, endLine]. Answers "which lines is this
// copper list responsible for": sweep the band and whatever stops changing was the copper's doing.
struct CopperBlock
{
    bool enabled = false;
    int startLine = kBlockDefaultStartLine;
    int endLine = kBlockDefaultEndLine;
};

// Drop copper writes to BPLxPT for the selected planes on lines in [startLine, endLine]. Pinning a
// plane's pointer makes the part of the picture it feeds visibly repeat, which identifies which
// copper MOVE re-points which plane, and where.
struct BplPtrBlock
{
    uint8_t mask = 0; // bit n = BPL(n+1)PT blocked (bit 0 = BPL1PT ... bit 5 = BPL6PT)
    int startLine = kBlockDefaultStartLine;
    int endLine = kBlockDefaultEndLine;
};

// Only planes 1-6 have blockable pointers: BPL7PT/BPL8PT are AGA-only and upstream's hook covers
// the OCS/ECS set, so the mask is six bits wide.
inline constexpr uint8_t kBplPtrBlockMaskAll = 0x3F;
inline constexpr int kBplPtrBlockPlaneCount = 6;

inline int ClampBlockLine(int line)
{
    if (line < 0)
        return 0;
    if (line > kBlockMaxLine)
        return kBlockMaxLine;
    return line;
}

} // namespace adm::core
