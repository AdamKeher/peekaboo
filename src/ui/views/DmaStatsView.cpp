#include "DmaStatsView.h"

#include <fmt/format.h>
#include <algorithm>
#include <cstring>

#include "../../core/Constants.h"
#include "../DmaColors.h"
#include "../IconsFontAwesome6.h"
#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

using DmaSlotRecord = adm::AmigaEmulator::DmaSlotRecord;
using DmaFrameStats = adm::AmigaEmulator::DmaFrameStats;

// Channel colors in enum order
static constexpr ImU32 kChannelColors[] = {
    kDmaColorCopper,   // kCopper
    kDmaColorBlitter,  // kBlitter
    kDmaColorCpu,      // kCpu
    kDmaColorBitplane, // kBitplane
    kDmaColorSprite,   // kSprite
    kDmaColorDisk,     // kDisk
    kDmaColorAudio,    // kAudio
    kDmaColorOther,    // kOther
    kDmaColorIdle,     // kIdle
};

DmaStatsView::DmaStatsView(adm::AmigaEmulator* emu) : m_emu(emu) {}

void DmaStatsView::UpdateStats()
{
    m_stats = DmaFrameStats{};

    DmaSlotRecord* records = nullptr;
    int hposCount = 0, vposCount = 0;
    if (!m_emu->GetDmaRecord(&records, hposCount, vposCount) || !records)
        return;

    // The DMA buffer (NR_DMA_REC_HPOS=288 × NR_DMA_REC_VPOS=1000) is much larger than an
    // actual Amiga frame (228 CCK × 313 lines for PAL). Using the buffer dimensions as the
    // scale denominator would make all bars appear 4× smaller than meaningful. Instead, track
    // the highest scanline with non-idle DMA activity and use 228 CCK/line × frameLines.
    int maxActiveVpos = -1;

    for (int v = 0; v < vposCount; ++v) {
        for (int h = 0; h < hposCount; ++h) {
            const auto& rec = records[v * hposCount + h];
            switch (rec.type) {
                case AmigaEmulator::kDmaRecordCopper:
                    m_stats.copperSlots++;
                    maxActiveVpos = v;
                    break;
                case AmigaEmulator::kDmaRecordBlitter:
                    m_stats.blitterSlots++;
                    maxActiveVpos = v;
                    break;
                case AmigaEmulator::kDmaRecordCpu:
                    m_stats.cpuSlots++;
                    maxActiveVpos = v;
                    break;
                case AmigaEmulator::kDmaRecordBitplane:
                    m_stats.bitplaneSlots++;
                    maxActiveVpos = v;
                    break;
                case AmigaEmulator::kDmaRecordSprite:
                    m_stats.spriteSlots++;
                    maxActiveVpos = v;
                    break;
                case AmigaEmulator::kDmaRecordDisk:
                    m_stats.diskSlots++;
                    maxActiveVpos = v;
                    break;
                case AmigaEmulator::kDmaRecordAudio:
                    m_stats.audioSlots++;
                    maxActiveVpos = v;
                    break;
                case AmigaEmulator::kDmaRecordRefresh:
                case AmigaEmulator::kDmaRecordConflict:
                    m_stats.otherSlots++;
                    maxActiveVpos = v;
                    break;
                case AmigaEmulator::kDmaRecordNone:
                default:
                    break;
            }
        }
    }

    // Fall back to 313 PAL lines if no active DMA was found this frame.
    constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;
    static constexpr uint32_t kFallbackLines = 313u;
    uint32_t frameLines =
        (maxActiveVpos >= 0) ? static_cast<uint32_t>(maxActiveVpos + 1) : kFallbackLines;
    m_stats.maxSlots = kCCKPerLine * frameLines;

    // Active DMA slots (non-idle, within the actual hardware frame)
    uint32_t activeSlots = m_stats.copperSlots + m_stats.blitterSlots + m_stats.cpuSlots +
                           m_stats.bitplaneSlots + m_stats.spriteSlots + m_stats.diskSlots +
                           m_stats.audioSlots + m_stats.otherSlots;
    // Idle = all non-active CCK cycles in the actual frame (not buffer padding)
    m_stats.idleSlots = (m_stats.maxSlots > activeSlots) ? m_stats.maxSlots - activeSlots : 0;
    m_stats.totalSlots = m_stats.maxSlots;
}

