#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "../src/core/LibraryCallTracer.h"

using adm::AmigaEmulator;
using adm::core::CpuTraceHistory;
using adm::core::LibraryCallQuery;
using adm::core::LibraryCallTracer;
using adm::core::LvoArgType;

namespace ex = adm::core::exec;

namespace {

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
    void PutBString(uint32_t a, const std::string& s)
    {
        PutByte(a, static_cast<uint8_t>(s.size()));
        for (size_t i = 0; i < s.size(); ++i)
            PutByte(a + 1 + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
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
constexpr uint32_t kOpenLvo = 30;
constexpr uint32_t kOpenEntry = kDosBase - kOpenLvo;
constexpr uint32_t kCloseEntry = kDosBase - 36;
constexpr uint32_t kNameBstr = 0x00030000; // BCPL string "loader"
constexpr uint32_t kReturnPc = 0x00021006;

FakeMem BuildMachineWithDos()
{
    FakeMem m;
    m.PutLong(4, kExecBase);
    m.PutByte(kExecBase, 0);
    for (uint32_t off : {ex::EB_LIBLIST, ex::EB_DEVICELIST, ex::EB_RESOURCELIST})
        m.PutEmptyList(kExecBase + off);

    m.PutList(kExecBase + ex::EB_LIBLIST, {kDosBase});
    m.PutWord(kDosBase + ex::LIB_VERSION, 39);
    m.PutWord(kDosBase + ex::LIB_REVISION, 106);
    m.PutWord(kDosBase + ex::LIB_OPENCNT, 1);
    m.PutLong(kDosBase + ex::LN_NAME, 0x00C10900);
    m.PutCString(0x00C10900, "dos.library");

    m.PutBString(kNameBstr, "loader");
    return m;
}

// Writes a small arg-metadata file so the test does not depend on the shipped
// asset (or on the working directory the test binary happens to run in).
std::string WriteArgTable()
{
    const auto path = std::filesystem::temp_directory_path() / "adm_lvo_args_test.json";
    std::ofstream f(path);
    f << R"({
  "_comment": "test fixture",
  "dos.library": {
    "-30": { "name": "Open",
             "args": [{"reg": "d1", "name": "name", "type": "bstr"},
                      {"reg": "d2", "name": "accessMode", "type": "long"}],
             "result": {"name": "file", "type": "bptr"} },
    "-36": { "name": "Close", "args": [{"reg": "d1", "name": "file", "type": "bptr"}] }
  }
})";
    f.close();
    return path.string();
}

AmigaEmulator::CpuTraceEntry Entry(uint32_t pc, uint32_t cycle)
{
    AmigaEmulator::CpuTraceEntry e{};
    e.pc = pc;
    e.absolute_cycle = cycle;
    e.raw[0] = 0x4E;
    e.raw[1] = 0x71; // NOP; the tracer keys off the PC, not the opcode
    e.raw_len = 2;
    e.a[7] = 0x0003FFFC;
    return e;
}

LibraryCallTracer MakeTracer()
{
    LibraryCallTracer t;
    EXPECT_TRUE(t.LoadArgTables(WriteArgTable()));
    return t;
}

} // namespace

TEST(LibraryCallTracerTest, ParsesEveryArgumentType)
{
    adm::core::LvoArgType type{};
    EXPECT_TRUE(LibraryCallTracer::ParseArgType("long", type));
    EXPECT_EQ(type, LvoArgType::Long);
    EXPECT_TRUE(LibraryCallTracer::ParseArgType("BSTR", type));
    EXPECT_EQ(type, LvoArgType::BStr);
    EXPECT_TRUE(LibraryCallTracer::ParseArgType("bptr", type));
    EXPECT_EQ(type, LvoArgType::BPtr);
    EXPECT_FALSE(LibraryCallTracer::ParseArgType("widget", type));
}

TEST(LibraryCallTracerTest, MissingArgTableIsNotFatal)
{
    LibraryCallTracer t;
    EXPECT_FALSE(t.LoadArgTables("no/such/file.json"));
    EXPECT_FALSE(t.ArgTablesLoaded());
}

TEST(LibraryCallTracerTest, LoadsTheShippedTables)
{
    // The real assets must stay parseable; a broken edit should fail here rather
    // than silently degrading every trace to raw register values.
    LibraryCallTracer t;
    for (const char* prefix : {"", "../", "../../"}) {
        if (t.LoadArgTables(std::string(prefix) + "assets/amiga_lvo_args.json")) {
            EXPECT_NE(t.FindSpec("dos.library", 30), nullptr);
            EXPECT_TRUE(t.LoadNameTables(std::string(prefix) + "assets/amiga_lvos.json"));
            return;
        }
    }
    GTEST_SKIP() << "assets/ not reachable from the test working directory";
}

