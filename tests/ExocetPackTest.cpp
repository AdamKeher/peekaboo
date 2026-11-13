#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../src/core/ExocetPack.h"

using adm::core::ExocetPack;

namespace fs = std::filesystem;

namespace {

// The fixtures are .exocet files as the crass repo's tools/exocet.py wrote them: the anim
// production's sync file and the exotest one (which has fix tracks and every interp).
std::string Fixture(const char* name)
{
    for (const char* dir : {"tests/data/exocet/", "../tests/data/exocet/",
                            "../../tests/data/exocet/", "../../../tests/data/exocet/"}) {
        const std::string p = std::string(dir) + name;
        if (fs::exists(p))
            return p;
    }
    return {};
}

std::string ReadText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    // a checkout may have given the fixture CRLF; Python writes LF
    std::string out;
    for (char c : s)
        if (c != '\r')
            out += c;
    return out;
}

void WriteText(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

class TempDir
{
public:
    TempDir()
    {
        m_path = fs::temp_directory_path() /
                 ("adm_exocet_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(m_path);
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    const fs::path& Path() const { return m_path; }

private:
    fs::path m_path;
};

} // namespace

TEST(ExocetPackTest, DumpIsByteForByteWhatExocetPyWrites)
{
    for (const char* name : {"cra-sneezeman.exocet", "exotest.exocet"}) {
        const std::string path = Fixture(name);
        ASSERT_FALSE(path.empty()) << name;
        ExocetPack pack;
        std::string error;
        ASSERT_TRUE(pack.Load(path, &error)) << error;
        EXPECT_EQ(pack.Dump(), ReadText(path)) << name;
    }
}

TEST(ExocetPackTest, IndexesTheManifest)
{
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(Fixture("cra-sneezeman.exocet")));
    EXPECT_EQ(pack.Production(), "anim");
    EXPECT_EQ(pack.ManifestHash(), 0x09515221u);
    EXPECT_EQ(pack.SongDigest(0), 0x111F7093u);
    const auto order = pack.SongOrder(0);
    ASSERT_EQ(order.size(), 58u);
    EXPECT_EQ(order[11], 4);

    ASSERT_EQ(pack.Tracks().size(), 13u);
    EXPECT_EQ(pack.Tracks()[0].name, "av.speed");
    EXPECT_EQ(pack.Tracks()[0].type, "int");
    const auto* colour = pack.FindTrack("lst.flash_color");
    ASSERT_NE(colour, nullptr);
    EXPECT_EQ(colour->type, "color");
    EXPECT_EQ(colour->def, 4294967295.0); // 0xFFFFFFFF, white at full strength
    const auto* cut = pack.FindTrack("director.cut");
    ASSERT_NE(cut, nullptr);
    EXPECT_EQ(cut->type, "event");
    EXPECT_EQ(cut->index, 12);
    EXPECT_FALSE(cut->entries.empty());

    EXPECT_EQ(pack.Keys("director.cut").size(), 4u);
    EXPECT_EQ(pack.Keys("lst.flash").size(), 126u); // 63 flashes, up and back down
    EXPECT_EQ(pack.KeyCount(), 130u);
    EXPECT_TRUE(pack.Keys("no.such").empty());
}

TEST(ExocetPackTest, PathsResolveAgainstTheFile)
{
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(Fixture("cra-sneezeman.exocet")));
    const fs::path dir = fs::path(pack.Path()).parent_path();
    EXPECT_EQ(fs::path(pack.KitPath()), (dir / "kit/kit.json").lexically_normal());
    EXPECT_EQ(fs::path(pack.ModulePath(0)), (dir / "ice_cold_diet_phunk.mod").lexically_normal());
}

TEST(ExocetPackTest, SaveKeepsWhatItDoesNotKnow)
{
    TempDir tmp;
    const fs::path file = tmp.Path() / "x.exocet";
    WriteText(file, "{\n  \"exocet\": 1,\n  \"future\": {\"a\": [1, 2.5, \"\\u00e9t\\u00e9\"]},\n"
                    "  \"keys\": {}\n}\n");
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    ASSERT_TRUE(pack.Save());
    // ensure_ascii off, as exocet.py writes it: the accents come back as UTF-8
    EXPECT_EQ(ReadText(file.string()), "{\n  \"exocet\": 1,\n  \"future\": {\n    \"a\": [1, 2.5, "
                                       "\"\xC3\xA9t\xC3\xA9\"]\n  },\n  \"keys\": {}\n}\n");
    EXPECT_FALSE(fs::exists(file.string() + ".tmp"));
}

