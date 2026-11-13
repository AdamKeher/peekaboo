#include <gtest/gtest.h>

#include <map>
#include <vector>

#include "../src/core/ExecIntrospect.h"

using namespace adm::core::exec;

namespace {

// A sparse big-endian memory image. Only explicitly-written bytes are mapped, so
// the walks are exercised against unmapped addresses too.
class FakeMemory
{
public:
    void PutByte(uint32_t addr, uint8_t v) { m_bytes[addr] = v; }

    void PutWord(uint32_t addr, uint16_t v)
    {
        PutByte(addr, static_cast<uint8_t>(v >> 8));
        PutByte(addr + 1, static_cast<uint8_t>(v));
    }

    void PutLong(uint32_t addr, uint32_t v)
    {
        PutWord(addr, static_cast<uint16_t>(v >> 16));
        PutWord(addr + 2, static_cast<uint16_t>(v));
    }

    void PutCString(uint32_t addr, const std::string& s)
    {
        for (size_t i = 0; i < s.size(); ++i)
            PutByte(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
        PutByte(addr + static_cast<uint32_t>(s.size()), 0);
    }

    void PutBString(uint32_t addr, const std::string& s)
    {
        PutByte(addr, static_cast<uint8_t>(s.size()));
        for (size_t i = 0; i < s.size(); ++i)
            PutByte(addr + 1 + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
    }

    // An empty Exec List: lh_Head points at lh_Tail, whose ln_Succ is 0.
    void PutEmptyList(uint32_t listAddr) { PutLong(listAddr, listAddr + 4); }

    // Chains `nodes` into an Exec List at listAddr, terminated by the tail sentinel.
    void PutList(uint32_t listAddr, const std::vector<uint32_t>& nodes)
    {
        if (nodes.empty()) {
            PutEmptyList(listAddr);
            return;
        }
        PutLong(listAddr, nodes.front());
        for (size_t i = 0; i + 1 < nodes.size(); ++i)
            PutLong(nodes[i] + LN_SUCC, nodes[i + 1]);
        PutLong(nodes.back() + LN_SUCC, listAddr + 4); // -> lh_Tail
        PutLong(listAddr + 4 + LN_SUCC, 0);            // tail sentinel
    }

    Readers MakeReaders() const
    {
        Readers r;
        r.readLong = [this](uint32_t addr, bool& ok) {
            uint32_t v = 0;
            ok = true;
            for (int i = 0; i < 4; ++i) {
                auto it = m_bytes.find(addr + static_cast<uint32_t>(i));
                if (it == m_bytes.end()) {
                    ok = false;
                    return 0u;
                }
                v = (v << 8) | it->second;
            }
            return v;
        };
        r.readByte = [this](uint32_t addr, bool& ok) -> uint8_t {
            auto it = m_bytes.find(addr);
            ok = it != m_bytes.end();
            return ok ? it->second : uint8_t{0};
        };
        r.isMapped = [this](uint32_t addr) { return m_bytes.count(addr) != 0; };
        return r;
    }

private:
    std::map<uint32_t, uint8_t> m_bytes;
};

constexpr uint32_t kExecBase = 0x00C00276;

} // namespace

TEST(ExecIntrospectTest, ExecBaseReadsAndValidatesTheVector)
{
    FakeMemory mem;
    mem.PutLong(4, kExecBase);
    mem.PutByte(kExecBase, 0); // make the target mapped
    auto r = mem.MakeReaders();

    auto base = ExecBase(r);
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(*base, kExecBase);
}

TEST(ExecIntrospectTest, ExecBaseRejectsUnmappedNullAndOddPointers)
{
    {
        FakeMemory mem; // nothing at $4 at all
        EXPECT_FALSE(ExecBase(mem.MakeReaders()).has_value());
    }
    {
        FakeMemory mem;
        mem.PutLong(4, 0);
        EXPECT_FALSE(ExecBase(mem.MakeReaders()).has_value());
    }
    {
        FakeMemory mem;
        mem.PutLong(4, 0x00C00277); // odd address can never be an ExecBase
        mem.PutByte(0x00C00277, 0);
        EXPECT_FALSE(ExecBase(mem.MakeReaders()).has_value());
    }
    {
        FakeMemory mem;
        mem.PutLong(4, 0x00C00276); // plausible but unmapped
        EXPECT_FALSE(ExecBase(mem.MakeReaders()).has_value());
    }
}

TEST(ExecIntrospectTest, ReadCStringStopsAtTerminatorAndMaxLen)
{
    FakeMemory mem;
    mem.PutCString(0x1000, "dos.library");
    auto r = mem.MakeReaders();

    EXPECT_EQ(ReadCString(r, 0x1000), "dos.library");
    EXPECT_EQ(ReadCString(r, 0x1000, 3), "dos");
    EXPECT_EQ(ReadCString(r, 0), "");
    EXPECT_EQ(ReadCString(r, 0x9999), ""); // unmapped
}

TEST(ExecIntrospectTest, ReadCStringSanitisesNonPrintableBytes)
{
    FakeMemory mem;
    mem.PutByte(0x2000, 'a');
    mem.PutByte(0x2001, 0x01);
    mem.PutByte(0x2002, 0xFF);
    mem.PutByte(0x2003, 'b');
    mem.PutByte(0x2004, 0);
    EXPECT_EQ(ReadCString(mem.MakeReaders(), 0x2000), "a..b");
}

TEST(ExecIntrospectTest, ReadBStringUsesTheLengthByte)
{
    FakeMemory mem;
    mem.PutBString(0x3000, "loader");
    // No terminator is written, so a C-string read would run past the end; the
    // BCPL read must stop at exactly six characters.
    mem.PutByte(0x3007, 'X');
    auto r = mem.MakeReaders();

    EXPECT_EQ(ReadBString(r, 0x3000), "loader");
    EXPECT_EQ(ReadBString(r, 0x3000, 3), "loa");
    EXPECT_EQ(ReadBString(r, 0), "");
}

TEST(ExecIntrospectTest, ForEachNodeWalksToTheTailSentinel)
{
    FakeMemory mem;
    mem.PutList(0x4000, {0x5000, 0x5100, 0x5200});
    std::vector<uint32_t> seen;
    ForEachNode(mem.MakeReaders(), 0x4000, kListCap, [&](uint32_t n) { seen.push_back(n); });
    EXPECT_EQ(seen, (std::vector<uint32_t>{0x5000, 0x5100, 0x5200}));
}

TEST(ExecIntrospectTest, ForEachNodeHandlesAnEmptyList)
{
    FakeMemory mem;
    mem.PutEmptyList(0x4000);
    mem.PutLong(0x4004 + LN_SUCC, 0);
    int count = 0;
    ForEachNode(mem.MakeReaders(), 0x4000, kListCap, [&](uint32_t) { ++count; });
    EXPECT_EQ(count, 0);
}

TEST(ExecIntrospectTest, ForEachNodeCapsACircularChain)
{
    FakeMemory mem;
    // A node whose ln_Succ points back at itself would otherwise spin forever.
    mem.PutLong(0x4000, 0x5000);
    mem.PutLong(0x5000 + LN_SUCC, 0x5000);
    int count = 0;
    ForEachNode(mem.MakeReaders(), 0x4000, 10, [&](uint32_t) { ++count; });
    EXPECT_EQ(count, 10);
}

TEST(ExecIntrospectTest, ListLibraryNodesDecodesVersionOpenCountAndName)
{
    FakeMemory mem;
    const uint32_t lib = 0x6000;
    mem.PutList(kExecBase + EB_LIBLIST, {lib});
    mem.PutWord(lib + LIB_VERSION, 39);
    mem.PutWord(lib + LIB_REVISION, 106);
    mem.PutWord(lib + LIB_OPENCNT, 3);
    mem.PutLong(lib + LN_NAME, 0x7000);
    mem.PutCString(0x7000, "dos.library");
    mem.PutLong(lib + LIB_IDSTRING, 0x7100);
    mem.PutCString(0x7100, "dos 39.106");

    auto libs = ListLibraryNodes(mem.MakeReaders(), kExecBase + EB_LIBLIST);
    ASSERT_EQ(libs.size(), 1u);
    EXPECT_EQ(libs[0].node, lib);
    EXPECT_EQ(libs[0].version, 39);
    EXPECT_EQ(libs[0].revision, 106);
    EXPECT_EQ(libs[0].openCount, 3);
    EXPECT_EQ(libs[0].name, "dos.library");
    EXPECT_EQ(libs[0].idString, "dos 39.106");
}

TEST(ExecIntrospectTest, ListTasksPutsThisTaskFirstAndReadsStackBounds)
{
    FakeMemory mem;
    const uint32_t thisTask = 0x8000, ready = 0x8100;
    mem.PutLong(kExecBase + EB_THISTASK, thisTask);
    mem.PutByte(thisTask + LN_PRI, static_cast<uint8_t>(-5));
    mem.PutByte(thisTask + TC_STATE, 2);
    mem.PutLong(thisTask + TC_SPLOWER, 0x40000);
    mem.PutLong(thisTask + TC_SPUPPER, 0x41000);
    mem.PutLong(thisTask + LN_NAME, 0x8900);
    mem.PutCString(0x8900, "loader");

    mem.PutList(kExecBase + EB_TASKREADY, {ready});
    mem.PutByte(ready + TC_STATE, 3);
    mem.PutLong(ready + LN_NAME, 0x8A00);
    mem.PutCString(0x8A00, "input.device");
    mem.PutEmptyList(kExecBase + EB_TASKWAIT);
    mem.PutLong(kExecBase + EB_TASKWAIT + 4 + LN_SUCC, 0);

    auto tasks = ListTasks(mem.MakeReaders(), kExecBase);
    ASSERT_EQ(tasks.size(), 2u);
    EXPECT_TRUE(tasks[0].isThisTask);
    EXPECT_EQ(tasks[0].name, "loader");
    EXPECT_EQ(tasks[0].pri, -5);
    EXPECT_EQ(tasks[0].spLower, 0x40000u);
    EXPECT_EQ(tasks[0].spUpper, 0x41000u);
    EXPECT_FALSE(tasks[1].isThisTask);
    EXPECT_EQ(tasks[1].name, "input.device");
    EXPECT_STREQ(TaskStateName(tasks[1].state), "Ready");
}

TEST(ExecIntrospectTest, ListMemHeadersAndFreeChunks)
{
    FakeMemory mem;
    const uint32_t mh = 0x9000;
    mem.PutList(kExecBase + EB_MEMLIST, {mh});
    mem.PutWord(mh + MH_ATTRIBUTES, 0x0002); // MEMF_CHIP
    mem.PutLong(mh + MH_LOWER, 0x00001000);
    mem.PutLong(mh + MH_UPPER, 0x00080000);
    mem.PutLong(mh + MH_FREE, 0x00060000);
    mem.PutLong(mh + MH_FIRST, 0x00010000);
    mem.PutLong(mh + LN_NAME, 0x9900);
    mem.PutCString(0x9900, "chip memory");

    // Two free chunks, then a null next pointer.
    mem.PutLong(0x00010000 + MC_BYTES, 0x1000);
    mem.PutLong(0x00010000 + MC_NEXT, 0x00020000);
    mem.PutLong(0x00020000 + MC_BYTES, 0x2000);
    mem.PutLong(0x00020000 + MC_NEXT, 0);

    auto r = mem.MakeReaders();
    auto headers = ListMemHeaders(r, kExecBase);
    ASSERT_EQ(headers.size(), 1u);
    EXPECT_EQ(headers[0].name, "chip memory");
    EXPECT_EQ(headers[0].lower, 0x1000u);
    EXPECT_EQ(headers[0].upper, 0x80000u);

    auto chunks = ListFreeChunks(r, headers[0]);
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0].addr, 0x00010000u);
    EXPECT_EQ(chunks[0].bytes, 0x1000u);
    EXPECT_EQ(chunks[1].addr, 0x00020000u);
    EXPECT_EQ(chunks[1].bytes, 0x2000u);
}

