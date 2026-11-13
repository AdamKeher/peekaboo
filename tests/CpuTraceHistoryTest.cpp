#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "../src/core/CpuTraceHistory.h"

using adm::AmigaEmulator;
using adm::core::CpuTraceHistory;
using adm::core::CpuTraceQuery;
using adm::core::TraceQueryMode;

namespace {

// One core trace entry. `words` is the instruction, big-endian.
AmigaEmulator::CpuTraceEntry Entry(uint32_t pc, std::initializer_list<uint16_t> words,
                                   uint32_t cycle)
{
    AmigaEmulator::CpuTraceEntry e{};
    e.pc = pc;
    e.absolute_cycle = cycle;
    e.vpos = static_cast<uint16_t>(cycle / 228);
    e.hpos = static_cast<uint16_t>(cycle % 228);
    int i = 0;
    for (uint16_t w : words) {
        e.raw[i++] = static_cast<uint8_t>(w >> 8);
        e.raw[i++] = static_cast<uint8_t>(w);
    }
    e.raw_len = static_cast<uint8_t>(i);
    return e;
}

CpuTraceHistory MakeHistory()
{
    CpuTraceHistory h;
    h.SetEnabled(true);
    return h;
}

} // namespace

TEST(CpuTraceHistoryTest, RecordsNothingWhileDisabled)
{
    CpuTraceHistory h;
    auto e = Entry(0x1000, {0x4E71}, 100);
    h.AppendFrame(&e, 1, 1, 0);
    EXPECT_EQ(h.Size(), 0u);

    auto snap = h.Query({});
    EXPECT_FALSE(snap.available);
    EXPECT_FALSE(snap.enabled);
    EXPECT_NE(snap.hint.find("configure_cpu_trace_history"), std::string::npos);
}

TEST(CpuTraceHistoryTest, AppendsFramesAndAssignsMonotonicSequences)
{
    auto h = MakeHistory();
    std::vector<AmigaEmulator::CpuTraceEntry> f1 = {Entry(0x1000, {0x4E71}, 1000),
                                                    Entry(0x1002, {0x4E71}, 1004)};
    std::vector<AmigaEmulator::CpuTraceEntry> f2 = {Entry(0x2000, {0x4E71}, 5000)};
    h.AppendFrame(f1.data(), 2, 1, 900);
    h.AppendFrame(f2.data(), 1, 2, 4900);

    EXPECT_EQ(h.Size(), 3u);
    EXPECT_EQ(h.FrameCount(), 2);
    EXPECT_EQ(h.OldestSeq(), 1u);
    EXPECT_EQ(h.NewestSeq(), 3u);

    CpuTraceQuery q;
    q.limit = 0;
    auto snap = h.Query(q);
    ASSERT_EQ(snap.matches.size(), 3u);
    EXPECT_EQ(snap.matches[0].record.lRef, 100u); // 1000 - 900
    EXPECT_EQ(snap.matches[2].record.frame, 2u);
    EXPECT_FALSE(snap.dropped);
}

TEST(CpuTraceHistoryTest, IgnoresARepeatedFrame)
{
    auto h = MakeHistory();
    auto e = Entry(0x1000, {0x4E71}, 1000);
    h.AppendFrame(&e, 1, 7, 0);
    h.AppendFrame(&e, 1, 7, 0); // the same harvested frame, polled twice
    EXPECT_EQ(h.Size(), 1u);
    h.AppendFrame(&e, 1, 8, 0);
    EXPECT_EQ(h.Size(), 2u);
}

TEST(CpuTraceHistoryTest, RingDropsOldestAndFlagsIt)
{
    auto h = MakeHistory();
    h.SetCapacity(3);
    for (uint32_t i = 0; i < 5; ++i) {
        auto e = Entry(0x1000 + i * 2, {0x4E71}, 1000 + i);
        h.AppendFrame(&e, 1, i + 1, 0);
    }
    EXPECT_EQ(h.Size(), 3u);
    EXPECT_TRUE(h.Dropped());
    EXPECT_EQ(h.OldestSeq(), 3u);
    EXPECT_EQ(h.NewestSeq(), 5u);
}

TEST(CpuTraceHistoryTest, EnablingAgainStartsAFreshHistoryButDisablingKeepsIt)
{
    auto h = MakeHistory();
    auto e = Entry(0x1000, {0x4E71}, 1000);
    h.AppendFrame(&e, 1, 1, 0);

    h.SetEnabled(false);
    EXPECT_EQ(h.Size(), 1u); // still queryable after you stop recording
    auto snap = h.Query({});
    EXPECT_TRUE(snap.available);
    EXPECT_FALSE(snap.enabled);

    h.SetEnabled(true);
    EXPECT_EQ(h.Size(), 0u);
}

