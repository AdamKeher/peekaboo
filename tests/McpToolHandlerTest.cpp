#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "../src/core/CopperDisassembler.h"
#include "../src/core/EmulatorStateStore.h"
#include "../src/core/ExocetPack.h"
#include "../src/core/ExocetSession.h"
#include "../src/core/HexUtil.h"
#include "../src/core/McpConsoleTools.h"
#include "../src/core/McpServer.h"
#include "../src/core/McpToolHandler.h"
#include "../src/core/MemoryAnnotations.h"
#include "../src/core/PrivateFeatures.h"

using adm::core::McpHost;
using adm::core::McpToolHandler;
using nlohmann::json;

namespace {

McpHost MakeHost(adm::core::MemoryAnnotations* ann)
{
    McpHost host;
    host.appVersion = "test-version";
    host.annotations = ann;
    // No emulator wired — emulator-dependent tools should report errors gracefully.
    host.emulator = []() -> adm::AmigaEmulator* { return nullptr; };
    return host;
}

// Exocet's and the crAss engine's tools only exist with private features on, and the manifest is
// built with the handler, so this goes first in a test that uses them.
struct PrivateFeaturesOn
{
    PrivateFeaturesOn() { adm::core::SetPrivateFeatures(true); }
    ~PrivateFeaturesOn() { adm::core::SetPrivateFeatures(false); }
};

} // namespace

// A plain Peekaboo has no Exocet or crAss engine tools: not listed, and refused by name.
TEST(McpToolHandlerTest, PrivateToolsAreHiddenByDefault)
{
    ASSERT_FALSE(adm::core::PrivateFeatures());
    McpToolHandler handler(MakeHost(nullptr));
    for (const auto& t : handler.Manifest()) {
        const std::string name = t.value("name", std::string{});
        EXPECT_FALSE(McpToolHandler::IsPrivateTool(name)) << name << " is listed";
    }
    for (const char* name : {"get_exocet", "get_exocet_grid", "push_exocet", "get_crass_profile"}) {
        const json r = handler.Handle(name, json::object());
        EXPECT_EQ(r.value("error", ""), std::string("unknown tool: ") + name);
    }

    PrivateFeaturesOn privateOn;
    McpToolHandler shown(MakeHost(nullptr));
    int privateTools = 0;
    for (const auto& t : shown.Manifest())
        privateTools += McpToolHandler::IsPrivateTool(t.value("name", std::string{})) ? 1 : 0;
    EXPECT_GT(privateTools, 20) << "the Exocet tools and get_crass_profile";
}

TEST(McpToolHandlerTest, ManifestListsCoreTools)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    ASSERT_TRUE(manifest.is_array());

    std::vector<std::string> names;
    for (const auto& t : manifest)
        names.push_back(t.value("name", std::string{}));

    for (const char* expected :
         {"get_status", "get_registers", "read_memory", "disassemble", "set_label", "set_comment",
          "screenshot", "exec_console", "add_breakpoint"}) {
        EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end())
            << "missing tool: " << expected;
    }

    // Every tool must carry a name, description and inputSchema.
    for (const auto& t : manifest) {
        EXPECT_TRUE(t.contains("name"));
        EXPECT_TRUE(t.contains("description"));
        EXPECT_TRUE(t.contains("inputSchema"));
    }
}

TEST(McpToolHandlerTest, ConsoleParityRegistryCoversEveryConsoleCommand)
{
    // Keep this list aligned with ArCommandInterpreter::HandleCommand. Aliases are included: a
    // regression here means a new console command was added without either a structured MCP tool
    // or a console-backed parity tool.
    const std::vector<std::string> consoleCommands = {
        "M",         "MM",       "MMM",        "N",         "NN",         "NNN",       "NO",
        "Y",         "YY",       "YYY",        "YS",        "MQ",         "NQ",        "D",
        "DD",        "DDD",      "R",          "RC",        "RF",         "RM",        "E",
        "C",         "G",        "X",          "STOP",      "START",      "TURBO",     "WARP",
        "SF",        "EF",       "RUNTO",      "PAUSE",     "S",          "ST",        "TR",
        "TA",        "BS",       "BD",         "B",         "BDA",        "BKS",       "BKD",
        "BK",        "BKDA",     "MS",         "MD",        "MW",         "MDA",       "F",
        "FS",        "FR",       "FA",         "FAQ",       "COMP",       "TRANS",     "EX",
        "O",         "WS",       "MEMCODE",    "ADD",       "LM",         "SM",        "A",
        "FB",        "TRB",      "EB",         "BL",        "BC",         "TS",        "T",
        "TX",        "TF",       "TFD",        "TDS",       "TDC",        "TD",        "TDI",
        "TDD",       "TDX",      "LOAD",       "L",         "LIST",       "RESET",     "DOCS",
        "HD",        "CLOSE",    "?",          "HELP",      "H",          "KEYS",      "CLS",
        "CLEAR",     "VERSION",  "VER",        "V",         "FRAME",      "RT",        "RS",
        "RB",        "WT",       "WDS",        "WDB",       "BOOTCHK",    "DATACHK",   "BAMCHK",
        "CRC16",     "CRC32",    "W",          "PALETTE",   "PAL",        "STACK",     "CALL",
        "PROFILE",   "PROF",     "CALLSTACK",  "CS",        "BLIT",       "BLITLOG",   "BPL",
        "BITPLANES", "SPR",      "SPRITES",    "PF",        "PLAYFIELDS", "DISPLAY",   "DISP",
        "TRACE",     "EXECBASE", "AVAIL",      "LIBRARIES", "LIBS",       "RESOURCES", "DEVICES",
        "TASKS",     "PORTS",    "INTERRUPTS", "INTS",      "EXCEPTIONS", "CHIPREGS",  "DCHIP",
        "KICKVER",   "TM",       "TL",         "TMS",       "TMD",        "TB",        "TBD",
        "DSKLOG",    "BDSK",     "AUDIO",      "AUD",       "AUDREC",     "PR",        "PROTECT",
    };
    const std::vector<std::string> nativeCommands = {
        "D",     "DD",      "DDD", "C",         "STOP", "START",   "SF",   "EF",         "RUNTO",
        "PAUSE", "S",       "ST",  "TR",        "BKS",  "BKD",     "BK",   "BKDA",       "DOCS",
        "CLOSE", "FRAME",   "W",   "PALETTE",   "PAL",  "PROFILE", "PROF", "CALLSTACK",  "CS",
        "BLIT",  "BLITLOG", "BPL", "BITPLANES", "SPR",  "SPRITES", "PF",   "PLAYFIELDS", "DISPLAY",
        "DISP",  "TRACE",   "TM",  "TL",        "TMS",  "TMD",     "TB",   "TBD",        "DSKLOG",
        "BDSK",  "AUDIO",   "AUD", "AUDREC",    "RC",   "RF",      "RM",
    };

    std::vector<std::string> covered = nativeCommands;
    for (const auto& spec : adm::core::McpConsoleToolSpecs())
        for (const char* command : spec.commands)
            covered.emplace_back(command);

    for (const auto& command : consoleCommands) {
        EXPECT_NE(std::find(covered.begin(), covered.end(), command), covered.end())
            << "console command has no full MCP equivalent: " << command;
    }
}

TEST(McpToolHandlerTest, ConsoleParityToolsRouteVariantsAndArgumentsToSharedInterpreter)
{
    McpHost host = MakeHost(nullptr);
    std::vector<std::string> seen;
    host.execConsole = [&seen](const std::string& line) {
        seen.push_back(line);
        return "output:" + line;
    };
    McpToolHandler handler(std::move(host));

    json result = handler.Handle("find_memory",
                                 json{{"variant", "fs"}, {"arguments", "-L \"score\",0 7FFFF"}});
    ASSERT_FALSE(result.contains("error"));
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], "FS -L \"score\",0 7FFFF");
    EXPECT_EQ(result.value("command", std::string{}), seen[0]);
    EXPECT_EQ(result.value("output", std::string{}), "output:" + seen[0]);
}

TEST(McpToolHandlerTest, ConsoleParityToolsSupportInteractiveInputWithoutDuplicatingParsers)
{
    McpHost host = MakeHost(nullptr);
    std::vector<std::string> seen;
    host.execConsole = [&seen](const std::string& line) {
        seen.push_back(line);
        return "<" + line + ">";
    };
    McpToolHandler handler(std::move(host));

    json result = handler.Handle(
        "assemble", json{{"arguments", "$400"}, {"input", json::array({"NOP", "RTS"})}});
    ASSERT_FALSE(result.contains("error"));
    EXPECT_EQ(seen, (std::vector<std::string>{"A $400", "NOP", "RTS"}));
    ASSERT_TRUE(result.contains("transcript"));
    EXPECT_EQ(result["transcript"].size(), 3u);
}

// MCP clients validate every tool's inputSchema before they will use ANY of them: one tool with
// a null `properties` failed the whole tools/list handshake and disconnected the session.
TEST(McpToolHandlerTest, EveryToolSchemaIsAValidObjectSchema)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    ASSERT_TRUE(manifest.is_array());
    ASSERT_FALSE(manifest.empty());

    for (size_t i = 0; i < manifest.size(); ++i) {
        const auto& tool = manifest[i];
        const std::string name = tool.value("name", std::string{});
        const std::string where = "tools[" + std::to_string(i) + "] (" + name + ")";

        ASSERT_TRUE(tool.contains("inputSchema")) << where << " has no inputSchema";
        const auto& schema = tool["inputSchema"];
        EXPECT_EQ(schema.value("type", std::string{}), "object") << where;

        // A parameterless tool must still say `"properties": {}` — null is not a record.
        ASSERT_TRUE(schema.contains("properties")) << where << " has no properties";
        EXPECT_TRUE(schema["properties"].is_object())
            << where << " properties must be an object, got " << schema["properties"].dump();

        for (const auto& [param, spec] : schema["properties"].items())
            EXPECT_TRUE(spec.is_object())
                << where << " property '" << param << "' must be an object";

        if (schema.contains("required")) {
            ASSERT_TRUE(schema["required"].is_array()) << where << " required must be an array";
            for (const auto& req : schema["required"]) {
                ASSERT_TRUE(req.is_string()) << where << " required entries must be strings";
                EXPECT_TRUE(schema["properties"].contains(req.get<std::string>()))
                    << where << " requires '" << req.get<std::string>()
                    << "' but never declares it";
            }
        }
    }
}

