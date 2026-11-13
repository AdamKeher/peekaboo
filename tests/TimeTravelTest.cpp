#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>

#include "core/TimeTravel.h"

using adm::core::TimePosition;
using adm::core::TimeTravel;

namespace {

/**
 * A toy deterministic machine: executing instruction i of frame f puts the PC at a value
 * derived from (f, i). That makes "did the replay land where history says" checkable exactly,
 * which is the property TimeTravel exists to guarantee.
 */
struct FakeMachine
{
    static constexpr uint32_t kInstructionsPerFrame = 5000;

    uint64_t frame = 0;
    uint32_t index = 0;
    uint64_t oldest = 0, newest = 0;

    int restores = 0;      // frame restores performed
    int loads = 0;         // checkpoint restores performed
    uint64_t replayed = 0; // instructions replayed, the cost this class is meant to minimise

    // Flip to make replay produce different PCs than history recorded.
    bool diverge = false;

    static uint32_t PcAt(uint64_t f, uint32_t i)
    {
        return 0x00C00000u + static_cast<uint32_t>(f * 0x100 + i * 2);
    }
    uint32_t Pc() const { return PcAt(frame, index) + (diverge ? 0x2000u : 0u); }

    TimeTravel::Deps Deps()
    {
        TimeTravel::Deps d;
        d.restoreFrame = [this](uint64_t f) {
            if (f < oldest || f > newest)
                return false;
            ++restores;
            frame = f;
            index = 0;
            return true;
        };
        d.recordedRange = [this](uint64_t& o, uint64_t& n) {
            o = oldest;
            n = newest;
            return true;
        };
        d.replayInstructions = [this](uint32_t n) {
            index += n;
            replayed += n;
            return true;
        };
        d.currentPc = [this] { return Pc(); };
        d.recordedPc = [this](uint64_t f, uint32_t i, uint32_t& out) {
            if (f < oldest || f > newest || i > kInstructionsPerFrame)
                return false;
            out = PcAt(f, i); // history, unaffected by `diverge`
            return true;
        };
        d.frameInstructionCount = [this](uint64_t f, uint32_t& out) {
            if (f < oldest || f > newest)
                return false;
            out = kInstructionsPerFrame + 1; // indices 0..kInstructionsPerFrame
            return true;
        };
        d.saveState = [this] {
            // Encode the position; enough for the checkpoint ladder to be exercised.
            std::vector<uint8_t> s(12);
            std::memcpy(s.data(), &frame, 8);
            std::memcpy(s.data() + 8, &index, 4);
            return s;
        };
        d.loadState = [this](const std::vector<uint8_t>& s) {
            if (s.size() != 12)
                return false;
            ++loads;
            std::memcpy(&frame, s.data(), 8);
            std::memcpy(&index, s.data() + 8, 4);
            return true;
        };
        return d;
    }
};

} // namespace

TEST(TimeTravelTest, SeeksToAnExactPosition)
{
    FakeMachine m;
    m.oldest = 10;
    m.newest = 20;
    TimeTravel tt;
    tt.SetDeps(m.Deps());

    std::string err;
    ASSERT_TRUE(tt.SeekTo(TimePosition{15, 300, 0}, err)) << err;
    EXPECT_EQ(m.frame, 15u);
    EXPECT_EQ(m.index, 300u);
    EXPECT_EQ(m.Pc(), FakeMachine::PcAt(15, 300));
}

TEST(TimeTravelTest, StepBackMovesExactlyOneInstruction)
{
    FakeMachine m;
    m.oldest = 10;
    m.newest = 20;
    TimeTravel tt;
    tt.SetDeps(m.Deps());

    std::string err;
    TimePosition at{15, 100, 0};
    ASSERT_TRUE(tt.SeekTo(at, err)) << err;

    for (uint32_t expected = 99; expected > 95; --expected) {
        ASSERT_TRUE(tt.StepBack(at, err)) << err;
        EXPECT_EQ(at.index, expected);
        EXPECT_EQ(at.pc, FakeMachine::PcAt(15, expected));
    }
}

