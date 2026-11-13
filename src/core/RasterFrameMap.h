#pragma once

#include <cmath>
#include <cstdint>

#include "AmigaEmulator.h"
#include "AmigaScreenMap.h"
#include "Constants.h"

namespace adm::core {

// Maps the FULL Amiga raster — every colour clock of every scanline, including the horizontal
// and vertical blanking that never reaches the screen — onto the emulator display area.
//
// AmigaScreenMap answers "where in the texture is this Amiga pixel?". It cannot answer anything
// about blanking, because the framebuffer does not contain any: PuAE emits roughly 180 of the 227
// colour clocks and 287 of the 313 lines. This struct is the surrounding coordinate system. The
// texture becomes a sub-rectangle of the raster, and the offscreen clocks get real estate around
// it — which is what lets the copper/DMA overlay show slots that have no pixel to sit on.
//
// Adapted from the ENGINE9000 debugger by alpine9000
// (https://github.com/alpine9000/engine9000-public), GPL-2 — the same licence this project is
// distributed under (see COPYING). Specifically `e9k-debugger/emu_ami.c`
// (`emu_ami_adjustVideoDst` / `emu_ami_getDmaDebugOverlayDst`, which shrink and offset the video
// quad inside a larger overlay rect) and `ami9000/libretro/e9k_debug.c`
// (`e9k_debug_amiga_dma_debug_get_frame_view`, which derives that larger rect). See PATCHES.md.
//
// Pure — no ImGui, no OpenGL — so it is unit-testable.
struct RasterFrameMap
{
    // Raster extent for this frame. Taken from the DMA record rather than hardcoded, so NTSC
    // (262 lines) and long/short PAL fields are all handled.
    //
    // Columns are BEAM TIME: column H of row V is the colour clock Agnus spent at hpos H of line
    // V, and the picture is placed under the clocks that drew it. Denise trails the beam by
    // constants::beam::DeniseLagCCK clocks, so a texture column showing Denise clock D sits at
    // raster column D + 4. That is what puts a Copper MOVE COLOR00 exactly on the colour edge it
    // causes; placing the texture by Denise's own coordinate left every slot 4 clocks right of
    // its pixels.
    //
    // rasterCC is the drawn width and exceeds the 227-clock Agnus line. The displayed line
    // straddles hsync: its rightmost clocks are painted during the NEXT Agnus line (custom.c
    // denisehtotal = maxhpos + 7). libretro-uae right-aligns the frame buffer to that longer line,
    // so a wide-overscan PAL frame shows Denise clocks 54.5..234.5, which is beam columns
    // 58.5..238.5. The raster is widened to whatever the picture needs and CopperRaster fills the
    // columns past the line end from the next line - the clocks that really drew those pixels.
    int rasterCC = 0;    // colour clocks per line, drawn (>= dataCC)
    int rasterLines = 0; // scanlines per frame, drawn (>= the picture's last line)

    // Colour clocks in an Agnus line - where the displayed line wraps into the next one. 227
    // (constants::beam::CCKPerAgnusLine); CopperRaster adds the extra clock of a long NTSC line
    // per row. PuAE's record buffer is NR_DMA_REC_HPOS = 288 wide, and its bus slots stop here.
    int dataCC = 0;

    // Where the emulator texture sits inside that raster, in raster units. Fractional because
    // one colour clock is two lores pixels and a texture edge may land on an odd lores pixel.
    float imageLeftCC = 0.0f;
    float imageTopLine = 0.0f;
    float imageWidthCC = 0.0f;
    float imageHeightLines = 0.0f;

    bool valid = false;

    // Ratio of the raster's extent to the image's, per axis. EmulatorView multiplies the
    // aspect-fitted image size by these to size the whole raster rect, so the picture keeps its
    // aspect ratio and the blanking margins grow around it.
    float RasterToImageX() const
    {
        return (imageWidthCC > 0.0f) ? static_cast<float>(rasterCC) / imageWidthCC : 1.0f;
    }
    float RasterToImageY() const
    {
        return (imageHeightLines > 0.0f) ? static_cast<float>(rasterLines) / imageHeightLines
                                         : 1.0f;
    }

    // Normalised [0,1] position of a colour clock / scanline within the raster rect. The overlay
    // texture is one texel per colour clock, so these are also its UVs.
    float CCToU(float cc) const
    {
        return (rasterCC > 0) ? cc / static_cast<float>(rasterCC) : 0.0f;
    }
    float LineToV(float line) const
    {
        return (rasterLines > 0) ? line / static_cast<float>(rasterLines) : 0.0f;
    }

