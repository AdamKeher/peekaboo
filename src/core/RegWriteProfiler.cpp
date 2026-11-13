#include "RegWriteProfiler.h"

#include <array>

#include "Constants.h"

namespace adm::core {

// PAL frame timing (see adm::core::constants::beam for the shared definitions).
static constexpr uint32_t kCCKPerLine = constants::beam::CCKPerLine;

// One slot per addressable custom register (offset 0x000-0x1FE, word-aligned).
static constexpr int kRegSlots = 256;

void RegWriteProfiler::UpdateFromEvents(const AmigaEmulator::RegWriteEvent* events, int count)
{
    if (count <= 0 || !events) {
        m_currentFrame.clear();
        return;
    }

    // Per-register running value, so `changed` compares against the previous write to the
    // same register rather than the previous row. Reset each frame: the log is per-frame,
    // so the first write to a register in this frame is reported as firstWrite.
    std::array<uint16_t, kRegSlots> lastValue{};
    std::array<bool, kRegSlots> seen{};

    m_currentFrame.clear();
    m_currentFrame.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
        const AmigaEmulator::RegWriteEvent& e = events[i];

        RegWriteRecord rec{};
        rec.raw = e;
        rec.lRef = static_cast<uint32_t>(e.vpos) * kCCKPerLine + static_cast<uint32_t>(e.hpos);

        // The core already masks the offset to 0x1FE, but this is host-side code reading a
        // DLL buffer — clamp rather than trust it, so a skewed core can't index out of range.
        const int slot = (e.reg >> 1) & (kRegSlots - 1);

        rec.firstWrite = !seen[static_cast<size_t>(slot)];
        rec.prevValue = rec.firstWrite ? e.value : lastValue[static_cast<size_t>(slot)];
        rec.changed = rec.firstWrite || (e.value != rec.prevValue);

        seen[static_cast<size_t>(slot)] = true;
        lastValue[static_cast<size_t>(slot)] = e.value;

        m_currentFrame.push_back(rec);
    }

    ++m_frameVersion;
}

void RegWriteProfiler::Clear()
{
    m_currentFrame.clear();
    ++m_frameVersion;
}

RegWriteProfiler::FrameStats RegWriteProfiler::ComputeStats() const
{
    FrameStats stats{};
    stats.totalWrites = static_cast<int>(m_currentFrame.size());

    std::array<bool, kRegSlots> touched{};
    for (const RegWriteRecord& rec : m_currentFrame) {
        if (rec.raw.fromCopper)
            ++stats.copperWrites;
        else
            ++stats.cpuWrites;
        if (rec.changed)
            ++stats.changedWrites;
        touched[static_cast<size_t>((rec.raw.reg >> 1) & (kRegSlots - 1))] = true;
    }
    for (bool t : touched) {
        if (t)
            ++stats.distinctRegisters;
    }
    return stats;
}

} // namespace adm::core
