#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "CpuProfiler.h"     // InstructionStats
#include "SegListResolver.h" // AmigaSegment

namespace adm::core {

class DebugSymbols;

/// Per-function aggregation of a frame's CPU cycles.
struct FunctionStats
{
    std::string name;         // resolved function name
    uint32_t address = 0;     // link-time function start (st_value)
    uint32_t totalCycles = 0; // summed CCKs across all PCs in this function
    uint32_t count = 0;       // instruction executions attributed to it
    float pctFrame = 0.0f;    // share of the frame's total traced cycles

    [[nodiscard]] float AvgCycles() const
    {
        return count ? static_cast<float>(totalCycles) / static_cast<float>(count) : 0.0f;
    }
};

/**
 * @brief Rolls per-PC CPU stats up into per-function totals for the scrubber.
 *
 * This is Stage 3: it ties together the three earlier pieces. It takes the
 * per-PC `InstructionStats` produced by CpuProfiler, the running program's
 * segment list (from SegListResolver, so runtime PCs can be de-relocated to
 * link-time addresses) and the ELF symbols (from DebugSymbols, to name them).
 *
 * Only PCs inside the program's first (code) segment are attributed to
 * functions; everything else — Kickstart ROM, library code, gaps between
 * functions — accumulates into the "unresolved" bucket so percentages stay
 * honest. Like CpuProfiler this class is decoupled from the live emulator: the
 * caller supplies the already-captured stats and segments.
 */
class FunctionProfiler
{
public:
    /**
     * @param perPc     Per-PC stats for the frame (CpuProfiler::GetSorted()).
     * @param segments  The running process's seglist; index 0 is the code hunk.
     *                  Empty => nothing can be resolved (all cycles unresolved).
     * @param syms      ELF symbols; if not loaded, nothing resolves.
     */
    void Update(const std::vector<InstructionStats>& perPc,
                const std::vector<AmigaSegment>& segments, const DebugSymbols& syms);

    void Clear();

    /// Functions sorted by totalCycles descending (hot-path first).
    [[nodiscard]] const std::vector<FunctionStats>& GetSorted() const { return m_sorted; }

    [[nodiscard]] uint32_t GetResolvedCycles() const { return m_resolvedCycles; }
    [[nodiscard]] uint32_t GetUnresolvedCycles() const { return m_unresolvedCycles; }
    [[nodiscard]] uint32_t GetTotalCycles() const { return m_resolvedCycles + m_unresolvedCycles; }
    [[nodiscard]] uint32_t GetFrameVersion() const { return m_frameVersion; }

private:
    std::vector<FunctionStats> m_sorted;
    // Scratch for Update(); a member so the per-frame rebuild reuses the bucket array.
    std::unordered_map<uint32_t, FunctionStats> m_byFunc;
    uint32_t m_resolvedCycles = 0;
    uint32_t m_unresolvedCycles = 0;
    uint32_t m_frameVersion = 0;
};

} // namespace adm::core
