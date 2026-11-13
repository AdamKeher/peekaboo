#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "../src/core/CrassMemMap.h"

using adm::core::CrassFreeChunk;
using adm::core::CrassMemHeader;
using adm::core::CrassMemMapRead;
using adm::core::CrassMemMapTable;
using adm::core::CrassMemRegion;
using adm::core::CrassMemSegment;
using adm::core::ExocetRam;
using adm::core::LayOutCrassMemory;
using adm::core::ReadCrassMemMap;

namespace {

// Chip at the bottom, slow RAM at $C00000, nothing between: an A500's, as the table sees it.
constexpr uint32_t kChipBase = 0x000000;
constexpr uint32_t kChipSize = 0x080000;
constexpr uint32_t kSlowBase = 0xC00000;
constexpr uint32_t kSlowSize = 0x080000;

class FakeRam
{
public:
    std::vector<ExocetRam> Banks() const
    {
        return {{kChipBase, kChipSize}, {kSlowBase, kSlowSize}};
    }

    void W8(uint32_t addr, uint8_t v) { m_bytes[addr] = v; }
    void W16(uint32_t addr, uint16_t v)
    {
        W8(addr, static_cast<uint8_t>(v >> 8));
        W8(addr + 1, static_cast<uint8_t>(v));
    }
    void W32(uint32_t addr, uint32_t v)
    {
        W16(addr, static_cast<uint16_t>(v >> 16));
        W16(addr + 2, static_cast<uint16_t>(v));
    }
    void Str(uint32_t addr, const std::string& s)
    {
        for (size_t i = 0; i < s.size(); ++i)
            W8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
    }

    // A read that bumps the table's generation after `after` reads - a change landing between
    // the reader's two looks at the header
    int bumpAfter = -1;
    uint32_t bumpAt = 0;

    adm::core::ExocetRead Reader()
    {
        return [this](uint32_t addr, uint8_t* out, uint32_t len) {
            if (bumpAfter == 0)
                W16(bumpAt, static_cast<uint16_t>(Get16(bumpAt) + 2));
            if (bumpAfter >= 0)
                --bumpAfter;
            for (uint32_t i = 0; i < len; ++i) {
                const uint32_t a = addr + i;
                const bool mapped = (a >= kChipBase && a < kChipBase + kChipSize) ||
                                    (a >= kSlowBase && a < kSlowBase + kSlowSize);
                if (!mapped)
                    return false;
                auto it = m_bytes.find(a);
                out[i] = (it == m_bytes.end()) ? 0 : it->second;
            }
            return true;
        };
    }

private:
    uint16_t Get16(uint32_t a) { return static_cast<uint16_t>((m_bytes[a] << 8) | m_bytes[a + 1]); }
    std::map<uint32_t, uint8_t> m_bytes;
};

// engine/memmap.h, kept literal so a change to it has to be made here too
constexpr uint32_t kTable = 0x00C000;

struct E
{
    uint32_t addr, size;
    uint16_t line;
    char owner;
    uint8_t flags;
    const char* name;
};

void WriteTable(FakeRam& ram, const std::vector<E>& entries, uint16_t capacity = 8,
                uint16_t generation = 40, uint32_t base = kTable)
{
    ram.W32(base + 0, 0x4D4D4150); // "MMAP"
    ram.W16(base + 4, 1);
    ram.W16(base + 6, 32);
    ram.W16(base + 8, capacity);
    ram.W16(base + 10, static_cast<uint16_t>(entries.size()));
    ram.W16(base + 12, static_cast<uint16_t>(entries.size() + 1)); // peak
    ram.W16(base + 14, 2);                                         // dropped
    ram.W16(base + 16, generation);
    ram.W32(base + 20, 57);
    for (size_t i = 0; i < entries.size(); ++i) {
        const uint32_t p = base + 24 + 32 * static_cast<uint32_t>(i);
        ram.W32(p + 0, entries[i].addr);
        ram.W32(p + 4, entries[i].size);
        ram.W16(p + 8, entries[i].line);
        ram.W8(p + 10, static_cast<uint8_t>(entries[i].owner));
        ram.W8(p + 11, entries[i].flags);
        ram.Str(p + 12, entries[i].name);
    }
}

const std::vector<E> kEntries = {
    {0xC22008, 75448, 968, 'P', 0x02, "anim.bin"},
    {0x0059F0, 6144, 968, 'P', 0x03, "disk buffer"},
    {0xC218C0, 34, 30, 'P', 0x00, "diskload.c"},
    {0x04A000, 20000, 1369, 'S', 0x01, "large_scroll_twist."},
};

} // namespace