TEST(McpToolHandlerTest, ConsoleParityManifestHasUniqueDiscoverableTools)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    std::vector<std::string> names;
    for (const auto& tool : manifest) {
        const std::string name = tool.value("name", std::string{});
        EXPECT_EQ(std::count(names.begin(), names.end(), name), 0) << "duplicate tool: " << name;
        names.push_back(name);
    }

    for (const auto& spec : adm::core::McpConsoleToolSpecs()) {
        auto it = std::find(names.begin(), names.end(), spec.name);
        EXPECT_NE(it, names.end()) << "missing console parity tool: " << spec.name;
    }
}

TEST(McpToolHandlerTest, ManifestListsCopperDisassemblyTool)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    auto it = std::find_if(manifest.begin(), manifest.end(), [](const json& tool) {
        return tool.value("name", std::string{}) == "disassemble_copper";
    });
    ASSERT_NE(it, manifest.end());
    EXPECT_EQ((*it)["inputSchema"]["required"], json::array({"source"}));
}

TEST(McpToolHandlerTest, CopperDisassemblyNeedsEmulator)
{
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("disassemble_copper", json{{"source", "COP1"}});
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, CopperDisassemblerExposesMoveRegisterName)
{
    json registers = {{"100", {{"name", "BPLCON0"}}}};
    auto instruction = adm::core::CopperDisassembler::Disassemble(0x0100, 0x4200, registers);
    EXPECT_EQ(instruction.name, "MOVE");
    EXPECT_EQ(instruction.registerName, "BPLCON0");
    EXPECT_EQ(instruction.details, "BPLCON0 = $4200");
}

TEST(McpToolHandlerTest, ManifestListsMouseClickTool)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    auto it = std::find_if(manifest.begin(), manifest.end(), [](const json& tool) {
        return tool.value("name", std::string{}) == "click_mouse";
    });
    ASSERT_NE(it, manifest.end());
    EXPECT_EQ((*it)["inputSchema"]["required"], json::array({"button"}));
}

TEST(McpToolHandlerTest, MouseClickNeedsRunningEmulator)
{
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("click_mouse", json{{"button", "left"}, {"durationFrames", 2}});
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, StatusWorksWithoutEmulator)
{
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("get_status", json::object());
    EXPECT_FALSE(r.contains("error"));
    EXPECT_EQ(r.value("appVersion", std::string{}), "test-version");
    EXPECT_EQ(r.value("coreLoaded", true), false);
}

TEST(McpToolHandlerTest, EmulatorToolsErrorWithoutEmulator)
{
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("read_memory", json{{"addr", "0x1000"}, {"len", 16}});
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, UnknownToolReportsError)
{
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("does_not_exist", json::object());
    ASSERT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, AnnotationRoundTrip)
{
    adm::core::MemoryAnnotations ann;
    ann.Clear(); // start from a known-empty store
    McpToolHandler handler(MakeHost(&ann));

    json setRes = handler.Handle("set_label", json{{"addr", "0x400"}, {"text", "MainLoop"}});
    EXPECT_TRUE(setRes.value("ok", false));

    json setC = handler.Handle("set_comment", json{{"addr", 0x400}, {"text", "entry point"}});
    EXPECT_TRUE(setC.value("ok", false));

    json got = handler.Handle("get_annotations", json::object());
    ASSERT_TRUE(got.contains("annotations"));
    ASSERT_EQ(got["annotations"].size(), 1u);
    EXPECT_EQ(got["annotations"][0].value("label", std::string{}), "MainLoop");
    EXPECT_EQ(got["annotations"][0].value("comment", std::string{}), "entry point");

    json rem = handler.Handle("remove_annotation", json{{"addr", "0x400"}});
    EXPECT_TRUE(rem.value("removed", false));

    json got2 = handler.Handle("get_annotations", json::object());
    EXPECT_EQ(got2["annotations"].size(), 0u);

    // Records each call in the activity log.
    EXPECT_GE(handler.Activity().size(), 5u);

    ann.Clear();
}

TEST(McpToolHandlerTest, ManifestListsProfilingTools)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    std::vector<std::string> names;
    for (const auto& t : manifest)
        names.push_back(t.value("name", std::string{}));
    for (const char* expected : {"set_cpu_trace", "get_profile"}) {
        EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end())
            << "missing tool: " << expected;
    }
}

TEST(McpToolHandlerTest, ProfileUnavailableWithoutHook)
{
    // MakeHost wires no getProfile hook -> the tool reports it is unavailable.
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("get_profile", json{{"view", "functions"}});
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, ProfileRejectsUnknownView)
{
    McpHost host = MakeHost(nullptr);
    host.getProfile = [](const adm::core::ProfileQuery&) { return adm::core::ProfileSnapshot{}; };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("get_profile", json{{"view", "bogus"}});
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, ProfileFunctionsSerialized)
{
    McpHost host = MakeHost(nullptr);
    adm::core::ProfileQuery seen;
    host.getProfile = [&seen](const adm::core::ProfileQuery& q) {
        seen = q;
        adm::core::ProfileSnapshot snap;
        snap.available = true;
        snap.haveSymbols = true;
        snap.totalCycles = 1000;
        snap.resolvedCycles = 800;
        snap.unresolvedCycles = 200;
        adm::core::FunctionStats fn;
        fn.name = "main";
        fn.address = 0x1234;
        fn.totalCycles = 400;
        fn.count = 100;
        fn.pctFrame = 0.4f;
        snap.functions.push_back(fn);
        return snap;
    };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("get_profile", json{{"view", "functions"}, {"limit", 10}});
    ASSERT_FALSE(r.contains("error"));
    EXPECT_EQ(r.value("view", std::string{}), "functions");
    EXPECT_TRUE(r.value("available", false));
    EXPECT_EQ(seen.view, adm::core::ProfileView::Functions);
    EXPECT_EQ(seen.limit, 10);
    ASSERT_TRUE(r.contains("functions"));
    ASSERT_EQ(r["functions"].size(), 1u);
    EXPECT_EQ(r["functions"][0].value("name", std::string{}), "main");
    EXPECT_EQ(r["functions"][0].value("totalCycles", 0u), 400u);
}

TEST(McpToolHandlerTest, BlitterLogUnavailableWithoutHook)
{
    // MakeHost wires no getBlitLog hook -> the tool reports it is unavailable.
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("get_blitter_log", json::object());
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, BlitterLogSerializesBlitsAndClampsLimit)
{
    McpHost host = MakeHost(nullptr);
    int seenLimit = -999;
    host.getBlitLog = [&seenLimit](int limit) {
        seenLimit = limit;
        adm::core::BlitLogSnapshot snap;
        snap.available = true;
        snap.totalBlits = 1;
        snap.cpuBlits = 1;
        snap.totalDmaCycles = 320;
        snap.busPercent = 0.45f;
        adm::core::BlitLogEntry e;
        e.startCycle = 0x1A2B;
        e.op = "copy";
        e.minterm = 0xCA;
        e.bltcon0 = 0x09F0;
        e.dmaCycles = 320;
        e.useA = true;
        e.aptr = 0x012345;
        e.amod = 0;
        e.useD = true;
        e.dptr = 0x0ABCDE;
        e.dmod = -2;
        snap.blits.push_back(e);
        return snap;
    };
    McpToolHandler handler(std::move(host));
    // limit above the cap (2000) is clamped.
    json r = handler.Handle("get_blitter_log", json{{"limit", 99999}});
    ASSERT_FALSE(r.contains("error"));
    EXPECT_EQ(seenLimit, 2000);
    EXPECT_TRUE(r.value("available", false));
    EXPECT_EQ(r.value("totalBlits", 0), 1);
    ASSERT_TRUE(r.contains("blits"));
    ASSERT_EQ(r["blits"].size(), 1u);
    const json& b0 = r["blits"][0];
    EXPECT_EQ(b0.value("op", std::string{}), "copy");
    EXPECT_EQ(b0.value("trigger", std::string{}), "cpu");
    EXPECT_EQ(b0.value("minterm", std::string{}), "CA");
    ASSERT_TRUE(b0["channels"].contains("A"));
    EXPECT_EQ(b0["channels"]["A"].value("addr", std::string{}), "012345");
    EXPECT_EQ(b0["channels"]["D"].value("mod", 0), -2);
    // Unused channels are omitted.
    EXPECT_FALSE(b0["channels"].contains("B"));
}

TEST(McpToolHandlerTest, BlitterLogReportsUnavailableFrame)
{
    McpHost host = MakeHost(nullptr);
    host.getBlitLog = [](int) { return adm::core::BlitLogSnapshot{}; }; // available = false
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("get_blitter_log", json::object());
    ASSERT_FALSE(r.contains("error"));
    EXPECT_FALSE(r.value("available", true));
    EXPECT_TRUE(r.contains("hint"));
}

TEST(McpToolHandlerTest, ManifestListsBlitterLogTool)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    std::vector<std::string> names;
    for (const auto& t : manifest)
        names.push_back(t.value("name", std::string{}));
    EXPECT_NE(std::find(names.begin(), names.end(), "get_blitter_log"), names.end());
}

// ---------------------------------------------------------------------------
// get_crass_profile

TEST(McpToolHandlerTest, CrassProfileUnavailableWithoutHook)
{
    PrivateFeaturesOn privateOn;
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("get_crass_profile", json::object());
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, CrassProfileReportsAbsentBlockWithoutFakeNumbers)
{
    PrivateFeaturesOn privateOn;
    McpHost host = MakeHost(nullptr);
    // The reader's "not present" result: a program with no PROFILE build, or a registry entry
    // that has outlived the program that made it.
    host.getCrassProfile = []() { return adm::core::CrassProfileStats{}; };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("get_crass_profile", json::object());
    ASSERT_FALSE(r.contains("error"));
    EXPECT_FALSE(r.value("present", true));
    EXPECT_TRUE(r.contains("reason"));
    EXPECT_FALSE(r.contains("frames"));
    EXPECT_FALSE(r.contains("sections"));
}

