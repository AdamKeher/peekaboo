#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace adm::core {

// Reader for the Crass engine's in-program raster-line profiler (engine/profile.h, compiled in
// by a production's `#define PROFILE`). The program counts raster lines out of CIA-B's HSYNC
// time-of-day counter and keeps exact per-window frame statistics and per-section min/avg/max;
// it publishes them by registering a ProfileBlock through the standard Bartman
// debug_cmd(barto_cmd_register_resource) path as resource type 3.
//
// The host then reads that block out of Amiga RAM once per host frame, which costs the emulated
// machine nothing. That is the whole point: an armed watchpoint over the same memory perturbs
// timing enough to drop the very fields being counted.
//
// Everything in the block is big-endian in m68k gcc layout: 2-byte alignment, 16-bit BOOL,
// 32-bit pointers. A program can exit and have its memory reused while its registry entry still
// points at the old block, so every read is validated from scratch and any inconsistency simply
// reports "not present" rather than drawing whatever was there.

// One named section - a PROFILE_START/PROFILE_STOP pair - read from a ProfileSection node.
struct CrassProfileSection
{
    std::string name;
    uint16_t lastLines = 0; // lines measured by the most recent STOP (live)

    // Current, incomplete window.
    uint32_t total = 0; // lines
    uint32_t count = 0; // stops
    uint16_t min = 0;   // 0xFFFF until the first STOP of the window
    uint16_t max = 0;

    // Last completed window.
    uint16_t lastMin = 0;
    uint16_t lastAvg = 0;
    uint16_t lastMax = 0;
    uint32_t lastCount = 0; // 0 => the section was not called during that window
};

// A validated snapshot of the whole ProfileBlock. `present` false means every other field is at
// its default and nothing should be drawn.
struct CrassProfileStats
{
    bool present = false;
    uint32_t blockAddress = 0;
    uint16_t version = 0;
    uint16_t windowFields = 0;    // fields per report window (250 by default)
    uint16_t linesPer2Fields = 0; // 626 long frames, 624 short, 625 interlace
    bool timingOk = false;        // false => the line counter was never verified at init

    // Frame statistics (ProfileFrames).
    uint16_t rendered = 0;   // frames rendered so far this window (live)
    uint16_t worstGap = 0;   // longest single frame so far this window, in fields (live)
    uint16_t lastFields = 0; // 0 => no window has completed yet
    uint16_t lastRendered = 0;
    uint16_t lastDropped = 0;
    uint16_t lastWorstGap = 0;
    uint32_t totalRendered = 0; // since profile_init()
    uint32_t totalDropped = 0;

    std::vector<CrassProfileSection> sections;
    bool sectionsTruncated = false; // the list held more than kCrassProfileMaxSections
};

// Reads `len` bytes at `addr`. Must return false unless every byte is mapped and valid.
using CrassProfileRead = std::function<bool(uint32_t addr, uint8_t* out, uint32_t len)>;

// A span of mapped RAM. Every pointer in the block must lie wholly inside one of these; the
// caller passes chip/slow/fast, not ROM and not the chip-register space.
struct CrassProfileRam
{
    uint32_t base = 0;
    uint32_t size = 0;
};

inline constexpr uint32_t kCrassProfileMagic = 0x50524631u; // "PRF1"
inline constexpr uint16_t kCrassProfileVersion = 1;
inline constexpr int kCrassProfileMaxSections = 16;
inline constexpr uint32_t kCrassProfileMaxNameLength = 31;

// On-Amiga structure sizes, as m68k gcc lays them out.
inline constexpr uint32_t kCrassProfileBlockSize = 20;
inline constexpr uint32_t kCrassProfileFramesSize = 30;
inline constexpr uint32_t kCrassProfileSectionSize = 38;

// Reads and validates the ProfileBlock at `blockAddr`. Returns false - leaving `out` at its
// "not present" default - on a bad magic or version, an implausible line count, any pointer
// outside `ram`, a cyclic section list, or a section name that is not a short printable
// NUL-terminated string. Those are the normal path, not edge cases: the registry outlives the
// program that filled it in.
bool ReadCrassProfile(const CrassProfileRead& read, const std::vector<CrassProfileRam>& ram,
                      uint32_t blockAddr, CrassProfileStats& out);

} // namespace adm::core
