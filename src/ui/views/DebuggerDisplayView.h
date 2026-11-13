#pragma once

#include <imgui.h>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "../../core/AmigaEmulator.h"
#include "../../core/DisplayBlocks.h"
#include "../../core/RasterFrameMap.h"
#include "../BlitOverlaySettings.h"
#include "../RasterOverlaySettings.h"

namespace adm::ui {

class EmulatorView;

class DebuggerDisplayView
{
public:
    explicit DebuggerDisplayView(EmulatorView* emulatorView);

    void Draw();
    bool* GetOpenPtr() { return &m_open; }
    void Restore(bool open);
    void DrawOverlays(ImDrawList* dl, ImVec2 screenMin, ImVec2 screenMax);
    bool IsGeometryOverlayVisible() const { return m_showGeometryOverlay; }

    // --- Blit overlay ------------------------------------------------------------------------
    // One feature, two renderers: the core-tagged provenance fill for everything the display
    // actually fetched, and a geometry-derived box (BlitScreenMapper) for the blits it did not --
    // a destination in a buffer that is never scanned out produces no spans at all. Application
    // owns the overlay and polls these, the same split as the raster settings below.
    using BlitSettings = adm::ui::BlitOverlaySettings;

    const BlitSettings& GetBlitSettings() const { return m_blit; }
    void SetBlitSettings(const BlitSettings& s) { m_blit = s; }
    bool IsBlitOverlayVisible() const { return m_blit.enabled; }

    // Live counts from the last overlay pass, pushed back by Application so the panel can tell
    // "nothing is showing" apart from "nothing was tagged" -- and can say how many blits had to
    // fall back to a box, which is the diagnostic for "my blits are going somewhere off-screen".
    // `unplacedBlits` are the ones neither layer could draw, so that pixels + boxes + unplaced
    // accounts for every blit in the frame (with Outline all off, which is what makes the sum
    // checkable against the Blitter Operations total).
    void SetBlitOverlayStats(int spans, int pixelBlits, int boxBlits, int unplacedBlits)
    {
        m_blitSpans = spans;
        m_blitPixelBlits = pixelBlits;
        m_blitBoxBlits = boxBlits;
        m_blitUnplacedBlits = unplacedBlits;
    }

    // Destination-data capture, mirrored from the emulator by Application::SyncBlitDestCapture so
    // the checkbox reflects what the core is actually doing across a core reload. The Blitter
    // Operations window keeps its own mirror for the Data column; this is the control.
    void SetCaptureDestData(bool on) { m_captureDestData = on; }
    void SetCaptureDestDataSupported(bool supported) { m_captureDestSupported = supported; }

    // Lock the crosshair to the given L-Ref position (called by scrubber / JumpToLRef)
    void SetCrosshairLRef(uint32_t lRef);

    // --- Copper / DMA raster overlay -------------------------------------------------------
    // The "beam view": the emulator picture is inset inside a rect standing for the whole
    // raster, and every colour clock is painted with what the bus did in it. Adapted from the
    // ENGINE9000 debugger by alpine9000 -- see core/CopperRaster.h and PATCHES.md.
    //
    // Only the settings live here. Application owns the overlay itself (as it does the blit
    // provenance one) because it owns the emulator the DMA record comes from, and polls these
    // each frame -- the same shape as the blit overlay above.
    using RasterSettings = adm::ui::RasterSettings;

    const RasterSettings& GetRasterSettings() const { return m_raster; }
    void SetRasterSettings(const RasterSettings& s) { m_raster = s; }
    bool IsRasterOverlayVisible() const { return m_raster.enabled; }

    // Fired when the raster overlay is toggled. Application uses it to arm the core's DMA
    // recording and to release the overlay texture when nobody is looking at it.
    std::function<void(bool)> m_onShowCopperRasterChanged;

    // Live cell counts from the last raster build, pushed back by Application so the panel can
    // tell "nothing is showing" apart from "nothing was recorded".
    void SetRasterStats(int copperCells, int dmaCells)
    {
        m_rasterCopperCells = copperCells;
        m_rasterDmaCells = dmaCells;
    }

