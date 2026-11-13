#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace adm::core {

/**
 * @brief Identifies named Amiga copy-protection track formats via libdisk.
 *
 * libdisk (keirf/disk-utilities) ships ~200 hand-written handlers, one per known
 * protection/loader. Detection is trial decoding: each handler's write_raw only
 * succeeds on the format it recognises. We feed it the per-track MFM we already
 * extract from capsimg using libdisk's in-memory "soft" stream, then probe every
 * handler (mirroring the disk-analyse tool) and return the first match.
 *
 * Available() is false when libdisk was not built (ADM_HAVE_LIBDISK undefined) or
 * the probe disk could not be created; Classify() then returns an empty string.
 * Header intentionally pulls in no libdisk headers (they define clashing macros);
 * the disk handle is held as an opaque pointer.
 */
class LibdiskClassifier
{
public:
    LibdiskClassifier();
    ~LibdiskClassifier();

    LibdiskClassifier(const LibdiskClassifier&) = delete;
    LibdiskClassifier& operator=(const LibdiskClassifier&) = delete;

    bool Available() const { return m_disk != nullptr; }

    /**
     * @brief Identify the format of one track from its raw MFM bitstream.
     * @param mfm MFM cell bytes (8 cells/byte, MSB first) — capsimg trackbuf.
     * @param lenBytes Length of the MFM buffer in bytes.
     * @param trackNumber Physical track number (cyl*2 + head).
     * @return Human-readable format name (e.g. "Copylock"), or "" if no match.
     */
    std::string Classify(const uint8_t* mfm, size_t lenBytes, uint32_t trackNumber);

private:
    void* m_disk = nullptr; // struct disk* (opaque)
    std::string m_tempPath;
};

} // namespace adm::core
