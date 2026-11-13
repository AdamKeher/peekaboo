#include "RegisterOpsView.h"
#include "../FontRoles.h"
#include "../TableLayout.h"

#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include "../AddressLink.h"
#include "../HyperlinkText.h"
#include "../IconsFontAwesome6.h"

namespace adm::ui {

namespace {
// Row tints, matching the Copper/CPU colours the Blitter window and Frame Scrubber use.
constexpr ImVec4 kCopperTint = {1.0f, 0.55f, 0.1f, 0.12f};
constexpr ImVec4 kCpuTint = {0.2f, 0.6f, 1.0f, 0.12f};
constexpr ImVec4 kCopperText = {1.0f, 0.6f, 0.1f, 1.0f};
constexpr ImVec4 kCpuText = {0.3f, 0.7f, 1.0f, 1.0f};
const std::string kUnknownName = "-";

// Highest PAL scanline; the line-range filter is clamped to this.
constexpr int kMaxLine = 312;
} // namespace

RegisterOpsView::RegisterOpsView(adm::core::RegWriteProfiler* profiler) : m_profiler(profiler) {}

void RegisterOpsView::LoadRegsDef()
{
    if (m_regsDefLoaded)
        return;
    m_regsDefLoaded = true;
    try {
        std::ifstream f("assets/custom_registers.json");
        if (f.is_open())
            m_regsDef = nlohmann::json::parse(f);
        else
            m_regsDef = nlohmann::json::object();
    } catch (...) {
        m_regsDef = nlohmann::json::object();
    }
}

RegisterOpsView::RegInfo& RegisterOpsView::Info(uint16_t offset)
{
    const int slot = (offset >> 1) & (kRegSlots - 1);
    RegInfo& info = m_regInfo[slot];
    if (info.resolved)
        return info;

    LoadRegsDef();
    info.resolved = true;

    char hex[16];
    snprintf(hex, sizeof(hex), "%03X", static_cast<unsigned>(offset & 0x1FEu));
    if (m_regsDef.contains(hex)) {
        const auto& def = m_regsDef[hex];
        if (def.contains("name") && def["name"].is_string())
            info.name = def["name"].get<std::string>();
        if (def.contains("description") && def["description"].is_string())
            info.description = def["description"].get<std::string>();
        if (def.contains("is_color") && def["is_color"].is_boolean())
            info.isColor = def["is_color"].get<bool>();
    }
    return info;
}

const std::string& RegisterOpsView::RegisterName(uint16_t offset)
{
    const RegInfo& info = Info(offset);
    return info.name.empty() ? kUnknownName : info.name;
}

const std::string& RegisterOpsView::RegisterDescription(uint16_t offset)
{
    return Info(offset).description;
}

bool RegisterOpsView::RegisterIsColor(uint16_t offset) { return Info(offset).isColor; }

bool RegisterOpsView::PassesFilter(const adm::core::RegWriteRecord& rec)
{
    if (m_sourceFilter == 1 && rec.raw.fromCopper)
        return false;
    if (m_sourceFilter == 2 && !rec.raw.fromCopper)
        return false;
    if (m_changedOnly && !rec.changed)
        return false;

    const int line = static_cast<int>(rec.raw.vpos);
    if (line < m_lineFrom || line > m_lineTo)
        return false;

    if (m_nameFilter[0] != '\0') {
        // Case-insensitive substring over the register name, so "col" finds every COLORxx.
        std::string hay = RegisterName(rec.raw.reg);
        std::string needle = m_nameFilter;
        auto upper = [](std::string& s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        };
        upper(hay);
        upper(needle);
        if (hay.find(needle) == std::string::npos)
            return false;
    }
    return true;
}

void RegisterOpsView::RebuildVisible(const std::vector<adm::core::RegWriteRecord>& records)
{
    m_visible.clear();
    m_visible.reserve(records.size());
    for (int i = 0; i < static_cast<int>(records.size()); ++i) {
        if (PassesFilter(records[static_cast<size_t>(i)]))
            m_visible.push_back(i);
    }
    if (m_selectedIdx >= static_cast<int>(m_visible.size()))
        m_selectedIdx = -1;
}

void RegisterOpsView::SetHighlightLRef(uint32_t lRef)
{
    m_highlightLRef = lRef;
    if (!m_profiler)
        return;

    const auto& records = m_profiler->GetCurrentFrame();
    if (m_visible.empty() || records.empty()) {
        m_selectedIdx = -1;
        return;
    }

    // Select the last write at or before lRef: at a given point in the frame the register state
    // is the result of every write up to here, so the most recent one is the row that explains
    // it. Writes are captured in beam order, so a linear scan over the filtered list is monotonic.
    int best = -1;
    for (int i = 0; i < static_cast<int>(m_visible.size()); ++i) {
        if (records[static_cast<size_t>(m_visible[static_cast<size_t>(i)])].lRef <= lRef)
            best = i;
        else
            break;
    }
    m_selectedIdx = (best >= 0) ? best : 0;
}

void RegisterOpsView::Draw()
{
    if (!m_isOpen)
        return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }

