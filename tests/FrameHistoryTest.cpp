#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "../src/core/FrameHistory.h"

using adm::core::DecodeDelta;
using adm::core::EncodeDelta;
using adm::core::FrameHistory;
using adm::core::FrameInput;

namespace {

// A stand-in savestate. Deterministic per seed so a test can rebuild the exact blob it
// expects a frame to reconstruct to.
std::vector<uint8_t> MakeBlob(size_t size, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(size);
    for (auto& b : v)
        b = static_cast<uint8_t>(rng());
    return v;
}

// Mutates `count` scattered bytes, the way one emulated frame touches a savestate:
// a lot of the blob is untouched, the churn is clustered.
void Churn(std::vector<uint8_t>& blob, uint32_t seed, size_t count)
{
    std::mt19937 rng(seed);
    for (size_t i = 0; i < count; ++i) {
        const size_t at = rng() % blob.size();
        const size_t run = 1 + (rng() % 64);
        for (size_t j = at; j < blob.size() && j < at + run; ++j)
            blob[j] = static_cast<uint8_t>(rng());
    }
}

FrameInput MakeInput(uint16_t joy0)
{
    FrameInput in;
    in.joypad[0] = joy0;
    in.keysDown.push_back(static_cast<uint16_t>(joy0 + 1));
    in.keyEvents.push_back({static_cast<uint16_t>(joy0 + 2), true});
    return in;
}

// A history configured the way the recorder uses it, with the budget wide open.
FrameHistory MakeHistory(int keyframeInterval)
{
    FrameHistory h;
    h.SetEnabled(true);
    h.SetKeyframeInterval(keyframeInterval);
    h.SetBudgetBytes(size_t(512) * 1024 * 1024);
    return h;
}

} // namespace

// ---- delta codec ----

TEST(FrameHistoryDelta, RoundTripsScatteredChanges)
{
    const auto base = MakeBlob(64 * 1024, 1);
    auto cur = base;
    Churn(cur, 2, 40);

    std::vector<uint8_t> delta;
    EncodeDelta(base.data(), cur.data(), base.size(), delta);
    EXPECT_LT(delta.size(), base.size()) << "a lightly churned blob should compress";

    auto out = base;
    ASSERT_TRUE(DecodeDelta(delta.data(), delta.size(), out.data(), out.size()));
    EXPECT_EQ(out, cur);
}

TEST(FrameHistoryDelta, IdenticalBlobsEncodeToNothing)
{
    const auto base = MakeBlob(4096, 3);
    std::vector<uint8_t> delta;
    EncodeDelta(base.data(), base.data(), base.size(), delta);
    EXPECT_TRUE(delta.empty());

    auto out = base;
    EXPECT_TRUE(DecodeDelta(delta.data(), delta.size(), out.data(), out.size()));
    EXPECT_EQ(out, base);
}

TEST(FrameHistoryDelta, HandlesChangesAtBothEnds)
{
    const auto base = MakeBlob(1024, 4);
    auto cur = base;
    cur.front() = static_cast<uint8_t>(cur.front() + 1);
    cur.back() = static_cast<uint8_t>(cur.back() + 1);

    std::vector<uint8_t> delta;
    EncodeDelta(base.data(), cur.data(), base.size(), delta);
    auto out = base;
    ASSERT_TRUE(DecodeDelta(delta.data(), delta.size(), out.data(), out.size()));
    EXPECT_EQ(out, cur);
}

TEST(FrameHistoryDelta, RejectsMalformedStream)
{
    auto blob = MakeBlob(64, 5);
    // A run that claims more bytes than the buffer holds must not be applied.
    const uint8_t bad[] = {0x00, 0x7F};
    EXPECT_FALSE(DecodeDelta(bad, sizeof(bad), blob.data(), blob.size()));
    // A truncated varint is equally invalid.
    const uint8_t truncated[] = {0x80};
    EXPECT_FALSE(DecodeDelta(truncated, sizeof(truncated), blob.data(), blob.size()));
}

// ---- record / reconstruct ----

