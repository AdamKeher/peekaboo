#pragma once

#include <cstdint>
#include <vector>

#include "AmigaEmulator.h"

namespace adm::core {

// Enriched custom-register write, computed from a raw RegWriteEvent.
struct RegWriteRecord
{
    AmigaEmulator::RegWriteEvent raw; // Raw hardware capture

    // L-reference: beam-relative timestamp (Hardware Cycle Offset) in CCK units.
    // Formula: vpos * 228 + hpos — the same join key the CPU, Copper and Blitter
    // traces use, which is what lets a row click sync every other debugger window.
    uint32_t lRef;

    // Previous value written to this register earlier in the same frame. Equals
    // `raw.value` when this is the first write (see firstWrite).
    uint16_t prevValue;

    // Whether this write actually changed the register relative to the previous
    // write this frame. Drives the "changed values only" filter — a copper list
    // re-writing the same colour every frame is noise, a value that moves is not.
    bool changed;

    // Whether this is the first write to this register in the frame.
    bool firstWrite;
};

class RegWriteProfiler
{
public:
    // Rebuild the frame from an already-fetched event buffer. Call once per emulated frame.
    // The caller does the fetch (as Application already does for copper events), which keeps
    // this class free of any emulator/UI dependency and therefore unit-testable on its own.
    void UpdateFromEvents(const AmigaEmulator::RegWriteEvent* events, int count);

    void Clear();

    const std::vector<RegWriteRecord>& GetCurrentFrame() const { return m_currentFrame; }

    // Monotonically-increasing counter, incremented each time new data arrives.
    uint32_t GetFrameVersion() const { return m_frameVersion; }

    struct FrameStats
    {
        int totalWrites;
        int cpuWrites;
        int copperWrites;
        int changedWrites;     // writes whose value differed from the previous one
        int distinctRegisters; // how many different registers were touched
    };
    FrameStats ComputeStats() const;

private:
    std::vector<RegWriteRecord> m_currentFrame;
    uint32_t m_frameVersion = 0;
};

} // namespace adm::core
