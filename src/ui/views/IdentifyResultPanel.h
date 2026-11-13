#pragma once

#include <glad/glad.h>
#include "../../core/DiskIdentifier.h"
#include "../TextureManager.h"

namespace adm::ui {

// Shared panel that renders ScreenScraper and Pouet identification data.
// Used by both IdentifyView and TosecBrowserView to avoid duplication.
class IdentifyResultPanel
{
public:
    explicit IdentifyResultPanel(TextureManager* textureMgr);
    ~IdentifyResultPanel();

    void SetResult(const core::IdentifyResult& result);
    void ClearResult();
    bool HasResult() const { return m_hasResult; }
    bool HasScreenScraper() const { return m_hasResult && m_result.found; }
    bool HasPouet() const { return m_hasResult && m_result.hasPouetInfo; }

    // Draw the ScreenScraper data section (no spinner, no search button)
    void DrawScreenScraper();
    // Draw the Pouet data section (no spinner, no search button)
    void DrawPouet();

private:
    void ReleaseTextures();
    void EnsureTexturesLoaded();

    TextureManager* m_textureManager = nullptr;
    core::IdentifyResult m_result;
    bool m_hasResult = false;
    bool m_texturesLoaded = false;

    GLuint m_screenshotTex = 0;
    int m_screenshotW = 0, m_screenshotH = 0;
    GLuint m_boxArtTex = 0;
    int m_boxArtW = 0, m_boxArtH = 0;
    GLuint m_wheelTex = 0;
    int m_wheelW = 0, m_wheelH = 0;
    GLuint m_devLogoTex = 0;
    int m_devLogoW = 0, m_devLogoH = 0;
    GLuint m_pubLogoTex = 0;
    int m_pubLogoW = 0, m_pubLogoH = 0;
    GLuint m_rulezTex = 0;
    GLuint m_isokTex = 0;
    GLuint m_sucksTex = 0;
};

} // namespace adm::ui