TEST(ExecIntrospectTest, ListFreeChunksStopsOnAnImplausibleChunk)
{
    FakeMemory mem;
    MemHeaderNode mh;
    mh.lower = 0x1000;
    mh.upper = 0x80000;
    mh.first = 0x10000;
    // A chunk claiming more bytes than the header can hold means the chain is
    // corrupt; the walk must stop rather than report nonsense.
    mem.PutLong(0x10000 + MC_BYTES, 0x00FFFFFF);
    mem.PutLong(0x10000 + MC_NEXT, 0);
    EXPECT_TRUE(ListFreeChunks(mem.MakeReaders(), mh).empty());
}

TEST(ExecIntrospectTest, ListPortsCountsQueuedMessages)
{
    FakeMemory mem;
    const uint32_t port = 0xA000;
    mem.PutList(kExecBase + EB_PORTLIST, {port});
    mem.PutByte(port + LN_PRI, 0);
    mem.PutLong(port + LN_NAME, 0xA900);
    mem.PutCString(0xA900, "PeekabooPort");
    mem.PutList(port + MP_MSGLIST, {0xB000, 0xB100});

    auto ports = ListPorts(mem.MakeReaders(), kExecBase);
    ASSERT_EQ(ports.size(), 1u);
    EXPECT_EQ(ports[0].name, "PeekabooPort");
    EXPECT_EQ(ports[0].messages, 2);
}

TEST(ExecIntrospectTest, VectorAndInterruptNames)
{
    EXPECT_STREQ(ExceptionVectorName(3), "Address Error");
    EXPECT_STREQ(ExceptionVectorName(27), "Level 3 Int");
    EXPECT_EQ(ExceptionVectorName(32), nullptr); // TRAP #0 is named by the caller
    EXPECT_EQ(ExceptionVectorName(-1), nullptr);

    EXPECT_STREQ(InterruptName(5), "VERTB");
    EXPECT_EQ(InterruptName(16), nullptr);
    EXPECT_EQ(InterruptName(-1), nullptr);
}
