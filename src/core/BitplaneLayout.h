#pragma once

#include <cstdint>

namespace adm::core {

enum class BitplaneResolution {
    Lores,
    Hires,
    SuperHires,
};

// BPLCON0 resolution bits: SHRES (bit 6, AGA) wins over HIRES (bit 15).
inline BitplaneResolution ResolutionFromBplcon0(uint16_t bplcon0)
{
    if (bplcon0 & 0x0040)
        return BitplaneResolution::SuperHires;
    if (bplcon0 & 0x8000)
        return BitplaneResolution::Hires;
    return BitplaneResolution::Lores;
}

// Native display pixels per lores pixel: 1 (lores), 2 (hires), 4 (super-hires). DIW/DDF
// register positions are always in lores units, so anything reported in the screen's own
// pixels has to be scaled by this.
inline int ResolutionPixelsPerLores(BitplaneResolution resolution)
{
    switch (resolution) {
        case BitplaneResolution::Hires:
            return 2;
        case BitplaneResolution::SuperHires:
            return 4;
        case BitplaneResolution::Lores:
        default:
            return 1;
    }
}

inline const char* ResolutionName(BitplaneResolution resolution)
{
    switch (resolution) {
        case BitplaneResolution::Hires:
            return "hires";
        case BitplaneResolution::SuperHires:
            return "shres";
        case BitplaneResolution::Lores:
        default:
            return "lores";
    }
}

// Upper-case form, for tables that label a screen mode ("LORES" / "HIRES" / "SHRES").
inline const char* ResolutionLabel(BitplaneResolution resolution)
{
    switch (resolution) {
        case BitplaneResolution::Hires:
            return "HIRES";
        case BitplaneResolution::SuperHires:
            return "SHRES";
        case BitplaneResolution::Lores:
        default:
            return "LORES";
    }
}

// Words of bitplane DMA fetched per scanline for the given DDF window, or 0 if the window is
// empty/inverted (callers apply their own fallback).
//
// DDFSTOP marks the START of the last fetch block, so a trailing block is always added. Finer
// resolutions fetch more words over the SAME DDF span rather than covering more color clocks,
// which is why both numbers change with resolution (Hardware Reference Manual: lores stops
// every 8 clocks with 1 trailing word, hires every 4 with 2, super-hires every 2 with 4 —
// e.g. lores $38/$D0 → 20 words = 320px, hires $3C/$D4 → 40 words = 640px).
//
// This is the one place the formula lives: it used to be copy-pasted into the decoder, the
// memory-map guesser and the bitplane frame trace, and the copies drifted apart on super-hires.
inline int FetchWordsPerRow(uint16_t ddfstrt, uint16_t ddfstop, BitplaneResolution resolution)
{
    if (ddfstop <= ddfstrt)
        return 0;

    int clocksPerFetch = 8;
    int trailingWords = 1;
    if (resolution == BitplaneResolution::Hires) {
        clocksPerFetch = 4;
        trailingWords = 2;
    } else if (resolution == BitplaneResolution::SuperHires) {
        clocksPerFetch = 2;
        trailingWords = 4;
    }

    return (static_cast<int>(ddfstop) - static_cast<int>(ddfstrt)) / clocksPerFetch + trailingWords;
}

// DDF fetch width in the screen's own (resolution-dependent) pixels. 0 for an empty DDF window.
inline int FetchWidthPx(uint16_t ddfstrt, uint16_t ddfstop, BitplaneResolution resolution)
{
    return FetchWordsPerRow(ddfstrt, ddfstop, resolution) * 16;
}

enum class BitplaneDisplayMode {
    Indexed,
    ExtraHalfBrite,
    DualPlayfield,
    Ham6,
    Ham8,
};

// Plain snapshot of the currently-decoded bitplane geometry.
//
// The authoritative decode (BPU count, DDF/DIW resolution, interleave auto-detect,
// HAM/dual-playfield handling) lives in ui::BitplanesView. This struct is the read-only
// value it hands out so non-UI code (e.g. the blit-destination overlay mapper) can reason
// about where a chip address lands on screen without depending on the view.
struct BitplaneLayout
{
    static constexpr int kMaxPlanes = 8;

    int bplCount = 0;
    int screenWidth = 0;  // decoded DMA width in native (resolution-dependent) pixels
    int screenHeight = 0; // visible height in scanlines
    int fetchWidth = 0;   // width of the decoded DMA fetch; currently the preview extent
    int visibleWidth = 0; // DIW display-window width in native pixels (may differ from fetchWidth)
    int fetchUnitBytes = 2; // 2 for word fetch, 4 when FMODE BPL32 is active
    int modulo1 = 0;        // BPL1MOD — applied to odd  bitplanes (0-based index 0,2,4,…)
    int modulo2 = 0;        // BPL2MOD — applied to even bitplanes (0-based index 1,3,5,…)
    bool interleaved = false;
    bool interlaced = false;
    // The bitplane's own horizontal resolution, from the BPLCON0 HIRES/SHRES bits. Independent of
    // the emulator texture's resolution (AmigaScreenMap::loresScale), which can differ when a
    // lores screen is rendered into a hires-width texture.
    BitplaneResolution resolution = BitplaneResolution::Lores;
    BitplaneDisplayMode displayMode = BitplaneDisplayMode::Indexed;
    uint32_t bplBase[kMaxPlanes] = {}; // per-plane chip base address (top-left visible pixel)

    // Screen-space anchors of the top-left visible bitplane pixel, so a plane-relative
    // (column,row) can be converted to Amiga lores-pixel / scanline coordinates.
    int ddfStartLoresPx = 0; // horizontal origin in lores pixels: (DDFSTRT & 0xFE) * 2
    int diwVStart = 0;       // vertical origin scanline: DIWSTRT vstart
    int scroll1 = 0;         // BPLCON1 PF1 fine-scroll value
    int scroll2 = 0;         // BPLCON1 PF2 fine-scroll value

    bool valid = false;

    // Native display pixels per lores pixel: 1 / 2 / 4. Derived from `resolution` rather than
    // paired with a stored `hires` bool — that pairing let the two disagree, and made super-hires
    // silently decode as hires.
    int PixelsPerLores() const { return ResolutionPixelsPerLores(resolution); }

    // Bytes between the start of one scanline and the next, for plane index p.
    int StrideBytes(int p) const
    {
        const int bytesPerRow = screenWidth / 8;
        if (interleaved)
            return bytesPerRow * bplCount + modulo1;
        return bytesPerRow + ((p % 2 == 0) ? modulo1 : modulo2);
    }
};

} // namespace adm::core
