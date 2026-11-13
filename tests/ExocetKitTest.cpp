#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../src/core/ExocetKit.h"
#include "../src/core/Subprocess.h"

using namespace adm::core;
namespace fs = std::filesystem;

namespace {

class TempDir
{
public:
    TempDir()
    {
        m_path = fs::temp_directory_path() /
                 ("adm_exocet_kit_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
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

void Write(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

const char* kKitJson = R"({
  "kit": 1, "production": "test",
  "requires": {"python": ">=3.8", "python-lz4": "4.4.5", "salvador": "1.4.1"},
  "params": "params.json",
  "commands": {"check": ["kit.py", "check"], "pack": ["kit.py", "pack", "--fast"]}
})";

bool PythonRuns()
{
    Subprocess p;
    return p.Run({"python", "-c", "print(1)"}, [](const std::string&) {}) == 0;
}

} // namespace

TEST(ExocetKitTest, ScratchBesideTheExocet)
{
    EXPECT_EQ(fs::path(ExocetScratchDir("C:/prod/sync/disk.exocet")),
              fs::path("C:/prod/sync/exocet_temp"));
    EXPECT_FALSE(ExocetScratchDir({}).empty()); // no pack: the system's temp folder
}

TEST(ExocetKitTest, ReadsKitJson)
{
    TempDir tmp;
    Write(tmp.Path() / "kit" / "kit.json", kKitJson);
    ExocetKitInfo kit;
    std::string error;
    ASSERT_TRUE(kit.Load((tmp.Path() / "kit" / "kit.json").string(), &error)) << error;
    EXPECT_EQ(kit.production, "test");
    EXPECT_EQ(kit.requirements.at("python-lz4"), "4.4.5");
    EXPECT_EQ(fs::path(kit.paramsPath).filename(), "params.json");
    ASSERT_TRUE(kit.HasRole("pack"));
    EXPECT_EQ(kit.commands.at("pack"), (std::vector<std::string>{"kit.py", "pack", "--fast"}));
    EXPECT_FALSE(kit.HasRole("rebuild"));

    Write(tmp.Path() / "bad.json", R"({"kit": 2})");
    EXPECT_FALSE(kit.Load((tmp.Path() / "bad.json").string(), &error));
}

TEST(ExocetKitTest, HashFollowsTheFilesNotTheirCaches)
{
    TempDir tmp;
    const fs::path kit = tmp.Path() / "kit";
    Write(kit / "kit.json", kKitJson);
    Write(kit / "kit.py", "print('a')\n");
    const std::string h1 = ExocetKitHash(kit.string());
    EXPECT_EQ(h1.size(), 32u);
    // Python's own caches are not the kit
    Write(kit / "__pycache__" / "kit.cpython-312.pyc", "junk");
    EXPECT_EQ(ExocetKitHash(kit.string()), h1);
    // a changed script is another kit, to trust again
    Write(kit / "kit.py", "print('b')\n");
    EXPECT_NE(ExocetKitHash(kit.string()), h1);
    // and so is a renamed one with the same bytes
    Write(kit / "kit.py", "print('a')\n");
    EXPECT_EQ(ExocetKitHash(kit.string()), h1);
    fs::rename(kit / "kit.py", kit / "kat.py");
    EXPECT_NE(ExocetKitHash(kit.string()), h1);
    fs::rename(kit / "kat.py", kit / "kit.py");

    // the params are data: rewritten per build, and left out when asked
    Write(kit / "params.json", R"({"kit": 1, "disk_room": 648})");
    const std::string h2 = ExocetKitHash(kit.string(), {"params.json"});
    EXPECT_EQ(h2, h1);
    Write(kit / "params.json", R"({"kit": 1, "disk_room": 645})");
    EXPECT_EQ(ExocetKitHash(kit.string(), {"params.json"}), h2);
    EXPECT_NE(ExocetKitHash(kit.string()), h2);
}

TEST(ExocetKitTest, RequirementsNameTheToolAndBothVersions)
{
    ExocetKitInfo kit;
    kit.requirements = {{"python", ">=3.12"}, {"python-lz4", "4.4.5"}, {"salvador", "1.4.1"}};
    ExocetToolVersions v;
    v.pythonRuns = true;
    v.python = "3.12.10";
    v.lz4 = "4.4.5";
    v.salvador = "1.4.1";
    EXPECT_TRUE(ExocetCheckRequires(kit, v).empty());

    v.python = "3.11.9";
    v.lz4 = "4.3.2";
    v.salvador.clear();
    const auto issues = ExocetCheckRequires(kit, v);
    ASSERT_EQ(issues.size(), 3u);
    for (const auto& i : issues) {
        EXPECT_EQ(i.level, "block");
        EXPECT_TRUE(i.Blocks("push") && i.Blocks("write"));
        EXPECT_TRUE(i.peekaboo);
    }
    EXPECT_NE(issues[1].text.find("4.4.5"), std::string::npos);
    EXPECT_NE(issues[1].text.find("4.3.2"), std::string::npos);
    EXPECT_NE(issues[2].text.find("none"), std::string::npos);

    v.pythonRuns = false;
    v.error = "could not run python";
    const auto noPython = ExocetCheckRequires(kit, v);
    ASSERT_EQ(noPython.size(), 1u);
    EXPECT_EQ(noPython[0].code, "tool.python");
}

// Two songs (the Sneezeman disk's intro, then its main song): the rows end to end with their
// song, the intro's lead on negative positions, each song's digest and order
TEST(ExocetKitTest, ParsesTwoSongs)
{
    const auto report = nlohmann::json::parse(R"({
      "command": "check", "issues": [],
      "music": {"frames": 40, "digest": "0x22222222", "order": [3, 4], "channels": 4,
                "orders": [[0], [3, 4]],
                "songs": [{"digest": "0x11111111", "resident": true},
                          {"digest": "0x22222222"}]},
      "rows": [{"song": 0, "pos": -1, "pattern": 1, "row": 0, "pass": 0, "frame": 0, "frames": 6},
               {"song": 0, "pos": 0, "pattern": 0, "row": 0, "pass": 0, "frame": 6, "frames": 6},
               {"song": 1, "pos": 0, "pattern": 3, "row": 0, "pass": 0, "frame": 12, "frames": 6}],
      "samples": []
    })");
    ExocetCheck c;
    ASSERT_TRUE(c.Parse(report));
    EXPECT_EQ(c.digest, 0x11111111u); // the first song's: the one exocet.bin's header names
    EXPECT_EQ(c.digests, (std::vector<uint32_t>{0x11111111u, 0x22222222u}));
    EXPECT_EQ(c.orders.size(), 2u);
    EXPECT_EQ(c.musicSong, 1);
    EXPECT_EQ(c.FindRow(0, -1, 0, 0), 0);
    EXPECT_EQ(c.FindRow(0, 0, 0, 0), 1);
    EXPECT_EQ(c.FindRow(1, 0, 0, 0), 2);
    EXPECT_EQ(adm::core::ExocetPlaceLabel(c.rows[0], true), "0 L1:00");
    EXPECT_EQ(adm::core::ExocetPlaceLabel(c.rows[2], true), "1 00:00");
    EXPECT_EQ(adm::core::ExocetPlaceLabel(c.rows[2], false), "00:00");
}

TEST(ExocetKitTest, ParsesACheckReport)
{
    const auto report = nlohmann::json::parse(R"({
      "command": "check",
      "issues": [{"level": "warn", "code": "exocet.orphan", "title": "t", "text": "x",
                  "subject": {"track": "a.b"}},
                 {"level": "block", "code": "music.arena", "title": "u", "text": "y",
                  "at": {"pos": 1, "row": 2, "pass": 0}, "blocks": ["inject"]}],
      "music": {"frames": 40, "digest": "0x111f7093", "order": [0, 1], "channels": 4},
      "rows": [{"pos": 0, "pattern": 0, "row": 0, "pass": 0, "frame": 0, "frames": 6,
                "cells": [[1, 428, 0, 0], [0, 0, 0, 0], [0, 0, 12, 32], [0, 0, 0, 0]]},
               {"pos": 1, "pattern": 1, "row": 0, "pass": 0, "frame": 6, "frames": 6, "cells": []},
               {"pos": 1, "pattern": 1, "row": 0, "pass": 1, "frame": 12, "frames": 6, "cells": []}],
      "samples": [{"index": 1, "name": "bass", "bytes": 2336, "volume": 48, "loop": true}],
      "keys": {"lst.flash": [{"row": 1, "interp": 1, "value": 16}]}
    })");
    ExocetCheck c;
    ASSERT_TRUE(c.Parse(report));
    EXPECT_EQ(c.rows.size(), 3u);
    EXPECT_EQ(c.digest, 0x111F7093u);
    EXPECT_EQ(c.order, (std::vector<int>{0, 1}));
    EXPECT_EQ(c.FindRow(0, 1, 0, 1), 2);
    EXPECT_EQ(c.FindRow(0, 1, 0, 0), 1);
    EXPECT_EQ(c.FindRow(0, 5, 0, 0), -1);
    EXPECT_EQ(c.FindRow(1, 1, 0, 0), -1);
    EXPECT_EQ(c.RowAtFrame(0), 0);
    EXPECT_EQ(c.RowAtFrame(11), 1);
    EXPECT_EQ(c.RowAtFrame(1000), 2);
    ASSERT_EQ(c.rows[0].cells.size(), 4u);
    EXPECT_EQ(c.rows[0].cells[0][1], 428);
    EXPECT_EQ(c.keys.at("lst.flash")[0].value, 16);
    ASSERT_EQ(c.issues.size(), 2u);
    EXPECT_TRUE(c.issues[1].Blocks("inject"));
    EXPECT_FALSE(c.issues[1].Blocks("write"));
    EXPECT_FALSE(c.issues[0].Blocks("inject")); // a warning blocks nothing
    EXPECT_EQ(c.issues[1].at.value("row", 0), 2);

