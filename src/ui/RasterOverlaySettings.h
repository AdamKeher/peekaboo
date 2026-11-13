#pragma once

#include <cstdint>

namespace adm::ui {

// User-facing settings for the Copper / DMA raster ("beam view") overlay.
//
// Lives in its own header because ownership is split: the controls are on DebuggerDisplayView,
// but Application owns the overlay itself (it owns the emulator the DMA record comes from) and
// DebuggerView passes the settings between them through a forward declaration.
//
// Adapted from the ENGINE9000 debugger by alpine9000 -- see core/CopperRaster.h and PATCHES.md.
struct RasterSettings
{
    bool enabled = false;
    bool showCopper = true;
    bool showWaitSpans = false; // dense: a WAIT can span most of a line
    bool showCopperWanted = true;
    bool showBlanking = true;
    bool showGeometryEvents = true;
    // Bit per DMARECORD_*; bit 0 (idle) unused. Zero = draw no DMA at all, which is the
    // default: a slot lands on nearly every colour clock and buries the picture underneath.
    uint16_t channelMask = 0;
    float opacity = 0.6f;
};

} // namespace adm::ui
