#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <string>
#include <vector>

#include "../src/core/McpReTools.h"
#include "../src/core/McpToolHandler.h"

using adm::core::McpHost;
using adm::core::McpReToolNames;
using adm::core::McpToolHandler;
using nlohmann::json;

namespace ex = adm::core::exec;

namespace {

// --- fixtures ---------------------------------------------------------------

// Backing store for the "emulated RAM" the snapshot tools capture out of.
struct FakeRam
{
    std::vector<uint8_t> chip = std::vector<uint8_t>(1024, 0);

    std::vector<adm::core::LiveMemoryRegion> Live()
    {
        return {{0x1000, static_cast<uint32_t>(chip.size()), chip.data(), "Chip RAM", true}};
    }
    std::vector<adm::core::AddressRegion> Regions()
    {
        return {{0x1000, static_cast<uint32_t>(chip.size()), "Chip RAM", true},
                {0xF80000, 0x80000, "Kickstart ROM", false}};
    }
};

// A sparse image with just enough Exec structure for the AmigaOS-shaped tools.
class FakeMem
{
public:
    void PutByte(uint32_t a, uint8_t v) { m_bytes[a] = v; }
    void PutWord(uint32_t a, uint16_t v)
    {
        PutByte(a, static_cast<uint8_t>(v >> 8));
        PutByte(a + 1, static_cast<uint8_t>(v));
    }
    void PutLong(uint32_t a, uint32_t v)
    {
        PutWord(a, static_cast<uint16_t>(v >> 16));
        PutWord(a + 2, static_cast<uint16_t>(v));
    }
    void PutCString(uint32_t a, const std::string& s)
    {
        for (size_t i = 0; i < s.size(); ++i)
            PutByte(a + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
        PutByte(a + static_cast<uint32_t>(s.size()), 0);
    }
    void PutEmptyList(uint32_t a)
    {
        PutLong(a, a + 4);
        PutLong(a + 4, 0);
    }
    void PutList(uint32_t listAddr, const std::vector<uint32_t>& nodes)
    {
        PutLong(listAddr, nodes.empty() ? listAddr + 4 : nodes.front());
        for (size_t i = 0; i + 1 < nodes.size(); ++i)
            PutLong(nodes[i], nodes[i + 1]);
        if (!nodes.empty())
            PutLong(nodes.back(), listAddr + 4);
        PutLong(listAddr + 4, 0);
    }

    ex::Readers Readers() const
    {
        ex::Readers r;
        r.readLong = [this](uint32_t a, bool& ok) {
            uint32_t v = 0;
            ok = true;
            for (int i = 0; i < 4; ++i) {
                auto it = m_bytes.find(a + static_cast<uint32_t>(i));
                if (it == m_bytes.end()) {
                    ok = false;
                    return 0u;
                }
                v = (v << 8) | it->second;
            }
            return v;
        };
        r.readByte = [this](uint32_t a, bool& ok) -> uint8_t {
            auto it = m_bytes.find(a);
            ok = it != m_bytes.end();
            return ok ? it->second : uint8_t{0};
        };
        r.isMapped = [this](uint32_t a) { return m_bytes.count(a) != 0; };
        return r;
    }

private:
    std::map<uint32_t, uint8_t> m_bytes;
};

constexpr uint32_t kExecBase = 0x00C00276;
constexpr uint32_t kDosBase = 0x00C10000;

FakeMem BuildBootedMachine()
{
    FakeMem m;
    m.PutLong(4, kExecBase);
    m.PutByte(kExecBase, 0);
    for (uint32_t off : {ex::EB_MEMLIST, ex::EB_RESOURCELIST, ex::EB_DEVICELIST, ex::EB_INTRLIST,
                         ex::EB_LIBLIST, ex::EB_PORTLIST, ex::EB_TASKREADY, ex::EB_TASKWAIT})
        m.PutEmptyList(kExecBase + off);
    for (uint32_t off :
         {ex::EB_COLDCAPTURE, ex::EB_COOLCAPTURE, ex::EB_WARMCAPTURE, ex::EB_KICKMEMPTR,
          ex::EB_KICKTAGPTR, ex::EB_KICKCHECKSUM, ex::EB_RESMODULES, ex::EB_THISTASK})
        m.PutLong(kExecBase + off, 0);

    m.PutList(kExecBase + ex::EB_LIBLIST, {kDosBase});
    m.PutWord(kDosBase + ex::LIB_VERSION, 39);
    m.PutWord(kDosBase + ex::LIB_OPENCNT, 1);
    m.PutLong(kDosBase + ex::LN_NAME, 0x00C10900);
    m.PutCString(0x00C10900, "dos.library");
    return m;
}

// A host with no emulator wired, so unavailability paths are the default.
McpHost BareHost()
{
    McpHost host;
    host.appVersion = "test-version";
    host.emulator = []() -> adm::AmigaEmulator* { return nullptr; };
    return host;
}

json Manifest()
{
    json tools = json::array();
    adm::core::AppendMcpReToolManifest(tools);
    return tools;
}

bool ToolIn(const json& manifest, const std::string& name)
{
    return std::any_of(manifest.begin(), manifest.end(),
                       [&](const json& t) { return t.value("name", std::string{}) == name; });
}

} // namespace

// --- manifest ----------------------------------------------------------------

TEST(McpReToolsTest, ManifestAndDispatcherCoverTheSameNames)
{
    const json manifest = Manifest();
    ASSERT_EQ(manifest.size(), McpReToolNames().size())
        << "a tool was added to one of the manifest/name lists but not the other";

    for (const auto& name : McpReToolNames())
        EXPECT_TRUE(ToolIn(manifest, name)) << "missing from the manifest: " << name;

    // Every declared tool must actually dispatch — a name in the manifest with
    // no handler would surface to the client as an unknown tool.
    McpHost host = BareHost();
    for (const auto& name : McpReToolNames())
        EXPECT_TRUE(adm::core::DispatchMcpReTool(name, json::object(), host).has_value())
            << "declared but not dispatched: " << name;
}

TEST(McpReToolsTest, UnknownNamesAreNotClaimed)
{
    McpHost host = BareHost();
    EXPECT_FALSE(adm::core::DispatchMcpReTool("get_registers", json::object(), host).has_value());
    EXPECT_FALSE(adm::core::DispatchMcpReTool("", json::object(), host).has_value());
}

TEST(McpReToolsTest, EveryToolHasANameDescriptionAndSchema)
{
    for (const auto& t : Manifest()) {
        ASSERT_TRUE(t.contains("name"));
        const auto name = t["name"].get<std::string>();
        EXPECT_TRUE(t.contains("description")) << name;
        EXPECT_FALSE(t.value("description", std::string{}).empty()) << name;
        ASSERT_TRUE(t.contains("inputSchema")) << name;
        EXPECT_EQ(t["inputSchema"].value("type", std::string{}), "object") << name;
    }
}

TEST(McpReToolsTest, ToolsAppearInTheFullHandlerManifest)
{
    McpToolHandler handler(BareHost());
    const json manifest = handler.Manifest();
    for (const auto& name : McpReToolNames())
        EXPECT_TRUE(ToolIn(manifest, name)) << "missing from the handler manifest: " << name;
}

TEST(McpReToolsTest, ToolNamesAreUniqueAcrossTheWholeManifest)
{
    McpToolHandler handler(BareHost());
    std::vector<std::string> names;
    for (const auto& t : handler.Manifest())
        names.push_back(t.value("name", std::string{}));
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end())
        << "duplicate tool name in the manifest";
}

