#include <gtest/gtest.h>

#include "../src/core/BlitDestImage.h"

using namespace adm::core;
using BlitWord = adm::AmigaEmulator::BlitWord;

namespace {

// A plain area-mode copy blit: `words` words per row, `rows` rows, D modulo `dmod` bytes.
BlitRecord MakeAreaBlit(uint16_t words, uint16_t rows, int16_t dmod, uint32_t dest = 0x20000)
{
    BlitRecord rec{};
    rec.raw.bltdpt = dest;
    rec.raw.bltdmod = static_cast<uint16_t>(dmod);
    rec.raw.bltcon0 = 0x09F0; // USEA|USED, minterm $F0 (D = A)
    rec.raw.bltcon1 = 0x0000;
    rec.wordsPerRow = words;
    rec.rows = rows;
    rec.useA = true;
    rec.useD = true;
    rec.minterm = 0xF0;
    rec.lineMode = false;
    return rec;
}

// Emits the D writes an ascending area blit would produce, filling every word with `fill`.
std::vector<BlitWord> AscendingWrites(const BlitRecord& rec, uint16_t fill)
{
    std::vector<BlitWord> out;
    const uint32_t stride =
        static_cast<uint32_t>(rec.wordsPerRow) * 2u + static_cast<int16_t>(rec.raw.bltdmod);
    for (uint16_t r = 0; r < rec.rows; ++r)
        for (uint16_t c = 0; c < rec.wordsPerRow; ++c)
            out.push_back({rec.raw.bltdpt + r * stride + c * 2u, fill, 0});
    return out;
}

} // namespace

TEST(BlitDestImageTest, EmptyCaptureIsInvalid)
{
    const BlitRecord rec = MakeAreaBlit(2, 4, 0);
    const BlitDestImage img = BuildBlitDestImage(rec, nullptr, 0);
    EXPECT_FALSE(img.valid);
}

TEST(BlitDestImageTest, AreaBlitGeometry)
{
    const BlitRecord rec = MakeAreaBlit(/*words=*/3, /*rows=*/5, /*dmod=*/0);
    const auto words = AscendingWrites(rec, 0xFFFF);

    const BlitDestImage img = BuildBlitDestImage(rec, words.data(), words.size());
    ASSERT_TRUE(img.valid);
    EXPECT_EQ(img.width, 48); // 3 words x 16 px
    EXPECT_EQ(img.height, 5);
    EXPECT_EQ(img.baseAddr, 0x20000u);
    EXPECT_EQ(img.rowStride, 6u);
    EXPECT_EQ(img.wordsCaptured, 15u);

    // Every pixel written, every bit set.
    for (uint8_t px : img.pixels)
        EXPECT_EQ(px, BlitDestImage::kSet);
}

TEST(BlitDestImageTest, BitOrderIsMsbLeft)
{
    const BlitRecord rec = MakeAreaBlit(1, 1, 0);
    const std::vector<BlitWord> words = {{0x20000u, 0x8001u, 0}};

    const BlitDestImage img = BuildBlitDestImage(rec, words.data(), words.size());
    ASSERT_TRUE(img.valid);
    ASSERT_EQ(img.width, 16);
    EXPECT_EQ(img.pixels[0], BlitDestImage::kSet);  // bit 15 -> leftmost pixel
    EXPECT_EQ(img.pixels[15], BlitDestImage::kSet); // bit 0  -> rightmost pixel
    for (int x = 1; x < 15; ++x)
        EXPECT_EQ(img.pixels[x], BlitDestImage::kClear) << "x=" << x;
}

TEST(BlitDestImageTest, ModuloGapIsMarkedUntouched)
{
    // 2 words per row with a 4-byte modulo: each row spans 8 bytes, only the first 4 written.
    const BlitRecord rec = MakeAreaBlit(/*words=*/2, /*rows=*/3, /*dmod=*/4);
    const auto words = AscendingWrites(rec, 0xFFFF);

    const BlitDestImage img = BuildBlitDestImage(rec, words.data(), words.size());
    ASSERT_TRUE(img.valid);
    EXPECT_EQ(img.rowStride, 8u);
    // The image is only as wide as the blit — the gap belongs to the next row, not this one.
    EXPECT_EQ(img.width, 32);
    EXPECT_EQ(img.height, 3);
    for (uint8_t px : img.pixels)
        EXPECT_EQ(px, BlitDestImage::kSet);
}

