#include "CrassMemMap.h"

#include <algorithm>

#include <fmt/format.h>

namespace adm::core {
namespace {

uint16_t Be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t Be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool InRam(const std::vector<ExocetRam>& ram, uint32_t addr, uint32_t len)
{
    for (const auto& r : ram) {
        if (r.size == 0)
            continue;
        const uint64_t end = static_cast<uint64_t>(r.base) + r.size;
        if (addr >= r.base && static_cast<uint64_t>(addr) + len <= end)
            return true;
    }
    return false;
}

std::string EntryName(const uint8_t* p)
{
    std::string s;
    for (uint32_t i = 0; i < 20 && p[i]; ++i) {
        const char c = static_cast<char>(p[i]);
        s += (c >= 32 && c < 127) ? c : '.';
    }
    return s;
}

} // namespace

bool CrassMemMapEntry::Chip() const { return (flags & kCrassMemMapChip) != 0; }

bool CrassMemMapEntry::Named() const { return (flags & kCrassMemMapNamed) != 0; }

std::string CrassMemMapEntry::Label() const
{
    if (Named() || line == 0)
        return name;
    return fmt::format("{}:{}", name, line);
}

CrassMemMapRead ReadCrassMemMap(const ExocetRead& read, const std::vector<ExocetRam>& ram,
                                uint32_t addr, CrassMemMapTable& out)
{
    out = CrassMemMapTable{};
    if (!read || addr == 0 || (addr & 1) != 0 || !InRam(ram, addr, kCrassMemMapHeaderSize))
        return CrassMemMapRead::Invalid;
    uint8_t h[kCrassMemMapHeaderSize];
    if (!read(addr, h, sizeof h))
        return CrassMemMapRead::Invalid;
    if (Be32(h + 0) != kCrassMemMapMagic || Be16(h + 4) != kCrassMemMapVersion ||
        Be16(h + 6) != kCrassMemMapEntrySize)
        return CrassMemMapRead::Invalid;

    CrassMemMapTable t;
    t.address = addr;
    t.version = Be16(h + 4);
    t.capacity = Be16(h + 8);
    t.count = Be16(h + 10);
    t.peak = Be16(h + 12);
    t.dropped = Be16(h + 14);
    t.generation = Be16(h + 16);
    t.changes = Be32(h + 20);
    if (t.capacity == 0 || t.capacity > kCrassMemMapMaxCapacity || t.count > t.capacity ||
        t.peak > t.capacity)
        return CrassMemMapRead::Invalid;
    t.changing = (t.generation & 1) != 0;

    const uint32_t bytes = static_cast<uint32_t>(t.count) * kCrassMemMapEntrySize;
    if (!InRam(ram, addr + kCrassMemMapHeaderSize,
               static_cast<uint32_t>(t.capacity) * kCrassMemMapEntrySize))
        return CrassMemMapRead::Invalid;
    std::vector<uint8_t> raw(bytes);
    if (bytes && !read(addr + kCrassMemMapHeaderSize, raw.data(), bytes))
        return CrassMemMapRead::Invalid;
    // The header again: a change that began after it was read shows as a new generation
    if (!read(addr, h, sizeof h) || Be16(h + 16) != t.generation)
        t.changing = true;

    t.entries.reserve(t.count);
    for (uint16_t i = 0; i < t.count; ++i) {
        const uint8_t* p = raw.data() + kCrassMemMapEntrySize * i;
        CrassMemMapEntry e;
        e.addr = Be32(p + 0);
        e.size = Be32(p + 4);
        e.line = Be16(p + 8);
        e.owner = static_cast<char>(p[10]);
        e.flags = p[11];
        e.name = EntryName(p + 12);
        if (e.size == 0 || static_cast<uint64_t>(e.addr) + e.size > 0x100000000ull) {
            // the entry being written when the machine stopped
            if (t.changing)
                continue;
            return CrassMemMapRead::Invalid;
        }
        t.entries.push_back(std::move(e));
    }
    std::sort(t.entries.begin(), t.entries.end(),
              [](const CrassMemMapEntry& a, const CrassMemMapEntry& b) { return a.addr < b.addr; });
    t.present = true;
    const bool changing = t.changing;
    out = std::move(t);
    return changing ? CrassMemMapRead::Changing : CrassMemMapRead::Ok;
}

std::vector<CrassMemRegion> LayOutCrassMemory(const std::vector<CrassMemHeader>& headers,
                                              const CrassMemMapTable& table)
{
    struct Span
    {
        uint32_t start, end;
        int entry;
    };
    std::vector<CrassMemRegion> out;
    for (const auto& mh : headers) {
        if (mh.upper <= mh.lower)
            continue;
        CrassMemRegion r;
        r.name = mh.name;
        r.chip = (mh.attributes & 0x2) != 0;
        r.lower = mh.lower;
        r.upper = mh.upper;
        r.free = mh.free;

        // Clipped to the header's span: exec's free chunks, and the table's entries
        std::vector<Span> free, used;
        for (const auto& c : mh.chunks) {
            const uint32_t s = std::max(c.addr, mh.lower);
            const uint64_t e =
                std::min<uint64_t>(static_cast<uint64_t>(c.addr) + c.bytes, mh.upper);
            if (e <= s)
                continue;
            free.push_back({s, static_cast<uint32_t>(e), -1});
            r.freeChunks++;
            r.largestFree = std::max(r.largestFree, c.bytes);
        }
        for (size_t i = 0; i < table.entries.size(); ++i) {
            const auto& en = table.entries[i];
            const uint32_t s = std::max(en.addr, mh.lower);
            const uint64_t e =
                std::min<uint64_t>(static_cast<uint64_t>(en.addr) + en.size, mh.upper);
            if (e <= s)
                continue;
            used.push_back({s, static_cast<uint32_t>(e), static_cast<int>(i)});
        }
        const auto byStart = [](const Span& a, const Span& b) { return a.start < b.start; };
        std::sort(free.begin(), free.end(), byStart);
        std::sort(used.begin(), used.end(), byStart);

        // An entry over free memory is a table that is wrong or stale: counted, and exec's list
        // is believed
        for (const auto& u : used)
            for (const auto& f : free)
                if (u.start < f.end && f.start < u.end) {
                    r.overlaps++;
                    break;
                }

        // The gaps between free chunks are in use: by the entries there, and by what the table
        // does not know about in between them
        const auto fillGap = [&](uint32_t a, uint32_t b) {
            uint32_t at = a;
            for (const auto& u : used) {
                if (u.end <= at || u.start >= b)
                    continue;
                const uint32_t s = std::max(u.start, at);
                const uint32_t e = std::min(u.end, b);
                if (s > at)
                    r.segments.push_back({at, s, CrassMemSegment::Kind::Untracked, -1});
                r.segments.push_back({s, e, CrassMemSegment::Kind::Allocated, u.entry});
                at = e;
            }
            if (at < b)
                r.segments.push_back({at, b, CrassMemSegment::Kind::Untracked, -1});
        };
        uint32_t at = mh.lower;
        for (const auto& f : free) {
            if (f.start < at)
                continue; // chunks do not overlap in a sane list
            if (f.start > at)
                fillGap(at, f.start);
            r.segments.push_back({f.start, f.end, CrassMemSegment::Kind::Free, -1});
            at = f.end;
        }
        if (at < mh.upper)
            fillGap(at, mh.upper);

        for (const auto& seg : r.segments) {
            const uint32_t n = seg.end - seg.start;
            if (seg.kind == CrassMemSegment::Kind::Allocated)
                r.allocated += n;
            else if (seg.kind == CrassMemSegment::Kind::Untracked)
                r.untracked += n;
        }
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace adm::core
