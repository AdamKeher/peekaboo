#pragma once

#include <imgui.h>
#include <map>
#include <string>

namespace adm::ui {

/**
 * @brief Centralized color manager that loads colors from JSON definitions.
 *
 * Singleton pattern to avoid reloading colors in every view.
 * Loads colors from:
 *   - definitions/colors.json (structure field colors)
 *   - definitions/colors_blocks.json (block type colors)
 */
class ColorManager
{
public:
    /// Get the singleton instance
    static ColorManager& Instance();

    /// Get a color by name from the loaded color map
    ImU32 GetColor(const std::string& name) const;

    /// Get a block color by type name
    ImU32 GetBlockColor(const std::string& type) const;

    /// Get all block colors (for legends)
    const std::map<std::string, ImU32>& GetBlockColors() const;

    /// Parse a hex color string (e.g., "#FF0000FF") to ImU32
    static ImU32 ParseHexColor(const std::string& hex);

    /// Check if a color exists
    bool HasColor(const std::string& name) const;
    bool HasBlockColor(const std::string& type) const;

    /// Force reload colors from disk
    void Reload();

private:
    ColorManager();
    void LoadColors();
    void LoadSyntaxColors();
    void LoadBlockColors();

    std::map<std::string, ImU32> m_colors;
    std::map<std::string, ImU32> m_blockColors;
    bool m_loaded = false;
};

} // namespace adm::ui
