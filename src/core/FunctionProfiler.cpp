#include "FunctionProfiler.h"

#include <algorithm>
#include <unordered_map>

#include "DebugSymbols.h"

namespace adm::core {

void FunctionProfiler::Update(const std::vector<InstructionStats>& perPc,
                              const std::vector<AmigaSegment>& segments, const DebugSymbols& syms)
{
    m_sorted.clear();
    m_resolvedCycles = 0;
    m_unresolvedCycles = 0;

    // The code hunk is segment 0; runtime PCs in it de-relocate to link-time
    // addresses by subtracting its base. Without a segment list (no running
    // process) or symbols, nothing can be attributed.
    const bool haveCode = !segments.empty();
    const uint32_t base = haveCode ? segments.front().base : 0;
    const uint32_t codeEnd = haveCode ? base + segments.front().size : 0;

    // Member map: Update runs every frame, and rebuilding the table from scratch reallocated
    // the whole bucket array each time. clear() keeps the buckets.
    auto& byFunc = m_byFunc;
    byFunc.clear();

    for (const InstructionStats& s : perPc) {
        const FunctionSymbol* fn = nullptr;
        if (haveCode && s.pc >= base && s.pc < codeEnd) {
            fn = syms.ResolvePc(s.pc - base);
        }
        if (!fn) {
            m_unresolvedCycles += s.totalCycles;
            continue;
        }
        m_resolvedCycles += s.totalCycles;
        auto& fs = byFunc[fn->address];
        if (fs.count == 0) {
            fs.name = fn->name;
            fs.address = fn->address;
        }
        fs.totalCycles += s.totalCycles;
        fs.count += s.count;
    }

    const uint32_t frameTotal = m_resolvedCycles + m_unresolvedCycles;
    m_sorted.reserve(byFunc.size());
    for (auto& [addr, fs] : byFunc) {
        fs.pctFrame =
            frameTotal ? static_cast<float>(fs.totalCycles) / static_cast<float>(frameTotal) : 0.0f;
        m_sorted.push_back(std::move(fs));
    }

    std::sort(m_sorted.begin(), m_sorted.end(), [](const FunctionStats& a, const FunctionStats& b) {
        return a.totalCycles > b.totalCycles;
    });

    ++m_frameVersion;
}

void FunctionProfiler::Clear()
{
    m_sorted.clear();
    m_resolvedCycles = 0;
    m_unresolvedCycles = 0;
    ++m_frameVersion;
}

} // namespace adm::core
