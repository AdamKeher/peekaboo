// Tests for the break-on-KPrintF-text rule store. AmigaEmulator feeds each completed serial
// debug-output line to MatchLine(); everything about *whether* a line breaks lives here, so
// these tests pin the match modes, case handling, one-shot behaviour and session round-trip.

#include "core/KprintfBreakpoints.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

using adm::core::KprintfBreakpoints;
using adm::core::KprintfMatch;

TEST(KprintfBreakpoints, ContainsMatchesAnywhereAndIsCaseInsensitiveByDefault)
{
    KprintfBreakpoints bps;
    ASSERT_NE(bps.Add("level done", KprintfMatch::Contains, /*caseSensitive=*/false,
                      /*oneShot=*/false),
              0);

    EXPECT_TRUE(bps.MatchLine("player: LEVEL DONE, score 100").has_value());
    EXPECT_FALSE(bps.MatchLine("player: level failed").has_value());
}

TEST(KprintfBreakpoints, CaseSensitiveRuleRejectsDifferentCasing)
{
    KprintfBreakpoints bps;
    ASSERT_NE(bps.Add("Boom", KprintfMatch::Contains, /*caseSensitive=*/true, false), 0);

    EXPECT_FALSE(bps.MatchLine("boom").has_value());
    EXPECT_TRUE(bps.MatchLine("-- Boom --").has_value());
}

TEST(KprintfBreakpoints, ExactAndPrefixModes)
{
    KprintfBreakpoints exact;
    ASSERT_NE(exact.Add("ready", KprintfMatch::Exact, false, false), 0);
    EXPECT_TRUE(exact.MatchLine("ready").has_value());
    EXPECT_FALSE(exact.MatchLine("ready to go").has_value());

    KprintfBreakpoints prefix;
    ASSERT_NE(prefix.Add("ERR", KprintfMatch::Prefix, false, false), 0);
    EXPECT_TRUE(prefix.MatchLine("ERR: out of chip ram").has_value());
    EXPECT_FALSE(prefix.MatchLine("fatal ERR: out of chip ram").has_value());
}

TEST(KprintfBreakpoints, RegexModeSearchesAnywhere)
{
    KprintfBreakpoints bps;
    ASSERT_NE(bps.Add("frame [0-9]+ dropped", KprintfMatch::Regex, false, false), 0);

    auto hit = bps.MatchLine("warning: frame 42 dropped");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->line, "warning: frame 42 dropped");
    EXPECT_FALSE(bps.MatchLine("frame dropped").has_value());
}

TEST(KprintfBreakpoints, InvalidRegexIsRejectedRatherThanNeverFiring)
{
    KprintfBreakpoints bps;
    EXPECT_EQ(bps.Add("unterminated (group", KprintfMatch::Regex, false, false), 0);
    EXPECT_TRUE(bps.Empty());
}

TEST(KprintfBreakpoints, DuplicateRuleIsRejected)
{
    KprintfBreakpoints bps;
    ASSERT_NE(bps.Add("hello", KprintfMatch::Contains, false, false), 0);
    EXPECT_EQ(bps.Add("hello", KprintfMatch::Contains, false, false), 0);
    // Same text in a different mode / casing is a genuinely different rule.
    EXPECT_NE(bps.Add("hello", KprintfMatch::Exact, false, false), 0);
    EXPECT_NE(bps.Add("hello", KprintfMatch::Contains, true, false), 0);
    EXPECT_EQ(bps.Entries().size(), 3u);
}

TEST(KprintfBreakpoints, EmptyTextIsRejected)
{
    KprintfBreakpoints bps;
    EXPECT_EQ(bps.Add("", KprintfMatch::Contains, false, false), 0);
    EXPECT_TRUE(bps.Empty());
}

TEST(KprintfBreakpoints, HitsAndLastLineAreRecorded)
{
    KprintfBreakpoints bps;
    const int id = bps.Add("tick", KprintfMatch::Contains, false, false);
    ASSERT_NE(id, 0);

    ASSERT_TRUE(bps.MatchLine("tick 1").has_value());
    ASSERT_TRUE(bps.MatchLine("tick 2").has_value());

    const auto* e = bps.Find(id);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->hits, 2u);
    EXPECT_EQ(e->lastLine, "tick 2");

    bps.ResetHits();
    EXPECT_EQ(bps.Find(id)->hits, 0u);
    EXPECT_TRUE(bps.Find(id)->lastLine.empty());
}

TEST(KprintfBreakpoints, OneShotRuleIsRemovedAfterItFires)
{
    KprintfBreakpoints bps;
    ASSERT_NE(bps.Add("boot", KprintfMatch::Contains, false, /*oneShot=*/true), 0);

    auto hit = bps.MatchLine("boot ok");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->text, "boot"); // the banner still names what matched
    EXPECT_TRUE(bps.Empty());
    EXPECT_FALSE(bps.MatchLine("boot ok").has_value());
}

