#pragma once

#include <cstdint>

namespace adm::ui {

// Friendly labels for the most common blitter configurations, matched on (BLTCON0, BLTCON1).
// Shared by the Blitter Operations table and the Frame Scrubber's per-blit flags.
struct BlitOpPreset
{
    const char* name;
    uint16_t bltcon0;
    uint16_t bltcon1;
};

inline constexpr BlitOpPreset kBlitOpPresets[] = {
    {"clear", 0x100, 0x0}, {"fill", 0x9f0, 0x12}, {"copy", 0x9f0, 0x0},
    {"mask", 0xdc0, 0x0},  {"bob", 0xfea, 0x0},   {"xfill", 0x9f0, 0x8},
};

} // namespace adm::ui