// --- unavailability ----------------------------------------------------------

TEST(McpReToolsTest, EveryToolDegradesGracefullyWithNoEmulator)
{
    McpHost host = BareHost();
    for (const auto& name : McpReToolNames()) {
        auto r = adm::core::DispatchMcpReTool(name, json::object(), host);
        ASSERT_TRUE(r.has_value()) << name;
        // Either a clear error about the missing argument, or an availability
        // report with a hint — never a silent empty success.
        const bool ok =
            r->contains("error") ||
            (r->contains("available") && !r->value("available", true) && r->contains("hint"));
        EXPECT_TRUE(ok) << name << " -> " << r->dump();
    }
}

// --- memory snapshots --------------------------------------------------------

TEST(McpReToolsTest, SnapshotAndDiffRoundTripThroughTheToolLayer)
{
    FakeRam ram;
    adm::core::MemorySnapshotStore store;
    McpHost host = BareHost();
    host.memorySnapshots = &store;
    host.liveMemoryRegions = [&ram]() { return ram.Live(); };
    host.getFrameCount = []() -> uint64_t { return 42; };
    McpToolHandler handler(std::move(host));

    json a = handler.Handle("snapshot_memory", json{{"name", "before"}});
    ASSERT_FALSE(a.contains("error")) << a.dump();
    EXPECT_EQ(a.value("frame", 0), 42);
    EXPECT_EQ(a.value("bytes", 0), 1024);

    ram.chip[0x10] = 0xAA;
    ram.chip[0x11] = 0xBB;
    ASSERT_FALSE(handler.Handle("snapshot_memory", json{{"name", "after"}}).contains("error"));

    json d = handler.Handle("diff_snapshots", json{{"a", "before"}, {"b", "after"}});
    ASSERT_FALSE(d.contains("error")) << d.dump();
    EXPECT_TRUE(d.value("available", false));
    EXPECT_EQ(d.value("totalRuns", 0), 1);
    ASSERT_EQ(d["runs"].size(), 1u);
    EXPECT_EQ(d["runs"][0].value("addr", std::string{}), "$00001010");
    EXPECT_EQ(d["runs"][0].value("len", 0), 2);
    EXPECT_EQ(d["runs"][0].value("after", std::string{}), "AABB");

    json list = handler.Handle("list_memory_snapshots", json::object());
    EXPECT_EQ(list.value("count", 0), 2);

    EXPECT_FALSE(
        handler.Handle("delete_memory_snapshot", json{{"name", "before"}}).contains("error"));
    EXPECT_TRUE(
        handler.Handle("delete_memory_snapshot", json{{"name", "before"}}).contains("error"));
}

