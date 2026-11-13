#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "AmigaEmulator.h"

namespace adm::core {

// Turns blitter-provenance spans into overlay pixels.
//
// The core hands us spans already in native output pixel coordinates — they were tagged at
// display-fetch time, so BPLxMOD, fetch mode, x-scroll delay, plane number, crop and scale
// have all been applied by the emulator's own display logic (see patches/adm_blitvis.c).
// There is therefore no coordinate mapping in here, which is the entire point of the
// technique: the fragile step that BlitScreenMapper has to guess at simply does not exist.
//
// What is left is the painting: clipping, per-blit colour, the dimming used to isolate a
// selected blit, and the decay that keeps a one-shot blit on screen for longer than the single
// frame it was fetched in.
//
// Painting keeps two parallel buffers: the RGBA pixels, and an id map holding the blit id that
// owns each pixel. Isolation is then a pass over the id map rather than something baked in at
// paint time, which is what lets the selection change while the emulator is paused (the pixels
// are only repainted when a frame is run). The id map doubles as the hit-test index for picking
// a blit off the screen.
//
// Pure — no ImGui, no OpenGL, no emulator — so it is unit-testable. BlitterVisOverlay is the
// thin GL wrapper around it.
namespace blitvis {

// Pixels are RGBA8 in the byte order OpenGL's GL_RGBA/GL_UNSIGNED_BYTE expects on a
// little-endian host, i.e. 0xAABBGGRR when read as a uint32_t.
inline uint32_t PackRGBA(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (a << 24) | (b << 16) | (g << 8) | r;
}

inline uint32_t AlphaOf(uint32_t pixel) { return pixel >> 24; }

// Alpha a freshly-tagged span is painted at.
inline constexpr uint32_t kFreshAlpha = 255;

// Alpha a span is dimmed to when a different blit is isolated. Non-zero on purpose: the
// selected blit reads better in the context of everything else the blitter did. Applied as a
// ceiling rather than an assignment so a pixel already faded below it keeps fading.
inline constexpr uint32_t kDimmedAlpha = 40;

inline constexpr int kMaxDecayFrames = 30;

// Stable per-blit colour, packed RGBA.
//
// Uses the same Knuth multiplicative hash as the upstream ENGINE9000 front end so a blit keeps
// one colour for as long as it is on screen. Only the high bits of that hash are well mixed,
// hence the shift. Mapped through HSV at fixed saturation and value so consecutive blit ids —
// which is exactly what the core hands out — stay far apart in hue, and nothing comes out
// near-black against the picture.
inline uint32_t ColorForBlitId(uint32_t blitId)
{
    if (blitId == 0)
        return 0; // 0 means "no blit"; fully transparent

    const uint32_t hash = blitId * 2654435761u;
    const float h = static_cast<float>((hash >> 8) & 0xFFFF) / 65536.0f * 6.0f;
    constexpr float s = 0.85f;
    constexpr float v = 1.0f;

    const int sector = static_cast<int>(h) % 6;
    const float f = h - static_cast<float>(static_cast<int>(h));
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));

    float rf = 0.0f, gf = 0.0f, bf = 0.0f;
    switch (sector) {
        case 0:
            rf = v;
            gf = t;
            bf = p;
            break;
        case 1:
            rf = q;
            gf = v;
            bf = p;
            break;
        case 2:
            rf = p;
            gf = v;
            bf = t;
            break;
        case 3:
            rf = p;
            gf = q;
            bf = v;
            break;
        case 4:
            rf = t;
            gf = p;
            bf = v;
            break;
        default:
            rf = v;
            gf = p;
            bf = q;
            break;
    }

