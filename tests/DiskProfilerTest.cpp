#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <numeric>
#include <vector>

#include "../src/core/AmigaMfmDecoder.h"
#include "../src/core/DiskProfiler.h"

using adm::AmigaEmulator;
using adm::core::AmigaMfmDecoder;
using adm::core::DiskProfiler;
using adm::core::DiskRecord;

namespace {

constexpr int kSectorSize = AmigaMfmDecoder::kSectorSize;
constexpr int kSectorsPerTrack = AmigaMfmDecoder::kSectorsPerTrack;

// A standard 11-sector AmigaDOS track whose sectors are filled with a recognisable,
// per-sector byte pattern so hashes differ between sectors.
std::vector<uint8_t> MakeStandardTrackMfm(uint8_t trackNumber, uint8_t seed = 0)
{
    std::vector<std::array<uint8_t, kSectorSize>> sectors(kSectorsPerTrack);
    for (int s = 0; s < kSectorsPerTrack; ++s)
        sectors[s].fill(static_cast<uint8_t>(seed + s * 7 + 1));
    return AmigaMfmDecoder::EncodeStandardTrack(trackNumber, sectors);
}

AmigaEmulator::DiskDmaEvent MakeEvent(uint32_t seq, uint8_t direction, uint32_t payloadLen)
{
    AmigaEmulator::DiskDmaEvent ev{};
    ev.seq = seq;
    ev.direction = direction; // 2 = read, 3 = write
    ev.drive = 0;
    ev.cyl = 40;
    ev.side = 1;
    ev.pc = 0x012345;
    ev.startCycle = 1000 + seq;
    ev.dskptStart = 0x020000;
    ev.dskptEnd = 0x020000 + payloadLen;
    ev.words = static_cast<uint16_t>(payloadLen / 2);
    ev.payloadOff = 0;
    ev.payloadLen = payloadLen;
    return ev;
}

// Serves one fixed buffer as the payload arena.
DiskProfiler::PayloadReader ReaderFor(const std::vector<uint8_t>& buf)
{
    return [&buf](uint32_t off, uint32_t len, uint8_t* out) -> int {
        if (off >= buf.size())
            return 0;
        const uint32_t n = std::min<uint32_t>(len, static_cast<uint32_t>(buf.size()) - off);
        std::memcpy(out, buf.data() + off, n);
        return static_cast<int>(n);
    };
}

} // namespace