TEST(McpReToolsTest, SnapshotRejectsMalformedRangesAndDiffRejectsBadModes)
{
    FakeRam ram;
    adm::core::MemorySnapshotStore store;
    McpHost host = BareHost();
    host.memorySnapshots = &store;
    host.liveMemoryRegions = [&ram]() { return ram.Live(); };
    McpToolHandler handler(std::move(host));

    EXPECT_TRUE(handler.Handle("snapshot_memory", json{{"name", "x"}, {"ranges", "nope"}})
                    .contains("error"));
    EXPECT_TRUE(handler
                    .Handle("snapshot_memory",
                            json{{"name", "x"}, {"ranges", json::array({json::object()})}})
                    .contains("error"));
    EXPECT_TRUE(handler.Handle("snapshot_memory", json{{"name", "bad name"}}).contains("error"));

    ASSERT_FALSE(handler.Handle("snapshot_memory", json{{"name", "a"}}).contains("error"));
    EXPECT_TRUE(handler.Handle("diff_snapshots", json{{"a", "a"}, {"b", "a"}, {"mode", "weird"}})
                    .contains("error"));
    EXPECT_TRUE(
        handler.Handle("diff_snapshots", json{{"a", "a"}, {"b", "ghost"}}).contains("error"));
}

TEST(McpReToolsTest, SnapshotAcceptsExplicitRangesAsHexStrings)
{
    FakeRam ram;
    adm::core::MemorySnapshotStore store;
    McpHost host = BareHost();
    host.memorySnapshots = &store;
    host.liveMemoryRegions = [&ram]() { return ram.Live(); };
    McpToolHandler handler(std::move(host));

    json r = handler.Handle("snapshot_memory",
                            json{{"name", "vectors"},
                                 {"ranges", json::array({{{"addr", "$1000"}, {"len", "0x100"}}})}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_EQ(r.value("bytes", 0), 0x100);
}

// --- describe_address --------------------------------------------------------

TEST(McpReToolsTest, DescribeAddressReportsLayers)
{
    FakeRam ram;
    FakeMem mem = BuildBootedMachine();
    McpHost host = BareHost();
    host.execReaders = [&mem]() { return mem.Readers(); };
    host.memoryRegions = [&ram]() { return ram.Regions(); };
    host.customRegName = [](uint32_t off) { return off == 0x180 ? "COLOR00" : ""; };
    McpToolHandler handler(std::move(host));

    json r = handler.Handle("describe_address", json{{"addr", "$1010"}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_TRUE(r.value("mapped", false));
    EXPECT_EQ(r.value("addr", std::string{}), "$00001010");
    ASSERT_TRUE(r.contains("layers"));
    ASSERT_GE(r["layers"].size(), 1u);
    EXPECT_EQ(r["layers"][0].value("kind", std::string{}), "region");
    EXPECT_EQ(r["layers"][0].value("name", std::string{}), "Chip RAM");
}

TEST(McpReToolsTest, DescribeAddressNeedsAnAddress)
{
    FakeRam ram;
    FakeMem mem;
    McpHost host = BareHost();
    host.execReaders = [&mem]() { return mem.Readers(); };
    host.memoryRegions = [&ram]() { return ram.Regions(); };
    McpToolHandler handler(std::move(host));
    EXPECT_TRUE(handler.Handle("describe_address", json::object()).contains("error"));
    EXPECT_TRUE(handler.Handle("describe_address", json{{"addr", "zz"}}).contains("error"));
}

// --- residents ---------------------------------------------------------------

TEST(McpReToolsTest, ResidentToolsReportABootedMachine)
{
    FakeRam ram;
    FakeMem mem = BuildBootedMachine();
    McpHost host = BareHost();
    host.execReaders = [&mem]() { return mem.Readers(); };
    host.memoryRegions = [&ram]() { return ram.Regions(); };
    McpToolHandler handler(std::move(host));

    json mods = handler.Handle("list_resident_modules", json::object());
    ASSERT_FALSE(mods.contains("error")) << mods.dump();
    EXPECT_TRUE(mods.value("available", false));
    EXPECT_EQ(mods.value("count", -1), 0); // ResModules is null in this fixture

    json surv = handler.Handle("get_reboot_survivors", json::object());
    ASSERT_FALSE(surv.contains("error")) << surv.dump();
    EXPECT_TRUE(surv.value("available", false));
    EXPECT_FALSE(surv.value("anySurvivors", true));
    ASSERT_EQ(surv["captures"].size(), 3u);
    EXPECT_EQ(surv["captures"][0].value("name", std::string{}), "ColdCapture");
}

TEST(McpReToolsTest, ResidentToolsSayWhenNoOsIsBooted)
{
    FakeRam ram;
    FakeMem mem; // nothing at $4
    McpHost host = BareHost();
    host.execReaders = [&mem]() { return mem.Readers(); };
    host.memoryRegions = [&ram]() { return ram.Regions(); };
    McpToolHandler handler(std::move(host));

    for (const char* name : {"list_resident_modules", "get_reboot_survivors"}) {
        json r = handler.Handle(name, json::object());
        EXPECT_FALSE(r.value("available", true)) << name;
        EXPECT_NE(r.value("hint", std::string{}).find("ExecBase"), std::string::npos) << name;
    }
}

// --- CPU trace ---------------------------------------------------------------

TEST(McpReToolsTest, TraceHistoryIsOffUntilConfigured)
{
    adm::core::CpuTraceHistory history;
    McpHost host = BareHost();
    host.cpuTraceHistory = &history;
    McpToolHandler handler(std::move(host));

    json r = handler.Handle("get_cpu_trace", json::object());
    EXPECT_FALSE(r.value("available", true));
    EXPECT_FALSE(r.value("hint", std::string{}).empty());

    json cfg = handler.Handle("configure_cpu_trace_history",
                              json{{"enabled", true}, {"maxRecords", 2000}});
    ASSERT_FALSE(cfg.contains("error")) << cfg.dump();
    EXPECT_TRUE(cfg.value("enabled", false));
    EXPECT_EQ(cfg.value("maxRecords", 0), 2000);
    EXPECT_TRUE(history.IsEnabled());

    EXPECT_TRUE(
        handler.Handle("configure_cpu_trace_history", json{{"maxRecords", 10}}).contains("error"));
}

TEST(McpReToolsTest, FindInTraceValidatesItsArguments)
{
    adm::core::CpuTraceHistory history;
    history.SetEnabled(true);
    adm::AmigaEmulator::CpuTraceEntry e{};
    e.pc = 0x1000;
    e.raw[0] = 0x33;
    e.raw[1] = 0xC0;
    e.raw[2] = 0x00;
    e.raw[3] = 0x01;
    e.raw[4] = 0x23;
    e.raw[5] = 0x40; // MOVE.W D0,$00012340
    e.raw_len = 6;
    e.absolute_cycle = 100;
    history.AppendFrame(&e, 1, 1, 0);

    McpHost host = BareHost();
    host.cpuTraceHistory = &history;
    McpToolHandler handler(std::move(host));

    EXPECT_TRUE(handler.Handle("find_in_trace", json{{"mode", "nope"}}).contains("error"));
    EXPECT_TRUE(handler.Handle("find_in_trace", json{{"mode", "lastWriter"}}).contains("error"));
    EXPECT_TRUE(handler
                    .Handle("find_in_trace", json{{"mode", "accesses"},
                                                  {"addr", "$12340"},
                                                  {"readsOnly", true},
                                                  {"writesOnly", true}})
                    .contains("error"));
    EXPECT_TRUE(handler.Handle("find_in_trace", json{{"pcLo", "$1000"}}).contains("error"));
    EXPECT_TRUE(handler.Handle("find_in_trace", json{{"pcLo", "$2000"}, {"pcHi", "$1000"}})
                    .contains("error"));
    EXPECT_TRUE(handler.Handle("find_in_trace", json{{"reg", "d0"}}).contains("error"));

    json r = handler.Handle("find_in_trace",
                            json{{"mode", "lastWriter"}, {"addr", "$12340"}, {"len", 2}});
    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_TRUE(r.value("available", false));
    EXPECT_EQ(r.value("mode", std::string{}), "lastWriter");
    ASSERT_EQ(r["matches"].size(), 1u);
    EXPECT_EQ(r["matches"][0].value("pc", std::string{}), "$00001000");
    EXPECT_TRUE(r["matches"][0]["access"].value("write", false));
}

TEST(McpReToolsTest, GetCpuTraceIncludesRegistersOnRequest)
{
    adm::core::CpuTraceHistory history;
    history.SetEnabled(true);
    adm::AmigaEmulator::CpuTraceEntry e{};
    e.pc = 0x1000;
    e.raw[0] = 0x4E;
    e.raw[1] = 0x71;
    e.raw_len = 2;
    e.d[0] = 0xDEADBEEF;
    history.AppendFrame(&e, 1, 1, 0);

    McpHost host = BareHost();
    host.cpuTraceHistory = &history;
    McpToolHandler handler(std::move(host));

    json plain = handler.Handle("get_cpu_trace", json::object());
    ASSERT_EQ(plain["matches"].size(), 1u);
    EXPECT_FALSE(plain["matches"][0].contains("d"));
    EXPECT_EQ(plain["matches"][0].value("bytes", std::string{}), "4E71");

    json withRegs = handler.Handle("get_cpu_trace", json{{"includeRegs", true}});
    ASSERT_EQ(withRegs["matches"].size(), 1u);
    ASSERT_TRUE(withRegs["matches"][0].contains("d"));
    EXPECT_EQ(withRegs["matches"][0]["d"][0].get<std::string>(), "$DEADBEEF");
}

// --- library calls / DOS breakpoints ----------------------------------------

TEST(McpReToolsTest, DosBreakpointsRequireAResolvableFunction)
{
    FakeMem mem = BuildBootedMachine();
    adm::core::DosBreakpoints bps;
    adm::core::LibraryCallTracer tracer;
    McpHost host = BareHost();
    host.dosBreakpoints = &bps;
    host.libraryTracer = &tracer;
    host.execReaders = [&mem]() { return mem.Readers(); };
    host.addBreakpoint = [](uint32_t) { return true; };
    McpToolHandler handler(std::move(host));

    EXPECT_TRUE(handler.Handle("add_dos_breakpoint", json::object()).contains("error"));
    // No LVO tables are loaded, so nothing resolves — and the error has to say
    // so rather than silently setting a breakpoint at zero.
    json r = handler.Handle("add_dos_breakpoint", json{{"function", "Open"}});
    ASSERT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("Open"), std::string::npos);
    EXPECT_TRUE(bps.Empty());
}

TEST(McpReToolsTest, DosBreakpointLifecycleThroughTheToolLayer)
{
    FakeMem mem = BuildBootedMachine();
    adm::core::DosBreakpoints bps;
    adm::core::LibraryCallTracer tracer;
    ASSERT_TRUE(tracer.LoadArgTables("assets/amiga_lvo_args.json") ||
                tracer.LoadArgTables("../assets/amiga_lvo_args.json") ||
                tracer.LoadArgTables("../../assets/amiga_lvo_args.json"))
        << "assets/amiga_lvo_args.json not reachable from the test working directory";

    std::vector<uint32_t> added, removed;
    McpHost host = BareHost();
    host.dosBreakpoints = &bps;
    host.libraryTracer = &tracer;
    host.execReaders = [&mem]() { return mem.Readers(); };
    host.addBreakpoint = [&added](uint32_t a) {
        added.push_back(a);
        return true;
    };
    host.removeBreakpoint = [&removed](uint32_t a) {
        removed.push_back(a);
        return true;
    };
    McpToolHandler handler(std::move(host));

    json add =
        handler.Handle("add_dos_breakpoint", json{{"function", "Open"}, {"nameMatch", "loader"}});
    ASSERT_FALSE(add.contains("error")) << add.dump();
    EXPECT_EQ(add.value("library", std::string{}), "dos.library");
    EXPECT_EQ(add.value("lvo", 0), 30);
    EXPECT_EQ(add.value("argReg", std::string{}), "d1");
    ASSERT_EQ(added.size(), 1u);
    EXPECT_EQ(added[0], kDosBase - 30);

    json list = handler.Handle("list_dos_breakpoints", json::object());
    EXPECT_EQ(list.value("count", 0), 1);

    const int id = add.value("id", 0);
    ASSERT_NE(id, 0);
    json rm = handler.Handle("remove_dos_breakpoint", json{{"id", id}});
    ASSERT_FALSE(rm.contains("error")) << rm.dump();
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0], kDosBase - 30);
    EXPECT_TRUE(handler.Handle("remove_dos_breakpoint", json{{"id", id}}).contains("error"));
}

TEST(McpReToolsTest, DosBreakpointRejectsAnUnknownMatchMode)
{
    FakeMem mem = BuildBootedMachine();
    adm::core::DosBreakpoints bps;
    adm::core::LibraryCallTracer tracer;
    McpHost host = BareHost();
    host.dosBreakpoints = &bps;
    host.libraryTracer = &tracer;
    host.execReaders = [&mem]() { return mem.Readers(); };
    host.addBreakpoint = [](uint32_t) { return true; };
    McpToolHandler handler(std::move(host));

    json r =
        handler.Handle("add_dos_breakpoint", json{{"function", "Open"}, {"matchMode", "regex"}});
    ASSERT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("matchMode"), std::string::npos);
}

