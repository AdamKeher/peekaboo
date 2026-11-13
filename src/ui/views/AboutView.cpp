#include "AboutView.h"

#include <imgui.h>
#include "../../core/Constants.h"
#include "../../core/Logger.h"

// nanosvg - headers only (implementation in nanosvg static lib)
#include <nanosvg.h>
#include <nanosvgrast.h>

namespace adm::ui {

AboutView::AboutView(TextureManager* textureMgr) : m_textureMgr(textureMgr)
{
    m_appName = core::constants::AppName;
    m_appVersion = core::constants::AppVersion;
    m_githubUrl = "https://github.com/AdamKeher/peekaboo";

    // The emulation first, then the libraries; every licence is in THIRD_PARTY_NOTICES.md
    m_libraries = {
        {"PUAE / libretro-uae (GPL-2.0)", "https://github.com/libretro/libretro-uae"},
        {"WinUAE (GPL-2.0)", "https://github.com/tonioni/WinUAE"},
        {"AROS Kickstart replacement (APL)", "https://aros.sourceforge.io"},
        {"WHDLoad", "https://www.whdload.de"},
        {"HRTmon", "https://github.com/tonioni/WinUAE"},
        {"FloppyBridge", "https://github.com/RobSmithDev/FloppyDriveBridge"},
        {"Boot block library: AmigaBootBlockReader (used with permission)",
         "https://github.com/jasonthesmith79/AmigaBootBlockReader"},
        {"ENGINE9000 (blitter provenance, copper overlay)",
         "https://github.com/alpine9000/engine9000-public"},
        {"Topaz and friends by dMG of Trueschool", "https://www.trueschool.se"},
        {"Font Awesome Free", "https://fontawesome.com"},
        {"Dear ImGui", "https://github.com/ocornut/imgui"},
        {"GLFW", "https://github.com/glfw/glfw"},
        {"GLAD", "https://github.com/Dav1dde/glad"},
        {"{fmt}", "https://github.com/fmtlib/fmt"},
        {"Native File Dialog (NFD)", "https://github.com/btzy/nativefiledialog-extended"},
        {"nlohmann/json", "https://github.com/nlohmann/json"},
        {"Crc32c", "https://github.com/google/crc32c"},
        {"ADFlib", "https://github.com/adflib/ADFlib"},
        {"xdms", "https://gitlab.com/heikkiorsila/xdms"},
        {"RECOIL", "https://recoil.sourceforge.net/"},
        {"miniaudio", "https://github.com/mackron/miniaudio"},
        {"libopenmpt", "https://libopenmpt.org/"}};
}

void AboutView::Open()
{
    m_isOpen = true;
    m_openRequested = true;
}

void AboutView::Close() { m_isOpen = false; }

void AboutView::LoadLogoTexture()
{
    m_logoLoadAttempted = true;
    if (!m_textureMgr)
        return;

    const char* svgPath = "assets/logo/peekaboo_wide.svg";
    NSVGimage* image = nsvgParseFromFile(svgPath, "px", 96.0f);
    if (!image) {
        adm::core::Logger::Error("AboutView: failed to parse SVG: {}", svgPath);
        return;
    }
    if (image->width <= 0.0f || image->height <= 0.0f) {
        nsvgDelete(image);
        return;
    }

    // Rasterize at high resolution for crispness when displayed large
    const int targetH = 256;
    const float scale = static_cast<float>(targetH) / image->height;
    const int targetW = static_cast<int>(image->width * scale + 0.5f);

    std::vector<uint8_t> pixels(static_cast<size_t>(targetW * targetH * 4), 0);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (rast) {
        nsvgRasterize(rast, image, 0.0f, 0.0f, scale, pixels.data(), targetW, targetH, targetW * 4);
        nsvgDeleteRasterizer(rast);
    }

    m_logoAspect = static_cast<float>(targetW) / static_cast<float>(targetH);
    m_logoTexture = m_textureMgr->CreateTexture(pixels.data(), targetW, targetH);

    nsvgDelete(image);
}

void AboutView::Draw()
{
    if (!m_isOpen)
        return;

    if (m_openRequested) {
        ImGui::OpenPopup("About");
        m_openRequested = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("About", &m_isOpen,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize)) {
        // Lazy-load the logo texture on first draw (GL context is ready by then)
        if (!m_logoLoadAttempted)
            LoadLogoTexture();

        // Logo — 80% of the available content width, aspect-ratio preserved, centred
        if (m_logoTexture != 0) {
            float availW = ImGui::GetContentRegionAvail().x;
            float logoW = availW * 0.8f;
            float logoH = logoW / m_logoAspect;
            float offsetX = (availW - logoW) * 0.5f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
            ImGui::Image(TextureManager::ToImTextureID(static_cast<GLuint>(m_logoTexture)),
                         ImVec2(logoW, logoH));
            ImGui::Spacing();
        }

        // App Info
        ImGui::SetWindowFontScale(1.2f);
        ImGui::Text("%s", m_appName.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Text("Version %s", m_appVersion.c_str());
        if (!m_coreVersion.empty())
            ImGui::Text("PuAE core %s", m_coreVersion.c_str());
        ImGui::Spacing();

        ImGui::Text("Github:");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL(m_githubUrl.c_str());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextDisabled("No Kickstart ROMs or Workbench are included: bring your own.");
        ImGui::TextDisabled("Licences: THIRD_PARTY_NOTICES.md beside the program.");
        ImGui::Spacing();
        ImGui::Text("Built with:");
        if (ImGui::BeginChild("libraries_scrolling", ImVec2(0, 150), true)) {
            for (const auto& lib : m_libraries) {
                ImGui::TextUnformatted(lib.name.c_str());
                ImGui::TextLinkOpenURL(lib.url.c_str(), lib.url.c_str());
                ImGui::Spacing();
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Close", ImVec2(120, 0))) {
            Close();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace adm::ui
