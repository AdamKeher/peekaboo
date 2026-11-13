#pragma once

#include <cstdint>
#include <string>

#include <glad/glad.h>
#include <imgui.h>

#include "../../core/ImageLoader.h"

namespace adm {
namespace ui {

class TextureManager;

/**
 * @brief Image viewer window with zoom and aspect ratio correction.
 *
 * Displays decoded Amiga images using ImGui::Image() with support for
 * non-square pixel aspect ratios common in Amiga display modes.
 */
class ImageViewer
{
public:
    explicit ImageViewer(TextureManager* textureManager);
    ~ImageViewer();

    // Non-copyable
    ImageViewer(const ImageViewer&) = delete;
    ImageViewer& operator=(const ImageViewer&) = delete;

    /**
     * @brief Set the decoded image to display.
     * @param image The decoded image data from ImageLoader.
     */
    void SetImage(const core::DecodedImage& image);

    /**
     * @brief Set the window title.
     * @param title The title string.
     */
    void SetTitle(const std::string& title);

    /**
     * @brief Open the viewer window.
     */
    void Open();

    /**
     * @brief Draw the viewer window.
     */
    void Draw();

    /**
     * @brief Check if the window is open.
     * @return true if open, false otherwise.
     */
    [[nodiscard]] bool IsOpen() const { return m_isOpen; }

private:
    void ReleaseTexture();
    void UpdateDisplaySize();

    TextureManager* m_textureManager;

    std::string m_title = "Image Viewer";
    bool m_isOpen = false;

    // Image data
    int m_imageWidth = 0;
    int m_imageHeight = 0;
    float m_pixelAspectRatio = 1.0f;
    GLuint m_textureId = 0;

    // View state
    float m_zoom = 1.0f;
    bool m_fitToWindow = true;
    bool m_correctAspectRatio = true;

    // Display dimensions (after aspect correction)
    float m_displayWidth = 0;
    float m_displayHeight = 0;
};

} // namespace ui
} // namespace adm
