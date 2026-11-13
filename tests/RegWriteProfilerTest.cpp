#include <gtest/gtest.h>

#include <vector>

#include "../src/core/Constants.h"
#include "../src/core/RegWriteProfiler.h"

using adm::AmigaEmulator;
using adm::core::RegWriteProfiler;

namespace {

constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;

// Register offsets used by the fixtures.
constexpr uint16_t kCOLOR00 = 0x180;
constexpr uint16_t kCOLOR01 = 0x182;
constexpr uint16_t kBPLCON0 = 0x100;

AmigaEmulator::RegWriteEvent MakeWrite(uint16_t vpos, uint16_t hpos, uint16_t reg, uint16_t value,
                                       bool fromCopper = true, uint32_t pc = 0x20000)
{
    AmigaEmulator::RegWriteEvent ev{};
    ev.vpos = vpos;
    ev.hpos = hpos;
    ev.reg = reg;
    ev.value = value;
    ev.pc = pc;
    ev.fromCopper = fromCopper;
    ev.absolute_cycle = vpos * kCCKPerLine + hpos;
    return ev;
}

std::vector<adm::core::RegWriteRecord> Enrich(const std::vector<AmigaEmulator::RegWriteEvent>& evs)
{
    RegWriteProfiler p;
    p.UpdateFromEvents(evs.data(), static_cast<int>(evs.size()));
    return p.GetCurrentFrame();
}

} // namespace

// The L-Ref formula is the join key every other debugger view uses; if this drifts, clicking a
// row syncs the rest of the app to the wrong point in the frame.
TEST(RegWriteProfilerTest, LRefIsVposTimesCCKPerLinePlusHpos)
{
    const auto recs = Enrich({MakeWrite(0, 0, kCOLOR00, 0x000), MakeWrite(100, 57, kCOLOR00, 0x111),
                              MakeWrite(312, 227, kCOLOR00, 0x222)});

    ASSERT_EQ(recs.size(), 3u);
    EXPECT_EQ(recs[0].lRef, 0u);
    EXPECT_EQ(recs[1].lRef, 100u * kCCKPerLine + 57u);
    EXPECT_EQ(recs[2].lRef, 312u * kCCKPerLine + 227u);
}

TEST(RegWriteProfilerTest, FirstWriteToARegisterIsFlaggedAndCountsAsChanged)
{
    const auto recs = Enrich({MakeWrite(10, 0, kCOLOR00, 0x0F0)});

    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(recs[0].firstWrite);
    // Nothing to compare against yet, so prevValue mirrors the written value...
    EXPECT_EQ(recs[0].prevValue, 0x0F0);
    // ...but the write is still reported as changed, so a "changed only" filter never hides
    // the write that established a register's value for the frame.
    EXPECT_TRUE(recs[0].changed);
}

TEST(RegWriteProfilerTest, RepeatedIdenticalWriteIsNotChanged)
{
    const auto recs = Enrich({MakeWrite(10, 0, kCOLOR00, 0x0F0), MakeWrite(11, 0, kCOLOR00, 0x0F0),
                              MakeWrite(12, 0, kCOLOR00, 0x123)});

    ASSERT_EQ(recs.size(), 3u);
    EXPECT_TRUE(recs[0].firstWrite);

    EXPECT_FALSE(recs[1].firstWrite);
    EXPECT_FALSE(recs[1].changed);
    EXPECT_EQ(recs[1].prevValue, 0x0F0);

    EXPECT_FALSE(recs[2].firstWrite);
    EXPECT_TRUE(recs[2].changed);
    EXPECT_EQ(recs[2].prevValue, 0x0F0);
}

// The running value is per register, not per row — interleaving a second register must not
// make the next write to the first one look like a change.
TEST(RegWriteProfilerTest, ChangeTrackingIsPerRegister)
{
    const auto recs = Enrich({MakeWrite(10, 0, kCOLOR00, 0xAAA), MakeWrite(10, 4, kCOLOR01, 0xBBB),
                              MakeWrite(10, 8, kCOLOR00, 0xAAA)});

    ASSERT_EQ(recs.size(), 3u);
    EXPECT_TRUE(recs[0].firstWrite);
    EXPECT_TRUE(recs[1].firstWrite);
    EXPECT_FALSE(recs[2].firstWrite);
    EXPECT_FALSE(recs[2].changed) << "COLOR00 was re-written with the value it already held";
    EXPECT_EQ(recs[2].prevValue, 0xAAA);
}

