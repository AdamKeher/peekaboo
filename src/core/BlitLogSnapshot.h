#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adm::core {

// One blitter operation from the current frame, mirroring a row of the "Blitter Operations"
// window. Self-contained (no emulator/profiler types) so the AR console BLIT command and the
// MCP get_blitter_log tool can consume it without pulling in AmigaEmulator/BlitProfiler.
struct BlitLogEntry
{
    uint32_t startCycle = 0; // absolute CCK — the L####### key shown by the window
    uint32_t lRef = 0;       // beam-relative offset (vpos*228 + hpos)
    uint16_t startLine = 0;  // VLine
    uint16_t startClock = 0; // Clock (hpos)
    bool fromCopper = false; // trigger: Copper vs CPU
    std::string op;          // friendly op label ("copy"/"line"/…) or "-" when unrecognized
    bool lineMode = false;   // BLTCON1 bit0 (line-draw mode)
    uint16_t widthPx = 0;    // wordsPerRow * 16
    uint16_t rows = 0;
    uint16_t wordsPerRow = 0;
    uint32_t aptr = 0, bptr = 0, cptr = 0, dptr = 0;
    int16_t amod = 0, bmod = 0, cmod = 0, dmod = 0;
    bool useA = false, useB = false, useC = false, useD = false;
    uint8_t minterm = 0;
    uint16_t bltcon0 = 0, bltcon1 = 0;
    uint32_t dmaCycles = 0;
};

// Snapshot of the current frame's blitter activity, produced on demand by Application from the
// BlitProfiler and consumed by the AR console BLIT command + the MCP get_blitter_log tool.
// `blits` is truncated to the caller's limit; `truncated`/`totalBlits` report the full count.
struct BlitLogSnapshot
{
    bool available = false; // emulator running / profiler present
    int totalBlits = 0;     // total blits this frame (before any limit)
    int cpuBlits = 0;
    int copperBlits = 0;
    uint32_t totalDmaCycles = 0;
    float busPercent = 0.0f; // totalDmaCycles / 71364 * 100
    bool truncated = false;  // true when more blits exist than were returned
    std::vector<BlitLogEntry> blits;
};

} // namespace adm::core