void DmaStatsView::PushHistory()
{
    float vals[kChannelCount] = {
        static_cast<float>(m_stats.copperSlots), static_cast<float>(m_stats.blitterSlots),
        static_cast<float>(m_stats.cpuSlots),    static_cast<float>(m_stats.bitplaneSlots),
        static_cast<float>(m_stats.spriteSlots), static_cast<float>(m_stats.diskSlots),
        static_cast<float>(m_stats.audioSlots),  static_cast<float>(m_stats.otherSlots),
        static_cast<float>(m_stats.idleSlots),
    };

    for (int c = 0; c < kChannelCount; ++c) {
        m_history[c][m_historyPos] = vals[c];
        // Rolling peak: decay towards current, jump up to new highs
        m_peaks[c] = std::max(vals[c], m_peaks[c] * kPeakDecayRate);
    }
    m_historyPos = (m_historyPos + 1) % kHistorySize;
}

void DmaStatsView::DrawChannelBar(ImDrawList* dl, const char* label, uint32_t slots,
                                  uint32_t maxSlots, ImU32 color, float peakSlots, ImVec2 pos,
                                  float barWidth, float barHeight)
{
    // Track background
    dl->AddRectFilled(pos, {pos.x + barWidth, pos.y + barHeight}, IM_COL32(30, 30, 30, 255));

    // Filled portion
    if (maxSlots > 0 && slots > 0) {
        float fillW = (static_cast<float>(slots) / static_cast<float>(maxSlots)) * barWidth;
        fillW = std::min(fillW, barWidth);
        dl->AddRectFilled(pos, {pos.x + fillW, pos.y + barHeight}, color);
    }

    // Rolling peak indicator
    if (maxSlots > 0 && peakSlots > 0.0f) {
        float peakX = (peakSlots / static_cast<float>(maxSlots)) * barWidth;
        peakX = std::min(peakX, barWidth);
        float lineX = pos.x + peakX;
        // Vertical tick mark
        dl->AddLine({lineX, pos.y + 1.0f}, {lineX, pos.y + barHeight - 1.0f},
                    IM_COL32(255, 255, 255, 140), 1.5f);
    }

    // Label on the left
    float fontSize = ImGui::GetFontSize() * 0.85f;
    dl->AddText(ImGui::GetFont(), fontSize, {pos.x + 4.0f, pos.y + (barHeight - fontSize) * 0.5f},
                IM_COL32(255, 255, 255, 220), label);

    // Slot count on the right
    std::string slotsText = fmt::format("{}", slots);
    ImVec2 textSize = ImGui::CalcTextSize(slotsText.c_str());
    dl->AddText(
        ImGui::GetFont(), fontSize,
        {pos.x + barWidth - textSize.x * 0.85f - 4.0f, pos.y + (barHeight - fontSize) * 0.5f},
        IM_COL32(255, 255, 255, 180), slotsText.c_str());
}

void DmaStatsView::DrawStackedBar(ImDrawList* dl, ImVec2 pos, float barWidth, float barHeight)
{
    // Track background
    dl->AddRectFilled(pos, {pos.x + barWidth, pos.y + barHeight}, IM_COL32(30, 30, 30, 255));

    if (m_stats.maxSlots == 0)
        return;

    struct Segment
    {
        uint32_t slots;
        ImU32 color;
    };
    Segment segments[] = {
        {m_stats.bitplaneSlots, kDmaColorBitplane}, {m_stats.copperSlots, kDmaColorCopper},
        {m_stats.blitterSlots, kDmaColorBlitter},   {m_stats.cpuSlots, kDmaColorCpu},
        {m_stats.spriteSlots, kDmaColorSprite},     {m_stats.diskSlots, kDmaColorDisk},
        {m_stats.audioSlots, kDmaColorAudio},       {m_stats.otherSlots, kDmaColorOther},
        {m_stats.idleSlots, kDmaColorIdle},
    };

    float x = pos.x;
    for (const auto& seg : segments) {
        if (seg.slots == 0)
            continue;
        float w = (static_cast<float>(seg.slots) / static_cast<float>(m_stats.maxSlots)) * barWidth;
        if (w < 1.0f)
            w = 1.0f;
        dl->AddRectFilled({x, pos.y}, {x + w, pos.y + barHeight}, seg.color);
        x += w;
    }
}

