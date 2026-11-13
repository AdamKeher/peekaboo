#pragma once

#include <cstdint>
#include <string>

// Shared description of a memory watchpoint, so the MCP layer, the AR console and the
// Breakpoints window all talk about the same thing.
//
// These map onto UAE's memwatch command, "w <slot> <addr> <len> <source> <R/W/I> [V<val>]":
// the watched region is [addr, addr+len), access selects read / write / instruction fetch,
// and source names which agents (CPU, Copper, Blitter, DMA) can trigger it.

namespace adm::core {

struct WatchpointSpec
{
    uint32_t addr = 0;
    // Watched region length in bytes. UAE breaks when an access touches any byte in
    // [addr, addr+len), so this is a genuine range, not just an access width.
    uint32_t len = 2;
    bool read = false;
    bool write = true;
    bool exec = false; // instruction fetch ("I") — fires when the CPU executes from here
    // UAE access-source token: CPU, CPUD, CPUI, COP, BLT, DSK, BPL, SPR, AUD, DMA, ALL.
    // CPU is the sensible default: it restricts to CPU accesses, which are the ones with a
    // meaningful break PC.
    std::string source = "CPU";
    // Fire only when the written value matches (UAE's "V <value>").
    bool valueEnabled = false;
    uint32_t value = 0;
    // Log mode: record the hit and keep running instead of stopping. A read watchpoint over
    // a wide range fires constantly, so breaking on every access is unusable; this is what
    // makes "who touches this region?" answerable in one run.
    bool logOnly = false;
    // Stop recording (and disarm) after this many hits. 0 = unlimited.
    uint32_t maxHits = 0;
    // Remove the watchpoint after it first fires (host-side).
    bool oneShot = false;

    // "r", "w", "rw", "x", "rwx", ... in the order read/write/exec. Empty if nothing is set.
    std::string AccessString() const
    {
        std::string s;
        if (read)
            s += 'r';
        if (write)
            s += 'w';
        if (exec)
            s += 'x';
        return s;
    }
};

struct WatchpointInfo : WatchpointSpec
{
    int slot = -1; // UAE memwatch slot index
    bool enabled = true;
    uint64_t hits = 0;
};

// One recorded access, for log-mode watchpoints.
struct WatchpointHit
{
    int slot = -1;
    uint32_t pc = 0;    // instruction that performed the access
    uint32_t addr = 0;  // exact address touched
    uint32_t value = 0; // value written (0 for reads)
    int rwi = 0;        // bit0=read, bit1=write, bit2=instruction fetch
    int size = 0;       // access width in bytes
    std::string source; // resolved agent name (CPU/Copper/Blitter/...) when known
    uint64_t frame = 0; // emulated frame the hit was recorded on
};

} // namespace adm::core
