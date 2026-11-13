#pragma once

#include <imgui.h>

namespace adm::ui {

// Shared link colour for clickable addresses / register names in the debugger tables.
inline constexpr ImVec4 kHyperlinkColor = {0.45f, 0.72f, 1.0f, 1.0f};

// Give the most recently submitted item hyperlink affordance: an underline and hand cursor while
// hovered. Hit-tests the item's rect directly (instead of ImGui::IsItemHovered) so it still works
// when a span-all-columns Selectable owns the hovered id — the situation every debugger table is
// in.
//
// Returns true while hovered; writes left-click state to *clicked (when non-null) so the caller can
// run its navigation action. Draw the link text (typically TextColored(kHyperlinkColor, ...))
// first.
inline bool HoverableLink(bool* clicked = nullptr)
{
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    bool hovered = ImGui::IsMouseHoveringRect(mn, mx);
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::GetWindowDrawList()->AddLine({mn.x, mx.y - 1.0f}, {mx.x, mx.y - 1.0f},
                                            ImGui::ColorConvertFloat4ToU32(kHyperlinkColor));
    }
    if (clicked)
        *clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    return hovered;
}

} // namespace adm::ui