void DmaStatsView::DrawGraph(ImDrawList* dl, ImVec2 pos, float graphWidth, float graphHeight)
{
    // Background
    dl->AddRectFilled(pos, {pos.x + graphWidth, pos.y + graphHeight}, IM_COL32(20, 20, 20, 255));
    dl->AddRect(pos, {pos.x + graphWidth, pos.y + graphHeight}, IM_COL32(60, 60, 60, 180));

    // Find maximum value across all channels (exclude idle for better scaling)
    float maxVal = 1.0f;
    for (int c = 0; c < kChannelCount; ++c) {
        if (c == kIdle)
            continue; // Don't scale to idle — it'd flatten everything else
        for (int i = 0; i < kHistorySize; ++i)
            maxVal = std::max(maxVal, m_history[c][i]);
        maxVal = std::max(maxVal, m_peaks[c]);
    }
    // Round up to a nice number for grid lines
    float yScale = maxVal * 1.1f; // 10% headroom

    // Horizontal grid lines (4 divisions)
    for (int i = 1; i < 4; ++i) {
        float gy = pos.y + graphHeight - (graphHeight * (i / 4.0f));
        dl->AddLine({pos.x, gy}, {pos.x + graphWidth, gy}, IM_COL32(50, 50, 50, 120));
        // Label
        int val = static_cast<int>(yScale * (i / 4.0f));
        std::string label = fmt::format("{}", val);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.7f, {pos.x + 2.0f, gy - 10.0f},
                    IM_COL32(120, 120, 120, 180), label.c_str());
    }

    // Draw each channel as a filled line graph (draw idle last, active channels on top)
    // Draw order: idle first (background), then active channels
    int drawOrder[] = {kIdle, kOther, kDisk, kAudio, kSprite, kBitplane, kCpu, kBlitter, kCopper};

    float stepX = graphWidth / static_cast<float>(kHistorySize - 1);

    for (int ch : drawOrder) {
        // Skip idle in the graph to keep it clean — it dominates and hides everything
        if (ch == kIdle)
            continue;

        ImU32 lineColor = kChannelColors[ch];
        // Semi-transparent fill
        ImU32 fillColor = (lineColor & 0x00FFFFFF) | (0x30 << 24);

        // Build points from oldest to newest
        ImVec2 prevPt;
        for (int i = 0; i < kHistorySize; ++i) {
            int idx = (m_historyPos + i) % kHistorySize;
            float val = m_history[ch][idx];
            float nx = pos.x + stepX * i;
            float ny = pos.y + graphHeight - (val / yScale) * graphHeight;
            ny = std::clamp(ny, pos.y, pos.y + graphHeight);
            ImVec2 pt = {nx, ny};

            if (i > 0) {
                // Filled quad from baseline
                float baseY = pos.y + graphHeight;
                dl->AddQuadFilled(prevPt, pt, {pt.x, baseY}, {prevPt.x, baseY}, fillColor);
                // Line on top
                dl->AddLine(prevPt, pt, lineColor, 1.0f);
            }
            prevPt = pt;
        }
    }
}

