#pragma once

#include <imgui.h>

#include <cstdint>
#include <vector>

#include "../../core/AmigaEmulator.h"
#include "../../core/BlitVisRaster.h"

typedef unsigned int GLuint;

namespace adm::ui {

// Paints the core's blitter destination provenance spans over the emulator image.
//
// The spans arrive from the core already in native output pixel coordinates — they were
// tagged at display-fetch time, so BPLxMOD, fetch mode, x-scroll delay, plane number, crop
// and scale have all been accounted for by the emulator's own display logic. That is the
// whole point of the technique (see patches/adm_blitvis.c): there is no coordinate mapping
// to get wrong here, unlike the geometry-based BlitScreenMapper path.
//
// This class is only the GL side: keep a texture the size of the emulator frame and upload it.
// The rasterisation itself lives in adm::core::blitvis, which is pure and unit-tested.
class BlitterVisOverlay
{
public:
    ~BlitterVisOverlay();

    // Rebuilds the overlay texture from the emulator's spans for the frame just run.
    // Cheap no-op when the core has no spans (or predates the provenance patch).
    void Update(const adm::AmigaEmulator& emu);

    // Draws the overlay over an emulator image occupying [imgMin, imgMax].
    void Draw(ImDrawList* dl, ImVec2 imgMin, ImVec2 imgMax) const;

    // Frees the texture and pixel buffer. Call when the overlay is switched off so it is not
    // holding a frame-sized RGBA buffer for a feature nobody is looking at.
    void Reset();

    // When non-zero, only this blit's spans are drawn at full strength and every other blit
    // is dimmed — used to highlight the blit selected in the Frame Scrubber or the Blitter
    // Operations window.
    //
    // Re-composites and re-uploads the texture on a change rather than waiting for the next
    // Update(), because Update() only runs when the emulator advances a frame and the
    // selection is usually being made while it is paused. Needs a current GL context.
    void SetIsolatedBlitId(uint32_t blitId);
    uint32_t GetIsolatedBlitId() const { return m_isolatedBlitId; }

    // Which blit owns the native output pixel at (x, y), or 0 for none. The inverse of the
    // overlay: it turns a point on the emulator picture back into a blit id, which the caller
    // joins onto BlitRecord::blitId. Includes pixels still fading from earlier frames, whose
    // ids may no longer match any record in the current frame.
    uint32_t BlitIdAt(int x, int y) const;

    // Whether this blit painted anything in the frame just run.
    //
    // False means the display never fetched what it wrote -- an off-screen buffer, typically --
    // so there is nothing here to show for it and the caller should fall back to a geometry box
    // (core/BlitScreenMapper.h). Answers only for the current frame: pixels still decaying from
    // earlier frames belong to blits the profiler no longer holds records for.
    bool HasSpansThisFrame(uint32_t blitId) const;

    // Size of the overlay texture in native output pixels (0 before the first Update).
    int GetTexWidth() const { return m_texW; }
    int GetTexHeight() const { return m_texH; }

    // Frames a span stays visible for. 1 = only the frame it was fetched in, which makes a
    // one-shot blit a single-frame flicker; the default trails it over a few frames so the
    // eye can catch it. Clamped to [1, adm::core::blitvis::kMaxDecayFrames].
    void SetDecayFrames(int frames);
    int GetDecayFrames() const { return m_decayFrames; }

    void SetOpacity(float opacity) { m_opacity = opacity; }
    float GetOpacity() const { return m_opacity; }

    // Number of spans painted in the most recent Update() — surfaced in the debugger UI so
    // "nothing is showing" can be told apart from "nothing was tagged".
    int GetLastSpanCount() const { return m_lastSpanCount; }

private:
    void EnsureTexture(int w, int h);

    // Applies the current isolation to m_pixels and uploads the result.
    void CompositeAndUpload();

    GLuint m_texId = 0;
    int m_texW = 0;
    int m_texH = 0;
    // The canonical, un-isolated overlay: alpha carries the decay level only. Isolation is
    // applied on the way to the texture so the selection can change without a repaint.
    std::vector<uint32_t> m_pixels; // RGBA8, m_texW * m_texH
    std::vector<uint32_t> m_idMap;  // owning blit id per pixel, parallel to m_pixels
    std::vector<uint32_t> m_isoBuf; // scratch for the isolated composite (empty while unused)
    std::vector<adm::AmigaEmulator::BlitVisSpan> m_spanBuf;

    // Blit ids painted by the frame just run, sorted for binary search. Rebuilt every Update()
    // from that frame's spans only -- not from m_idMap, which still holds decaying ids.
    std::vector<uint32_t> m_paintedIds;

    uint32_t m_isolatedBlitId = 0;
    int m_decayFrames = 5;
    float m_opacity = 0.65f;
    int m_lastSpanCount = 0;
    bool m_dirty = false;
};

} // namespace adm::ui
