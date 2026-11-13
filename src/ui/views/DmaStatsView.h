#pragma once

#include <imgui.h>
#include <array>
#include <cstdint>

#include "../../core/AmigaEmulator.h"

namespace adm::ui {

class DmaStatsView
{
public:
    explicit DmaStatsView(adm::AmigaEmulator* emu);

    void Open() { m_isOpen = true; }
    void Restore()
    {
        m_isOpen = true;
        m_noFocusOnAppearing = true;
    }
    void Close() { m_isOpen = false; }
    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    void Draw();

private:
    // --- Channel indexing for history / peaks ---
    enum Channel : int {
        kCopper,
        kBlitter,
        kCpu,
        kBitplane,
        kSprite,
        kDisk,
        kAudio,
        kOther,
        kIdle,
        kChannelCount
    };

    static constexpr int kHistorySize = 256;        // frames of history
    static constexpr float kPeakDecayRate = 0.997f; // per-frame decay for rolling peak

    adm::AmigaEmulator* m_emu = nullptr;
    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;
    bool m_wasOpen = false; // track previous state for auto-enable/disable of debug_dma

    adm::AmigaEmulator::DmaFrameStats m_stats{};

    // History ring buffer: m_history[channel][frame]
    std::array<std::array<float, kHistorySize>, kChannelCount> m_history{};
    int m_historyPos = 0;
    // Emulator frame of the last history sample — the graph only advances on a new emulated frame,
    // so it freezes while the emulator is paused instead of scrolling stale repeated data.
    uint64_t m_lastHistoryFrame = UINT64_MAX;
    uint64_t m_lastStatsRun = UINT64_MAX;

    // Rolling peak per channel
    std::array<float, kChannelCount> m_peaks{};

    void UpdateStats();
    void PushHistory();
    void DrawChannelBar(ImDrawList* dl, const char* label, uint32_t slots, uint32_t maxSlots,
                        ImU32 color, float peakSlots, ImVec2 pos, float barWidth, float barHeight);
    void DrawStackedBar(ImDrawList* dl, ImVec2 pos, float barWidth, float barHeight);
    void DrawGraph(ImDrawList* dl, ImVec2 pos, float graphWidth, float graphHeight);
};

} // namespace adm::ui
