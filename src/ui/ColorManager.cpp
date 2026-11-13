#include "ColorManager.h"

#include <fstream>
#include <nlohmann/json.hpp>

#include "../core/Logger.h"

namespace adm::ui {

ColorManager& ColorManager::Instance()
{
    static ColorManager instance;
    return instance;
}

ColorManager::ColorManager()
{
    LoadColors();
    LoadSyntaxColors();
    LoadBlockColors();
    m_loaded = true;
}

ImU32 ColorManager::GetColor(const std::string& name) const
{
    auto it = m_colors.find(name);
    if (it != m_colors.end()) {
        return it->second;
    }
    return IM_COL32_WHITE;
}

ImU32 ColorManager::GetBlockColor(const std::string& type) const
{
    auto it = m_blockColors.find(type);
    if (it != m_blockColors.end()) {
        return it->second;
    }
    return IM_COL32(50, 50, 50, 255); // Default gray
}

const std::map<std::string, ImU32>& ColorManager::GetBlockColors() const { return m_blockColors; }

ImU32 ColorManager::ParseHexColor(const std::string& hex)
{
    if (hex.empty() || hex[0] != '#')
        return IM_COL32_WHITE;
    uint32_t r = 255, g = 255, b = 255, a = 255;
    if (hex.length() == 9) {
        // #RRGGBBAA
        if (sscanf(hex.c_str(), "#%02x%02x%02x%02x", &r, &g, &b, &a) == 4) {
            return IM_COL32(r, g, b, a);
        }
    } else if (hex.length() == 7) {
        // #RRGGBB
        if (sscanf(hex.c_str(), "#%02x%02x%02x", &r, &g, &b) == 3) {
            return IM_COL32(r, g, b, 255);
        }
    }
    return IM_COL32_WHITE;
}

bool ColorManager::HasColor(const std::string& name) const
{
    return m_colors.find(name) != m_colors.end();
}

bool ColorManager::HasBlockColor(const std::string& type) const
{
    return m_blockColors.find(type) != m_blockColors.end();
}

void ColorManager::Reload()
{
    m_colors.clear();
    m_blockColors.clear();
    LoadColors();
    LoadSyntaxColors();
    LoadBlockColors();
}

void ColorManager::LoadColors()
{
    std::vector<std::string> paths = {"definitions/colors.json", "../../definitions/colors.json"};
    for (const auto& p : paths) {
        std::ifstream f(p);
        if (f.is_open()) {
            try {
                auto j = nlohmann::json::parse(f);
                if (j.contains("colors")) {
                    for (auto& [name, hex] : j["colors"].items()) {
                        m_colors[name] = ParseHexColor(hex);
                    }
                }
                return;
            } catch (...) {
                // Fall through to the next candidate path, but say why every color is about
                // to render as the default — a corrupt palette file was invisible before.
                core::Logger::Warn("ColorManager: failed to parse {} — defaults in effect", p);
            }
        }
    }
}

void ColorManager::LoadSyntaxColors()
{
    // Merged into m_colors so GetColor("Asm.*") resolves alongside the base palette.
    std::vector<std::string> paths = {"definitions/colors_syntax.json",
                                      "../../definitions/colors_syntax.json"};
    for (const auto& p : paths) {
        std::ifstream f(p);
        if (f.is_open()) {
            try {
                auto j = nlohmann::json::parse(f);
                if (j.contains("colors")) {
                    for (auto& [name, hex] : j["colors"].items()) {
                        m_colors[name] = ParseHexColor(hex);
                    }
                }
                return;
            } catch (...) {
                // Fall through to the next candidate path, but say why every color is about
                // to render as the default — a corrupt palette file was invisible before.
                core::Logger::Warn("ColorManager: failed to parse {} — defaults in effect", p);
            }
        }
    }
}

void ColorManager::LoadBlockColors()
{
    std::vector<std::string> paths = {"definitions/colors_blocks.json",
                                      "../../definitions/colors_blocks.json"};
    for (const auto& p : paths) {
        std::ifstream f(p);
        if (f.is_open()) {
            try {
                auto j = nlohmann::json::parse(f);
                if (j.contains("colors_blocks")) {
                    for (auto& [type, hex] : j["colors_blocks"].items()) {
                        m_blockColors[type] = ParseHexColor(hex);
                    }
                }
                return;
            } catch (...) {
                // Fall through to the next candidate path, but say why every color is about
                // to render as the default — a corrupt palette file was invisible before.
                core::Logger::Warn("ColorManager: failed to parse {} — defaults in effect", p);
            }
        }
    }
}

} // namespace adm::ui
