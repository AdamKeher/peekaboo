#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>

#include "../src/core/AddressDescriber.h"

using adm::core::AddressDescriber;
using adm::core::AddressDescription;
using adm::core::AddressRegion;

namespace {

// Sparse big-endian image, same shape as the other introspection tests.
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
        if (nodes.empty()) {
            PutEmptyList(listAddr);
            return;
        }
        PutLong(listAddr, nodes.front());
        for (size_t i = 0; i + 1 < nodes.size(); ++i)
            PutLong(nodes[i], nodes[i + 1]);
        PutLong(nodes.back(), listAddr + 4);
        PutLong(listAddr + 4, 0);
    }

    adm::core::exec::Readers Readers() const
    {
        adm::core::exec::Readers r;
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

namespace ex = adm::core::exec;

constexpr uint32_t kExecBase = 0x00C00276;

const adm::core::AddressLayer* Layer(const AddressDescription& d, const std::string& kind)
{
    auto it = std::find_if(d.layers.begin(), d.layers.end(),
                           [&](const adm::core::AddressLayer& l) { return l.kind == kind; });
    return it == d.layers.end() ? nullptr : &*it;
}

bool HasKind(const AddressDescription& d, const std::string& kind)
{
    return Layer(d, kind) != nullptr;
}

// A booted-looking machine: ExecBase, one chip MemHeader with a free chunk, one
// task with a stack, and dos.library on the library list.
FakeMem BuildBootedMachine()
{
    FakeMem m;
    m.PutLong(4, kExecBase);
    m.PutByte(kExecBase, 0); // make ExecBase itself mapped

    // Every list ExecBase owns must at least terminate cleanly.
    for (uint32_t off : {ex::EB_MEMLIST, ex::EB_RESOURCELIST, ex::EB_DEVICELIST, ex::EB_INTRLIST,
                         ex::EB_LIBLIST, ex::EB_PORTLIST, ex::EB_TASKREADY, ex::EB_TASKWAIT})
        m.PutEmptyList(kExecBase + off);

    // Chip pool $1000-$80000 with one free chunk at $10000.
    const uint32_t mh = 0x00C01000;
    m.PutList(kExecBase + ex::EB_MEMLIST, {mh});
    m.PutWord(mh + ex::MH_ATTRIBUTES, 0x0002);
    m.PutLong(mh + ex::MH_LOWER, 0x00001000);
    m.PutLong(mh + ex::MH_UPPER, 0x00080000);
    m.PutLong(mh + ex::MH_FREE, 0x00001000);
    m.PutLong(mh + ex::MH_FIRST, 0x00010000);
    m.PutLong(mh + ex::LN_NAME, 0x00C01900);
    m.PutCString(0x00C01900, "chip memory");
    m.PutLong(0x00010000 + ex::MC_BYTES, 0x1000);
    m.PutLong(0x00010000 + ex::MC_NEXT, 0);

    // Current task with a stack at $40000-$41000.
    const uint32_t task = 0x00C02000;
    m.PutLong(kExecBase + ex::EB_THISTASK, task);
    m.PutByte(task + ex::TC_STATE, 2);
    m.PutLong(task + ex::TC_SPLOWER, 0x00040000);
    m.PutLong(task + ex::TC_SPUPPER, 0x00041000);
    m.PutLong(task + ex::LN_NAME, 0x00C02900);
    m.PutCString(0x00C02900, "loader");

    // dos.library with a jump table below its base.
    const uint32_t dos = 0x00C03000;
    m.PutList(kExecBase + ex::EB_LIBLIST, {dos});
    m.PutWord(dos + ex::LIB_VERSION, 39);
    m.PutWord(dos + ex::LIB_REVISION, 106);
    m.PutWord(dos + ex::LIB_OPENCNT, 2);
    m.PutLong(dos + ex::LN_NAME, 0x00C03900);
    m.PutCString(0x00C03900, "dos.library");
    return m;
}

AddressDescriber::Context ContextFor(const FakeMem& mem)
{
    AddressDescriber::Context ctx;
    ctx.readers = mem.Readers();
    ctx.regions = {
        {0x00000000, 0x00080000, "Chip RAM", true},
        {0x00C00000, 0x00080000, "Slow RAM", true},
        {0x00F80000, 0x00080000, "Kickstart ROM", false},
    };
    return ctx;
}

} // namespace

TEST(AddressDescriberTest, ReportsTheContainingMemoryRegion)
{
    FakeMem mem;
    auto ctx = ContextFor(mem);
    ctx.inspectOs = false;

    auto d = AddressDescriber::Describe(0x00012345, ctx);
    EXPECT_TRUE(d.mapped);
    const auto* r = Layer(d, "region");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->name, "Chip RAM");
    EXPECT_EQ(r->offset, 0x00012345u);
    EXPECT_EQ(r->detail, "writable");

    auto rom = AddressDescriber::Describe(0x00F80010, ctx);
    ASSERT_NE(Layer(rom, "region"), nullptr);
    EXPECT_EQ(Layer(rom, "region")->detail, "read-only");
}

