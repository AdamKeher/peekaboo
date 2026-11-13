#include <gtest/gtest.h>

#include <filesystem>

#include "core/CpuTraceHistory.h"
#include "core/DebugLines.h"
#include "core/ElfHunkMap.h"
#include "core/LineCoverage.h"

using adm::core::CpuTraceHistory;
using adm::core::DebugLines;
using adm::core::LineCoverage;
using adm::core::LineLookupStatus;

namespace {

std::string FindFixture()
{
    for (const char* c :
         {"tests/images/a.elf", "../tests/images/a.elf", "../../tests/images/a.elf"})
        if (std::filesystem::exists(c))
            return c;
    return {};
}

// Feed the history synthetic instructions at the given PCs, so coverage can be checked against
// a trace whose contents are known exactly.
void Record(CpuTraceHistory& history, const std::vector<uint32_t>& pcs, uint64_t frame)
{
    std::vector<adm::AmigaEmulator::CpuTraceEntry> entries;
    entries.reserve(pcs.size());
    uint32_t cycle = 1000;
    for (uint32_t pc : pcs) {
        adm::AmigaEmulator::CpuTraceEntry e{};
        e.pc = pc;
        e.raw_len = 2;
        e.absolute_cycle = cycle;
        e.d[0] = 0xD0D0'0000u + static_cast<uint32_t>(pc & 0xFFFF);
        e.a[7] = 0x00C2'4A00u;
        e.sr = 0x2000;
        cycle += 4;
        entries.push_back(e);
    }
    history.AppendFrame(entries.data(), entries.size(), frame, 1000);
}

} // namespace

TEST(LineCoverageTest, ReportsNoCodeWithoutPretendingItNeverRan)
{
    // A line that generated nothing is answerable without any trace at all, and must not be
    // confused with "it ran zero times".
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));

    LineCoverage::Inputs in;
    in.lines = &lines;
    const auto result = LineCoverage::Query(in, "system.c", 1);

    EXPECT_TRUE(result.available);
    EXPECT_EQ(result.status, LineLookupStatus::NoCode);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_EQ(result.totalHits, 0u);
}

TEST(LineCoverageTest, WithoutRelocationItSaysSoRatherThanReportingZeroHits)
{
    // Trace PCs are runtime addresses. Matching them against link-time ranges would find
    // nothing, and "0 hits" would read as "never ran" -- a lie. It must decline instead.
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));
    CpuTraceHistory history;
    history.SetEnabled(true);
    Record(history, {0x2498, 0x249a}, 1);

    LineCoverage::Inputs in;
    in.lines = &lines;
    in.history = &history;
    const auto result = LineCoverage::Query(in, "system.c", 30);

    EXPECT_TRUE(result.available);
    ASSERT_FALSE(result.sites.empty());
    EXPECT_EQ(result.totalHits, 0u);
    EXPECT_NE(result.hint.find("not relocated"), std::string::npos) << result.hint;
}

TEST(LineCoverageTest, TheRangeArithmeticAgreesWithTheTrace)
{
    // Coverage counts a hit by asking the trace for PCs inside [runtimeLo, runtimeHi). This
    // pins that arithmetic down: instructions inside the line's stretch are counted, one just
    // outside it is not. Relocation itself is exercised in ElfHunkMapTest.
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));

    const auto lookup = lines.ResolveLineToAddresses("system.c", 30, /*stmtOnly=*/false);
    ASSERT_EQ(lookup.status, LineLookupStatus::Exact);
    ASSERT_FALSE(lookup.sites.empty());
    const auto& site = lookup.sites.front();

    constexpr uint32_t kBase = 0x00C0CB68; // a plausible .text load address
    CpuTraceHistory history;
    history.SetEnabled(true);
    Record(history, {kBase + site.lo, kBase + site.hi - 1, kBase + site.hi}, 7);

    adm::core::CpuTraceQuery q;
    q.mode = adm::core::TraceQueryMode::Filter;
    q.havePcRange = true;
    q.pcLo = kBase + site.lo;
    q.pcHi = kBase + site.hi; // exclusive
    q.limit = 0;
    const auto snap = history.Query(q);

    ASSERT_TRUE(snap.available);
    EXPECT_EQ(snap.totalMatches, 2u) << "both ends of the half-open range, and nothing past it";
}

TEST(LineCoverageTest, UnexecutedStretchesAreStillReported)
{
    // "This line ran, but only some of its copies did" is the interesting answer on an
    // optimised build, and it disappears if zero-hit stretches are dropped.
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));

    // Find a line the optimiser split into several stretches.
    uint32_t multiLine = 0;
    for (uint32_t line : lines.LinesWithCode("system.c")) {
        if (lines.ResolveLineToAddresses("system.c", line, false, 0).sites.size() > 1) {
            multiLine = line;
            break;
        }
    }
    ASSERT_NE(multiLine, 0u) << "expected at least one multi-stretch line in an -Ofast build";

    LineCoverage::Inputs in;
    in.lines = &lines;
    const auto result = LineCoverage::Query(in, "system.c", multiLine);
    EXPECT_GT(result.sites.size(), 1u);
    for (const auto& site : result.sites)
        EXPECT_LT(site.lo, site.hi);
}

TEST(LineCoverageTest, MissingLineTableIsReportedNotCrashed)
{
    LineCoverage::Inputs in; // everything null
    const auto result = LineCoverage::Query(in, "anything.c", 10);
    EXPECT_FALSE(result.available);
    EXPECT_NE(result.hint.find("no debug line table"), std::string::npos) << result.hint;
}

TEST(LineCoverageTest, AnUnknownFileIsDistinguishedFromALineWithNoCode)
{
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));

    LineCoverage::Inputs in;
    in.lines = &lines;
    const auto result = LineCoverage::Query(in, "not_in_this_build.c", 10);
    EXPECT_TRUE(result.available);
    EXPECT_EQ(result.status, LineLookupStatus::FileUnknown);
}