// ReconstructInto answers from a rolling cursor when frames are asked for in timeline order,
// which is the path timeline playback takes. It must agree with Reconstruct() byte for byte
// whatever order the frames are asked for, and after any mutation that moves entries around.
TEST(FrameHistory, ReconstructIntoMatchesReconstructInEveryAccessOrder)
{
    FrameHistory h = MakeHistory(10);

    std::vector<std::vector<uint8_t>> expected;
    auto blob = MakeBlob(32 * 1024, 10);
    for (uint64_t f = 0; f < 100; ++f) {
        Churn(blob, static_cast<uint32_t>(100 + f), 20);
        expected.push_back(blob);
        ASSERT_TRUE(h.Record(f, blob, MakeInput(static_cast<uint16_t>(f))));
    }

    std::vector<uint8_t> out;
    // Forward, which is the cursor's fast path: every frame is the entry after the last.
    for (uint64_t f = 0; f < 100; ++f) {
        SCOPED_TRACE(f);
        ASSERT_TRUE(h.ReconstructInto(f, out));
        EXPECT_EQ(out, expected[f]);
    }
    // The same frame twice running.
    ASSERT_TRUE(h.ReconstructInto(50, out));
    EXPECT_EQ(out, expected[50]);
    ASSERT_TRUE(h.ReconstructInto(50, out));
    EXPECT_EQ(out, expected[50]);
    // Backwards, and jumping about: every one of these takes the rebuild path.
    for (uint64_t f = 99; f-- > 0;) {
        SCOPED_TRACE(f);
        ASSERT_TRUE(h.ReconstructInto(f, out));
        EXPECT_EQ(out, expected[f]);
    }
    for (uint64_t f : {7u, 93u, 60u, 0u, 99u, 59u}) {
        SCOPED_TRACE(f);
        ASSERT_TRUE(h.ReconstructInto(f, out));
        EXPECT_EQ(out, expected[f]);
    }
    // A frame that was never recorded leaves the caller with a clear answer.
    EXPECT_FALSE(h.ReconstructInto(1000, out));
}

// A trimmed timeline re-uses the frame numbers it just dropped. A cursor left holding one of
// them would answer for a recording that no longer exists.
TEST(FrameHistory, ReconstructIntoCursorSurvivesATrimAndRewrite)
{
    FrameHistory h = MakeHistory(10);
    auto blob = MakeBlob(4096, 3);
    std::vector<std::vector<uint8_t>> expected;
    for (uint64_t f = 0; f < 30; ++f) {
        Churn(blob, static_cast<uint32_t>(f + 1), 8);
        expected.push_back(blob);
        ASSERT_TRUE(h.Record(f, blob, MakeInput(static_cast<uint16_t>(f))));
    }

    std::vector<uint8_t> out;
    for (uint64_t f = 0; f < 30; ++f)
        ASSERT_TRUE(h.ReconstructInto(f, out)); // leaves the cursor on frame 29

    h.TrimAfter(24);
    // Play on down a different branch, re-using frames 25-29.
    auto branch = expected[24];
    std::vector<std::vector<uint8_t>> rewritten;
    for (uint64_t f = 25; f < 30; ++f) {
        Churn(branch, static_cast<uint32_t>(9000 + f), 8);
        rewritten.push_back(branch);
        ASSERT_TRUE(h.Record(f, branch, MakeInput(static_cast<uint16_t>(f))));
    }

    for (uint64_t f = 25; f < 30; ++f) {
        SCOPED_TRACE(f);
        ASSERT_TRUE(h.ReconstructInto(f, out));
        EXPECT_EQ(out, rewritten[f - 25]);
        EXPECT_EQ(out, h.Reconstruct(f));
    }
}

// Deserialize can replace the timeline with one that has the same blob size and the same
// frame numbers but different contents.
TEST(FrameHistory, ReconstructIntoCursorIsDroppedByDeserialize)
{
    FrameHistory a = MakeHistory(10);
    FrameHistory b = MakeHistory(10);
    auto blobA = MakeBlob(4096, 1);
    auto blobB = MakeBlob(4096, 200);
    std::vector<std::vector<uint8_t>> expectedB;
    for (uint64_t f = 0; f < 20; ++f) {
        Churn(blobA, static_cast<uint32_t>(f + 1), 8);
        ASSERT_TRUE(a.Record(f, blobA, MakeInput(0)));
        Churn(blobB, static_cast<uint32_t>(f + 500), 8);
        expectedB.push_back(blobB);
        ASSERT_TRUE(b.Record(f, blobB, MakeInput(0)));
    }

    std::vector<uint8_t> out;
    for (uint64_t f = 0; f < 20; ++f)
        ASSERT_TRUE(a.ReconstructInto(f, out)); // cursor sits on a's frame 19

    const auto stream = b.Serialize();
    ASSERT_TRUE(a.Deserialize(stream.data(), stream.size(), 4096));

    for (uint64_t f = 0; f < 20; ++f) {
        SCOPED_TRACE(f);
        ASSERT_TRUE(a.ReconstructInto(f, out));
        EXPECT_EQ(out, expectedB[f]);
    }
}

