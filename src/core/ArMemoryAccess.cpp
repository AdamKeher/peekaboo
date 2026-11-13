#include "ArMemoryAccess.h"

#include <cstring>

#include <fmt/format.h>

namespace adm::core {

void ArMemoryAccess::SetRegions(std::vector<Region> regions)
{
    m_regions.clear();
    for (auto& r : regions) {
        if (r.hostPtr && r.size > 0)
            m_regions.push_back(std::move(r));
    }
}

const ArMemoryAccess::Region* ArMemoryAccess::FindRegion(uint32_t addr) const
{
    for (const auto& r : m_regions) {
        if (addr >= r.base && addr - r.base < r.size)
            return &r;
    }
    return nullptr;
}

bool ArMemoryAccess::DecodeCia(uint32_t addr, bool& chipB, int& reg)
{
    if (addr < kCiaBase || addr >= kCiaEnd)
        return false;
    reg = static_cast<int>((addr >> 8) & 0x0F);
    // CIA-A: A12 low, odd address (data on D0-D7).  CIA-B: A13 low, even address.
    if ((addr & 0x1000) == 0 && (addr & 1) != 0) {
        chipB = false;
        return true;
    }
    if ((addr & 0x2000) == 0 && (addr & 1) == 0) {
        chipB = true;
        return true;
    }
    return false;
}

bool ArMemoryAccess::ReadOne(uint32_t addr, uint8_t& out) const
{
    if (const Region* r = FindRegion(addr)) {
        out = r->hostPtr[addr - r->base];
        return true;
    }
    if (addr >= kCustomBase && addr < kCustomEnd && readCustomReg) {
        uint16_t w = 0;
        if (readCustomReg((addr - kCustomBase) & ~1u, w)) {
            out = (addr & 1) ? static_cast<uint8_t>(w & 0xFF) : static_cast<uint8_t>(w >> 8);
            return true;
        }
    }
    if (readCiaReg) {
        bool chipB = false;
        int reg = 0;
        if (DecodeCia(addr, chipB, reg) && readCiaReg(chipB, reg, out))
            return true;
    }
    out = 0;
    return false;
}

size_t ArMemoryAccess::Read(uint32_t addr, uint8_t* out, size_t len, std::vector<bool>* valid) const
{
    if (valid) {
        valid->assign(len, false);
    }
    size_t validCount = 0;
    for (size_t i = 0; i < len; ++i) {
        bool ok = ReadOne(addr + static_cast<uint32_t>(i), out[i]);
        if (ok) {
            ++validCount;
            if (valid)
                (*valid)[i] = true;
        }
    }
    return validCount;
}

uint8_t ArMemoryAccess::ReadByte(uint32_t addr, bool* ok) const
{
    uint8_t b = 0;
    bool valid = ReadOne(addr, b);
    if (ok)
        *ok = valid;
    return b;
}

uint16_t ArMemoryAccess::ReadWord(uint32_t addr, bool* ok) const
{
    bool okHi = false, okLo = false;
    uint8_t hi = ReadByte(addr, &okHi);
    uint8_t lo = ReadByte(addr + 1, &okLo);
    if (ok)
        *ok = okHi && okLo;
    return static_cast<uint16_t>((hi << 8) | lo);
}

uint32_t ArMemoryAccess::ReadLong(uint32_t addr, bool* ok) const
{
    bool okHi = false, okLo = false;
    uint16_t hi = ReadWord(addr, &okHi);
    uint16_t lo = ReadWord(addr + 2, &okLo);
    if (ok)
        *ok = okHi && okLo;
    return (static_cast<uint32_t>(hi) << 16) | lo;
}

bool ArMemoryAccess::WriteCustomBytes(uint32_t addr, const uint8_t* data, size_t len,
                                      std::string& err)
{
    if (!writeCustomReg) {
        err = "CUSTOM REGISTER SPACE NOT WRITABLE";
        return false;
    }
    // Compose full 16-bit words; missing halves come from the read snapshot.
    size_t i = 0;
    while (i < len) {
        uint32_t byteAddr = addr + static_cast<uint32_t>(i);
        uint32_t wordOffset = (byteAddr - kCustomBase) & ~1u;
        uint16_t word = 0;
        if (readCustomReg)
            readCustomReg(wordOffset, word);

        if ((byteAddr & 1) == 0) {
            word = static_cast<uint16_t>((data[i] << 8) | (word & 0x00FF));
            if (i + 1 < len) {
                word = static_cast<uint16_t>((word & 0xFF00) | data[i + 1]);
                ++i;
            }
        } else {
            word = static_cast<uint16_t>((word & 0xFF00) | data[i]);
        }
        ++i;

        if (!writeCustomReg(wordOffset, word)) {
            err = fmt::format("CUSTOM REGISTER WRITE FAILED AT {:08X}", kCustomBase + wordOffset);
            return false;
        }
    }
    return true;
}

bool ArMemoryAccess::WriteCiaByte(uint32_t addr, uint8_t value, std::string& err)
{
    if (!writeCiaReg) {
        err = "CIA REGISTER SPACE NOT WRITABLE";
        return false;
    }
    bool chipB = false;
    int reg = 0;
    if (!DecodeCia(addr, chipB, reg)) {
        // An address in CIA space with the wrong parity for either chip select: nothing
        // answers it on real hardware, so there is no register to write.
        err = fmt::format("NO CIA RESPONDS AT {:08X}", addr);
        return false;
    }
    if (!writeCiaReg(chipB, reg, value)) {
        err = fmt::format("CIA REGISTER WRITE FAILED AT {:08X}", addr);
        return false;
    }
    return true;
}

bool ArMemoryAccess::Write(uint32_t addr, const uint8_t* data, size_t len, std::string& err)
{
    if (len == 0)
        return true;

    // Validate the whole range first so partial writes never happen.
    for (size_t i = 0; i < len; ++i) {
        uint32_t a = addr + static_cast<uint32_t>(i);
        const Region* r = FindRegion(a);
        if (r) {
            if (!r->writable) {
                err = fmt::format("WRITE PROTECTED ({}) AT {:08X}", r->name, a);
                return false;
            }
        } else if (a >= kCustomBase && a < kCustomEnd) {
            if (!writeCustomReg) {
                err = "CUSTOM REGISTER SPACE NOT WRITABLE";
                return false;
            }
        } else if (a >= kCiaBase && a < kCiaEnd) {
            if (!writeCiaReg) {
                err = "CIA REGISTER SPACE NOT WRITABLE";
                return false;
            }
            bool chipB = false;
            int reg = 0;
            if (!DecodeCia(a, chipB, reg)) {
                err = fmt::format("NO CIA RESPONDS AT {:08X}", a);
                return false;
            }
        } else {
            err = fmt::format("UNMAPPED ADDRESS {:08X}", a);
            return false;
        }
    }

    size_t i = 0;
    while (i < len) {
        uint32_t a = addr + static_cast<uint32_t>(i);
        if (const Region* r = FindRegion(a)) {
            // Contiguous run within this region
            size_t run = 0;
            while (i + run < len && (a + run) - r->base < r->size)
                ++run;
            memcpy(r->hostPtr + (a - r->base), data + i, run);
            i += run;
        } else if (a >= kCiaBase && a < kCiaEnd) {
            // CIA registers are 8-bit and sparsely decoded (one register per 256 bytes),
            // so they are written one byte at a time rather than as a run.
            if (!WriteCiaByte(a, data[i], err))
                return false;
            ++i;
        } else {
            // Custom space run
            size_t run = 0;
            while (i + run < len && !FindRegion(a + static_cast<uint32_t>(run)) &&
                   a + run < kCustomEnd)
                ++run;
            if (!WriteCustomBytes(a, data + i, run, err))
                return false;
            i += run;
        }
    }
    return true;
}

} // namespace adm::core