    const auto to8 = [](float c) {
        return static_cast<uint32_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return PackRGBA(to8(rf), to8(gf), to8(bf), kFreshAlpha);
}

// Fades every non-transparent pixel by one frame's worth of alpha. Returns true while anything
// is still visible, which the caller uses to decide whether the texture needs re-uploading.
//
// When an id map is supplied, a pixel that fades all the way out drops its blit id too, so the
// map never claims ownership of a pixel that is no longer showing anything.
inline bool DecayPixels(std::vector<uint32_t>& pixels, int decayFrames,
                        std::vector<uint32_t>* idMap = nullptr)
{
    const int frames = std::clamp(decayFrames, 1, kMaxDecayFrames);
    const uint32_t step = std::max(1u, 255u / static_cast<uint32_t>(frames));
    const bool haveIds = idMap && idMap->size() == pixels.size();
    bool anyVisible = false;
    for (size_t i = 0; i < pixels.size(); ++i) {
        uint32_t& px = pixels[i];
        const uint32_t a = AlphaOf(px);
        if (a == 0)
            continue;
        const uint32_t na = (a > step) ? (a - step) : 0u;
        px = (px & 0x00FFFFFFu) | (na << 24);
        if (na != 0)
            anyVisible = true;
        else if (haveIds)
            (*idMap)[i] = 0;
    }
    return anyVisible;
}

// Paints spans into an RGBA8 buffer of w*h pixels, clipping to those bounds.
//
// `x` and `xEnd` are inclusive. Where two blits wrote the same pixels in one frame the later
// span wins, which matches "what is on screen came from this blit".
//
// Everything is painted at full strength; isolation is ApplyIsolation's job. When an id map of
// the same size is supplied, each painted pixel also records its blit id there.
inline void PaintSpans(std::vector<uint32_t>& pixels, int w, int h,
                       const AmigaEmulator::BlitVisSpan* spans, int count,
                       std::vector<uint32_t>* idMap = nullptr)
{
    if (!spans || w <= 0 || h <= 0)
        return;
    if (pixels.size() < static_cast<size_t>(w) * static_cast<size_t>(h))
        return;
    const bool haveIds = idMap && idMap->size() == pixels.size();

    for (int i = 0; i < count; ++i) {
        const AmigaEmulator::BlitVisSpan& s = spans[i];
        if (s.blitId == 0)
            continue;

        const int y = static_cast<int>(s.y);
        if (y < 0 || y >= h)
            continue;

        int x0 = static_cast<int>(s.x);
        int x1 = static_cast<int>(s.xEnd);
        if (x1 < x0)
            continue; // malformed: xEnd is inclusive, so this is never a legitimate empty run
        x0 = std::max(x0, 0);
        x1 = std::min(x1, w - 1);
        if (x1 < x0)
            continue; // entirely past the right edge

        const uint32_t packed = (ColorForBlitId(s.blitId) & 0x00FFFFFFu) | (kFreshAlpha << 24);

        const size_t base = static_cast<size_t>(y) * static_cast<size_t>(w);
        uint32_t* row = pixels.data() + base;
        for (int x = x0; x <= x1; ++x)
            row[x] = packed;
        if (haveIds) {
            uint32_t* idRow = idMap->data() + base;
            for (int x = x0; x <= x1; ++x)
                idRow[x] = s.blitId;
        }
    }
}

// Produces the isolated view of `pixels` into `out`: pixels belonging to isolatedBlitId keep
// their alpha, every other pixel is capped at kDimmedAlpha.
//
// Kept separate from PaintSpans so changing the selection does not need a repaint — which is
// the whole point, since the pixels are only repainted when the emulator runs a frame and the
// selection is usually made while it is paused. A zero id means "isolate nothing" and copies
// straight through.
inline void ApplyIsolation(const std::vector<uint32_t>& pixels, const std::vector<uint32_t>& idMap,
                           std::vector<uint32_t>& out, uint32_t isolatedBlitId)
{
    out.resize(pixels.size());
    if (isolatedBlitId == 0 || idMap.size() != pixels.size()) {
        out = pixels;
        return;
    }

    for (size_t i = 0; i < pixels.size(); ++i) {
        const uint32_t px = pixels[i];
        const uint32_t a = AlphaOf(px);
        if (a == 0 || idMap[i] == isolatedBlitId) {
            out[i] = px;
            continue;
        }
        out[i] = (px & 0x00FFFFFFu) | (std::min(a, kDimmedAlpha) << 24);
    }
}

// The set of blits these spans painted, sorted and deduplicated, ready for binary search.
// Id 0 means "no blit" and is skipped.
//
// This is what lets the overlay say which blits it could NOT show: a blit in the frame whose id
// is absent here was never fetched for display, so the geometry fallback draws it instead.
inline void CollectPaintedIds(const AmigaEmulator::BlitVisSpan* spans, int count,
                              std::vector<uint32_t>& out)
{
    out.clear();
    if (!spans || count <= 0)
        return;

    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        if (spans[i].blitId != 0)
            out.push_back(spans[i].blitId);

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

// The blit that owns the pixel at (x, y), or 0 for "no blit here" — out of bounds, never
// tagged, or faded out. This is how a click on the overlay resolves back to a blit record.
inline uint32_t BlitIdAt(const std::vector<uint32_t>& idMap, int w, int h, int x, int y)
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return 0;
    const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
    return (i < idMap.size()) ? idMap[i] : 0u;
}

} // namespace blitvis
} // namespace adm::core