TEST(FrameHistory, ReconstructsEveryRecordedFrameByteForByte)
{
    FrameHistory h = MakeHistory(10);

    std::vector<std::vector<uint8_t>> expected;
    auto blob = MakeBlob(32 * 1024, 10);
    for (uint64_t f = 0; f < 100; ++f) {
        Churn(blob, static_cast<uint32_t>(100 + f), 20);
        expected.push_back(blob);
        ASSERT_TRUE(h.Record(f, blob, MakeInput(static_cast<uint16_t>(f))));
    }

    for (uint64_t f = 0; f < 100; ++f) {
        SCOPED_TRACE(f);
        ASSERT_TRUE(h.Has(f));
        EXPECT_EQ(h.Reconstruct(f), expected[f]);
    }

    const auto s = h.Stats();
    EXPECT_EQ(s.frameCount, 100u);
    EXPECT_EQ(s.keyframeCount, 10u) << "one keyframe per interval";
    EXPECT_LT(s.bytesUsed, 100u * 32u * 1024u) << "deltas must beat storing every blob";
}

TEST(FrameHistory, RecordsAndReturnsInput)
{
    FrameHistory h = MakeHistory(4);
    const auto blob = MakeBlob(1024, 11);
    ASSERT_TRUE(h.Record(7, blob, MakeInput(0x1234)));

    FrameInput in;
    ASSERT_TRUE(h.GetInput(7, in));
    EXPECT_EQ(in.joypad[0], 0x1234);
    ASSERT_EQ(in.keysDown.size(), 1u);
    EXPECT_EQ(in.keysDown[0], 0x1235);
    ASSERT_EQ(in.keyEvents.size(), 1u);
    EXPECT_EQ(in.keyEvents[0].key, 0x1236);
    EXPECT_TRUE(in.keyEvents[0].down);

    EXPECT_FALSE(h.GetInput(8, in));
}

TEST(FrameHistory, IgnoresFramesThatAreNotNewer)
{
    FrameHistory h = MakeHistory(4);
    const auto blob = MakeBlob(1024, 12);
    ASSERT_TRUE(h.Record(5, blob, {}));
    EXPECT_FALSE(h.Record(5, blob, {}));
    EXPECT_FALSE(h.Record(4, blob, {}));
    EXPECT_EQ(h.FrameCount(), 1u);
}

TEST(FrameHistory, RecordsNothingWhileDisabled)
{
    FrameHistory h;
    EXPECT_FALSE(h.Record(0, MakeBlob(1024, 13), {}));
    EXPECT_EQ(h.FrameCount(), 0u);
}

TEST(FrameHistory, DisablingClearsTheTimeline)
{
    FrameHistory h = MakeHistory(4);
    ASSERT_TRUE(h.Record(0, MakeBlob(1024, 14), {}));
    h.SetEnabled(false);
    EXPECT_EQ(h.FrameCount(), 0u);
}

TEST(FrameHistory, ChangedBlobSizeRestartsTheTimeline)
{
    FrameHistory h = MakeHistory(4);
    ASSERT_TRUE(h.Record(0, MakeBlob(1024, 15), {}));
    ASSERT_TRUE(h.Record(1, MakeBlob(1024, 16), {}));

    // A model switch changes the savestate size; the older states describe a machine
    // that no longer exists, so they must go rather than be reconstructed wrongly.
    const auto bigger = MakeBlob(2048, 17);
    ASSERT_TRUE(h.Record(2, bigger, {}));
    EXPECT_EQ(h.FrameCount(), 1u);
    EXPECT_FALSE(h.Has(0));
    EXPECT_EQ(h.Reconstruct(2), bigger);
}

// ---- eviction ----

