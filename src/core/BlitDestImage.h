#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "BlitProfiler.h"

namespace adm::core {

// A one-bitplane bitmap rebuilt from the words a blit actually wrote to chip RAM.
//
// The words come from the core's D-write capture (AmigaEmulator::BlitWord), each carrying
// its own destination address. That is what makes this exact rather than a reconstruction:
// descending blits, fill mode, masked edges and line-draw blits all place themselves,
// because the layout below is derived from the addresses rather than from write order.
//
// `pixels` is one byte per pixel, row-major, `width` x `height`:
//   0 = the blit wrote this pixel and the bit is clear
//   1 = the blit wrote this pixel and the bit is set
//   2 = the blit never wrote here (the modulo gap, or a row it skipped)
// A blit is one bitplane's worth of bits, so there is no palette to apply — the caller
// picks the two colours and draws "untouched" as transparent.
struct BlitDestImage
{
    bool valid = false;
    int width = 0;          // pixels
    int height = 0;         // rows
    uint32_t baseAddr = 0;  // chip address of the top-left word
    uint32_t rowStride = 0; // bytes between the start of one row and the next
    uint32_t wordsCaptured = 0;
    bool truncated = false; // the capture window was cut short (per-blit cap or ring recycle)
    std::vector<uint8_t> pixels;

    static constexpr uint8_t kClear = 0;
    static constexpr uint8_t kSet = 1;
    static constexpr uint8_t kUntouched = 2;
};

namespace detail {

// A blit big enough to need more than this many rows is almost certainly a stride the
// address arithmetic guessed wrong, not a real bitmap; refuse it rather than allocating.
inline constexpr int kMaxDestRows = 4096;
inline constexpr int kMaxDestWords = 512; // 8192 pixels wide

} // namespace detail

// Rebuilds the destination bitmap from a blit's captured D writes. Returns an invalid
// image when nothing was captured or the geometry does not describe a bitmap.
inline BlitDestImage BuildBlitDestImage(const BlitRecord& rec, const AmigaEmulator::BlitWord* words,
                                        size_t count)
{
    BlitDestImage img;
    if (!words || count == 0)
        return img;

    // Row stride. In area mode the blitter advances D by the row's words plus BLTDMOD; in
    // line mode BLTDMOD *is* the bitplane's row stride, since a line blit writes at most one
    // word per row.
    int stride = rec.lineMode ? static_cast<int>(static_cast<int16_t>(rec.raw.bltdmod))
                              : static_cast<int>(rec.wordsPerRow) * 2 +
                                    static_cast<int>(static_cast<int16_t>(rec.raw.bltdmod));

    uint32_t minAddr = words[0].addr, maxAddr = words[0].addr;
    for (size_t i = 1; i < count; ++i) {
        minAddr = std::min(minAddr, words[i].addr);
        maxAddr = std::max(maxAddr, words[i].addr);
    }

    // A non-positive stride means the rows overlap or run backwards — there is no sane
    // rectangle to lay out. Fall back to a single strip spanning the touched addresses,
    // which at least shows the bits in memory order.
    if (stride <= 0)
        stride = static_cast<int>(maxAddr - minAddr) + 2;

    const int cols = rec.lineMode ? stride / 2 : static_cast<int>(rec.wordsPerRow);
    if (cols <= 0 || cols > detail::kMaxDestWords)
        return img;

    const int rows = static_cast<int>((maxAddr - minAddr) / static_cast<uint32_t>(stride)) + 1;
    if (rows <= 0 || rows > detail::kMaxDestRows)
        return img;

    img.width = cols * 16;
    img.height = rows;
    img.baseAddr = minAddr;
    img.rowStride = static_cast<uint32_t>(stride);
    img.wordsCaptured = static_cast<uint32_t>(count);
    img.pixels.assign(static_cast<size_t>(img.width) * static_cast<size_t>(img.height),
                      BlitDestImage::kUntouched);

    // Oldest write first, so a pixel written twice within the blit ends up showing the
    // value the blit left behind.
    for (size_t i = 0; i < count; ++i) {
        const uint32_t off = words[i].addr - minAddr;
        const int row = static_cast<int>(off / static_cast<uint32_t>(stride));
        const int colByte = static_cast<int>(off % static_cast<uint32_t>(stride));
        if (row < 0 || row >= rows || (colByte & 1) != 0)
            continue;
        const int col = colByte / 2;
        if (col < 0 || col >= cols)
            continue; // lands in the modulo gap between rows

        uint8_t* px = img.pixels.data() + (static_cast<size_t>(row) * img.width) + col * 16;
        const uint16_t v = words[i].val;
        for (int b = 0; b < 16; ++b)
            px[b] = ((v >> (15 - b)) & 1) ? BlitDestImage::kSet : BlitDestImage::kClear;
    }

    img.valid = true;
    return img;
}

} // namespace adm::core
