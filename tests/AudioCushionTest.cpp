#include "core/AudioCushion.h"

#include <gtest/gtest.h>

#include <cstdint>

using adm::core::AudioCushion;

namespace {

constexpr uint32_t kTarget = 2048;
constexpr uint32_t kBlock = 882;    // one PAL frame of audio at 44.1 kHz
constexpr uint32_t kPeriod = 441;   // a 10 ms device callback
constexpr uint32_t kWindow = 44100; // one second of output

// The ring as the device callback sees it: the core delivers a block every 20 ms, the sound card
// takes a period every 10 ms. Every drop the cushion asks for is applied, as the callback does.
struct Ring
{
    AudioCushion cushion;
    uint32_t level = 0;
    uint64_t dropped = 0;
    uint32_t minEntry = UINT32_MAX;
    int step = 0;

    void Prime(uint32_t available)
    {
        level = available;
        const uint32_t drop = cushion.OnPrime(level);
        level -= drop;
        dropped += drop;
        level -= kPeriod;
    }

    // One 10 ms device callback, preceded by a block on every other one.
    void Tick(uint32_t extraProduced = 0)
    {
        if (++step % 2 == 0)
            level += kBlock;
        level += extraProduced;
        const uint32_t drop = cushion.OnCallback(level, kPeriod);
        level -= drop;
        dropped += drop;
        if (level < minEntry)
            minEntry = level;
        ASSERT_GE(level, kPeriod) << "the ring ran dry";
        level -= kPeriod;
    }

    void Run(int ticks)
    {
        for (int i = 0; i < ticks; ++i)
            Tick();
    }
};

} // namespace

TEST(AudioCushionTest, OutputStartsWithExactlyTheTarget)
{
    AudioCushion c;
    c.Reset(kTarget, kBlock, kPeriod, kWindow);
    EXPECT_EQ(c.OnPrime(kTarget), 0u);
    EXPECT_EQ(c.OnPrime(kTarget + 1), 1u);
    // A catch-up burst landing on an underrun: everything past the target is stale.
    EXPECT_EQ(c.OnPrime(8030), 8030u - kTarget);
}

TEST(AudioCushionTest, ASteadyStreamIsNeverTrimmed)
{
    Ring r;
    r.cushion.Reset(kTarget, kBlock, kPeriod, kWindow);
    r.Prime(kTarget);
    r.Run(100 * 60); // a minute
    EXPECT_EQ(r.dropped, 0u);
}

TEST(AudioCushionTest, ACatchUpBurstIsTrimmedBackWithinAWindow)
{
    Ring r;
    r.cushion.Reset(kTarget, kBlock, kPeriod, kWindow);
    r.Prime(kTarget);
    r.Run(500);
    const uint32_t steadyFloor = r.minEntry;

    // A 100 ms stall the ring rode out, then five frames of catch-up at once. The ring had not
    // drained by that much, so the burst leaves it five frames deeper than it needs to be.
    r.Tick(5 * kBlock);
    r.Run(100); // one window: at most one trim
    EXPECT_GE(r.dropped, 4u * kBlock);

    // Back where it was, and it stays there.
    const uint64_t droppedAfterTrim = r.dropped;
    r.minEntry = UINT32_MAX;
    r.Run(1000);
    EXPECT_EQ(r.dropped, droppedAfterTrim);
    EXPECT_LE(r.minEntry, kTarget);
    EXPECT_GE(r.minEntry + kBlock / 2, steadyFloor);
}

TEST(AudioCushionTest, ClockDriftIsTrimmedRarelyAndOnlyByAFraction)
{
    // A sound card 100 ppm slower than the producer: the ring grows ~4.4 frames a second.
    Ring r;
    r.cushion.Reset(kTarget, kBlock, kPeriod, kWindow);
    r.Prime(kTarget);
    int trims = 0;
    uint64_t last = 0;
    double owed = 0.0;
    for (int second = 0; second < 600; ++second) {
        for (int i = 0; i < 100; ++i) {
            owed += 4.41 / 100.0;
            const uint32_t extra = static_cast<uint32_t>(owed);
            owed -= extra;
            r.Tick(extra);
        }
        if (r.dropped != last) {
            ++trims;
            EXPECT_LE(r.dropped - last, kBlock) << "a drift trim should be small";
            last = r.dropped;
        }
    }
    // Ten minutes accumulate ~2650 frames of drift; each trim returns half a block.
    EXPECT_GE(trims, 2);
    EXPECT_LE(trims, 10);
}

TEST(AudioCushionTest, TrimNeverLeavesLessThanHalfABlockBelowTheTarget)
{
    AudioCushion c;
    c.Reset(kTarget, kBlock, kPeriod, kPeriod); // a window of one callback
    const uint32_t available = 9000;
    const uint32_t drop = c.OnCallback(available, kPeriod);
    EXPECT_EQ(available - drop, kTarget - kBlock / 2);
}

TEST(AudioCushionTest, PaceHoldsStillAtTheSetpoint)
{
    AudioCushion c;
    c.Reset(kTarget, kBlock, kPeriod, kWindow);
    // Output starts at the target and saws down by a block less a period between blocks.
    EXPECT_DOUBLE_EQ(c.Setpoint(), kTarget - (kBlock - kPeriod) / 2.0);
    EXPECT_DOUBLE_EQ(c.PaceFactor(c.Setpoint()), 1.0);
}

