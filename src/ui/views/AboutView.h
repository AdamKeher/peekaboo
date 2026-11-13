#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../TextureManager.h"

namespace adm::ui {

class AboutView
{
public:
    explicit AboutView(TextureManager* textureMgr);
    ~AboutView() = default;

    void Open();
    void Close();
    void Draw();

    void SetCoreVersion(const std::string& version) { m_coreVersion = version; }

    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

private:
    struct LibraryInfo
    {
        std::string name;
        std::string url;
    };

    bool m_isOpen = false;
    bool m_openRequested = false;
    std::string m_appName;
    std::string m_appVersion;
    std::string m_coreVersion;
    std::string m_githubUrl;
    std::vector<LibraryInfo> m_libraries;

    // Logo texture (loaded lazily on first Draw)
    TextureManager* m_textureMgr = nullptr;
    uint32_t m_logoTexture = 0; // GLuint
    float m_logoAspect = 1.0f;
    bool m_logoLoadAttempted = false;
    void LoadLogoTexture();
};

} // namespace adm::ui