TEST(TimeTravelTest, RefusesToPresentADivergentReplayAsHistory)
{
    // The property that matters most. A savestate does not carry the host filesystem or
    // in-flight disk DMA, so a replay can legitimately produce different execution. Showing
    // that as "what happened" would be a lie, and a silently wrong machine state is far worse
    // than a refusal.
    FakeMachine m;
    m.oldest = 10;
    m.newest = 20;
    TimeTravel tt;
    tt.SetDeps(m.Deps());

    std::string err;
    ASSERT_TRUE(tt.SeekTo(TimePosition{15, 50, 0}, err)) << err;

    m.diverge = true;
    tt.Clear(); // force a real replay rather than a checkpoint restore
    EXPECT_FALSE(tt.SeekTo(TimePosition{15, 200, 0}, err));
    EXPECT_NE(err.find("diverged"), std::string::npos) << err;
    EXPECT_NE(err.find("not reproducible"), std::string::npos) << err;
}

TEST(TimeTravelTest, CheckpointsMakeRepeatedStepBackCheap)
{
    // Without a checkpoint ladder every step back replays from the frame start, so walking back
    // N instructions costs O(N^2). This is the whole reason the ladder exists.
    FakeMachine m;
    m.oldest = 10;
    m.newest = 20;
    TimeTravel tt;
    tt.SetDeps(m.Deps());
    tt.SetCheckpointStride(64);

    std::string err;
    TimePosition at{15, 4000, 0};
    ASSERT_TRUE(tt.SeekTo(at, err)) << err;

    const uint64_t afterSeek = m.replayed;
    for (int i = 0; i < 20; ++i)
        ASSERT_TRUE(tt.StepBack(at, err)) << err;

    const uint64_t stepCost = m.replayed - afterSeek;
    // Twenty steps back from 4000 would replay ~80,000 instructions without checkpoints.
    EXPECT_LT(stepCost, 20u * 64u * 3u) << "replayed " << stepCost << " instructions";
    EXPECT_GT(m.loads, 0) << "checkpoints were never used";
    EXPECT_EQ(at.index, 3980u);
}

TEST(TimeTravelTest, StepBackCrossesAFrameBoundary)
{
    FakeMachine m;
    m.oldest = 10;
    m.newest = 20;
    TimeTravel tt;
    tt.SetDeps(m.Deps());

    std::string err;
    TimePosition at{15, 0, 0};
    ASSERT_TRUE(tt.SeekTo(at, err)) << err;
    ASSERT_TRUE(tt.StepBack(at, err)) << err;
    EXPECT_EQ(at.frame, 14u) << "must land in the previous frame";
    EXPECT_EQ(at.index, FakeMachine::kInstructionsPerFrame);
}

TEST(TimeTravelTest, RunningOutOfHistoryIsReportedNotFaked)
{
    FakeMachine m;
    m.oldest = 10;
    m.newest = 20;
    TimeTravel tt;
    tt.SetDeps(m.Deps());

    std::string err;
    TimePosition at{10, 0, 0};
    ASSERT_TRUE(tt.SeekTo(at, err)) << err;
    EXPECT_FALSE(tt.StepBack(at, err));
    EXPECT_NE(err.find("no more reverse history"), std::string::npos) << err;
}

TEST(TimeTravelTest, ReverseContinueStopsOnABreakpoint)
{
    FakeMachine m;
    m.oldest = 10;
    m.newest = 20;
    TimeTravel tt;
    tt.SetDeps(m.Deps());

    std::string err;
    TimePosition at{15, 100, 0};
    ASSERT_TRUE(tt.SeekTo(at, err)) << err;

    const uint32_t wanted = FakeMachine::PcAt(15, 60);
    ASSERT_TRUE(tt.ReverseContinue({wanted}, at, err)) << err;
    EXPECT_EQ(at.index, 60u);
    EXPECT_EQ(at.pc, wanted);
}

TEST(TimeTravelTest, ReachabilityIsHonestWhenNothingIsRecorded)
{
    TimeTravel tt; // no deps at all
    const auto reach = tt.Reachable();
    EXPECT_FALSE(reach.available);
    EXPECT_FALSE(reach.hint.empty());
}
