#include "TextureManager.h"

#include "../core/Logger.h"

namespace adm::ui {

TextureManager::~TextureManager() { ReleaseAll(); }

TextureManager::TextureManager(TextureManager&& other) noexcept
    : m_textures(std::move(other.m_textures))
{
    other.m_textures.clear();
}

TextureManager& TextureManager::operator=(TextureManager&& other) noexcept
{
    if (this != &other) {
        ReleaseAll();
        m_textures = std::move(other.m_textures);
        other.m_textures.clear();
    }
    return *this;
}

GLuint TextureManager::CreateTexture(const uint8_t* pixels, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0) {
        core::Logger::Error("TextureManager: Invalid texture parameters");
        return 0;
    }

    GLuint textureId = 0;
    glGenTextures(1, &textureId);

    if (textureId == 0) {
        core::Logger::Error("TextureManager: Failed to generate texture");
        return 0;
    }

    glBindTexture(GL_TEXTURE_2D, textureId);

    // Set texture parameters
    // Use NEAREST for magnification (better for pixel art)
    // Use LINEAR for minification (smoother when scaled down)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Upload pixel data
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glBindTexture(GL_TEXTURE_2D, 0);

    m_textures.insert(textureId);

    // core::Logger::Debug("TextureManager: Created texture {} ({}x{})", textureId, width, height);

    return textureId;
}

void TextureManager::ReleaseTexture(GLuint textureId)
{
    if (textureId == 0) {
        return;
    }

    auto it = m_textures.find(textureId);
    if (it != m_textures.end()) {
        glDeleteTextures(1, &textureId);
        m_textures.erase(it);
        // core::Logger::Debug("TextureManager: Released texture {}", textureId);
    }
}

void TextureManager::ReleaseAll()
{
    for (GLuint textureId : m_textures) {
        glDeleteTextures(1, &textureId);
    }

    if (!m_textures.empty()) {
        // core::Logger::Debug("TextureManager: Released {} textures", m_textures.size());
    }

    m_textures.clear();
}

ImTextureID TextureManager::ToImTextureID(GLuint textureId)
{
    // ImGui expects ImTextureID which is typically void* or uintptr_t
    // Cast through uintptr_t to safely convert from GLuint
    return static_cast<ImTextureID>(static_cast<uintptr_t>(textureId));
}

} // namespace adm::ui