TEST(FrameHistory, EvictionNeverOrphansADelta)
{
    FrameHistory h = MakeHistory(8);
    // Room for a few keyframe groups, not for all of them.
    h.SetBudgetBytes(200 * 1024);

    auto blob = MakeBlob(16 * 1024, 20);
    std::vector<std::vector<uint8_t>> expected;
    for (uint64_t f = 0; f < 200; ++f) {
        Churn(blob, static_cast<uint32_t>(200 + f), 10);
        expected.push_back(blob);
        h.Record(f, blob, {});
    }

    uint64_t oldest = 0, newest = 0;
    ASSERT_TRUE(h.Range(oldest, newest));
    EXPECT_EQ(newest, 199u);
    EXPECT_GT(oldest, 0u) << "the budget should have forced eviction";

    // Everything still held must reconstruct exactly - an evicted keyframe would leave
    // its deltas undecodable, which is the failure this guards.
    for (uint64_t f = oldest; f <= newest; ++f) {
        SCOPED_TRACE(f);
        ASSERT_TRUE(h.Has(f));
        EXPECT_EQ(h.Reconstruct(f), expected[f]);
    }
    for (uint64_t f = 0; f < oldest; ++f)
        EXPECT_FALSE(h.Has(f));

    const auto s = h.Stats();
    EXPECT_GT(s.evictedFrames, 0u);
    EXPECT_LE(s.bytesUsed, s.bytesBudget);
}

TEST(FrameHistory, KeepsTheLastGroupWhenItAloneExceedsTheBudget)
{
    FrameHistory h = MakeHistory(1000);
    h.SetBudgetBytes(64 * 1024);

    auto blob = MakeBlob(32 * 1024, 21);
    for (uint64_t f = 0; f < 50; ++f) {
        Churn(blob, static_cast<uint32_t>(300 + f), 30);
        h.Record(f, blob, {});
    }

    // Only one keyframe exists, so there is no whole group to drop. Losing the timeline
    // entirely would be worse than overshooting the budget.
    EXPECT_GT(h.FrameCount(), 0u);
    EXPECT_TRUE(h.Has(49));
    EXPECT_EQ(h.Reconstruct(49), blob);
}

TEST(FrameHistory, RefusesToRecordWhenOneBlobExceedsTheBudget)
{
    FrameHistory h = MakeHistory(4);
    h.SetBudgetBytes(1024);
    EXPECT_FALSE(h.Record(0, MakeBlob(4096, 22), {}));
    EXPECT_EQ(h.FrameCount(), 0u);
    EXPECT_FALSE(h.Stats().hint.empty()) << "the UI needs to be told why nothing records";
}

TEST(FrameHistory, ShrinkingTheBudgetEvictsImmediately)
{
    FrameHistory h = MakeHistory(8);
    auto blob = MakeBlob(16 * 1024, 23);
    for (uint64_t f = 0; f < 100; ++f) {
        Churn(blob, static_cast<uint32_t>(400 + f), 10);
        h.Record(f, blob, {});
    }
    const size_t before = h.FrameCount();

    h.SetBudgetBytes(150 * 1024);
    EXPECT_LT(h.FrameCount(), before);
    EXPECT_LE(h.Stats().bytesUsed, h.Stats().bytesBudget);
    EXPECT_EQ(h.Reconstruct(99), blob);
}

// ---- trimming ----

TEST(FrameHistory, TrimAfterDropsTheRecordedFutureAndKeepsRecording)
{
    FrameHistory h = MakeHistory(10);
    auto blob = MakeBlob(8 * 1024, 30);
    std::vector<std::vector<uint8_t>> expected;
    for (uint64_t f = 0; f < 50; ++f) {
        Churn(blob, static_cast<uint32_t>(500 + f), 10);
        expected.push_back(blob);
        h.Record(f, blob, {});
    }

    h.TrimAfter(24);
    EXPECT_EQ(h.FrameCount(), 25u);
    EXPECT_TRUE(h.Has(24));
    EXPECT_FALSE(h.Has(25));
    EXPECT_EQ(h.Reconstruct(24), expected[24]);

    // Recording must resume cleanly on top of the trimmed timeline - the delta base is
    // now frame 24, not the frame 49 that was just discarded.
    auto resumed = expected[24];
    Churn(resumed, 999, 10);
    ASSERT_TRUE(h.Record(25, resumed, {}));
    EXPECT_EQ(h.Reconstruct(25), resumed);
    EXPECT_EQ(h.Reconstruct(24), expected[24]);
}

