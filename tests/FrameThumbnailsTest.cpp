#include <gtest/gtest.h>

#include <vector>

#include "../src/core/FrameThumbnails.h"

using adm::core::FrameThumbnails;

namespace {

constexpr int kW = 32;
constexpr int kH = 24;
constexpr size_t kBytes = static_cast<size_t>(kW) * kH * 3;

// A screen that is mostly static: a fixed background with one small moving block, which is
// the shape the delta codec is meant to exploit.
std::vector<uint8_t> MakeScreen(int step)
{
    std::vector<uint8_t> img(kBytes, 0x20);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const size_t off = (static_cast<size_t>(y + 4) * kW + (x + step % 16)) * 3;
            img[off + 0] = 0xFF;
            img[off + 1] = static_cast<uint8_t>(step);
            img[off + 2] = 0x00;
        }
    }
    return img;
}

FrameThumbnails MakeStore(int count, int stride = 10)
{
    FrameThumbnails t;
    for (int i = 0; i < count; ++i) {
        const auto img = MakeScreen(i);
        EXPECT_TRUE(t.Record(static_cast<uint64_t>(i * stride), img.data(), kW, kH));
    }
    return t;
}

} // namespace

TEST(FrameThumbnailsTest, EmptyStoreHasNothing)
{
    FrameThumbnails t;
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    uint64_t got = 0;
    EXPECT_FALSE(t.GetNearest(0, out, w, h, got));
    EXPECT_EQ(t.Count(), 0u);
}

TEST(FrameThumbnailsTest, RoundTripsEveryShot)
{
    FrameThumbnails t = MakeStore(50);
    ASSERT_EQ(t.Count(), 50u);

    for (int i = 0; i < 50; ++i) {
        std::vector<uint8_t> out;
        int w = 0, h = 0;
        ASSERT_TRUE(t.Get(static_cast<uint64_t>(i * 10), out, w, h)) << "shot " << i;
        EXPECT_EQ(w, kW);
        EXPECT_EQ(h, kH);
        EXPECT_EQ(out, MakeScreen(i)) << "shot " << i;
    }
}

TEST(FrameThumbnailsTest, DeltasCompressAStaticScreen)
{
    FrameThumbnails t = MakeStore(64);
    const auto s = t.Stats();
    EXPECT_EQ(s.imageBytes, kBytes);
    // A 4x4 moving block on a static background must not cost anything close to a whole
    // image per shot - that is the entire reason for the delta chain.
    EXPECT_LT(s.compressionRatio, 0.5);
    EXPECT_GT(s.keyframeCount, 0u);
}

TEST(FrameThumbnailsTest, NearestFallsBackToTheShotBefore)
{
    FrameThumbnails t = MakeStore(10); // frames 0,10,20,...,90

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    uint64_t got = 0;
    ASSERT_TRUE(t.GetNearest(37, out, w, h, got));
    EXPECT_EQ(got, 30u);
    EXPECT_EQ(out, MakeScreen(3));

    // Past the end clamps to the newest shot rather than failing.
    ASSERT_TRUE(t.GetNearest(1000, out, w, h, got));
    EXPECT_EQ(got, 90u);

    // Before everything held, the oldest is the closest there is.
    FrameThumbnails t2;
    const auto img = MakeScreen(1);
    ASSERT_TRUE(t2.Record(500, img.data(), kW, kH));
    ASSERT_TRUE(t2.GetNearest(10, out, w, h, got));
    EXPECT_EQ(got, 500u);
}

TEST(FrameThumbnailsTest, ExactGetRejectsAFrameWithNoShot)
{
    FrameThumbnails t = MakeStore(5);
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    EXPECT_FALSE(t.Get(7, out, w, h));
    EXPECT_TRUE(t.Get(10, out, w, h));
}

TEST(FrameThumbnailsTest, RejectsOutOfOrderFrames)
{
    FrameThumbnails t = MakeStore(3); // 0, 10, 20
    const auto img = MakeScreen(9);
    EXPECT_FALSE(t.Record(20, img.data(), kW, kH));
    EXPECT_FALSE(t.Record(5, img.data(), kW, kH));
    EXPECT_EQ(t.Count(), 3u);
}