// --- disk filesystem ---------------------------------------------------------

TEST(McpReToolsTest, DiskToolsSayWhenNoDiskIsOpen)
{
    McpHost host = BareHost();
    host.activeDiskDocument = []() { return adm::core::DiskDocumentAccess{}; };
    McpToolHandler handler(std::move(host));

    for (const char* name :
         {"list_disk_files", "read_disk_file", "which_file_owns", "find_disk_bytes"}) {
        json r = handler.Handle(name, json{{"path", "s/startup-sequence"}, {"block", 880}});
        EXPECT_FALSE(r.value("available", true)) << name;
        EXPECT_NE(r.value("hint", std::string{}).find("disk"), std::string::npos) << name;
    }
}

// --- segments ----------------------------------------------------------------

TEST(McpReToolsTest, ListSegmentsSaysWhenNothingIsRunning)
{
    FakeMem mem = BuildBootedMachine(); // booted, but no CLI process
    McpHost host = BareHost();
    host.execReaders = [&mem]() { return mem.Readers(); };
    McpToolHandler handler(std::move(host));

    json r = handler.Handle("list_segments", json::object());
    EXPECT_FALSE(r.value("available", true));
    EXPECT_NE(r.value("hint", std::string{}).find("AmigaDOS"), std::string::npos);
}