    // --- Collapsible sections ----------------------------------------------------------------
    // Which sections of this panel are expanded, keyed by section title. Persisted in the
    // workspace rather than ImGui's own ini, so a saved workspace restores the panel exactly as
    // it was left. A section absent from the map defaults to expanded.
    std::map<std::string, bool> GetSectionStates() const
    {
        return {m_sections.begin(), m_sections.end()};
    }
    void SetSectionStates(const std::map<std::string, bool>& states)
    {
        m_sections.clear();
        m_sections.insert(states.begin(), states.end());
    }

    // The geometry the raster was laid out with, echoed back for the panel's diagnostic line.
    // Without it, "the picture is the wrong size" is unattributable to a number.
    void SetRasterGeometry(const adm::core::RasterFrameMap& map) { m_rasterMap = map; }

    // Fired when the user left-clicks the overlay to jump to an L-Ref
    std::function<void(uint32_t)> m_onJumpToLRef;

    // Fired when the user toggles the sprite bounding box overlay
    std::function<void(bool)> m_onShowSpriteBBoxesChanged;

    // Fired when any blit overlay setting changes. Application uses it to arm or release the
    // core-side provenance map — it costs a per-fetch lookup and ~50 MB of tables, so it only
    // runs while somebody is looking at it — and to push opacity/decay into the overlay.
    std::function<void(const BlitSettings&)> m_onBlitOverlayChanged;

    // Fired when the destination-capture checkbox is toggled — arms the core D-write ring.
    std::function<void(bool)> m_onCaptureDestDataChanged;

private:
    EmulatorView* m_emulatorView;
    bool m_open = false;
    bool m_noFocus = false;

    // Checkbox state, refreshed from AmigaEmulator::GetDisplayMasks() each frame (the emulator
    // owns the masks so the Sprites window, the AR console and MCP stay in sync with us).
    uint8_t m_bitplaneMask = 0xFF;
    uint8_t m_spriteMask = 0xFF;
    uint8_t m_playfieldMask = 0x3;

    // Chipset display options, refreshed from the emulator each frame for the same reason.
    bool m_paletteVis = false;
    bool m_bplcon1Scroll = true;
    adm::core::CopperBlock m_copperBlock;
    adm::core::BplPtrBlock m_bplPtrBlock;
    bool m_blitterWrites = true;

    bool m_showGeometryOverlay = false;
    bool m_showFrameCounterOverlay = false;
    bool m_showSpriteBBoxes = false;
    adm::AmigaEmulator::VideoResolution m_videoResolution =
        adm::AmigaEmulator::VideoResolution::Lores;

    // Transparent comparator: BeginSection looks headers up by const char* every frame, and
    // std::less<> avoids materialising a std::string key for each lookup.
    std::map<std::string, bool, std::less<>> m_sections;

    // Draws a section header and returns true when its body should be drawn. The open state is
    // driven from m_sections every frame, so the workspace is the single source of truth for it.
    bool BeginSection(const char* label);

    // Two equal columns that share the panel width. Fixed-width columns overlapped their
    // neighbours as soon as a label was wider than the column, which is any label at all once
    // the panel is narrowed.
    static bool BeginTwoColumns(const char* id);

    BlitSettings m_blit;
    int m_blitSpans = 0;
    int m_blitPixelBlits = 0;
    int m_blitBoxBlits = 0;
    int m_blitUnplacedBlits = 0;
    bool m_captureDestData = false;
    bool m_captureDestSupported = true;
    void DrawBlitterSection();

    RasterSettings m_raster;
    int m_rasterCopperCells = 0;
    int m_rasterDmaCells = 0;
    adm::core::RasterFrameMap m_rasterMap;
    void DrawRasterSection();
    void DrawBlocksSection();

    // Crosshair lock state
    bool m_crosshairLocked = false;
    int m_lockedHpix = 0;
    int m_lockedVscan = 0;

    void DrawContent();
    void PushDisplayMasks();
};

} // namespace adm::ui