TEST(CpuTraceHistoryTest, FilterMatchesOnPcRangeFrameAndRegister)
{
    auto h = MakeHistory();
    std::vector<AmigaEmulator::CpuTraceEntry> entries = {
        Entry(0x1000, {0x4E71}, 100),
        Entry(0x2000, {0x4E71}, 104),
        Entry(0x3000, {0x4E71}, 108),
    };
    entries[1].d[0] = 0xCAFE;
    h.AppendFrame(entries.data(), 3, 1, 0);

    CpuTraceQuery pcQuery;
    pcQuery.havePcRange = true;
    pcQuery.pcLo = 0x2000;
    pcQuery.pcHi = 0x3000; // exclusive, so 0x3000 is out
    auto snap = h.Query(pcQuery);
    ASSERT_EQ(snap.matches.size(), 1u);
    EXPECT_EQ(snap.matches[0].record.pc, 0x2000u);

    CpuTraceQuery regQuery;
    regQuery.reg = "d0";
    regQuery.regValue = 0xCAFE;
    snap = h.Query(regQuery);
    ASSERT_EQ(snap.matches.size(), 1u);
    EXPECT_EQ(snap.matches[0].record.pc, 0x2000u);

    CpuTraceQuery frameQuery;
    frameQuery.haveFrame = true;
    frameQuery.frame = 99;
    EXPECT_TRUE(h.Query(frameQuery).matches.empty());
}

TEST(CpuTraceHistoryTest, LimitTruncatesButTotalMatchesCountsEverything)
{
    auto h = MakeHistory();
    std::vector<AmigaEmulator::CpuTraceEntry> entries;
    for (uint32_t i = 0; i < 10; ++i)
        entries.push_back(Entry(0x1000 + i * 2, {0x4E71}, 100 + i));
    h.AppendFrame(entries.data(), 10, 1, 0);

    CpuTraceQuery q;
    q.limit = 4;
    auto snap = h.Query(q);
    EXPECT_EQ(snap.matches.size(), 4u);
    EXPECT_TRUE(snap.truncated);
    EXPECT_EQ(snap.totalMatches, 10u);

    q.newestFirst = true;
    snap = h.Query(q);
    ASSERT_EQ(snap.matches.size(), 4u);
    EXPECT_EQ(snap.matches[0].record.pc, 0x1012u);
}

TEST(CpuTraceHistoryTest, StartSeqPagesThroughTheHistory)
{
    auto h = MakeHistory();
    std::vector<AmigaEmulator::CpuTraceEntry> entries;
    for (uint32_t i = 0; i < 6; ++i)
        entries.push_back(Entry(0x1000 + i * 2, {0x4E71}, 100 + i));
    h.AppendFrame(entries.data(), 6, 1, 0);

    CpuTraceQuery q;
    q.startSeq = 4;
    q.limit = 0;
    auto snap = h.Query(q);
    ASSERT_EQ(snap.matches.size(), 3u);
    EXPECT_EQ(snap.matches.front().record.seq, 4u);
}

TEST(CpuTraceHistoryTest, AccessesModeFindsInstructionsTouchingAnAddress)
{
    auto h = MakeHistory();
    std::vector<AmigaEmulator::CpuTraceEntry> entries = {
        Entry(0x1000, {0x33C0, 0x0001, 0x2340}, 100), // MOVE.W D0,$00012340
        Entry(0x1006, {0x3039, 0x0001, 0x2340}, 104), // MOVE.W $00012340,D0
        Entry(0x100C, {0x33C0, 0x0009, 0x9990}, 108), // an unrelated write
    };
    h.AppendFrame(entries.data(), 3, 1, 0);

    CpuTraceQuery q;
    q.mode = TraceQueryMode::Accesses;
    q.addr = 0x00012340;
    q.len = 2;
    auto snap = h.Query(q);
    ASSERT_EQ(snap.matches.size(), 2u);
    EXPECT_TRUE(snap.matches[0].haveAccess);
    EXPECT_TRUE(snap.matches[0].access.write);
    EXPECT_TRUE(snap.matches[1].access.read);

    q.writesOnly = true;
    snap = h.Query(q);
    ASSERT_EQ(snap.matches.size(), 1u);
    EXPECT_EQ(snap.matches[0].record.pc, 0x1000u);

    q.writesOnly = false;
    q.readsOnly = true;
    snap = h.Query(q);
    ASSERT_EQ(snap.matches.size(), 1u);
    EXPECT_EQ(snap.matches[0].record.pc, 0x1006u);
}