TEST(LibraryCallTracerTest, DecodesValuesAccordingToTheirType)
{
    FakeMem mem = BuildMachineWithDos();
    mem.PutCString(0x00031000, "dos.library");
    auto r = mem.Readers();

    adm::core::LvoArgSpec bstr{"d1", "name", LvoArgType::BStr};
    auto v = LibraryCallTracer::DecodeValue(bstr, kNameBstr >> 2, r);
    EXPECT_EQ(v.text, "loader");
    EXPECT_EQ(v.value, kNameBstr >> 2);

    adm::core::LvoArgSpec cstr{"a1", "libName", LvoArgType::CStr};
    EXPECT_EQ(LibraryCallTracer::DecodeValue(cstr, 0x00031000, r).text, "dos.library");

    adm::core::LvoArgSpec bptr{"d0", "lock", LvoArgType::BPtr};
    EXPECT_EQ(LibraryCallTracer::DecodeValue(bptr, 0x1000, r).text, "$00004000");
    EXPECT_EQ(LibraryCallTracer::DecodeValue(bptr, 0, r).text, "NULL");

    adm::core::LvoArgSpec sLong{"d2", "accessMode", LvoArgType::Long};
    EXPECT_EQ(LibraryCallTracer::DecodeValue(sLong, 0xFFFFFFFF, r).text, "-1");

    adm::core::LvoArgSpec ptr{"a0", "buffer", LvoArgType::Ptr};
    EXPECT_EQ(LibraryCallTracer::DecodeValue(ptr, 0x1234, r).text, "$00001234");
}

TEST(LibraryCallTracerTest, ResolvesAFunctionToItsJumpTableEntry)
{
    FakeMem mem = BuildMachineWithDos();
    auto tracer = MakeTracer();

    uint32_t lvo = 0;
    auto entry = tracer.ResolveEntry(mem.Readers(), kExecBase, "dos.library", "Open", &lvo);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(*entry, kOpenEntry);
    EXPECT_EQ(lvo, kOpenLvo);

    // Case-insensitive on the function name, but the library must be open.
    EXPECT_TRUE(tracer.ResolveEntry(mem.Readers(), kExecBase, "dos.library", "open").has_value());
    EXPECT_FALSE(
        tracer.ResolveEntry(mem.Readers(), kExecBase, "graphics.library", "Open").has_value());
    EXPECT_FALSE(
        tracer.ResolveEntry(mem.Readers(), kExecBase, "dos.library", "Nonsense").has_value());
    EXPECT_FALSE(tracer.ResolveEntry(mem.Readers(), 0, "dos.library", "Open").has_value());
}

TEST(LibraryCallTracerTest, TracesACallWithDecodedArgumentsAndResult)
{
    FakeMem mem = BuildMachineWithDos();
    mem.PutLong(0x0003FFFC, kReturnPc); // the return address the JSR pushed
    auto tracer = MakeTracer();

    CpuTraceHistory history;
    history.SetEnabled(true);
    std::vector<AmigaEmulator::CpuTraceEntry> entries = {
        Entry(0x00021000, 100), Entry(kOpenEntry, 104), // the jump-table JMP: this is the call
        Entry(0x00F81000, 108),                         // somewhere inside dos.library
        Entry(kReturnPc, 200),                          // control comes back here
    };
    entries[1].d[1] = kNameBstr >> 2; // d1 = BPTR to "loader"
    entries[1].d[2] = 1005;           // MODE_OLDFILE
    entries[3].d[0] = 0x00008000;     // the returned file handle, as a BPTR

    history.AppendFrame(entries.data(), static_cast<int>(entries.size()), 1, 0);

    auto snap = tracer.Trace(history, mem.Readers(), kExecBase, {});
    ASSERT_TRUE(snap.available) << snap.hint;
    ASSERT_EQ(snap.calls.size(), 1u);

    const auto& call = snap.calls[0];
    EXPECT_EQ(call.library, "dos.library");
    EXPECT_EQ(call.function, "Open");
    EXPECT_EQ(call.lvo, kOpenLvo);
    EXPECT_EQ(call.entryPc, kOpenEntry);
    EXPECT_EQ(call.callerPc, kReturnPc);
    ASSERT_EQ(call.args.size(), 2u);
    EXPECT_EQ(call.args[0].name, "name");
    EXPECT_EQ(call.args[0].text, "loader");
    EXPECT_EQ(call.args[1].name, "accessMode");
    EXPECT_EQ(call.args[1].text, "1005");
    ASSERT_TRUE(call.haveResult);
    EXPECT_EQ(call.result.value, 0x00008000u);
    EXPECT_EQ(call.result.text, "$00020000");
}

