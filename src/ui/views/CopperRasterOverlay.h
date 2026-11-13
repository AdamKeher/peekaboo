#pragma once

#include <imgui.h>

#include <cstdint>

#include "../../core/AmigaEmulator.h"
#include "../../core/CopperRaster.h"
#include "../../core/RasterFrameMap.h"

typedef unsigned int GLuint;

namespace adm::ui {

// Paints the Copper / DMA raster over the emulator display area.
//
// The rasterisation itself lives in adm::core::copraster, which is pure and unit-tested. This
// class is only the GL and ImGui side: keep a texture one texel per colour clock, upload it, draw
// it over the raster rect, and hit-test a mouse position back to the cell under it.
//
// Adapted from the ENGINE9000 debugger by alpine9000
// (https://github.com/alpine9000/engine9000-public), GPL-2 — the same licence this project is
// distributed under (see COPYING). See PATCHES.md and core/CopperRaster.h.
class CopperRasterOverlay
{
public:
    ~CopperRasterOverlay();

    // Rebuilds the overlay texture for the frame just run. `map` fixes the raster extent, so it
    // must be the same one EmulatorView laid the display out with.
    void Update(const adm::core::RasterFrameMap& map, const adm::core::copraster::Input& in);

    // Draws the raster over [rasterMin, rasterMax] — the enlarged rect that includes the
    // offscreen colour clocks, not the emulator image rect.
    void Draw(ImDrawList* dl, ImVec2 rasterMin, ImVec2 rasterMax) const;

    // Draws the selection / hover / beam markers on top of the raster. Kept separate from Draw()
    // so they are never baked into the texture and can change without a re-raster.
    void DrawMarkers(ImDrawList* dl, ImVec2 rasterMin, ImVec2 rasterMax) const;

    // Frees the texture and pixel buffers. Call when the overlay is switched off so it is not
    // holding a raster-sized buffer for a feature nobody is looking at.
    void Reset();

    // The cell under a screen position, or nullptr when outside the raster. `outCC`/`outLine`
    // receive the beam position even when the cell is empty, so a click on blanking still
    // resolves to an L-Ref.
    const adm::core::copraster::Cell* CellAt(ImVec2 pos, ImVec2 rasterMin, ImVec2 rasterMax,
                                             int& outCC, int& outLine) const;

    adm::core::copraster::Options& MutableOptions() { return m_options; }
    const adm::core::copraster::Options& GetOptions() const { return m_options; }

    adm::core::copraster::Palette& MutablePalette() { return m_palette; }
    const adm::core::copraster::Palette& GetPalette() const { return m_palette; }

    void SetOpacity(float opacity) { m_opacity = opacity; }
    float GetOpacity() const { return m_opacity; }

    // Highlighted (hovered elsewhere) and selected colour clocks, as L-Refs. UINT32_MAX = none.
    void SetSelectedLRef(uint32_t lRef) { m_selectedLRef = lRef; }
    uint32_t GetSelectedLRef() const { return m_selectedLRef; }
    void SetHighlightedLRef(uint32_t lRef) { m_highlightedLRef = lRef; }

    // Frame-scrubber playhead, drawn as a beam marker that sweeps the raster as it is dragged.
    void SetPlayheadLRef(uint32_t lRef) { m_playheadLRef = lRef; }

    // Counts from the most recent Update(), surfaced in the UI so "nothing is showing" can be
    // told apart from "nothing was recorded".
    int GetCopperCellCount() const { return m_result.copperCells; }
    int GetDmaCellCount() const { return m_result.dmaCells; }
    bool HasData() const { return m_result.width > 0 && m_result.height > 0; }

    const adm::core::copraster::Result& GetResult() const { return m_result; }

private:
    void EnsureTexture(int w, int h);
    void UploadTexture();

    // Screen rect of one colour clock within the raster rect.
    void CellRect(int cc, int line, ImVec2 rasterMin, ImVec2 rasterMax, ImVec2& outMin,
                  ImVec2& outMax) const;

    GLuint m_texId = 0;
    int m_texW = 0;
    int m_texH = 0;

    adm::core::copraster::Result m_result;
    adm::core::copraster::Options m_options;
    adm::core::copraster::Palette m_palette = adm::core::copraster::Palette::Default();

    float m_opacity = 0.85f;
    uint32_t m_selectedLRef = UINT32_MAX;
    uint32_t m_highlightedLRef = UINT32_MAX;
    uint32_t m_playheadLRef = UINT32_MAX;
};

} // namespace adm::ui