TEST(CrassMemMapTest, ReadsATable)
{
    FakeRam ram;
    WriteTable(ram, kEntries);
    CrassMemMapTable t;
    ASSERT_EQ(ReadCrassMemMap(ram.Reader(), ram.Banks(), kTable, t), CrassMemMapRead::Ok);
    EXPECT_TRUE(t.present);
    EXPECT_EQ(t.address, kTable);
    EXPECT_EQ(t.capacity, 8);
    EXPECT_EQ(t.count, 4);
    EXPECT_EQ(t.peak, 5);
    EXPECT_EQ(t.dropped, 2);
    EXPECT_EQ(t.changes, 57u);
    ASSERT_EQ(t.entries.size(), 4u);
    // by address
    EXPECT_EQ(t.entries[0].addr, 0x0059F0u);
    EXPECT_EQ(t.entries[0].name, "disk buffer");
    EXPECT_TRUE(t.entries[0].Chip());
    EXPECT_TRUE(t.entries[0].Named());
    EXPECT_EQ(t.entries[0].Label(), "disk buffer");
    EXPECT_EQ(t.entries[1].owner, 'S');
    // a name as long as the field has no terminator in it, and stops there
    EXPECT_EQ(t.entries[1].name, "large_scroll_twist.");
    EXPECT_EQ(t.entries[1].Label(), "large_scroll_twist.:1369");
    EXPECT_EQ(t.entries[2].Label(), "diskload.c:30");
    EXPECT_EQ(t.entries[3].name, "anim.bin");
}

// The owners' names are the demo's (its block's owner table): a letter it does not name is itself
TEST(CrassMemMapTest, OwnerNamesComeFromTheDemo)
{
    adm::core::ExocetMemoryState m;
    m.owners = {{'S', 95, 190, 95, "scroller"}, {'X', 1, 2, 3, "tunnel"}};
    EXPECT_EQ(m.OwnerName('S'), "scroller");
    EXPECT_EQ(m.OwnerName('X'), "tunnel");
    EXPECT_EQ(m.OwnerName('A'), "A"); // Sneezeman's animation, not this demo's
    ASSERT_NE(m.Owner('X'), nullptr);
    EXPECT_EQ(m.Owner('X')->b, 3);
    EXPECT_EQ(m.Owner('Q'), nullptr);
}

TEST(CrassMemMapTest, AnOddGenerationIsAChangeInProgressShownAsItStood)
{
    FakeRam ram;
    WriteTable(ram, kEntries, 8, 41);
    CrassMemMapTable t;
    EXPECT_EQ(ReadCrassMemMap(ram.Reader(), ram.Banks(), kTable, t), CrassMemMapRead::Changing);
    EXPECT_TRUE(t.present);
    EXPECT_TRUE(t.changing);
    EXPECT_EQ(t.entries.size(), 4u);
}

TEST(CrassMemMapTest, AHalfWrittenEntryIsLeftOutOfATableBeingChanged)
{
    // mm_add stopped between counting the entry and writing its size
    FakeRam ram;
    WriteTable(ram, kEntries, 8, 41);
    ram.W32(kTable + 24 + 32 * 3 + 4, 0);
    CrassMemMapTable t;
    EXPECT_EQ(ReadCrassMemMap(ram.Reader(), ram.Banks(), kTable, t), CrassMemMapRead::Changing);
    EXPECT_EQ(t.entries.size(), 3u);
    // the same in a table that is not being changed is not a table
    WriteTable(ram, kEntries, 8, 40);
    ram.W32(kTable + 24 + 32 * 3 + 4, 0);
    EXPECT_EQ(ReadCrassMemMap(ram.Reader(), ram.Banks(), kTable, t), CrassMemMapRead::Invalid);
}