TEST(AudioCushionTest, PaceSlowsAFullRingAndHurriesALowOne)
{
    AudioCushion c;
    c.Reset(kTarget, kBlock, kPeriod, kWindow);
    const double half = kBlock / 2.0;
    EXPECT_LT(c.PaceFactor(c.Setpoint() + half), 1.0);
    EXPECT_GT(c.PaceFactor(c.Setpoint() - half), 1.0);
    EXPECT_NEAR(c.PaceFactor(c.Setpoint() + half), 1.0 - AudioCushion::kPaceGain / 2, 1e-12);
}

TEST(AudioCushionTest, PaceNeverExceedsWinUaesLimit)
{
    // A core producing far too little audio (the VPOSW fault: 485 frames per 882-frame frame)
    // must not be able to drive the emulator fast - that ran a demo at 91 fps once.
    AudioCushion c;
    c.Reset(kTarget, kBlock, kPeriod, kWindow);
    EXPECT_DOUBLE_EQ(c.PaceFactor(0.0), 1.0 + AudioCushion::kPaceLimit);
    EXPECT_DOUBLE_EQ(c.PaceFactor(32768.0), 1.0 - AudioCushion::kPaceLimit);
}

TEST(AudioCushionTest, PushRebuildsTheCushionOnAFreshStart)
{
    AudioCushion c;
    c.Reset(kTarget, kBlock, kPeriod, kWindow);
    const auto plan = c.PlanPush(0, kBlock, false);
    EXPECT_EQ(plan.silence, kTarget - kBlock);
    EXPECT_EQ(plan.skip, 0u);
    EXPECT_FALSE(plan.starved);
    EXPECT_EQ(c.Target(), kTarget);
}

TEST(AudioCushionTest, PushWritesASteadyBlockUntouched)
{
    AudioCushion c;
    c.Reset(kTarget, kBlock, kPeriod, kWindow);
    // A frame after the last write the sound card has taken about a block.
    const auto plan = c.PlanPush(kTarget - kBlock, kBlock, true);
    EXPECT_EQ(plan.silence, 0u);
    EXPECT_EQ(plan.skip, 0u);
}

TEST(AudioCushionTest, PushStarveRaisesTheTargetAndQuietLowersIt)
{
    AudioCushion c;
    c.Reset(kTarget, kBlock, kPeriod, kWindow);
    c.SetPushRange(kTarget + 2 * kPeriod, 10 * kBlock);
    const auto plan = c.PlanPush(0, kBlock, true);
    EXPECT_TRUE(plan.starved);
    EXPECT_EQ(c.Target(), kTarget + kPeriod);
    EXPECT_EQ(plan.silence, kTarget + kPeriod - kBlock);
    c.PlanPush(0, kBlock, true);
    c.PlanPush(0, kBlock, true); // capped
    EXPECT_EQ(c.Target(), kTarget + 2 * kPeriod);
    // Ten blocks played cleanly: half a period back down, never below where it started.
    for (int i = 0; i < 10; ++i)
        c.PlanPush(kTarget, kBlock, true);
    EXPECT_EQ(c.Target(), kTarget + 2 * kPeriod - kPeriod / 2);
    for (int i = 0; i < 200; ++i)
        c.PlanPush(kTarget, kBlock, true);
    EXPECT_EQ(c.Target(), kTarget);
}

TEST(AudioCushionTest, PushCapsACatchUpBurstAtTwoBlocksAboveTheTarget)
{
    AudioCushion c;
    c.Reset(kTarget, kBlock, kPeriod, kWindow);
    // A burst's next frame lands on a queue already a block deep plus 300.
    EXPECT_EQ(c.PlanPush(kTarget + kBlock + 300, kBlock, true).skip, 300u);
    EXPECT_EQ(c.PlanPush(kTarget + 2 * kBlock, kBlock, true).skip, kBlock);
}

TEST(AudioCushionTest, PushTrimsAQueueThatStaysDeepInHalfBlockSteps)
{
    AudioCushion c;
    c.Reset(kTarget, kBlock, kPeriod, 10 * kBlock);
    // Most of a window with the queue never below the target + 500: nothing to do yet.
    for (int i = 0; i < 9; ++i)
        EXPECT_EQ(c.PlanPush(kTarget + 500, kBlock, true).skip, 0u) << i;
    // The write that closes the window starts bringing the floor back to a block below the
    // target - 500 + a block in all - half a block per write.
    EXPECT_EQ(c.PlanPush(kTarget + 500, kBlock, true).skip, kBlock / 2);
    uint32_t skipped = kBlock / 2;
    for (int i = 0; i < 5; ++i)
        skipped += c.PlanPush(kTarget, kBlock, true).skip;
    EXPECT_EQ(skipped, 500u + kBlock);
}

TEST(AudioCushionTest, PushPacingHoldsHalfABlockBelowTheTarget)
{
    AudioCushion c;
    c.Reset(kTarget, kBlock, kPeriod, kWindow);
    EXPECT_DOUBLE_EQ(c.PushSetpoint(), kTarget - kBlock / 2.0);
    EXPECT_DOUBLE_EQ(c.PaceFactor(c.PushSetpoint(), c.PushSetpoint()), 1.0);
    EXPECT_LT(c.PaceFactor(kTarget, c.PushSetpoint()), 1.0);
}
