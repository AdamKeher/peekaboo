#include <gtest/gtest.h>
#include "core/DiskCompare.h"

using adm::core::CompareDiskImages;
using adm::core::HashSector;

namespace {
constexpr int kTracks = 160;
constexpr int kSectors = 11;
constexpr int kBytes = 512;
constexpr size_t kImageBytes = static_cast<size_t>(kTracks) * kSectors * kBytes;

std::vector<uint8_t> MakeImage()
{
    std::vector<uint8_t> image(kImageBytes);
    for (size_t i = 0; i < image.size(); ++i)
        image[i] = static_cast<uint8_t>(i * 31 + (i >> 9));
    return image;
}
} // namespace

TEST(DiskCompareTest, IdenticalImagesMatchEveryTrack)
{
    const auto image = MakeImage();
    const auto results = CompareDiskImages(image, image, kTracks, kSectors, kBytes);
    ASSERT_EQ(results.size(), static_cast<size_t>(kTracks));
    for (const auto& r : results) {
        EXPECT_TRUE(r.Matches());
        EXPECT_EQ(r.missingSectors, 0u);
    }
}

TEST(DiskCompareTest, OneChangedByteFlagsOnlyItsSector)
{
    const auto a = MakeImage();
    auto b = a;
    // Track 41, sector 7, last byte.
    const size_t offset = (41 * kSectors + 7) * kBytes + kBytes - 1;
    b[offset] ^= 0x01;

    const auto results = CompareDiskImages(a, b, kTracks, kSectors, kBytes);
    for (int t = 0; t < kTracks; ++t) {
        if (t == 41)
            EXPECT_EQ(results[t].differingSectors, 1u << 7);
        else
            EXPECT_TRUE(results[t].Matches()) << "track " << t;
    }
}

TEST(DiskCompareTest, ShortImageReportsMissingSectors)
{
    const auto a = MakeImage();
    auto b = a;
    b.resize(kImageBytes - 2 * kBytes); // the last two sectors of track 159 are gone

    const auto results = CompareDiskImages(a, b, kTracks, kSectors, kBytes);
    const uint32_t lastTwo = (1u << 9) | (1u << 10);
    EXPECT_EQ(results[159].differingSectors, lastTwo);
    EXPECT_EQ(results[159].missingSectors, lastTwo);
    EXPECT_TRUE(results[158].Matches());
}

TEST(DiskCompareTest, HashSeparatesSectorsThatDiffer)
{
    std::vector<uint8_t> zeros(kBytes, 0);
    std::vector<uint8_t> one = zeros;
    one[0] = 1;
    EXPECT_EQ(HashSector(zeros.data(), zeros.size()), HashSector(zeros.data(), zeros.size()));
    EXPECT_NE(HashSector(zeros.data(), zeros.size()), HashSector(one.data(), one.size()));
}