TEST(CrassMemMapTest, AChangeBetweenTheTwoLooksIsCaught)
{
    FakeRam ram;
    WriteTable(ram, kEntries);
    ram.bumpAfter = 2; // the header, the entries, then the header again sees a new generation
    ram.bumpAt = kTable + 16;
    CrassMemMapTable t;
    EXPECT_EQ(ReadCrassMemMap(ram.Reader(), ram.Banks(), kTable, t), CrassMemMapRead::Changing);
    EXPECT_TRUE(t.changing);
}

TEST(CrassMemMapTest, RefusesWhatIsNotATable)
{
    struct Case
    {
        const char* what;
        uint32_t addr;
        uint32_t value;
        int width;
    };
    const Case cases[] = {
        {"magic", kTable + 0, 0x45584F42, 32},
        {"version", kTable + 4, 2, 16},
        {"entry size", kTable + 6, 28, 16},
        {"no capacity", kTable + 8, 0, 16},
        {"count past capacity", kTable + 10, 9, 16},
        {"peak past capacity", kTable + 12, 9, 16},
        {"an empty entry", kTable + 24 + 4, 0, 32},
        {"an entry past 4G", kTable + 24 + 0, 0xFFFFFF00, 32},
    };
    for (const auto& c : cases) {
        FakeRam ram;
        WriteTable(ram, kEntries);
        if (c.width == 32)
            ram.W32(c.addr, c.value);
        else
            ram.W16(c.addr, static_cast<uint16_t>(c.value));
        CrassMemMapTable t;
        t.present = true;
        EXPECT_EQ(ReadCrassMemMap(ram.Reader(), ram.Banks(), kTable, t), CrassMemMapRead::Invalid)
            << c.what;
        EXPECT_FALSE(t.present) << c.what;
    }
    FakeRam ram;
    WriteTable(ram, kEntries);
    CrassMemMapTable t;
    EXPECT_EQ(ReadCrassMemMap(ram.Reader(), ram.Banks(), kTable + 1, t), CrassMemMapRead::Invalid);
    EXPECT_EQ(ReadCrassMemMap(ram.Reader(), ram.Banks(), 0, t), CrassMemMapRead::Invalid);
    EXPECT_EQ(ReadCrassMemMap(ram.Reader(), ram.Banks(), 0x100000, t), CrassMemMapRead::Invalid);
    // a table near the top of chip whose capacity would run its entries off the bank
    const uint32_t top = kChipSize - 64;
    WriteTable(ram, {}, 8, 40, top);
    EXPECT_EQ(ReadCrassMemMap(ram.Reader(), ram.Banks(), top, t), CrassMemMapRead::Invalid);
    WriteTable(ram, {}, 1, 40, top);
    EXPECT_EQ(ReadCrassMemMap(ram.Reader(), ram.Banks(), top, t), CrassMemMapRead::Ok);
}

namespace {

CrassMemMapTable TableOf(const std::vector<E>& es)
{
    CrassMemMapTable t;
    t.present = true;
    for (const auto& e : es) {
        adm::core::CrassMemMapEntry x;
        x.addr = e.addr;
        x.size = e.size;
        x.line = e.line;
        x.owner = e.owner;
        x.flags = e.flags;
        x.name = e.name;
        t.entries.push_back(x);
    }
    return t;
}

using K = CrassMemSegment::Kind;

} // namespace