TEST(KprintfBreakpoints, DisabledRuleDoesNotFire)
{
    KprintfBreakpoints bps;
    const int id = bps.Add("stop", KprintfMatch::Contains, false, false);
    ASSERT_NE(id, 0);

    ASSERT_TRUE(bps.SetEnabled(id, false));
    EXPECT_FALSE(bps.MatchLine("stop now").has_value());
    ASSERT_TRUE(bps.SetEnabled(id, true));
    EXPECT_TRUE(bps.MatchLine("stop now").has_value());
}

TEST(KprintfBreakpoints, FirstMatchingRuleWinsAndOnlyItCountsTheHit)
{
    KprintfBreakpoints bps;
    const int first = bps.Add("err", KprintfMatch::Contains, false, false);
    const int second = bps.Add("out of memory", KprintfMatch::Contains, false, false);
    ASSERT_NE(first, 0);
    ASSERT_NE(second, 0);

    auto hit = bps.MatchLine("err: out of memory");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->id, first);
    EXPECT_EQ(bps.Find(first)->hits, 1u);
    EXPECT_EQ(bps.Find(second)->hits, 0u);
}

TEST(KprintfBreakpoints, RemoveByIdAndByText)
{
    KprintfBreakpoints bps;
    const int id = bps.Add("alpha", KprintfMatch::Contains, false, false);
    ASSERT_NE(bps.Add("beta", KprintfMatch::Contains, false, false), 0);

    EXPECT_TRUE(bps.Remove(id));
    EXPECT_FALSE(bps.Remove(id));
    EXPECT_TRUE(bps.RemoveByText("beta"));
    EXPECT_FALSE(bps.RemoveByText("beta"));
    EXPECT_TRUE(bps.Empty());
}

TEST(KprintfBreakpoints, ExportImportRoundTrip)
{
    KprintfBreakpoints bps;
    ASSERT_NE(bps.Add("plain", KprintfMatch::Contains, false, false), 0);
    const int disabledId = bps.Add("MiXeD", KprintfMatch::Exact, /*caseSensitive=*/true, false);
    ASSERT_NE(disabledId, 0);
    ASSERT_TRUE(bps.SetEnabled(disabledId, false));
    ASSERT_NE(bps.Add("re[0-9]", KprintfMatch::Regex, false, /*oneShot=*/true), 0);

    KprintfBreakpoints restored;
    restored.Import(bps.Export());

    ASSERT_EQ(restored.Entries().size(), 3u);
    EXPECT_EQ(restored.Entries()[0].text, "plain");
    EXPECT_EQ(restored.Entries()[0].match, KprintfMatch::Contains);
    EXPECT_TRUE(restored.Entries()[0].enabled);
    EXPECT_EQ(restored.Entries()[1].match, KprintfMatch::Exact);
    EXPECT_TRUE(restored.Entries()[1].caseSensitive);
    EXPECT_FALSE(restored.Entries()[1].enabled);
    EXPECT_EQ(restored.Entries()[2].match, KprintfMatch::Regex);
    EXPECT_TRUE(restored.Entries()[2].oneShot);
    // A restored regex rule must still be compiled, not just remembered.
    EXPECT_TRUE(restored.MatchLine("re7").has_value());
}

TEST(KprintfBreakpoints, ImportReplacesExistingRules)
{
    KprintfBreakpoints bps;
    ASSERT_NE(bps.Add("old", KprintfMatch::Contains, false, false), 0);
    bps.Import(nlohmann::json::array());
    EXPECT_TRUE(bps.Empty());
}

TEST(KprintfBreakpoints, MatchNameAndParseMatchRoundTrip)
{
    for (auto m :
         {KprintfMatch::Contains, KprintfMatch::Exact, KprintfMatch::Prefix, KprintfMatch::Regex}) {
        auto parsed = KprintfBreakpoints::ParseMatch(KprintfBreakpoints::MatchName(m));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, m);
    }
    EXPECT_TRUE(KprintfBreakpoints::ParseMatch("REGEX").has_value());
    EXPECT_FALSE(KprintfBreakpoints::ParseMatch("fuzzy").has_value());
}

TEST(KprintfBreakpoints, EmptyLineNeverMatches)
{
    KprintfBreakpoints bps;
    // A "contains empty string" rule can't be created, but a regex that matches everything can.
    ASSERT_NE(bps.Add(".*", KprintfMatch::Regex, false, false), 0);
    EXPECT_FALSE(bps.MatchLine("").has_value());
    EXPECT_TRUE(bps.MatchLine("anything").has_value());
}