    ExocetCheck none;
    EXPECT_FALSE(none.Parse(nlohmann::json::object())); // no rows: a failed check
}

TEST(ExocetKitTest, NoteNames)
{
    EXPECT_EQ(ExocetNoteName(0), "---");
    EXPECT_EQ(ExocetNoteName(428), "C-2");
    EXPECT_EQ(ExocetNoteName(856), "C-1");
    EXPECT_EQ(ExocetNoteName(113), "B-3");
    EXPECT_EQ(ExocetNoteName(254), "A-2");
    EXPECT_EQ(ExocetNoteName(240), "A#2");
}

TEST(ExocetKitTest, RunsARoleAsItsOwnProcess)
{
    if (!PythonRuns())
        GTEST_SKIP() << "no python on PATH";
    TempDir tmp;
    const fs::path kit = tmp.Path() / "kit";
    Write(kit / "kit.json", kKitJson);
    // the report path comes last; the environment is Peekaboo's to set
    Write(kit / "kit.py", R"(import json, os, sys
role, args, report = sys.argv[1], sys.argv[2:-1], sys.argv[-1]
out = {"command": role, "args": args, "tools": os.environ.get("EXOCET_TOOLS"),
       "pythonpath": os.environ.get("PYTHONPATH"), "cwd": os.getcwd(), "issues": []}
if args and args[-1] == "fail":
    out["issues"].append({"level": "block", "code": "t.fail", "title": "no", "text": "failed",
                          "blocks": ["push"]})
json.dump(out, open(report, "w"))
sys.exit(1 if out["issues"] else 0)
)");
    ExocetKitInfo info;
    ASSERT_TRUE(info.Load((kit / "kit.json").string()));
    ExocetTools tools{"python", "C:/some/tools"};

    auto r = ExocetRunKit(info, tools, "pack", {"a", "b"});
    ASSERT_TRUE(r.launched);
    EXPECT_EQ(r.exitCode, 0);
    EXPECT_TRUE(r.Ok());
    EXPECT_EQ(r.report.value("command", ""), "pack");
    EXPECT_EQ(r.report["args"], nlohmann::json({"--fast", "a", "b"}));
    EXPECT_EQ(r.report.value("tools", ""), "C:/some/tools");
    EXPECT_TRUE(r.report["pythonpath"].is_null());
    EXPECT_NE(r.report.value("cwd", ""), fs::current_path().string()); // a scratch directory

    // under the root it is given - made if it is not there - and gone after
    const fs::path root = fs::path(ExocetScratchDir((kit / "x.exocet").string())) / "session-1";
    fs::remove_all(root.parent_path());
    r = ExocetRunKit(info, tools, "pack", {"a"}, 120000, nullptr, root.string());
    EXPECT_TRUE(r.Ok());
    EXPECT_EQ(fs::path(r.report.value("cwd", "")).parent_path(), root);
    EXPECT_TRUE(fs::is_empty(root));
    fs::remove_all(root.parent_path());

    r = ExocetRunKit(info, tools, "check", {"fail"});
    EXPECT_EQ(r.exitCode, 1);
    EXPECT_FALSE(r.Ok());
    ASSERT_EQ(r.issues.size(), 1u);
    EXPECT_TRUE(r.issues[0].Blocks("push"));

    r = ExocetRunKit(info, tools, "rebuild", {});
    EXPECT_FALSE(r.launched);
    ASSERT_EQ(r.issues.size(), 1u);
    EXPECT_EQ(r.issues[0].code, "kit.role");

    // a kit that dies without its report still says something
    Write(kit / "kit.py", "import sys\nprint('boom')\nsys.exit(3)\n");
    r = ExocetRunKit(info, tools, "check", {});
    EXPECT_EQ(r.exitCode, 3);
    ASSERT_EQ(r.issues.size(), 1u);
    EXPECT_NE(r.issues[0].text.find("boom"), std::string::npos);

    // and one that hangs is stopped
    Write(kit / "kit.py", "import time\ntime.sleep(30)\n");
    r = ExocetRunKit(info, tools, "check", {}, 1500);
    EXPECT_TRUE(r.timedOut);
    EXPECT_LT(r.seconds, 10.0);
    ASSERT_FALSE(r.issues.empty());
    EXPECT_EQ(r.issues.back().code, "kit.timeout");
}

