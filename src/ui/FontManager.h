#pragma once

#include <imgui.h>
#include <string>
#include <vector>

namespace adm::ui {

struct FontInfo
{
    std::string name;
    ImFont* ptr;
};

class FontManager
{
public:
    void LoadFonts(ImGuiIO& io, float scale = 1.0f);
    [[nodiscard]] const std::vector<FontInfo>& GetAvailableFonts() const;
    [[nodiscard]] ImFont* GetFont(const std::string& name) const;
    // Face names a user can pick for text: every loaded font except the fixed-size copies.
    [[nodiscard]] std::vector<std::string> GetSelectableFaces() const;

private:
    std::vector<FontInfo> m_fonts;
};

} // namespace adm::ui
