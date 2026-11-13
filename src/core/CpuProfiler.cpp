#include "CpuProfiler.h"

#include <algorithm>

namespace adm::core {

void CpuProfiler::Update(const AmigaEmulator::CpuTraceEntry* entries, int count)
{
    m_sorted.clear();
    m_deltaCycles.clear();

    if (count <= 0) {
        ++m_frameVersion;
        return;
    }

    m_deltaCycles.resize(static_cast<size_t>(count), 0u);

    // Build per-row delta cycles and accumulate per-PC stats.
    // delta[i] = entries[i+1].absolute_cycle - entries[i].absolute_cycle.
    // Unsigned subtraction handles uint32_t wrap correctly when delta < 2^31.
    // Last entry gets 0 (no successor, duration unknown).
    std::unordered_map<uint32_t, InstructionStats> map;
    map.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
        uint32_t delta = 0u;
        if (i < count - 1)
            delta = entries[i + 1].absolute_cycle - entries[i].absolute_cycle;
        m_deltaCycles[static_cast<size_t>(i)] = delta;

        // Skip the last entry from aggregation (unknown duration)
        if (i == count - 1)
            continue;

        auto& s = map[entries[i].pc];
        if (s.count == 0) {
            s.pc = entries[i].pc;
            s.minCycles = delta;
            s.maxCycles = delta;
        } else {
            if (delta < s.minCycles)
                s.minCycles = delta;
            if (delta > s.maxCycles)
                s.maxCycles = delta;
        }
        ++s.count;
        s.totalCycles += delta;
    }

    m_sorted.reserve(map.size());
    for (auto& [pc, stat] : map)
        m_sorted.push_back(stat);

    std::sort(m_sorted.begin(), m_sorted.end(),
              [](const InstructionStats& a, const InstructionStats& b) {
                  return a.totalCycles > b.totalCycles;
              });

    ++m_frameVersion;
}

void CpuProfiler::Clear()
{
    m_sorted.clear();
    m_deltaCycles.clear();
    ++m_frameVersion;
}

} // namespace adm::core
