#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Curated tables of watchable hardware registers, used by the Breakpoints window's
// symbolic register picker (so you can watch "DMACON" instead of hand-entering
// $DFF096) and by watchpoint hit info to name the register an access touched.
//
// Each entry carries a sensible default watch width: pointer registers (COPxLC,
// BLTxPT, BPLxPT, SPRxPT, audio LC) are Long; everything else is Word, except CIA
// registers which are Byte. Source defaults are intentionally left to the caller —
// register writes frequently come from the Copper, but defaulting to anything but
// CPU makes continuously-rewritten registers (e.g. COLOR00) break every frame.
namespace adm::ui {

struct WatchRegister
{
    std::string name;
    uint32_t address;
    int size; // 1, 2 or 4 bytes
};

struct WatchRegisterGroup
{
    std::string label;
    std::vector<WatchRegister> regs;
};

namespace detail {

inline void AddCustomRegs(std::vector<WatchRegister>& v)
{
    constexpr uint32_t kBase = 0xDFF000;
    auto w = [&](const char* n, uint32_t off, int sz) { v.push_back({n, kBase + off, sz}); };
    // DMA / interrupt control (the classic watch targets)
    w("DMACON", 0x096, 2);
    w("INTENA", 0x09A, 2);
    w("INTREQ", 0x09C, 2);
    w("ADKCON", 0x09E, 2);
    // Copper
    w("COP1LC", 0x080, 4);
    w("COP2LC", 0x084, 4);
    w("COPJMP1", 0x088, 2);
    w("COPJMP2", 0x08A, 2);
    w("COPCON", 0x02E, 2);
    // Display / bitplane control
    w("DIWSTRT", 0x08E, 2);
    w("DIWSTOP", 0x090, 2);
    w("DDFSTRT", 0x092, 2);
    w("DDFSTOP", 0x094, 2);
    w("BPLCON0", 0x100, 2);
    w("BPLCON1", 0x102, 2);
    w("BPLCON2", 0x104, 2);
    w("BPLCON3", 0x106, 2);
    w("BPL1MOD", 0x108, 2);
    w("BPL2MOD", 0x10A, 2);
    // Bitplane pointers
    for (int i = 0; i < 6; ++i)
        w(("BPL" + std::to_string(i + 1) + "PT").c_str(), 0x0E0 + i * 4, 4);
    // Blitter
    w("BLTCON0", 0x040, 2);
    w("BLTCON1", 0x042, 2);
    w("BLTAFWM", 0x044, 2);
    w("BLTALWM", 0x046, 2);
    w("BLTCPT", 0x048, 4);
    w("BLTBPT", 0x04C, 4);
    w("BLTAPT", 0x050, 4);
    w("BLTDPT", 0x054, 4);
    w("BLTSIZE", 0x058, 2);
    w("BLTCMOD", 0x060, 2);
    w("BLTBMOD", 0x062, 2);
    w("BLTAMOD", 0x064, 2);
    w("BLTDMOD", 0x066, 2);
    // Sprites
    for (int i = 0; i < 8; ++i)
        w(("SPR" + std::to_string(i) + "PT").c_str(), 0x120 + i * 4, 4);
    // Audio (LC long, the rest word)
    for (int i = 0; i < 4; ++i) {
        const uint32_t ab = 0x0A0 + i * 0x10;
        w(("AUD" + std::to_string(i) + "LC").c_str(), ab + 0x0, 4);
        w(("AUD" + std::to_string(i) + "LEN").c_str(), ab + 0x4, 2);
        w(("AUD" + std::to_string(i) + "PER").c_str(), ab + 0x6, 2);
        w(("AUD" + std::to_string(i) + "VOL").c_str(), ab + 0x8, 2);
    }
    // Disk
    w("DSKPT", 0x020, 4);
    w("DSKLEN", 0x024, 2);
    w("DSKSYNC", 0x07E, 2);
    // Colours
    for (int i = 0; i < 32; ++i) {
        char nm[12];
        snprintf(nm, sizeof(nm), "COLOR%02d", i);
        w(nm, 0x180 + i * 2, 2);
    }
}

// CIA registers are byte-wide. CIA-A sits at odd addresses from $BFE001, CIA-B at
// even addresses from $BFD000; both step $100 per register index.
inline void AddCiaRegs(std::vector<WatchRegister>& v, uint32_t base)
{
    static const char* kNames[16] = {"PRA",  "PRB",  "DDRA",   "DDRB",   "TALO",  "TAHI",
                                     "TBLO", "TBHI", "TODLOW", "TODMID", "TODHI", "",
                                     "SDR",  "ICR",  "CRA",    "CRB"};
    for (int i = 0; i < 16; ++i) {
        if (kNames[i][0] == '\0')
            continue; // reserved slot
        v.push_back({kNames[i], base + static_cast<uint32_t>(i) * 0x100, 1});
    }
}

} // namespace detail

inline const std::vector<WatchRegisterGroup>& WatchRegisterGroups()
{
    static const std::vector<WatchRegisterGroup> groups = [] {
        std::vector<WatchRegisterGroup> g;
        WatchRegisterGroup custom{"Custom ($DFF000)", {}};
        detail::AddCustomRegs(custom.regs);
        g.push_back(std::move(custom));
        WatchRegisterGroup ciaa{"CIA-A ($BFE001)", {}};
        detail::AddCiaRegs(ciaa.regs, 0xBFE001);
        g.push_back(std::move(ciaa));
        WatchRegisterGroup ciab{"CIA-B ($BFD000)", {}};
        detail::AddCiaRegs(ciab.regs, 0xBFD000);
        g.push_back(std::move(ciab));
        return g;
    }();
    return groups;
}

// Reverse lookup: name of the register exactly at addr, or empty. Used by hit info to
// label the accessed address (e.g. "$DFF096 DMACON").
inline std::string WatchRegisterNameAt(uint32_t addr)
{
    for (const auto& grp : WatchRegisterGroups())
        for (const auto& r : grp.regs)
            if (r.address == addr)
                return r.name;
    return {};
}

} // namespace adm::ui
