#include <gtest/gtest.h>

#include <filesystem>

#include "core/DebugSymbols.h"
#include "core/FunctionProfiler.h"
#include "core/SegListResolver.h"

using adm::core::AmigaSegment;
using adm::core::DebugSymbols;
using adm::core::FunctionProfiler;
using adm::core::InstructionStats;

namespace {

std::string FindFixture()
{
    const char* candidates[] = {
        "tests/images/a.elf",
        "../tests/images/a.elf",
        "../../tests/images/a.elf",
    };
    for (const char* c : candidates) {
        if (std::filesystem::exists(c)) {
            return c;
        }
    }
    return {};
}

// Known link-time addresses from `readelf -sW a.elf`.
constexpr uint32_t kSystemDisableCache = 0x2498; // size 84
constexpr uint32_t kGetCacheStatus = 0x2a30;     // size 6

// A synthetic runtime load base for the code hunk. Nothing special about the
// value; the profiler just subtracts it to recover link-time addresses.
constexpr uint32_t kBase = 0x21000;

InstructionStats MakePc(uint32_t pc, uint32_t totalCycles, uint32_t count)
{
    InstructionStats s;
    s.pc = pc;
    s.totalCycles = totalCycles;
    s.count = count;
    return s;
}

} // namespace

TEST(FunctionProfilerTest, AggregatesCyclesPerFunction)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugSymbols syms;
    ASSERT_TRUE(syms.LoadFromElf(path));

    std::vector<AmigaSegment> segs = {{kBase, 0x10000}}; // code hunk covering both funcs

    std::vector<InstructionStats> perPc = {
        // Two PCs inside system_disable_cache -> should combine.
        MakePc(kBase + kSystemDisableCache, 100, 10),
        MakePc(kBase + kSystemDisableCache + 40, 50, 5),
        // One PC inside get_cache_status.
        MakePc(kBase + kGetCacheStatus, 30, 3),
        // A PC outside the code segment (e.g. Kickstart ROM) -> unresolved.
        MakePc(0xF80100, 20, 2),
    };

    FunctionProfiler prof;
    prof.Update(perPc, segs, syms);

    ASSERT_EQ(prof.GetSorted().size(), 2u);
    // Hottest first.
    EXPECT_EQ(prof.GetSorted()[0].name, "system_disable_cache");
    EXPECT_EQ(prof.GetSorted()[0].totalCycles, 150u);
    EXPECT_EQ(prof.GetSorted()[0].count, 15u);
    EXPECT_EQ(prof.GetSorted()[0].address, kSystemDisableCache);

    EXPECT_EQ(prof.GetSorted()[1].name, "get_cache_status");
    EXPECT_EQ(prof.GetSorted()[1].totalCycles, 30u);

    EXPECT_EQ(prof.GetResolvedCycles(), 180u);
    EXPECT_EQ(prof.GetUnresolvedCycles(), 20u);
    EXPECT_EQ(prof.GetTotalCycles(), 200u);

    // pctFrame is relative to the whole frame (resolved + unresolved).
    EXPECT_FLOAT_EQ(prof.GetSorted()[0].pctFrame, 150.0f / 200.0f);
}

TEST(FunctionProfilerTest, NoSegmentsMeansAllUnresolved)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugSymbols syms;
    ASSERT_TRUE(syms.LoadFromElf(path));

    std::vector<InstructionStats> perPc = {
        MakePc(kBase + kSystemDisableCache, 100, 10),
    };
    FunctionProfiler prof;
    prof.Update(perPc, {}, syms); // empty segment list

    EXPECT_TRUE(prof.GetSorted().empty());
    EXPECT_EQ(prof.GetResolvedCycles(), 0u);
    EXPECT_EQ(prof.GetUnresolvedCycles(), 100u);
}

TEST(FunctionProfilerTest, UnloadedSymbolsMeansAllUnresolved)
{
    DebugSymbols syms; // never loaded
    std::vector<AmigaSegment> segs = {{kBase, 0x10000}};
    std::vector<InstructionStats> perPc = {
        MakePc(kBase + kSystemDisableCache, 100, 10),
    };
    FunctionProfiler prof;
    prof.Update(perPc, segs, syms);

    EXPECT_TRUE(prof.GetSorted().empty());
    EXPECT_EQ(prof.GetUnresolvedCycles(), 100u);
}

TEST(FunctionProfilerTest, PcInsideSegmentButPastCodeIsUnresolved)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugSymbols syms;
    ASSERT_TRUE(syms.LoadFromElf(path));

    std::vector<AmigaSegment> segs = {{kBase, 0x10000}};
    // .text is ~0x4832 bytes, so link address 0x8000 is past every function but
    // still inside the (deliberately oversized) segment: exercises the
    // "in segment, resolves to no function" branch rather than "outside segment".
    std::vector<InstructionStats> perPc = {MakePc(kBase + 0x8000, 40, 4)};
    FunctionProfiler prof;
    prof.Update(perPc, segs, syms);

    EXPECT_TRUE(prof.GetSorted().empty());
    EXPECT_EQ(prof.GetUnresolvedCycles(), 40u);
}

TEST(FunctionProfilerTest, ClearResets)
{
    FunctionProfiler prof;
    DebugSymbols syms;
    std::vector<AmigaSegment> segs = {{kBase, 0x10000}};
    prof.Update({MakePc(kBase, 10, 1)}, segs, syms);
    uint32_t ver = prof.GetFrameVersion();
    prof.Clear();
    EXPECT_TRUE(prof.GetSorted().empty());
    EXPECT_EQ(prof.GetResolvedCycles(), 0u);
    EXPECT_EQ(prof.GetUnresolvedCycles(), 0u);
    EXPECT_GT(prof.GetFrameVersion(), ver);
}
