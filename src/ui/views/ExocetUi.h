#pragma once

// What the Exocet windows share: a toolbar laid out to fit the window, the tooltip a disabled
// control gives for why, the groups' colours, the song's clock and how a grid cell reads.

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "../../core/ExocetActivity.h"
#include "../../core/ExocetEval.h"
#include "../../core/ExocetKit.h"
#include "../../core/ExocetPack.h"
#include "../../core/ExocetSession.h"
#include "../IconsFontAwesome6.h"

namespace adm::ui::exo {

inline void TooltipWhenDisabled(const std::string& why)
{
    if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", why.c_str());
}

// Push, in the Exocet window and the grid both: the keys are edited in the grid, and pushing
// from there saves switching windows to see an edit run
inline void PushButton(core::ExocetSession& session)
{
    const std::string why = session.PushBlockedReason();
    ImGui::BeginDisabled(!why.empty());
    if (ImGui::Button(ICON_FA_UPLOAD " Push"))
        session.Push();
    ImGui::EndDisabled();
    TooltipWhenDisabled(why.empty() ? "Pack the keys with the kit and write them into the "
                                      "running demo; clears the frame history"
                                    : why);
}

// Lays the toolbar out in groups, left to right, starting a new line before a group that would
// not fit: the window is often narrower than the whole toolbar (a Lores screen, a docked panel).
class ToolbarFlow
{
public:
    // `ruleH`: the height of the rule drawn between two groups on one line
    explicit ToolbarFlow(float ruleH) : m_ruleH(ruleH) {}

    // Before something `width` wide: beside the last group, after a rule, if it fits there, or
    // else at the start of a new line
    void Next(float width)
    {
        if (m_first) {
            m_first = false;
            return;
        }
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < 1.0f + ImGui::GetStyle().ItemSpacing.x + width) {
            ImGui::NewLine();
            return;
        }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 2.0f),
                                            ImVec2(p.x, p.y + m_ruleH - 2.0f),
                                            ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(1.0f, m_ruleH));
        ImGui::SameLine();
    }

    // Widgets whose width only drawing them tells: placed by how wide they were the frame before
    void Begin(const char* id)
    {
        m_id = ImGui::GetID(id);
        Next(ImGui::GetStateStorage()->GetFloat(m_id, 0.0f));
        ImGui::BeginGroup();
    }
    void End()
    {
        ImGui::EndGroup();
        ImGui::GetStateStorage()->SetFloat(m_id, ImGui::GetItemRectSize().x);
    }

    // A message as a group of its own, wrapped at the window's edge when it is wider than a line
    void Message(const std::string& text)
    {
        if (text.empty())
            return;
        Next(ImGui::CalcTextSize(text.c_str()).x);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", text.c_str());
        ImGui::PopTextWrapPos();
    }

private:
    float m_ruleH;
    bool m_first = true;
    ImGuiID m_id = 0;
};

// A song frame as mm:ss.cc: the song clock is the vertical blank, `fps` frames a second - 50 on
// PAL, 60 on NTSC (the demo's block says which; the kit refuses a CIA tempo)
inline std::string SongTime(uint32_t frame, int fps)
{
    const uint32_t f = static_cast<uint32_t>(fps > 0 ? fps : 50);
    return fmt::format("{:02}:{:02}.{:02}", frame / (60 * f), frame / f % 60, frame % f * 100 / f);
}

// A bookmark's, in the grid's gutter and on the timeline: dark text reads its slot on it
constexpr ImU32 kBookmarkColour = IM_COL32(96, 160, 245, 255);

// The colour of the manifest's group `index`: mid-light hues that dark text reads on, so a group
// keeps its colour while the manifest keeps its order of groups, wherever it is dragged to
inline ImU32 GroupColour(int index, int alpha = 255)
{
    static const ImU32 kPalette[] = {IM_COL32(222, 170, 92, 255),  IM_COL32(106, 182, 222, 255),
                                     IM_COL32(190, 142, 222, 255), IM_COL32(122, 200, 132, 255),
                                     IM_COL32(228, 132, 146, 255), IM_COL32(206, 204, 108, 255),
                                     IM_COL32(112, 204, 192, 255), IM_COL32(160, 162, 232, 255)};
    constexpr int kCount = sizeof kPalette / sizeof kPalette[0];
    const ImU32 c = kPalette[std::max(0, index) % kCount];
    return (c & ~IM_COL32_A_MASK) |
           (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << IM_COL32_A_SHIFT);
}

// The pack's group indices in the demo's order: the director first - the cuts are what the rest
// is timed around - then each group by the first row it was recorded enabled on, the ones not
// seen running yet last, ties in the manifest's order. The grid's and the timeline's order.
inline std::vector<int> GroupsByFirstRun(const core::ExocetPack& pack,
                                         const core::ExocetActivity& activity)
{
    const auto& groups = pack.Groups();
    const int n = static_cast<int>(groups.size());
    const std::string director = pack.DirectorGroup();
    std::vector<int> first(n), order(n);
    for (int g = 0; g < n; ++g) {
        const auto spans = activity.ActiveSpans(groups[g].name);
        first[g] = groups[g].name == director ? -1
                   : spans.empty()            ? std::numeric_limits<int>::max()
                                              : spans.front().first;
        order[g] = g;
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return first[a] < first[b]; });
    return order;
}

// A key's interpolation as the grid marks it: step, linear, smooth, ramp
inline const char* Glyph(uint8_t interp)
{
    switch (interp) {
        case 1:
            return "/";
        case 2:
            return "~";
        case 3:
            return "^";
        default:
            return " ";
    }
}

inline core::ExocetType TypeOf(const core::ExocetTrackInfo& t)
{
    return static_cast<core::ExocetType>(core::ExocetTypeFromName(t.type));
}

// A track's value as its type reads: a fix to three places, a colour as #RRGGBBAA, an event or a
// table slot by its name
inline std::string FormatValue(const core::ExocetTrackInfo& t, int32_t v)
{
    const auto type = TypeOf(t);
    if (type == core::ExocetType::Fix)
        return fmt::format("{:.3f}", v / 65536.0);
    if (type == core::ExocetType::Color)
        return core::ExocetColorText(static_cast<uint32_t>(v));
    if (type == core::ExocetType::Event || type == core::ExocetType::Index ||
        type == core::ExocetType::Ptr) {
        for (const auto& e : t.entries)
            if (e.value == v)
                return e.name;
    }
    return std::to_string(v);
}

inline std::string Cell(const std::array<uint16_t, 4>& c)
{
    // "C-3 05 A0F", the way a tracker shows a cell
    const std::string note = core::ExocetNoteName(c[1]);
    const std::string smp = c[0] ? fmt::format("{:02X}", c[0]) : "..";
    const std::string fx = (c[2] || c[3]) ? fmt::format("{:X}{:02X}", c[2], c[3]) : "...";
    return note + " " + smp + " " + fx;
}

} // namespace adm::ui::exo
