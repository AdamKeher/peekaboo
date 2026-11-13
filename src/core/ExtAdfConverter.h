#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "IpfTypes.h"

namespace adm::core {

/**
 * @brief Decodes a UAE-1ADF "extended ADF" image into mountable ADF bytes.
 *
 * Mirrors DmsConverter / IpfConverter: a single static entry point that the
 * shared decode pipeline (Application::ConvertToAdfBytes) calls. An extended
 * ADF stores per-track data of arbitrary length so it can represent long/short
 * tracks and copy protection that a flat 901120-byte ADF cannot.
 *
 * Each track is stored either as already-decoded AmigaDOS sectors (type 0,
 * copied straight in) or as a raw MFM bitstream (type >= 1, run through
 * AmigaMfmDecoder to recover standard sectors). Non-standard / protected tracks
 * are left blank in the ADF (the volume then mounts as NDOS) but still reported
 * in the track metadata so the Track Map view can show the preservation picture.
 *
 * The emulator is handed the raw UAE-1ADF bytes instead of this decoded ADF, so
 * copy protection survives — PuAE reads the format natively (magic detection).
 */
class ExtAdfConverter
{
public:
    /**
     * @brief True if the buffer starts with the "UAE-1ADF" magic.
     */
    static bool IsExtAdf(const std::vector<uint8_t>& data);

    /**
     * @brief Decode UAE-1ADF data into an in-memory ADF buffer.
     * @param extData The raw extended-ADF file content.
     * @param outTracks If non-null, receives per-track preservation metadata.
     * @return ADF bytes, or std::nullopt on a malformed header.
     */
    static std::optional<std::vector<uint8_t>>
    ConvertToBuffer(const std::vector<uint8_t>& extData,
                    std::vector<IpfTrackInfo>* outTracks = nullptr);
};

} // namespace adm::core
