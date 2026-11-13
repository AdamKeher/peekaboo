#include <gtest/gtest.h>

#include "../src/core/BitplaneDecoder.h"

using namespace adm::core;

namespace {

BitplaneDecodeOptions ManualOptions(int planes = 1, int width = 16, int height = 1)
{
    BitplaneDecodeOptions options;
    options.automatic = false;
    options.manualPlaneCount = planes;
    options.manualWidth = width;
    options.manualHeight = height;
    options.useRegisterPointers = false;
    options.automaticInterleaving = false;
    return options;
}

Custom ActiveRegisters()
{
    Custom regs{};
    regs.dmaconr = 0x0100;
    regs.ddfstrt = 0x0038;
    regs.ddfstop = 0x00D0;
    return regs;
}

} // namespace

TEST(BitplaneDecoderTest, AutomaticModePreservesZeroPlaneState)
{
    const Custom regs = ActiveRegisters();
    BitplaneDecodeOptions options;

    const auto result = BitplaneDecoder::Decode(regs, options, {});

    EXPECT_EQ(result.layout.bplCount, 0);
    EXPECT_FALSE(result.layout.valid);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().severity, BitplaneDiagnosticSeverity::Info);
}

TEST(BitplaneDecoderTest, ManualModeDecodesMappedPlaneData)
{
    const Custom regs = ActiveRegisters();
    auto options = ManualOptions(1, 16, 2);
    options.manualPointers[0] = 0x100;
    const std::vector<uint8_t> bytes = {0x80, 0x01, 0xFF, 0x00};
    const std::vector<BitplaneMemoryView> memory = {
        {bytes.data(), 0x100, static_cast<uint32_t>(bytes.size())}};

    const auto result = BitplaneDecoder::Decode(regs, options, memory);

    ASSERT_TRUE(result.layout.valid);
    EXPECT_EQ(result.provenance.width, BitplaneValueSource::Manual);
    EXPECT_EQ(result.provenance.pointers, BitplaneValueSource::Manual);
    ASSERT_EQ(result.planeBits[0].size(), 32u);
    EXPECT_EQ(result.planeBits[0][0], 1);
    EXPECT_EQ(result.planeBits[0][15], 1);
    EXPECT_EQ(result.planeBits[0][16], 1);
    EXPECT_EQ(result.planeBits[0][31], 0);
    EXPECT_TRUE(result.planeStatus[0].complete());
}

TEST(BitplaneDecoderTest, PartialMemoryProducesDataAndDiagnostic)
{
    const Custom regs = ActiveRegisters();
    auto options = ManualOptions(1, 16, 2);
    options.manualPointers[0] = 0x200;
    const std::vector<uint8_t> bytes = {0xFF};
    const std::vector<BitplaneMemoryView> memory = {{bytes.data(), 0x200, 1}};

    const auto result = BitplaneDecoder::Decode(regs, options, memory);

    EXPECT_EQ(result.planeStatus[0].requestedBytes, 4u);
    EXPECT_EQ(result.planeStatus[0].readableBytes, 1u);
    EXPECT_TRUE(result.planeStatus[0].partial());
    EXPECT_TRUE(result.HasWarnings());
    EXPECT_EQ(result.planeBits[0][0], 1);
    EXPECT_EQ(result.planeBits[0][8], 0);
}

TEST(BitplaneDecoderTest, ForcedInterleavingUsesPlaneGroupStride)
{
    Custom regs = ActiveRegisters();
    regs.bpl1mod = 2;
    regs.bpl2mod = 2;
    auto options = ManualOptions(2, 16, 2);
    options.manualPointers[0] = 0x300;
    options.manualPointers[1] = 0x302;
    options.forcedInterleaving = true;
    const std::vector<uint8_t> bytes = {
        0x80, 0x00, 0x40, 0x00, // row 0, planes 1 and 2
        0x20, 0x00, 0x10, 0x00, // row 1, planes 1 and 2
    };
    const std::vector<BitplaneMemoryView> memory = {
        {bytes.data(), 0x300, static_cast<uint32_t>(bytes.size())}};

    const auto result = BitplaneDecoder::Decode(regs, options, memory);

    EXPECT_TRUE(result.layout.interleaved);
    EXPECT_EQ(result.layout.StrideBytes(0), 4);
    EXPECT_EQ(result.planeBits[0][0], 1);
    EXPECT_EQ(result.planeBits[0][16 + 2], 1);
    EXPECT_EQ(result.planeBits[1][1], 1);
    EXPECT_EQ(result.planeBits[1][16 + 3], 1);
}

