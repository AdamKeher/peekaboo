#pragma once

// Menu rows that carry a FontAwesome icon draw that icon inside a fixed-width gully, so every
// label in a menu starts at the same x. The old "icon + two spaces + label" concatenation could
// not do that: icon glyphs have different advance widths, so the gap between icon and text changed
// from row to row and the menus looked ragged.
//
// Usage mirrors ImGui::MenuItem() / ImGui::BeginMenu() with the icon split out of the label:
//
//     if (MenuItemIcon(ICON_FA_FLOPPY_DISK, "Save ADF", "Ctrl+S", false, hasFile)) { ... }
//     if (BeginMenuIcon(ICON_FA_CLOCK, "Open Recents")) { ...; ImGui::EndMenu(); }
//
// A null or empty icon is fine - the row still reserves the gully, so iconless rows line up with
// their iconed neighbours. Plain ImGui::MenuItem() calls in the same menu line up too, because the
// gully widens ImGui's shared menu-column layout for the whole menu window.

namespace adm {
namespace ui {

// Width of the icon column in pixels; scales with the current font size.
float MenuIconGullyWidth();

bool MenuItemIcon(const char* icon, const char* label, const char* shortcut = nullptr,
                  bool selected = false, bool enabled = true);
bool MenuItemIcon(const char* icon, const char* label, const char* shortcut, bool* pSelected,
                  bool enabled = true);

// Pair with ImGui::EndMenu() exactly like ImGui::BeginMenu().
bool BeginMenuIcon(const char* icon, const char* label, bool enabled = true);

} // namespace ui
} // namespace adm