void DmaStatsView::Draw()
{
    // SetDebugDma(1) arms a per-cycle recording pass inside the core's own retro_run() loop -
    // measured to nearly double per-frame cost (~5ms -> ~8ms on this machine/content). That is
    // pure waste while turbo is actively running: nobody can usefully read a per-cycle DMA
    // trace faster than it is overwritten, and the harvest-suppression flag already exists for
    // exactly this "no meaningful output while turbo runs" condition (AmigaEmulator.h). Treated
    // the same as the window being closed: disarm while suppressed, re-arm the moment turbo
    // ends or the machine pauses.
    const bool suppressed = m_emu && m_emu->IsDebugHarvestSuppressed();

    if (!m_isOpen || suppressed) {
        // Auto-disable debug_dma when closing (or when turbo suppresses the live view)
        if (m_wasOpen && m_emu->HasDmaDebug()) {
            m_emu->SetDebugDma(0);
            m_wasOpen = false;
        }
        if (!m_isOpen)
            return;
    } else {
        // Auto-enable debug_dma while the window is open. Re-check every frame so that
        // a stop/restart of the emulator (which resets debug_dma to 0) is recovered from.
        if (m_emu->HasDmaDebug() && m_emu->GetDebugDma() == 0)
            m_emu->SetDebugDma(1);
        m_wasOpen = true;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }

    ImGui::SetNextWindowSize(ImVec2(400, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_CHART_COLUMN " DMA Stats###DMA Stats##dma_stats", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }

    if (suppressed) {
        DrawTurboSuppressedPlaceholder();
        ImGui::End();
        return;
    }

    if (!m_emu->HasDmaDebug()) {
        ImGui::TextDisabled("DMA debug not available (core too old)");
        ImGui::End();
        return;
    }

    const uint64_t run = m_emu->GetRunCount();
    if (!m_emu->IsGameLoaded()) {
        m_stats = {};
        m_lastStatsRun = UINT64_MAX;
    } else if (run != m_lastStatsRun) {
        UpdateStats();
        m_lastStatsRun = run;
    }
    // Only advance the rolling graph on a freshly emulated frame; while paused the frame counter
    // is static, so the plot freezes instead of scrolling duplicated samples.
    const uint64_t frame = m_emu->GetTotalFrameCount();
    if (frame != m_lastHistoryFrame) {
        PushHistory();
        m_lastHistoryFrame = frame;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 contentPos = ImGui::GetCursorScreenPos();
    float contentWidth = ImGui::GetContentRegionAvail().x;
    if (contentWidth < 50.0f)
        contentWidth = 50.0f;

    static constexpr float kBarHeight = 22.0f;
    static constexpr float kBarGap = 3.0f;
    static constexpr float kStackedHeight = 28.0f;
    static constexpr float kGraphHeight = 150.0f;

    uint32_t maxForBars =
        m_stats.maxSlots; // Full-frame CCK count — bars show true bandwidth fraction

    struct BarDef
    {
        const char* label;
        uint32_t slots;
        ImU32 color;
        int channel;
    };
    BarDef bars[] = {
        {"Copper", m_stats.copperSlots, kDmaColorCopper, kCopper},
        {"Blitter", m_stats.blitterSlots, kDmaColorBlitter, kBlitter},
        {"CPU", m_stats.cpuSlots, kDmaColorCpu, kCpu},
        {"Bitplane", m_stats.bitplaneSlots, kDmaColorBitplane, kBitplane},
        {"Sprite", m_stats.spriteSlots, kDmaColorSprite, kSprite},
        {"Disk", m_stats.diskSlots, kDmaColorDisk, kDisk},
        {"Audio", m_stats.audioSlots, kDmaColorAudio, kAudio},
        {"Other", m_stats.otherSlots, kDmaColorOther, kOther},
        {"Idle", m_stats.idleSlots, kDmaColorIdle, kIdle},
    };

    float y = contentPos.y;
    for (const auto& bar : bars) {
        DrawChannelBar(dl, bar.label, bar.slots, maxForBars, bar.color, m_peaks[bar.channel],
                       {contentPos.x, y}, contentWidth, kBarHeight);
        y += kBarHeight + kBarGap;
    }

    // Separator
    y += 4.0f;
    dl->AddLine({contentPos.x, y}, {contentPos.x + contentWidth, y}, IM_COL32(80, 80, 80, 180));
    y += 6.0f;

    // Stacked bar
    DrawStackedBar(dl, {contentPos.x, y}, contentWidth, kStackedHeight);
    y += kStackedHeight + 4.0f;

    // Summary text
    float barsH = y - contentPos.y;
    ImGui::Dummy(ImVec2(contentWidth, barsH));

    ImGui::TextDisabled("Total: %u / %u slots  (%.1f%% utilised)",
                        m_stats.totalSlots - m_stats.idleSlots, m_stats.maxSlots,
                        m_stats.maxSlots > 0
                            ? 100.0f * static_cast<float>(m_stats.totalSlots - m_stats.idleSlots) /
                                  static_cast<float>(m_stats.maxSlots)
                            : 0.0f);

    // Separator before graph
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // History graph
    ImVec2 graphPos = ImGui::GetCursorScreenPos();
    DrawGraph(dl, graphPos, contentWidth, kGraphHeight);
    ImGui::Dummy(ImVec2(contentWidth, kGraphHeight));

    ImGui::End();
}

} // namespace adm::ui
