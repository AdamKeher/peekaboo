#include "FontRoles.h"

#include "../core/PrivateFeatures.h"
#include "FontManager.h"

namespace adm::ui {

const char* FontRoleName(FontRole role)
{
    switch (role) {
        case FontRole::Interface:
            return "Interface";
        case FontRole::Code:
            return "Code & registers";
        case FontRole::Memory:
            return "Memory & hex";
        case FontRole::Console:
            return "Console & logs";
    }
    return "";
}

const char* FontRoleWindows(FontRole role)
{
    switch (role) {
        case FontRole::Interface:
            return "Menus, title bars, tabs, toolbars and every window not listed below.";
        case FontRole::Code:
            return "Disassembly, Profile, Copper, CPU, Custom Registers, CIA Registers, Call "
                   "Stack, "
                   "Blitter Operations, Register Operations, Breakpoints, Hunk viewer.";
        case FontRole::Memory:
            return core::PrivateFeatures() ? "Memory, Exocet Memory, Block (hex), Annotations."
                                           : "Memory, Block (hex), Annotations.";
        case FontRole::Console:
            return "Debugger Console, Log, MCP Activity.";
    }
    return "";
}

void FontRoles::Configure(const core::UserPreferences::FontPrefs& prefs, const FontManager& fonts)
{
    for (int i = 0; i < kFontRoleCount; ++i) {
        const auto& role = prefs.roles[i];
        // A face that is no longer installed falls back to the interface font rather than
        // leaving the role unreadable.
        m_fonts[i] = role.face.empty() ? nullptr : fonts.GetFont(role.face);
        m_scales[i] = (i == static_cast<int>(FontRole::Interface)) ? 1.0f : role.scale;
    }
}

void FontRoles::RequestStep(FontRole role, int steps)
{
    const int i = static_cast<int>(role);
    const int frame = ImGui::GetFrameCount();
    if (m_stepFrame[i] == frame)
        return;
    m_stepFrame[i] = frame;
    m_steps[i] += steps;
}

FontRoles& GlobalFontRoles()
{
    static FontRoles roles;
    return roles;
}

namespace {
// Which role each open PushFontRole was for, so the pop knows what Ctrl+wheel should resize.
ImVector<FontRole> g_roleStack;
} // namespace

void PushFontRole(FontRole role)
{
    const auto& roles = GlobalFontRoles();
    ImGui::PushFont(roles.Font(role), ImGui::GetStyle().FontSizeBase * roles.Scale(role));
    g_roleStack.push_back(role);
}

void PopFontRole()
{
    ImGui::PopFont();
    IM_ASSERT(!g_roleStack.empty());
    const FontRole role = g_roleStack.back();
    g_roleStack.pop_back();

    // ImGui neither scrolls nor zooms on Ctrl+wheel (FontAllowUserScaling is off), so the wheel
    // is free for this.
    const ImGuiIO& io = ImGui::GetIO();
    if (role != FontRole::Interface && io.KeyCtrl && io.MouseWheel != 0.0f &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        GlobalFontRoles().RequestStep(role, io.MouseWheel > 0.0f ? 1 : -1);
    }
}

} // namespace adm::ui