TEST(LibraryCallTracerTest, ResultIsAbsentWhenControlNeverComesBack)
{
    FakeMem mem = BuildMachineWithDos();
    mem.PutLong(0x0003FFFC, kReturnPc);
    auto tracer = MakeTracer();

    CpuTraceHistory history;
    history.SetEnabled(true);
    auto entry = Entry(kOpenEntry, 104);
    entry.d[1] = kNameBstr >> 2;
    history.AppendFrame(&entry, 1, 1, 0);

    auto snap = tracer.Trace(history, mem.Readers(), kExecBase, {});
    ASSERT_EQ(snap.calls.size(), 1u);
    EXPECT_FALSE(snap.calls[0].haveResult);
    EXPECT_EQ(snap.calls[0].args[0].text, "loader");
}

TEST(LibraryCallTracerTest, FiltersByLibraryAndFunction)
{
    FakeMem mem = BuildMachineWithDos();
    auto tracer = MakeTracer();

    CpuTraceHistory history;
    history.SetEnabled(true);
    std::vector<AmigaEmulator::CpuTraceEntry> entries = {Entry(kOpenEntry, 100),
                                                         Entry(kCloseEntry, 104)};
    history.AppendFrame(entries.data(), 2, 1, 0);

    EXPECT_EQ(tracer.Trace(history, mem.Readers(), kExecBase, {}).calls.size(), 2u);

    LibraryCallQuery onlyOpen;
    onlyOpen.functions = {"open"};
    auto snap = tracer.Trace(history, mem.Readers(), kExecBase, onlyOpen);
    ASSERT_EQ(snap.calls.size(), 1u);
    EXPECT_EQ(snap.calls[0].function, "Open");

    LibraryCallQuery wrongLib;
    wrongLib.library = "graphics.library";
    snap = tracer.Trace(history, mem.Readers(), kExecBase, wrongLib);
    EXPECT_FALSE(snap.available);
    EXPECT_NE(snap.hint.find("graphics.library"), std::string::npos);
}

TEST(LibraryCallTracerTest, LimitTruncatesButTheTotalIsHonest)
{
    FakeMem mem = BuildMachineWithDos();
    auto tracer = MakeTracer();

    CpuTraceHistory history;
    history.SetEnabled(true);
    std::vector<AmigaEmulator::CpuTraceEntry> entries;
    for (uint32_t i = 0; i < 5; ++i)
        entries.push_back(Entry(kOpenEntry, 100 + i));
    history.AppendFrame(entries.data(), 5, 1, 0);

    LibraryCallQuery q;
    q.limit = 2;
    auto snap = tracer.Trace(history, mem.Readers(), kExecBase, q);
    EXPECT_EQ(snap.calls.size(), 2u);
    EXPECT_TRUE(snap.truncated);
    EXPECT_EQ(snap.totalCalls, 5u);
}

TEST(LibraryCallTracerTest, ReportsWhyItCannotTrace)
{
    FakeMem mem = BuildMachineWithDos();
    auto tracer = MakeTracer();
    CpuTraceHistory empty;

    auto noOs = tracer.Trace(empty, mem.Readers(), 0, {});
    EXPECT_FALSE(noOs.available);
    EXPECT_NE(noOs.hint.find("ExecBase"), std::string::npos);

    auto noTrace = tracer.Trace(empty, mem.Readers(), kExecBase, {});
    EXPECT_FALSE(noTrace.available);
    EXPECT_FALSE(noTrace.hint.empty());
}

TEST(LibraryCallTracerTest, WithNoTablesAtAllItSaysSoRatherThanReturningNothing)
{
    FakeMem mem = BuildMachineWithDos();
    LibraryCallTracer tracer; // neither the names nor the argument metadata

    CpuTraceHistory history;
    history.SetEnabled(true);
    auto entry = Entry(kOpenEntry, 100);
    history.AppendFrame(&entry, 1, 1, 0);

    auto snap = tracer.Trace(history, mem.Readers(), kExecBase, {});
    // Without a table there is nothing to key the jump table off; an empty
    // success would read as "this program makes no library calls".
    EXPECT_FALSE(snap.available);
    EXPECT_NE(snap.hint.find("LVO"), std::string::npos);
}

TEST(LibraryCallTracerTest, ArgMetadataAloneIsEnoughToNameAndDecodeACall)
{
    FakeMem mem = BuildMachineWithDos();
    mem.PutLong(0x0003FFFC, kReturnPc);
    LibraryCallTracer tracer;
    ASSERT_TRUE(tracer.LoadArgTables(WriteArgTable())); // no generated name table

    CpuTraceHistory history;
    history.SetEnabled(true);
    auto entry = Entry(kOpenEntry, 100);
    entry.d[1] = kNameBstr >> 2;
    history.AppendFrame(&entry, 1, 1, 0);

    auto snap = tracer.Trace(history, mem.Readers(), kExecBase, {});
    ASSERT_TRUE(snap.available) << snap.hint;
    ASSERT_EQ(snap.calls.size(), 1u);
    EXPECT_EQ(snap.calls[0].function, "Open");
    EXPECT_EQ(snap.calls[0].args[0].text, "loader");
}
