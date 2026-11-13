#pragma once

#include <imgui.h>

#include "../IconsFontAwesome6.h"

namespace adm::ui {

// Centered placeholder shown by debugger windows that suppress live content while turbo is
// actively running the machine (AmigaEmulator::IsDebugHarvestSuppressed()) - the underlying
// data changes every emulated frame faster than anyone could read it, so drawing it live is
// pure waste. Call inside the window's Begin()/End() pair in place of its normal content.
inline void DrawTurboSuppressedPlaceholder()
{
    constexpr float kIconScale = 3.0f;
    const char* kLabel = "Turbo Enabled";

    ImGui::SetWindowFontScale(kIconScale);
    const ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_FORWARD_FAST);
    ImGui::SetWindowFontScale(1.0f);
    const ImVec2 labelSize = ImGui::CalcTextSize(kLabel);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float totalH = iconSize.y + spacing + labelSize.y;

    const ImVec2 origin = ImGui::GetCursorPos();
    ImGui::SetCursorPos(
        ImVec2(origin.x + (avail.x - iconSize.x) * 0.5f, origin.y + (avail.y - totalH) * 0.5f));
    ImGui::SetWindowFontScale(kIconScale);
    ImGui::TextDisabled("%s", ICON_FA_FORWARD_FAST);
    ImGui::SetWindowFontScale(1.0f);

    ImGui::SetCursorPosX(origin.x + (avail.x - labelSize.x) * 0.5f);
    ImGui::TextDisabled("%s", kLabel);
}

} // namespace adm::ui
