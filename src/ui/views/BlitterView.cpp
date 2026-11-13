#include "BlitterView.h"
#include "../FontRoles.h"
#include "../TableLayout.h"

#include <fmt/format.h>
#include <algorithm>
#include <cmath>

#include "../HyperlinkText.h"
#include "../IconsFontAwesome6.h"
#include "BlitOpPresets.h"

namespace adm::ui {

// Row height used while destination thumbnails are shown. Fixed for every row (not just the
// ones with data) because ImGuiListClipper assumes a uniform row height.
static constexpr float kDataRowHeight = 44.0f;

BlitterView::BlitterView(adm::core::BlitProfiler* profiler) : m_profiler(profiler) {}

std::string BlitterView::FormatHex16(uint16_t v) { return fmt::format("${:04X}", v); }

void BlitterView::SetHighlightLRef(uint32_t lRef)
{
    m_highlightLRef = lRef;
    if (!m_profiler)
        return;

    const auto& records = m_profiler->GetCurrentFrame();
    if (records.empty()) {
        m_selectedIdx = -1;
        return;
    }

    // Find the blit whose range contains lRef, or the nearest one before it
    int best = 0;
    uint32_t bestStart = records[0].lRef;
    for (int i = 0; i < static_cast<int>(records.size()); ++i) {
        uint32_t start = records[i].lRef;
        uint32_t end = start + records[i].dmaCycles;
        if (lRef >= start && lRef <= end) {
            m_selectedIdx = i;
            return;
        }
        if (start <= lRef && start >= bestStart) {
            bestStart = start;
            best = i;
        }
    }
    m_selectedIdx = best;
}

void BlitterView::Draw()
{
    if (!m_isOpen)
        return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }

    ImGui::SetNextWindowSize(ImVec2(900, 500), ImGuiCond_FirstUseEver);
    // The ###Blitter id tag keeps the imgui.ini docking identity stable across the rename.
    if (!ImGui::Begin(ICON_FA_FILL_DRIP " Blitter Operations###Blitter", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }

    if (!m_profiler) {
        ImGui::TextDisabled("No profiler attached.");
        ImGui::End();
        return;
    }

    // Captured before the table's child window so the scroll-guard reflects the Blitter window,
    // not the inner table child.
    m_windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

    // Ages out thumbnails for rows that have scrolled away; must run before any of them draw.
    m_destPreview.BeginFrame();

    const auto& records = m_profiler->GetCurrentFrame();
    const auto stats = m_profiler->ComputeStats();

    DrawStatsBar(stats);
    ImGui::Separator();
    PushFontRole(FontRole::Code);
    DrawTable(records);
    PopFontRole();

    ImGui::End();
}

void BlitterView::DrawStatsBar(const adm::core::BlitProfiler::FrameStats& stats)
{
    if (stats.totalBlits == 0)
        ImGui::TextDisabled("No blits this frame");
    else
        ImGui::Text("%d blits | CPU: %d  Copper: %d | Bus: %.1f%% (%u / 71,364 CCK)",
                    stats.totalBlits, stats.cpuBlits, stats.copperBlits, stats.busPercent,
                    stats.totalDmaCycles);

    // The capture toggle itself lives in Debugger -> Display -> Blitter, alongside the overlay
    // that shows where those writes landed. Only its state is mirrored here, to decide whether
    // the Data column is worth showing -- and to say where the switch went when it is off.
    if (!m_captureDestData) {
        ImGui::SameLine();
        ImGui::TextUnformatted("|");
        ImGui::SameLine();
        ImGui::TextDisabled("Data off");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!m_captureSupported)
                ImGui::SetTooltip("The loaded PuAE core predates destination capture.\n"
                                  "Rebuild it with scripts/build_puae to enable this.");
            else
                ImGui::SetTooltip("Capture the words each blit writes through its D channel to\n"
                                  "fill the Data column. Turn it on in\n"
                                  "Debugger -> Display -> Blitter.");
        }
    }
}

