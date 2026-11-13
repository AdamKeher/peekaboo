#include "UnsavedChangesDialog.h"

#include <imgui.h>

namespace adm::ui {

void UnsavedChangesDialog::Enqueue(const std::string& filename, std::function<bool()> onSave,
                                   std::function<void()> onProceed, std::function<void()> onCancel)
{
    bool wasEmpty = m_queue.empty();
    m_queue.push_back({filename, std::move(onSave), std::move(onProceed), std::move(onCancel)});
    if (wasEmpty)
        m_trigger = true;
}

void UnsavedChangesDialog::Draw()
{
    if (m_trigger && !m_queue.empty()) {
        ImGui::OpenPopup("Unsaved Changes");
        m_trigger = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_queue.empty()) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        auto& item = m_queue.front();

        ImGui::Dummy(ImVec2(360.0f, 0.0f));
        if (item.filename.empty()) {
            ImGui::TextUnformatted("You have unsaved changes.");
        } else {
            ImGui::Text("'%s' has unsaved changes.", item.filename.c_str());
        }
        ImGui::Text("Do you want to save before continuing?");
        ImGui::Separator();

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (item.onCancel)
                item.onCancel();
            m_queue.clear();
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::IsWindowAppearing())
            ImGui::SetNavCursorVisible(true);

        bool triggerDontSave =
            ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

        auto advance = [&]() {
            m_queue.pop_front();
            if (!m_queue.empty())
                m_trigger = true;
            ImGui::CloseCurrentPopup();
        };

        if (ImGui::Button("Don't Save") || triggerDontSave) {
            if (item.onProceed)
                item.onProceed();
            advance();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            bool saved = item.onSave ? item.onSave() : false;
            if (saved) {
                if (item.onProceed)
                    item.onProceed();
                advance();
            } else {
                // Save failed or was cancelled — treat as "did not proceed".
                if (item.onCancel)
                    item.onCancel();
                m_queue.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            if (item.onCancel)
                item.onCancel();
            m_queue.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace adm::ui
