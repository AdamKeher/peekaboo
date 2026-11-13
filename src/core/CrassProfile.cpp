#include "CrassProfile.h"

#include <algorithm>

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

// The RAM span holding all of [addr, addr+len), or null when it straddles a gap or falls
// outside every bank. Arithmetic is done in 64 bits so a bank reaching the top of the address
// space cannot wrap its end to zero.
const CrassProfileRam* FindRam(const std::vector<CrassProfileRam>& ram, uint32_t addr, uint32_t len)
{
    for (const auto& r : ram) {
        if (r.size == 0)
            continue;
        const uint64_t end = static_cast<uint64_t>(r.base) + r.size;
        if (addr >= r.base && static_cast<uint64_t>(addr) + len <= end)
            return &r;
    }
    return nullptr;
}

// Reads one Amiga struct. A null pointer, an odd one (every struct here holds UWORD/ULONG
// members, so gcc aligns them to 2 and the 68000 would bus-error on an odd one anyway), or one
// pointing outside RAM is rejected without troubling the memory reader.
bool ReadStruct(const CrassProfileRead& read, const std::vector<CrassProfileRam>& ram,
                uint32_t addr, uint8_t* out, uint32_t len)
{
    if (!read || addr == 0 || (addr & 1) != 0 || !FindRam(ram, addr, len))
        return false;
    return read(addr, out, len);
}

// Reads a section name. The read is clipped to the end of the name's own bank so a string
// sitting at the very top of RAM is not rejected for a fixed-size read running off the end.
// An unterminated, empty or non-printable name fails: that is exactly what a stale pointer
// into reused memory looks like, and catching it is the point of this whole reader.
bool ReadName(const CrassProfileRead& read, const std::vector<CrassProfileRam>& ram, uint32_t addr,
              std::string& out)
{
    const CrassProfileRam* bank = (read && addr != 0) ? FindRam(ram, addr, 1) : nullptr;
    if (!bank)
        return false;

    const uint64_t bankEnd = static_cast<uint64_t>(bank->base) + bank->size;
    const uint32_t len =
        static_cast<uint32_t>(std::min<uint64_t>(kCrassProfileMaxNameLength + 1, bankEnd - addr));
    uint8_t buf[kCrassProfileMaxNameLength + 1] = {};
    if (!read(addr, buf, len))
        return false;

    for (uint32_t i = 0; i < len; ++i) {
        if (buf[i] == 0) {
            if (i == 0)
                return false; // PROFILE_DEFINE always supplies a name
            out.assign(reinterpret_cast<const char*>(buf), i);
            return true;
        }
        if (buf[i] < 0x20 || buf[i] > 0x7E)
            return false;
    }
    return false; // no terminator inside the first 31 characters
}

} // namespace

bool ReadCrassProfile(const CrassProfileRead& read, const std::vector<CrassProfileRam>& ram,
                      uint32_t blockAddr, CrassProfileStats& out)
{
    out = CrassProfileStats{};

    uint8_t blk[kCrassProfileBlockSize];
    if (!ReadStruct(read, ram, blockAddr, blk, kCrassProfileBlockSize))
        return false;
    if (Be32(blk + 0) != kCrassProfileMagic)
        return false;
    const uint16_t version = Be16(blk + 4);
    if (version != kCrassProfileVersion)
        return false;
    // A PAL/NTSC field pair is 624-626 lines. Anything else is not a line counter.
    const uint16_t linesPer2Fields = Be16(blk + 8);
    if (linesPer2Fields < 620 || linesPer2Fields > 630)
        return false;

    uint8_t fr[kCrassProfileFramesSize];
    if (!ReadStruct(read, ram, Be32(blk + 12), fr, kCrassProfileFramesSize))
        return false;

    CrassProfileStats s;
    s.blockAddress = blockAddr;
    s.version = version;
    s.windowFields = Be16(blk + 6);
    s.linesPer2Fields = linesPer2Fields;
    s.timingOk = Be16(blk + 10) != 0;

    s.rendered = Be16(fr + 10);
    s.worstGap = Be16(fr + 12);
    s.lastFields = Be16(fr + 14);
    s.lastRendered = Be16(fr + 16);
    s.lastDropped = Be16(fr + 18);
    s.lastWorstGap = Be16(fr + 20);
    s.totalRendered = Be32(fr + 22);
    s.totalDropped = Be32(fr + 26);

    // Walk the section list. Sections link themselves in lazily on their first START, so the
    // head moves as the program runs and is re-read every frame; the cap and the visited set
    // bound a list that reused memory has turned into a ring.
    uint32_t node = Be32(blk + 16);
    std::vector<uint32_t> visited;
    visited.reserve(kCrassProfileMaxSections);
    while (node != 0) {
        if (static_cast<int>(s.sections.size()) >= kCrassProfileMaxSections) {
            s.sectionsTruncated = true;
            break;
        }
        if (std::find(visited.begin(), visited.end(), node) != visited.end())
            return false;
        visited.push_back(node);

        uint8_t sec[kCrassProfileSectionSize];
        if (!ReadStruct(read, ram, node, sec, kCrassProfileSectionSize))
            return false;

        CrassProfileSection cs;
        if (!ReadName(read, ram, Be32(sec + 0), cs.name))
            return false;
        cs.total = Be32(sec + 14);
        cs.count = Be32(sec + 18);
        cs.min = Be16(sec + 22);
        cs.max = Be16(sec + 24);
        cs.lastMin = Be16(sec + 26);
        cs.lastAvg = Be16(sec + 28);
        cs.lastMax = Be16(sec + 30);
        cs.lastCount = Be32(sec + 32);
        cs.lastLines = Be16(sec + 36);
        s.sections.push_back(std::move(cs));

        node = Be32(sec + 4);
    }

    s.present = true;
    out = std::move(s);
    return true;
}

} // namespace adm::core