TEST(FrameThumbnailsTest, TrimAfterDropsTheFutureAndKeepsRecordingUsable)
{
    FrameThumbnails t = MakeStore(40);
    t.TrimAfter(200); // keeps 0..200

    uint64_t oldest = 0, newest = 0;
    ASSERT_TRUE(t.Range(oldest, newest));
    EXPECT_EQ(newest, 200u);
    EXPECT_EQ(t.Count(), 21u);

    // Appending after a trim must still decode: the delta chain is re-anchored on what
    // survived, not on the shot that was thrown away.
    const auto img = MakeScreen(99);
    ASSERT_TRUE(t.Record(210, img.data(), kW, kH));
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(t.Get(210, out, w, h));
    EXPECT_EQ(out, img);
    ASSERT_TRUE(t.Get(200, out, w, h));
    EXPECT_EQ(out, MakeScreen(20));
}

TEST(FrameThumbnailsTest, PruneBeforeKeepsEverythingStillDecodable)
{
    FrameThumbnails t = MakeStore(64);
    t.PruneBefore(300);

    uint64_t oldest = 0, newest = 0;
    ASSERT_TRUE(t.Range(oldest, newest));
    EXPECT_LE(oldest, 300u);
    EXPECT_EQ(newest, 630u);

    // Whatever survived must still reconstruct - a group is never half-dropped.
    for (uint64_t f = oldest; f <= newest; f += 10) {
        std::vector<uint8_t> out;
        int w = 0, h = 0;
        ASSERT_TRUE(t.Get(f, out, w, h)) << "frame " << f;
        EXPECT_EQ(out, MakeScreen(static_cast<int>(f / 10))) << "frame " << f;
    }
}

TEST(FrameThumbnailsTest, BudgetEvictsOldestGroupsAndLeavesTheRestDecodable)
{
    FrameThumbnails t;
    t.SetBudgetBytes(kBytes * 8); // room for a handful of groups at most
    for (int i = 0; i < 200; ++i) {
        const auto img = MakeScreen(i);
        ASSERT_TRUE(t.Record(static_cast<uint64_t>(i * 10), img.data(), kW, kH));
    }

    uint64_t oldest = 0, newest = 0;
    ASSERT_TRUE(t.Range(oldest, newest));
    EXPECT_EQ(newest, 1990u);
    EXPECT_GT(oldest, 0u) << "nothing was evicted, so the budget was not enforced";
    EXPECT_LT(t.Count(), 200u);

    for (uint64_t f = oldest; f <= newest; f += 10) {
        std::vector<uint8_t> out;
        int w = 0, h = 0;
        ASSERT_TRUE(t.Get(f, out, w, h)) << "frame " << f;
        EXPECT_EQ(out, MakeScreen(static_cast<int>(f / 10))) << "frame " << f;
    }
}

TEST(FrameThumbnailsTest, GeometryChangeClearsTheChain)
{
    FrameThumbnails t = MakeStore(20);

    // A resolution change makes the held shots useless as delta bases and unshowable at the
    // new size, so they go rather than decoding to garbage.
    std::vector<uint8_t> big(static_cast<size_t>(64) * 48 * 3, 0x11);
    ASSERT_TRUE(t.Record(1000, big.data(), 64, 48));
    EXPECT_EQ(t.Count(), 1u);

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(t.Get(1000, out, w, h));
    EXPECT_EQ(w, 64);
    EXPECT_EQ(h, 48);
    EXPECT_EQ(out, big);
}

TEST(FrameThumbnailsTest, RefusesAnImageLargerThanTheBudget)
{
    FrameThumbnails t;
    t.SetBudgetBytes(kBytes / 2);
    const auto img = MakeScreen(0);
    EXPECT_FALSE(t.Record(0, img.data(), kW, kH));
    EXPECT_EQ(t.Count(), 0u);
}
