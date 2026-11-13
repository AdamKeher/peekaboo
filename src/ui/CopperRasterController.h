#pragma once

#include <vector>

#include <imgui.h>

#include "../core/AmigaEmulator.h"
#include "views/CopperRasterOverlay.h"

namespace adm::ui {

class DebuggerView;
class EmulatorView;

/**
 * @brief Drives the copper / DMA raster ("beam view") overlay on the emulator display.
 *
 * Owns the overlay itself, the frame extent it was last built at, and the cell a right-click
 * latched. Lives here rather than in the display window because the raster is fed from the
 * emulator's DMA record and copper trace, exactly like the blit provenance overlay - see
 * core/CopperRaster.h and PATCHES.md. Adapted from the ENGINE9000 debugger by alpine9000.
 *
 * Extracted from Application.
 */
class CopperRasterController
{
public:
    /// Both views are owned by Application and outlive this object; either may be null.
    void Init(EmulatorView* emulatorView, DebuggerView* debuggerView);

    // Applies the debugger toggle: arms the core's DMA recording and releases the overlay's
    // texture when it is switched off.
    void SetEnabled(bool enabled);

    // Rebuilds the raster for the frame just run and hands the emulator window the geometry it
    // needs to inset the picture. Called from the per-frame debugger block.
    void Update(AmigaEmulator* emu, const AmigaEmulator::DmaSlotRecord* records, int hposCount,
                int vposCount, const std::vector<AmigaEmulator::CopperExecEvent>& copperEvents);

    // Draws the raster plus its markers, and handles hover/click on a colour clock.
    void DrawOverlay(ImDrawList* dl, ImVec2 rasterMin, ImVec2 rasterMax, ImVec2 imgMin,
                     ImVec2 imgMax);

    /// Which blit the scrubber has selected / is hovering, mirrored onto the raster.
    void SetSelectedLRef(uint32_t lRef) { m_copperRaster.SetSelectedLRef(lRef); }
    void SetHighlightedLRef(uint32_t lRef) { m_copperRaster.SetHighlightedLRef(lRef); }

private:
    // The raster's tooltip for a colour clock. Copper cells defer to the Copper trace's own
    // disassembly so the two cannot describe the same instruction differently.
    void DrawTooltip(int cc, int line);

    // Right-click menu on a colour clock: arms a one-shot beam breakpoint through
    // BreakpointsView, so it appears in the Breakpoints window like any other.
    void DrawContextMenu();

    static constexpr const char* kRasterContextPopup = "##rasterctx";
    // PAL long frame; used until a DMA record arrives to say otherwise.
    static constexpr int kDefaultFrameLines = 313;

    EmulatorView* m_emulatorView = nullptr;
    DebuggerView* m_debuggerView = nullptr;

    CopperRasterOverlay m_copperRaster;

    // The cell the menu was opened on; the mouse has moved on by the time it is used.
    int m_rasterCtxLine = 0;
    int m_rasterCtxCC = 0;

    // Last raster extent seen, so the overlay keeps its geometry on frames where the DMA record
    // is unavailable (recording not yet armed) instead of collapsing to nothing.
    int m_rasterHposCount = 0;
    int m_rasterVposCount = 0;
};

} // namespace adm::ui