TEST(McpToolHandlerTest, CrassProfileSerializesFramesAndSections)
{
    PrivateFeaturesOn privateOn;
    McpHost host = MakeHost(nullptr);
    host.getCrassProfile = []() {
        adm::core::CrassProfileStats p;
        p.present = true;
        p.blockAddress = 0x00A120;
        p.version = 1;
        p.windowFields = 250;
        p.linesPer2Fields = 626;
        p.timingOk = true;
        p.rendered = 37;
        p.worstGap = 1;
        p.lastFields = 250;
        p.lastRendered = 248;
        p.lastDropped = 2;
        p.lastWorstGap = 3;
        p.totalRendered = 1250;
        p.totalDropped = 5;

        adm::core::CrassProfileSection build;
        build.name = "cv_build";
        build.lastLines = 154;
        build.lastMin = 96;
        build.lastAvg = 154;
        build.lastMax = 161;
        build.lastCount = 250;
        build.min = 0xFFFF;
        p.sections.push_back(build);

        adm::core::CrassProfileSection idle;
        idle.name = "cv_idle";
        idle.lastCount = 0; // registered, but not called during the last window
        p.sections.push_back(idle);
        return p;
    };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("get_crass_profile", json::object());
    ASSERT_FALSE(r.contains("error"));
    EXPECT_TRUE(r.value("present", false));
    EXPECT_EQ(r.value("blockAddress", std::string{}), "0000A120");
    EXPECT_EQ(r.value("linesPer2Fields", 0), 626);
    EXPECT_TRUE(r.value("timingOk", false));
    EXPECT_FALSE(r.contains("hint"));

    ASSERT_TRUE(r.contains("frames"));
    const json& f = r["frames"];
    EXPECT_EQ(f.value("lastRendered", 0), 248);
    EXPECT_EQ(f.value("lastFields", 0), 250);
    EXPECT_EQ(f.value("lastDropped", 0), 2);
    EXPECT_EQ(f.value("lastWorstGap", 0), 3);
    EXPECT_EQ(f.value("rendered", 0), 37);
    EXPECT_EQ(f.value("totalDropped", 0), 5);
    EXPECT_TRUE(f.value("windowComplete", false));

    ASSERT_TRUE(r.contains("sections"));
    ASSERT_EQ(r["sections"].size(), 2u);
    EXPECT_EQ(r["sections"][0].value("name", std::string{}), "cv_build");
    EXPECT_EQ(r["sections"][0].value("lastAvg", 0), 154);
    EXPECT_TRUE(r["sections"][0].value("called", false));
    EXPECT_FALSE(r["sections"][1].value("called", true));
    EXPECT_FALSE(r.contains("sectionsTruncated"));
}

TEST(McpToolHandlerTest, CrassProfileFlagsAStoppedLineCounter)
{
    PrivateFeaturesOn privateOn;
    McpHost host = MakeHost(nullptr);
    host.getCrassProfile = []() {
        adm::core::CrassProfileStats p;
        p.present = true;
        p.linesPer2Fields = 626;
        p.timingOk = false;
        return p;
    };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("get_crass_profile", json::object());
    EXPECT_TRUE(r.value("present", false));
    EXPECT_FALSE(r.value("timingOk", true));
    EXPECT_TRUE(r.contains("hint"));
}

TEST(McpToolHandlerTest, ManifestListsCrassProfileTool)
{
    PrivateFeaturesOn privateOn;
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    std::vector<std::string> names;
    for (const auto& t : manifest)
        names.push_back(t.value("name", std::string{}));
    EXPECT_NE(std::find(names.begin(), names.end(), "get_crass_profile"), names.end());
}

// ---------------------------------------------------------------------------
// get_exocet

namespace {

std::shared_ptr<adm::core::ExocetPack> LoadExocetFixture()
{
    for (const char* p :
         {"tests/data/exocet/cra-sneezeman.exocet", "../tests/data/exocet/cra-sneezeman.exocet",
          "../../tests/data/exocet/cra-sneezeman.exocet"}) {
        auto pack = std::make_shared<adm::core::ExocetPack>();
        if (pack->Load(p))
            return pack;
    }
    return nullptr;
}

adm::core::ExocetBlockState ExocetFixtureBlock(uint32_t manifest)
{
    adm::core::ExocetBlockState b;
    b.present = true;
    b.blockAddress = 0x0020A000;
    b.version = 1;
    b.tracks = 13;
    b.manifest = manifest;
    b.row = 1340;
    b.frame = 5362;
    b.generation = 1;
    b.seen = 1;
    // av.* (a colour at 5 and 7), lst.flash 16, lst.flash_color, lst.twist_speed -8, ...
    b.values = {80, 16, 0, 0, 0, -1, 0, -1, 16, static_cast<int32_t>(0xFF8000C0u), -8, 3, 0};
    b.arena.valid = true;
    b.arena.song = 0x111F7093;
    b.fired = {{12, 12, 577}};
    b.cuts = {{1, 11, 521, 12, 577}, {2, 540, 2148, 0xFFFF, 0xFFFF}};
    return b;
}

} // namespace

TEST(McpToolHandlerTest, ExocetUnavailableWithoutHook)
{
    PrivateFeaturesOn privateOn;
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("get_exocet", json::object());
    EXPECT_TRUE(r.contains("error"));
}

// Every session tool is declared, and answers "unavailable" without a session and "no .exocet"
// with one that has no pack - never falls through to "unknown tool"
TEST(McpToolHandlerTest, ExocetSessionToolsDeclaredAndGuarded)
{
    PrivateFeaturesOn privateOn;
    const std::vector<std::string> names = {"get_exocet_timeline",
                                            "get_exocet_grid",
                                            "get_exocet_keys",
                                            "get_exocet_music",
                                            "get_exocet_disk_map",
                                            "set_exocet_keys",
                                            "undo_exocet",
                                            "redo_exocet",
                                            "save_exocet",
                                            "check_exocet",
                                            "seek_exocet",
                                            "manage_exocet_breakpoints",
                                            "set_exocet_key",
                                            "delete_exocet_key",
                                            "key_exocet_every_hit",
                                            "clear_exocet_track",
                                            "manage_exocet_bookmarks"};
    McpToolHandler bare(MakeHost(nullptr));
    const json manifest = bare.Manifest();
    for (const auto& n : names) {
        bool declared = false;
        for (const auto& t : manifest)
            declared |= t.value("name", "") == n;
        EXPECT_TRUE(declared) << n;
        const json r = bare.Handle(n, json::object());
        EXPECT_EQ(r.value("error", ""), "exocet unavailable") << n;
    }
    adm::core::ExocetSession session;
    McpHost host = MakeHost(nullptr);
    host.exocetSession = [&session]() { return &session; };
    McpToolHandler handler(std::move(host));
    for (const auto& n : names) {
        const json r = handler.Handle(n, json::object());
        if (n == "get_exocet_disk_map") { // the window's answer: no map, and why
            EXPECT_FALSE(r.value("ok", true));
            EXPECT_EQ(r.value("status", ""), "No .exocet beside the disk.");
            continue;
        }
        ASSERT_TRUE(r.contains("error")) << n;
        EXPECT_EQ(r["error"].get<std::string>().find("unknown tool"), std::string::npos) << n;
    }
}

TEST(McpToolHandlerTest, ExocetWithNeitherBlockNorPack)
{
    PrivateFeaturesOn privateOn;
    McpHost host = MakeHost(nullptr);
    host.getExocet = []() {
        adm::core::ExocetSnapshot s;
        s.documentPath = "C:/disks/game.adf";
        return s;
    };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("get_exocet", json::object());
    ASSERT_FALSE(r.contains("error"));
    EXPECT_FALSE(r["block"].value("present", true));
    EXPECT_FALSE(r["pack"].value("present", true));
    EXPECT_NE(r["pack"].value("reason", std::string{}).find("game.adf"), std::string::npos);
    EXPECT_FALSE(r.contains("manifestMatch"));
    EXPECT_FALSE(r.contains("fired"));
    EXPECT_TRUE(r["tracks"].empty());
}

TEST(McpToolHandlerTest, ExocetNamesLiveValuesFromAMatchingPack)
{
    PrivateFeaturesOn privateOn;
    auto pack = LoadExocetFixture();
    ASSERT_NE(pack, nullptr);
    McpHost host = MakeHost(nullptr);
    host.getExocet = [pack]() {
        adm::core::ExocetSnapshot s;
        s.block = ExocetFixtureBlock(pack->ManifestHash());
        s.pack = pack;
        return s;
    };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("get_exocet", json::object());
    ASSERT_FALSE(r.contains("error"));
    EXPECT_TRUE(r.value("manifestMatch", false));
    EXPECT_TRUE(r.value("songMatch", false));
    EXPECT_TRUE(r["block"].value("settled", false));
    EXPECT_EQ(r["block"].value("clock", std::string{}), "music");
    EXPECT_EQ(r["pack"].value("manifest", std::string{}), "0x09515221");
    ASSERT_EQ(r["tracks"].size(), 13u);
    EXPECT_EQ(r["tracks"][0].value("name", std::string{}), "av.speed");
    EXPECT_EQ(r["tracks"][8].value("name", std::string{}), "lst.flash");
    EXPECT_EQ(r["tracks"][8].value("value", 0), 16);
    EXPECT_EQ(r["tracks"][9].value("type", std::string{}), "color");
    EXPECT_EQ(r["tracks"][9].value("color", std::string{}), "#FF8000C0");
    EXPECT_FALSE(r["tracks"][8].contains("color"));
    EXPECT_EQ(r["tracks"][10].value("value", 0), -8);
    EXPECT_EQ(r["tracks"][12].value("keys", 0), 4);
    ASSERT_EQ(r["fired"].size(), 1u);
    EXPECT_EQ(r["fired"][0].value("name", std::string{}), "director.cut");
    ASSERT_EQ(r["cuts"].size(), 2u);
    EXPECT_EQ(r["cuts"][0].value("cutFrame", 0), 577);
    EXPECT_TRUE(r["cuts"][1]["cutFrame"].is_null());
}

TEST(McpToolHandlerTest, ExocetDoesNotNameTracksAcrossBuilds)
{
    PrivateFeaturesOn privateOn;
    // A disk from one build, an .exocet from another: the indices mean different tracks, so the
    // block's values are reported bare rather than under the wrong names.
    auto pack = LoadExocetFixture();
    ASSERT_NE(pack, nullptr);
    McpHost host = MakeHost(nullptr);
    host.getExocet = [pack]() {
        adm::core::ExocetSnapshot s;
        s.block = ExocetFixtureBlock(0x12345678);
        s.pack = pack;
        return s;
    };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("get_exocet", json::object());
    EXPECT_FALSE(r.value("manifestMatch", true));
    ASSERT_EQ(r["tracks"].size(), 13u);
    EXPECT_FALSE(r["tracks"][0].contains("name"));
    EXPECT_EQ(r["tracks"][0].value("value", 0), 80);
    EXPECT_FALSE(r["tracks"][9].contains("color")); // the type is the unmatched pack's
    EXPECT_FALSE(r["fired"][0].contains("name"));
}

TEST(McpToolHandlerTest, ManifestListsExocetTool)
{
    PrivateFeaturesOn privateOn;
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    std::vector<std::string> names;
    for (const auto& t : manifest)
        names.push_back(t.value("name", std::string{}));
    EXPECT_NE(std::find(names.begin(), names.end(), "get_exocet"), names.end());
}

