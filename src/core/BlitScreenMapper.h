#pragma once

#include <algorithm>

#include "AmigaScreenMap.h"
#include "BitplaneLayout.h"
#include "BlitProfiler.h"

namespace adm::core {

enum class BlitBufferKind {
    None,  // destination is not a visible bitplane buffer
    Front, // destination is one of the on-screen bitplane buffers
    Back   // destination aligns to an off-screen double/triple buffer of the same geometry
};

// Result of mapping a blit's destination onto the emulator display, in normalized [0,1]
// texture coordinates (u=horizontal, v=vertical). Convert to screen space with the image
// corners: screen = min + uv * (max - min).
struct BlitOverlayBox
{
    bool valid = false;
    BlitBufferKind kind = BlitBufferKind::None;
    int plane = -1; // bitplane index the destination fell within
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

// Where a blit's destination falls within a bitplane's buffer, in plane-local terms:
// `plane` selects the buffer, (colByte,row) is the top-left of the written block relative
// to that buffer's start. Resolution/border conversions are applied by the callers that
// turn this into screen- or tile-space coordinates.
struct BlitDestPlacement
{
    bool found = false;
    BlitBufferKind kind = BlitBufferKind::None;
    int plane = -1;
    int colByte = 0; // byte column within the plane row where the destination starts
    int row = 0;     // row within the plane buffer where the destination starts
};

namespace detail {

// How many buffer-spans away from an on-screen plane we still consider a plausible
// off-screen buffer of the same geometry. Bounds the back-buffer search so an arbitrary
// high address can't be made to "match" some distant row.
inline constexpr int kMaxBackBufferSpans = 8;

// Descending-mode blits point bltdpt at the END of the destination block; normalize to the
// lowest address so the geometry below is buffer-start-relative.
inline uint32_t NormalizeDestStart(const BlitRecord& rec)
{
    uint32_t startAddr = rec.raw.bltdpt;
    if (rec.raw.bltcon1 & 0x0002) {
        const uint32_t rowStep = static_cast<uint32_t>(rec.wordsPerRow) * 2u + rec.raw.bltdmod;
        startAddr -= static_cast<uint32_t>(rec.rows - 1) * rowStep;
        startAddr -= static_cast<uint32_t>(rec.wordsPerRow) * 2u - 2u;
    }
    return startAddr;
}

// Attempts to place startAddr within a plane whose buffer begins at `base`. On success fills
// the placement's plane/row/colByte (kind/found left to the caller) and returns true.
inline bool PlaceInBuffer(uint32_t startAddr, uint32_t base, int plane,
                          const BitplaneLayout& layout, BlitDestPlacement& out)
{
    const int stride = layout.StrideBytes(plane);
    const int bytesPerRow = layout.screenWidth / 8;
    if (stride <= 0 || bytesPerRow <= 0 || layout.screenHeight <= 0)
        return false;

    const uint32_t span =
        static_cast<uint32_t>(stride) * static_cast<uint32_t>(layout.screenHeight);
    if (startAddr < base || startAddr >= base + span)
        return false;

    const uint32_t offset = startAddr - base;
    const int row = static_cast<int>(offset / static_cast<uint32_t>(stride));
    const int colByte = static_cast<int>(offset % static_cast<uint32_t>(stride));
    if (colByte >= bytesPerRow)
        return false; // lands in the modulo gap — not a visible column

    out.plane = plane;
    out.row = row;
    out.colByte = colByte;
    return true;
}

// Locates a blit's destination within the bitplane buffers. Pass 1 looks for an on-screen
// plane (Front); pass 2 for a contiguous off-screen buffer of identical geometry (Back —
// the common double/triple-buffer case, which can't otherwise be located while paused).
inline BlitDestPlacement FindDestPlacement(const BlitRecord& rec, const BitplaneLayout& layout)
{
    BlitDestPlacement out;
    if (!layout.valid || layout.bplCount <= 0)
        return out;
    if (rec.rows == 0 || rec.wordsPerRow == 0 || !rec.useD)
        return out;

    const uint32_t startAddr = NormalizeDestStart(rec);
    const int planes = std::min(layout.bplCount, BitplaneLayout::kMaxPlanes);

    // Pass 1: an on-screen plane (front buffer).
    for (int p = 0; p < planes; ++p) {
        if (layout.bplBase[p] == 0)
            continue;
        if (PlaceInBuffer(startAddr, layout.bplBase[p], p, layout, out)) {
            out.kind = BlitBufferKind::Front;
            out.found = true;
            return out;
        }
    }

    // Pass 2: a contiguous off-screen buffer of identical geometry. Only an exact buffer-span
    // multiple from a real plane base, landing in a real column, qualifies — which makes false
    // positives unlikely.
    for (int p = 0; p < planes; ++p) {
        if (layout.bplBase[p] == 0)
            continue;
        const int stride = layout.StrideBytes(p);
        if (stride <= 0 || layout.screenHeight <= 0)
            continue;
        const uint32_t span =
            static_cast<uint32_t>(stride) * static_cast<uint32_t>(layout.screenHeight);
        for (int k = 1; k <= kMaxBackBufferSpans; ++k) {
            const uint32_t up = layout.bplBase[p] + static_cast<uint32_t>(k) * span;
            const bool hasDown = layout.bplBase[p] >= static_cast<uint32_t>(k) * span;
            const uint32_t down = hasDown ? layout.bplBase[p] - static_cast<uint32_t>(k) * span : 0;
            if (PlaceInBuffer(startAddr, up, p, layout, out) ||
                (hasDown && PlaceInBuffer(startAddr, down, p, layout, out))) {
                out.kind = BlitBufferKind::Back;
                out.found = true;
                return out;
            }
        }
    }

    return out;
}

} // namespace detail

// Computes where a blit's destination rectangle lands on the displayed frame.
//
// Handles descending-mode normalization and resolution scaling. A destination that matches
// an on-screen plane is classified Front; one that aligns to a contiguous off-screen buffer
// of the same geometry is classified Back. No match returns {valid=false}.
//
// Pure function — no ImGui, no emulator access — so it is unit-testable.
inline BlitOverlayBox MapBlitDestToScreen(const BlitRecord& rec, const BitplaneLayout& layout,
                                          const AmigaScreenMap& map)
{
    BlitOverlayBox box;
    if (!map.valid)
        return box;

    const BlitDestPlacement p = detail::FindDestPlacement(rec, layout);
    if (!p.found)
        return box;

    // Plane-relative position → Amiga lores pixel / scanline. A byte spans 8 bitplane pixels;
    // convert those to the lores units the screen map expects using the BITPLANE's own
    // resolution (a hires bitplane pixel is half a lores pixel, a super-hires one a quarter).
    // This must not use map.loresScale: that reflects the texture's resolution, which differs
    // from the bitplane's when a lores screen is rendered into a hires-width texture — using it
    // would halve the box.
    const float loresPerBit = 1.0f / static_cast<float>(layout.PixelsPerLores());
    const float ax = static_cast<float>(p.colByte * 8) * loresPerBit;
    const float aw = static_cast<float>(rec.wordsPerRow * 16) * loresPerBit;
    const float hpix = static_cast<float>(layout.ddfStartLoresPx) + ax;
    const float vscan = static_cast<float>(layout.diwVStart + p.row);

    box.plane = p.plane;
    box.kind = p.kind;
    box.u0 = map.ToU(hpix);
    box.u1 = map.ToU(hpix + aw);
    box.v0 = map.ToV(vscan);
    box.v1 = map.ToV(vscan + static_cast<float>(rec.rows));
    box.valid = true;
    return box;
}

// Plane-local destination box: where the blit wrote within a single bitplane buffer, in
// normalized [0,1] coordinates relative to that plane's texture (which spans the visible
// screenWidth × screenHeight). Use `plane` to draw on the matching plane tile (and, since
// the geometry is identical, the composite tile). Only Front placements are returned valid —
// a plane/composite tile renders the front buffer, so a Back-buffer write doesn't belong on it.
//
// Pure function — unit-testable.
inline BlitOverlayBox MapBlitDestToPlane(const BlitRecord& rec, const BitplaneLayout& layout)
{
    BlitOverlayBox box;
    const BlitDestPlacement p = detail::FindDestPlacement(rec, layout);
    if (!p.found || p.kind != BlitBufferKind::Front)
        return box;
    if (layout.screenWidth <= 0 || layout.screenHeight <= 0)
        return box;

    const float x0 = static_cast<float>(p.colByte * 8);
    const float x1 = x0 + static_cast<float>(rec.wordsPerRow * 16);
    const float y0 = static_cast<float>(p.row);
    const float y1 = y0 + static_cast<float>(rec.rows);

    box.plane = p.plane;
    box.kind = p.kind;
    box.u0 = x0 / static_cast<float>(layout.screenWidth);
    box.u1 = x1 / static_cast<float>(layout.screenWidth);
    box.v0 = y0 / static_cast<float>(layout.screenHeight);
    box.v1 = y1 / static_cast<float>(layout.screenHeight);
    box.valid = true;
    return box;
}

// Index of the smallest box containing the normalized point (u,v), or -1 when none does.
//
// Smallest wins because blits nest: a full-screen clear and the sprite blitted on top of it both
// contain the same point, and the one worth selecting is almost always the small one. Invalid
// boxes are skipped, and edges are inclusive so a click on a one-pixel-tall box still lands.
//
// Pure function -- no ImGui -- so it is unit-testable.
inline int PickSmallestBoxContaining(const BlitOverlayBox* boxes, int count, float u, float v)
{
    int best = -1;
    float bestArea = 0.0f;

    for (int i = 0; i < count; ++i) {
        const BlitOverlayBox& b = boxes[i];
        if (!b.valid)
            continue;

        // Normalize: MapBlitDestToScreen always emits u0<=u1, but a caller-built box need not.
        const float x0 = std::min(b.u0, b.u1);
        const float x1 = std::max(b.u0, b.u1);
        const float y0 = std::min(b.v0, b.v1);
        const float y1 = std::max(b.v0, b.v1);
        if (u < x0 || u > x1 || v < y0 || v > y1)
            continue;

        const float area = (x1 - x0) * (y1 - y0);
        if (best < 0 || area < bestArea) {
            best = i;
            bestArea = area;
        }
    }

    return best;
}

} // namespace adm::core