TEST(ExocetPackTest, LoadRefusesWhatIsNotAnExocetFile)
{
    TempDir tmp;
    const fs::path a = tmp.Path() / "a.exocet";
    const fs::path b = tmp.Path() / "b.exocet";
    WriteText(a, "not json");
    WriteText(b, "{\"exocet\": 2}");
    ExocetPack pack;
    std::string error;
    EXPECT_FALSE(pack.Load(a.string(), &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(pack.Load(b.string(), &error));
    EXPECT_FALSE(pack.Load((tmp.Path() / "missing.exocet").string(), &error));
}

TEST(ExocetPackTest, FindSiblingFollowsTheElfRules)
{
    TempDir tmp;
    const fs::path disk = tmp.Path() / "cra-sneezeman.adf";
    WriteText(disk, "");

    // none
    EXPECT_FALSE(ExocetPack::FindSibling(disk.string()).has_value());

    // the one in the directory, whatever it is called
    WriteText(tmp.Path() / "other.exocet", "{}");
    auto found = ExocetPack::FindSibling(disk.string());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(fs::path(*found).filename(), "other.exocet");

    // two, neither named for the disk: no pack
    WriteText(tmp.Path() / "third.EXOCET", "{}");
    EXPECT_FALSE(ExocetPack::FindSibling(disk.string()).has_value());

    // the same base name wins over any number of others
    WriteText(tmp.Path() / "cra-sneezeman.exocet", "{}");
    found = ExocetPack::FindSibling(disk.string());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(fs::path(*found).filename(), "cra-sneezeman.exocet");
}

TEST(ExocetPackTest, SetKeyFollowsTheKitsRules)
{
    TempDir tmp;
    const fs::path file = tmp.Path() / "cra-sneezeman.exocet";
    fs::copy_file(Fixture("cra-sneezeman.exocet"), file);
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    const auto revision = pack.Revision();
    std::string error;

    EXPECT_FALSE(pack.SetKey("no.such", 0, 0, 0, 0, 1, "step", &error));
    EXPECT_FALSE(pack.SetKey("lst.flash", 0, 0, 0, 0, 17, "step", &error));  // 0..16
    EXPECT_FALSE(pack.SetKey("lst.flash", 0, 0, 0, 0, 1.5, "step", &error)); // an int track
    EXPECT_FALSE(pack.SetKey("lst.flash", 0, 0, 64, 0, 1, "step", &error));  // no row 64
    EXPECT_FALSE(pack.SetKey("lst.flash", 0, 0, 0, 0, 1, "wobble", &error));
    EXPECT_FALSE(pack.SetKey("director.cut", 0, 0, 0, 0, 0, "step", &error)); // 0 is no event
    EXPECT_FALSE(pack.SetKey("director.cut", 0, 0, 0, 0, 9, "step", &error)); // not an entry
    EXPECT_FALSE(pack.Dirty());
    EXPECT_EQ(pack.Revision(), revision);

    // an event is always a step, whatever was asked
    ASSERT_TRUE(pack.SetKey("director.cut", 0, 1, 0, 0, 1, "linear", &error)) << error;
    EXPECT_EQ(pack.FindKey("director.cut", 0, 1, 0, 0)->interp, "step");
    // in song order, before the first key (3/0)
    EXPECT_EQ(pack.Keys("director.cut").front().pos, 1);
    // a key at the same place is replaced, not added
    ASSERT_TRUE(pack.SetKey("director.cut", 0, 1, 0, 0, 2, "step", &error)) << error;
    EXPECT_EQ(pack.Keys("director.cut").size(), 5u);
    EXPECT_EQ(pack.FindKey("director.cut", 0, 1, 0, 0)->value, 2);
    EXPECT_TRUE(pack.Dirty());
    EXPECT_GT(pack.Revision(), revision);

    // a new track's first key
    ASSERT_TRUE(pack.SetKey("lst.rot_speed", 0, 21, 59, 0, -8, "smooth", &error)) << error;
    EXPECT_EQ(pack.Keys("lst.rot_speed").size(), 1u);
    EXPECT_TRUE(pack.DeleteKey("lst.rot_speed", 0, 21, 59, 0));
    EXPECT_FALSE(pack.DeleteKey("lst.rot_speed", 0, 21, 59, 0));
    EXPECT_TRUE(pack.DeleteKey("director.cut", 0, 1, 0, 0));

    // back where it was, the file comes back byte for byte - and is written as exocet.py writes
    ASSERT_TRUE(pack.Save(&error)) << error;
    EXPECT_FALSE(pack.Dirty());
    std::string saved = ReadText(file.string());
    // the empty rot_speed list is the one trace the edits left
    EXPECT_NE(saved.find("\"lst.rot_speed\": []"), std::string::npos);
}

TEST(ExocetPackTest, ColourKeysAreWrittenAsExocetPyWritesThem)
{
    TempDir tmp;
    const fs::path file = tmp.Path() / "cra-sneezeman.exocet";
    fs::copy_file(Fixture("cra-sneezeman.exocet"), file);
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    std::string error;

    EXPECT_FALSE(pack.SetKey("lst.flash_color", 0, 0, 0, 0, -1, "step", &error));
    EXPECT_FALSE(pack.SetKey("lst.flash_color", 0, 0, 0, 0, 4294967296.0, "step", &error));
    EXPECT_FALSE(pack.SetKey("lst.flash_color", 0, 0, 0, 0, 0.5, "step", &error));
    ASSERT_TRUE(pack.SetKey("lst.flash_color", 0, 20, 0, 0, 0xFF8000C0u, "linear", &error))
        << error;
    EXPECT_EQ(pack.FindKey("lst.flash_color", 0, 20, 0, 0)->value, double(0xFF8000C0u));
    ASSERT_TRUE(pack.Save(&error)) << error;

    const std::string saved = ReadText(file.string());
    EXPECT_NE(saved.find("\"value\": \"#FF8000C0\""), std::string::npos);
    ExocetPack again;
    ASSERT_TRUE(again.Load(file.string()));
    ASSERT_EQ(again.Keys("lst.flash_color").size(), 1u);
    EXPECT_EQ(again.Keys("lst.flash_color")[0].value, double(0xFF8000C0u));
    EXPECT_EQ(again.Keys("lst.flash_color")[0].interp, "linear");
}

TEST(ExocetPackTest, ColoursParseAsTheKitParsesThem)
{
    uint32_t v = 0;
    ASSERT_TRUE(adm::core::ExocetParseColor("#12AbCdEf", &v));
    EXPECT_EQ(v, 0x12ABCDEFu);
    ASSERT_TRUE(adm::core::ExocetParseColor(" #FF0001 ", &v)); // alpha FF
    EXPECT_EQ(v, 0xFF0001FFu);
    EXPECT_FALSE(adm::core::ExocetParseColor("FF0001", &v));
    EXPECT_FALSE(adm::core::ExocetParseColor("#FF001", &v));
    EXPECT_FALSE(adm::core::ExocetParseColor("#FF0001GG", &v));
    EXPECT_FALSE(adm::core::ExocetParseColor("0xFF0001FF", &v));
    EXPECT_EQ(adm::core::ExocetColorText(0x0102FEFDu), "#0102FEFD");

    // the exotest fixture keys them both ways, and in the number form a manifest default takes
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(Fixture("exotest.exocet")));
    const auto keys = pack.Keys("b.tint");
    ASSERT_EQ(keys.size(), 8u);
    EXPECT_EQ(keys[0].value, double(0x00FF80FFu));
    EXPECT_EQ(keys[1].value, double(0xFF0001FFu)); // "#FF0001"
    EXPECT_EQ(pack.FindTrack("b.tint")->def, double(0x12345678u));
}

TEST(ExocetPackTest, SetSongOnlyDirtiesWhenItChanges)
{
    TempDir tmp;
    const fs::path file = tmp.Path() / "x.exocet";
    fs::copy_file(Fixture("cra-sneezeman.exocet"), file);
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    pack.SetSongs({pack.SongOrder(0)}, {pack.SongDigest(0)});
    EXPECT_FALSE(pack.Dirty());
    pack.SetSongs({pack.SongOrder(0)}, {0x5333A5EE});
    EXPECT_TRUE(pack.Dirty());
    EXPECT_EQ(pack.SongDigest(0), 0x5333A5EEu);
}

TEST(ExocetPackTest, UndoAndRedoAreWholeEdits)
{
    TempDir tmp;
    const fs::path file = tmp.Path() / "x.exocet";
    fs::copy_file(Fixture("cra-sneezeman.exocet"), file);
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    const std::string loaded = pack.Dump();
    EXPECT_FALSE(pack.CanUndo());

    // a refused edit, and one to the key it already is, are no step
    EXPECT_FALSE(pack.SetKey("lst.flash", 0, 0, 0, 0, 17, "step"));
    EXPECT_FALSE(pack.CanUndo());

    ASSERT_TRUE(pack.SetKey("lst.rot_speed", 0, 21, 59, 0, -8, "smooth"));
    const std::string one = pack.Dump();
    ASSERT_TRUE(pack.SetKey("lst.rot_speed", 0, 21, 59, 0, -8, "smooth"));
    ASSERT_TRUE(pack.DeleteKey("lst.rot_speed", 0, 21, 59, 0));
    EXPECT_NE(pack.Dump(), one);

    ASSERT_TRUE(pack.Undo()); // the delete
    EXPECT_EQ(pack.Dump(), one);
    EXPECT_TRUE(pack.Dirty());
    ASSERT_TRUE(pack.Undo()); // the set
    EXPECT_EQ(pack.Dump(), loaded);
    EXPECT_FALSE(pack.Dirty()); // back to what was loaded
    EXPECT_FALSE(pack.CanUndo());

    const auto revision = pack.Revision();
    ASSERT_TRUE(pack.Redo());
    EXPECT_EQ(pack.Dump(), one);
    EXPECT_GT(pack.Revision(), revision);

    // an edit after an undo drops what could have been redone
    ASSERT_TRUE(pack.Undo());
    ASSERT_TRUE(pack.SetKey("lst.flash", 0, 0, 0, 0, 3, "step"));
    EXPECT_FALSE(pack.CanRedo());
}

TEST(ExocetPackTest, ClearTrackIsOneStepAndDropsTheList)
{
    TempDir tmp;
    const fs::path file = tmp.Path() / "x.exocet";
    fs::copy_file(Fixture("cra-sneezeman.exocet"), file);
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    const std::string loaded = pack.Dump();
    const size_t keyed = pack.Keys("lst.flash").size();
    ASSERT_GT(keyed, 1u);

    EXPECT_EQ(pack.ClearTrack("lst.flash"), keyed);
    EXPECT_TRUE(pack.Keys("lst.flash").empty());
    // the file lists only keyed tracks, as the kit writes it
    const auto tracks = pack.KeyedTracks();
    EXPECT_EQ(std::find(tracks.begin(), tracks.end(), "lst.flash"), tracks.end());
    EXPECT_TRUE(pack.Dirty());
    EXPECT_EQ(pack.UndoSteps(), 1u);

    // a track with no keys: nothing to clear, no step
    EXPECT_EQ(pack.ClearTrack("lst.flash"), 0u);
    EXPECT_EQ(pack.ClearTrack("lst.rot_speed"), 0u);
    EXPECT_EQ(pack.UndoSteps(), 1u);

    ASSERT_TRUE(pack.Undo());
    EXPECT_EQ(pack.Dump(), loaded);
    EXPECT_FALSE(pack.Dirty());
}

TEST(ExocetPackTest, EditsWithOneMergeAreOneStep)
{
    TempDir tmp;
    const fs::path file = tmp.Path() / "x.exocet";
    fs::copy_file(Fixture("cra-sneezeman.exocet"), file);
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    const std::string loaded = pack.Dump();

    for (uint32_t v : {0x10203040u, 0x20304050u, 0x30405060u})
        ASSERT_TRUE(pack.SetKey("lst.flash_color", 0, 20, 0, 0, v, "linear", nullptr, "drag"));
    ASSERT_TRUE(pack.Undo());
    EXPECT_EQ(pack.Dump(), loaded);
    EXPECT_FALSE(pack.CanUndo());

    // BreakMerge ends the drag: the next edit with the same merge is a step of its own
    ASSERT_TRUE(pack.Redo());
    const std::string redone = pack.Dump();
    pack.SetKey("lst.flash_color", 0, 20, 0, 0, 0x40506070u, "linear", nullptr, "drag");
    pack.BreakMerge();
    pack.SetKey("lst.flash_color", 0, 20, 0, 0, 0x50607080u, "linear", nullptr, "drag");
    ASSERT_TRUE(pack.Undo());
    EXPECT_NE(pack.Dump(), redone);
    ASSERT_TRUE(pack.Undo());
    EXPECT_EQ(pack.Dump(), redone);
}

TEST(ExocetPackTest, TickKeysAreTheirOwnPlaceAndWrittenAfterInterp)
{
    TempDir tmp;
    const fs::path file = tmp.Path() / "cra-sneezeman.exocet";
    fs::copy_file(Fixture("cra-sneezeman.exocet"), file);
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    std::string error;
    EXPECT_FALSE(pack.SetKey("lst.flash", 0, 19, 8, 0, 4, "step", &error, {}, 256)); // not a tick
    EXPECT_FALSE(pack.SetKey("lst.flash", 0, 19, 8, 0, 4, "step", &error, {}, -1));
    // lst.flash has a key on 19/8; one on its tick 2 is another key, after it
    ASSERT_TRUE(pack.SetKey("lst.flash", 0, 19, 8, 0, 4, "linear", &error, {}, 2)) << error;
    const auto keys = pack.Keys("lst.flash");
    ASSERT_EQ(keys.size(), 127u);
    EXPECT_EQ(keys[0].tick, 0);
    EXPECT_EQ(keys[1].row, 8);
    EXPECT_EQ(keys[1].tick, 2);
    ASSERT_NE(pack.FindKey("lst.flash", 0, 19, 8, 0, 2), nullptr);
    EXPECT_EQ(pack.FindKey("lst.flash", 0, 19, 8, 0, 2)->value, 4);
    EXPECT_EQ(pack.FindKey("lst.flash", 0, 19, 8, 0, 1), nullptr);
    EXPECT_EQ(pack.FindKey("lst.flash", 0, 19, 8, 0)->value, 16); // tick 0's is untouched
    const std::string text = pack.Dump();
    EXPECT_NE(text.find("{\"pos\": 19, \"row\": 8, \"pass\": 0, \"value\": 4, \"interp\": "
                        "\"linear\", \"tick\": 2}"),
              std::string::npos);
    // saved and loaded again, the tick is kept
    ASSERT_TRUE(pack.Save(&error)) << error;
    ExocetPack again;
    ASSERT_TRUE(again.Load(file.string()));
    EXPECT_EQ(again.Dump(), text);
    // deleting the tick's key leaves the row's own, and the file as it was
    EXPECT_TRUE(again.DeleteKey("lst.flash", 0, 19, 8, 0, 2));
    EXPECT_FALSE(again.DeleteKey("lst.flash", 0, 19, 8, 0, 2));
    EXPECT_NE(again.FindKey("lst.flash", 0, 19, 8, 0), nullptr);
    EXPECT_EQ(again.Dump(), ReadText(Fixture("cra-sneezeman.exocet")));
}

// The music section (the crass repo's docs/exocet.md, "The music"): which samples may be 4-bit
TEST(ExocetPackTest, SampleSettingsAreTheMusicSection)
{
    TempDir tmp;
    const fs::path file = tmp.Path() / "x.exocet";
    fs::copy_file(Fixture("cra-sneezeman.exocet"), file);
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    const std::string loaded = pack.Dump();
    EXPECT_EQ(pack.SampleSetting(3), "auto");
    EXPECT_EQ(pack.AutoDb(), ExocetPack::kAutoDb);

    // refused: no such sample, no such setting - and no step for either
    std::string error;
    EXPECT_FALSE(pack.SetSampleSetting(0, "4bit", &error));
    EXPECT_FALSE(pack.SetSampleSetting(32, "4bit", &error));
    EXPECT_FALSE(pack.SetSampleSetting(3, "6bit", &error));
    EXPECT_FALSE(pack.CanUndo());
    EXPECT_TRUE(pack.SetSampleSetting(3, "auto")); // already: nothing to do
    EXPECT_FALSE(pack.Dirty());

    const uint64_t rev = pack.Revision();
    ASSERT_TRUE(pack.SetSampleSetting(7, "8bit"));
    ASSERT_TRUE(pack.SetSampleSetting(3, "4bit"));
    ASSERT_TRUE(pack.SetAutoDb(26.5));
    EXPECT_GT(pack.Revision(), rev); // the kit checks again
    EXPECT_EQ(pack.SampleSetting(3), "4bit");
    EXPECT_EQ(pack.SampleSetting(7), "8bit");
    EXPECT_EQ(pack.AutoDb(), 26.5);
    // as tools/exocet.py writes it: the samples in order, whatever order they were set in
    const std::string dump = pack.Dump();
    const std::string want = " },\n  \"music\": {\n    \"samples\": {\n      \"3\": \"4bit\",\n"
                             "      \"7\": \"8bit\"\n    },\n    \"auto_db\": 26.5\n  }\n}\n";
    ASSERT_GE(dump.size(), want.size());
    EXPECT_EQ(dump.substr(dump.size() - want.size()), want);
    EXPECT_NE(pack.MusicText(), "");

    // each change is a step; undone, the section goes with the last of it
    EXPECT_EQ(pack.UndoSteps(), 3u);
    ASSERT_TRUE(pack.Undo());
    EXPECT_EQ(pack.AutoDb(), ExocetPack::kAutoDb);
    // back to auto takes a sample out, and the default bar is not written
    ASSERT_TRUE(pack.SetSampleSetting(3, "auto"));
    ASSERT_TRUE(pack.SetSampleSetting(7, "auto"));
    EXPECT_EQ(pack.Dump(), loaded);
    EXPECT_EQ(pack.MusicText(), "");
    EXPECT_FALSE(pack.Dirty());

    // a whole number of dB is written as one
    ASSERT_TRUE(pack.SetAutoDb(28.0));
    EXPECT_NE(pack.Dump().find("\"auto_db\": 28\n"), std::string::npos);
    ASSERT_TRUE(pack.SetAutoDb(ExocetPack::kAutoDb));
    EXPECT_EQ(pack.Dump(), loaded);
}

// EXO-01: the file arrives beside a disk; a wrong type is refused, never thrown
TEST(ExocetPackTest, LoadRefusesWrongTypes)
{
    TempDir tmp;
    ExocetPack pack;
    std::string error;
    for (const char* text :
         {"{\"exocet\": \"1\"}", "{\"exocet\": null}", "{\"exocet\": 1.5}", "{\"exocet\": [1]}"}) {
        const fs::path f = tmp.Path() / "bad.exocet";
        WriteText(f, text);
        bool loaded = true;
        EXPECT_NO_THROW(loaded = pack.Load(f.string(), &error)) << text;
        EXPECT_FALSE(loaded) << text;
    }
}

TEST(ExocetPackTest, SetKeyToleratesMalformedKeyList)
{
    TempDir tmp;
    const fs::path file = tmp.Path() / "cra-sneezeman.exocet";
    auto doc = nlohmann::ordered_json::parse(ReadText(Fixture("cra-sneezeman.exocet")));
    auto& list = doc["keys"]["director.cut"];
    list.insert(list.begin(), nullptr);
    list.push_back({{"pos", "3"}, {"row", 0}, {"value", 1}});
    WriteText(file, doc.dump(2));
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    std::string error;
    bool ok = false;
    EXPECT_NO_THROW(ok = pack.SetKey("director.cut", 0, 1, 0, 0, 1, "step", &error));
    EXPECT_TRUE(ok) << error;
    EXPECT_NO_THROW(ok = pack.DeleteKey("director.cut", 0, 1, 0, 0));
    EXPECT_TRUE(ok);
    EXPECT_NO_THROW(pack.DeleteKey("director.cut", 0, 99, 0, 0));
}

TEST(ExocetPackTest, SampleSettingIgnoresNonStrings)
{
    TempDir tmp;
    const fs::path file = tmp.Path() / "x.exocet";
    auto doc = nlohmann::ordered_json::parse(ReadText(Fixture("cra-sneezeman.exocet")));
    doc["music"]["samples"]["3"] = 8;
    doc["music"]["samples"]["4"] = nullptr;
    WriteText(file, doc.dump(2));
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    EXPECT_EQ(pack.SampleSetting(3), "auto");
    bool ok = false;
    EXPECT_NO_THROW(ok = pack.SetSampleSetting(5, "8bit"));
    EXPECT_TRUE(ok);
    EXPECT_EQ(pack.SampleSetting(5), "8bit");
    EXPECT_EQ(pack.SampleSetting(3), "auto");
}

TEST(ExocetPackTest, BookmarksAreASectionInSongOrderWithOneSlotEach)
{
    using adm::core::ExocetBookmarkInfo;
    TempDir tmp;
    const fs::path file = tmp.Path() / "x.exocet";
    fs::copy_file(Fixture("exotest.exocet"), file);
    ExocetPack pack;
    ASSERT_TRUE(pack.Load(file.string()));
    const std::string loaded = pack.Dump();
    EXPECT_TRUE(pack.Bookmarks().empty());

    ASSERT_TRUE(pack.SetBookmark({0, 4, 16, 0, 1, "drop"}));
    ASSERT_TRUE(pack.SetBookmark({0, 2, 0, 0, 2, ""}));
    EXPECT_TRUE(pack.Dirty());
    auto marks = pack.Bookmarks();
    ASSERT_EQ(marks.size(), 2u);
    EXPECT_EQ(marks[0].pos, 2); // song order, whatever order they were set in
    EXPECT_EQ(marks[0].slot, 2);
    EXPECT_EQ(marks[1].name, "drop");
    // a line each, as exocet.py writes a list of objects; one song: no "song", no empty name
    EXPECT_NE(pack.Dump().find("\n    {\"pos\": 4, \"row\": 16, \"pass\": 0, \"slot\": 1, "
                               "\"name\": \"drop\"}"),
              std::string::npos)
        << pack.Dump();
    EXPECT_NE(pack.Dump().find("{\"pos\": 2, \"row\": 0, \"pass\": 0, \"slot\": 2}"),
              std::string::npos);

    // a slot another has is taken from it
    ASSERT_TRUE(pack.SetBookmark({0, 2, 0, 0, 1, "intro"}));
    marks = pack.Bookmarks();
    EXPECT_EQ(marks[0].slot, 1);
    EXPECT_EQ(marks[0].name, "intro");
    EXPECT_EQ(marks[1].slot, 0);

    std::string error;
    EXPECT_FALSE(pack.SetBookmark({0, 2, 0, 0, 10, ""}, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(pack.SetBookmark({0, 2, 64, 0, 1, ""}, &error));

    // saved and read back
    ASSERT_TRUE(pack.Save(&error)) << error;
    ExocetPack again;
    ASSERT_TRUE(again.Load(file.string()));
    EXPECT_EQ(again.Bookmarks().size(), 2u);
    EXPECT_EQ(again.Dump(), pack.Dump());

    // removed, the section with the last; cleared is one step; undo brings them back
    EXPECT_TRUE(pack.DeleteBookmark(0, 4, 16, 0));
    EXPECT_FALSE(pack.DeleteBookmark(0, 4, 16, 0));
    EXPECT_EQ(pack.ClearBookmarks(), 1u);
    EXPECT_EQ(pack.Dump(), loaded);
    EXPECT_EQ(pack.ClearBookmarks(), 0u);
    ASSERT_TRUE(pack.Undo());
    EXPECT_EQ(pack.Bookmarks().size(), 1u);
    ASSERT_TRUE(pack.Undo());
    EXPECT_EQ(pack.Bookmarks().size(), 2u);
    EXPECT_FALSE(pack.Dirty()); // as saved
}
