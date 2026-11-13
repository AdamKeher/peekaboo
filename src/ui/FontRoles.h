#pragma once

#include <imgui.h>
#include <array>

#include "../core/UserPreferences.h"

namespace adm::ui {

class FontManager;

/**
 * @brief What a piece of text is, for sizing: the interface around it, or one of the dense
 *        listings people read for long stretches and want larger or smaller than the chrome.
 *
 * The order is the order of UserPreferences::FontPrefs::roles.
 */
enum class FontRole {
    Interface = 0,
    Code,    // disassembly, copper list, CPU / custom / CIA registers, call stack, blitter log
    Memory,  // hex and memory dumps
    Console, // the debugger console and the logs
};

inline constexpr int kFontRoleCount = core::UserPreferences::kFontRoleCount;

const char* FontRoleName(FontRole role);
// The windows a role covers, for the Preferences table.
const char* FontRoleWindows(FontRole role);

/**
 * @brief The resolved font and size of every role, set from preferences.
 *
 * ImGui 1.92 draws any font at any size on demand, so nothing here rebuilds the atlas: a change
 * is a different ImFont* or a different number passed to PushFont on the next frame.
 */
class FontRoles
{
public:
    void Configure(const core::UserPreferences::FontPrefs& prefs, const FontManager& fonts);

    // nullptr means "the interface font" (PushFont keeps the current one).
    [[nodiscard]] ImFont* Font(FontRole role) const { return m_fonts[static_cast<int>(role)]; }
    // Relative to the interface text size; 1.0 for the Interface role itself.
    [[nodiscard]] float Scale(FontRole role) const { return m_scales[static_cast<int>(role)]; }

    // Ctrl+wheel over a role's window asks for a step; Application takes the steps once per
    // frame and turns them into a preference change.
    // One step per role per frame, however many scopes of that role the hovered window holds.
    void RequestStep(FontRole role, int steps);
    [[nodiscard]] std::array<int, kFontRoleCount> TakeSteps()
    {
        auto out = m_steps;
        m_steps.fill(0);
        return out;
    }

private:
    std::array<ImFont*, kFontRoleCount> m_fonts{};
    std::array<float, kFontRoleCount> m_scales{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<int, kFontRoleCount> m_steps{};
    std::array<int, kFontRoleCount> m_stepFrame{-1, -1, -1, -1};
};

FontRoles& GlobalFontRoles();

/**
 * @brief Draws everything up to the matching PopFontRole in a role's font and size.
 *
 * The ImGui-style pair for spans too long to wrap in a scope; ScopedFontRole is the same thing
 * as RAII. Pop also turns Ctrl+wheel over the current window into a step of the role's size.
 */
void PushFontRole(FontRole role);
void PopFontRole();

/**
 * @brief Draws everything in its scope in a role's font and size.
 *
 * Construct it after a successful Begin, around the listing rather than the toolbar above it,
 * so title bars, tabs and buttons stay at interface size. Ctrl+wheel over the window steps the
 * role's size.
 */
class ScopedFontRole
{
public:
    explicit ScopedFontRole(FontRole role) { PushFontRole(role); }
    ~ScopedFontRole() { PopFontRole(); }
    ScopedFontRole(const ScopedFontRole&) = delete;
    ScopedFontRole& operator=(const ScopedFontRole&) = delete;
};

} // namespace adm::ui
