#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace adm::core {

/**
 * @brief Which set of screen-dependent settings is live: workspace layouts, text sizes, the main
 * window rectangle, table column widths and a few splitters.
 *
 * Workspace layouts are absolute-pixel ImGui ini text, so a layout made on a 4K monitor does not
 * fit a laptop (or the same machine reached over Remote Desktop) and vice versa. Every workspace
 * therefore keeps one layout per profile, and the user picks the profile by hand - the same
 * user_prefs.json serves both screens, so no launch-time guess can be right for both.
 */
enum class ScreenProfile {
    Hires,
    Lores,
};

inline constexpr ScreenProfile kAllScreenProfiles[] = {ScreenProfile::Hires, ScreenProfile::Lores};

/// The key used in workspaces.json and user_prefs.json.
inline const char* ScreenProfileKey(ScreenProfile p)
{
    return p == ScreenProfile::Lores ? "lores" : "hires";
}

/// The name shown in menus and dialogs.
inline const char* ScreenProfileLabel(ScreenProfile p)
{
    return p == ScreenProfile::Lores ? "Lores" : "Hires";
}

inline std::optional<ScreenProfile> ParseScreenProfile(std::string_view key)
{
    if (key == "hires")
        return ScreenProfile::Hires;
    if (key == "lores")
        return ScreenProfile::Lores;
    return std::nullopt;
}

inline ScreenProfile OtherScreenProfile(ScreenProfile p)
{
    return p == ScreenProfile::Lores ? ScreenProfile::Hires : ScreenProfile::Lores;
}

/// Narrowest screen, in interface units (work-area pixels divided by the DPI and zoom scale),
/// that the Hires layouts are suggested for. A 4K monitor at 100% is 3840; a MacBook Air is
/// 1470 points; Remote Desktop at 200% and a 1080p monitor land around 1500-1900.
inline constexpr float kHiresMinEffectiveWidth = 2560.0f;

/// The profile suggested for a screen - only ever a suggestion, offered once on first launch.
inline ScreenProfile ClassifyScreen(float effectiveWidth)
{
    return effectiveWidth >= kHiresMinEffectiveWidth ? ScreenProfile::Hires : ScreenProfile::Lores;
}

/**
 * @brief The main viewport's dock space rectangle in an ImGui ini: the DockSpace with the
 * largest area. Panels host dock spaces of their own, but always inside the viewport.
 * @return false when the ini has no dock space.
 */
inline bool FindMainDockSpace(std::string_view ini, float& x, float& y, float& w, float& h)
{
    float bestArea = 0.0f;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t eol = ini.find('\n', pos);
        if (eol == std::string_view::npos)
            eol = ini.size();
        const std::string_view line = ini.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.rfind("DockSpace", 0) != 0)
            continue;
        const size_t p = line.find(" Pos=");
        const size_t s = line.find(" Size=");
        if (p == std::string_view::npos || s == std::string_view::npos)
            continue;
        const std::string posText(line.substr(p + 5));
        const std::string sizeText(line.substr(s + 6));
        char* end = nullptr;
        const float px = std::strtof(posText.c_str(), &end);
        const float py = (*end == ',') ? std::strtof(end + 1, nullptr) : 0.0f;
        const float sw = std::strtof(sizeText.c_str(), &end);
        const float sh = (*end == ',') ? std::strtof(end + 1, nullptr) : 0.0f;
        if (sw * sh > bestArea) {
            bestArea = sw * sh;
            x = px;
            y = py;
            w = sw;
            h = sh;
        }
    }
    return bestArea > 0.0f;
}

/**
 * @brief @p ini with every Pos=, Size= and SizeRef= pair multiplied by (@p sx, @p sy).
 *
 * Those are the only geometry an ImGui ini holds, so this turns a layout made for one screen into
 * one for another: floating windows keep their place relative to the screen, and the dock tree
 * keeps its proportions because every SizeRef scales alike. ImGui refits the root dock node to
 * the real viewport when the layout is applied, so the result need only be close. Everything
 * else - ids, DockId, Collapsed, Selected, [Table] sections - passes through untouched.
 */
inline std::string ScaleImGuiIni(std::string_view ini, float sx, float sy)
{
    static constexpr std::string_view kKeys[] = {"Pos=", "Size=", "SizeRef="};
    std::string out;
    out.reserve(ini.size());
    size_t i = 0;
    while (i < ini.size()) {
        // A key only counts at the start of a line or after a space - "Size=" must not match the
        // tail of some other key.
        const bool atTokenStart = (i == 0 || ini[i - 1] == '\n' || ini[i - 1] == ' ');
        std::string_view key;
        if (atTokenStart) {
            for (std::string_view k : kKeys) {
                if (ini.substr(i, k.size()) == k) {
                    key = k;
                    break;
                }
            }
        }
        if (key.empty()) {
            out.push_back(ini[i++]);
            continue;
        }

        const size_t valueStart = i + key.size();
        size_t valueEnd = valueStart;
        while (valueEnd < ini.size() && ini[valueEnd] != ' ' && ini[valueEnd] != '\n' &&
               ini[valueEnd] != '\r')
            ++valueEnd;
        const std::string value(ini.substr(valueStart, valueEnd - valueStart));
        const size_t comma = value.find(',');
        char* endA = nullptr;
        char* endB = nullptr;
        const long a = comma == std::string::npos ? 0 : std::strtol(value.c_str(), &endA, 10);
        const long b =
            comma == std::string::npos ? 0 : std::strtol(value.c_str() + comma + 1, &endB, 10);
        const bool parsed = comma != std::string::npos && endA == value.c_str() + comma &&
                            endB != nullptr && *endB == '\0';
        out.append(key);
        if (!parsed) {
            out.append(value);
        } else {
            long na = std::lround(static_cast<float>(a) * sx);
            long nb = std::lround(static_cast<float>(b) * sy);
            if (key != "Pos=") {
                // A zero-sized window or node is one nobody can grab again.
                na = std::max(na, 1L);
                nb = std::max(nb, 1L);
            }
            out.append(std::to_string(na));
            out.push_back(',');
            out.append(std::to_string(nb));
        }
        i = valueEnd;
    }
    return out;
}

} // namespace adm::core