TEST(RegWriteProfilerTest, StatsSplitSourcesAndCountDistinctRegisters)
{
    RegWriteProfiler p;
    const std::vector<AmigaEmulator::RegWriteEvent> evs = {
        MakeWrite(10, 0, kCOLOR00, 0x111, /*fromCopper=*/true),
        MakeWrite(10, 4, kCOLOR00, 0x111, /*fromCopper=*/true), // unchanged
        MakeWrite(11, 0, kCOLOR01, 0x222, /*fromCopper=*/false),
        MakeWrite(12, 0, kBPLCON0, 0x3200, /*fromCopper=*/false),
    };
    p.UpdateFromEvents(evs.data(), static_cast<int>(evs.size()));

    const auto stats = p.ComputeStats();
    EXPECT_EQ(stats.totalWrites, 4);
    EXPECT_EQ(stats.copperWrites, 2);
    EXPECT_EQ(stats.cpuWrites, 2);
    EXPECT_EQ(stats.changedWrites, 3); // the repeated COLOR00 write is excluded
    EXPECT_EQ(stats.distinctRegisters, 3);
}

TEST(RegWriteProfilerTest, SourceAndPcSurviveEnrichment)
{
    const auto recs = Enrich({MakeWrite(10, 0, kCOLOR00, 0x111, /*fromCopper=*/true, 0x00C0DE),
                              MakeWrite(11, 0, kBPLCON0, 0x3200, /*fromCopper=*/false, 0x0F0000)});

    ASSERT_EQ(recs.size(), 2u);
    EXPECT_TRUE(recs[0].raw.fromCopper);
    EXPECT_EQ(recs[0].raw.pc, 0x00C0DEu);
    EXPECT_FALSE(recs[1].raw.fromCopper);
    EXPECT_EQ(recs[1].raw.pc, 0x0F0000u);
}

TEST(RegWriteProfilerTest, EmptyFrameClearsPreviousResults)
{
    RegWriteProfiler p;
    const auto evs = std::vector<AmigaEmulator::RegWriteEvent>{MakeWrite(10, 0, kCOLOR00, 0x111)};
    p.UpdateFromEvents(evs.data(), 1);
    ASSERT_EQ(p.GetCurrentFrame().size(), 1u);

    p.UpdateFromEvents(nullptr, 0);
    EXPECT_TRUE(p.GetCurrentFrame().empty());
}

// The host reads this buffer straight out of the DLL, so a core built at a different ABI must
// not be able to index the per-register tracking arrays out of range.
TEST(RegWriteProfilerTest, OutOfRangeRegisterOffsetIsClamped)
{
    const auto recs = Enrich({MakeWrite(10, 0, 0xFFFF, 0x1234)});

    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(recs[0].firstWrite);
    EXPECT_EQ(recs[0].raw.reg, 0xFFFF) << "the raw event is reported verbatim";
}

// Guards the wire format against silent drift from the PuAE core's adm_reg_write_event_t.
TEST(RegWriteProfilerTest, RegWriteEventMatchesCoreWireLayout)
{
    static_assert(sizeof(AmigaEmulator::RegWriteEvent) == 20);
    EXPECT_EQ(offsetof(AmigaEmulator::RegWriteEvent, vpos), 0u);
    EXPECT_EQ(offsetof(AmigaEmulator::RegWriteEvent, hpos), 2u);
    EXPECT_EQ(offsetof(AmigaEmulator::RegWriteEvent, reg), 4u);
    EXPECT_EQ(offsetof(AmigaEmulator::RegWriteEvent, value), 6u);
    EXPECT_EQ(offsetof(AmigaEmulator::RegWriteEvent, pc), 8u);
    EXPECT_EQ(offsetof(AmigaEmulator::RegWriteEvent, fromCopper), 12u);
    EXPECT_EQ(offsetof(AmigaEmulator::RegWriteEvent, absolute_cycle), 16u);
}
