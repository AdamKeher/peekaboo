#include "PanelView.h"
#include <fmt/core.h>
#include <imgui.h>
#include <imgui_internal.h>

namespace adm::ui {

namespace {

// If a window's label begins with a FontAwesome glyph (Private Use Area, U+E000..U+F8FF),
// return just that glyph as a UTF-8 string; otherwise return empty. Window labels in this app
// embed their icon as the leading character (e.g. ICON_FA_CODE " Disassembly###..."), so this
// lets a panel surface the icons of whatever windows are docked inside it.
std::string LeadingIcon(const char* name)
{
    if (!name)
        return {};
    const unsigned char* s = reinterpret_cast<const unsigned char*>(name);
    const unsigned char c0 = s[0];

    int len;
    unsigned int cp;
    if (c0 < 0x80)
        return {}; // plain ASCII first char → no icon
    else if ((c0 & 0xE0) == 0xC0) {
        len = 2;
        cp = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        len = 3;
        cp = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        len = 4;
        cp = c0 & 0x07;
    } else
        return {};

    for (int k = 1; k < len; k++) {
        if ((s[k] & 0xC0) != 0x80)
            return {};
        cp = (cp << 6) | (s[k] & 0x3F);
    }

    if (cp >= 0xE000 && cp <= 0xF8FF)
        return std::string(name, len);
    return {};
}

// Return the leading icon of the first window docked inside the dock node tree rooted at dockId
// (in tab order, walking the left split first), as a prefix with a trailing space. Empty if no
// docked window has an icon.
std::string FirstDockedIcon(ImGuiID dockId)
{
    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockId);
    if (!root)
        return {};

    ImGuiDockNode* stack[64];
    int sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        ImGuiDockNode* node = stack[--sp];
        if (!node)
            continue;
        for (int i = 0; i < node->Windows.Size; i++) {
            ImGuiWindow* w = node->Windows[i];
            if (!w)
                continue;
            std::string icon = LeadingIcon(w->Name);
            if (!icon.empty())
                return icon + " ";
        }
        // Push children last-first so the left split is processed before the right one.
        if (node->ChildNodes[1] && sp < 64)
            stack[sp++] = node->ChildNodes[1];
        if (node->ChildNodes[0] && sp < 64)
            stack[sp++] = node->ChildNodes[0];
    }
    return {};
}

// Ask every leaf in this panel's dock tree to close its tabs. ImGui applies this through each
// window's p_open pointer, so the owning view sees the same state transition as if its individual
// close button had been clicked.
void CloseDockedWindows(ImGuiID dockId)
{
    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockId);
    if (!root)
        return;

    ImGuiDockNode* stack[64];
    int sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        ImGuiDockNode* node = stack[--sp];
        if (!node)
            continue;
        if (node->IsLeafNode()) {
            node->WantCloseAll = true;
            continue;
        }
        if (node->ChildNodes[1] && sp < 64)
            stack[sp++] = node->ChildNodes[1];
        if (node->ChildNodes[0] && sp < 64)
            stack[sp++] = node->ChildNodes[0];
    }
}

} // namespace

PanelView::PanelView(std::string name, uint32_t id) : m_name(std::move(name)), m_id(id) {}

void PanelView::Draw()
{
    if (!m_isOpen && !m_permanent) {
        // This path also handles panels closed from the View menu, where Begin() is not called.
        CloseDockedWindows(static_cast<ImGuiID>(m_id));
        return;
    }

    // Prefix the panel title with an icon: a pinned icon if one was set (e.g. the Documents
    // panel's folder), otherwise the icon of the first window docked inside it (from last frame's
    // dock layout). The icon lives in the display portion of the label, ahead of the "###" marker,
    // so it doesn't affect the window's persistent ID — the ID seed stays "{name}##panel_{id}",
    // matching the historical "{name}##panel_{id}" identity so imgui.ini / dock state still binds.
    std::string iconPrefix =
        m_icon.empty() ? FirstDockedIcon(static_cast<ImGuiID>(m_id)) : m_icon + " ";
    std::string title = fmt::format("{}{}###{}##panel_{:08X}", iconPrefix, m_name, m_name, m_id);
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (m_noFocusOnAppearing) {
        windowFlags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }
    // Permanent panels have no close button (pass nullptr instead of &m_isOpen).
    bool* pOpen = m_permanent ? nullptr : &m_isOpen;
    bool windowOpen = ImGui::Begin(title.c_str(), pOpen, windowFlags);

    if (!m_isOpen && !m_permanent)
        CloseDockedWindows(static_cast<ImGuiID>(m_id));

    // Always submit the DockSpace, even when the window is collapsed (Begin returns false).
    // Skipping DockSpace() for even one frame causes ImGui to undock all child windows.
    // m_id is used directly as the DockSpace ID — it's a stable random uint32 stored in
    // workspaces.json, which ensures the ini can rebind docked children after workspace load.
    ImGui::DockSpace(static_cast<ImGuiID>(m_id), ImVec2(0, 0), ImGuiDockNodeFlags_None);

    if (windowOpen && !m_permanent) {
        if (ImGui::BeginPopupContextWindow("##panel_ctx", ImGuiPopupFlags_MouseButtonRight |
                                                              ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("Rename...") && m_onRename)
                m_onRename(m_id);
            if (ImGui::MenuItem("Delete Panel") && m_onDelete)
                m_onDelete(m_id);
            ImGui::EndPopup();
        }
    }

    ImGui::End();
}

} // namespace adm::ui