TEST(CrassMemMapTest, LaysEntriesAndFreeChunksOverARegion)
{
    // $1000-$2000: in use $1000-$1100 (untracked), entry $1100-$1400, free $1400-$1800,
    // entry $1800-$1900, untracked $1900-$1A00, free $1A00-$2000
    const CrassMemMapTable t =
        TableOf({{0x1800, 0x100, 12, 'P', 2, "b"}, {0x1100, 0x300, 11, 'S', 2, "a"}});
    CrassMemHeader h;
    h.name = "chip memory";
    h.attributes = 0x3;
    h.lower = 0x1000;
    h.upper = 0x2000;
    h.free = 0x400 + 0x600;
    h.chunks = {{0x1400, 0x400}, {0x1A00, 0x600}};
    const auto regions = LayOutCrassMemory({h}, t);
    ASSERT_EQ(regions.size(), 1u);
    const CrassMemRegion& r = regions[0];
    EXPECT_TRUE(r.chip);
    EXPECT_EQ(r.free, 0xA00u);
    EXPECT_EQ(r.largestFree, 0x600u);
    EXPECT_EQ(r.freeChunks, 2);
    EXPECT_EQ(r.allocated, 0x400u);
    EXPECT_EQ(r.untracked, 0x200u);
    EXPECT_EQ(r.overlaps, 0);
    const std::vector<std::pair<uint32_t, K>> want = {
        {0x1000, K::Untracked}, {0x1100, K::Allocated}, {0x1400, K::Free},
        {0x1800, K::Allocated}, {0x1900, K::Untracked}, {0x1A00, K::Free}};
    ASSERT_EQ(r.segments.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(r.segments[i].start, want[i].first) << i;
        EXPECT_EQ(r.segments[i].kind, want[i].second) << i;
        if (i + 1 < want.size())
            EXPECT_EQ(r.segments[i].end, want[i + 1].first) << i;
    }
    EXPECT_EQ(r.segments.back().end, 0x2000u);
    // an allocated segment names its entry
    EXPECT_EQ(t.entries[static_cast<size_t>(r.segments[1].entry)].name, "a");
    EXPECT_EQ(t.entries[static_cast<size_t>(r.segments[3].entry)].name, "b");
}

TEST(CrassMemMapTest, AnEntryOverFreeMemoryIsAnOverlapAndExecIsBelieved)
{
    // a stale entry across a free chunk: the free chunk shows, the entry only where it is not
    const CrassMemMapTable t = TableOf({{0x1000, 0x800, 1, 'P', 2, "stale"}});
    CrassMemHeader h;
    h.attributes = 0x5;
    h.lower = 0x1000;
    h.upper = 0x2000;
    h.chunks = {{0x1400, 0x200}};
    const auto regions = LayOutCrassMemory({h}, t);
    ASSERT_EQ(regions.size(), 1u);
    const CrassMemRegion& r = regions[0];
    EXPECT_FALSE(r.chip);
    EXPECT_EQ(r.overlaps, 1);
    ASSERT_EQ(r.segments.size(), 4u);
    EXPECT_EQ(r.segments[0].kind, K::Allocated);
    EXPECT_EQ(r.segments[0].end, 0x1400u);
    EXPECT_EQ(r.segments[1].kind, K::Free);
    EXPECT_EQ(r.segments[2].kind, K::Allocated);
    EXPECT_EQ(r.segments[2].start, 0x1600u);
    EXPECT_EQ(r.segments[2].end, 0x1800u);
    EXPECT_EQ(r.segments[3].kind, K::Untracked);
}

TEST(CrassMemMapTest, EntriesOutsideARegionAreLeftToTheirOwn)
{
    const CrassMemMapTable t =
        TableOf({{0xC00100, 0x100, 1, 'P', 2, "slow"}, {0x000100, 0x100, 2, 'P', 3, "chip"}});
    CrassMemHeader chip;
    chip.attributes = 0x3;
    chip.lower = 0x0;
    chip.upper = 0x1000;
    CrassMemHeader slow;
    slow.attributes = 0x5;
    slow.lower = 0xC00000;
    slow.upper = 0xC01000;
    const auto regions = LayOutCrassMemory({chip, slow}, t);
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(regions[0].allocated, 0x100u);
    EXPECT_EQ(regions[1].allocated, 0x100u);
    EXPECT_EQ(regions[0].untracked, 0xF00u);
    EXPECT_EQ(regions[1].segments[1].kind, K::Allocated);
    EXPECT_EQ(t.entries[static_cast<size_t>(regions[1].segments[1].entry)].name, "slow");
}
