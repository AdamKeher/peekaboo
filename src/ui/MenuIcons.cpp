#include "MenuIcons.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace adm {
namespace ui {

namespace {

struct IconGully
{
    ImGuiWindow* window = nullptr;
    ImVec2 pos{0.0f, 0.0f};
    float width = 0.0f;
    bool valid = false;
};

// Reserves the icon column for the item that is about to be submitted, and reports where its icon
// should land. ImGui's menu columns are accumulators shared by every row of a menu window, so
// declaring a floor here widens the column for the whole menu - which is what keeps the gully the
// same width on rows with a narrow glyph, a wide glyph, or no glyph at all. The offsets those
// widths feed are locked in one frame later, so read the locked value back instead of assuming our
// own floor won this frame.
IconGully ReserveIconGully(const char* icon)
{
    IconGully gully;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems || window->DC.LayoutType == ImGuiLayoutType_Horizontal)
        return gully; // menu-bar rows lay out horizontally and have no icon column

    const float iconWidth = (icon && icon[0]) ? ImGui::CalcTextSize(icon).x : 0.0f;
    const float declared = ImMax(MenuIconGullyWidth(), IM_TRUNC(iconWidth));
    window->DC.MenuColumns.DeclColumns(declared, 0.0f, 0.0f, 0.0f);

    const float locked =
        (float)window->DC.MenuColumns.OffsetLabel - (float)window->DC.MenuColumns.Spacing;
    gully.window = window;
    gully.pos =
        ImVec2(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    gully.width = locked > 0.0f ? locked : declared;
    gully.valid = true;
    return gully;
}

// Drawn after the item so it sits on top of the row highlight. ImGui's own icon column renders
// left-aligned; centring in the gully is what makes narrow and wide glyphs read as one column.
void DrawIconInGully(const IconGully& gully, const char* icon, bool enabled)
{
    if (!gully.valid || !icon || !icon[0])
        return;

    const float iconWidth = ImGui::CalcTextSize(icon).x;
    const ImVec2 at(gully.pos.x + IM_TRUNC((gully.width - iconWidth) * 0.5f), gully.pos.y);
    // Matches how BeginDisabled() dims an item: the text colour at DisabledAlpha, not TextDisabled.
    const ImU32 color =
        ImGui::GetColorU32(ImGuiCol_Text, enabled ? 1.0f : ImGui::GetStyle().DisabledAlpha);
    gully.window->DrawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, color, icon);
}

} // namespace

float MenuIconGullyWidth() { return IM_TRUNC(ImGui::GetFontSize() * 1.30f); }

bool MenuItemIcon(const char* icon, const char* label, const char* shortcut, bool selected,
                  bool enabled)
{
    const IconGully gully = ReserveIconGully(icon);
    const bool pressed = ImGui::MenuItemEx(label, nullptr, shortcut, selected, enabled);
    DrawIconInGully(gully, icon, enabled);
    return pressed;
}

bool MenuItemIcon(const char* icon, const char* label, const char* shortcut, bool* pSelected,
                  bool enabled)
{
    if (MenuItemIcon(icon, label, shortcut, pSelected && *pSelected, enabled)) {
        if (pSelected)
            *pSelected = !*pSelected;
        return true;
    }
    return false;
}

bool BeginMenuIcon(const char* icon, const char* label, bool enabled)
{
    const IconGully gully = ReserveIconGully(icon);
    const bool open = ImGui::BeginMenuEx(label, nullptr, enabled);
    DrawIconInGully(gully, icon, enabled);
    return open;
}

} // namespace ui
} // namespace adm