TEST(BlitDestImageTest, DescendingBlitLandsTheSameWayUp)
{
    BlitRecord rec = MakeAreaBlit(/*words=*/2, /*rows=*/3, /*dmod=*/0);
    rec.raw.bltcon1 |= 0x0002; // DESC

    // A descending blit writes the last word first. Distinguish rows by their contents so a
    // reversed reconstruction would be visible.
    std::vector<BlitWord> words;
    for (int r = 2; r >= 0; --r)
        for (int c = 1; c >= 0; --c)
            words.push_back(
                {0x20000u + static_cast<uint32_t>(r) * 4u + static_cast<uint32_t>(c) * 2u,
                 static_cast<uint16_t>(r == 0 ? 0xFFFF : 0x0000), 0});

    const BlitDestImage img = BuildBlitDestImage(rec, words.data(), words.size());
    ASSERT_TRUE(img.valid);
    EXPECT_EQ(img.width, 32);
    EXPECT_EQ(img.height, 3);
    // Addresses place the words, not write order, so the set row is still row 0.
    for (int x = 0; x < 32; ++x) {
        EXPECT_EQ(img.pixels[x], BlitDestImage::kSet) << "row 0 x=" << x;
        EXPECT_EQ(img.pixels[32 + x], BlitDestImage::kClear) << "row 1 x=" << x;
    }
}

TEST(BlitDestImageTest, PartialCaptureLeavesTheRestUntouched)
{
    const BlitRecord rec = MakeAreaBlit(/*words=*/2, /*rows=*/4, /*dmod=*/0);
    auto words = AscendingWrites(rec, 0xFFFF);
    words.resize(2); // only the first row came back

    const BlitDestImage img = BuildBlitDestImage(rec, words.data(), words.size());
    ASSERT_TRUE(img.valid);
    // Height follows what was captured, not what the blit programmed.
    EXPECT_EQ(img.height, 1);
    EXPECT_EQ(img.width, 32);
}

TEST(BlitDestImageTest, LineModeUsesBltdmodAsRowStride)
{
    BlitRecord rec{};
    rec.raw.bltdpt = 0x30000;
    rec.raw.bltdmod = 40;     // one 320px lores bitplane row
    rec.raw.bltcon1 = 0x0001; // LINE
    rec.lineMode = true;
    rec.wordsPerRow = 2; // BLTSIZE encodes something else entirely in line mode
    rec.rows = 4;
    rec.useD = true;

    // One word per scanline, stepping right by a word every other line.
    const std::vector<BlitWord> words = {
        {0x30000u + 0 * 40u + 0, 0x8000, 0},
        {0x30000u + 1 * 40u + 0, 0x0001, 0},
        {0x30000u + 2 * 40u + 2, 0x8000, 0},
        {0x30000u + 3 * 40u + 2, 0x0001, 0},
    };

    const BlitDestImage img = BuildBlitDestImage(rec, words.data(), words.size());
    ASSERT_TRUE(img.valid);
    EXPECT_EQ(img.rowStride, 40u);
    EXPECT_EQ(img.width, 320); // 40 bytes = 20 words = 320 px
    EXPECT_EQ(img.height, 4);

    // Row 0 word 0 bit 15, row 3 word 1 bit 0 — i.e. a diagonal marching right and down.
    EXPECT_EQ(img.pixels[0], BlitDestImage::kSet);
    EXPECT_EQ(img.pixels[static_cast<size_t>(3) * 320 + 31], BlitDestImage::kSet);
    // Nothing was written past the two words each row touched.
    EXPECT_EQ(img.pixels[32], BlitDestImage::kUntouched);
}

TEST(BlitDestImageTest, ImplausibleStrideIsRejected)
{
    // 4096 words per row is beyond anything the blitter can address in one row; refuse
    // rather than allocating a huge image from a misread geometry.
    const BlitRecord rec = MakeAreaBlit(/*words=*/4096, /*rows=*/1, /*dmod=*/0);
    const std::vector<BlitWord> words = {{0x20000u, 0xFFFF, 0}};

    const BlitDestImage img = BuildBlitDestImage(rec, words.data(), words.size());
    EXPECT_FALSE(img.valid);
}
