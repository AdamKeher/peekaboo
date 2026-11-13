#include "ExocetBlock.h"

namespace adm::core {
namespace {

// A name read from Amiga RAM is bytes in whatever charset the demo used; JSON needs UTF-8, and
// nlohmann's dump() throws on anything else. Printable ASCII is kept, any other byte is '.',
// as CrassMemMap's entry names are.
char Printable(uint8_t c) { return c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.'; }

uint16_t Be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t Be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// The RAM span holding all of [addr, addr+len), in 64 bits so a bank at the top of the address
// space cannot wrap its end to zero.
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

// A null, odd or out-of-RAM pointer is refused before the memory reader sees it: every struct
// here holds words and longs, which gcc aligns to 2 and a 68000 could not have read anyway.
bool ReadStruct(const ExocetRead& read, const std::vector<ExocetRam>& ram, uint32_t addr,
                uint8_t* out, uint32_t len)
{
    if (!read || addr == 0 || (addr & 1) != 0 || !InRam(ram, addr, len))
        return false;
    return read(addr, out, len);
}

} // namespace

bool ReadExocetBlock(const ExocetRead& read, const std::vector<ExocetRam>& ram, uint32_t blockAddr,
                     ExocetBlockState& out)
{
    out = ExocetBlockState{};

    uint8_t b[kExocetBlockSize] = {};
    if (!ReadStruct(read, ram, blockAddr, b, kExocetBlockSize))
        return false;
    if (Be32(b + 0) != kExocetBlockMagic || Be16(b + 4) != kExocetBlockVersion)
        return false;

    ExocetBlockState s;
    s.blockAddress = blockAddr;
    s.version = Be16(b + 4);
    s.tracks = Be16(b + 6);
    if (s.tracks == 0 || s.tracks > kExocetMaxTracks)
        return false;
    s.manifest = Be32(b + 8);
    s.row = Be16(b + 12);
    s.frac = Be16(b + 14);
    s.frame = Be16(b + 16);
    s.clock = Be16(b + 18);
    s.enabled = Be32(b + 20);
    s.generation = Be16(b + 24);
    s.seen = Be16(b + 26);
    s.firedCount = Be16(b + 28);
    s.valuesAddress = Be32(b + 30);
    s.arenaAddress = Be32(b + 34);
    s.arenaBytes = Be32(b + 38);
    if (s.clock > 1)
        return false;

    // The values: one LONG a track
    std::vector<uint8_t> vals(4u * s.tracks);
    if (!ReadStruct(read, ram, s.valuesAddress, vals.data(), static_cast<uint32_t>(vals.size())))
        return false;
    s.values.resize(s.tracks);
    for (uint16_t i = 0; i < s.tracks; ++i)
        s.values[i] = static_cast<int32_t>(Be32(vals.data() + 4 * i));

    // The arena: somewhere a whole exocet.bin header fits, and no bigger than any disk's
    if (s.arenaBytes < kExocetBinHeader || s.arenaBytes > kExocetMaxArena ||
        !InRam(ram, s.arenaAddress, s.arenaBytes) || (s.arenaAddress & 1) != 0)
        return false;
    uint8_t h[kExocetBinHeader];
    if (!ReadStruct(read, ram, s.arenaAddress, h, sizeof h))
        return false;
    if (Be32(h + 0) == kExocetBinMagic && Be16(h + 4) == kExocetBinVersion) {
        s.arena.valid = true;
        s.arena.version = Be16(h + 4);
        s.arena.tracks = Be16(h + 6);
        s.arena.manifest = Be32(h + 8);
        s.arena.song = Be32(h + 12);
        s.arena.rows = Be16(h + 16);
        s.arena.keys = Be16(h + 18);
        s.arena.spans = Be16(h + 20);
        s.arena.frames = Be16(h + 22);
        s.arena.main = Be32(h + 36);
    }

    // The fire ring, oldest first: [firedCount % 16] is the next to be written
    const int n = s.firedCount < kExocetFired ? s.firedCount : kExocetFired;
    for (int i = 0; i < n; ++i) {
        const int slot = (s.firedCount - n + i) % kExocetFired;
        const uint8_t* f = b + 42 + 6 * slot;
        ExocetFire fire{Be16(f + 0), Be16(f + 2), Be16(f + 4)};
        if (fire.track >= s.tracks)
            return false;
        s.fired.push_back(fire);
    }

    // The cut record: an entry per cut value, the ones that have happened
    const uint16_t cutCount = Be16(b + 138);
    const uint32_t cutsAddr = Be32(b + 140);
    if (cutCount > kExocetMaxCuts)
        return false;
    if (cutCount) {
        std::vector<uint8_t> cuts(8u * cutCount);
        if (!ReadStruct(read, ram, cutsAddr, cuts.data(), static_cast<uint32_t>(cuts.size())))
            return false;
        for (uint16_t c = 0; c < cutCount; ++c) {
            const uint8_t* p = cuts.data() + 8 * c;
            ExocetCut cut{c, Be16(p + 0), Be16(p + 2), Be16(p + 4), Be16(p + 6)};
            if (cut.readyFrame != 0xFFFF || cut.cutFrame != 0xFFFF)
                s.cuts.push_back(cut);
        }
    }

    s.flags = Be16(b + 172);
    s.fps = Be16(b + 174);
    if (s.fps != 50 && s.fps != 60)
        return false;
    if (s.flags & kExocetHasMusic) {
        s.reseat = Be16(b + 144);
        s.reseatSeen = Be16(b + 146);
        s.reseatRow = Be16(b + 148);
        s.reseatError = Be16(b + 150);
        s.musicAddress = Be32(b + 152);
        s.musicBytes = Be32(b + 156);
    }
    s.memmapAddress = (s.flags & kExocetHasMemmap) ? Be32(b + 160) : 0;
    if (s.flags & kExocetHasParts) {
        s.partRun = Be16(b + 164);
        s.partLoad = Be16(b + 166);
        // the names, NUL-terminated, an empty one after the last; a bad table is no names
        uint8_t names[256] = {};
        if (ReadStruct(read, ram, Be32(b + 168), names, sizeof names)) {
            size_t at = 0;
            while (at < sizeof names && names[at] != 0 && s.partNames.size() < 32) {
                size_t end = at;
                while (end < sizeof names && names[end] != 0)
                    ++end;
                if (end == sizeof names)
                    break;
                std::string name;
                for (size_t k = at; k < end; ++k)
                    name += Printable(names[k]);
                s.partNames.push_back(std::move(name));
                at = end + 1;
            }
        }
    }
    if (s.flags & kExocetHasOwners) {
        // 8 bytes an owner (letter, r, g, b, the name's address), owner 0 after the last; a bad
        // table stops where it went bad
        uint32_t at = Be32(b + 176);
        for (int i = 0; i < 64; ++i, at += 8) {
            uint8_t o[8];
            if (!ReadStruct(read, ram, at, o, sizeof o) || o[0] == 0)
                break;
            char name[32] = {};
            const uint32_t nameAddr = Be32(o + 4);
            // a name is bytes, at any address: read it a byte at a time, up to its zero
            for (uint32_t k = 0; k + 1 < sizeof name; ++k) {
                uint8_t c = 0;
                if (!read || !InRam(ram, nameAddr + k, 1) || !read(nameAddr + k, &c, 1) || c == 0)
                    break;
                name[k] = Printable(c);
            }
            s.owners.push_back(ExocetOwner{Printable(o[0]), o[1], o[2], o[3], name});
        }
    }

    s.present = true;
    out = std::move(s);
    return true;
}

uint32_t FindExocetBlock(const ExocetRead& read, const std::vector<ExocetRam>& ram,
                         const std::vector<ExocetHostBank>& banks, ExocetBlockState& out)
{
    out = ExocetBlockState{};
    for (const auto& bank : banks) {
        const uint8_t* p = bank.bytes;
        if (!p)
            continue;
        for (uint32_t off = 0; off + 4 <= bank.size; off += 2) {
            if (p[off] != 'E' || p[off + 1] != 'X' || p[off + 2] != 'O' || p[off + 3] != 'B')
                continue;
            ExocetBlockState s;
            if (ReadExocetBlock(read, ram, bank.base + off, s) && s.arena.valid) {
                out = std::move(s);
                return bank.base + off;
            }
        }
    }
    return 0;
}

} // namespace adm::core
