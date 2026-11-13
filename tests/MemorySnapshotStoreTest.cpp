#include <gtest/gtest.h>

#include <numeric>
#include <vector>

#include "../src/core/MemorySnapshotStore.h"

using adm::core::LiveMemoryRegion;
using adm::core::MemoryDiffOptions;
using adm::core::MemoryRange;
using adm::core::MemorySnapshotStore;

namespace {

// Backing store for the fake "emulated RAM" the store captures out of.
struct FakeRam
{
    std::vector<uint8_t> chip = std::vector<uint8_t>(256, 0);
    std::vector<uint8_t> rom = std::vector<uint8_t>(64, 0xFF);

    std::vector<LiveMemoryRegion> Regions()
    {
        return {
            LiveMemoryRegion{0x1000, static_cast<uint32_t>(chip.size()), chip.data(), "chip", true},
            LiveMemoryRegion{0xF80000, static_cast<uint32_t>(rom.size()), rom.data(), "rom", false},
        };
    }
};

// Convenience: capture and fail the test if the store refuses.
void MustCapture(MemorySnapshotStore& store, const std::string& name, FakeRam& ram,
                 const std::vector<MemoryRange>& ranges = {}, uint64_t frame = 0)
{
    std::string err;
    ASSERT_TRUE(store.Capture(name, ranges, ram.Regions(), frame, true, err)) << err;
}

} // namespace

TEST(MemorySnapshotStoreTest, CapturesOnlyWritableRegionsByDefault)
{
    FakeRam ram;
    MemorySnapshotStore store;
    MustCapture(store, "boot", ram);

    auto info = store.Info("boot");
    ASSERT_TRUE(info.has_value());
    // The ROM region is read-only, so a blind capture must leave it out.
    EXPECT_EQ(info->regionCount, 1);
    EXPECT_EQ(info->bytes, ram.chip.size());
    EXPECT_EQ(info->regions.front().addr, 0x1000u);
}

TEST(MemorySnapshotStoreTest, CapturesExplicitRangesClippedToMappedMemory)
{
    FakeRam ram;
    MemorySnapshotStore store;
    // Runs off the end of chip RAM and also asks for read-only ROM.
    MustCapture(store, "vec", ram, {{0x1080, 0x400}, {0xF80000, 8}});

    auto info = store.Info("vec");
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->regions.size(), 2u);
    EXPECT_EQ(info->regions[0].addr, 0x1080u);
    EXPECT_EQ(info->regions[0].len, 0x80u); // clipped to the end of chip RAM
    EXPECT_EQ(info->regions[1].addr, 0xF80000u);
    EXPECT_EQ(info->regions[1].len, 8u); // an explicit range may name read-only memory
}

TEST(MemorySnapshotStoreTest, CaptureRejectsBadNamesDuplicatesAndUnmappedRanges)
{
    FakeRam ram;
    MemorySnapshotStore store;
    std::string err;

    EXPECT_FALSE(store.Capture("has space", {}, ram.Regions(), 0, true, err));
    EXPECT_FALSE(err.empty());

    ASSERT_TRUE(store.Capture("a", {}, ram.Regions(), 0, false, err)) << err;
    EXPECT_FALSE(store.Capture("a", {}, ram.Regions(), 0, false, err));
    EXPECT_NE(err.find("already exists"), std::string::npos);
    EXPECT_TRUE(store.Capture("a", {}, ram.Regions(), 0, true, err)) << err;

    EXPECT_FALSE(store.Capture("nowhere", {{0x900000, 16}}, ram.Regions(), 0, true, err));
    EXPECT_NE(err.find("mapped"), std::string::npos);
}

TEST(MemorySnapshotStoreTest, ListIsNewestFirstAndRemoveWorks)
{
    FakeRam ram;
    MemorySnapshotStore store;
    MustCapture(store, "first", ram);
    MustCapture(store, "second", ram);

    auto list = store.List();
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].name, "second");
    EXPECT_EQ(list[1].name, "first");

    EXPECT_TRUE(store.Remove("first"));
    EXPECT_FALSE(store.Remove("first"));
    EXPECT_EQ(store.Count(), 1u);
}

TEST(MemorySnapshotStoreTest, DiffGroupsChangedBytesIntoRuns)
{
    FakeRam ram;
    MemorySnapshotStore store;
    MustCapture(store, "a", ram);

    ram.chip[0x10] = 0xAA;
    ram.chip[0x11] = 0xBB; // contiguous with the previous byte -> one run of 2
    ram.chip[0x40] = 0x01; // isolated -> one run of 1
    MustCapture(store, "b", ram);

    auto res = store.Diff("a", "b", {});
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(res.runs.size(), 2u);
    EXPECT_EQ(res.totalRuns, 2u);
    EXPECT_EQ(res.bytesMatching, 3u);

    EXPECT_EQ(res.runs[0].addr, 0x1010u);
    EXPECT_EQ(res.runs[0].len, 2u);
    EXPECT_EQ(res.runs[0].before, (std::vector<uint8_t>{0x00, 0x00}));
    EXPECT_EQ(res.runs[0].after, (std::vector<uint8_t>{0xAA, 0xBB}));

    EXPECT_EQ(res.runs[1].addr, 0x1040u);
    EXPECT_EQ(res.runs[1].len, 1u);
}