// ---------------------------------------------------------------------------
// get_disk_log / add_disk_breakpoint

TEST(McpToolHandlerTest, DiskLogUnavailableWithoutHook)
{
    // MakeHost wires no getDiskLog hook -> the tool reports it is unavailable.
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("get_disk_log", json::object());
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, DiskLogReportsUnavailableSession)
{
    McpHost host = MakeHost(nullptr);
    host.getDiskLog = [](const adm::core::DiskLogQuery&) {
        return adm::core::DiskLogSnapshot{}; // available = false
    };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("get_disk_log", json::object());
    ASSERT_FALSE(r.contains("error"));
    EXPECT_FALSE(r.value("available", true));
    EXPECT_TRUE(r.contains("hint"));
}

TEST(McpToolHandlerTest, DiskLogSerializesTransfersAndClampsLimit)
{
    McpHost host = MakeHost(nullptr);
    adm::core::DiskLogQuery seen;
    host.getDiskLog = [&seen](const adm::core::DiskLogQuery& q) {
        seen = q;
        adm::core::DiskLogSnapshot snap;
        snap.available = true;
        snap.totalTransfers = 1;
        snap.writes = 1;
        snap.bytesWritten = 5632;
        snap.anyWrites = true;

        adm::core::DiskLogEntry e;
        e.seq = 7;
        e.direction = "write";
        e.trigger = "cpu";
        e.pc = 0x012345;
        e.cyl = 40;
        e.side = 1;
        e.dskpt = 0x0ABCDE;
        e.bytes = 5632;
        e.sha1 = "abc123";
        e.standardTrack = true;
        e.sectorCount = 11;
        e.sectorsChecksumOk = 11;
        e.persistedToFile = true;
        adm::core::DiskLogSectorInfo s;
        s.track = 81;
        s.sector = 3;
        s.checksumOk = true;
        s.sha1 = "def456";
        e.sectors.push_back(s);
        snap.transfers.push_back(e);
        return snap;
    };
    McpToolHandler handler(std::move(host));
    // limit above the cap (2000) is clamped.
    json r = handler.Handle("get_disk_log", json{{"limit", 99999}, {"direction", "write"}});
    ASSERT_FALSE(r.contains("error"));
    EXPECT_EQ(seen.limit, 2000);
    EXPECT_EQ(seen.direction, 3); // "write" -> DSKDMA_WRITE
    EXPECT_TRUE(r.value("available", false));
    EXPECT_TRUE(r.value("anyWrites", false));

    ASSERT_TRUE(r.contains("transfers"));
    ASSERT_EQ(r["transfers"].size(), 1u);
    const json& t0 = r["transfers"][0];
    EXPECT_EQ(t0.value("direction", std::string{}), "write");
    EXPECT_EQ(t0.value("pc", std::string{}), "012345");
    EXPECT_EQ(t0.value("track", -1), 81); // cyl*2 + side
    EXPECT_TRUE(t0.value("persistedToFile", false));
    ASSERT_TRUE(t0.contains("sectors"));
    EXPECT_EQ(t0["sectors"][0].value("sector", -1), 3);
    // Exceptional flags are omitted on an ordinary transfer.
    EXPECT_FALSE(t0.contains("aborted"));
    EXPECT_FALSE(t0.contains("turbo"));
}

TEST(McpToolHandlerTest, DiskLogRejectsBadDirection)
{
    McpHost host = MakeHost(nullptr);
    host.getDiskLog = [](const adm::core::DiskLogQuery&) {
        adm::core::DiskLogSnapshot snap;
        snap.available = true;
        return snap;
    };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("get_disk_log", json{{"direction", "sideways"}});
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, DiskBreakpointUnavailableWithoutHook)
{
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("add_disk_breakpoint", json{{"access", "write"}});
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, DiskBreakpointTranslatesTrackAndAccess)
{
    McpHost host = MakeHost(nullptr);
    int seenAccess = -99, seenCyl = -99, seenSide = -99, seenSector = -99;
    host.armDiskBreak = [&](int access, int cyl, int side, int sector, uint32_t, uint32_t) {
        seenAccess = access;
        seenCyl = cyl;
        seenSide = side;
        seenSector = sector;
        return true;
    };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("add_disk_breakpoint", json{{"access", "write"}, {"track", 81}});
    ASSERT_FALSE(r.contains("error"));
    EXPECT_TRUE(r.value("ok", false));
    EXPECT_EQ(seenAccess, 2); // write
    EXPECT_EQ(seenCyl, 40);   // 81 / 2
    EXPECT_EQ(seenSide, 1);   // 81 % 2
    EXPECT_EQ(seenSector, -1);
}

TEST(McpToolHandlerTest, DiskBreakpointSectorFilterCarriesCompletionNote)
{
    McpHost host = MakeHost(nullptr);
    host.armDiskBreak = [](int, int, int, int, uint32_t, uint32_t) { return true; };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("add_disk_breakpoint", json{{"access", "any"}, {"sector", 3}});
    ASSERT_FALSE(r.contains("error"));
    // A sector predicate can only be evaluated once the MFM data has arrived.
    EXPECT_TRUE(r.contains("note"));
}

TEST(McpToolHandlerTest, DiskBreakpointRejectsBadArguments)
{
    McpHost host = MakeHost(nullptr);
    host.armDiskBreak = [](int, int, int, int, uint32_t, uint32_t) { return true; };
    McpToolHandler handler(std::move(host));
    EXPECT_TRUE(
        handler.Handle("add_disk_breakpoint", json{{"access", "sideways"}}).contains("error"));
    EXPECT_TRUE(handler.Handle("add_disk_breakpoint", json{{"access", "any"}, {"track", 400}})
                    .contains("error"));
    EXPECT_TRUE(handler.Handle("add_disk_breakpoint", json{{"access", "any"}, {"sector", 44}})
                    .contains("error"));
}

TEST(McpToolHandlerTest, RemoveDiskBreakpointDisarms)
{
    McpHost host = MakeHost(nullptr);
    int seenAccess = -99;
    host.armDiskBreak = [&](int access, int, int, int, uint32_t, uint32_t) {
        seenAccess = access;
        return true;
    };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("remove_disk_breakpoint", json::object());
    ASSERT_FALSE(r.contains("error"));
    EXPECT_EQ(seenAccess, -1); // negative access disarms
}

TEST(McpToolHandlerTest, ManifestListsDiskTools)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    std::vector<std::string> names;
    for (const auto& t : manifest)
        names.push_back(t.value("name", std::string{}));
    EXPECT_NE(std::find(names.begin(), names.end(), "get_disk_log"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "add_disk_breakpoint"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "remove_disk_breakpoint"), names.end());
}

TEST(McpToolHandlerTest, ManifestListsDisplayMaskTools)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    auto find = [&](const char* name) {
        return std::find_if(manifest.begin(), manifest.end(), [name](const json& tool) {
            return tool.value("name", std::string{}) == name;
        });
    };

    auto get = find("get_display_masks");
    ASSERT_NE(get, manifest.end());
    auto set = find("set_display_masks");
    ASSERT_NE(set, manifest.end());

    const auto& props = (*set)["inputSchema"]["properties"];
    for (const char* key : {"bitplanes", "sprites", "playfields", "reset"})
        EXPECT_TRUE(props.contains(key)) << "missing argument: " << key;
    // No required arguments — every group is optional.
    EXPECT_FALSE((*set)["inputSchema"].contains("required"));

    // The descriptions must spell out the screenshot relationship and that masking is cosmetic,
    // since that is the whole point of the tools.
    for (auto it : {get, set}) {
        const std::string desc = (*it).value("description", std::string{});
        EXPECT_NE(desc.find("screenshot"), std::string::npos);
        EXPECT_NE(desc.find("cosmetic"), std::string::npos);
    }

    auto shot = find("screenshot");
    ASSERT_NE(shot, manifest.end());
    EXPECT_NE((*shot).value("description", std::string{}).find("set_display_masks"),
              std::string::npos);
}

TEST(McpToolHandlerTest, DisplayMaskToolsNeedEmulator)
{
    McpToolHandler handler(MakeHost(nullptr));
    EXPECT_TRUE(handler.Handle("get_display_masks", json::object()).contains("error"));
    EXPECT_TRUE(handler.Handle("set_display_masks", json{{"bitplanes", "none"}}).contains("error"));
}

TEST(McpToolHandlerTest, SetCpuTraceNeedsEmulator)
{
    // No emulator wired -> tool errors rather than crashing.
    McpToolHandler handler(MakeHost(nullptr));
    json r = handler.Handle("set_cpu_trace", json{{"enabled", true}});
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, ManifestListsFrameControlTools)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    std::vector<std::string> names;
    for (const auto& t : manifest)
        names.push_back(t.value("name", std::string{}));
    for (const char* expected : {"step_frame", "run_until_frame"}) {
        EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end())
            << "missing tool: " << expected;
    }
}

namespace {

// Host wired for the frame-control tools with a settable running/paused/frame state.
struct FrameControlState
{
    bool running = true;
    bool paused = true;
    uint64_t frame = 10;
    int stepped = 0;
    uint64_t runTarget = 0;
    bool runTurbo = false;
};

McpHost MakeFrameControlHost(FrameControlState* s)
{
    McpHost host;
    host.appVersion = "test-version";
    host.emulator = []() -> adm::AmigaEmulator* { return nullptr; };
    host.isRunning = [s]() { return s->running; };
    host.isPaused = [s]() { return s->paused; };
    host.getFrameCount = [s]() { return s->frame; };
    host.stepFrame = [s]() {
        ++s->stepped;
        ++s->frame;
    };
    host.runUntilFrame = [s](uint64_t target, bool turbo) {
        s->runTarget = target;
        s->runTurbo = turbo;
        s->paused = false;
        return true;
    };
    return host;
}

} // namespace

TEST(McpToolHandlerTest, StepFrameStepsCountFrames)
{
    FrameControlState s;
    McpToolHandler handler(MakeFrameControlHost(&s));
    json r = handler.Handle("step_frame", json{{"count", 3}});
    ASSERT_FALSE(r.contains("error"));
    EXPECT_EQ(s.stepped, 3);
    EXPECT_EQ(r.value("stepped", 0), 3);
    EXPECT_EQ(r.value("frameCount", 0u), 13u);
}

