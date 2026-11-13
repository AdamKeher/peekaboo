#include "BlitterVisOverlay.h"

#include <glad/glad.h>

#include <algorithm>

namespace adm::ui {

namespace blitvis = adm::core::blitvis;

BlitterVisOverlay::~BlitterVisOverlay()
{
    // No GL context is guaranteed at teardown; Reset() is the supported path and the owner
    // calls it. Deleting the texture here as well would risk a call on a dead context.
    m_pixels.clear();
    m_idMap.clear();
    m_isoBuf.clear();
    m_spanBuf.clear();
    m_paintedIds.clear();
}

void BlitterVisOverlay::SetDecayFrames(int frames)
{
    m_decayFrames = std::clamp(frames, 1, blitvis::kMaxDecayFrames);
}

void BlitterVisOverlay::Reset()
{
    if (m_texId != 0) {
        glDeleteTextures(1, &m_texId);
        m_texId = 0;
    }
    m_texW = m_texH = 0;
    m_pixels.clear();
    m_pixels.shrink_to_fit();
    m_idMap.clear();
    m_idMap.shrink_to_fit();
    m_isoBuf.clear();
    m_isoBuf.shrink_to_fit();
    m_spanBuf.clear();
    m_spanBuf.shrink_to_fit();
    m_paintedIds.clear();
    m_paintedIds.shrink_to_fit();
    m_lastSpanCount = 0;
    m_dirty = false;
}

void BlitterVisOverlay::SetIsolatedBlitId(uint32_t blitId)
{
    if (blitId == m_isolatedBlitId)
        return;
    m_isolatedBlitId = blitId;
    // Re-composite now: the pixels themselves are unchanged, but which of them are dimmed is
    // not, and the next Update() may be a long way off (or never, while paused).
    if (m_texId != 0 && !m_pixels.empty())
        CompositeAndUpload();
}

uint32_t BlitterVisOverlay::BlitIdAt(int x, int y) const
{
    return blitvis::BlitIdAt(m_idMap, m_texW, m_texH, x, y);
}

bool BlitterVisOverlay::HasSpansThisFrame(uint32_t blitId) const
{
    if (blitId == 0 || m_paintedIds.empty())
        return false;
    return std::binary_search(m_paintedIds.begin(), m_paintedIds.end(), blitId);
}

void BlitterVisOverlay::EnsureTexture(int w, int h)
{
    if (w == m_texW && h == m_texH && m_texId != 0)
        return;

    if (m_texId == 0)
        glGenTextures(1, &m_texId);

    m_texW = w;
    m_texH = h;
    m_pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
    m_idMap.assign(m_pixels.size(), 0u);
    m_isoBuf.clear();

    glBindTexture(GL_TEXTURE_2D, m_texId);
    // Nearest, not linear: a span is an exact set of Amiga pixels, and interpolating would
    // smear its edges past the pixels the blitter actually wrote.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void BlitterVisOverlay::CompositeAndUpload()
{
    if (m_texId == 0 || m_pixels.empty())
        return;

    const uint32_t* src = m_pixels.data();
    if (m_isolatedBlitId != 0) {
        blitvis::ApplyIsolation(m_pixels, m_idMap, m_isoBuf, m_isolatedBlitId);
        src = m_isoBuf.data();
    } else if (!m_isoBuf.empty()) {
        m_isoBuf.clear();
        m_isoBuf.shrink_to_fit();
    }

    glBindTexture(GL_TEXTURE_2D, m_texId);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_texW, m_texH, GL_RGBA, GL_UNSIGNED_BYTE, src);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void BlitterVisOverlay::Update(const adm::AmigaEmulator& emu)
{
    const int w = static_cast<int>(emu.GetWidth());
    const int h = static_cast<int>(emu.GetHeight());
    if (w <= 0 || h <= 0) {
        m_lastSpanCount = 0;
        m_paintedIds.clear();
        return;
    }

    EnsureTexture(w, h);

    // Fade what is already on the overlay before painting this frame's spans on top: a span
    // re-tagged every frame stays saturated, while a one-shot blit trails off over
    // m_decayFrames instead of being a single-frame flicker.
    bool anyVisible = blitvis::DecayPixels(m_pixels, m_decayFrames, &m_idMap);

    const int spanCount = emu.GetBlitVisSpanCount();
    m_lastSpanCount = spanCount;

    // Rebuilt from scratch every frame, including the empty case: a stale list would let a blit
    // that painted nothing this frame pass for one that did, and lose its geometry fallback.
    m_paintedIds.clear();

    if (spanCount > 0) {
        if (static_cast<int>(m_spanBuf.size()) < spanCount)
            m_spanBuf.resize(static_cast<size_t>(spanCount));
        const int got = emu.GetBlitVisSpans(m_spanBuf.data(), spanCount);
        blitvis::PaintSpans(m_pixels, m_texW, m_texH, m_spanBuf.data(), got, &m_idMap);
        blitvis::CollectPaintedIds(m_spanBuf.data(), got, m_paintedIds);
        anyVisible = true;
    }

    // Upload while anything is visible, plus the one frame that clears the last of it —
    // otherwise a faded-out overlay would stay on screen until the next blit.
    if (anyVisible || m_dirty)
        CompositeAndUpload();
    m_dirty = anyVisible;
}

void BlitterVisOverlay::Draw(ImDrawList* dl, ImVec2 imgMin, ImVec2 imgMax) const
{
    if (!dl || m_texId == 0 || m_texW <= 0 || m_texH <= 0)
        return;
    if (m_opacity <= 0.0f)
        return;

    const int a = std::clamp(static_cast<int>(m_opacity * 255.0f), 0, 255);
    dl->AddImage(static_cast<ImTextureID>(m_texId), imgMin, imgMax, ImVec2(0.0f, 0.0f),
                 ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, a));
}

} // namespace adm::ui
