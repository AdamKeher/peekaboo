#pragma once

#include <cstdint>

namespace adm::core::be {

// The one set of big-endian byte readers. Nine hand-rolled copies of these existed across the
// disk/savestate/debug-info parsers — all of them decoding untrusted file bytes — and only
// drifted apart in style. Callers own the bounds check (or go through SafeReader, which is the
// bounds-checked reader built on the same layout).

[[nodiscard]] constexpr uint16_t U16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

[[nodiscard]] constexpr uint32_t U24(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

[[nodiscard]] constexpr uint32_t U32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

[[nodiscard]] constexpr int32_t S32(const uint8_t* p) { return static_cast<int32_t>(U32(p)); }

} // namespace adm::core::be