TEST(McpToolHandlerTest, StepSupportsConsoleParityCount)
{
    McpHost host = MakeHost(nullptr);
    int stepped = 0;
    host.stepInto = [&stepped]() { ++stepped; };
    McpToolHandler handler(std::move(host));

    json result = handler.Handle("step", json{{"count", 4}});
    ASSERT_FALSE(result.contains("error"));
    EXPECT_EQ(stepped, 4);
    EXPECT_EQ(result.value("stepped", 0), 4);
}

TEST(McpToolHandlerTest, AddBreakpointSupportsConditionsIgnoreCountAndOneShot)
{
    McpHost host = MakeHost(nullptr);
    bool called = false;
    host.addSmartBreakpoint = [&](uint32_t addr, bool condition, const std::string& reg,
                                  const std::string& oper, uint32_t value, uint32_t mask,
                                  int ignoreCount, bool oneShot) {
        called = true;
        EXPECT_EQ(addr, 0x400u);
        EXPECT_TRUE(condition);
        EXPECT_EQ(reg, "D0");
        EXPECT_EQ(oper, "==");
        EXPECT_EQ(value, 5u);
        EXPECT_EQ(mask, 0xFFu);
        EXPECT_EQ(ignoreCount, 3);
        EXPECT_TRUE(oneShot);
        return true;
    };
    McpToolHandler handler(std::move(host));

    json result = handler.Handle("add_breakpoint", json{{"addr", "$400"},
                                                        {"register", "d0"},
                                                        {"operator", "=="},
                                                        {"value", "$5"},
                                                        {"mask", "$ff"},
                                                        {"ignoreCount", 3},
                                                        {"oneShot", true}});
    ASSERT_FALSE(result.contains("error"));
    EXPECT_TRUE(called);
    EXPECT_TRUE(result.value("conditional", false));
    EXPECT_EQ(result.value("ignoreCount", 0), 3);
    EXPECT_TRUE(result.value("oneShot", false));
}

TEST(McpToolHandlerTest, StepFrameDefaultsToOne)
{
    FrameControlState s;
    McpToolHandler handler(MakeFrameControlHost(&s));
    json r = handler.Handle("step_frame", json::object());
    ASSERT_FALSE(r.contains("error"));
    EXPECT_EQ(s.stepped, 1);
}

TEST(McpToolHandlerTest, StepFrameRequiresPaused)
{
    FrameControlState s;
    s.paused = false;
    McpToolHandler handler(MakeFrameControlHost(&s));
    json r = handler.Handle("step_frame", json::object());
    EXPECT_TRUE(r.contains("error"));
    EXPECT_EQ(s.stepped, 0);
}

TEST(McpToolHandlerTest, RunUntilFrameStartsRun)
{
    FrameControlState s;
    McpToolHandler handler(MakeFrameControlHost(&s));
    json r = handler.Handle("run_until_frame", json{{"frame", 100}, {"turbo", true}});
    ASSERT_FALSE(r.contains("error"));
    EXPECT_EQ(s.runTarget, 100u);
    EXPECT_TRUE(s.runTurbo);
    EXPECT_EQ(r.value("target", 0u), 100u);
    EXPECT_TRUE(r.value("turbo", false));
}

TEST(McpToolHandlerTest, RunUntilFramePastTargetIsNotAnError)
{
    // Superseded RunUntilFrameRejectsPastTarget: a target behind the current frame satisfies
    // "run until at least frame N", so it reports alreadyPast instead of failing. See
    // RunUntilFrameTargetBehindCurrentSucceeds for the full response shape.
    FrameControlState s;
    s.frame = 100;
    McpToolHandler handler(MakeFrameControlHost(&s));
    json r = handler.Handle("run_until_frame", json{{"frame", 50}});
    EXPECT_FALSE(r.contains("error"));
    EXPECT_TRUE(r.value("alreadyPast", false));
    EXPECT_EQ(s.runTarget, 0u); // hook not invoked
}

TEST(McpToolHandlerTest, RunUntilFrameRequiresFrameArg)
{
    FrameControlState s;
    McpToolHandler handler(MakeFrameControlHost(&s));
    json r = handler.Handle("run_until_frame", json::object());
    EXPECT_TRUE(r.contains("error"));
}

TEST(McpToolHandlerTest, ManifestListsSessionControlTools)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    std::vector<std::string> names;
    for (const auto& t : manifest)
        names.push_back(t.value("name", std::string{}));
    for (const char* expected : {"start", "stop", "end_frame", "close_document"}) {
        EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end())
            << "missing tool: " << expected;
    }
}

TEST(McpToolHandlerTest, StartRejectsWhenAlreadyRunning)
{
    FrameControlState s;
    McpHost host = MakeFrameControlHost(&s);
    bool started = false;
    host.startEmulator = [&started]() { started = true; };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("start", json::object());
    EXPECT_TRUE(r.contains("error")); // running == true in the default state
    EXPECT_FALSE(started);
}

TEST(McpToolHandlerTest, StartRunsWhenStopped)
{
    FrameControlState s;
    s.running = false;
    McpHost host = MakeFrameControlHost(&s);
    bool started = false;
    host.startEmulator = [&started]() { started = true; };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("start", json::object());
    ASSERT_FALSE(r.contains("error"));
    EXPECT_TRUE(started);
}

TEST(McpToolHandlerTest, StopRequiresRunning)
{
    FrameControlState s;
    s.running = false;
    McpHost host = MakeFrameControlHost(&s);
    bool stopped = false;
    host.stopEmulator = [&stopped]() { stopped = true; };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("stop", json::object());
    EXPECT_TRUE(r.contains("error"));
    EXPECT_FALSE(stopped);
}

TEST(McpToolHandlerTest, EndFrameRequiresPaused)
{
    FrameControlState s;
    s.paused = false;
    McpHost host = MakeFrameControlHost(&s);
    bool ended = false;
    host.endFrame = [&ended]() { ended = true; };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("end_frame", json::object());
    EXPECT_TRUE(r.contains("error"));
    EXPECT_FALSE(ended);
}

TEST(McpToolHandlerTest, EndFrameCompletesFrame)
{
    FrameControlState s; // running && paused by default
    McpHost host = MakeFrameControlHost(&s);
    bool ended = false;
    host.endFrame = [&ended]() { ended = true; };
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("end_frame", json::object());
    ASSERT_FALSE(r.contains("error"));
    EXPECT_TRUE(ended);
    EXPECT_EQ(r.value("frameCount", 0u), s.frame);
}

TEST(McpToolHandlerTest, CloseDocumentValidatesIndexAndForce)
{
    McpHost host = MakeHost(nullptr);
    host.listDocuments = []() { return std::vector<std::string>{"a.adf", "b.adf"}; };
    int closedIdx = -1;
    bool forceSeen = false;
    // Refuse index 1 unless force is set (simulates unsaved changes on the second doc).
    host.closeDocument = [&](int idx, bool force) -> bool {
        forceSeen = force;
        if (idx == 1 && !force)
            return false;
        closedIdx = idx;
        return true;
    };
    McpToolHandler handler(std::move(host));

    // Out-of-range index is rejected before the hook runs.
    json bad = handler.Handle("close_document", json{{"index", 5}});
    EXPECT_TRUE(bad.contains("error"));

    // Unsaved doc without force is refused.
    json refused = handler.Handle("close_document", json{{"index", 1}});
    EXPECT_TRUE(refused.contains("error"));

    // With force it closes.
    json ok = handler.Handle("close_document", json{{"index", 1}, {"force", true}});
    ASSERT_FALSE(ok.contains("error"));
    EXPECT_EQ(ok.value("closed", -1), 1);
    EXPECT_TRUE(forceSeen);
}

// ---------------------------------------------------------------------------
// Watchpoints: range, read/write/execute access, source, value, log mode
// ---------------------------------------------------------------------------

namespace {

// Records what the handler asked the host for, so the tests can assert on the parsed
// spec rather than on emulator behaviour.
struct WatchpointHostState
{
    std::vector<adm::core::WatchpointSpec> added;
    std::vector<int> removedSlots;
    std::vector<adm::core::WatchpointInfo> list;
    std::vector<adm::core::WatchpointHit> log;
    int lastLogSlot = -99;
    int lastLogLimit = -99;
    bool lastLogClear = false;
    std::string failWith; // non-empty -> addWatchpointFull fails with this message
};

McpHost MakeWatchpointHost(WatchpointHostState* s)
{
    McpHost host = MakeHost(nullptr);
    host.addWatchpointFull = [s](const adm::core::WatchpointSpec& spec,
                                 std::string& outError) -> int {
        if (!s->failWith.empty()) {
            outError = s->failWith;
            return -1;
        }
        s->added.push_back(spec);
        return static_cast<int>(s->added.size()) - 1;
    };
    host.removeWatchpointSlot = [s](int slot) {
        s->removedSlots.push_back(slot);
        return slot >= 0 && slot < static_cast<int>(s->added.size());
    };
    host.listWatchpointsFull = [s]() { return s->list; };
    host.getWatchpointLog = [s](int slot, int limit, bool clear) {
        s->lastLogSlot = slot;
        s->lastLogLimit = limit;
        s->lastLogClear = clear;
        return s->log;
    };
    return host;
}

} // namespace