TEST(CpuTraceHistoryTest, AccessesModeMatchesAnyByteOfTheRequestedRange)
{
    auto h = MakeHistory();
    // MOVE.L D0,$00012340 covers $12340-$12343.
    auto e = Entry(0x1000, {0x23C0, 0x0001, 0x2340}, 100);
    h.AppendFrame(&e, 1, 1, 0);

    CpuTraceQuery q;
    q.mode = TraceQueryMode::Accesses;
    q.addr = 0x00012342;
    q.len = 1;
    EXPECT_EQ(h.Query(q).matches.size(), 1u);

    q.addr = 0x00012344;
    EXPECT_TRUE(h.Query(q).matches.empty());
}

TEST(CpuTraceHistoryTest, LastWriterWalksProvenanceBackwards)
{
    auto h = MakeHistory();
    std::vector<AmigaEmulator::CpuTraceEntry> entries = {
        Entry(0x1000, {0x33C0, 0x0001, 0x2340}, 100), // first write
        Entry(0x1006, {0x3039, 0x0001, 0x2340}, 104), // a read in between
        Entry(0x100C, {0x33C0, 0x0001, 0x2340}, 108), // last write
    };
    h.AppendFrame(entries.data(), 3, 1, 0);

    CpuTraceQuery q;
    q.mode = TraceQueryMode::LastWriter;
    q.addr = 0x00012340;
    q.len = 2;
    auto snap = h.Query(q);
    ASSERT_EQ(snap.matches.size(), 1u);
    EXPECT_EQ(snap.matches[0].record.pc, 0x100Cu);
    EXPECT_TRUE(snap.matches[0].access.write);

    // beforeSeq steps back to the previous writer.
    q.beforeSeq = snap.matches[0].record.seq;
    snap = h.Query(q);
    ASSERT_EQ(snap.matches.size(), 1u);
    EXPECT_EQ(snap.matches[0].record.pc, 0x1000u);

    // Nothing wrote before that.
    q.beforeSeq = snap.matches[0].record.seq;
    EXPECT_TRUE(h.Query(q).matches.empty());
}

TEST(CpuTraceHistoryTest, LastWriterReportsNothingForAnUntouchedAddress)
{
    auto h = MakeHistory();
    auto e = Entry(0x1000, {0x33C0, 0x0001, 0x2340}, 100);
    h.AppendFrame(&e, 1, 1, 0);

    CpuTraceQuery q;
    q.mode = TraceQueryMode::LastWriter;
    q.addr = 0x00099999;
    auto snap = h.Query(q);
    EXPECT_TRUE(snap.available); // the history exists; nothing matched
    EXPECT_TRUE(snap.matches.empty());
    EXPECT_EQ(snap.totalMatches, 0u);
}

TEST(CpuTraceHistoryTest, ReadRegisterUnderstandsTheUsualNames)
{
    adm::core::CpuTraceRecord rec;
    rec.d[3] = 0x11;
    rec.a[5] = 0x22;
    rec.a[7] = 0x33;
    rec.pc = 0x44;
    rec.sr = 0x55;

    uint32_t v = 0;
    EXPECT_TRUE(CpuTraceHistory::ReadRegister(rec, "d3", v));
    EXPECT_EQ(v, 0x11u);
    EXPECT_TRUE(CpuTraceHistory::ReadRegister(rec, "A5", v));
    EXPECT_EQ(v, 0x22u);
    EXPECT_TRUE(CpuTraceHistory::ReadRegister(rec, "sp", v));
    EXPECT_EQ(v, 0x33u);
    EXPECT_TRUE(CpuTraceHistory::ReadRegister(rec, "pc", v));
    EXPECT_EQ(v, 0x44u);
    EXPECT_TRUE(CpuTraceHistory::ReadRegister(rec, "SR", v));
    EXPECT_EQ(v, 0x55u);
    EXPECT_FALSE(CpuTraceHistory::ReadRegister(rec, "d9", v));
    EXPECT_FALSE(CpuTraceHistory::ReadRegister(rec, "nope", v));
}

TEST(CpuTraceHistoryTest, ClearResetsEverything)
{
    auto h = MakeHistory();
    auto e = Entry(0x1000, {0x4E71}, 100);
    h.AppendFrame(&e, 1, 1, 0);
    h.Clear();
    EXPECT_EQ(h.Size(), 0u);
    EXPECT_FALSE(h.Dropped());
    EXPECT_FALSE(h.Query({}).available);

    // A frame with the same number as before must still be recorded after a clear.
    h.AppendFrame(&e, 1, 1, 0);
    EXPECT_EQ(h.Size(), 1u);
}