TEST(AddressDescriberTest, UnmappedAddressIsReportedAsSuch)
{
    FakeMem mem;
    auto ctx = ContextFor(mem);
    ctx.inspectOs = false;
    auto d = AddressDescriber::Describe(0x00900000, ctx);
    EXPECT_FALSE(d.mapped);
    EXPECT_FALSE(HasKind(d, "region"));
}

TEST(AddressDescriberTest, NamesCustomChipRegisters)
{
    FakeMem mem;
    auto ctx = ContextFor(mem);
    ctx.inspectOs = false;
    ctx.customRegName = [](uint32_t off) { return off == 0x180 ? "COLOR00" : ""; };
    ctx.customRegDesc = [](uint32_t off) { return off == 0x180 ? "background colour" : ""; };

    auto d = AddressDescriber::Describe(0xDFF180, ctx);
    const auto* c = Layer(d, "customRegister");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->name, "COLOR00");
    EXPECT_EQ(c->detail, "background colour");
    EXPECT_TRUE(d.mapped);

    // The odd byte of a register still resolves to that register.
    auto odd = AddressDescriber::Describe(0xDFF181, ctx);
    ASSERT_NE(Layer(odd, "customRegister"), nullptr);
    EXPECT_EQ(Layer(odd, "customRegister")->base, 0xDFF180u);

    // An unnamed register falls back to its address rather than going missing.
    auto spare = AddressDescriber::Describe(0xDFF07C, ctx);
    ASSERT_NE(Layer(spare, "customRegister"), nullptr);
    EXPECT_EQ(Layer(spare, "customRegister")->name, "$DFF07C");
}

TEST(AddressDescriberTest, DecodesCiaRegisters)
{
    char chip = 0;
    std::string reg;
    uint32_t idx = 0;

    ASSERT_TRUE(AddressDescriber::DescribeCia(0xBFE001, chip, reg, idx));
    EXPECT_EQ(chip, 'A');
    EXPECT_EQ(reg, "PRA");
    EXPECT_EQ(idx, 0u);

    ASSERT_TRUE(AddressDescriber::DescribeCia(0xBFE801, chip, reg, idx));
    EXPECT_EQ(chip, 'A');
    EXPECT_EQ(reg, "TODLOW"); // the TOD latch, a classic scratch hiding place
    EXPECT_EQ(idx, 8u);

    ASSERT_TRUE(AddressDescriber::DescribeCia(0xBFD100, chip, reg, idx));
    EXPECT_EQ(chip, 'B');
    EXPECT_EQ(reg, "PRB");

    EXPECT_FALSE(AddressDescriber::DescribeCia(0xBFE002, chip, reg, idx));
    EXPECT_FALSE(AddressDescriber::DescribeCia(0xBFF001, chip, reg, idx));
}

TEST(AddressDescriberTest, NamesLowMemoryExceptionVectors)
{
    FakeMem mem;
    auto ctx = ContextFor(mem);
    ctx.inspectOs = false;

    auto lvl3 = AddressDescriber::Describe(0x6C, ctx);
    const auto* v = Layer(lvl3, "vector");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->name, "Level 3 Int");
    EXPECT_EQ(v->base, 0x6Cu);

    // A mid-longword byte still names its vector and says which byte it is.
    auto mid = AddressDescriber::Describe(0x6E, ctx);
    ASSERT_NE(Layer(mid, "vector"), nullptr);
    EXPECT_EQ(Layer(mid, "vector")->base, 0x6Cu);
    EXPECT_EQ(Layer(mid, "vector")->offset, 2u);

    EXPECT_EQ(AddressDescriber::VectorName(0), "Reset SSP");
    EXPECT_EQ(AddressDescriber::VectorName(32), "TRAP #0");
    EXPECT_EQ(AddressDescriber::VectorName(47), "TRAP #15");

    // Above $400 there are no vectors.
    EXPECT_FALSE(HasKind(AddressDescriber::Describe(0x400, ctx), "vector"));
}

TEST(AddressDescriberTest, NamesExecBaseFields)
{
    FakeMem mem = BuildBootedMachine();
    auto ctx = ContextFor(mem);

    auto cool = AddressDescriber::Describe(kExecBase + ex::EB_COOLCAPTURE, ctx);
    const auto* f = Layer(cool, "execBase");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->name, "ExecBase.CoolCapture");

    auto kick = AddressDescriber::Describe(kExecBase + ex::EB_KICKTAGPTR + 2, ctx);
    ASSERT_NE(Layer(kick, "execBase"), nullptr);
    EXPECT_EQ(Layer(kick, "execBase")->name, "ExecBase.KickTagPtr");
    EXPECT_EQ(Layer(kick, "execBase")->offset, 2u);

    auto libList = AddressDescriber::Describe(kExecBase + ex::EB_LIBLIST, ctx);
    ASSERT_NE(Layer(libList, "execBase"), nullptr);
    EXPECT_EQ(Layer(libList, "execBase")->name, "ExecBase.LibList");
}