TEST(BitplaneDecoderTest, DiwHighExtendsVerticalGeometry)
{
    Custom regs = ActiveRegisters();
    regs.bplcon0 = 0x1000;
    regs.diwstrt = 0xF000;
    regs.diwstop = 0x2000;
    regs.diwhigh = 0x0100; // stop V8 = 1, giving 0x120 - 0x0F0 = 48 lines
    BitplaneDecodeOptions options;

    const auto result = BitplaneDecoder::Decode(regs, options, {}, false);

    EXPECT_EQ(result.layout.screenHeight, 48);
    EXPECT_EQ(result.layout.diwVStart, 0xF0);
}

TEST(BitplaneDecoderTest, SeparatesDisplayWindowFromFetchWidth)
{
    Custom regs = ActiveRegisters();
    regs.bplcon0 = 0x1000;
    regs.diwstrt = 0x2C81;
    regs.diwstop = 0x2CC1;
    BitplaneDecodeOptions options;

    const auto result = BitplaneDecoder::Decode(regs, options, {}, false);

    EXPECT_EQ(result.layout.fetchWidth, 320);
    EXPECT_EQ(result.layout.visibleWidth, 320);
    EXPECT_EQ(result.layout.fetchUnitBytes, 2);
}

// --- Shared DDF fetch formula ------------------------------------------------------------
// The Hardware Reference Manual's standard DDF windows, which every resolution-aware width
// calculation in the app now goes through.

TEST(FetchWidthTest, StandardWindowsMatchHardwareReferenceManual)
{
    // lores $38/$D0 -> 20 words -> 320 lores px
    EXPECT_EQ(FetchWordsPerRow(0x38, 0xD0, BitplaneResolution::Lores), 20);
    EXPECT_EQ(FetchWidthPx(0x38, 0xD0, BitplaneResolution::Lores), 320);

    // hires $3C/$D4 -> 40 words -> 640 hires px (2 trailing words, not 1)
    EXPECT_EQ(FetchWordsPerRow(0x3C, 0xD4, BitplaneResolution::Hires), 40);
    EXPECT_EQ(FetchWidthPx(0x3C, 0xD4, BitplaneResolution::Hires), 640);

    // super-hires over the same span fetches twice as many words again
    EXPECT_EQ(FetchWidthPx(0x3C, 0xD4, BitplaneResolution::SuperHires), 1280);
}

TEST(FetchWidthTest, EmptyOrInvertedWindowYieldsZero)
{
    EXPECT_EQ(FetchWordsPerRow(0xD0, 0x38, BitplaneResolution::Lores), 0);
    EXPECT_EQ(FetchWidthPx(0x38, 0x38, BitplaneResolution::Hires), 0);
}

TEST(FetchWidthTest, ResolutionSpansTheSameColorClocks)
{
    // Same DDF span in lores units at every resolution: finer modes fetch more words rather
    // than covering more of the scanline. This is what lets the geometry overlay scale a
    // lores span by pixels-per-lores to get the native width.
    const int lores = FetchWidthPx(0x38, 0xD0, BitplaneResolution::Lores);
    const int hires = FetchWidthPx(0x38, 0xD0, BitplaneResolution::Hires);
    const int shres = FetchWidthPx(0x38, 0xD0, BitplaneResolution::SuperHires);
    EXPECT_EQ(hires, lores * 2);
    EXPECT_EQ(shres, lores * 4);
}

// --- Screen mode summary -----------------------------------------------------------------

