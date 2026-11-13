#pragma once
#include <imgui.h>

namespace adm::ui {

// Shared DMA channel colors used by DmaStatsView and DebuggerFrameScrubberView.
// These are modelled after the Engine 9000 debugger palette.
static constexpr ImU32 kDmaColorCopper = IM_COL32(255, 128, 0, 255);   // orange
static constexpr ImU32 kDmaColorBlitter = IM_COL32(200, 50, 50, 255);  // red
static constexpr ImU32 kDmaColorCpu = IM_COL32(80, 140, 220, 255);     // blue
static constexpr ImU32 kDmaColorBitplane = IM_COL32(80, 200, 80, 255); // green
static constexpr ImU32 kDmaColorSprite = IM_COL32(80, 200, 200, 255);  // cyan
static constexpr ImU32 kDmaColorDisk = IM_COL32(160, 80, 200, 255);    // purple
static constexpr ImU32 kDmaColorAudio = IM_COL32(220, 200, 60, 255);   // yellow
static constexpr ImU32 kDmaColorOther = IM_COL32(100, 100, 100, 255);  // grey
static constexpr ImU32 kDmaColorIdle = IM_COL32(40, 40, 40, 255);      // dark
static constexpr ImU32 kDmaColorRefresh = IM_COL32(80, 80, 80, 255);   // dim grey

// Map DMARECORD_* type to its colour.
inline ImU32 DmaTypeColor(int type)
{
    switch (type) {
        case 1:
            return kDmaColorRefresh; // DMARECORD_REFRESH
        case 2:
            return kDmaColorCpu; // DMARECORD_CPU
        case 3:
            return kDmaColorCopper; // DMARECORD_COPPER
        case 4:
            return kDmaColorAudio; // DMARECORD_AUDIO
        case 5:
            return kDmaColorBlitter; // DMARECORD_BLITTER
        case 6:
            return kDmaColorBitplane; // DMARECORD_BITPLANE
        case 7:
            return kDmaColorSprite; // DMARECORD_SPRITE
        case 8:
            return kDmaColorDisk; // DMARECORD_DISK
        case 9:
            return kDmaColorOther; // DMARECORD_CONFLICT
        default:
            return kDmaColorIdle;
    }
}

// Human-readable label for a DMARECORD_* type.
inline const char* DmaTypeLabel(int type)
{
    switch (type) {
        case 1:
            return "Refresh";
        case 2:
            return "CPU";
        case 3:
            return "Copper";
        case 4:
            return "Audio";
        case 5:
            return "Blitter";
        case 6:
            return "Bitplane";
        case 7:
            return "Sprite";
        case 8:
            return "Disk";
        case 9:
            return "Conflict";
        default:
            return "Idle";
    }
}

} // namespace adm::ui
