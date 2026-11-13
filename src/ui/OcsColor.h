#pragma once

#include <cstdint>

#include <imgui.h>

namespace adm::ui {

// Expand a 12-bit OCS colour value ($0RGB, one nibble per channel) to an opaque ImU32. Each 4-bit
// channel is scaled by 17 so $F maps to $FF (a linear 0..255 ramp).
inline ImU32 OcsColorToImU32(uint16_t ocs)
{
    uint8_t r = static_cast<uint8_t>(((ocs >> 8) & 0xF) * 17);
    uint8_t g = static_cast<uint8_t>(((ocs >> 4) & 0xF) * 17);
    uint8_t b = static_cast<uint8_t>((ocs & 0xF) * 17);
    return IM_COL32(r, g, b, 255);
}

} // namespace adm::ui