TEST(ScreenModeTest, HiresScreenReportsNativePixelsNotLoresUnits)
{
    // A 640x256 hires screen has exactly the same DIW as a 320x256 lores one — reading the
    // registers literally is what made the Display window's geometry overlay say 320x256.
    Custom regs{};
    regs.bplcon0 = 0x8000 | (4 << 12); // HIRES, BPU=4
    regs.dmaconr = 0x0100;
    regs.diwstrt = 0x2C81;
    regs.diwstop = 0x2CC1;
    regs.ddfstrt = 0x003C;
    regs.ddfstop = 0x00D4;

    const ScreenMode m = DescribeScreenMode(regs);
    EXPECT_EQ(m.resolution, BitplaneResolution::Hires);
    EXPECT_EQ(m.pixelsPerLores, 2);
    EXPECT_EQ(m.bitplanes, 4);
    EXPECT_EQ(m.displayWidth, 640);
    EXPECT_EQ(m.displayHeight, 256);
    EXPECT_EQ(m.fetchWidth, 640);
    EXPECT_FALSE(m.interlaced);
    EXPECT_TRUE(m.bitplaneDma);
    EXPECT_EQ(ScreenModeSummary(m), "640x256 hires 4bpl");
}

TEST(ScreenModeTest, LoresScreenWithSameDiwIsHalfAsWide)
{
    Custom regs{};
    regs.bplcon0 = (3 << 12); // lores, BPU=3
    regs.dmaconr = 0x0100;
    regs.diwstrt = 0x2C81;
    regs.diwstop = 0x2CC1;
    regs.ddfstrt = 0x0038;
    regs.ddfstop = 0x00D0;

    const ScreenMode m = DescribeScreenMode(regs);
    EXPECT_EQ(m.pixelsPerLores, 1);
    EXPECT_EQ(m.displayWidth, 320);
    EXPECT_EQ(m.displayHeight, 256);
    EXPECT_EQ(ScreenModeSummary(m), "320x256 lores 3bpl");
}

TEST(ScreenModeTest, SuperHiresBeatsHiresBit)
{
    Custom regs{};
    regs.bplcon0 = 0x8000 | 0x0040 | (2 << 12); // both bits set: SHRES wins
    regs.diwstrt = 0x2C81;
    regs.diwstop = 0x2CC1;
    regs.ddfstrt = 0x003C;
    regs.ddfstop = 0x00D4;

    const ScreenMode m = DescribeScreenMode(regs);
    EXPECT_EQ(m.resolution, BitplaneResolution::SuperHires);
    EXPECT_EQ(m.pixelsPerLores, 4);
    EXPECT_EQ(m.displayWidth, 1280);
}

TEST(ScreenModeTest, InterlaceDoublesReportedHeight)
{
    Custom regs{};
    regs.bplcon0 = 0x8000 | 0x0004 | (4 << 12); // HIRES + LACE
    regs.diwstrt = 0x2C81;
    regs.diwstop = 0x2CC1;
    regs.ddfstrt = 0x003C;
    regs.ddfstop = 0x00D4;

    const ScreenMode m = DescribeScreenMode(regs);
    EXPECT_TRUE(m.interlaced);
    EXPECT_EQ(m.displayHeight, 512);
    EXPECT_EQ(ScreenModeSummary(m), "640x512 hires lace 4bpl");
}

TEST(ScreenModeTest, AgaEighthBitplaneIsNotClamped)
{
    Custom regs{};
    regs.bplcon0 = 0x0010 | (0 << 12) | 0x0800; // BPU3 set -> 8 planes, HAM
    regs.diwstrt = 0x2C81;
    regs.diwstop = 0x2CC1;
    regs.ddfstrt = 0x0038;
    regs.ddfstop = 0x00D0;

    const ScreenMode m = DescribeScreenMode(regs);
    EXPECT_EQ(m.bitplanes, 8);
    EXPECT_EQ(m.displayMode, BitplaneDisplayMode::Ham8);
    EXPECT_EQ(ScreenModeSummary(m), "320x256 lores 8bpl HAM8");
}
