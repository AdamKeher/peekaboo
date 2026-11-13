#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "../src/core/DiskSearcher.h"

using adm::core::DiskSearcher;

namespace {

constexpr uint32_t kBlockSize = 512;
constexpr uint32_t kBlocks = 8;

// A whole disk image in memory, handed to DiskSearcher one block at a time.
class FakeDisk
{
public:
    FakeDisk() : m_data(kBlocks * kBlockSize, 0) {}

    void Put(uint32_t block, uint32_t offset, const std::string& text)
    {
        const size_t at = block * kBlockSize + offset;
        std::memcpy(m_data.data() + at, text.data(), text.size());
    }

    void PutAtGlobal(uint64_t offset, const std::string& text)
    {
        std::memcpy(m_data.data() + offset, text.data(), text.size());
    }

    void Attach(DiskSearcher& s)
    {
        s.SetDiskInfo(kBlocks, [this](uint32_t block) -> std::vector<uint8_t> {
            if (block >= kBlocks)
                return {};
            const size_t at = block * kBlockSize;
            return std::vector<uint8_t>(m_data.begin() + at, m_data.begin() + at + kBlockSize);
        });
    }

private:
    std::vector<uint8_t> m_data;
};

std::vector<uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

} // namespace

TEST(DiskSearcherTest, ParseHexReadsBytePairsAndIgnoresWhitespace)
{
    EXPECT_EQ(DiskSearcher::ParseHex("4E75"), (std::vector<uint8_t>{0x4E, 0x75}));
    EXPECT_EQ(DiskSearcher::ParseHex("4e75"), (std::vector<uint8_t>{0x4E, 0x75}));
    EXPECT_EQ(DiskSearcher::ParseHex("4e 75"), (std::vector<uint8_t>{0x4E, 0x75}));
    EXPECT_EQ(DiskSearcher::ParseHex("4e\t75"), (std::vector<uint8_t>{0x4E, 0x75}));
    EXPECT_TRUE(DiskSearcher::ParseHex("").empty());

    // A trailing half-byte is taken as its own byte rather than dropped.
    EXPECT_EQ(DiskSearcher::ParseHex("4E7"), (std::vector<uint8_t>{0x4E, 0x07}));
}

TEST(DiskSearcherTest, ParseHexDoesNotStripPrefixesOrRejectJunk)
{
    // Documenting the current contract: callers must hand it bare hex digits.
    // A "$" or "0x" prefix is consumed as a byte pair, and non-hex digits parse
    // as zero rather than failing, so validation belongs to the caller.
    EXPECT_NE(DiskSearcher::ParseHex("$4E75"), (std::vector<uint8_t>{0x4E, 0x75}));
    EXPECT_NE(DiskSearcher::ParseHex("0x4E75"), (std::vector<uint8_t>{0x4E, 0x75}));
    EXPECT_EQ(DiskSearcher::ParseHex("zz"), (std::vector<uint8_t>{0x00}));
}

TEST(DiskSearcherTest, FindNextLocatesAPatternAndReportsBlockAndOffset)
{
    FakeDisk disk;
    disk.Put(3, 100, "SECRET");
    DiskSearcher s;
    disk.Attach(s);

    uint32_t block = 0;
    size_t offset = 0;
    ASSERT_TRUE(s.FindNext(Bytes("SECRET"), true, false, 0, 0, block, offset));
    EXPECT_EQ(block, 3u);
    EXPECT_EQ(offset, 100u);
}

TEST(DiskSearcherTest, FindNextIsCaseSensitiveOnDemand)
{
    FakeDisk disk;
    disk.Put(1, 10, "Loader");
    DiskSearcher s;
    disk.Attach(s);

    uint32_t block = 0;
    size_t offset = 0;
    EXPECT_FALSE(s.FindNext(Bytes("loader"), true, false, 0, 0, block, offset));
    ASSERT_TRUE(s.FindNext(Bytes("loader"), false, false, 0, 0, block, offset));
    EXPECT_EQ(block, 1u);
    EXPECT_EQ(offset, 10u);
}

TEST(DiskSearcherTest, FindNextMatchesAcrossABlockBoundary)
{
    FakeDisk disk;
    // Straddles the block 2 / block 3 boundary — the case a naive per-block
    // scan silently misses.
    disk.PutAtGlobal(3 * kBlockSize - 3, "BOUNDARY");
    DiskSearcher s;
    disk.Attach(s);

    uint32_t block = 0;
    size_t offset = 0;
    ASSERT_TRUE(s.FindNext(Bytes("BOUNDARY"), true, false, 0, 0, block, offset));
    EXPECT_EQ(block, 2u);
    EXPECT_EQ(offset, kBlockSize - 3);
}

