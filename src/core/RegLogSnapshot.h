#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adm::core {

// One custom-register write from the current frame, mirroring a row of the "Register
// Operations" window. Self-contained (no emulator/profiler types) so the AR console REGLOG
// command and the MCP get_register_log tool can consume it without pulling in
// AmigaEmulator/RegWriteProfiler.
struct RegLogEntry
{
    uint32_t lRef = 0;       // beam-relative offset (vpos*228 + hpos) — the L####### key
    uint16_t line = 0;       // vpos
    uint16_t clock = 0;      // hpos, in CCK
    uint16_t reg = 0;        // register offset (0x000-0x1FE)
    uint16_t value = 0;      // value written
    uint16_t prevValue = 0;  // previous value written to this register this frame
    bool changed = false;    // value differed from the previous write
    bool firstWrite = false; // first write to this register this frame
    bool fromCopper = false; // source: Copper vs CPU
    uint32_t pc = 0;         // writing CPU instruction, or the copper MOVE's address
    std::string name;        // register name ("COLOR00"), or "-" when unknown
    std::string description; // one-line register description, or empty
};

// Snapshot of the current frame's custom-register writes, produced on demand by Application
// from the RegWriteProfiler and consumed by the AR console REGLOG command + the MCP
// get_register_log tool. `writes` is truncated to the caller's limit; `truncated`/`totalWrites`
// report the full count.
//
// Covers CPU and Copper writes only — chipset activity that bypasses the register-write path
// (sprite DMA, bitplane pointer auto-increment) is not represented. See
// AmigaEmulator::RegWriteEvent.
struct RegLogSnapshot
{
    bool available = false;    // emulator running / profiler present
    bool coreSupported = true; // false when the loaded DLL predates the register-log patch
    int totalWrites = 0;       // total writes this frame (before any limit)
    int cpuWrites = 0;
    int copperWrites = 0;
    int changedWrites = 0;
    int distinctRegisters = 0;
    bool truncated = false; // true when more writes exist than were returned
    std::vector<RegLogEntry> writes;
};

} // namespace adm::core
