#include "ImageViewer.h"

#include "../TextureManager.h"

#include <algorithm>

namespace adm {
namespace ui {

ImageViewer::ImageViewer(TextureManager* textureManager) : m_textureManager(textureManager) {}

ImageViewer::~ImageViewer() { ReleaseTexture(); }

void ImageViewer::SetImage(const core::DecodedImage& image)
{
    ReleaseTexture();

    if (!image.IsValid()) {
        return;
    }

    m_imageWidth = image.width;
    m_imageHeight = image.height;
    m_pixelAspectRatio = image.pixelAspectRatio;

    if (m_textureManager) {
        m_textureId =
            m_textureManager->CreateTexture(image.pixels.data(), image.width, image.height);
    }

    UpdateDisplaySize();
    m_fitToWindow = true;
    m_zoom = 1.0f;
}

void ImageViewer::SetTitle(const std::string& title) { m_title = title; }

void ImageViewer::Open() { m_isOpen = true; }

void ImageViewer::UpdateDisplaySize()
{
    if (m_correctAspectRatio && m_pixelAspectRatio != 1.0f) {
        // Apply pixel aspect ratio correction
        // PAR > 1 means pixels are tall (stretch width)
        // PAR < 1 means pixels are wide (stretch height)
        if (m_pixelAspectRatio > 1.0f) {
            m_displayWidth = static_cast<float>(m_imageWidth) * m_pixelAspectRatio;
            m_displayHeight = static_cast<float>(m_imageHeight);
        } else {
            m_displayWidth = static_cast<float>(m_imageWidth);
            m_displayHeight = static_cast<float>(m_imageHeight) / m_pixelAspectRatio;
        }
    } else {
        m_displayWidth = static_cast<float>(m_imageWidth);
        m_displayHeight = static_cast<float>(m_imageHeight);
    }
}

void ImageViewer::Draw()
{
    if (!m_isOpen || m_textureId == 0) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(m_title.c_str(), &m_isOpen, ImGuiWindowFlags_NoSavedSettings)) {
        // Toolbar
        if (ImGui::Button("Fit")) {
            m_fitToWindow = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("1:1")) {
            m_zoom = 1.0f;
            m_fitToWindow = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("2x")) {
            m_zoom = 2.0f;
            m_fitToWindow = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("4x")) {
            m_zoom = 4.0f;
            m_fitToWindow = false;
        }

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        bool aspectChanged = ImGui::Checkbox("Correct Aspect Ratio", &m_correctAspectRatio);
        if (aspectChanged) {
            UpdateDisplaySize();
        }

        // Info line
        ImGui::Text("%dx%d", m_imageWidth, m_imageHeight);
        if (m_pixelAspectRatio != 1.0f) {
            ImGui::SameLine();
            ImGui::TextDisabled("(PAR: %.2f)", m_pixelAspectRatio);
        }
        if (m_correctAspectRatio && m_pixelAspectRatio != 1.0f) {
            ImGui::SameLine();
            ImGui::TextDisabled("-> %.0fx%.0f", m_displayWidth, m_displayHeight);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Zoom: %.0f%%", m_zoom * 100.0f);

        ImGui::Separator();

        // Image display area with scrolling
        ImVec2 availSize = ImGui::GetContentRegionAvail();

        float displayW = m_displayWidth;
        float displayH = m_displayHeight;

        if (m_fitToWindow && availSize.x > 0 && availSize.y > 0) {
            // Scale to fit within available area while maintaining aspect ratio
            float scaleX = availSize.x / displayW;
            float scaleY = availSize.y / displayH;
            m_zoom = std::min(scaleX, scaleY);
            // Don't scale up beyond 1:1 in fit mode unless image is smaller
            if (m_zoom > 1.0f && displayW <= availSize.x && displayH <= availSize.y) {
                m_zoom = 1.0f;
            }
        }

        float finalW = displayW * m_zoom;
        float finalH = displayH * m_zoom;

        // Check if image is larger than available area (needs panning)
        bool canPan = (finalW > availSize.x) || (finalH > availSize.y);

        // Use child window for scrolling with visible scrollbars for feedback
        ImGuiWindowFlags childFlags = ImGuiWindowFlags_HorizontalScrollbar;
        if (ImGui::BeginChild("ImageArea", availSize, false, childFlags)) {
            ImVec2 childSize = ImGui::GetContentRegionAvail();

            // Create an invisible button over the entire area to capture mouse input
            // This prevents the click from dragging the parent window
            ImVec2 contentSize =
                ImVec2(std::max(finalW, childSize.x), std::max(finalH, childSize.y));
            ImGui::InvisibleButton("##imagePanArea", contentSize);
            bool isHovered = ImGui::IsItemHovered();
            bool isActive = ImGui::IsItemActive();

            // Handle drag-to-pan
            if (canPan && isHovered) {
                // Change cursor to indicate draggable
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            if (isActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                float newScrollX = ImGui::GetScrollX() - delta.x;
                float newScrollY = ImGui::GetScrollY() - delta.y;
                ImGui::SetScrollX(newScrollX);
                ImGui::SetScrollY(newScrollY);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }

            // Reset cursor position to draw image at the right place
            ImGui::SetCursorPos(ImVec2(0, 0));

            // Center image if smaller than child area
            float offsetX = 0.0f;
            float offsetY = 0.0f;
            if (finalW < childSize.x) {
                offsetX = (childSize.x - finalW) * 0.5f;
            }
            if (finalH < childSize.y) {
                offsetY = (childSize.y - finalH) * 0.5f;
            }

            if (offsetX > 0 || offsetY > 0) {
                ImGui::SetCursorPos(
                    ImVec2(ImGui::GetCursorPosX() + offsetX, ImGui::GetCursorPosY() + offsetY));
            }

            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
            ImGui::Image(TextureManager::ToImTextureID(m_textureId), ImVec2(finalW, finalH));
            ImGui::GetWindowDrawList()->AddCallback(
                ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);

            // Handle zoom with mouse wheel
            if (ImGui::IsWindowHovered() && !m_fitToWindow) {
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    m_zoom *= (wheel > 0) ? 1.1f : 0.9f;
                    m_zoom = std::clamp(m_zoom, 0.1f, 16.0f);
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();

    // Handle window close - release texture
    if (!m_isOpen) {
        ReleaseTexture();
    }
}

void ImageViewer::ReleaseTexture()
{
    if (m_textureId && m_textureManager) {
        m_textureManager->ReleaseTexture(m_textureId);
        m_textureId = 0;
    }
}

} // namespace ui
} // namespace adm
