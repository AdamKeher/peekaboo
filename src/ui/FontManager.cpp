#include "FontManager.h"
#include <filesystem>
#include "../core/Constants.h"
#include "../core/Logger.h"
#include "ui/IconsFontAwesome6.h"

namespace adm::ui {

void FontManager::LoadFonts(ImGuiIO& io, float scale)
{
    m_fonts.clear();

    // Dynamic key loading
    namespace fs = std::filesystem;
    std::string fontDir(adm::core::constants::AssetsFontDir);

    bool fontsLoaded = false;

    // On macOS, the OS handles Retina scaling automatically, so don't scale fonts
    // On Windows/Linux, apply DPI scaling to fonts
#ifdef __APPLE__
    float fontScale = 1.0f;
#else
    float fontScale = scale;
#endif

    // Try to load system monospace fonts (platform-specific paths)
    const std::vector<std::pair<std::string, std::string>> systemFonts = {
        // macOS
        {"SF Mono", "/System/Library/Fonts/SFMono-Regular.otf"},
        {"SF Mono", "/Library/Fonts/SF-Mono-Regular.otf"},
        {"Menlo", "/System/Library/Fonts/Menlo.ttc"},
        // Windows
        {"Cascadia Code", "C:\\Windows\\Fonts\\Cascadia.ttf"},
        {"Cascadia Code", "C:\\Windows\\Fonts\\CascadiaCode.ttf"},
        {"Consolas", "C:\\Windows\\Fonts\\consola.ttf"}};

    // Helper to merge icons
    auto mergeIcons = [&](float scaleFactor) {
        const char* fontFiles[] = {FONT_ICON_FILE_NAME_FAR, FONT_ICON_FILE_NAME_FAS};

        for (const char* fileName : fontFiles) {
            std::string faPath = (fs::path(fontDir) / fileName).string();
            bool loaded = false;

            // Try assets directory
            if (fs::exists(faPath)) {
                ImFontConfig icons_config;
                icons_config.MergeMode = true;
                icons_config.PixelSnapH = true;
                static const ImWchar icons_ranges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
                io.Fonts->AddFontFromFileTTF(faPath.c_str(), 16.0f * scaleFactor, &icons_config,
                                             icons_ranges);
                loaded = true;
            }

            // Fallback to current directory if not found in assets
            if (!loaded) {
                faPath = fileName;
                if (fs::exists(faPath)) {
                    ImFontConfig icons_config;
                    icons_config.MergeMode = true;
                    icons_config.PixelSnapH = true;
                    static const ImWchar icons_ranges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
                    io.Fonts->AddFontFromFileTTF(faPath.c_str(), 16.0f * scaleFactor, &icons_config,
                                                 icons_ranges);
                }
            }
        }
    };

    for (const auto& [name, path] : systemFonts) {
        if (fs::exists(path)) {
            ImFont* newFont = io.Fonts->AddFontFromFileTTF(path.c_str(), 16.0f * fontScale);
            if (newFont) {
                mergeIcons(fontScale);
                m_fonts.push_back({name, newFont});
                core::Logger::Info("Fonts: Loaded system font: {}", name);
                fontsLoaded = true; // Mark as loaded so we don't hit fallback
                // Only load one system font as default
                break;
            }
        }
    }

    if (fs::exists(fontDir) && fs::is_directory(fontDir)) {
        for (const auto& entry : fs::directory_iterator(fontDir)) {
            // The Font Awesome files are merged into every face as icons; loaded on their own
            // they are not a face anyone can read text in.
            const bool iconFont = entry.path().filename() == FONT_ICON_FILE_NAME_FAR ||
                                  entry.path().filename() == FONT_ICON_FILE_NAME_FAS;
            if (entry.path().extension() == ".ttf" && !iconFont) {
                std::string fontPath = entry.path().string();
                ImFont* newFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f * fontScale);
                if (newFont) {
                    mergeIcons(fontScale);
                    std::string fontName = entry.path().stem().string();
                    m_fonts.push_back({fontName, newFont});
                    core::Logger::Info("Fonts: Loaded font: {}", fontName);
                    fontsLoaded = true;

                    // Specifically load amiga-demo-pixel-type-8x8 at larger sizes for transitions
                    if (fontName == "amiga-demo-pixel-type-8x8") {
                        ImFont* font48 =
                            io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 48.0f * fontScale);
                        if (font48)
                            m_fonts.push_back({fontName + "_48", font48});
                        ImFont* font24 =
                            io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 24.0f * fontScale);
                        if (font24)
                            m_fonts.push_back({fontName + "_24", font24});
                    }
                } else {
                    core::Logger::Error("Fonts: Failed to load font: {}", fontPath);
                }
            }
        }
    } else {
        core::Logger::Error("Fonts: Font directory not found: {}", fontDir);
    }

    if (!fontsLoaded) {
        // Fallback to default if no fonts loaded
        m_fonts.push_back({"Default", nullptr});
        io.Fonts->AddFontDefault();
        mergeIcons(fontScale);
    }

    // Build atlas
    core::Logger::Info("Fonts: Building font atlas...");
    if (!io.Fonts->Build()) {
        core::Logger::Error(
            "Fonts: Failed to build font atlas with custom fonts! Reverting to default.");
        io.Fonts->Clear();
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        m_fonts.clear();
        m_fonts.push_back({"Default", nullptr});
    } else {
        core::Logger::Info("Fonts: Font atlas built successfully.");
    }
}

const std::vector<FontInfo>& FontManager::GetAvailableFonts() const { return m_fonts; }

std::vector<std::string> FontManager::GetSelectableFaces() const
{
    // The _48 / _24 entries are the transition screen's fixed-size copies of a face already in
    // the list; with dynamic fonts they draw identically at any size, so offering them twice
    // only confuses the picker.
    std::vector<std::string> faces;
    for (const auto& font : m_fonts) {
        if (!font.ptr)
            continue;
        const auto& n = font.name;
        const bool sizedCopy = n.size() > 3 && (n.ends_with("_48") || n.ends_with("_24"));
        if (!sizedCopy)
            faces.push_back(n);
    }
    return faces;
}

ImFont* FontManager::GetFont(const std::string& name) const
{
    for (const auto& font : m_fonts) {
        if (font.name == name) {
            return font.ptr;
        }
    }
    return nullptr;
}

} // namespace adm::ui