TEST(AddressDescriberTest, DistinguishesFreeAndAllocatedMemory)
{
    FakeMem mem = BuildBootedMachine();
    auto ctx = ContextFor(mem);

    auto freeAddr = AddressDescriber::Describe(0x00010040, ctx);
    ASSERT_NE(Layer(freeAddr, "memoryHeader"), nullptr);
    EXPECT_EQ(Layer(freeAddr, "memoryHeader")->name, "chip memory");
    ASSERT_NE(Layer(freeAddr, "allocation"), nullptr);
    EXPECT_EQ(Layer(freeAddr, "allocation")->name, "free");

    auto usedAddr = AddressDescriber::Describe(0x00020000, ctx);
    ASSERT_NE(Layer(usedAddr, "allocation"), nullptr);
    EXPECT_EQ(Layer(usedAddr, "allocation")->name, "allocated");
}

TEST(AddressDescriberTest, IdentifiesATaskStack)
{
    FakeMem mem = BuildBootedMachine();
    auto ctx = ContextFor(mem);

    auto d = AddressDescriber::Describe(0x00040800, ctx);
    const auto* s = Layer(d, "taskStack");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->name, "loader");
    EXPECT_EQ(s->base, 0x00040000u);
    EXPECT_EQ(s->size, 0x1000u);

    EXPECT_FALSE(HasKind(AddressDescriber::Describe(0x00041000, ctx), "taskStack"));
}

TEST(AddressDescriberTest, IdentifiesLibraryBasesAndJumpTableEntries)
{
    FakeMem mem = BuildBootedMachine();
    auto ctx = ContextFor(mem);
    const uint32_t dos = 0x00C03000;

    auto base = AddressDescriber::Describe(dos, ctx);
    const auto* n = Layer(base, "execNode");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->name, "dos.library");

    // dos.library/Open() is LVO -30.
    auto open = AddressDescriber::Describe(dos - 30, ctx);
    const auto* v = Layer(open, "libraryVector");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->name, "dos.library LVO -30");
    EXPECT_EQ(v->size, 6u);

    // Not a multiple of six below the base, so not a vector entry.
    EXPECT_FALSE(HasKind(AddressDescriber::Describe(dos - 31, ctx), "libraryVector"));
}

TEST(AddressDescriberTest, ResolvesLoadedSegmentsAndFunctionSymbols)
{
    FakeMem mem = BuildBootedMachine();
    // Turn the current task into a CLI process with a one-segment list.
    const uint32_t task = 0x00C02000, cli = 0x00C04000, seg = 0x00021000;
    mem.PutByte(task + 8, 13);         // NT_PROCESS
    mem.PutLong(task + 172, cli >> 2); // pr_CLI (BPTR)
    mem.PutLong(cli + 60, seg >> 2);   // cli_Module (BPTR)
    mem.PutLong(seg - 4, 0x400);       // allocation length in BYTES (1 KB)
    mem.PutLong(seg, 0);               // end of chain

    auto ctx = ContextFor(mem);
    ctx.resolveSymbol = [](uint32_t linkAddr, std::string& name, uint32_t& base, uint32_t& size) {
        if (linkAddr < 0x100)
            return false;
        name = "LoadMenu";
        base = 0x100;
        size = 0x40;
        return true;
    };

    // seg + 4 is the segment base; +0x100 lands inside LoadMenu.
    auto d = AddressDescriber::Describe(seg + 4 + 0x100, ctx);
    const auto* s = Layer(d, "segment");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->base, seg + 4);
    const auto* f = Layer(d, "function");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->name, "LoadMenu");
    EXPECT_EQ(f->base, seg + 4 + 0x100);
}

TEST(AddressDescriberTest, IncludesAnnotationsYouSetEarlier)
{
    FakeMem mem;
    auto ctx = ContextFor(mem);
    ctx.inspectOs = false;
    ctx.annotationLabel = [](uint32_t a) { return a == 0x1234 ? "menuSelection" : ""; };
    ctx.annotationComment = [](uint32_t a) { return a == 0x1234 ? "written by the menu" : ""; };

    auto d = AddressDescriber::Describe(0x1234, ctx);
    ASSERT_GE(d.layers.size(), 2u);
    const auto* a = Layer(d, "annotation");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->name, "menuSelection");

    EXPECT_FALSE(HasKind(AddressDescriber::Describe(0x1235, ctx), "annotation"));
}

TEST(AddressDescriberTest, SkipsOsWalksWhenNoOsIsBooted)
{
    FakeMem mem; // nothing at $4
    auto ctx = ContextFor(mem);
    auto d = AddressDescriber::Describe(0x00010000, ctx);
    EXPECT_TRUE(d.mapped);
    EXPECT_FALSE(HasKind(d, "memoryHeader"));
    EXPECT_FALSE(HasKind(d, "taskStack"));
}
