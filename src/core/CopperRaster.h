#pragma once

#include <cstdint>
#include <vector>

#include "AmigaEmulator.h"
#include "RasterFrameMap.h"

namespace adm::core {

// Turns one frame's DMA record and Copper execution trace into a raster image: one texel per
// colour clock, laid out vpos down and hpos across, covering the whole beam including blanking.
//
// The app already renders both of these along ONE axis — the Frame Scrubber's DMA row paints a
// rect per colour clock, and DmaStatsView totals the same record. What is missing is the second
// dimension, which is the one that makes a copper effect legible: a colour split is a horizontal
// line, a stalled WAIT is a horizontal bar, and a copper that misses its slot is a gap with the
// culprit sitting right next to it.
//
// Adapted from the ENGINE9000 debugger by alpine9000
// (https://github.com/alpine9000/engine9000-public), GPL-2 — the same licence this project is
// distributed under (see COPYING). Specifically `e9k-debugger/emu_ami.c`
// (`emu_ami_copperColorForRecord` / `emu_ami_copperColorForMoveOffset`, the classification of a
// copper instruction into a colour by destination register, and `emu_ami_getCopperSlotTextureRect`,
// which places a slot on the raster). See PATCHES.md.
//
// Pure — no ImGui, no OpenGL, no emulator calls — so it is unit-testable. CopperRasterOverlay is
// the thin GL wrapper around it, and supplies the palette from ui/DmaColors.h so the raster, the
// scrubber's DMA row and DmaStatsView cannot drift apart.
namespace copraster {

// Pixels are RGBA8 in the byte order OpenGL's GL_RGBA/GL_UNSIGNED_BYTE expects on a
// little-endian host, i.e. 0xAABBGGRR read as a uint32_t — the same packing blitvis uses, and
// the same one ImGui's IM_COL32 produces, so palette entries can be passed straight through.
inline uint32_t PackRGBA(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (a << 24) | (b << 16) | (g << 8) | r;
}

inline uint32_t AlphaOf(uint32_t pixel) { return pixel >> 24; }
inline uint32_t WithAlpha(uint32_t pixel, uint32_t a) { return (pixel & 0x00FFFFFFu) | (a << 24); }

// How a Copper instruction is classified for colouring. ENGINE9000 draws WAIT and SKIP in the
// same red; they are split here because the trace distinguishes them for free and confusing a
// skipped branch with a stalled wait is exactly the bug this view exists to find.
enum class CopClass : uint8_t {
    None = 0,
    Wait,
    Skip,
    MoveSprite,  // SPRxPT / SPRxPOS / SPRxCTL / SPRxDATA  ($120-$17F)
    MoveBlitter, // BLTxxx                                  ($040-$077)
    MoveBplPtr,  // BPLxPTH/PTL                             ($0E0-$0FF)
    MoveColor,   // COLORxx                                 ($180-$1BF)
    MoveOther,
    Count
};

// Classifies a raw Copper instruction word pair. Works off register offsets rather than decoded
// names, so it needs neither the custom-register JSON nor a string compare per slot.
CopClass Classify(uint16_t word1, uint16_t word2);

// Colours, supplied by the caller so ui/DmaColors.h stays the single source of truth for the
// DMA channels. Entries are packed RGBA (see PackRGBA).
struct Palette
{
    uint32_t channel[10] = {}; // indexed by DMARECORD_*
    uint32_t copper[static_cast<size_t>(CopClass::Count)] = {};
    uint32_t waitSpan = 0;     // the stalled run between a WAIT and its wake
    uint32_t copperWanted = 0; // the Copper asked for the bus and was refused
    uint32_t blanking = 0;     // HBLANK / VBLANK shading
    uint32_t sync = 0;         // HSYNC / VSYNC shading
    uint32_t ddfMark = 0;      // per-line DDFSTRT/DDFSTOP/DDFSTOP2
    uint32_t diwMark = 0;      // per-line HDIWS/HDIWE

    // The default copper classification colours (ENGINE9000's, with WAIT/SKIP split).
    static Palette Default();
};

struct Options
{
    bool showCopper = true;
    bool showWaitSpans = false;
    bool showCopperWanted = true;
    bool showBlanking = true;
    bool showGeometryEvents = true;

    // Bit per DMARECORD_* type; bit 0 (idle) is ignored. Zero draws no DMA layer at all.
    uint16_t channelMask = 0;

    // Slots at or past this L-Ref are dimmed as "not yet executed", for a mid-frame breakpoint.
    // UINT32_MAX disables it.
    uint32_t breakLRef = UINT32_MAX;
};

// What occupies one colour clock, so a hovered cell can be described and clicked through to the
// view that owns it. Parallel to the pixel buffer.
struct Cell
{
    int32_t copperEvent = -1; // index into the CopperExecEvent array, -1 = none
    int32_t dmaSlot = -1;     // index into the DmaSlotRecord array, -1 = none
    CopClass copClass = CopClass::None;
    uint8_t dmaType = 0;       // DMARECORD_*
    bool waitBlocked = false;  // inside a WAIT's stalled run
    bool copperWanted = false; // Copper was refused the bus here
};

struct Input
{
    const AmigaEmulator::DmaSlotRecord* records = nullptr;
    int hposCount = 0;
    int vposCount = 0;
    const AmigaEmulator::CopperExecEvent* copperEvents = nullptr;
    int copperEventCount = 0;
};

struct Result
{
    std::vector<uint32_t> pixels; // width * height, RGBA8
    std::vector<Cell> cells;      // width * height
    int width = 0;
    int height = 0;

    // Colour clocks in an Agnus line (227). `width` exceeds it: the columns from a row's line
    // length to `width` are the displayed line's wrap into the next Agnus line, filled with the
    // clocks that drew them - that line's leading slots. See RasterFrameMap::rasterCC.
    int lineCC = 0;

    // Per-row line length: lineCC, or lineCC + 1 on a long NTSC line (the record's LOL marker).
    // This is where each row's wrap columns start.
    std::vector<int16_t> rowLineCC;

    int LineCCAt(int line) const
    {
        if (line >= 0 && line < static_cast<int>(rowLineCC.size()))
            return rowLineCC[static_cast<size_t>(line)];
        return lineCC;
    }

    // Populated counts, so the UI can tell "nothing is showing" from "nothing was recorded".
    int copperCells = 0;
    int dmaCells = 0;

    const Cell* At(int cc, int line) const
    {
        if (cc < 0 || line < 0 || cc >= width || line >= height)
            return nullptr;
        return &cells[static_cast<size_t>(line) * width + cc];
    }
};

// The frame's real scanline count, derived from the DMA record.
//
// PuAE hands out a record buffer NR_DMA_REC_VPOS = 1000 rows tall; the rows past the end of the
// frame are untouched padding. Using the buffer height as the frame height stretches the raster
// to three times its real size. Refresh DMA runs on every scanline including blanking, so the
// highest row carrying any activity is a reliable and frame-to-frame stable proxy for maxvpos.
//
// Returns `fallback` when there is no record or nothing was recorded.
int DeriveFrameHeight(const AmigaEmulator::DmaSlotRecord* records, int hposCount, int vposCount,
                      int fallback);

// Rasterises one frame. `out` is reused across frames to avoid reallocating a raster-sized
// buffer every frame; it is resized and cleared as needed.
void Build(const Input& in, const RasterFrameMap& map, const Palette& pal, const Options& opt,
           Result& out);

} // namespace copraster
} // namespace adm::core