TEST(MemorySnapshotStoreTest, DiffHonoursMinAndMaxRunLength)
{
    FakeRam ram;
    MemorySnapshotStore store;
    MustCapture(store, "a", ram);

    ram.chip[0x10] = 1;               // run of 1
    for (int i = 0x20; i < 0x24; ++i) // run of 4
        ram.chip[static_cast<size_t>(i)] = 1;
    MustCapture(store, "b", ram);

    MemoryDiffOptions minOnly;
    minOnly.minRun = 2;
    auto res = store.Diff("a", "b", minOnly);
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(res.runs.size(), 1u);
    EXPECT_EQ(res.runs[0].len, 4u);

    MemoryDiffOptions maxOnly;
    maxOnly.maxRun = 1;
    res = store.Diff("a", "b", maxOnly);
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(res.runs.size(), 1u);
    EXPECT_EQ(res.runs[0].addr, 0x1010u);
}

TEST(MemorySnapshotStoreTest, DiffExcludesRequestedRangesAndSplitsRunsAtThem)
{
    FakeRam ram;
    MemorySnapshotStore store;
    MustCapture(store, "a", ram);
    for (int i = 0x10; i < 0x14; ++i)
        ram.chip[static_cast<size_t>(i)] = 1;
    MustCapture(store, "b", ram);

    MemoryDiffOptions opts;
    opts.exclude = {{0x1012, 1}}; // punch a hole in the middle of the run
    auto res = store.Diff("a", "b", opts);
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(res.runs.size(), 2u);
    EXPECT_EQ(res.runs[0].addr, 0x1010u);
    EXPECT_EQ(res.runs[0].len, 2u);
    EXPECT_EQ(res.runs[1].addr, 0x1013u);
    EXPECT_EQ(res.runs[1].len, 1u);
}

TEST(MemorySnapshotStoreTest, DiffStableWithIsolatesBytesThatSettleAfterB)
{
    FakeRam ram;
    MemorySnapshotStore store;
    MustCapture(store, "a", ram);

    ram.chip[0x10] = 0x11; // changes A->B and then stays put
    ram.chip[0x20] = 0x22; // changes A->B but keeps moving afterwards (noise)
    MustCapture(store, "b", ram);

    ram.chip[0x20] = 0x33;
    MustCapture(store, "c", ram);

    MemoryDiffOptions opts;
    opts.stableWith = "c";
    auto res = store.Diff("a", "b", opts);
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(res.runs.size(), 1u);
    EXPECT_EQ(res.runs[0].addr, 0x1010u);

    // Without the third snapshot both bytes look equally interesting.
    EXPECT_EQ(store.Diff("a", "b", {}).runs.size(), 2u);
}

TEST(MemorySnapshotStoreTest, DiffSameModeReportsUnchangedSpans)
{
    FakeRam ram;
    MemorySnapshotStore store;
    // Capture a small window so "same" produces a checkable number of runs.
    MustCapture(store, "a", ram, {{0x1000, 4}});
    ram.chip[1] = 0xFF;
    MustCapture(store, "b", ram, {{0x1000, 4}});

    MemoryDiffOptions opts;
    opts.wantSame = true;
    auto res = store.Diff("a", "b", opts);
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(res.runs.size(), 2u);
    EXPECT_EQ(res.runs[0].addr, 0x1000u);
    EXPECT_EQ(res.runs[0].len, 1u);
    EXPECT_EQ(res.runs[1].addr, 0x1002u);
    EXPECT_EQ(res.runs[1].len, 2u);
}

TEST(MemorySnapshotStoreTest, DiffLimitTruncatesButTotalsCoverEverything)
{
    FakeRam ram;
    MemorySnapshotStore store;
    MustCapture(store, "a", ram);
    for (int i = 0; i < 10; ++i)
        ram.chip[static_cast<size_t>(i) * 4] = 1; // ten isolated runs
    MustCapture(store, "b", ram);

    MemoryDiffOptions opts;
    opts.limit = 3;
    auto res = store.Diff("a", "b", opts);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ(res.runs.size(), 3u);
    EXPECT_TRUE(res.truncated);
    EXPECT_EQ(res.totalRuns, 10u);
    EXPECT_EQ(res.bytesMatching, 10u);
}

TEST(MemorySnapshotStoreTest, DiffRefusesWhenTheLayoutChanged)
{
    FakeRam ram;
    MemorySnapshotStore store;
    MustCapture(store, "a", ram, {{0x1000, 16}});
    MustCapture(store, "b", ram, {{0x1000, 32}});

    auto res = store.Diff("a", "b", {});
    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.error.find("layout"), std::string::npos);
}

TEST(MemorySnapshotStoreTest, DiffReportsMissingSnapshotsByName)
{
    FakeRam ram;
    MemorySnapshotStore store;
    MustCapture(store, "a", ram);

    EXPECT_NE(store.Diff("a", "nope", {}).error.find("nope"), std::string::npos);
    EXPECT_NE(store.Diff("nope", "a", {}).error.find("nope"), std::string::npos);

    MemoryDiffOptions opts;
    opts.stableWith = "ghost";
    EXPECT_NE(store.Diff("a", "a", opts).error.find("ghost"), std::string::npos);
}

TEST(MemorySnapshotStoreTest, DiffCanOmitTheByteBodies)
{
    FakeRam ram;
    MemorySnapshotStore store;
    MustCapture(store, "a", ram);
    ram.chip[0] = 1;
    MustCapture(store, "b", ram);

    MemoryDiffOptions opts;
    opts.includeBytes = false;
    auto res = store.Diff("a", "b", opts);
    ASSERT_TRUE(res.ok) << res.error;
    ASSERT_EQ(res.runs.size(), 1u);
    EXPECT_TRUE(res.runs[0].before.empty());
    EXPECT_TRUE(res.runs[0].after.empty());
    EXPECT_EQ(res.runs[0].len, 1u);
}