// EXO-01: a wrong-typed version is refused, never thrown
TEST(ExocetKitTest, KitJsonRefusesWrongTypes)
{
    TempDir tmp;
    for (const char* text : {R"({"kit": "1"})", R"({"kit": null})", R"([1])"}) {
        Write(tmp.Path() / "kit.json", text);
        ExocetKitInfo kit;
        bool loaded = true;
        EXPECT_NO_THROW(loaded = kit.Load((tmp.Path() / "kit.json").string()));
        EXPECT_FALSE(loaded) << text;
    }
}

// EXO-02: a kit under development writes a wrong type: Parse skips it, never throws (it runs on
// the kit worker, where a throw is std::terminate)
TEST(ExocetKitTest, ParseSurvivesWrongTypes)
{
    const auto report = nlohmann::json::parse(R"({
      "music": {"frames": "40", "channels": null, "songs": [5, {"resident": "no"}]},
      "rows": [{"pos": "0", "row": 0, "frame": 0, "frames": 6,
                "cells": [[0, 0, null, 0], 7, [70000, -1, "x", 0]]},
               5,
               {"pos": 1, "row": 0, "frame": 6, "frames": 6}],
      "samples": [3, {"index": "1", "bytes": null, "codec": "8bit", "db": "loud"}],
      "keys": {"lst.flash": [null, {"row": "1", "value": 16}], "lst.other": 4}
    })");
    ExocetCheck c;
    bool ok = false;
    ASSERT_NO_THROW(ok = c.Parse(report));
    EXPECT_TRUE(ok);
    ASSERT_EQ(c.rows.size(), 2u);          // the non-object row is skipped
    ASSERT_EQ(c.rows[0].cells.size(), 3u); // a scalar cell stays a channel, empty
    EXPECT_EQ(c.rows[0].cells[0][2], 0);
    EXPECT_EQ(c.rows[0].cells[2][0], 0); // 70000 is no 16-bit value
    ASSERT_EQ(c.samples.size(), 1u);
    ASSERT_EQ(c.keys.at("lst.flash").size(), 1u);
    EXPECT_EQ(c.keys.at("lst.flash")[0].value, 16);
    ASSERT_FALSE(c.issues.empty());
    EXPECT_EQ(c.issues.back().code, "kit.report");
}
