#pragma once

#include <imgui.h>

namespace adm::ui {

// Returns a distinctive color for a memory region at a given index.
// Avoids red, which is used elsewhere to indicate unmapped/out-of-range memory.
inline ImU32 GetMemRegionColor(int regionIdx)
{
    static const ImU32 kColors[] = {
        IM_COL32(0, 200, 220, 255),   // cyan
        IM_COL32(80, 220, 110, 255),  // green
        IM_COL32(80, 140, 255, 255),  // blue
        IM_COL32(220, 220, 60, 255),  // yellow
        IM_COL32(255, 160, 50, 255),  // orange
        IM_COL32(190, 80, 230, 255),  // purple
        IM_COL32(50, 200, 255, 255),  // sky blue
        IM_COL32(255, 200, 50, 255),  // gold
        IM_COL32(70, 255, 170, 255),  // mint
        IM_COL32(210, 100, 255, 255), // violet
    };
    static constexpr int kCount = (int)(sizeof(kColors) / sizeof(kColors[0]));
    if (regionIdx < 0)
        return IM_COL32(100, 100, 100, 180);
    return kColors[regionIdx % kCount];
}

} // namespace adm::ui
