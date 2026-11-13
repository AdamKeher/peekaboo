#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "IpfTypes.h"

namespace adm::core {

/**
 * @brief Decodes a SuperCard Pro (.scp) flux image into mountable ADF bytes.
 *
 * Mirrors DmsConverter / IpfConverter / ExtAdfConverter: a single static entry
 * point that the shared decode pipeline (Application::ConvertToAdfBytes) calls.
 *
 * An SCP file stores raw magnetic flux-reversal timings, not decoded sectors —
 * several revolutions of every track, sampled at 25 ns resolution. To browse
 * one we have to do what a real drive's data separator does: run the flux
 * intervals through a phase-locked loop to recover the MFM cell bitstream, then
 * hand that stream to AmigaMfmDecoder to pull out standard AmigaDOS sectors.
 * The PLL here is a port of Keir Fraser's scp.c (the same one WinUAE and PuAE
 * use), so the host's view of a track matches what the emulated Paula sees.
 *
 * Tracks that yield no standard sectors (custom trackloaders, copy protection)
 * stay blank in the ADF and are reported as protected in the track metadata,
 * exactly as the extended-ADF path does. The emulator is handed the raw SCP
 * bytes instead of this decoded ADF, so those tracks still run — PuAE reads the
 * format natively via scp.c (magic detection).
 */
class ScpConverter
{
public:
    /**
     * @brief True if the buffer starts with the "SCP" magic.
     */
    static bool IsScp(const std::vector<uint8_t>& data);

    /**
     * @brief Decode SCP flux data into an in-memory ADF buffer.
     * @param scpData The raw .scp file content.
     * @param outTracks If non-null, receives per-track preservation metadata.
     * @return ADF bytes, or std::nullopt on a malformed/unsupported header.
     */
    static std::optional<std::vector<uint8_t>>
    ConvertToBuffer(const std::vector<uint8_t>& scpData,
                    std::vector<IpfTrackInfo>* outTracks = nullptr);

    /**
     * @brief Recover the MFM cell bitstream for one track, as the drive would.
     *
     * Exposed for tests and for callers that want the raw stream rather than
     * decoded sectors. `trackIndex` is the SCP track number (cylinder * 2 +
     * head). Returns an empty vector when the track is absent or malformed.
     *
     * @param revolutions How many stored revolutions to clock out back-to-back.
     *        Two is the useful default: a sector straddling the index hole is
     *        only ever whole in one of the two copies.
     */
    static std::vector<uint8_t> DecodeTrackMfm(const std::vector<uint8_t>& scpData, int trackIndex,
                                               int revolutions = 2);
};

} // namespace adm::core
