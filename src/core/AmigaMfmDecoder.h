#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace adm::core {

/**
 * @brief Decodes standard AmigaDOS sectors out of a raw MFM track bitstream.
 *
 * IPF tracks (via capsimg) are delivered as MFM cell data, not sectors. For a
 * standard AmigaDOS DD track this contains 11 sectors, each introduced by the
 * `0x4489 0x4489` sync mark and stored in the trackdisk.device odd/even MFM
 * layout. This decoder scans the bitstream for sync marks, decodes each sector
 * header + data, and verifies the Amiga checksums.
 *
 * Tracks that do not yield 11 checksum-valid sectors (copy-protected /
 * non-standard formats) are reported as non-standard with however many sectors
 * were recovered; the caller leaves the corresponding ADF region blank.
 */
class AmigaMfmDecoder
{
public:
    static constexpr int kSectorSize = 512;
    static constexpr int kSectorsPerTrack = 11;

    struct Sector
    {
        uint8_t trackNumber = 0;  // cyl * 2 + head, from the sector header
        uint8_t sectorNumber = 0; // 0..10
        bool checksumOk = false;  // header + data checksums both verified
        std::array<uint8_t, kSectorSize> data{};

        // Everything below describes the sector as it sits on the surface, for the
        // inspector panel. The decode needs all of it anyway; it used to be thrown
        // away once the checksums had been compared.
        uint8_t format = 0;              // info[0] - 0xFF on every AmigaDOS sector
        uint8_t sectorsToGap = 0;        // info[3] - sectors remaining before the track gap
        std::array<uint8_t, 16> label{}; // OS recovery label, zero on a normal disk

        uint32_t headerChecksum = 0;     // as stored on the surface
        uint32_t headerChecksumCalc = 0; // as recomputed from the header fields
        uint32_t dataChecksum = 0;
        uint32_t dataChecksumCalc = 0;
        bool headerChecksumOk = false;
        bool dataChecksumOk = false;

        // Bit ("cell") offsets into the track buffer this sector was decoded from.
        // syncCell is the first cell of the 0x4489 0x4489 mark, so
        // cellCount == endCell - syncCell covers sync + header + data.
        size_t syncCell = 0;
        size_t dataStartCell = 0; // first cell of the 512-byte data field
        size_t endCell = 0;       // one past the last data cell
        size_t cellCount = 0;
    };

    struct Track
    {
        std::vector<Sector> sectors;
        bool isStandard = false; // 11 checksum-valid sectors numbered 0..10
    };

    /**
     * @brief Decode all standard Amiga sectors found in an MFM bitstream.
     * @param mfm Pointer to the MFM track buffer (8 cells per byte, MSB first).
     * @param mfmLenBytes Length of the MFM buffer in bytes.
     */
    static Track DecodeTrack(const uint8_t* mfm, size_t mfmLenBytes);

    /**
     * @brief Encode a standard 11-sector AmigaDOS track to an MFM buffer.
     *
     * The clock bits are left zero (the decoder masks them out and the Amiga
     * checksum is computed over the encoded longs either way), which keeps the
     * encode/decode round-trip deterministic for unit tests and lets callers
     * synthesise standard tracks. Not used by the IPF load path.
     *
     * @param trackNumber cyl * 2 + head value to stamp in each sector header.
     * @param sectorData Exactly kSectorsPerTrack sectors of kSectorSize bytes.
     * @param leadingGapBits Extra zero bits prepended before the first sync, to
     *        exercise non-byte-aligned decoding.
     */
    static std::vector<uint8_t>
    EncodeStandardTrack(uint8_t trackNumber,
                        const std::vector<std::array<uint8_t, kSectorSize>>& sectorData,
                        uint32_t leadingGapBits = 0);

    /// Bytes one sector occupies on the surface: gap, sync, header, checksums, data.
    static constexpr size_t kSectorMfmBytes = 1088;
    /// What trackdisk leaves between the last sector and the first. Not critical -
    /// it only has to be long enough to cover the speed variation between the drive
    /// that wrote the disk and the one reading it.
    static constexpr size_t kDefaultTrackGapBytes = 1482;

    /**
     * @brief Encode a standard AmigaDOS track as MFM that can be WRITTEN to a disk.
     *
     * The difference from EncodeStandardTrack() is the clock bits. That one leaves
     * them zero, which round-trips through DecodeTrack() (it masks them out) and is
     * unreadable on real hardware: MFM is only decodable because the clock bits keep
     * every flux transition inside the window the PLL is hunting in, and a stream
     * without them has runs of zero cells no drive can clock through.
     *
     * The layout is trackdisk's, byte for byte - 1088 bytes a sector, gap first:
     *
     *     0..3      0xAA preamble
     *     4..7      0x4489 0x4489, the two sync words
     *     8..15     info long (0xFF, track, sector, sectors-to-gap), odd then even
     *     16..47    sector label, zero
     *     48..55    header checksum, odd then even
     *     56..63    data checksum, odd then even
     *     64..1087  512 data bytes: every odd half, then every even half
     *
     * The gap goes BEFORE sector 0 rather than after sector 10, which is the device
     * firmware's convention and worth keeping: if the gap is longer than what is left
     * of the track, what gets overwritten on the next revolution is the gap, not the
     * first sector's header.
     *
     * @param trackNumber cyl * 2 + head, stamped into every sector header.
     * @param sectorData Up to kSectorsPerTrack sectors of kSectorSize bytes.
     * @param trackGapBytes Gap written ahead of sector 0.
     * @result MFM cells, MSB first - exactly the shape FloppyBridge's writeData()
     *         takes and the device's writer clocks out.
     */
    static std::vector<uint8_t>
    EncodeWritableTrack(uint8_t trackNumber,
                        const std::vector<std::array<uint8_t, kSectorSize>>& sectorData,
                        size_t trackGapBytes = kDefaultTrackGapBytes);

    /**
     * @brief Check an MFM stream against the two rules a drive relies on.
     *
     * Never two adjacent 1 cells (transitions closer than the write circuitry can
     * place them), and never more than three 0 cells in a row (a gap longer than the
     * PLL's window, so it loses count) - except inside a `0x4489 0x4489` sync mark,
     * which breaks the second rule on purpose and is how a sync is recognisable at
     * all.
     *
     * This is the test that separates a stream a disk can hold from one that only
     * decodes in software. EncodeStandardTrack() fails it; EncodeWritableTrack()
     * passes it.
     *
     * @param firstBadCell If non-null, set to the offending cell index on failure.
     */
    static bool IsWritableMfm(const uint8_t* mfm, size_t lenBytes, size_t* firstBadCell = nullptr);
};

} // namespace adm::core