void BlitterView::DrawTable(const std::vector<adm::core::BlitRecord>& records)
{
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                           ImGuiTableFlags_BordersV | kAdjustableColumns |
                                           ImGuiTableFlags_SizingFixedFit;

    float tableH = ImGui::GetContentRegionAvail().y;
    if (!ImGui::BeginTable("##blits", 22, tableFlags, ImVec2(0, tableH)))
        return;

    // Freeze the header row and the L-Ref column so both stay visible while scrolling.
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("L-Ref", ImGuiTableColumnFlags_WidthFixed, 78.0f);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f);
    ImGui::TableSetupColumn("VLine", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("Clock", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("Trigger", ImGuiTableColumnFlags_WidthFixed, 55.0f);
    ImGui::TableSetupColumn("Op", ImGuiTableColumnFlags_WidthFixed, 45.0f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 75.0f);
    // What the blit actually wrote. Hidden by default until capture is armed, since the
    // column is empty (and the rows are needlessly tall) without it.
    ImGui::TableSetupColumn("Data",
                            ImGuiTableColumnFlags_WidthFixed |
                                (m_captureDestData ? 0 : ImGuiTableColumnFlags_DefaultHide),
                            150.0f);
    ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Amod", ImGuiTableColumnFlags_WidthFixed, 45.0f);
    ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Bmod", ImGuiTableColumnFlags_WidthFixed, 45.0f);
    ImGui::TableSetupColumn("C", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Cmod", ImGuiTableColumnFlags_WidthFixed, 45.0f);
    ImGui::TableSetupColumn("D", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Dmod", ImGuiTableColumnFlags_WidthFixed, 45.0f);
    ImGui::TableSetupColumn("Mint", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("BLTCON0", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("BLTCON1", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Rows", ImGuiTableColumnFlags_WidthFixed, 45.0f);
    ImGui::TableSetupColumn("Words", ImGuiTableColumnFlags_WidthFixed, 45.0f);
    ImGui::TableSetupColumn("Cycles", ImGuiTableColumnFlags_WidthFixed, 55.0f);
    ImGui::TableHeadersRow();

    static constexpr ImVec4 kCopperTint = {1.0f, 0.55f, 0.1f, 0.12f};
    // Row tint by blit mode, matching the Frame Scrubber: green = line-draw mode, blue = standard
    // fill/copy/clear. Copper-triggered standard blits keep the copper (orange) tint; the trigger
    // is also shown in the Trigger column.
    static constexpr ImVec4 kLineTint = {0.5f, 1.0f, 0.3f, 0.12f};
    static constexpr ImVec4 kFillTint = {0.2f, 0.6f, 1.0f, 0.12f};

    bool scrollToSelected = false;
    if (m_selectedIdx != m_lastSelectedIdx) {
        scrollToSelected = true;
        m_lastSelectedIdx = m_selectedIdx;
    }

    // Frame Scrubber hover → highlight the matching row. Resolve its index, and scroll it into
    // view only when the highlight changes AND the mouse isn't over this window (else the window's
    // own row-hover, which loops back through the same channel, would fight the scroll).
    int hoverHighlightIdx = -1;
    if (m_hoverHighlightLRef != UINT32_MAX) {
        for (int i = 0; i < static_cast<int>(records.size()); ++i) {
            if (records[i].lRef == m_hoverHighlightLRef) {
                hoverHighlightIdx = i;
                break;
            }
        }
    }
    bool scrollToHover = false;
    if (m_hoverHighlightLRef != m_lastHoverHighlightLRef) {
        scrollToHover = (hoverHighlightIdx >= 0) && !m_windowHovered;
        m_lastHoverHighlightLRef = m_hoverHighlightLRef;
    }

    uint32_t hoveredLRef = UINT32_MAX;

    // Renders a 24-bit channel address. In-use channels are blue hyperlinks that jump the
    // Memory window to the address; unused channels show the raw value dimmed (no link).
    auto AddrLink = [&](uint32_t addr, bool used) {
        if (!used || !m_onJumpToMemory) {
            ImGui::TextDisabled("$%06X", addr);
            return;
        }
        ImGui::TextColored(kHyperlinkColor, "$%06X", addr);
        bool clicked = false;
        HoverableLink(&clicked);
        if (clicked)
            m_onJumpToMemory(addr, 16);
    };

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(records.size()));
    if (scrollToSelected && m_selectedIdx >= 0 && m_selectedIdx < static_cast<int>(records.size()))
        clipper.IncludeItemsByIndex(m_selectedIdx, m_selectedIdx + 1);
    if (scrollToHover && hoverHighlightIdx >= 0)
        clipper.IncludeItemsByIndex(hoverHighlightIdx, hoverHighlightIdx + 1);

    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& rec = records[i];

            // ImGuiListClipper needs every row the same height, so the thumbnails get a fixed
            // row height for the whole table rather than each row sizing to its own image.
            ImGui::TableNextRow(ImGuiTableRowFlags_None, m_captureDestData ? kDataRowHeight : 0.0f);

            // Row tint by mode (green = line, blue = standard), copper-triggered standard
            // blits keep the orange copper tint. Mirrors the Frame Scrubber's blit colors.
            const ImVec4 rowTint =
                rec.lineMode ? kLineTint : (rec.raw.fromCopper ? kCopperTint : kFillTint);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   ImGui::ColorConvertFloat4ToU32(rowTint));

            // Frame Scrubber hover highlight — a brighter overlay on top of the mode tint, echoing
            // the white outline the scrubber draws on the hovered bar.
            if (i == hoverHighlightIdx)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(255, 255, 255, 56));

            bool isSelected = (i == m_selectedIdx);
            bool isFuture = (m_selectedIdx >= 0 && i > m_selectedIdx);

            // Dim all text for blits that haven't happened yet relative to the scrubber position
            if (isFuture)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(fmt::format("L{:07X}", rec.startCycle).c_str(), isSelected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                m_selectedIdx = i;
                if (m_onJumpToLRef)
                    m_onJumpToLRef(rec.startCycle);
            }
            if (ImGui::IsItemHovered())
                hoveredLRef = rec.lRef;

            if (scrollToSelected && isSelected)
                ImGui::SetScrollHereY(0.5f);
            else if (scrollToHover && i == hoverHighlightIdx)
                ImGui::SetScrollHereY(0.5f);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", i + 1);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", rec.startLine);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", rec.startClock);

            ImGui::TableSetColumnIndex(4);
            if (isFuture) {
                // Keep under the dimmed PushStyleColor — no separate color for trigger
                ImGui::TextUnformatted(rec.raw.fromCopper ? "Copper" : "CPU");
            } else if (rec.raw.fromCopper) {
                ImGui::TextColored({1.0f, 0.6f, 0.1f, 1.0f}, "Copper");
            } else {
                ImGui::TextColored({0.3f, 0.7f, 1.0f, 1.0f}, "CPU");
            }

            const auto& raw = rec.raw;

            // Op: friendly label for the most common blit configurations. Line-draw blits (BLTCON1
            // bit 0) never match the area-mode presets, so label them directly.
            ImGui::TableSetColumnIndex(5);
            const char* opName = "\xE2\x80\x94"; // em dash
            if (rec.lineMode) {
                opName = "line";
            } else {
                for (const auto& c : kBlitOpPresets) {
                    if (raw.bltcon0 == c.bltcon0 && raw.bltcon1 == c.bltcon1) {
                        opName = c.name;
                        break;
                    }
                }
            }
            ImGui::TextUnformatted(opName);

            // Size: blit resolution in pixels (words → 16px each).
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%ux%upx", static_cast<unsigned>(rec.wordsPerRow) * 16u,
                        static_cast<unsigned>(rec.rows));

            // Data: the bits this blit actually wrote, rebuilt from the captured D writes.
            // Hovering blows it up to full size in a tooltip.
            ImGui::TableSetColumnIndex(7);
            if (BlitDestPreview::HasData(rec)) {
                if (m_destPreview.Thumbnail(rec, 146.0f, kDataRowHeight - 4.0f) &&
                    ImGui::IsItemHovered()) {
                    hoveredLRef = rec.lRef;
                    ImGui::BeginTooltip();
                    m_destPreview.Detail(rec, 640.0f, 640.0f);
                    ImGui::EndTooltip();
                }
            } else if (m_captureDestData) {
                // Capture is armed but this blit produced nothing: either it has no D channel,
                // or the core ring recycled its window before the frame ended.
                ImGui::TextDisabled(rec.useD ? "\xE2\x80\x94" : "no D");
            }

            // Channels: address (hyperlinked when in use) + signed modulo. Future blits keep
            // the row's dimmed styling, so their addresses render as plain dimmed text.
            ImGui::TableSetColumnIndex(8);
            AddrLink(raw.bltapt, rec.useA && !isFuture);
            ImGui::TableSetColumnIndex(9);
            ImGui::Text("%d", static_cast<int16_t>(raw.bltamod));

            ImGui::TableSetColumnIndex(10);
            AddrLink(raw.bltbpt, rec.useB && !isFuture);
            ImGui::TableSetColumnIndex(11);
            ImGui::Text("%d", static_cast<int16_t>(raw.bltbmod));

            ImGui::TableSetColumnIndex(12);
            AddrLink(raw.bltcpt, rec.useC && !isFuture);
            ImGui::TableSetColumnIndex(13);
            ImGui::Text("%d", static_cast<int16_t>(raw.bltcmod));

            ImGui::TableSetColumnIndex(14);
            AddrLink(raw.bltdpt, rec.useD && !isFuture);
            ImGui::TableSetColumnIndex(15);
            ImGui::Text("%d", static_cast<int16_t>(raw.bltdmod));

            ImGui::TableSetColumnIndex(16);
            if (m_onOpenMinterm && !isFuture) {
                ImGui::TextColored(kHyperlinkColor, "$%02X", rec.minterm);
                bool clicked = false;
                HoverableLink(&clicked);
                if (clicked)
                    m_onOpenMinterm(raw.bltcon0, raw.bltcon1);
            } else {
                ImGui::Text("$%02X", rec.minterm);
            }

            ImGui::TableSetColumnIndex(17);
            ImGui::Text("%s", FormatHex16(raw.bltcon0).c_str());

            ImGui::TableSetColumnIndex(18);
            ImGui::Text("%s", FormatHex16(raw.bltcon1).c_str());

            ImGui::TableSetColumnIndex(19);
            ImGui::Text("%u", rec.rows);

            ImGui::TableSetColumnIndex(20);
            ImGui::Text("%u", rec.wordsPerRow);

            ImGui::TableSetColumnIndex(21);
            ImGui::Text("%u", rec.dmaCycles);

            if (isFuture)
                ImGui::PopStyleColor();
        }
    }
    clipper.End();

    if (hoveredLRef != m_lastHoveredLRef) {
        m_lastHoveredLRef = hoveredLRef;
        if (m_onHoverLRef)
            m_onHoverLRef(hoveredLRef);
    }

    ImGui::EndTable();
}

} // namespace adm::ui