TEST(DiskSearcherTest, FindNextResumesFromAPositionAndWrapsAround)
{
    FakeDisk disk;
    disk.Put(1, 0, "HIT");
    disk.Put(5, 0, "HIT");
    DiskSearcher s;
    disk.Attach(s);

    uint32_t block = 0;
    size_t offset = 0;
    ASSERT_TRUE(s.FindNext(Bytes("HIT"), true, false, 0, 0, block, offset));
    EXPECT_EQ(block, 1u);

    ASSERT_TRUE(s.FindNext(Bytes("HIT"), true, false, 1, 1, block, offset));
    EXPECT_EQ(block, 5u);

    // Past the last hit it wraps back to the first.
    ASSERT_TRUE(s.FindNext(Bytes("HIT"), true, false, 5, 1, block, offset));
    EXPECT_EQ(block, 1u);
}

TEST(DiskSearcherTest, FindNextSearchesBackwards)
{
    FakeDisk disk;
    disk.Put(1, 0, "HIT");
    disk.Put(5, 0, "HIT");
    DiskSearcher s;
    disk.Attach(s);

    uint32_t block = 0;
    size_t offset = 0;
    ASSERT_TRUE(s.FindNext(Bytes("HIT"), true, true, 6, 0, block, offset));
    EXPECT_EQ(block, 5u);
}

TEST(DiskSearcherTest, FindNextReportsFailureForAnAbsentPattern)
{
    FakeDisk disk;
    DiskSearcher s;
    disk.Attach(s);

    uint32_t block = 0;
    size_t offset = 0;
    EXPECT_FALSE(s.FindNext(Bytes("NOWHERE"), true, false, 0, 0, block, offset));
    EXPECT_FALSE(s.FindNext({}, true, false, 0, 0, block, offset));
}

TEST(DiskSearcherTest, FindAllReturnsEveryOccurrence)
{
    FakeDisk disk;
    disk.Put(0, 16, "MARK");
    disk.Put(2, 48, "MARK");
    disk.Put(7, 0, "MARK");
    DiskSearcher s;
    disk.Attach(s);

    auto hits = s.FindAll(Bytes("MARK"), true, DiskSearcher::SearchType::ASCII);
    ASSERT_EQ(hits.size(), 3u);
    EXPECT_EQ(hits[0].block, 0u);
    EXPECT_EQ(hits[0].offset, 16u);
    EXPECT_EQ(hits[1].block, 2u);
    EXPECT_EQ(hits[2].block, 7u);
    for (const auto& h : hits)
        EXPECT_EQ(h.length, 4u);
}

TEST(DiskSearcherTest, FindAllReportsProgress)
{
    FakeDisk disk;
    disk.Put(4, 0, "X");
    DiskSearcher s;
    disk.Attach(s);

    float last = -1.0f;
    bool monotonic = true;
    s.FindAll(Bytes("X"), true, DiskSearcher::SearchType::ASCII, [&](float p) {
        if (p < last)
            monotonic = false;
        last = p;
    });
    EXPECT_TRUE(monotonic);
}

TEST(DiskSearcherTest, WithoutADiskThereAreNoResults)
{
    DiskSearcher s;
    uint32_t block = 0;
    size_t offset = 0;
    EXPECT_FALSE(s.FindNext(Bytes("X"), true, false, 0, 0, block, offset));
    EXPECT_TRUE(s.FindAll(Bytes("X"), true, DiskSearcher::SearchType::ASCII).empty());
}

TEST(DiskSearcherTest, HexPatternsMatchBinaryContent)
{
    FakeDisk disk;
    disk.Put(6, 200, std::string("\x4E\x75", 2)); // RTS
    DiskSearcher s;
    disk.Attach(s);

    auto hits = s.FindAll(DiskSearcher::ParseHex("4E75"), true, DiskSearcher::SearchType::Hex);
    ASSERT_FALSE(hits.empty());
    const auto it = std::find_if(hits.begin(), hits.end(),
                                 [](const auto& h) { return h.block == 6 && h.offset == 200; });
    EXPECT_NE(it, hits.end());
}
