#pragma once

#include <string>
#include <vector>

#include <imgui.h>

namespace adm::ui {

// A combo box backed by a vector<string>. Returns true when the selection changes.
// `idx` is clamped into range. Empty lists render nothing and return false.
inline bool ComboFromVector(const char* label, int& idx, const std::vector<std::string>& items,
                            ImGuiComboFlags flags = 0)
{
    if (items.empty())
        return false;
    if (idx < 0 || idx >= static_cast<int>(items.size()))
        idx = 0;

    bool changed = false;
    if (ImGui::BeginCombo(label, items[idx].c_str(), flags)) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            bool sel = (idx == i);
            if (ImGui::Selectable(items[i].c_str(), sel)) {
                idx = i;
                changed = true;
            }
            if (sel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace adm::ui
