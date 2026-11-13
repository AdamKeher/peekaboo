#include "McpActivityView.h"
#include "../FontRoles.h"
#include "../TableLayout.h"

#include <imgui.h>

#include "../../core/McpToolHandler.h"
#include "../IconsFontAwesome6.h"

namespace adm::ui {

void McpActivityView::Draw()
{
    if (!m_isOpen)
        return;

    // Suppress the automatic focus-on-appear so that when this window is restored into a dock node
    // (e.g. on workspace load) it does not steal the tab selection from the layout's saved active
    // tab. The flag is cleared inside Begin() below, so it also works when the window starts
    // closed.
    ImGuiWindowFlags windowFlags = 0;
    if (m_noFocusOnAppearing) {
        windowFlags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }

    ImGui::SetNextWindowSize(ImVec2(620, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_MICROCHIP " MCP Activity###McpActivity", &m_isOpen, windowFlags)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button(ICON_FA_TRASH " Clear") && m_handler)
        m_handler->ClearActivity();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu calls", m_handler ? m_handler->Activity().size() : 0u);

    ImGui::Separator();

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  kAdjustableColumns | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    PushFontRole(FontRole::Console);
    if (m_handler && ImGui::BeginTable("##mcp_activity", 5, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Args", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableHeadersRow();

        for (const auto& e : m_handler->Activity()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.time.c_str());
            ImGui::TableNextColumn();
            if (e.ok)
                ImGui::TextUnformatted(e.tool.c_str());
            else
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", e.tool.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", e.args.c_str());
            ImGui::TableNextColumn();
            if (e.ok)
                ImGui::TextWrapped("%s", e.result.c_str());
            else
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", e.result.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", e.durationMs);
        }

        if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndTable();
    }
    PopFontRole();

    ImGui::End();
}

} // namespace adm::ui
