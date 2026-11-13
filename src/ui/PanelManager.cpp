#include "PanelManager.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <random>

namespace adm::ui {

uint32_t PanelManager::GenerateId() const
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, UINT32_MAX);
    uint32_t id;
    do {
        id = dist(gen);
    } while (std::any_of(m_panels.begin(), m_panels.end(),
                         [id](const auto& p) { return p->GetId() == id; }));
    return id;
}

uint32_t PanelManager::Create(const std::string& name)
{
    const uint32_t id = GenerateId();
    auto panel = std::make_unique<PanelView>(name, id);
    panel->SetOnRenameRequested([this](uint32_t panelId) { RequestRename(panelId); });
    panel->SetOnDeleteRequested([this](uint32_t panelId) { Delete(panelId); });
    m_panels.push_back(std::move(panel));
    return id;
}

bool PanelManager::Restore(const std::string& name, uint32_t id)
{
    if (Find(id) != nullptr)
        return false;

    auto panel = std::make_unique<PanelView>(name, id);
    panel->SetOnRenameRequested([this](uint32_t panelId) { RequestRename(panelId); });
    panel->SetOnDeleteRequested([this](uint32_t panelId) { Delete(panelId); });
    // Don't let a recreated panel grab focus on first appearance — otherwise whichever panel
    // happens to be drawn last steals focus from the one that was focused at exit.
    panel->SuppressFocusOnAppear();
    m_panels.push_back(std::move(panel));
    return true;
}

void PanelManager::Delete(uint32_t id)
{
    m_panels.erase(std::remove_if(m_panels.begin(), m_panels.end(),
                                  [id](const auto& p) { return p->GetId() == id; }),
                   m_panels.end());
}

void PanelManager::Rename(uint32_t id, const std::string& newName)
{
    if (PanelView* p = Find(id))
        p->SetName(newName);
}

PanelView* PanelManager::Find(uint32_t id)
{
    for (auto& p : m_panels) {
        if (p->GetId() == id)
            return p.get();
    }
    return nullptr;
}

void PanelManager::RequestNew()
{
    m_newNameBuf[0] = '\0';
    m_showNewDialog = true;
}

void PanelManager::RequestRename(uint32_t id)
{
    m_renameId = id;
    m_renameNameBuf[0] = '\0';
    if (PanelView* p = Find(id))
        std::snprintf(m_renameNameBuf, sizeof(m_renameNameBuf), "%s", p->GetName().c_str());
    m_showRenameDialog = true;
}

void PanelManager::DrawModals()
{
    if (m_showNewDialog) {
        ImGui::OpenPopup("New Panel");
        m_showNewDialog = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("New Panel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Dummy(ImVec2(300.0f, 0.0f));
        ImGui::Text("Panel name:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        bool enterPressed = ImGui::InputText("##newpanelname", m_newNameBuf, sizeof(m_newNameBuf),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        bool nameValid = (m_newNameBuf[0] != '\0');
        if ((ImGui::Button("Create", ImVec2(120, 0)) || enterPressed) && nameValid) {
            Create(std::string(m_newNameBuf));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_showRenameDialog) {
        ImGui::OpenPopup("Rename Panel");
        m_showRenameDialog = false;
    }

    center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Rename Panel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Dummy(ImVec2(300.0f, 0.0f));
        ImGui::Text("New name:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        bool enterPressed =
            ImGui::InputText("##renamepanel", m_renameNameBuf, sizeof(m_renameNameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        bool nameValid = (m_renameNameBuf[0] != '\0');
        if ((ImGui::Button("Rename", ImVec2(120, 0)) || enterPressed) && nameValid) {
            Rename(m_renameId, std::string(m_renameNameBuf));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace adm::ui
