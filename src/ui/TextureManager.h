#pragma once

#include <cstdint>
#include <unordered_set>

#include <glad/glad.h>
#include <imgui.h>

namespace adm::ui {

/**
 * @brief Manages OpenGL textures for ImGui display.
 *
 * Handles texture creation, tracking, and cleanup.
 * Must be destroyed before OpenGL context is destroyed.
 */
class TextureManager
{
public:
    TextureManager() = default;
    ~TextureManager();

    // Non-copyable
    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;

    // Movable
    TextureManager(TextureManager&& other) noexcept;
    TextureManager& operator=(TextureManager&& other) noexcept;

    /**
     * @brief Create texture from RGBA pixel data.
     * @param pixels RGBA8888 pixel data (4 bytes per pixel).
     * @param width Image width in pixels.
     * @param height Image height in pixels.
     * @return OpenGL texture ID, or 0 on failure.
     */
    [[nodiscard]] GLuint CreateTexture(const uint8_t* pixels, int width, int height);

    /**
     * @brief Release a texture.
     * @param textureId The texture to release.
     */
    void ReleaseTexture(GLuint textureId);

    /**
     * @brief Release all managed textures.
     */
    void ReleaseAll();

    /**
     * @brief Get the number of active textures.
     */
    [[nodiscard]] size_t GetTextureCount() const { return m_textures.size(); }

    /**
     * @brief Convert GLuint texture ID to ImTextureID for use with ImGui::Image().
     */
    [[nodiscard]] static ImTextureID ToImTextureID(GLuint textureId);

private:
    std::unordered_set<GLuint> m_textures;
};

} // namespace adm::ui
