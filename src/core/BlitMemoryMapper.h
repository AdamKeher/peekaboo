#pragma once

#include <cstdint>
#include <vector>

#include "BlitProfiler.h"

namespace adm::core {

// Which blit channel a highlighted memory rectangle belongs to. The Memory Map shows all of
// chip RAM regardless of screen geometry, so unlike the emulator/bitplane overlays it can
// also surface the read channels (A/B/C), not just the destination.
enum class BlitMemChannel { Dest, SourceA, SourceB, SourceC };

// One highlight rectangle in Memory Map texture pixel space (origin top-left). A blit row can
// span several rectangles because the column-major grid wraps a byte run onto the next grid
// row (and skips the inter-column gap). Heights are always one grid row.
struct BlitMemRect
{
    BlitMemChannel channel = BlitMemChannel::Dest;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // pixels; [x0,x1) × [y0,y1)
};

// Geometry of the Memory Map's main view, mirroring MemoryMapView::DoRenderPixels: memory is
// laid out column-major — each column is `colW` pixels wide (1 bit = 1 pixel) plus a 1px gap,
// holds `texH` rows of `colW/8` bytes, and columns advance by a whole column's worth of bytes.
struct MemoryGridLayout
{
    uint32_t startAddress = 0;
    int colW = 0; // row width in bits/pixels (multiple of 16)
    int texW = 0;
    int texH = 0;

    bool valid() const { return colW >= 8 && texW > 0 && texH > 0; }
    int colStride() const { return colW + 1; }
    int bytesPerRow() const { return colW / 8; }
    int bytesPerCol() const { return bytesPerRow() * texH; }
    uint32_t span() const
    {
        const int cols = (texW + colStride() - 1) / colStride();
        return static_cast<uint32_t>(cols) * static_cast<uint32_t>(bytesPerCol());
    }
};

namespace detail {

// Emits one rectangle per grid-row segment for the byte run [addr, addr+nbytes). Stops once
// the run leaves the mapped span or `out` hits `cap` (a guard against pathological blits).
inline void EmitMemRun(uint32_t addr, uint32_t nbytes, BlitMemChannel ch, const MemoryGridLayout& g,
                       std::vector<BlitMemRect>& out, size_t cap)
{
    const uint32_t base = g.startAddress;
    const uint32_t span = g.span();
    const int bytesPerRow = g.bytesPerRow();
    const int bytesPerCol = g.bytesPerCol();
    const int colStride = g.colStride();

    while (nbytes > 0 && out.size() < cap) {
        if (addr < base || addr >= base + span)
            return;
        const uint32_t off = addr - base;
        const int col = static_cast<int>(off / static_cast<uint32_t>(bytesPerCol));
        const int within = static_cast<int>(off % static_cast<uint32_t>(bytesPerCol));
        const int row = within / bytesPerRow;
        const int byteInRow = within % bytesPerRow;

        const int bytesLeftInRow = bytesPerRow - byteInRow;
        const int seg =
            static_cast<int>(std::min<uint32_t>(nbytes, static_cast<uint32_t>(bytesLeftInRow)));

        BlitMemRect r;
        r.channel = ch;
        r.x0 = col * colStride + byteInRow * 8;
        r.x1 = r.x0 + seg * 8;
        r.y0 = row;
        r.y1 = row + 1;
        out.push_back(r);

        addr += static_cast<uint32_t>(seg);
        nbytes -= static_cast<uint32_t>(seg);
    }
}

// Normalizes a channel pointer to the lowest address of its block, accounting for
// descending-mode blits (BLTCON1 bit1) which point each channel at the END of its data.
inline uint32_t NormalizeChannelStart(uint32_t ptr, uint16_t modulo, const BlitRecord& rec)
{
    if (rec.raw.bltcon1 & 0x0002) {
        const uint32_t rowStep = static_cast<uint32_t>(rec.wordsPerRow) * 2u + modulo;
        ptr -= static_cast<uint32_t>(rec.rows - 1) * rowStep;
        ptr -= static_cast<uint32_t>(rec.wordsPerRow) * 2u - 2u;
    }
    return ptr;
}

inline void EmitChannel(uint32_t ptr, uint16_t modulo, BlitMemChannel ch, const BlitRecord& rec,
                        const MemoryGridLayout& g, std::vector<BlitMemRect>& out, size_t cap)
{
    const uint32_t start = NormalizeChannelStart(ptr, modulo, rec);
    const uint32_t runBytes = static_cast<uint32_t>(rec.wordsPerRow) * 2u;
    const uint32_t rowStep = runBytes + modulo;
    for (int r = 0; r < rec.rows && out.size() < cap; ++r)
        EmitMemRun(start + static_cast<uint32_t>(r) * rowStep, runBytes, ch, g, out, cap);
}

} // namespace detail

// Maps a blit's destination (and any active A/B/C source channels) onto the Memory Map grid,
// returning highlight rectangles in texture pixel space. Pure function — unit-testable.
inline std::vector<BlitMemRect> MapBlitToMemoryGrid(const BlitRecord& rec,
                                                    const MemoryGridLayout& g)
{
    std::vector<BlitMemRect> rects;
    if (!g.valid() || rec.rows == 0 || rec.wordsPerRow == 0)
        return rects;

    constexpr size_t kCap = 8192; // bound output for pathological full-screen blits

    if (rec.useA)
        detail::EmitChannel(rec.raw.bltapt, rec.raw.bltamod, BlitMemChannel::SourceA, rec, g, rects,
                            kCap);
    if (rec.useB)
        detail::EmitChannel(rec.raw.bltbpt, rec.raw.bltbmod, BlitMemChannel::SourceB, rec, g, rects,
                            kCap);
    if (rec.useC)
        detail::EmitChannel(rec.raw.bltcpt, rec.raw.bltcmod, BlitMemChannel::SourceC, rec, g, rects,
                            kCap);
    if (rec.useD)
        detail::EmitChannel(rec.raw.bltdpt, rec.raw.bltdmod, BlitMemChannel::Dest, rec, g, rects,
                            kCap);

    return rects;
}

} // namespace adm::core
