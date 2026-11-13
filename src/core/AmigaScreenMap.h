#pragma once

#include "AmigaEmulator.h"

namespace adm::core {

// Maps Amiga hardware coordinates (lores pixels horizontally, scanlines vertically) to
// normalized [0,1] coordinates within the displayed emulator texture, accounting for the
// frame's resolution and the cropping/offsets baked into the UAE frame buffer.
//
// This is the single source of truth shared by every emulator-display overlay (DIW/DDF
// geometry, sprites, blit destination) so they all align pixel-for-pixel. Callers convert
// the normalized result to ImGui screen space with the image's min/max corners.
struct AmigaScreenMap
{
    float loresScale = 1.0f; // 4 for super-hires frames, 2 for hires, 1 for lores
    float vScale = 1.0f;     // 2 for doubled/interlaced frames, 1 otherwise
    int hbase = 0;           // Amiga lores pixel that maps to the texture's left edge
    int vbase = 0;           // Amiga scanline that maps to the texture's top edge
    float loresTexW = 0.0f;  // texture width in lores pixels
    float scanTexH = 0.0f;   // texture height in scanlines
    bool valid = false;

    float ToU(float hpixLores) const
    {
        return (loresTexW > 0.0f) ? (hpixLores - static_cast<float>(hbase)) / loresTexW : 0.0f;
    }
    float ToV(float vscan) const
    {
        return (scanTexH > 0.0f) ? (vscan - static_cast<float>(vbase)) / scanTexH : 0.0f;
    }

    static AmigaScreenMap Build(const AmigaEmulator::DisplayGeometry& geom, int texW, int texH)
    {
        AmigaScreenMap m;
        if (texW <= 0 || texH <= 0)
            return m;

        // loresScale: 4 for super-hires frames, 2 for hires, 1 for lores. Use the full native
        // width (geom.fullW) so detection is correct even when the visible output is cropped
        // below the threshold. PAL widths are ~360 / ~720 / ~1440 for the three resolutions
        // (puae_video_resolution = lores / hires / superhires).
        m.loresScale = (geom.fullW > 800) ? 4.0f : ((geom.fullW > 400) ? 2.0f : 1.0f);
        m.vScale = (geom.fullH > 300) ? 2.0f : 1.0f;
        m.loresTexW = static_cast<float>(texW) / m.loresScale;
        m.scanTexH = static_cast<float>(texH) / m.vScale;

        // hbase/vbase: the Amiga coordinate that maps to the texture's top-left corner.
        // cropX/cropY are in native (hires) pixels, so divide by scale.
        m.hbase = static_cast<int>((geom.cropX + geom.visibleLeftBorder) / m.loresScale);
        m.vbase = geom.frameYAdjust + static_cast<int>(geom.cropY / m.vScale);
        m.valid = true;
        return m;
    }
};

} // namespace adm::core
