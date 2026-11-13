#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "IpfTypes.h"

namespace adm::core {

/**
 * @brief Decodes an IPF disk image into mountable ADF bytes + track metadata.
 *
 * Mirrors DmsConverter: a single static entry point that the shared decode
 * pipeline (Application::ConvertToAdfBytes) calls. Internally it drives
 * CapsImageLoader (capsimg) for the per-track MFM and AmigaMfmDecoder to turn
 * standard AmigaDOS tracks into 512-byte sectors, assembling them into an
 * 880KB ADF. Non-standard / protected tracks are left blank in the ADF (the
 * volume then mounts as NDOS) but still reported in the track metadata so the
 * Track Viewer can show the preservation picture.
 */
class IpfConverter
{
public:
    /**
     * @brief Decode IPF data into an in-memory ADF buffer.
     * @param ipfData The raw .ipf file content.
     * @param outTracks If non-null, receives per-track preservation metadata.
     * @return ADF bytes, or std::nullopt on failure (capsimg missing/decode error).
     */
    static std::optional<std::vector<uint8_t>>
    ConvertToBuffer(const std::vector<uint8_t>& ipfData,
                    std::vector<IpfTrackInfo>* outTracks = nullptr);
};

} // namespace adm::core
