#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "AmigaEmulator.h"

namespace adm::core {

// Per-PC aggregated statistics computed from a CPU trace.
// All cycle counts are in CCK units (CYCLE_UNIT / 512).
struct InstructionStats
{
    uint32_t pc;
    uint32_t count;       // number of executions this frame
    uint32_t totalCycles; // sum of elapsed CCKs across all executions
    uint32_t minCycles;   // minimum elapsed CCKs for a single execution
    uint32_t maxCycles;   // maximum elapsed CCKs for a single execution
    std::string mnemonic; // cached disassembly text (filled in by the view)

    float AvgCycles() const
    {
        return count ? static_cast<float>(totalCycles) / static_cast<float>(count) : 0.0f;
    }
};

class CpuProfiler
{
public:
    // Re-compute statistics from a freshly captured trace.
    // Delta for entry i = entries[i+1].absolute_cycle - entries[i].absolute_cycle.
    // Last entry is set to 0 (its duration is unknown — no successor).
    void Update(const AmigaEmulator::CpuTraceEntry* entries, int count);
    void Clear();

    // Flat list sorted by totalCycles descending (hot-path first).
    const std::vector<InstructionStats>& GetSorted() const { return m_sorted; }

    // Per-row delta cycles parallel to the trace input (index i = delta for entry i).
    // Last entry is always 0 (unknown duration).
    const std::vector<uint32_t>& GetDeltaCycles() const { return m_deltaCycles; }

    uint32_t GetFrameVersion() const { return m_frameVersion; }

private:
    std::vector<InstructionStats> m_sorted;
    std::vector<uint32_t> m_deltaCycles;
    uint32_t m_frameVersion = 0;
};

} // namespace adm::core