    ImGui::SetNextWindowSize(ImVec2(940, 500), ImGuiCond_FirstUseEver);
    // The ###RegisterOps id tag keeps the imgui.ini docking identity stable across renames.
    if (!ImGui::Begin(ICON_FA_MICROCHIP " Register Operations###RegisterOps", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }

    if (!m_profiler) {
        ImGui::TextDisabled("No profiler attached.");
        ImGui::End();
        return;
    }

    if (!m_coreSupported) {
        ImGui::TextColored({1.0f, 0.7f, 0.2f, 1.0f},
                           "The loaded PuAE core predates the register-log patch.");
        ImGui::TextDisabled("Rebuild it with scripts/build_puae.ps1 (Windows) or "
                            "scripts/build_puae.sh (macOS/Linux).");
        ImGui::End();
        return;
    }

    // Captured before the table's child window so the scroll guard reflects this window rather
    // than the inner table child.
    m_windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

    const auto& records = m_profiler->GetCurrentFrame();
    const auto stats = m_profiler->ComputeStats();

    RebuildVisible(records);

    DrawToolbar(stats);
    ImGui::Separator();
    PushFontRole(FontRole::Code);
    DrawTable(records);
    PopFontRole();

    ImGui::End();
}

void RegisterOpsView::DrawToolbar(const adm::core::RegWriteProfiler::FrameStats& stats)
{
    if (stats.totalWrites == 0) {
        ImGui::TextDisabled("No register writes this frame");
    } else {
        ImGui::Text("%d writes | CPU: %d  Copper: %d | %d changed | %d registers touched",
                    stats.totalWrites, stats.cpuWrites, stats.copperWrites, stats.changedWrites,
                    stats.distinctRegisters);
        if (static_cast<int>(m_visible.size()) != stats.totalWrites) {
            ImGui::SameLine();
            ImGui::TextDisabled("| showing %d", static_cast<int>(m_visible.size()));
        }
    }

    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##regfilter", "filter register...", m_nameFilter,
                             sizeof(m_nameFilter));
    ImGui::SameLine();

    ImGui::SetNextItemWidth(90.0f);
    ImGui::Combo("##src", &m_sourceFilter, "All\0CPU\0Copper\0");
    ImGui::SameLine();

    ImGui::Checkbox("Changed only", &m_changedOnly);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hide writes that re-wrote the value the register already held.");
    ImGui::SameLine();

    ImGui::SetNextItemWidth(78.0f);
    ImGui::DragInt("##linefrom", &m_lineFrom, 1.0f, 0, kMaxLine, "line %d");
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(60.0f);
    ImGui::DragInt("##lineto", &m_lineTo, 1.0f, 0, kMaxLine, "- %d");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Restrict the log to a band of scanlines.");
    m_lineFrom = std::clamp(m_lineFrom, 0, kMaxLine);
    m_lineTo = std::clamp(m_lineTo, m_lineFrom, kMaxLine);
}

