#pragma once

#include <cstdint>
#include <string>

namespace adm::core {

/**
 * @brief Per-track preservation metadata recovered from an IPF image.
 *
 * Carried on DiskDocument and rendered by TrackView. Captures the
 * copy-protection picture that a plain ADF cannot represent: physical track
 * length, weak/fuzzy-bit zones, cell density type, and whether the track
 * decoded to a standard 11-sector AmigaDOS layout.
 */
struct IpfTrackInfo
{
    uint32_t cylinder = 0;
    uint32_t head = 0;
    uint32_t mfmLengthBits = 0; // physical track length (bits)
    uint32_t sectorCount = 0;   // standard AmigaDOS sectors successfully decoded
    uint32_t weakCount = 0;     // number of weak/fuzzy data areas (CapsTrackInfoT2.weakcnt)
    uint32_t trackType = 0;     // CAPS ctit* density classification
    bool isStandard = false;    // 11 checksum-valid sectors numbered 0..10

    // libdisk format identification (Phase 2). Empty when libdisk is unavailable
    // or the track matched no known handler.
    std::string formatName;    // e.g. "AmigaDOS", "Copylock", "RNC PDOS"
    bool isProtection = false; // matched a non-AmigaDOS copy-protection format
};

} // namespace adm::core
