#pragma once

#include <cstdint>

// Maps a UAE memwatch access mask (the `accessmask` of the access that triggered a
// watchpoint hit) to a short agent name. Bit values mirror MW_MASK_* in the core's
// debug.h. Returns nullptr when the mask is 0 (older cores don't report it).
//
// This matters because a watchpoint stops at the next CPU instruction boundary, so the
// hit's `pc` is the CPU instruction that was running — which, for a Copper/Blitter/DMA
// write, is NOT the writer. Naming the real agent avoids blaming an unrelated MOVEM/LEA.
namespace adm::core {

inline const char* MemwatchSourceName(uint32_t m)
{
    if (m == 0)
        return nullptr;
    if (m & 0x00000001u)
        return "CPU (fetch)";
    if (m & 0x00000002u)
        return "CPU (read)";
    if (m & 0x00000004u)
        return "CPU (write)";
    if (m & 0x000001F8u)
        return "Blitter";
    if (m & 0x00000200u)
        return "Copper";
    if (m & 0x00000400u)
        return "Disk DMA";
    if (m & 0x00007800u)
        return "Audio DMA";
    if (m & 0x007F8000u)
        return "Bitplane DMA";
    if (m & 0x7F800000u)
        return "Sprite DMA";
    return nullptr;
}

} // namespace adm::core
