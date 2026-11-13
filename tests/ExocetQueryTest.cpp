#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/core/ArCommandInterpreter.h"
#include "../src/core/ArMemoryAccess.h"
#include "../src/core/ExocetKit.h"
#include "../src/core/ExocetPack.h"
#include "../src/core/ExocetQuery.h"
#include "../src/core/ExocetSession.h"
#include "../src/core/PrivateFeatures.h"
#include "../src/core/Subprocess.h"

using namespace adm::core;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// The exotest pack (tests/data/exocet) in a folder of its own - a session writes its scratch and
// activity files beside it - with the rows a check of its song would give: positions 0-4 of 64
// rows of 6 frames, so a key at pos 1 row 9 is played row 73, song frame 438.
class ExocetQueryTest : public ::testing::Test
{
protected:
    static constexpr int kRowsPerPos = 64, kPositions = 5, kTicks = 6;

    void SetUp() override
    {
        m_dir = fs::temp_directory_path() /
                ("adm_exocet_query_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(m_dir);
        fs::create_directories(m_dir);
        fs::path src;
        for (const char* p :
             {"tests/data/exocet/exotest.exocet", "../tests/data/exocet/exotest.exocet",
              "../../tests/data/exocet/exotest.exocet"})
            if (fs::exists(p))
                src = p;
        ASSERT_FALSE(src.empty()) << "exotest.exocet fixture not found";
        m_exocet = m_dir / "exotest.exocet";
        fs::copy_file(src, m_exocet);
    }

    void TearDown() override
    {
        m_session.reset();
        std::error_code ec;
        fs::remove_all(m_dir, ec);
    }

    // The session over the pack (with `disk`, a "disk" section, added to it first)
    void Open(const json& disk = nullptr)
    {
        {
            // exotest declares no director; the anim production's does, and the cuts need one
            std::ifstream in(m_exocet);
            auto doc = nlohmann::ordered_json::parse(in);
            in.close();
            doc["manifest"]["director"] = "director.cut";
            if (!disk.is_null())
                doc["disk"] = disk;
            std::ofstream(m_exocet) << doc.dump(2);
        }
        m_pack = std::make_shared<ExocetPack>();
        std::string error;
        ASSERT_TRUE(m_pack->Load(m_exocet.string(), &error)) << error;
        m_session = std::make_unique<ExocetSession>();
        m_session->SetActivityFile(m_dir / "activity.json");
        ExocetSession::Hooks hooks;
        hooks.block = [this]() { return &m_block; };
        hooks.breakAt = [this](int ahead) { m_breaks.push_back(ahead); };
        m_session->SetHooks(std::move(hooks));
        m_session->SetPack(m_pack, "", (m_dir / "exotest.adf").string());

        ExocetCheck check;
        ASSERT_TRUE(check.Parse(CheckReport()));
        m_session->AdoptCheckForTest(std::move(check));
    }

    // The report the kit's check of the song would give
    json CheckReport() const
    {
        json rows = json::array();
        for (int p = 0; p < kPositions; ++p)
            for (int r = 0; r < kRowsPerPos; ++r) {
                json row = {{"pos", p},
                            {"pattern", p},
                            {"row", r},
                            {"pass", 0},
                            {"frame", (p * kRowsPerPos + r) * kTicks},
                            {"frames", kTicks}};
                if (p == 0 && r == 0) // sample 1 played once, on the first row
                    row["cells"] = {{1, 428, 0, 0}, {0, 0, 0, 0}, {0, 0, 12, 32}, {0, 0, 0, 0}};
                rows.push_back(row);
            }
        return {{"command", "check"},
                {"issues", json::array()},
                {"music",
                 {{"frames", kPositions * kRowsPerPos * kTicks},
                  {"digest", "0x111f7093"},
                  {"order", m_pack->SongOrder(0)},
                  {"channels", 4}}},
                {"rows", rows},
                {"samples", {{{"index", 1}, {"name", "bass"}, {"bytes", 2336}, {"volume", 48}}}}};
    }

    // Update the session until `done`, or false after `seconds`
    template <class Done> bool UpdateUntil(Done done, double seconds)
    {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(static_cast<int>(seconds * 1000));
        while (std::chrono::steady_clock::now() < end) {
            m_session->Update();
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    // The block a running build of this pack publishes, on `row` (its song frame `tick` in)
    void Play(int row, int tick = 0)
    {
        m_block.present = true;
        m_block.blockAddress = 0x00201000;
        m_block.manifest = m_pack->ManifestHash();
        m_block.arena.valid = true;
        m_block.arena.song = 0x111F7093u;
        m_block.row = static_cast<uint16_t>(row);
        m_block.frame = static_cast<uint16_t>(row * kTicks + tick);
        m_block.values.assign(m_pack->Tracks().size(), 0);
        m_session->OnFrame(++m_emuFrame);
    }

    static int Row(int pos, int row) { return pos * kRowsPerPos + row; }

    fs::path m_dir, m_exocet;
    std::shared_ptr<ExocetPack> m_pack;
    std::unique_ptr<ExocetSession> m_session;
    ExocetBlockState m_block;
    std::vector<int> m_breaks;
    uint64_t m_emuFrame = 1000;
};

} // namespace

TEST(ExocetPlaceTest, ParsesTheGridsLabels)
{
    int song = 9, pos = 0, row = 0, pass = 0;
    ASSERT_TRUE(ExocetParsePlace("05:10", &song, &pos, &row, &pass));
    EXPECT_EQ(song, 9); // left alone when the label names none
    EXPECT_EQ(pos, 5);
    EXPECT_EQ(row, 10);
    EXPECT_EQ(pass, 0);
    ASSERT_TRUE(ExocetParsePlace("1 05:10.2", &song, &pos, &row, &pass));
    EXPECT_EQ(song, 1);
    EXPECT_EQ(pass, 2);
    ASSERT_TRUE(ExocetParsePlace("0:L2:03", &song, &pos, &row, &pass));
    EXPECT_EQ(song, 0);
    EXPECT_EQ(pos, -2);
    EXPECT_EQ(row, 3);
    for (const char* bad : {"5", "a:b", "05:", ":10", "L0:01", "1:2:3:4", "05:10.x"})
        EXPECT_FALSE(ExocetParsePlace(bad, &song, &pos, &row, &pass)) << bad;

    // and round trips the label
    ExocetRow r;
    r.song = 1;
    r.pos = -1;
    r.row = 7;
    r.pass = 3;
    ASSERT_TRUE(ExocetParsePlace(ExocetPlaceLabel(r, true), &song, &pos, &row, &pass));
    EXPECT_EQ(song, 1);
    EXPECT_EQ(pos, -1);
    EXPECT_EQ(row, 7);
    EXPECT_EQ(pass, 3);
}

TEST_F(ExocetQueryTest, ResolvesEveryFormOfPlace)
{
    Open();
    std::string error;
    EXPECT_EQ(ExocetResolvePlace(*m_session, {{"place", "01:09"}}, &error), Row(1, 9));
    EXPECT_EQ(ExocetResolvePlace(*m_session, {{"pos", 1}, {"row", 9}}, &error), Row(1, 9));
    EXPECT_EQ(ExocetResolvePlace(*m_session, {{"playedRow", 73}}, &error), 73);
    EXPECT_EQ(ExocetResolvePlace(*m_session, {{"frame", 73 * kTicks + 5}}, &error), 73);
    EXPECT_EQ(ExocetResolvePlace(*m_session, json::object(), &error, false), -2);
    EXPECT_EQ(ExocetResolvePlace(*m_session, {{"place", "09:00"}}, &error), -1);
    EXPECT_NE(error.find("does not play"), std::string::npos);
    EXPECT_EQ(ExocetResolvePlace(*m_session, {{"playedRow", 100000}}, &error), -1);
    const json p = ExocetPlaceJson(m_session->Check(), Row(1, 9));
    EXPECT_EQ(p["label"], "01:09");
    EXPECT_EQ(p["frame"], Row(1, 9) * kTicks);
}

TEST_F(ExocetQueryTest, TimelineWindowAndCuts)
{
    Open();
    const json r = ExocetTimelineJson(*m_session, {{"from", 70}, {"count", 5}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_EQ(r["rows"], kPositions * kRowsPerPos);
    EXPECT_EQ(r["fps"], 50);
    ASSERT_EQ(r["rowsShown"].size(), 5u);
    EXPECT_EQ(r["rowsShown"][0]["playedRow"], 70);
    // the director's key at 01:09 is "one"
    EXPECT_EQ(r["rowsShown"][3]["cut"], "one");
    EXPECT_EQ(r["songs"][0]["firstRow"], 0);
    EXPECT_EQ(r["songs"][0]["lastRow"], kPositions * kRowsPerPos - 1);
    EXPECT_TRUE(r["playhead"].is_null()); // nothing running

    // by default, around the playhead
    Play(Row(2, 0));
    m_session->Update();
    const json around = ExocetTimelineJson(*m_session, {{"count", 9}});
    EXPECT_EQ(around["window"]["from"], Row(2, 0) - 4);
    EXPECT_EQ(around["playhead"]["label"], "02:00");
}

TEST_F(ExocetQueryTest, GridValuesKeysAndFilters)
{
    Open();
    // a.table: 2 ("third") keyed at 01:19, stepping to 0 ("first") at 01:20
    json r =
        ExocetGridJson(*m_session, {{"tracks", {"a.table"}}, {"from", Row(1, 19)}, {"count", 2}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    ASSERT_EQ(r["tracks"].size(), 1u);
    EXPECT_EQ(r["tracks"][0]["entries"].size(), 3u);
    ASSERT_EQ(r["rows"].size(), 2u);
    EXPECT_EQ(r["rows"][0]["values"]["a.table"], 2);
    EXPECT_EQ(r["rows"][0]["names"]["a.table"], "third");
    EXPECT_EQ(r["rows"][0]["keys"]["a.table"][0]["interp"], "step");
    EXPECT_EQ(r["rows"][1]["names"]["a.table"], "first");

    // a colour reads as #RRGGBBAA, a fix as a real
    r = ExocetGridJson(*m_session,
                       {{"tracks", {"b.tint", "a.fix"}}, {"place", "01:03"}, {"count", 1}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_EQ(r["rows"][0]["values"]["b.tint"], "#00FF80FF");
    EXPECT_TRUE(r["rows"][0]["values"]["a.fix"].is_number_float());

    r = ExocetGridJson(*m_session, {{"groups", {"b"}}, {"count", 1}});
    EXPECT_EQ(r["tracks"].size(), 4u);
    r = ExocetGridJson(*m_session, {{"channels", true}, {"count", 1}, {"tracks", {"a.fix"}}});
    EXPECT_EQ(r["rows"][0]["channels"][0]["note"], "C-2");
    EXPECT_TRUE(ExocetGridJson(*m_session, {{"tracks", {"no.such"}}}).contains("error"));
}

TEST_F(ExocetQueryTest, KeysWithTheirPlaces)
{
    Open();
    const json r = ExocetKeysJson(*m_session, {{"track", "b.tint"}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    const json& k = r["keys"]["b.tint"][0];
    EXPECT_EQ(k["value"], "#00FF80FF");
    EXPECT_EQ(k["playedRow"], Row(1, 3));
    EXPECT_EQ(k["label"], "01:03");
    EXPECT_TRUE(ExocetKeysJson(*m_session, {{"track", "nope"}}).contains("error"));
}

TEST_F(ExocetQueryTest, BulkKeysAreOneUndoStepAndAllOrNothing)
{
    Open();
    const size_t before = m_pack->KeyCount();
    json r = ExocetSetKeysJson(
        *m_session,
        {{"keys",
          {{{"track", "a.level"}, {"place", "00:05"}, {"value", 100}},
           {{"track", "b.tint"}, {"place", "00:06"}, {"value", "#102030"}, {"interp", "linear"}},
           {{"track", "a.pick"}, {"place", "01:02"}, {"delete", true}}}}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_EQ(r["set"], 2);
    EXPECT_EQ(r["deleted"], 1);
    EXPECT_EQ(r["undoSteps"], 1);
    EXPECT_EQ(m_pack->KeyCount(), before + 1);
    EXPECT_FALSE(ExocetEditJson(*m_session, "undo").contains("error"));
    EXPECT_EQ(m_pack->KeyCount(), before);
    EXPECT_FALSE(ExocetEditJson(*m_session, "redo").contains("error"));
    EXPECT_EQ(m_pack->KeyCount(), before + 1);

    // one refused: nothing changes
    const size_t now = m_pack->KeyCount();
    const size_t steps = m_pack->UndoSteps();
    r = ExocetSetKeysJson(*m_session,
                          {{"keys",
                            {{{"track", "a.level"}, {"place", "00:07"}, {"value", 1}},
                             {{"track", "a.level"}, {"place", "00:08"}, {"value", 99999}}}}});
    ASSERT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("keys[1]"), std::string::npos);
    EXPECT_EQ(m_pack->KeyCount(), now);
    EXPECT_EQ(m_pack->UndoSteps(), steps);
}

TEST_F(ExocetQueryTest, SingleKeyTakesAnyPlace)
{
    Open();
    json r =
        ExocetSetKeyJson(*m_session, {{"track", "a.level"}, {"playedRow", 3}, {"value", 5}}, false);
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_EQ(r["playedRow"], 3);
    r = ExocetSetKeyJson(*m_session, {{"track", "a.level"}, {"place", "00:03"}}, true);
    EXPECT_EQ(r["deleted"], true);
    EXPECT_TRUE(ExocetEditJson(*m_session, "fly").contains("error"));
}

TEST_F(ExocetQueryTest, ClearTrackIsOneUndoStep)
{
    Open();
    const size_t before = m_pack->KeyCount();
    const size_t level = m_pack->Keys("a.level").size();
    json r = ExocetClearTrackJson(*m_session, {{"track", "a.level"}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_EQ(r["cleared"], level);
    EXPECT_EQ(r["keys"], before - level);
    EXPECT_EQ(r["dirty"], true);
    EXPECT_TRUE(m_pack->Keys("a.level").empty());
    EXPECT_EQ(ExocetClearTrackJson(*m_session, {{"track", "a.level"}})["cleared"], 0);

    EXPECT_FALSE(ExocetEditJson(*m_session, "undo").contains("error"));
    EXPECT_EQ(m_pack->Keys("a.level").size(), level);
    EXPECT_EQ(m_pack->KeyCount(), before);

    EXPECT_TRUE(ExocetClearTrackJson(*m_session, {{"track", "nope"}}).contains("error"));
    EXPECT_TRUE(ExocetClearTrackJson(*m_session, json::object()).contains("error"));
}

TEST_F(ExocetQueryTest, RowAndKeyBreakpoints)
{
    Open();
    json r =
        ExocetBreakpointsJson(*m_session, {{"action", "add"}, {"kind", "row"}, {"place", "01:09"}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_EQ(r["rows"][0]["label"], "01:09");

    // the frame before the row: warned one frame ahead; the row itself: stopped
    Play(Row(1, 8), kTicks - 1);
    EXPECT_EQ(m_breaks, (std::vector<int>{1}));
    Play(Row(1, 9));
    EXPECT_EQ(m_breaks, (std::vector<int>{1, 0}));
    EXPECT_EQ(m_session->LastBreak().kind, "row");
    // staying on the row does not stop again
    Play(Row(1, 9), 1);
    EXPECT_EQ(m_breaks.size(), 2u);

    // a key break stops on a.pick's keyed rows (01:02), not on the others
    ExocetBreakpointsJson(*m_session, {{"action", "clear"}});
    r = ExocetBreakpointsJson(*m_session,
                              {{"action", "add"}, {"kind", "key"}, {"track", "a.pick"}});
    EXPECT_EQ(r["keys"][0], "a.pick");
    m_breaks.clear();
    Play(Row(1, 1));
    Play(Row(1, 2));
    EXPECT_EQ(m_breaks.back(), 0);
    r = ExocetBreakpointsJson(*m_session, json::object());
    EXPECT_EQ(r["lastBreak"]["kind"], "key");
    EXPECT_EQ(r["lastBreak"]["what"], "a.pick");
    EXPECT_EQ(r["lastBreak"]["place"]["label"], "01:02");

    // another build's block stops nothing
    m_breaks.clear();
    m_block.manifest ^= 1;
    m_session->OnFrame(++m_emuFrame);
    Play(Row(1, 3));
    m_block.manifest ^= 1;
    EXPECT_TRUE(ExocetBreakpointsJson(*m_session,
                                      {{"action", "add"}, {"kind", "event"}, {"track", "a.fix"}})
                    .contains("error")); // not an event track
}

TEST_F(ExocetQueryTest, BookmarksTakeSlotsAndArePlaces)
{
    Open();
    json r = ExocetBookmarksJson(*m_session, {{"action", "add"}, {"place", "01:09"}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    r = ExocetBookmarksJson(*m_session, {{"action", "add"}, {"place", "00:04"}, {"name", "drop"}});
    ASSERT_EQ(r["count"], 2);
    // song order; a new one takes the lowest free slot
    EXPECT_EQ(r["bookmarks"][0]["label"], "00:04");
    EXPECT_EQ(r["bookmarks"][0]["slot"], 2);
    EXPECT_EQ(r["bookmarks"][0]["name"], "drop");
    EXPECT_EQ(r["bookmarks"][1]["slot"], 1);
    EXPECT_TRUE(r["dirty"]);

    // a bookmark is a place anywhere one is taken: by slot or by name
    std::string error;
    EXPECT_EQ(ExocetResolvePlace(*m_session, {{"bookmark", 1}}, &error), Row(1, 9));
    EXPECT_EQ(ExocetResolvePlace(*m_session, {{"bookmark", "drop"}}, &error), Row(0, 4));
    EXPECT_EQ(ExocetResolvePlace(*m_session, {{"bookmark", 7}}, &error), -1);
    EXPECT_FALSE(error.empty());

    // the session by played row: the grid's lookups
    EXPECT_EQ(m_session->BookmarkSlotRow(2), Row(0, 4));
    EXPECT_EQ(m_session->FreeBookmarkSlot(), 3);
    ASSERT_TRUE(m_session->ToggleBookmark(Row(2, 0)));
    EXPECT_EQ(m_session->BookmarkAt(Row(2, 0))->slot, 3);
    ASSERT_TRUE(m_session->ToggleBookmark(Row(2, 0)));
    EXPECT_EQ(m_session->BookmarkAt(Row(2, 0)), nullptr);

    // the rows carry them
    r = ExocetGridJson(*m_session, {{"from", "00:04"}, {"count", 1}});
    EXPECT_EQ(r["rows"][0]["bookmark"]["name"], "drop");

    // an existing one keeps its slot unless given one; removed by its bookmark
    r = ExocetBookmarksJson(*m_session, {{"action", "add"}, {"bookmark", "drop"}, {"slot", 0}});
    EXPECT_TRUE(r["bookmarks"][0]["slot"].is_null());
    r = ExocetBookmarksJson(*m_session, {{"action", "remove"}, {"bookmark", 1}});
    EXPECT_EQ(r["count"], 1);
    EXPECT_TRUE(ExocetBookmarksJson(*m_session, {{"action", "remove"}, {"place", "01:09"}})
                    .contains("error"));
    r = ExocetBookmarksJson(*m_session, {{"action", "clear"}});
    EXPECT_EQ(r["count"], 0);
    ASSERT_TRUE(m_session->Undo());
    EXPECT_EQ(ExocetBookmarksJson(*m_session, json::object())["count"], 1);
}

TEST_F(ExocetQueryTest, EventAndCutBreakpoints)
{
    Open();
    const int hit = m_pack->FindTrack("b.hit")->index;
    // b.hit fires 7 at 01:11; a break on -3 does not take it, one on 7 does
    ExocetBreakpointsJson(
        *m_session, {{"action", "add"}, {"kind", "event"}, {"track", "b.hit"}, {"value", -3}});
    Play(Row(1, 10));
    m_block.firedCount = 1;
    m_block.fired = {{static_cast<uint16_t>(hit), static_cast<uint16_t>(Row(1, 11)), 0}};
    Play(Row(1, 11));
    EXPECT_TRUE(m_breaks.empty());
    ExocetBreakpointsJson(*m_session,
                          {{"action", "add"}, {"kind", "event"}, {"track", "b.hit"}, {"value", 7}});
    m_block.firedCount = 2;
    m_block.fired.push_back({static_cast<uint16_t>(hit), static_cast<uint16_t>(Row(1, 11)), 1});
    Play(Row(1, 11), 1);
    EXPECT_EQ(m_breaks, (std::vector<int>{0}));
    EXPECT_EQ(m_session->LastBreak().kind, "event");

    // a cut: ready, then made; a break set after it was made waits for the next one
    ExocetBreakpointsJson(*m_session, {{"action", "clear"}});
    ExocetBreakpointsJson(*m_session,
                          {{"action", "add"}, {"kind", "cut"}, {"cut", "one"}, {"ready", true}});
    m_breaks.clear();
    m_block.cuts = {{1, static_cast<uint16_t>(Row(1, 5)), 400, 0xFFFF, 0xFFFF}};
    Play(Row(1, 5));
    EXPECT_EQ(m_breaks, (std::vector<int>{0}));
    EXPECT_EQ(m_session->LastBreak().kind, "cut ready");
    EXPECT_EQ(m_session->LastBreak().what, "one");
    ExocetBreakpointsJson(*m_session, {{"action", "add"}, {"kind", "cut"}}); // any, made
    m_block.cuts[0].cutRow = static_cast<uint16_t>(Row(1, 9));
    m_block.cuts[0].cutFrame = 438;
    Play(Row(1, 9));
    EXPECT_EQ(m_breaks, (std::vector<int>{0, 0}));
    EXPECT_EQ(m_session->LastBreak().kind, "cut");
    Play(Row(1, 10)); // the same cut still in the record: not again
    EXPECT_EQ(m_breaks.size(), 2u);
    const json r = ExocetBreakpointsJson(*m_session, json::object());
    EXPECT_EQ(r["cuts"].size(), 2u);
    EXPECT_EQ(r["count"], 2);
}

// EXO-04: breaks are kept by track name: another pack with fewer tracks lists none of them,
// and they come back with the pack that has the tracks
TEST_F(ExocetQueryTest, BreakpointsFollowTrackNamesAcrossPacks)
{
    Open();
    const std::string last = m_pack->Tracks().back().name;
    ASSERT_GT(m_pack->FindTrack("b.hit")->index, 0);
    ASSERT_FALSE(
        ExocetBreakpointsJson(*m_session, {{"action", "add"}, {"kind", "key"}, {"track", last}})
            .contains("error"));
    ASSERT_FALSE(ExocetBreakpointsJson(*m_session,
                                       {{"action", "add"}, {"kind", "event"}, {"track", "b.hit"}})
                     .contains("error"));

    // a pack with only the first track
    const fs::path small = m_dir / "small.exocet";
    {
        std::ifstream in(m_exocet);
        auto doc = nlohmann::ordered_json::parse(in);
        auto& tracks = doc["manifest"]["tracks"];
        tracks.erase(tracks.begin() + 1, tracks.end());
        std::ofstream(small) << doc.dump(2);
    }
    auto other = std::make_shared<ExocetPack>();
    std::string error;
    ASSERT_TRUE(other->Load(small.string(), &error)) << error;
    m_session->SetPack(other, "", (m_dir / "small.adf").string());
    json r;
    ASSERT_NO_THROW(r = ExocetBreakpointsJson(*m_session, {{"action", "list"}}));
    EXPECT_TRUE(r["keys"].empty()) << r.dump();
    EXPECT_TRUE(r["events"].empty()) << r.dump();

    m_session->SetPack(m_pack, "", (m_dir / "exotest.adf").string());
    r = ExocetBreakpointsJson(*m_session, {{"action", "list"}});
    ASSERT_EQ(r["keys"].size(), 1u);
    EXPECT_EQ(r["keys"][0], last);
    ASSERT_EQ(r["events"].size(), 1u);
    EXPECT_EQ(r["events"][0]["track"], "b.hit");
}

TEST_F(ExocetQueryTest, SeeksOnlyToRecordedFrames)
{
    Open();
    uint64_t went = 0;
    auto seek = [&](uint64_t f) {
        went = f;
        return std::string();
    };
    json r = ExocetSeekJson(*m_session, {{"place", "01:09"}}, seek);
    ASSERT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("row breakpoint"), std::string::npos);
    Play(Row(1, 9));
    const uint64_t machine = m_emuFrame;
    r = ExocetSeekJson(*m_session, {{"place", "01:09"}}, seek);
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_EQ(went, machine);
    EXPECT_EQ(r["songFrame"], Row(1, 9) * kTicks);
    EXPECT_TRUE(ExocetSeekJson(*m_session, {{"place", "01:09"}}, nullptr).contains("error"));
}

TEST_F(ExocetQueryTest, MusicHits)
{
    Open();
    const json r = ExocetMusicJson(*m_session, {{"sample", 1}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    ASSERT_EQ(r["samples"].size(), 1u);
    EXPECT_EQ(r["samples"][0]["hits"], 1);
    EXPECT_EQ(r["samples"][0]["hitRows"][0]["label"], "00:00");
    EXPECT_TRUE(ExocetMusicJson(*m_session, {{"sample", 9}}).contains("error"));
}

TEST_F(ExocetQueryTest, DiskMapFromThePack)
{
    Open();
    json r = ExocetDiskMapJson(*m_session, json::object());
    EXPECT_FALSE(r["ok"].get<bool>());
    EXPECT_EQ(r["status"], "The .exocet has no \"disk\" map.");

    const json spec = json::parse(R"({
      "name": "XCET", "sector_bytes": 512, "sectors": 1760, "sectors_per_track": 11,
      "regions": [{"name": "bootblock", "first": 0, "count": 2}],
      "directory": {
        "sector": 2, "sectors": 1, "magic": "XCET", "version": 14, "count": "blocks",
        "header": [{"name": "magic", "type": "char4"}, {"name": "version", "type": "u16"},
                   {"name": "blocks", "type": "u16", "unit": "count"}],
        "entry": [{"name": "sector", "type": "u32", "role": "sector"},
                  {"name": "packed", "type": "u32", "role": "packed"},
                  {"name": "unpacked", "type": "u32", "role": "unpacked"},
                  {"name": "kind", "type": "u16", "role": "kind"},
                  {"name": "sectors", "type": "u16", "role": "sectors"}]},
      "kinds": [{"value": 1, "name": "anim"}, {"value": 7, "name": "music"}]
    })");
    std::vector<uint8_t> adf(1760 * 512, 0);
    const size_t dir = 2 * 512;
    std::memcpy(&adf[dir], "XCET", 4);
    adf[dir + 5] = 14;
    adf[dir + 7] = 2; // two blocks
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            adf[at + size_t(i)] = uint8_t(v >> (24 - 8 * i));
    };
    put32(dir + 8, 3);     // sector
    put32(dir + 12, 1000); // packed
    put32(dir + 16, 3000); // unpacked
    adf[dir + 21] = 1;     // anim
    adf[dir + 23] = 2;     // sectors
    put32(dir + 24, 5);    // sector
    put32(dir + 28, 512);  // packed
    put32(dir + 32, 512);  // unpacked
    adf[dir + 37] = 7;     // music
    adf[dir + 39] = 1;     // sectors
    std::ofstream(m_dir / "exotest.adf", std::ios::binary)
        .write(reinterpret_cast<const char*>(adf.data()), static_cast<std::streamsize>(adf.size()));
    m_session.reset();
    Open(spec);
    r = ExocetDiskMapJson(*m_session, json::object());
    ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
    EXPECT_EQ(r["entries"].size(), 2u);
    EXPECT_EQ(r["usedEnd"], 6);
    EXPECT_EQ(r["entries"][0]["cylHead"], "0.0");
    r = ExocetDiskMapJson(*m_session, {{"kind", "music"}, {"entries", true}});
    ASSERT_EQ(r["entries"].size(), 1u);
    EXPECT_EQ(r["entries"][0]["sector"], 5);
    EXPECT_FALSE(ExocetDiskMapJson(*m_session, {{"entries", false}}).contains("entries"));
}

TEST_F(ExocetQueryTest, ConsoleExoCommands)
{
    Open();
    // EXO is only a command with private features on (core/PrivateFeatures.h)
    struct PrivateOn
    {
        PrivateOn() { adm::core::SetPrivateFeatures(true); }
        ~PrivateOn() { adm::core::SetPrivateFeatures(false); }
    } privateOn;
    ArMemoryAccess mem;
    ArHost host;
    host.exocetSession = [this]() { return m_session.get(); };
    ArCommandInterpreter interp(&mem, std::move(host));
    std::vector<std::string> out;
    interp.emitLine = [&out](const ArLine& l) { out.push_back(l.text); };
    auto run = [&](const std::string& line) {
        out.clear();
        interp.ExecuteLine(line);
        std::string all;
        for (const auto& l : out)
            all += l + "\n";
        return all;
    };
    EXPECT_NE(run("EXO GRID 01:19 2 a.table").find("third"), std::string::npos);
    EXPECT_NE(run("EXO KEY a.level 00:05 100").find("Keyed a.level"), std::string::npos);
    EXPECT_NE(run("EXO KEYS a.level").find("00:05"), std::string::npos);
    EXPECT_NE(run("EXO UNDO").find("Undone"), std::string::npos);
    EXPECT_NE(run("EXO CLEAR a.pick").find("Cleared 4 key(s) from a.pick"), std::string::npos);
    EXPECT_NE(run("EXO UNDO").find("Undone"), std::string::npos);
    EXPECT_NE(run("EXO CLEAR").find("usage"), std::string::npos);
    EXPECT_NE(run("EXO BR ROW 01:09").find("01:09"), std::string::npos);
    EXPECT_NE(run("EXO BR CUT one READY").find("one ready"), std::string::npos);
    EXPECT_NE(run("EXO BD ROW 01:09").find("one ready"), std::string::npos);
    EXPECT_NE(run("EXO BR CLEAR").find("No Exocet breakpoints"), std::string::npos);
    EXPECT_NE(run("EXO TL #73 3").find("01:09"), std::string::npos);
    EXPECT_NE(run("EXO MUSIC 1").find("bass"), std::string::npos);
    EXPECT_NE(run("EXO BM 01:09 S4 the drop").find("4  01:09        the drop"), std::string::npos)
        << run("EXO BM");
    EXPECT_NE(run("EXO TL @4 1").find("01:09"), std::string::npos);
    EXPECT_NE(run("EXO BMD @the").find("Error"), std::string::npos); // a name is one word here
    EXPECT_NE(run("EXO BMD @4").find("No bookmarks"), std::string::npos);
    EXPECT_NE(run("EXO BM CLEAR").find("No bookmarks"), std::string::npos);
    // no arguments at all: every subcommand that reads takes its defaults
    for (const char* bare : {"EXO", "EXO TL", "EXO GRID", "EXO KEYS", "EXO MUSIC", "EXO DISK",
                             "EXO MEM", "EXO BR", "EXO BM", "EXO PROOF", "EXO ISSUES"})
        EXPECT_EQ(run(bare).find("Error"), std::string::npos) << bare << ": " << run(bare);
    EXPECT_NE(run("EXO KEY a.level").find("usage"), std::string::npos);
    EXPECT_NE(run("EXO BOGUS").find("unknown EXO"), std::string::npos);
    EXPECT_NE(run("EXO SEEK 01:09").find("Error"), std::string::npos); // no frame history
}

// Hidden, EXO is not a command at all, and help does not list it
TEST_F(ExocetQueryTest, ConsoleExoIsUnknownWhilePrivateFeaturesAreOff)
{
    Open();
    ASSERT_FALSE(adm::core::PrivateFeatures());
    ArMemoryAccess mem;
    ArHost host;
    host.exocetSession = [this]() { return m_session.get(); };
    ArCommandInterpreter interp(&mem, std::move(host));
    std::string all;
    interp.emitLine = [&all](const ArLine& l) { all += l.text + "\n"; };
    interp.ExecuteLine("EXO TL");
    EXPECT_NE(all.find("unknown command"), std::string::npos) << all;
    all.clear();
    interp.ExecuteLine("HELP");
    EXPECT_EQ(all.find("EXO"), std::string::npos) << "help lists EXO";
}

// What the grid marks: the keys added, changed and removed since the .exocet was last saved, each
// by the cell - (played row, sub) - it is in
TEST_F(ExocetQueryTest, ChangesSinceTheSave)
{
    using KC = ExocetSession::KeyChange;
    Open();
    const int level = m_pack->FindTrack("a.level")->index;
    EXPECT_TRUE(m_session->Changes(level).empty());
    EXPECT_EQ(m_session->ChangeCounts(), (std::array<int, 3>{0, 0, 0}));

    // a new key at 02:00, the one at 01:00 (32767, linear) changed, the one at 00:00 removed
    std::string error;
    ASSERT_TRUE(m_session->SetKey("a.level", Row(2, 0), 5, "step", &error)) << error;
    ASSERT_TRUE(m_session->SetKey("a.level", Row(1, 0), 100, "linear", &error)) << error;
    ASSERT_TRUE(m_session->DeleteKey("a.level", Row(0, 0)));
    {
        const auto& c = m_session->Changes(level);
        ASSERT_EQ(c.size(), 3u);
        EXPECT_EQ(c.at({Row(2, 0), 0}).kind, KC::Added);
        EXPECT_EQ(c.at({Row(1, 0), 0}).kind, KC::Changed);
        EXPECT_EQ(c.at({Row(1, 0), 0}).was, 32767);
        EXPECT_EQ(c.at({Row(0, 0), 0}).kind, KC::Removed);
        EXPECT_EQ(c.at({Row(0, 0), 0}).was, -32768);
        EXPECT_EQ(c.at({Row(0, 0), 0}).wasInterp, "linear");
    }
    EXPECT_EQ(m_session->ChangeCounts(), (std::array<int, 3>{1, 1, 1}));
    EXPECT_TRUE(m_session->Changes(m_pack->FindTrack("a.fix")->index).empty());

    // the interpolation alone is a change; set back as saved, the key is not
    ASSERT_TRUE(m_session->SetKey("a.level", Row(1, 0), 32767, "smooth", &error)) << error;
    EXPECT_EQ(m_session->Changes(level).at({Row(1, 0), 0}).kind, KC::Changed);
    ASSERT_TRUE(m_session->SetKey("a.level", Row(1, 0), 32767, "linear", &error)) << error;
    EXPECT_EQ(m_session->Changes(level).count({Row(1, 0), 0}), 0u);

    // undone back to the save, nothing; redone and saved, nothing
    while (m_session->CanUndo())
        ASSERT_TRUE(m_session->Undo());
    EXPECT_TRUE(m_session->Changes(level).empty());
    while (m_session->CanRedo())
        ASSERT_TRUE(m_session->Redo());
    EXPECT_EQ(m_session->ChangeCounts(), (std::array<int, 3>{1, 0, 1}));
    ASSERT_TRUE(m_session->Save(&error)) << error;
    EXPECT_TRUE(m_session->Changes(level).empty());
}

// The kit's check exports the music, so a key edit runs none (the push's pack checks the keys),
// and what a failed push said stops neither the next push nor, once the keys change, anything.
TEST_F(ExocetQueryTest, AKeyEditRunsNoCheckAndAFailedPushStopsNothing)
{
    Subprocess probe;
    if (probe.Run({"python", "-c", "print(1)"}, [](const std::string&) {}) != 0)
        GTEST_SKIP() << "no python on PATH";
    // a kit whose check gives the song's rows and whose other roles fail on the keys; each run
    // is a line in runs.txt
    const fs::path kit = m_dir / "kit";
    fs::create_directories(kit);
    std::ofstream(kit / "kit.json")
        << R"({"kit": 1, "production": "test", "commands": {"check": ["kit.py", "check"],
               "export": ["kit.py", "export"], "pack": ["kit.py", "pack"]}})";
    std::ofstream(kit / "kit.py") << R"(import json, os, shutil, sys
role, report = sys.argv[1], sys.argv[-1]
here = os.path.dirname(os.path.abspath(__file__))
open(os.path.join(here, "runs.txt"), "a").write(role + "\n")
if role == "check":
    shutil.copy(os.path.join(here, "check.json"), report)
    sys.exit(0)
json.dump({"issues": [{"level": "block", "code": "exocet.duplicate", "title": "Two keys",
                       "text": "on one tick", "blocks": ["push", "write", "inject"]}]},
          open(report, "w"))
sys.exit(1)
)";
    {
        std::ifstream in(m_exocet);
        auto doc = nlohmann::ordered_json::parse(in);
        in.close();
        doc["kit"] = "kit/kit.json";
        std::ofstream(m_exocet) << doc.dump(2);
    }
    Open();
    std::ofstream(kit / "check.json") << CheckReport().dump();
    const auto runs = [&kit](const std::string& role) {
        std::ifstream in(kit / "runs.txt");
        int n = 0;
        for (std::string line; std::getline(in, line);)
            n += line == role;
        return n;
    };
    m_session->SetTools({"python", ""});
    m_session->SetTrustStore([](const std::string&) { return false; }, [](const std::string&) {});
    m_session->TrustKit();
    ASSERT_EQ(m_session->GetKitState(), ExocetSession::KitState::Ready);
    ASSERT_TRUE(UpdateUntil([&] { return runs("check") == 1 && !m_session->Busy(); }, 30));
    ASSERT_TRUE(m_session->Check().valid);

    // a key: no check, past the debounce
    json r = ExocetSetKeyJson(*m_session, {{"track", "a.level"}, {"place", "02:00"}, {"value", 5}},
                              false);
    ASSERT_FALSE(r.contains("error")) << r.dump();
    UpdateUntil([] { return false; }, 1.5);
    EXPECT_EQ(runs("check"), 1);
    EXPECT_FALSE(m_session->Busy());

    // the music section: one check, and only one
    ASSERT_TRUE(m_pack->SetAutoDb(m_pack->AutoDb() + 1));
    ASSERT_TRUE(UpdateUntil([&] { return runs("check") == 2 && !m_session->Busy(); }, 30));
    UpdateUntil([] { return false; }, 1.5);
    EXPECT_EQ(runs("check"), 2);

    // a push the kit refuses over the keys
    Play(Row(2, 0));
    m_session->Update();
    ASSERT_EQ(m_session->PushBlockedReason(), "");
    ASSERT_TRUE(m_session->Push());
    ASSERT_TRUE(UpdateUntil([&] { return !m_session->Busy(); }, 30));
    EXPECT_EQ(m_session->LastPush().rfind("Push failed", 0), 0u) << m_session->LastPush();
    const auto has = [this](const std::string& code) {
        for (const auto& i : m_session->Issues())
            if (i.code == code)
                return true;
        return false;
    };
    EXPECT_TRUE(has("exocet.duplicate"));
    EXPECT_EQ(m_session->PushBlockedReason(), ""); // pushing again is how to find out again
    EXPECT_EQ(m_session->BlockedBy("write"), "Two keys");

    // the keys edited: what the push said about them is of keys that are not there now
    r = ExocetSetKeyJson(*m_session, {{"track", "a.level"}, {"place", "02:01"}, {"value", 6}},
                         false);
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_FALSE(has("exocet.duplicate"));
    EXPECT_EQ(m_session->BlockedBy("write"), "");
}

// EXO-05: a check that finishes after another pack became active is not adopted by that pack
TEST_F(ExocetQueryTest, AJobForAPackNoLongerActiveIsDropped)
{
    Subprocess probe;
    if (probe.Run({"python", "-c", "print(1)"}, [](const std::string&) {}) != 0)
        GTEST_SKIP() << "no python on PATH";
    const fs::path kit = m_dir / "kit";
    fs::create_directories(kit);
    std::ofstream(kit / "kit.json")
        << R"({"kit": 1, "production": "test", "commands": {"check": ["kit.py", "check"]}})";
    std::ofstream(kit / "kit.py") << R"(import os, shutil, sys, time
here = os.path.dirname(os.path.abspath(__file__))
time.sleep(1.5)
shutil.copy(os.path.join(here, "check.json"), sys.argv[-1])
)";
    // the other pack: the same song, no kit (so it starts no check of its own)
    const fs::path other = m_dir / "other.exocet";
    fs::copy_file(m_exocet, other);
    {
        std::ifstream in(m_exocet);
        auto doc = nlohmann::ordered_json::parse(in);
        in.close();
        doc["kit"] = "kit/kit.json";
        std::ofstream(m_exocet) << doc.dump(2);
    }
    Open();
    std::ofstream(kit / "check.json") << CheckReport().dump();
    m_session->SetTools({"python", ""});
    m_session->SetTrustStore([](const std::string&) { return false; }, [](const std::string&) {});
    m_session->TrustKit();
    ASSERT_TRUE(UpdateUntil([&] { return m_session->Busy(); }, 10));

    auto pack = std::make_shared<ExocetPack>();
    std::string error;
    ASSERT_TRUE(pack->Load(other.string(), &error)) << error;
    m_session->SetPack(pack, "", (m_dir / "other.adf").string());
    ASSERT_TRUE(UpdateUntil([&] { return !m_session->Busy(); }, 30));
    EXPECT_FALSE(m_session->Check().valid); // the old pack's rows are not this one's
}

// EXO-07: a key delete keeps SetKey's rules, so it is never a step that cannot be undone
TEST_F(ExocetQueryTest, DeleteKeyIsRefusedWhileEditingIs)
{
    Open();
    json report = CheckReport();
    report["music"]["order"] = {4, 3, 2, 1, 0}; // the kit has moved the song on
    ExocetCheck check;
    ASSERT_TRUE(check.Parse(report));
    m_session->AdoptCheckForTest(std::move(check));
    ASSERT_FALSE(m_session->EditBlockedReason().empty());

    const size_t keys = m_pack->Keys("a.level").size();
    ASSERT_GT(keys, 0u);
    std::string error;
    EXPECT_FALSE(m_session->DeleteKey("a.level", Row(0, 0), 0, {}, &error));
    EXPECT_EQ(error, m_session->EditBlockedReason());
    EXPECT_EQ(m_pack->Keys("a.level").size(), keys);
    const json r =
        ExocetSetKeyJson(*m_session, {{"track", "a.level"}, {"place", "00:00"}}, /*remove*/ true);
    EXPECT_TRUE(r.contains("error")) << r.dump();
}

// EXO-06: at the undo cap the step count stops growing; the rollback must still take the
// keys back whole, and leave nothing to redo
TEST_F(ExocetQueryTest, BulkKeysRollBackAtTheUndoCap)
{
    Open();
    for (int i = 0; i < 510; ++i) {
        m_pack->BreakMerge();
        const json r = ExocetSetKeyJson(
            *m_session, {{"track", "a.level"}, {"place", "00:05"}, {"value", i}}, false);
        ASSERT_FALSE(r.contains("error")) << r.dump();
    }
    ASSERT_EQ(m_pack->UndoSteps(), 500u);
    const std::string doc = m_pack->Dump();
    const json r = ExocetSetKeysJson(
        *m_session, {{"keys",
                      {{{"track", "a.level"}, {"place", "00:07"}, {"value", 1}},
                       {{"track", "a.level"}, {"place", "00:08"}, {"value", 99999}}}}});
    ASSERT_TRUE(r.contains("error"));
    EXPECT_EQ(m_pack->Dump(), doc);
    EXPECT_FALSE(m_pack->CanRedo());
    // the bulk's own step pushed the oldest off the end of the history; taken back, it is one
    // fewer
    EXPECT_EQ(m_pack->UndoSteps(), 499u);
}

TEST_F(ExocetQueryTest, BulkKeysRollBackOnMalformedEntry)
{
    Open();
    const std::string doc = m_pack->Dump();
    const size_t steps = m_pack->UndoSteps();
    json r;
    ASSERT_NO_THROW(
        r = ExocetSetKeysJson(
            *m_session, {{"keys", {{{"track", "a.level"}, {"place", "00:07"}, {"value", 1}}, 5}}}));
    ASSERT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("keys[1]"), std::string::npos);
    ASSERT_NO_THROW(
        r = ExocetSetKeysJson(*m_session,
                              {{"keys",
                                {{{"track", "a.level"}, {"place", "00:07"}, {"value", 1}},
                                 {{"track", "a.level"}, {"place", "00:08"}, {"delete", "yes"}}}}}));
    ASSERT_TRUE(r.contains("error"));
    EXPECT_EQ(m_pack->Dump(), doc);
    EXPECT_EQ(m_pack->UndoSteps(), steps);
    EXPECT_FALSE(m_pack->CanRedo());
}

// EXO-09: the divergence counts kept as frames are recorded equal a full recompute, including
// a frame recorded again with other values; and how long a proofing pass takes is printed
TEST_F(ExocetQueryTest, DivergenceKeptAsRecordedEqualsAFullRecompute)
{
    Open();
    const int frames = kPositions * kRowsPerPos * kTicks;
    const auto record = [&](int frame, int32_t value) {
        m_block.present = true;
        m_block.blockAddress = 0x00201000;
        m_block.manifest = m_pack->ManifestHash();
        m_block.arena.valid = true;
        m_block.arena.song = 0x111F7093u;
        m_block.enabled = 0xFFFFFFFFu;
        m_block.row = static_cast<uint16_t>(frame / kTicks);
        m_block.frac = static_cast<uint16_t>((frame % kTicks) * 65536 / kTicks);
        m_block.frame = static_cast<uint16_t>(frame);
        m_block.values.assign(m_pack->Tracks().size(), value);
        m_session->OnFrame(++m_emuFrame);
    };
    for (int f = 0; f < frames / 2; ++f)
        record(f, 0);
    m_session->Update(); // the session's own copy of the block: this build's
    const auto first = m_session->Divergence();
    ASSERT_FALSE(first.empty());
    for (int f = frames / 2; f < frames; ++f)
        record(f, 0);
    for (int f = 0; f < 50; ++f) // the start again, other values
        record(f, 7);

    const auto t0 = std::chrono::steady_clock::now();
    std::map<int, int> kept;
    for (int i = 0; i < 20; ++i)
        kept = m_session->Divergence();
    const auto t1 = std::chrono::steady_clock::now();

    // a revision moved on and back: the next Divergence is a recompute from the record
    std::string error;
    ASSERT_TRUE(m_session->SetKey("a.level", Row(2, 0), 5, "step", &error)) << error;
    ASSERT_TRUE(m_session->Undo());
    const auto t2 = std::chrono::steady_clock::now();
    const auto full = m_session->Divergence();
    const auto t3 = std::chrono::steady_clock::now();
    EXPECT_EQ(kept, full);
    std::printf("[ timing ] %d frames: Divergence() kept %.3f ms/call, full %.3f ms\n", frames,
                std::chrono::duration<double, std::milli>(t1 - t0).count() / 20,
                std::chrono::duration<double, std::milli>(t3 - t2).count());
}

// EXO-08: closing while a kit command hangs stops it, rather than waiting out its timeout
TEST_F(ExocetQueryTest, ClosingStopsAHungKitCommand)
{
    Subprocess probe;
    if (probe.Run({"python", "-c", "print(1)"}, [](const std::string&) {}) != 0)
        GTEST_SKIP() << "no python on PATH";
    const fs::path kit = m_dir / "kit";
    fs::create_directories(kit);
    std::ofstream(kit / "kit.json")
        << R"({"kit": 1, "production": "test", "commands": {"check": ["kit.py", "check"]}})";
    std::ofstream(kit / "kit.py") << "import time\ntime.sleep(300)\n";
    {
        std::ifstream in(m_exocet);
        auto doc = nlohmann::ordered_json::parse(in);
        in.close();
        doc["kit"] = "kit/kit.json";
        std::ofstream(m_exocet) << doc.dump(2);
    }
    Open();
    m_session->SetTools({"python", ""});
    m_session->SetTrustStore([](const std::string&) { return false; }, [](const std::string&) {});
    m_session->TrustKit();
    ASSERT_TRUE(UpdateUntil([&] { return m_session->Busy(); }, 10));
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // the command under way
    const auto t0 = std::chrono::steady_clock::now();
    m_session.reset();
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), 5.0);
}
