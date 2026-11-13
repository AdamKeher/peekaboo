#include <gtest/gtest.h>

#include <vector>

#include "core/FrameDigest.h"

using namespace adm::core;

namespace {

// A framebuffer with `pad` bytes of slack at the end of every row, filled with something
// that is deliberately NOT the picture - that padding is an allocator detail and must not
// reach the digest.
std::vector<uint8_t> MakeFrame(unsigned w, unsigned h, unsigned bpp, size_t pad, uint8_t seed,
                               uint8_t padFill)
{
    const size_t stride = static_cast<size_t>(w) * bpp + pad;
    std::vector<uint8_t> buf(stride * h, padFill);
    for (unsigned y = 0; y < h; ++y)
        for (size_t x = 0; x < static_cast<size_t>(w) * bpp; ++x)
            buf[y * stride + x] = static_cast<uint8_t>(seed + y * 7 + x * 13);
    return buf;
}

} // namespace

TEST(FrameDigestTest, IsStableForTheSamePicture)
{
    const auto a = MakeFrame(320, 256, 2, 0, 1, 0);
    const auto b = MakeFrame(320, 256, 2, 0, 1, 0);
    EXPECT_EQ(HashFramebuffer(a.data(), 320, 256, 640, 2, 0),
              HashFramebuffer(b.data(), 320, 256, 640, 2, 0));
}

TEST(FrameDigestTest, ChangesWhenOnePixelChanges)
{
    auto a = MakeFrame(64, 32, 2, 0, 5, 0);
    const uint64_t before = HashFramebuffer(a.data(), 64, 32, 128, 2, 0);
    a[17 * 128 + 40] ^= 0x01;
    EXPECT_NE(HashFramebuffer(a.data(), 64, 32, 128, 2, 0), before);
}

// The whole reason the digest walks rows rather than the buffer: two frames showing the
// same picture through buffers of different stride must agree.
TEST(FrameDigestTest, IgnoresPitchPadding)
{
    const auto tight = MakeFrame(100, 50, 4, 0, 9, 0x00);
    const auto padded = MakeFrame(100, 50, 4, 64, 9, 0xFF);
    EXPECT_EQ(HashFramebuffer(tight.data(), 100, 50, 400, 4, 0),
              HashFramebuffer(padded.data(), 100, 50, 464, 4, 0));
}

TEST(FrameDigestTest, SeparatesGeometryAndFormat)
{
    const auto buf = MakeFrame(64, 64, 2, 0, 3, 0);
    const uint64_t base = HashFramebuffer(buf.data(), 64, 64, 128, 2, 0);
    // Same buffer, read as a different shape or a different format. Both reshapes stay
    // inside the allocation - a digest that reads off the end is not a digest.
    EXPECT_NE(HashFramebuffer(buf.data(), 32, 64, 128, 2, 0), base);
    EXPECT_NE(HashFramebuffer(buf.data(), 64, 32, 128, 2, 0), base);
    EXPECT_NE(HashFramebuffer(buf.data(), 64, 64, 128, 2, 1), base);
}

TEST(FrameDigestTest, DegenerateInputsHashToZero)
{
    const auto buf = MakeFrame(8, 8, 2, 0, 1, 0);
    EXPECT_EQ(HashFramebuffer(nullptr, 8, 8, 16, 2, 0), 0u);
    EXPECT_EQ(HashFramebuffer(buf.data(), 0, 8, 16, 2, 0), 0u);
    EXPECT_EQ(HashFramebuffer(buf.data(), 8, 0, 16, 2, 0), 0u);
    EXPECT_EQ(HashFramebuffer(buf.data(), 8, 8, 16, 0, 0), 0u);
}

// A pitch the caller got wrong must not walk off the end of the buffer. The digest is
// allowed to be meaningless here; reading out of bounds is not.
TEST(FrameDigestTest, SurvivesAPitchNarrowerThanARow)
{
    const auto buf = MakeFrame(16, 4, 2, 0, 2, 0);
    EXPECT_NO_FATAL_FAILURE(HashFramebuffer(buf.data(), 16, 4, 4, 2, 0));
}

TEST(AudioDigestTest, BlockBoundariesDoNotChangeTheHash)
{
    std::vector<int16_t> samples(800 * 2);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(i * 37 - 4000);

    AudioDigestAccumulator whole;
    whole.AddSamples(samples.data(), 800);

    AudioDigestAccumulator split;
    split.AddSamples(samples.data(), 300);
    split.AddSamples(samples.data() + 300 * 2, 500);

    EXPECT_EQ(whole.Hash(), split.Hash());
    EXPECT_EQ(whole.Frames(), split.Frames());
    EXPECT_EQ(whole.Frames(), 800u);
}

TEST(AudioDigestTest, CountsFramesSeparatelyFromTheHash)
{
    std::vector<int16_t> silence(1000 * 2, 0);

    AudioDigestAccumulator few;
    few.AddSamples(silence.data(), 441);
    AudioDigestAccumulator many;
    many.AddSamples(silence.data(), 882);

    // Both are silence; the sample count is the only thing that separates them, and it is
    // the signal that catches a rate fault (one write to $DFF02A halves PuAE's rate).
    EXPECT_NE(few.Hash(), many.Hash());
    EXPECT_EQ(few.Frames(), 441u);
    EXPECT_EQ(many.Frames(), 882u);
}

TEST(AudioDigestTest, ResetReturnsToTheEmptyState)
{
    const std::vector<int16_t> samples(64, 1234);
    AudioDigestAccumulator acc;
    acc.AddSamples(samples.data(), 32);
    acc.Reset();

    const AudioDigestAccumulator fresh;
    EXPECT_EQ(acc.Hash(), fresh.Hash());
    EXPECT_EQ(acc.Frames(), 0u);
}

TEST(AudioDigestTest, ASilentFrameIsNotTheSameAsNoFrame)
{
    const std::vector<int16_t> silence(2, 0);
    AudioDigestAccumulator silent;
    silent.AddSamples(silence.data(), 1);

    const AudioDigestAccumulator nothing;
    EXPECT_NE(silent.Hash(), nothing.Hash());
}

TEST(Fnv1a64Test, MatchesTheKnownVectorForTheEmptyAndShortCase)
{
    EXPECT_EQ(Fnv1a64("", 0), kFnvOffsetBasis);
    // "a" -> 0xaf63dc4c8601ec8c is the published FNV-1a 64 vector; this pins the constants
    // against a typo, and with them every digest ever written into a scenario file.
    EXPECT_EQ(Fnv1a64("a", 1), 0xaf63dc4c8601ec8cull);
    EXPECT_EQ(Fnv1a64("foobar", 6), 0x85944171f73967e8ull);
}

TEST(Fnv1a64Test, ScalarFoldIsEndianIndependentByConstruction)
{
    const uint64_t value = 0x0123456789abcdefull;
    uint8_t le[8];
    for (int i = 0; i < 8; ++i)
        le[i] = static_cast<uint8_t>(value >> (8 * i));
    EXPECT_EQ(Fnv1a64Value(value), Fnv1a64(le, 8));
}