TEST(McpReToolsTest, MapFileToMemoryRejectsNonExecutables)
{
    McpHost host = BareHost();
    host.activeDocumentPath = []() { return std::string{}; };
    McpToolHandler handler(std::move(host));

    // With no path and no document, the tool has to say what is missing.
    EXPECT_TRUE(handler.Handle("map_file_to_memory", json::object()).contains("error"));

    json r = handler.Handle("map_file_to_memory", json{{"path", "no/such/file.exe"}});
    ASSERT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("cannot open"), std::string::npos);
}

TEST(McpReToolsTest, MapFileToMemoryReadsAHunkFileWithoutARunningProgram)
{
    // A minimal HUNK_HEADER executable: one 4-longword code hunk.
    const std::vector<uint32_t> words = {
        0x000003F3, // HUNK_HEADER
        0x00000000, // no resident library names
        0x00000001, // table size
        0x00000000, // first hunk
        0x00000000, // last hunk
        0x00000004, // hunk 0 size in longwords
        0x000003E9, // HUNK_CODE
        0x00000004, // 4 longwords of code
        0x4E754E75, 0x4E754E75, 0x4E754E75, 0x4E754E75,
        0x000003F2, // HUNK_END
    };
    std::vector<uint8_t> bytes;
    for (uint32_t w : words) {
        bytes.push_back(static_cast<uint8_t>(w >> 24));
        bytes.push_back(static_cast<uint8_t>(w >> 16));
        bytes.push_back(static_cast<uint8_t>(w >> 8));
        bytes.push_back(static_cast<uint8_t>(w));
    }
    const auto path = std::filesystem::temp_directory_path() / "adm_mcpretools_test.exe";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }

    McpHost host = BareHost();
    McpToolHandler handler(std::move(host));
    json r = handler.Handle("map_file_to_memory", json{{"path", path.string()}});
    std::error_code ec;
    std::filesystem::remove(path, ec);

    ASSERT_FALSE(r.contains("error")) << r.dump();
    EXPECT_TRUE(r.value("available", false));
    EXPECT_FALSE(r.value("loaded", true)); // nothing is running, so no bases
    ASSERT_EQ(r["hunks"].size(), 1u);
    EXPECT_EQ(r["hunks"][0].value("index", -1), 0);
    EXPECT_FALSE(r["hunks"][0].contains("base"));
}