void RegisterOpsView::DrawTable(const std::vector<adm::core::RegWriteRecord>& records)
{
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                           ImGuiTableFlags_BordersV | kAdjustableColumns |
                                           ImGuiTableFlags_SizingFixedFit;

    const float tableH = ImGui::GetContentRegionAvail().y;
    if (!ImGui::BeginTable("##regwrites", 10, tableFlags, ImVec2(0, tableH)))
        return;

    // Freeze the header row and the L-Ref column so both stay visible while scrolling.
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("L-Ref", ImGuiTableColumnFlags_WidthFixed, 78.0f);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 45.0f);
    ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("Clk", ImGuiTableColumnFlags_WidthFixed, 38.0f);
    ImGui::TableSetupColumn("Src", ImGuiTableColumnFlags_WidthFixed, 55.0f);
    ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 78.0f);
    ImGui::TableSetupColumn("Was", ImGuiTableColumnFlags_WidthFixed, 62.0f);
    ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    bool scrollToSelected = false;
    if (m_selectedIdx != m_lastSelectedIdx) {
        scrollToSelected = true;
        m_lastSelectedIdx = m_selectedIdx;
    }

    // External hover -> highlight the matching row. Scroll it into view only when the highlight
    // changes AND the mouse isn't over this window, else our own row-hover (which loops back
    // through the same channel) would fight the scroll.
    int hoverHighlightIdx = -1;
    if (m_hoverHighlightLRef != UINT32_MAX) {
        for (int i = 0; i < static_cast<int>(m_visible.size()); ++i) {
            if (records[static_cast<size_t>(m_visible[static_cast<size_t>(i)])].lRef ==
                m_hoverHighlightLRef) {
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

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_visible.size()));
    if (scrollToSelected && m_selectedIdx >= 0 &&
        m_selectedIdx < static_cast<int>(m_visible.size()))
        clipper.IncludeItemsByIndex(m_selectedIdx, m_selectedIdx + 1);
    if (scrollToHover && hoverHighlightIdx >= 0)
        clipper.IncludeItemsByIndex(hoverHighlightIdx, hoverHighlightIdx + 1);

    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const int recIdx = m_visible[static_cast<size_t>(i)];
            const auto& rec = records[static_cast<size_t>(recIdx)];
            const auto& raw = rec.raw;

            ImGui::TableNextRow();

            ImGui::TableSetBgColor(
                ImGuiTableBgTarget_RowBg0,
                ImGui::ColorConvertFloat4ToU32(raw.fromCopper ? kCopperTint : kCpuTint));
            if (i == hoverHighlightIdx)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(255, 255, 255, 56));

            const bool isSelected = (i == m_selectedIdx);
            // Writes that haven't landed yet at the current scrubber position.
            const bool isFuture = (m_selectedIdx >= 0 && i > m_selectedIdx);
            if (isFuture)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(fmt::format("L{:07X}", rec.lRef).c_str(), isSelected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                m_selectedIdx = i;
                if (m_onJumpToLRef)
                    m_onJumpToLRef(rec.lRef, raw.fromCopper, raw.pc);
            }
            if (ImGui::IsItemHovered())
                hoveredLRef = rec.lRef;

            if (scrollToSelected && isSelected)
                ImGui::SetScrollHereY(0.5f);
            else if (scrollToHover && i == hoverHighlightIdx)
                ImGui::SetScrollHereY(0.5f);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", recIdx + 1);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", raw.vpos);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", raw.hpos);

            ImGui::TableSetColumnIndex(4);
            if (isFuture)
                ImGui::TextUnformatted(raw.fromCopper ? "Copper" : "CPU");
            else
                ImGui::TextColored(raw.fromCopper ? kCopperText : kCpuText, "%s",
                                   raw.fromCopper ? "Copper" : "CPU");

            // Register name -> Custom Registers, with the decoder seeded from this row's value
            // (what was actually written here) rather than the live register.
            ImGui::TableSetColumnIndex(5);
            const std::string& name = RegisterName(raw.reg);
            if (m_onJumpToCustomReg && !isFuture) {
                ImGui::TextColored(kHyperlinkColor, "%s", name.c_str());
                bool clicked = false;
                HoverableLink(&clicked);
                if (clicked)
                    m_onJumpToCustomReg(raw.reg, raw.value);
            } else {
                ImGui::TextUnformatted(name.c_str());
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("$DFF%03X", static_cast<unsigned>(raw.reg));

            ImGui::TableSetColumnIndex(6);
            if (RegisterIsColor(raw.reg)) {
                // Colour registers get a swatch: a palette change reads far faster as a colour
                // than as four hex digits.
                const ImVec4 col{static_cast<float>((raw.value >> 8) & 0xF) / 15.0f,
                                 static_cast<float>((raw.value >> 4) & 0xF) / 15.0f,
                                 static_cast<float>(raw.value & 0xF) / 15.0f, 1.0f};
                ImGui::ColorButton("##sw", col,
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                                   ImVec2(12, 12));
                ImGui::SameLine(0.0f, 4.0f);
            }
            ImGui::Text("$%04X", raw.value);

            // What this write changed the register from — the column that separates the write
            // that did something from the hundreds that re-wrote the same value.
            ImGui::TableSetColumnIndex(7);
            if (rec.firstWrite)
                ImGui::TextDisabled("first");
            else if (rec.changed)
                ImGui::Text("$%04X", rec.prevValue);
            else
                ImGui::TextDisabled("same");

            ImGui::TableSetColumnIndex(8);
            if (m_addrCtx && !isFuture) {
                // Right-click gives the shared address menu (breakpoint, memory, disasm), so
                // ENGINE9000's "click ADDR to set a breakpoint" comes along for free.
                DrawAddressLink("regpc", raw.pc, "$%06X", *m_addrCtx, AddressLinkHost::Window,
                                AddressDefault::Disasm);
            } else {
                ImGui::Text("$%06X", raw.pc);
            }

            ImGui::TableSetColumnIndex(9);
            const std::string& desc = RegisterDescription(raw.reg);
            if (!desc.empty())
                ImGui::TextUnformatted(desc.c_str());

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
