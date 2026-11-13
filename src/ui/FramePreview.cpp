#include "FramePreview.h"

#include <glad/glad.h>

#include <imgui.h>

namespace adm::ui {

FramePreview::~FramePreview()
{
    // Only ever created on the GL thread inside a Draw(), and its owner outlives no context.
    if (m_tex && glad_glDeleteTextures) {
        GLuint tex = m_tex;
        glDeleteTextures(1, &tex);
        m_tex = 0;
    }
}

void FramePreview::SetRange(uint64_t first, uint64_t last)
{
    if (first != m_first || last != m_last)
        m_frame = UINT64_MAX;
    m_first = first;
    m_last = last;
}

bool FramePreview::Draw(uint64_t frame, const Source& source, float width)
{
    if (!source)
        return false;

    if (frame != m_frame) {
        int w = 0, h = 0;
        uint64_t got = 0;
        if (!source(frame, m_rgb, w, h, got) || w <= 0 || h <= 0)
            return false;

        if (!m_tex) {
            GLuint tex = 0;
            glGenTextures(1, &tex);
            m_tex = tex;
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, m_tex);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (w != m_w || h != m_h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, m_rgb.data());
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, m_rgb.data());
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);

        m_w = w;
        m_h = h;
        m_frame = frame;
        m_gotFrame = got;
    }
    if (!m_tex || m_w <= 0)
        return false;

    // Drawn at the Amiga's display shape rather than the thumbnail's own pixel ratio, so the
    // preview looks like the emulator window it stands in for.
    ImGui::BeginGroup();
    ImGui::Image((ImTextureID)(uintptr_t)m_tex, ImVec2(width, width * 3.0f / 4.0f));
    if (m_gotFrame != frame) {
        // Say which frame the picture is really from: previews are periodic, and letting it
        // pass as frame N when it is the shot from N-7 would be a quiet lie. Wrapped to the
        // image, so a small preview stays a column.
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextDisabled("nearest preview: frame %llu",
                            static_cast<unsigned long long>(m_gotFrame));
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    return true;
}

} // namespace adm::ui