    // Normalised position of the image's corners within the raster rect — the inset that
    // EmulatorView applies to the video quad.
    float ImageU0() const { return CCToU(imageLeftCC); }
    float ImageU1() const { return CCToU(imageLeftCC + imageWidthCC); }
    float ImageV0() const { return LineToV(imageTopLine); }
    float ImageV1() const { return LineToV(imageTopLine + imageHeightLines); }

    // Inverse: a normalised position within the raster rect back to a beam position. Used to
    // hit-test a mouse position against a cell. Not clamped — callers check the range so they
    // can tell "outside the raster" from "on the first slot".
    int UToCC(float u) const { return static_cast<int>(u * static_cast<float>(rasterCC)); }
    int VToLine(float v) const { return static_cast<int>(v * static_cast<float>(rasterLines)); }

    // Builds from the same inputs every other emulator overlay already uses. `hposCount` and
    // `vposCount` come from AmigaEmulator::GetDmaRecord().
    static RasterFrameMap Build(const AmigaScreenMap& smap, int hposCount, int vposCount)
    {
        RasterFrameMap m;
        if (!smap.valid || hposCount <= 0 || vposCount <= 0)
            return m;
        if (smap.loresTexW <= 0.0f || smap.scanTexH <= 0.0f)
            return m;

        m.dataCC = (hposCount < kLineCC) ? hposCount : kLineCC;
        m.rasterCC = m.dataCC;
        m.rasterLines = (vposCount < kMaxLines) ? vposCount : kMaxLines;

        // One colour clock is two lores pixels, and the picture sits under the beam clocks that
        // drew it - Denise's lag later (see imageLeftCC above).
        m.imageLeftCC = static_cast<float>(smap.hbase) * 0.5f +
                        static_cast<float>(constants::beam::DeniseLagCCK);
        m.imageWidthCC = smap.loresTexW * 0.5f;
        m.imageTopLine = static_cast<float>(smap.vbase);
        m.imageHeightLines = smap.scanTexH;

        // Grow the raster so the picture is always inside it, on both axes. Horizontally the
        // frame buffer reaches past the end of the Agnus line (see rasterCC above). Vertically the
        // record can come out a line short of the picture - a short field, or the first frame
        // after a start - and a raster shorter than the image pushed the picture's last line out
        // of the bottom of the overlay and made the whole layout jump by a line.
        const int needCC = static_cast<int>(std::ceil(m.imageLeftCC + m.imageWidthCC));
        if (needCC > m.rasterCC)
            m.rasterCC = (needCC < kMaxRasterCC) ? needCC : kMaxRasterCC;
        const int needLines = static_cast<int>(std::ceil(m.imageTopLine + m.imageHeightLines));
        if (needLines > m.rasterLines)
            m.rasterLines = (needLines < kMaxLines) ? needLines : kMaxLines;

        m.valid = true;
        return m;
    }

    static RasterFrameMap Build(const AmigaEmulator::DisplayGeometry& geom, int texW, int texH,
                                int hposCount, int vposCount)
    {
        return Build(AmigaScreenMap::Build(geom, texW, texH), hposCount, vposCount);
    }

private:
    // Colour clocks in an Agnus line. Bus slots at or past this in the DMA record are never real
    // cycles on PAL; past 228 they are buffer padding (NR_DMA_REC_HPOS = 288).
    static constexpr int kLineCC = static_cast<int>(constants::beam::CCKPerAgnusLine);

    // Upper bound on the drawn width. The picture's right edge is the Denise line end, maxhpos +
    // 7 (+ half a clock on ECS), plus Denise's lag: 12 clocks of slack is the real requirement;
    // the rest is a backstop against a nonsense geometry report stretching the raster off the
    // window.
    static constexpr int kMaxRasterCC = kLineCC + 16;

    // Upper bound on scanlines. PuAE's record buffer is NR_DMA_REC_VPOS = 1000 rows tall, which
    // is nothing like a frame -- a PAL long frame is 313 lines and NTSC is 263. Taking the
    // buffer height at face value stretches the raster to three times the real frame and squashes
    // the picture accordingly, so it is clamped here as a backstop. Callers should pass the
    // frame's actual height (see copraster::DeriveFrameHeight) rather than rely on this.
    static constexpr int kMaxLines = 320;
};

} // namespace adm::core