TEST(DiskProfilerTest, HashBytesMatchesKnownSha1)
{
    // SHA-1("abc") — pins the digest formatting (lowercase hex, 40 chars).
    const char* abc = "abc";
    EXPECT_EQ(DiskProfiler::HashBytes(reinterpret_cast<const uint8_t*>(abc), 3),
              "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(DiskProfilerTest, HashBytesIsEmptyForEmptyInput)
{
    EXPECT_TRUE(DiskProfiler::HashBytes(nullptr, 0).empty());
}

TEST(DiskProfilerTest, EnrichDecodesStandardTrackSectors)
{
    const std::vector<uint8_t> mfm = MakeStandardTrackMfm(/*trackNumber=*/81);
    const auto ev = MakeEvent(0, /*read*/ 2, static_cast<uint32_t>(mfm.size()));

    const DiskRecord rec = DiskProfiler::Enrich(ev, mfm.data(), mfm.size(), /*frame=*/5,
                                                /*frameStartCycle=*/1000);

    EXPECT_TRUE(rec.standardTrack);
    EXPECT_EQ(rec.sectors.size(), static_cast<size_t>(kSectorsPerTrack));
    EXPECT_EQ(rec.sectorsChecksumOk, kSectorsPerTrack);
    EXPECT_EQ(rec.sectorHashes.size(), rec.sectors.size());
    EXPECT_EQ(rec.frame, 5u);
    // lRef is derived from startCycle - frameStartCycle, as BlitProfiler does.
    EXPECT_EQ(rec.lRef, ev.startCycle - 1000u);

    // Every sector 0..10 is present, stamped with the track number we encoded.
    std::vector<int> seen;
    for (const auto& s : rec.sectors) {
        EXPECT_TRUE(s.checksumOk);
        EXPECT_EQ(s.trackNumber, 81);
        seen.push_back(s.sectorNumber);
    }
    std::sort(seen.begin(), seen.end());
    std::vector<int> expected(kSectorsPerTrack);
    std::iota(expected.begin(), expected.end(), 0);
    EXPECT_EQ(seen, expected);

    // Distinct sector contents must hash differently.
    EXPECT_NE(rec.sectorHashes[0], rec.sectorHashes[1]);
}

TEST(DiskProfilerTest, EnrichHashesAreStableAndContentSensitive)
{
    const std::vector<uint8_t> a = MakeStandardTrackMfm(81, /*seed=*/0);
    const std::vector<uint8_t> b = MakeStandardTrackMfm(81, /*seed=*/0);
    const std::vector<uint8_t> c = MakeStandardTrackMfm(81, /*seed=*/9);
    const auto ev = MakeEvent(0, 2, static_cast<uint32_t>(a.size()));

    const auto ra = DiskProfiler::Enrich(ev, a.data(), a.size(), 0, 0);
    const auto rb = DiskProfiler::Enrich(ev, b.data(), b.size(), 0, 0);
    const auto rc = DiskProfiler::Enrich(ev, c.data(), c.size(), 0, 0);

    EXPECT_FALSE(ra.sha1.empty());
    EXPECT_EQ(ra.sha1, rb.sha1); // same bytes -> same hash
    EXPECT_NE(ra.sha1, rc.sha1); // different sector data -> different hash
}

TEST(DiskProfilerTest, EnrichHandlesNonStandardPayload)
{
    // A short buffer of noise: no sync marks, so nothing decodes, but the transfer is
    // still recorded and hashed. This is the copy-protected / custom-trackloader case.
    std::vector<uint8_t> noise(64);
    for (size_t i = 0; i < noise.size(); ++i)
        noise[i] = static_cast<uint8_t>(i * 3);
    const auto ev = MakeEvent(0, 2, static_cast<uint32_t>(noise.size()));

    const auto rec = DiskProfiler::Enrich(ev, noise.data(), noise.size(), 0, 0);
    EXPECT_FALSE(rec.standardTrack);
    EXPECT_TRUE(rec.sectors.empty());
    EXPECT_EQ(rec.sectorsChecksumOk, 0);
    EXPECT_FALSE(rec.sha1.empty()); // still hashed
    EXPECT_EQ(rec.payload.size(), noise.size());
}

TEST(DiskProfilerTest, EnrichWithoutPayloadKeepsMetadataOnly)
{
    const auto ev = MakeEvent(0, 3, 0);
    const auto rec = DiskProfiler::Enrich(ev, nullptr, 0, 2, 0);
    EXPECT_TRUE(rec.payload.empty());
    EXPECT_TRUE(rec.sha1.empty());
    EXPECT_EQ(rec.raw.direction, 3);
    EXPECT_EQ(rec.frame, 2u);
}

TEST(DiskProfilerTest, UpdateDedupsAlreadySeenSequences)
{
    const std::vector<uint8_t> mfm = MakeStandardTrackMfm(81);
    DiskProfiler prof;

    // The core ring is not drained per frame, so the same events are re-presented.
    std::vector<AmigaEmulator::DiskDmaEvent> batch = {
        MakeEvent(0, 2, static_cast<uint32_t>(mfm.size())),
        MakeEvent(1, 2, static_cast<uint32_t>(mfm.size())),
    };
    prof.Update(batch.data(), 2, ReaderFor(mfm), 0, 0);
    EXPECT_EQ(prof.GetRecords().size(), 2u);

    // Second poll re-presents seq 0 and 1 plus a new seq 2: only seq 2 is ingested.
    batch.push_back(MakeEvent(2, 3, static_cast<uint32_t>(mfm.size())));
    prof.Update(batch.data(), 3, ReaderFor(mfm), 1, 0);
    EXPECT_EQ(prof.GetRecords().size(), 3u);
    EXPECT_FALSE(prof.DroppedEvents());

    const auto stats = prof.ComputeStats();
    EXPECT_EQ(stats.totalTransfers, 3);
    EXPECT_EQ(stats.reads, 2);
    EXPECT_EQ(stats.writes, 1);
    EXPECT_EQ(stats.bytesWritten, mfm.size());
    EXPECT_EQ(stats.bytesRead, mfm.size() * 2);
}

TEST(DiskProfilerTest, UpdateFlagsSeqGapAsDroppedEvents)
{
    const std::vector<uint8_t> mfm = MakeStandardTrackMfm(81);
    DiskProfiler prof;

    auto e0 = MakeEvent(0, 2, static_cast<uint32_t>(mfm.size()));
    prof.Update(&e0, 1, ReaderFor(mfm), 0, 0);
    EXPECT_FALSE(prof.DroppedEvents());

    // seq jumps 0 -> 5: the core ring overflowed between polls.
    auto e5 = MakeEvent(5, 2, static_cast<uint32_t>(mfm.size()));
    prof.Update(&e5, 1, ReaderFor(mfm), 1, 0);
    EXPECT_TRUE(prof.DroppedEvents());
    EXPECT_EQ(prof.GetRecords().size(), 2u);
}

TEST(DiskProfilerTest, SeqRewindToZeroStartsANewSessionRatherThanAGap)
{
    const std::vector<uint8_t> mfm = MakeStandardTrackMfm(81);
    DiskProfiler prof;

    auto e0 = MakeEvent(0, 2, static_cast<uint32_t>(mfm.size()));
    auto e1 = MakeEvent(1, 2, static_cast<uint32_t>(mfm.size()));
    prof.Update(&e0, 1, ReaderFor(mfm), 0, 0);
    prof.Update(&e1, 1, ReaderFor(mfm), 0, 0);
    ASSERT_EQ(prof.GetRecords().size(), 2u);

    // A ring clear (disk change / reset) rewinds seq to 0; that must not read as a gap,
    // and the transfer must still be ingested rather than skipped as "already seen".
    auto again = MakeEvent(0, 3, static_cast<uint32_t>(mfm.size()));
    prof.Update(&again, 1, ReaderFor(mfm), 1, 0);
    EXPECT_FALSE(prof.DroppedEvents());
    EXPECT_EQ(prof.GetRecords().size(), 3u);
}

TEST(DiskProfilerTest, MarkPersistedFlagsMostRecentMatchingWrite)
{
    const std::vector<uint8_t> mfm = MakeStandardTrackMfm(81);
    DiskProfiler prof;

    auto read = MakeEvent(0, 2, static_cast<uint32_t>(mfm.size()));
    auto write = MakeEvent(1, 3, static_cast<uint32_t>(mfm.size()));
    prof.Update(&read, 1, ReaderFor(mfm), 0, 0);
    prof.Update(&write, 1, ReaderFor(mfm), 0, 0);

    // Track is cyl*2 + side = 40*2 + 1 = 81.
    prof.MarkPersisted(/*drive=*/0, /*track=*/81);
    EXPECT_FALSE(prof.GetRecords()[0].persistedToFile); // the read is untouched
    EXPECT_TRUE(prof.GetRecords()[1].persistedToFile);
}

TEST(DiskProfilerTest, MarkPersistedIgnoresNonMatchingTrack)
{
    const std::vector<uint8_t> mfm = MakeStandardTrackMfm(81);
    DiskProfiler prof;
    auto write = MakeEvent(0, 3, static_cast<uint32_t>(mfm.size()));
    prof.Update(&write, 1, ReaderFor(mfm), 0, 0);

    prof.MarkPersisted(0, /*track=*/12);
    EXPECT_FALSE(prof.GetRecords()[0].persistedToFile);
}

TEST(DiskProfilerTest, PayloadsAreEvictedBeyondTheRetainWindow)
{
    const std::vector<uint8_t> mfm = MakeStandardTrackMfm(81);
    DiskProfiler prof;

    const size_t total = DiskProfiler::kPayloadRetainCount + 5;
    for (uint32_t i = 0; i < total; ++i) {
        auto ev = MakeEvent(i, 2, static_cast<uint32_t>(mfm.size()));
        prof.Update(&ev, 1, ReaderFor(mfm), 0, 0);
    }

    const auto& recs = prof.GetRecords();
    ASSERT_EQ(recs.size(), total);
    // Oldest records keep their hash and sector summary but drop the raw bytes.
    EXPECT_TRUE(recs.front().payload.empty());
    EXPECT_TRUE(recs.front().payloadDropped);
    EXPECT_FALSE(recs.front().sha1.empty());
    EXPECT_EQ(recs.front().sectors.size(), static_cast<size_t>(kSectorsPerTrack));
    // The newest still has its payload.
    EXPECT_FALSE(recs.back().payload.empty());
    EXPECT_FALSE(recs.back().payloadDropped);
}

TEST(DiskProfilerTest, ClearResetsRecordsAndSequenceTracking)
{
    const std::vector<uint8_t> mfm = MakeStandardTrackMfm(81);
    DiskProfiler prof;
    auto e0 = MakeEvent(0, 2, static_cast<uint32_t>(mfm.size()));
    prof.Update(&e0, 1, ReaderFor(mfm), 0, 0);
    ASSERT_EQ(prof.GetRecords().size(), 1u);

    prof.Clear();
    EXPECT_TRUE(prof.GetRecords().empty());
    EXPECT_FALSE(prof.DroppedEvents());

    // After a clear the same seq is accepted again (it is a fresh session).
    prof.Update(&e0, 1, ReaderFor(mfm), 0, 0);
    EXPECT_EQ(prof.GetRecords().size(), 1u);
}

// The core rewinds seq to 0 whenever the machine restarts, and `frame` restarts too,
// so a log spanning two runs used to contain duplicate seq values with unrelated frame
// numbers and no way to tell one run from the other.
TEST(DiskProfilerTest, StampsSessionAndOrdinalAcrossARestart)
{
    const std::vector<uint8_t> mfm = MakeStandardTrackMfm(81);
    DiskProfiler prof;
    const auto len = static_cast<uint32_t>(mfm.size());

    auto e0 = MakeEvent(0, 2, len);
    auto e1 = MakeEvent(1, 2, len);
    prof.Update(&e0, 1, ReaderFor(mfm), 6719, 0);
    prof.Update(&e1, 1, ReaderFor(mfm), 6730, 0);
    EXPECT_EQ(prof.CurrentSession(), 0u);
    EXPECT_EQ(prof.SessionCount(), 1);

    // Restart: seq rewinds, and the host frame counter is rebased too.
    auto again = MakeEvent(0, 2, len);
    prof.Update(&again, 1, ReaderFor(mfm), 12, 0);

    const auto& recs = prof.GetRecords();
    ASSERT_EQ(recs.size(), 3u);
    EXPECT_EQ(prof.CurrentSession(), 1u);
    EXPECT_EQ(prof.SessionCount(), 2);

    // seq is ambiguous across the boundary...
    EXPECT_EQ(recs[0].raw.seq, recs[2].raw.seq);
    // ...but session tells them apart, and ordinal orders the whole log.
    EXPECT_EQ(recs[0].session, 0u);
    EXPECT_EQ(recs[2].session, 1u);
    EXPECT_LT(recs[0].ordinal, recs[1].ordinal);
    EXPECT_LT(recs[1].ordinal, recs[2].ordinal);

    // And `frame` going backwards no longer implies the log is out of order.
    EXPECT_GT(recs[1].frame, recs[2].frame);
}

TEST(DiskProfilerTest, ClearResetsSessionAndOrdinal)
{
    const std::vector<uint8_t> mfm = MakeStandardTrackMfm(81);
    DiskProfiler prof;
    auto e0 = MakeEvent(0, 2, static_cast<uint32_t>(mfm.size()));
    prof.Update(&e0, 1, ReaderFor(mfm), 0, 0);
    ASSERT_EQ(prof.GetRecords().size(), 1u);

    prof.Clear();
    EXPECT_EQ(prof.CurrentSession(), 0u);
    EXPECT_EQ(prof.SessionCount(), 0);

    prof.Update(&e0, 1, ReaderFor(mfm), 0, 0);
    ASSERT_EQ(prof.GetRecords().size(), 1u);
    EXPECT_EQ(prof.GetRecords()[0].ordinal, 0u);
}
