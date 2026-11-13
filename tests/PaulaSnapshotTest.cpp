#include "core/PaulaStateSnapshot.h"

#include <gtest/gtest.h>

using adm::core::PaulaPeriodToHz;
using adm::core::PaulaPeriodToNote;

namespace {

// --- period -> Hz -------------------------------------------------------------------------

TEST(PaulaPeriod, PalRateMatchesHardware)
{
    // A sample played at period 428 (ProTracker C-2) runs at ~8287 Hz on PAL — the number every
    // Amiga sample-rate table quotes.
    EXPECT_NEAR(PaulaPeriodToHz(428), 8287.1, 0.1);
    // Period 254 (A-2) runs at ~13964 Hz.
    EXPECT_NEAR(PaulaPeriodToHz(254), 13964.2, 0.1);
    // Halving the period doubles the rate.
    EXPECT_NEAR(PaulaPeriodToHz(214), 2.0 * PaulaPeriodToHz(428), 0.1);
}

TEST(PaulaPeriod, NtscRateIsHigherThanPal)
{
    const double pal = PaulaPeriodToHz(428, true);
    const double ntsc = PaulaPeriodToHz(428, false);
    EXPECT_GT(ntsc, pal);
    EXPECT_NEAR(ntsc, 8363.4, 0.1);
}

TEST(PaulaPeriod, IllegalPeriodsReportSilenceNotInfinity)
{
    // Period 0 would divide by zero and period 1 yields a meaningless megahertz figure; both must
    // read as silence so the UI never prints "3546895 Hz" for an idle voice.
    EXPECT_DOUBLE_EQ(PaulaPeriodToHz(0), 0.0);
    EXPECT_DOUBLE_EQ(PaulaPeriodToHz(1), 0.0);
}

// --- period -> note -----------------------------------------------------------------------

TEST(PaulaNote, MatchesProTrackerTable)
{
    // The canonical ProTracker period table, one entry per octave boundary.
    EXPECT_EQ(PaulaPeriodToNote(856), "C-1");
    EXPECT_EQ(PaulaPeriodToNote(808), "C#1");
    EXPECT_EQ(PaulaPeriodToNote(762), "D-1");
    EXPECT_EQ(PaulaPeriodToNote(640), "F-1");
    EXPECT_EQ(PaulaPeriodToNote(604), "F#1");
    EXPECT_EQ(PaulaPeriodToNote(428), "C-2");
    EXPECT_EQ(PaulaPeriodToNote(404), "C#2");
    EXPECT_EQ(PaulaPeriodToNote(320), "F-2");
    EXPECT_EQ(PaulaPeriodToNote(302), "F#2");
    EXPECT_EQ(PaulaPeriodToNote(214), "C-3");
    EXPECT_EQ(PaulaPeriodToNote(160), "F-3");
    EXPECT_EQ(PaulaPeriodToNote(151), "F#3");
    EXPECT_EQ(PaulaPeriodToNote(113), "B-3");
}

TEST(PaulaNote, SharpsAreThreeColumnsWithNoSeparator)
{
    // Trackers render sharps as "C#2", not "C#-2" — the '#' is the padding. Naturals keep the
    // '-'. Both must be three characters so a column of notes lines up.
    EXPECT_EQ(PaulaPeriodToNote(808), "C#1");
    EXPECT_EQ(PaulaPeriodToNote(202), "C#3");
    EXPECT_EQ(PaulaPeriodToNote(856).size(), 3u);
    EXPECT_EQ(PaulaPeriodToNote(808).size(), 3u);
}

TEST(PaulaNote, OctavesAboveTheTableStillResolve)
{
    // Periods below the ProTracker range are legal on hardware (Paula clamps around 124 in
    // practice, but a program can write anything), so the mapping must not fall off the table.
    EXPECT_EQ(PaulaPeriodToNote(107), "C-4");
    EXPECT_EQ(PaulaPeriodToNote(54), "C-5");
}

TEST(PaulaNote, OctavesBelowTheTableUseFloorDivision)
{
    // Periods above 856 give a NEGATIVE semitone offset from the C-1 anchor. Truncating division
    // would round those toward zero and land them in the wrong octave, so this pins the floor.
    EXPECT_EQ(PaulaPeriodToNote(1712), "C-0");
    EXPECT_EQ(PaulaPeriodToNote(3424), "C--1");
    // One semitone below C-1 is B-0, not B-1.
    EXPECT_EQ(PaulaPeriodToNote(907), "B-0");
}

TEST(PaulaNote, IllegalPeriodsHaveNoNote)
{
    EXPECT_EQ(PaulaPeriodToNote(0), "--");
    EXPECT_EQ(PaulaPeriodToNote(1), "--");
}

TEST(PaulaNote, CentsReportOffsetFromTemperedNote)
{
    int cents = 999;
    // An exact table period is in tune with itself.
    PaulaPeriodToNote(856, &cents);
    EXPECT_EQ(cents, 0);

    // A period between two table entries is detuned, and the offset stays inside one semitone.
    PaulaPeriodToNote(832, &cents);
    EXPECT_GT(cents, 0);
    EXPECT_LE(cents, 50);

    // Illegal periods must clear the out-param rather than leave the caller's value in place.
    cents = 999;
    PaulaPeriodToNote(0, &cents);
    EXPECT_EQ(cents, 0);
}

// --- snapshot defaults --------------------------------------------------------------------

TEST(PaulaSnapshot, DefaultsToUnavailableWithAllVoicesAudible)
{
    // An unavailable snapshot must not read as "everything is muted" — callers distinguish the
    // two by `available`, and a zero mask would be a misleading answer for a stopped emulator.
    adm::core::PaulaStateSnapshot snap;
    EXPECT_FALSE(snap.available);
    EXPECT_FALSE(snap.traceAvailable);
    EXPECT_EQ(snap.channelMask, 0x0F);
    EXPECT_EQ(snap.channels.size(), 4u);
}

} // namespace
