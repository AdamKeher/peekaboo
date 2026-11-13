#include <gtest/gtest.h>

#include "../src/core/BlitMemoryMapper.h"

using namespace adm::core;

namespace {

// 320-bit-wide grid (40 bytes/row), 256 rows tall, single column visible (texW just over one
// column). Memory starts at 0x10000 so address math is easy to follow.
MemoryGridLayout MakeGrid(uint32_t start = 0x10000)
{
    MemoryGridLayout g;
    g.startAddress = start;
    g.colW = 320; // 40 bytes/row
    g.texW = 360; // one column (colStride 321) fits
    g.texH = 256;
    return g;
}

BlitRecord MakeBlit(uint32_t dst, uint16_t rows, uint16_t words)
{
    BlitRecord r{};
    r.useD = true;
    r.rows = rows;
    r.wordsPerRow = words;
    r.raw.bltdpt = dst;
    return r;
}

int CountChannel(const std::vector<BlitMemRect>& v, BlitMemChannel ch)
{
    int n = 0;
    for (const auto& r : v)
        if (r.channel == ch)
            ++n;
    return n;
}

} // namespace

TEST(BlitMemoryMapper, DestRunFittingInOneRowIsOneRect)
{
    auto g = MakeGrid();
    // 4 rows × 2 words (4 bytes/row) at byte offset 6 of row 3: 0x10000 + 3*40 + 6
    auto blit = MakeBlit(0x10000 + 3 * 40 + 6, /*rows=*/4, /*words=*/2);
    auto rects = MapBlitToMemoryGrid(blit, g);

    ASSERT_EQ(CountChannel(rects, BlitMemChannel::Dest), 4); // one rect per blit row
    const auto& r0 = rects.front();
    EXPECT_EQ(r0.channel, BlitMemChannel::Dest);
    EXPECT_EQ(r0.y0, 3);
    EXPECT_EQ(r0.y1, 4);
    EXPECT_EQ(r0.x0, 6 * 8);       // byteInRow 6 → pixel 48
    EXPECT_EQ(r0.x1, (6 + 4) * 8); // 4 bytes wide
    // This blit's 4-byte row stride is contiguous in linear memory, so the next blit row
    // continues along the same grid row (the map shows raw memory, not screen geometry).
    EXPECT_EQ(rects[1].y0, 3);
    EXPECT_EQ(rects[1].x0, (6 + 4) * 8); // 80
}

TEST(BlitMemoryMapper, RunWrappingRowBoundarySplitsIntoTwoRects)
{
    auto g = MakeGrid();
    // Start near the end of a row so the 8-byte run spills onto the next grid row.
    // byteInRow 38, run 8 bytes → 2 bytes on row 0, 6 bytes wrap to row 1.
    auto blit = MakeBlit(0x10000 + 38, /*rows=*/1, /*words=*/4);
    auto rects = MapBlitToMemoryGrid(blit, g);

    ASSERT_EQ(rects.size(), 2u);
    EXPECT_EQ(rects[0].y0, 0);
    EXPECT_EQ(rects[0].x0, 38 * 8);
    EXPECT_EQ(rects[0].x1, 40 * 8); // clipped at row end (40 bytes)
    EXPECT_EQ(rects[1].y0, 1);
    EXPECT_EQ(rects[1].x0, 0);
    EXPECT_EQ(rects[1].x1, 6 * 8);
}

TEST(BlitMemoryMapper, EmitsSourceChannelsWithDistinctTags)
{
    auto g = MakeGrid();
    auto blit = MakeBlit(0x10000 + 100, /*rows=*/2, /*words=*/1);
    blit.useA = true;
    blit.raw.bltapt = 0x10000 + 500;
    blit.useC = true;
    blit.raw.bltcpt = 0x10000 + 900;

    auto rects = MapBlitToMemoryGrid(blit, g);
    EXPECT_EQ(CountChannel(rects, BlitMemChannel::SourceA), 2);
    EXPECT_EQ(CountChannel(rects, BlitMemChannel::Dest), 2);
    EXPECT_EQ(CountChannel(rects, BlitMemChannel::SourceC), 2);
    EXPECT_EQ(CountChannel(rects, BlitMemChannel::SourceB), 0); // B not used
}

TEST(BlitMemoryMapper, AddressesOutsideViewProduceNoRects)
{
    auto g = MakeGrid(0x10000);
    // Destination is below the visible window's start address.
    auto blit = MakeBlit(0x8000, /*rows=*/4, /*words=*/2);
    EXPECT_TRUE(MapBlitToMemoryGrid(blit, g).empty());
}

TEST(BlitMemoryMapper, DescendingModeNormalizesToLowestAddress)
{
    auto g = MakeGrid();
    const uint16_t rows = 4, words = 2;
    const uint32_t startAddr = 0x10000 + 3 * 40 + 6;
    const uint32_t rowStep = words * 2u; // modulo 0
    const uint32_t endAddr = startAddr + (rows - 1) * rowStep + (words * 2u - 2u);

    auto blit = MakeBlit(endAddr, rows, words);
    blit.raw.bltcon1 = 0x0002; // DESC
    auto rects = MapBlitToMemoryGrid(blit, g);

    ASSERT_FALSE(rects.empty());
    // Same lowest-address row 3 as the ascending case.
    EXPECT_EQ(rects.front().y0, 3);
    EXPECT_EQ(rects.front().x0, 6 * 8);
}