TEST(FrameHistory, TrimAfterCanEmptyTheTimeline)
{
    FrameHistory h = MakeHistory(10);
    const auto blob = MakeBlob(4096, 31);
    ASSERT_TRUE(h.Record(3, blob, {}));
    h.TrimAfter(0);
    EXPECT_EQ(h.FrameCount(), 0u);

    const auto fresh = MakeBlob(4096, 32);
    ASSERT_TRUE(h.Record(0, fresh, {}));
    EXPECT_EQ(h.Reconstruct(0), fresh);
}

// ---- stats ----

TEST(FrameHistory, StatsReportRangeAndReach)
{
    FrameHistory h = MakeHistory(10);
    h.SetBudgetBytes(4 * 1024 * 1024);

    auto blob = MakeBlob(16 * 1024, 40);
    for (uint64_t f = 100; f < 160; ++f) {
        Churn(blob, static_cast<uint32_t>(600 + f), 10);
        h.Record(f, blob, {});
    }

    const auto s = h.Stats();
    EXPECT_TRUE(s.recording);
    EXPECT_FALSE(s.empty);
    EXPECT_EQ(s.oldestFrame, 100u);
    EXPECT_EQ(s.newestFrame, 159u);
    EXPECT_EQ(s.blobSize, 16u * 1024u);
    EXPECT_GT(s.avgDeltaBytes, 0.0);
    EXPECT_LT(s.avgDeltaBytes, static_cast<double>(s.blobSize));
    EXPECT_GT(s.estimatedCapacityFrames, s.frameCount)
        << "an unfilled budget should reach further back than what is stored";
}

TEST(FrameHistory, EmptyStatsAreInert)
{
    FrameHistory h;
    const auto s = h.Stats();
    EXPECT_FALSE(s.recording);
    EXPECT_TRUE(s.empty);
    EXPECT_EQ(s.frameCount, 0u);
    EXPECT_EQ(s.estimatedCapacityFrames, 0u);

    uint64_t oldest = 0, newest = 0;
    EXPECT_FALSE(h.Range(oldest, newest));
    EXPECT_TRUE(h.Reconstruct(0).empty());
}

// ---- serialization ----

TEST(FrameHistory, SerializeRoundTripsTheWholeTimeline)
{
    FrameHistory h = MakeHistory(10);
    auto blob = MakeBlob(8 * 1024, 50);
    std::vector<std::vector<uint8_t>> expected;
    for (uint64_t f = 10; f < 70; ++f) {
        Churn(blob, static_cast<uint32_t>(700 + f), 10);
        expected.push_back(blob);
        ASSERT_TRUE(h.Record(f, blob, MakeInput(static_cast<uint16_t>(f))));
    }

    const std::vector<uint8_t> stream = h.Serialize();
    ASSERT_FALSE(stream.empty());

    FrameHistory restored = MakeHistory(10);
    ASSERT_TRUE(restored.Deserialize(stream.data(), stream.size(), 8 * 1024));

    uint64_t oldest = 0, newest = 0;
    ASSERT_TRUE(restored.Range(oldest, newest));
    EXPECT_EQ(oldest, 10u);
    EXPECT_EQ(newest, 69u);
    EXPECT_EQ(restored.FrameCount(), 60u);

    for (uint64_t f = 10; f < 70; ++f) {
        SCOPED_TRACE(f);
        EXPECT_EQ(restored.Reconstruct(f), expected[f - 10]);
        FrameInput in;
        ASSERT_TRUE(restored.GetInput(f, in));
        EXPECT_EQ(in.joypad[0], static_cast<uint16_t>(f));
    }
}

TEST(FrameHistory, RecordingResumesOnTopOfARestoredTimeline)
{
    FrameHistory h = MakeHistory(10);
    auto blob = MakeBlob(4096, 51);
    for (uint64_t f = 0; f < 25; ++f) {
        Churn(blob, static_cast<uint32_t>(800 + f), 5);
        h.Record(f, blob, {});
    }
    const std::vector<uint8_t> stream = h.Serialize();

    FrameHistory restored = MakeHistory(10);
    ASSERT_TRUE(restored.Deserialize(stream.data(), stream.size(), 4096));

    // The delta base has to have been rebuilt, or the next frame is encoded against nothing.
    auto next = blob;
    Churn(next, 8080, 5);
    ASSERT_TRUE(restored.Record(25, next, {}));
    EXPECT_EQ(restored.Reconstruct(25), next);
    EXPECT_EQ(restored.Reconstruct(24), blob);
}