TEST(McpToolHandlerTest, AddWatchpointDefaultsToSingleByteCpuWrite)
{
    WatchpointHostState s;
    McpToolHandler handler(MakeWatchpointHost(&s));

    json r = handler.Handle("add_watchpoint", json{{"addr", 0x100}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    ASSERT_EQ(s.added.size(), 1u);
    const auto& spec = s.added[0];
    EXPECT_EQ(spec.addr, 0x100u);
    EXPECT_EQ(spec.len, 1u);
    EXPECT_TRUE(spec.write);
    EXPECT_FALSE(spec.read);
    EXPECT_FALSE(spec.exec);
    EXPECT_EQ(spec.source, "CPU");
    EXPECT_FALSE(spec.logOnly);
    EXPECT_EQ(r.value("access", std::string{}), "w");
    EXPECT_EQ(r.value("mode", std::string{}), "break");
}

TEST(McpToolHandlerTest, AddWatchpointParsesRangeAccessAndLogMode)
{
    WatchpointHostState s;
    McpToolHandler handler(MakeWatchpointHost(&s));

    // The motivating case: "who reads any byte of this region?", recorded without stopping.
    json r = handler.Handle("add_watchpoint", json{{"addr", "0x000100"},
                                                   {"len", "0x300"},
                                                   {"access", "r"},
                                                   {"mode", "log"},
                                                   {"maxHits", 500}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    ASSERT_EQ(s.added.size(), 1u);
    const auto& spec = s.added[0];
    EXPECT_EQ(spec.addr, 0x100u);
    EXPECT_EQ(spec.len, 0x300u);
    EXPECT_TRUE(spec.read);
    EXPECT_FALSE(spec.write);
    EXPECT_TRUE(spec.logOnly);
    EXPECT_EQ(spec.maxHits, 500u);
    EXPECT_EQ(r.value("mode", std::string{}), "log");
    EXPECT_EQ(r.value("len", 0u), 0x300u);
}

TEST(McpToolHandlerTest, AddWatchpointAcceptsExecuteAccess)
{
    WatchpointHostState s;
    McpToolHandler handler(MakeWatchpointHost(&s));

    json r =
        handler.Handle("add_watchpoint", json{{"addr", 0x21000}, {"len", 0x800}, {"access", "x"}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    ASSERT_EQ(s.added.size(), 1u);
    EXPECT_TRUE(s.added[0].exec);
    EXPECT_FALSE(s.added[0].read);
    EXPECT_FALSE(s.added[0].write);
    EXPECT_EQ(r.value("access", std::string{}), "x");
}

TEST(McpToolHandlerTest, AddWatchpointRejectsBadArguments)
{
    WatchpointHostState s;
    McpToolHandler handler(MakeWatchpointHost(&s));

    EXPECT_TRUE(handler.Handle("add_watchpoint", json::object()).contains("error"));
    EXPECT_TRUE(
        handler.Handle("add_watchpoint", json{{"addr", 0x100}, {"len", 0}}).contains("error"));
    EXPECT_TRUE(
        handler.Handle("add_watchpoint", json{{"addr", 0x100}, {"access", "q"}}).contains("error"));
    EXPECT_TRUE(handler.Handle("add_watchpoint", json{{"addr", 0x100}, {"source", "NOPE"}})
                    .contains("error"));
    EXPECT_TRUE(handler.Handle("add_watchpoint", json{{"addr", 0x100}, {"mode", "sometimes"}})
                    .contains("error"));
    EXPECT_TRUE(s.added.empty());
}

TEST(McpToolHandlerTest, AddWatchpointSurfacesHostError)
{
    WatchpointHostState s;
    s.failWith = "no free watchpoint slots (the core provides 20)";
    McpToolHandler handler(MakeWatchpointHost(&s));

    json r = handler.Handle("add_watchpoint", json{{"addr", 0x100}});
    ASSERT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("no free watchpoint slots"), std::string::npos);
}

TEST(McpToolHandlerTest, RemoveWatchpointBySlotAndByAddress)
{
    WatchpointHostState s;
    adm::core::WatchpointInfo info;
    info.addr = 0x100;
    info.slot = 3;
    s.list.push_back(info);
    s.added.resize(4); // make slots 0..3 "exist" for removeWatchpointSlot
    McpToolHandler handler(MakeWatchpointHost(&s));

    json bySlot = handler.Handle("remove_watchpoint", json{{"slot", 2}});
    ASSERT_FALSE(bySlot.contains("error")) << bySlot.dump();
    EXPECT_EQ(bySlot.value("slot", -1), 2);

    json byAddr = handler.Handle("remove_watchpoint", json{{"addr", 0x100}});
    ASSERT_FALSE(byAddr.contains("error")) << byAddr.dump();
    EXPECT_EQ(byAddr.value("slot", -1), 3);

    // An address with no watchpoint is an error, not a silent success.
    EXPECT_TRUE(handler.Handle("remove_watchpoint", json{{"addr", 0x200}}).contains("error"));
}

TEST(McpToolHandlerTest, ListWatchpointsReportsFullRecords)
{
    WatchpointHostState s;
    adm::core::WatchpointInfo info;
    info.addr = 0x100;
    info.len = 0x300;
    info.read = true;
    info.write = false;
    info.logOnly = true;
    info.source = "CPU";
    info.slot = 0;
    info.hits = 42;
    s.list.push_back(info);
    McpToolHandler handler(MakeWatchpointHost(&s));

    json r = handler.Handle("list_watchpoints", json::object());
    ASSERT_TRUE(r.contains("watchpoints"));
    ASSERT_EQ(r["watchpoints"].size(), 1u);
    const auto& w = r["watchpoints"][0];
    EXPECT_EQ(w.value("addr", std::string{}), "00000100");
    EXPECT_EQ(w.value("len", 0u), 0x300u);
    EXPECT_EQ(w.value("access", std::string{}), "r");
    EXPECT_EQ(w.value("mode", std::string{}), "log");
    EXPECT_EQ(w.value("hits", 0u), 42u);
}

TEST(McpToolHandlerTest, GetWatchpointLogReturnsHitsAndPassesFilters)
{
    WatchpointHostState s;
    adm::core::WatchpointHit hit;
    hit.slot = 1;
    hit.pc = 0xFC00D2;
    hit.addr = 0x104;
    hit.value = 0;
    hit.rwi = 1; // read
    hit.size = 4;
    hit.source = "CPU (read)";
    hit.frame = 77;
    s.log.push_back(hit);
    McpToolHandler handler(MakeWatchpointHost(&s));

    json r =
        handler.Handle("get_watchpoint_log", json{{"slot", 1}, {"limit", 10}, {"clear", true}});
    ASSERT_TRUE(r.contains("hits"));
    ASSERT_EQ(r["hits"].size(), 1u);
    const auto& h = r["hits"][0];
    EXPECT_EQ(h.value("pc", std::string{}), "00FC00D2");
    EXPECT_EQ(h.value("addr", std::string{}), "00000104");
    EXPECT_EQ(h.value("access", std::string{}), "r");
    EXPECT_EQ(h.value("source", std::string{}), "CPU (read)");
    EXPECT_EQ(h.value("frame", 0u), 77u);
    EXPECT_EQ(s.lastLogSlot, 1);
    EXPECT_EQ(s.lastLogLimit, 10);
    EXPECT_TRUE(s.lastLogClear);

    // Defaults: every slot, no limit, non-destructive.
    handler.Handle("get_watchpoint_log", json::object());
    EXPECT_EQ(s.lastLogSlot, -1);
    EXPECT_EQ(s.lastLogLimit, 0);
    EXPECT_FALSE(s.lastLogClear);
}

// ---------------------------------------------------------------------------
// Machine-state snapshots
// ---------------------------------------------------------------------------

TEST(McpToolHandlerTest, StateToolsReportUnavailableWithoutAStore)
{
    McpToolHandler handler(MakeHost(nullptr));
    EXPECT_TRUE(handler.Handle("list_states", json::object()).contains("error"));
    EXPECT_TRUE(handler.Handle("save_state", json{{"name", "menu"}}).contains("error"));
}

TEST(McpToolHandlerTest, StateToolsValidateNames)
{
    adm::core::EmulatorStateStore store(std::filesystem::temp_directory_path() /
                                        "peekaboo_test_states_names");
    McpHost host = MakeHost(nullptr);
    host.states = &store;
    McpToolHandler handler(std::move(host));

    // A name that would escape the states directory must be refused before any file work.
    EXPECT_TRUE(handler.Handle("save_state", json{{"name", "../evil"}}).contains("error"));
    EXPECT_TRUE(handler.Handle("save_state", json{{"name", ""}}).contains("error"));
    EXPECT_TRUE(handler.Handle("delete_state", json{{"name", "nope"}}).contains("error"));

    json list = handler.Handle("list_states", json::object());
    ASSERT_TRUE(list.contains("states"));
    EXPECT_TRUE(list["states"].empty());
}

// ---------------------------------------------------------------------------
// Input injection
// ---------------------------------------------------------------------------

TEST(McpToolHandlerTest, ManifestListsInputAndStateTools)
{
    McpToolHandler handler(MakeHost(nullptr));
    json manifest = handler.Manifest();
    std::vector<std::string> names;
    for (const auto& t : manifest)
        names.push_back(t.value("name", std::string{}));

    for (const char* expected :
         {"press_key", "type_text", "set_joystick", "save_state", "load_state", "list_states",
          "delete_state", "get_watchpoint_log"}) {
        EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end())
            << "missing tool: " << expected;
    }
}

TEST(McpToolHandlerTest, InputToolsRequireARunningEmulator)
{
    McpToolHandler handler(MakeHost(nullptr));
    EXPECT_TRUE(handler.Handle("press_key", json{{"key", "F1"}}).contains("error"));
    EXPECT_TRUE(handler.Handle("type_text", json{{"text", "dir"}}).contains("error"));
    EXPECT_TRUE(handler.Handle("set_joystick", json{{"right", true}}).contains("error"));
}

// --- run_until_frame ergonomics ---------------------------------------------

TEST(McpToolHandlerTest, RunUntilFrameTargetBehindCurrentSucceeds)
{
    // "Run until at least frame N" is already satisfied when N is behind us, so this reports
    // alreadyPast rather than an error the caller has to detect and work around.
    FrameControlState s;
    s.frame = 3577;
    McpToolHandler handler(MakeFrameControlHost(&s));

    json r = handler.Handle("run_until_frame", json{{"frame", 3500}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_TRUE(r.value("ok", false));
    EXPECT_TRUE(r.value("alreadyPast", false));
    EXPECT_EQ(r.value("current", 0u), 3577u);
    EXPECT_EQ(r.value("target", 0u), 3500u);
    EXPECT_EQ(s.runTarget, 0u) << "nothing should have been armed";
}

TEST(McpToolHandlerTest, RunUntilFrameWithoutWaitReturnsImmediately)
{
    FrameControlState s;
    s.frame = 10;
    McpToolHandler handler(MakeFrameControlHost(&s));

    json r = handler.Handle("run_until_frame", json{{"frame", 100}, {"turbo", true}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_FALSE(r.contains(adm::core::McpServer::kRetryKey));
    EXPECT_EQ(s.runTarget, 100u);
    EXPECT_TRUE(s.runTurbo);
}

TEST(McpToolHandlerTest, RunUntilFrameWaitDefersUntilTheRunEnds)
{
    FrameControlState s;
    s.frame = 10;
    McpToolHandler handler(MakeFrameControlHost(&s));
    const json args{{"frame", 13}, {"wait", true}};

    // First dispatch arms the run and asks to be called again rather than blocking the thread
    // that has to advance the emulator.
    json r = handler.Handle("run_until_frame", args);
    ASSERT_TRUE(r.value(adm::core::McpServer::kRetryKey, false)) << r.dump();
    EXPECT_EQ(s.runTarget, 13u);

    // Frames tick by while the request is parked.
    s.frame = 12;
    EXPECT_TRUE(
        handler.Handle("run_until_frame", args).value(adm::core::McpServer::kRetryKey, false));

    // Target reached; the emulator auto-pauses and the wait resolves with the final count.
    s.frame = 13;
    s.paused = true;
    r = handler.Handle("run_until_frame", args);
    ASSERT_FALSE(r.value(adm::core::McpServer::kRetryKey, false)) << r.dump();
    EXPECT_TRUE(r.value("ok", false));
    EXPECT_TRUE(r.value("reached", false));
    EXPECT_FALSE(r.value("stoppedEarly", false));
    EXPECT_EQ(r.value("current", 0u), 13u);
}

TEST(McpToolHandlerTest, RunUntilFrameWaitReportsAnEarlyBreakpointStop)
{
    FrameControlState s;
    s.frame = 10;
    McpToolHandler handler(MakeFrameControlHost(&s));
    const json args{{"frame", 500}, {"wait", true}};

    ASSERT_TRUE(
        handler.Handle("run_until_frame", args).value(adm::core::McpServer::kRetryKey, false));

    // A breakpoint got there first: paused, but short of the target.
    s.frame = 42;
    s.paused = true;
    json r = handler.Handle("run_until_frame", args);
    ASSERT_FALSE(r.value(adm::core::McpServer::kRetryKey, false)) << r.dump();
    EXPECT_TRUE(r.value("ok", false));
    EXPECT_FALSE(r.value("reached", true));
    EXPECT_TRUE(r.value("stoppedEarly", false));
    EXPECT_EQ(r.value("current", 0u), 42u);
}

TEST(McpToolHandlerTest, RunUntilFrameSchemaDocumentsWaitAndAlreadyPast)
{
    McpToolHandler handler(MakeHost(nullptr));
    for (const auto& t : handler.Manifest()) {
        if (t.value("name", std::string{}) != "run_until_frame")
            continue;
        const std::string desc = t.value("description", std::string{});
        EXPECT_NE(desc.find("wait"), std::string::npos);
        EXPECT_NE(desc.find("alreadyPast"), std::string::npos);
        EXPECT_TRUE(t["inputSchema"]["properties"].contains("wait"));
        return;
    }
    ADD_FAILURE() << "run_until_frame missing from the manifest";
}

// --- malformed address arguments --------------------------------------------

TEST(McpToolHandlerTest, MalformedAddrIsReportedAsMalformedNotMissing)
{
    adm::core::MemoryAnnotations ann;
    McpToolHandler handler(MakeHost(&ann));

    // A stray space makes this unparseable; it used to come back as "addr required", which
    // sends the caller re-reading the schema for a field they did pass.
    const std::string bad =
        handler.Handle("set_label", json{{"addr", "0x D390"}, {"text", "x"}}).value("error", "");
    EXPECT_NE(bad.find("could not parse addr"), std::string::npos) << bad;
    EXPECT_NE(bad.find("0x D390"), std::string::npos) << bad;

    const std::string missing = handler.Handle("set_label", json{{"text", "x"}}).value("error", "");
    EXPECT_EQ(missing, "addr required");
}

TEST(McpToolHandlerTest, MalformedOptionalAddrIsNotSilentlyIgnored)
{
    adm::core::MemoryAnnotations ann;
    McpToolHandler handler(MakeHost(&ann));
    const std::string err =
        handler.Handle("get_annotations", json{{"start", "nonsense!"}}).value("error", "");
    EXPECT_NE(err.find("could not parse start"), std::string::npos) << err;
}

// --- number base is stated, not implied -------------------------------------

TEST(McpToolHandlerTest, ConsoleBackedToolsStateTheirNumberBase)
{
    // The AR console parses bare numbers as hex. That is deliberate and stays, but every schema
    // that inherits it has to say so — an unprefixed decimal raster line silently lands on a
    // plausible-looking wrong value rather than erroring.
    McpToolHandler handler(MakeHost(nullptr));
    int checked = 0;
    for (const auto& t : handler.Manifest()) {
        const std::string desc = t.value("description", std::string{});
        if (desc.find("Uses the same implementation as the AR console") == std::string::npos)
            continue;
        ++checked;
        EXPECT_NE(desc.find("HEX"), std::string::npos)
            << t.value("name", std::string{}) << " does not state its number base";
    }
    EXPECT_GT(checked, 20) << "console parity tools missing from the manifest";
}

TEST(McpToolHandlerTest, BeamBreakSchemaCarriesAWorkedExample)
{
    McpToolHandler handler(MakeHost(nullptr));
    for (const auto& t : handler.Manifest()) {
        if (t.value("name", std::string{}) != "set_beam_break")
            continue;
        const std::string desc = t.value("description", std::string{});
        EXPECT_NE(desc.find("227"), std::string::npos) << desc;
        EXPECT_NE(desc.find("551"), std::string::npos) << desc;
        return;
    }
    ADD_FAILURE() << "set_beam_break missing from the manifest";
}

// --- beam position / partial frames -----------------------------------------

TEST(McpToolHandlerTest, StatusAndScreenshotSchemasDocumentTheBeamPosition)
{
    McpToolHandler handler(MakeHost(nullptr));
    int found = 0;
    for (const auto& t : handler.Manifest()) {
        const std::string name = t.value("name", std::string{});
        if (name != "get_status" && name != "screenshot")
            continue;
        ++found;
        const std::string desc = t.value("description", std::string{});
        EXPECT_NE(desc.find("beam"), std::string::npos) << name << ": " << desc;
        EXPECT_NE(desc.find("partialFrame"), std::string::npos) << name << ": " << desc;
    }
    EXPECT_EQ(found, 2);
}

// --- schema descriptions must keep up with the responses ---------------------

namespace {

std::string DescriptionOf(const json& manifest, const std::string& name)
{
    for (const auto& t : manifest)
        if (t.value("name", std::string{}) == name)
            return t.value("description", std::string{});
    return {};
}

} // namespace

TEST(McpToolHandlerTest, ToolsReturningBeamSaySoInTheirDescription)
{
    // Drift guard. get_custom_registers and get_blitter_log grew a `beam` field without their
    // descriptions mentioning it, which defeats the point of adding it: a caller reading only
    // the schema never learns the key exists.
    McpToolHandler handler(MakeHost(nullptr));
    const json manifest = handler.Manifest();
    for (const char* name :
         {"get_status", "screenshot", "get_custom_registers", "get_blitter_log"}) {
        const std::string desc = DescriptionOf(manifest, name);
        ASSERT_FALSE(desc.empty()) << name << " missing from the manifest";
        EXPECT_NE(desc.find("beam"), std::string::npos)
            << name << " returns beam but does not document it";
    }
}

TEST(McpToolHandlerTest, StatusDescriptionStatesThatBeamNeedsALoadedCore)
{
    // beam/partialFrame are omitted, not null, when no core is loaded. Undocumented that reads
    // as "not implemented" rather than "not applicable".
    McpToolHandler handler(MakeHost(nullptr));
    const std::string desc = DescriptionOf(handler.Manifest(), "get_status");
    EXPECT_NE(desc.find("partialFrame"), std::string::npos) << desc;
    EXPECT_NE(desc.find("core is loaded"), std::string::npos) << desc;
}

TEST(McpToolHandlerTest, StatusOmitsBeamKeysEntirelyWithoutACore)
{
    // The claim the description makes, asserted against the response.
    McpToolHandler handler(MakeHost(nullptr));
    const json r = handler.Handle("get_status", json::object());
    EXPECT_FALSE(r.value("coreLoaded", true));
    EXPECT_FALSE(r.contains("beam"));
    EXPECT_FALSE(r.contains("partialFrame"));
}

TEST(McpToolHandlerTest, StateSnapshotDescriptionsMatchTheRestoreGranularity)
{
    // UAE's state format reads VPOSR/VHPOSR back and discards them, and retro_unserialize runs
    // whole frames until the restore settles, so a restore lands on a frame boundary. The
    // descriptions used to promise the "exact PC/cycle", which turns an ordinary mid-frame
    // chipset difference across a round trip into a plausible-looking restore bug.
    McpToolHandler handler(MakeHost(nullptr));
    const json manifest = handler.Manifest();

    const std::string load = DescriptionOf(manifest, "load_state");
    ASSERT_FALSE(load.empty());
    EXPECT_EQ(load.find("exact PC/cycle"), std::string::npos) << load;
    EXPECT_NE(load.find("frame-granular"), std::string::npos) << load;
    // The two surprises a caller trips over after a restore.
    EXPECT_NE(load.find("frameCount"), std::string::npos) << load;
    EXPECT_NE(load.find("run/pause state"), std::string::npos) << load;

    const std::string save = DescriptionOf(manifest, "save_state");
    ASSERT_FALSE(save.empty());
    EXPECT_NE(save.find("frame-granular"), std::string::npos) << save;
}

// ---------------------------------------------------------------------------
// Regressions for the seven MCP contract bugs fixed together (joystick ports,
// stale media, key-press collapse, disk-log ordering, frame-count rebasing,
// breakpoint reporting, CIA reads).
// ---------------------------------------------------------------------------

// The tool used to call the Amiga's port 1 "port 0" and refuse port 1 outright, so
// anyone reasoning in Amiga terms concluded joystick input was unavailable.
TEST(McpToolHandlerTest, SetJoystickDescribesAmigaPortNumbering)
{
    McpToolHandler handler(MakeHost(nullptr));
    std::string desc;
    json schema;
    for (const auto& t : handler.Manifest()) {
        if (t.value("name", std::string{}) == "set_joystick") {
            desc = t.value("description", std::string{});
            schema = t.value("inputSchema", json::object());
        }
    }
    ASSERT_FALSE(desc.empty());

    // The mapping a caller needs must be in the description, both directions.
    EXPECT_NE(desc.find("JOY1DAT"), std::string::npos);
    EXPECT_NE(desc.find("JOY0DAT"), std::string::npos);
    EXPECT_NE(desc.find("bit 7"), std::string::npos);
    EXPECT_NE(desc.find("bit 6"), std::string::npos);
    // The old refusal must not survive in the text.
    EXPECT_EQ(desc.find("only port 0 is wired"), std::string::npos);
    EXPECT_EQ(desc.find("only 0 is wired"), std::string::npos);

    // Default is Amiga port 1, so callers that omit `port` are unaffected.
    ASSERT_TRUE(schema.contains("properties"));
    ASSERT_TRUE(schema["properties"].contains("port"));
    EXPECT_EQ(schema["properties"]["port"].value("default", -1), 1);
}

// Rejecting a bad port must stay an honest refusal, never a silent no-op.
TEST(McpToolHandlerTest, SetJoystickRejectsOutOfRangePort)
{
    McpHost host = MakeHost(nullptr);
    McpToolHandler handler(std::move(host));
    // No emulator wired, so this reports the emulator error rather than succeeding;
    // the point is that it never returns ok:true for input it did not deliver.
    json r = handler.Handle("set_joystick", json{{"port", 2}, {"fire", true}});
    EXPECT_TRUE(r.contains("error"));
    EXPECT_FALSE(r.value("ok", false));
}

// load_document silently served the previous build of a rebuilt ADF because an
// already-open path short-circuited to a focus change. It must now always re-read.
TEST(McpToolHandlerTest, LoadDocumentReReadsFromDiskByDefault)
{
    McpHost host = MakeHost(nullptr);
    bool sawReload = false;
    std::string sawPath;
    host.loadDocument = [&](const std::string& path, bool, bool reloadIfOpen) {
        sawPath = path;
        sawReload = reloadIfOpen;
        return true;
    };
    McpToolHandler handler(std::move(host));

    json r = handler.Handle("load_document", json{{"path", "game.adf"}});
    ASSERT_FALSE(r.contains("error"));
    EXPECT_TRUE(sawReload) << "load_document must re-read the file by default";
    EXPECT_EQ(sawPath, "game.adf");
    EXPECT_TRUE(r.value("reloadedFromDisk", false));

    // Opting out is possible but must be explicit, and is flagged in the reply.
    r = handler.Handle("load_document", json{{"path", "game.adf"}, {"reload", false}});
    ASSERT_FALSE(r.contains("error"));
    EXPECT_FALSE(sawReload);
    EXPECT_TRUE(r.contains("note"));
}

// A browser page has the image but no file to point load_document at: the bytes go to a file
// of that name in Peekaboo's temp folder and are loaded from there, re-read every time.
TEST(McpToolHandlerTest, LoadDocumentBytesWritesTheImageAndLoadsIt)
{
    McpHost host = MakeHost(nullptr);
    std::string sawPath;
    bool sawReload = false;
    host.loadDocument = [&](const std::string& path, bool, bool reloadIfOpen) {
        sawPath = path;
        sawReload = reloadIfOpen;
        return true;
    };
    McpToolHandler handler(std::move(host));

    const std::vector<uint8_t> image = {'D', 'O', 'S', 0, 0xFF, 0x80, 0x01};
    json r = handler.Handle("load_document_bytes", json{{"name", "sync_test.adf"},
                                                        {"data", adm::core::Base64Encode(image)}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_TRUE(r.value("ok", false));
    EXPECT_EQ(r.value("bytes", 0), 7);
    EXPECT_EQ(sawPath, r.value("path", std::string{}));
    EXPECT_TRUE(sawReload);
    EXPECT_EQ(std::filesystem::path(sawPath).filename().string(), "sync_test.adf");
    std::ifstream in(sawPath, std::ios::binary);
    std::vector<uint8_t> back((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(back, image);
    in.close();
    std::filesystem::remove(sawPath);
}

TEST(McpToolHandlerTest, LoadDocumentBytesRefusesPathsAndBadData)
{
    McpHost host = MakeHost(nullptr);
    int loads = 0;
    host.loadDocument = [&](const std::string&, bool, bool) {
        loads++;
        return true;
    };
    McpToolHandler handler(std::move(host));
    const std::string data = adm::core::Base64Encode({1, 2, 3});
    for (const char* bad : {"../escape.adf", "dir/x.adf", "C:x.adf", ".hidden.adf", "noext", ""})
        EXPECT_TRUE(handler.Handle("load_document_bytes", json{{"name", bad}, {"data", data}})
                        .contains("error"))
            << bad;
    EXPECT_TRUE(handler.Handle("load_document_bytes", json{{"name", "a.adf"}, {"data", "!!"}})
                    .contains("error"));
    EXPECT_TRUE(handler.Handle("load_document_bytes", json{{"name", "a.adf"}}).contains("error"));
    EXPECT_EQ(loads, 0);
}

TEST(McpToolHandlerTest, Base64DecodeInvertsEncode)
{
    for (size_t n = 0; n < 10; n++) {
        std::vector<uint8_t> v;
        for (size_t i = 0; i < n; i++)
            v.push_back(static_cast<uint8_t>(i * 37 + 250));
        auto back = adm::core::Base64Decode(adm::core::Base64Encode(v));
        ASSERT_TRUE(back.has_value()) << n;
        EXPECT_EQ(*back, v) << n;
    }
    EXPECT_EQ(*adm::core::Base64Decode("TWFu\nTWE="),
              (std::vector<uint8_t>{'M', 'a', 'n', 'M', 'a'}));
    EXPECT_FALSE(adm::core::Base64Decode("TW*u").has_value());
    EXPECT_FALSE(adm::core::Base64Decode("TWFuT").has_value());
    EXPECT_FALSE(adm::core::Base64Decode("TQ==TQ==").has_value());
}

// The array came back ascending while the description promised most-recent-first,
// and the summary fields never said which scope they described.
TEST(McpToolHandlerTest, DiskLogReportsOrderSessionAndFieldScope)
{
    McpHost host = MakeHost(nullptr);
    adm::core::DiskLogQuery seen;
    host.getDiskLog = [&seen](const adm::core::DiskLogQuery& q) {
        seen = q;
        adm::core::DiskLogSnapshot snap;
        snap.available = true;
        snap.matched = 2;        // filter-scoped
        snap.totalTransfers = 9; // whole-log
        snap.reads = 9;
        snap.currentSession = 3;
        snap.sessionCount = 2;
        snap.order = "newestFirst";

        adm::core::DiskLogEntry e;
        e.seq = 4;
        e.session = 3;
        e.ordinal = 41;
        e.direction = "read";
        snap.transfers.push_back(e);
        return snap;
    };
    McpToolHandler handler(std::move(host));

    json r = handler.Handle("get_disk_log", json::object());
    ASSERT_FALSE(r.contains("error")) << r.dump(2);
    // Default order matches what the description claims.
    EXPECT_TRUE(seen.newestFirst);
    EXPECT_EQ(r.value("order", std::string{}), "newestFirst");

    // The two scopes are separately named, so 252-vs-342 is no longer a mystery.
    EXPECT_EQ(r.value("matched", -1), 2);
    EXPECT_EQ(r.value("returned", -1), 1);
    EXPECT_EQ(r.value("totalTransfers", -1), 9);
    ASSERT_TRUE(r.contains("scope"));
    EXPECT_EQ(r["scope"].value("matched", std::string{}), "filter");
    EXPECT_EQ(r["scope"].value("totalTransfers", std::string{}), "log");

    // Session boundaries are explicit, and each entry is orderable across them.
    EXPECT_EQ(r.value("currentSession", -1), 3);
    EXPECT_EQ(r.value("sessionCount", -1), 2);
    EXPECT_TRUE(r.contains("note")) << "a multi-session log must say so";
    EXPECT_EQ(r["transfers"][0].value("session", -1), 3);
    EXPECT_EQ(r["transfers"][0].value("ordinal", -1), 41);
}

TEST(McpToolHandlerTest, DiskLogAcceptsOrderAndOrdinalFilters)
{
    McpHost host = MakeHost(nullptr);
    adm::core::DiskLogQuery seen;
    host.getDiskLog = [&seen](const adm::core::DiskLogQuery& q) {
        seen = q;
        adm::core::DiskLogSnapshot snap;
        snap.available = true;
        return snap;
    };
    McpToolHandler handler(std::move(host));

    json r = handler.Handle("get_disk_log", json{{"order", "oldestFirst"}, {"sinceOrdinal", 100}});
    ASSERT_FALSE(r.contains("error"));
    EXPECT_FALSE(seen.newestFirst);
    EXPECT_EQ(seen.sinceOrdinal, 100u);

    EXPECT_TRUE(handler.Handle("get_disk_log", json{{"order", "sideways"}}).contains("error"));
}

// ok:false meant "already set" but read as "the core refused it".
TEST(McpToolHandlerTest, AddBreakpointReportsArmedStateAndDuplicateReason)
{
    McpHost host = MakeHost(nullptr);
    bool running = false;
    int calls = 0;
    host.isRunning = [&running]() { return running; };
    host.addBreakpoint = [&calls](uint32_t) { return ++calls == 1; };
    McpToolHandler handler(std::move(host));

    json r = handler.Handle("add_breakpoint", json{{"addr", "30000"}});
    ASSERT_FALSE(r.contains("error"));
    EXPECT_TRUE(r.value("ok", false));
    EXPECT_EQ(r.value("addr", std::string{}), "00030000");
    // Not running: the breakpoint is held host-side and injected on start. Saying so
    // is the difference between "it will fire" and "it silently did nothing".
    EXPECT_FALSE(r.value("armed", true));
    EXPECT_TRUE(r.contains("note"));

    r = handler.Handle("add_breakpoint", json{{"addr", "30000"}});
    EXPECT_FALSE(r.value("ok", true));
    EXPECT_NE(r.value("reason", std::string{}).find("already exists"), std::string::npos);
}

TEST(McpToolHandlerTest, ExtendedCpuRegistersCarryOnlyWhatTheCpuHas)
{
    adm::core::CpuExtendedState ext;
    ext.cpuModel = 68000;
    ext.supervisor = true;
    ext.usp = 0x1000;
    ext.isp = 0xC7F000;

    const json control = adm::core::BuildExtendedCpuRegistersJson(ext, "control");
    EXPECT_EQ(control["stack"]["USP"], "00001000");
    EXPECT_EQ(control["stack"]["SSP"], "00C7F000");
    EXPECT_EQ(control["activeStack"], "SSP");
    EXPECT_FALSE(control.contains("control"));
    EXPECT_FALSE(control.contains("fpu"));
    bool vbrListed = false;
    for (const auto& u : control["unsupported"])
        if (u["name"] == "VBR") {
            vbrListed = true;
            EXPECT_EQ(u["supportedOn"], "68010+");
        }
    EXPECT_TRUE(vbrListed);

    ext.cpuModel = 68040;
    ext.fpuModel = 68040;
    ext.fpuPresent = true;
    ext.fp[0].ext[0] = 0x3FFF0000;
    ext.fp[0].ext[1] = 0x80000000;
    ext.fp[0].text = "#1.000000e+00";
    const json fpu = adm::core::BuildExtendedCpuRegistersJson(ext, "fpu");
    EXPECT_EQ(fpu["fpu"]["FP0"]["hex"], "3FFF-80000000-00000000");
    EXPECT_EQ(fpu["fpu"]["FP0"]["value"], "1.000000e+00");
    EXPECT_EQ(fpu["fpu"]["FPCR"]["decoded"], "prec X round RN enable -");
    EXPECT_EQ(fpu["cpu"]["fpu"], "68040 internal");
}
