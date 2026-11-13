#pragma once

#include <cstdint>

namespace adm::core {

// Debug display masking: which bitplanes / sprites / playfields Denise is allowed to draw.
//
// This is purely cosmetic. The emulated machine still fetches, blits and computes everything
// exactly as before; only the final pixel composition is filtered, so hiding a plane never
// changes program behaviour — it just makes it possible to see which layer draws what.
//
// Owned by AmigaEmulator so every surface shares one source of truth: the debugger's Display
// window, the Sprites window, the AR console (BPL/SPR/PF/DISPLAY) and MCP
// (get_display_masks/set_display_masks). Because the masks are applied inside the core's
// renderer, everything read back from the display texture — the emulator window, GIF recording
// and the MCP `screenshot` tool — shows the masked image.
struct DisplayMasks
{
    uint8_t bitplanes = 0xFF; // bit n = BPL(n+1) drawn  (bit 0 = BPL1 ... bit 7 = BPL8)
    uint8_t sprites = 0xFF;   // bit n = sprite n drawn   (bit 0 = SPR0 ... bit 7 = SPR7)
    uint8_t playfields = 0x3; // bit 0 = playfield 1, bit 1 = playfield 2
};

// Bitplane mask actually pushed to the core: the user's bitplane selection with the planes
// belonging to a hidden playfield cleared. In dual-playfield mode (BPLCON0 DBLPF) playfield 1
// is drawn from the odd bitplanes (BPL1/3/5) and playfield 2 from the even ones (BPL2/4/6), so
// hiding a playfield is the same as hiding its planes. BPL7/BPL8 belong to neither and are
// left alone.
inline uint8_t EffectiveBitplaneMask(const DisplayMasks& m)
{
    uint8_t bpl = m.bitplanes;
    if (!(m.playfields & 1))
        bpl &= static_cast<uint8_t>(~0x15u); // BPL1/3/5
    if (!(m.playfields & 2))
        bpl &= static_cast<uint8_t>(~0x2Au); // BPL2/4/6
    return bpl;
}

} // namespace adm::core