TEST(FrameHistory, DeserializeRejectsAMismatchedMachine)
{
    FrameHistory h = MakeHistory(10);
    const auto blob = MakeBlob(4096, 52);
    ASSERT_TRUE(h.Record(0, blob, {}));
    const std::vector<uint8_t> stream = h.Serialize();

    FrameHistory restored = MakeHistory(10);
    // A timeline recorded on a different machine holds states this core cannot apply.
    EXPECT_FALSE(restored.Deserialize(stream.data(), stream.size(), 8192));
    EXPECT_EQ(restored.FrameCount(), 0u);
    // With the check waived it loads.
    EXPECT_TRUE(restored.Deserialize(stream.data(), stream.size(), 0));
    EXPECT_EQ(restored.FrameCount(), 1u);
}

TEST(FrameHistory, DeserializeRejectsGarbageAndTruncation)
{
    FrameHistory h = MakeHistory(10);
    auto blob = MakeBlob(4096, 53);
    for (uint64_t f = 0; f < 12; ++f) {
        Churn(blob, static_cast<uint32_t>(900 + f), 5);
        h.Record(f, blob, {});
    }
    const std::vector<uint8_t> stream = h.Serialize();

    FrameHistory restored = MakeHistory(10);
    const std::vector<uint8_t> garbage(64, 0xAB);
    EXPECT_FALSE(restored.Deserialize(garbage.data(), garbage.size(), 0));
    EXPECT_EQ(restored.FrameCount(), 0u);

    // Every truncation must be refused outright rather than half-loaded: a partially
    // applied timeline would hand the core states that decode to nonsense.
    for (size_t cut : {size_t(8), stream.size() / 3, stream.size() / 2, stream.size() - 1}) {
        SCOPED_TRACE(cut);
        EXPECT_FALSE(restored.Deserialize(stream.data(), cut, 0));
        EXPECT_EQ(restored.FrameCount(), 0u);
    }
    EXPECT_TRUE(restored.Deserialize(stream.data(), stream.size(), 0));
}

TEST(FrameHistory, SerializeOfAnEmptyTimelineRoundTripsToEmpty)
{
    FrameHistory h = MakeHistory(10);
    const std::vector<uint8_t> stream = h.Serialize();
    FrameHistory restored = MakeHistory(10);
    // No entries means no blob size, and nothing to anchor a timeline on.
    EXPECT_FALSE(restored.Deserialize(stream.data(), stream.size(), 0));
    EXPECT_EQ(restored.FrameCount(), 0u);
}

// ---- timeline adjacency ----
//
// Frame numbers are not always consecutive: a mid-frame breakpoint stop never completes a
// frame under its number, and the recorder simply never sees one. Everything that walks the
// timeline has to move between *entries*, because entry N+1 is the machine state that
// follows entry N whatever the numbers say. Walking by frame number instead is what left a
// single missing frame able to strand a seek and stop a playback dead.

TEST(FrameHistoryAdjacency, WalksEntriesAcrossAMissingFrame)
{
    auto h = MakeHistory(4);
    auto blob = MakeBlob(4096, 1);
    // 10, 11, then a hole where 12 would be, then 13.
    for (uint64_t f : {10ull, 11ull, 13ull, 14ull}) {
        Churn(blob, static_cast<uint32_t>(f), 6);
        ASSERT_TRUE(h.Record(f, blob, MakeInput(static_cast<uint16_t>(f))));
    }

    EXPECT_FALSE(h.Has(12));

    uint64_t v = 0;
    EXPECT_TRUE(h.NextFrame(11, v));
    EXPECT_EQ(v, 13u); // the hole is stepped over, not treated as the end
    EXPECT_TRUE(h.PreviousFrame(13, v));
    EXPECT_EQ(v, 11u);

    // Ends of the timeline have no neighbour in that direction.
    EXPECT_FALSE(h.PreviousFrame(10, v));
    EXPECT_FALSE(h.NextFrame(14, v));
    // A frame that is not held has no neighbours either - the caller has to snap first.
    EXPECT_FALSE(h.NextFrame(12, v));
}

