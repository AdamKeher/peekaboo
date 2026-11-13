#include <gtest/gtest.h>

#include "../src/core/ExocetActivity.h"

using adm::core::ExocetActivity;
using State = ExocetActivity::State;

namespace {

const std::vector<std::string> kGroups = {"av", "lst", "director"};

} // namespace

TEST(ExocetActivityTest, ARowNeverPlayedIsUnknown)
{
    ExocetActivity a;
    a.SetSong(0x111F7093, 100);
    EXPECT_EQ(a.At("lst", 10), State::Unknown);
    a.Mark(10, 0b110, kGroups); // lst and the director
    EXPECT_EQ(a.At("lst", 10), State::Active);
    EXPECT_EQ(a.At("director", 10), State::Active);
    EXPECT_EQ(a.At("av", 10), State::Inactive);
    EXPECT_EQ(a.At("no.such", 10), State::Inactive); // a seen row: nothing else was on
    EXPECT_EQ(a.At("lst", 11), State::Unknown);
    EXPECT_EQ(a.At("lst", -1), State::Unknown);
    EXPECT_EQ(a.At("lst", 100), State::Unknown);
    EXPECT_EQ(a.SeenRows(), 1);
}

TEST(ExocetActivityTest, ActiveRowsAndSpans)
{
    ExocetActivity a;
    a.SetSong(1, 50);
    for (int r = 0; r < 50; ++r)
        a.Mark(r, (r >= 10 && r < 20) || r >= 40 ? 0b010 : 0, kGroups);
    // a row marked twice (several frames a row) counts once
    a.Mark(12, 0b010, kGroups);
    EXPECT_EQ(a.ActiveRows("lst"), 20);
    const auto spans = a.ActiveSpans("lst");
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0], std::make_pair(10, 19));
    EXPECT_EQ(spans[1], std::make_pair(40, 49));
    EXPECT_TRUE(a.ActiveSpans("av").empty());
    ASSERT_EQ(a.SeenSpans().size(), 1u);
    EXPECT_EQ(a.SeenSpans()[0], std::make_pair(0, 49));
}

TEST(ExocetActivityTest, OnlyNewsMovesTheRevision)
{
    ExocetActivity a;
    a.SetSong(1, 10);
    const auto r0 = a.Revision();
    a.Mark(3, 0b1, kGroups);
    const auto r1 = a.Revision();
    EXPECT_GT(r1, r0);
    a.Mark(3, 0b1, kGroups); // the same again: nothing new to save
    EXPECT_EQ(a.Revision(), r1);
    a.Mark(3, 0b11, kGroups); // another group on that row is news
    EXPECT_GT(a.Revision(), r1);
    a.Mark(99, 0b1, kGroups); // past the song: ignored
    EXPECT_EQ(a.SeenRows(), 1);
}

TEST(ExocetActivityTest, AnotherSongStartsOver)
{
    ExocetActivity a;
    a.SetSong(1, 10);
    a.Mark(3, 0b1, kGroups);
    a.SetSong(1, 10); // the same song: kept
    EXPECT_EQ(a.At("av", 3), State::Active);
    a.SetSong(2, 10);
    EXPECT_EQ(a.At("av", 3), State::Unknown);
    EXPECT_EQ(a.SeenRows(), 0);
}

TEST(ExocetActivityTest, JsonRoundTrip)
{
    ExocetActivity a;
    a.SetSong(0x111F7093, 3617);
    for (int r = 0; r < 3617; ++r)
        a.Mark(r, 0b100 | (r >= 1249 && r < 2170 ? 0b010 : 0) | (r >= 2300 && r < 2600 ? 1 : 0),
               kGroups);
    const auto j = a.ToJson();
    EXPECT_EQ(j["song"], "0x111f7093");
    EXPECT_EQ(j["rows"], 3617);
    EXPECT_EQ(j["groups"]["lst"], nlohmann::json::parse("[[1249, 2169]]"));

    ExocetActivity b;
    ASSERT_TRUE(b.FromJson(j, 0x111F7093, 3617));
    EXPECT_EQ(b.SeenRows(), 3617);
    EXPECT_EQ(b.ActiveRows("lst"), 921);
    EXPECT_EQ(b.ActiveRows("av"), 300);
    EXPECT_EQ(b.At("av", 2299), State::Inactive);
    EXPECT_EQ(b.At("av", 2300), State::Active);
    EXPECT_EQ(b.ToJson(), j);
}

TEST(ExocetActivityTest, JsonOfAnotherSongOrMalformedIsRefused)
{
    ExocetActivity a;
    a.SetSong(5, 20);
    a.Mark(1, 1, kGroups);
    const auto j = a.ToJson();

    ExocetActivity b;
    b.SetSong(6, 20);
    b.Mark(2, 2, kGroups);
    EXPECT_FALSE(b.FromJson(j, 6, 20)); // another song
    EXPECT_FALSE(b.FromJson(j, 5, 21)); // other rows
    auto bad = j;
    bad["seen"] = nlohmann::json::parse("[[3, 40]]"); // past the song
    EXPECT_FALSE(b.FromJson(bad, 5, 20));
    bad = j;
    bad["groups"]["av"] = "all of it";
    EXPECT_FALSE(b.FromJson(bad, 5, 20));
    // what it had is untouched
    EXPECT_EQ(b.Song(), 6u);
    EXPECT_EQ(b.At("lst", 2), State::Active);
}

// EXO-01: a wrong-typed row count is a mismatch, not a throw
TEST(ExocetActivityTest, FromJsonRefusesWrongTypes)
{
    ExocetActivity a;
    const auto j = nlohmann::json::parse(R"({"song": "0x00000001", "rows": "4", "seen": []})");
    bool ok = true;
    EXPECT_NO_THROW(ok = a.FromJson(j, 1, 4));
    EXPECT_FALSE(ok);
}
