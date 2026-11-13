#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace adm::core {

// Read/write access to the emulated Amiga address space via host-mapped memory
// regions, with the custom-chip register block routed through injected hooks.
class ArMemoryAccess
{
public:
    struct Region
    {
        uint8_t* hostPtr = nullptr;
        uint32_t base = 0;
        uint32_t size = 0;
        std::string name;
        bool writable = false;
    };

    static constexpr uint32_t kCustomBase = 0xDFF000;
    static constexpr uint32_t kCustomEnd = 0xDFF200;

    // CIA space. On an A500 the two 8520s are selected by address lines rather than a
    // dense register map: CIA-A answers when A12 is low and A0 is high (canonically
    // $BFE001 + reg*$100), CIA-B when A13 is low and A0 is low ($BFD000 + reg*$100).
    // Both chips have 16 registers, selected by A8-A11.
    static constexpr uint32_t kCiaBase = 0xBFD000;
    static constexpr uint32_t kCiaEnd = 0xBFF000;

    // Decodes a CIA address. Returns false when no chip answers at `addr` (an address
    // in CIA space with the wrong parity for either chip select - reading one on real
    // hardware returns bus junk, so it is not something to invent a value for).
    // chipB is true for CIA-B, false for CIA-A; reg is 0-15.
    static bool DecodeCia(uint32_t addr, bool& chipB, int& reg);

    void SetRegions(std::vector<Region> regions);
    const std::vector<Region>& Regions() const { return m_regions; }
    const Region* FindRegion(uint32_t addr) const;
    bool HasRegions() const { return !m_regions.empty(); }

    // Custom-chip space hooks (offset is relative to kCustomBase, word-aligned).
    // Reads should come from a side-effect-free snapshot; writes go to the chipset.
    std::function<bool(uint32_t offset, uint16_t& out)> readCustomReg;
    std::function<bool(uint32_t offset, uint16_t value)> writeCustomReg;

    // CIA register read hook. Like readCustomReg this must come from a side-effect-free
    // snapshot: reading ICR on the real chip clears it, and a debugger read must not.
    // `reg` is 0-15; `chipB` selects CIA-B over CIA-A.
    std::function<bool(bool chipB, int reg, uint8_t& out)> readCiaReg;

    // CIA register write hook. Unlike the read hook this goes straight to the chip, with
    // all the side effects a real write has (loading timer latches, acknowledging
    // interrupts). `reg` is 0-15; `chipB` selects CIA-B over CIA-A.
    std::function<bool(bool chipB, int reg, uint8_t value)> writeCiaReg;

    // Reads len bytes; unmapped bytes are zero-filled and flagged false in valid.
    // Returns the number of valid bytes.
    size_t Read(uint32_t addr, uint8_t* out, size_t len, std::vector<bool>* valid = nullptr) const;

    uint8_t ReadByte(uint32_t addr, bool* ok = nullptr) const;
    uint16_t ReadWord(uint32_t addr, bool* ok = nullptr) const;
    uint32_t ReadLong(uint32_t addr, bool* ok = nullptr) const;

    // Writes len bytes. Fails (with message) if any byte is unmapped or read-only.
    bool Write(uint32_t addr, const uint8_t* data, size_t len, std::string& err);

private:
    bool ReadOne(uint32_t addr, uint8_t& out) const;
    bool WriteCustomBytes(uint32_t addr, const uint8_t* data, size_t len, std::string& err);
    bool WriteCiaByte(uint32_t addr, uint8_t value, std::string& err);

    std::vector<Region> m_regions;
};

} // namespace adm::core