TEST(FrameHistoryAdjacency, ReconstructsAcrossAMissingFrame)
{
    auto h = MakeHistory(4);
    auto blob = MakeBlob(4096, 2);
    std::vector<std::vector<uint8_t>> expected;
    const std::vector<uint64_t> frames = {1, 2, 5, 6, 9};
    for (uint64_t f : frames) {
        Churn(blob, static_cast<uint32_t>(f * 7), 8);
        ASSERT_TRUE(h.Record(f, blob, MakeInput(static_cast<uint16_t>(f))));
        expected.push_back(blob);
    }
    // A gap in the numbering says nothing about the delta chain: consecutive entries are
    // consecutive machine states, so every one of them still rebuilds byte for byte.
    for (size_t i = 0; i < frames.size(); ++i)
        EXPECT_EQ(h.Reconstruct(frames[i]), expected[i]) << "frame " << frames[i];
}

TEST(FrameHistoryAdjacency, NearestFrameSnapsOntoSomethingRecorded)
{
    auto h = MakeHistory(4);
    auto blob = MakeBlob(2048, 3);
    for (uint64_t f : {10ull, 20ull, 30ull}) {
        Churn(blob, static_cast<uint32_t>(f), 4);
        ASSERT_TRUE(h.Record(f, blob, FrameInput{}));
    }

    uint64_t v = 0;
    EXPECT_TRUE(h.NearestFrame(20, v));
    EXPECT_EQ(v, 20u); // exact hit stays put
    EXPECT_TRUE(h.NearestFrame(21, v));
    EXPECT_EQ(v, 20u);
    EXPECT_TRUE(h.NearestFrame(29, v));
    EXPECT_EQ(v, 30u);
    // A tie goes to the older frame: a scrub landing between two entries is nearly always
    // reaching for the one it has already passed.
    EXPECT_TRUE(h.NearestFrame(25, v));
    EXPECT_EQ(v, 20u);
    // Outside the range in either direction clamps onto the ends.
    EXPECT_TRUE(h.NearestFrame(1, v));
    EXPECT_EQ(v, 10u);
    EXPECT_TRUE(h.NearestFrame(9999, v));
    EXPECT_EQ(v, 30u);

    FrameHistory empty;
    EXPECT_FALSE(empty.NearestFrame(5, v));
}

TEST(FrameHistoryAdjacency, FirstSeekableIsTheSecondFrameHeld)
{
    auto h = MakeHistory(4);
    auto blob = MakeBlob(1024, 4);
    uint64_t v = 0;

    EXPECT_FALSE(h.FirstSeekable(v));
    ASSERT_TRUE(h.Record(7, blob, FrameInput{}));
    // One frame held is a base to replay from, never a destination.
    EXPECT_FALSE(h.FirstSeekable(v));

    Churn(blob, 1, 4);
    ASSERT_TRUE(h.Record(9, blob, FrameInput{}));
    EXPECT_TRUE(h.FirstSeekable(v));
    EXPECT_EQ(v, 9u); // the second entry, not oldest+1 - frame 8 was never recorded
}

TEST(FrameHistoryStatsGaps, CountsPlacesWhereFrameNumbersSkip)
{
    auto h = MakeHistory(4);
    auto blob = MakeBlob(1024, 5);
    for (uint64_t f : {1ull, 2ull, 3ull}) {
        Churn(blob, static_cast<uint32_t>(f), 2);
        ASSERT_TRUE(h.Record(f, blob, FrameInput{}));
    }
    EXPECT_EQ(h.Stats().gapCount, 0u);

    Churn(blob, 10, 2);
    ASSERT_TRUE(h.Record(8, blob, FrameInput{})); // 4..7 missing
    EXPECT_EQ(h.Stats().gapCount, 1u);

    Churn(blob, 11, 2);
    ASSERT_TRUE(h.Record(9, blob, FrameInput{})); // contiguous again
    EXPECT_EQ(h.Stats().gapCount, 1u);

    Churn(blob, 12, 2);
    ASSERT_TRUE(h.Record(20, blob, FrameInput{}));
    EXPECT_EQ(h.Stats().gapCount, 2u);
}
