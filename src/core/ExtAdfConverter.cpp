#include "ExtAdfConverter.h"
#include "BigEndian.h"

#include "AmigaMfmDecoder.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>

namespace adm::core {

namespace {

constexpr int kSectorsPerTrack = AmigaMfmDecoder::kSectorsPerTrack; // 11 (DD)
constexpr int kSectorSize = AmigaMfmDecoder::kSectorSize;           // 512

// UAE image_tracktype values (see disk.c). Only AMIGADOS has a distinct
// storage layout for us; every other type is treated as raw MFM and probed.
constexpr uint8_t kTrackAmigaDos = 0;

// UAE-1ADF header + per-track descriptor sizes.
constexpr size_t kHeaderBytes = 8 + 2 + 2;        // "UAE-1ADF" + reserved + track count
constexpr size_t kTrackDescBytes = 2 + 2 + 4 + 4; // reserved + type + len + bitlen

// 24-bit big-endian read (the high byte of the 4-byte field is unused in
// practice — read_header_ext2 ignores buffer[4]/buffer[8]).
uint32_t Be24(const uint8_t* p) { return be::U24(p); }

} // namespace

bool ExtAdfConverter::IsExtAdf(const std::vector<uint8_t>& data)
{
    return data.size() >= 8 && std::memcmp(data.data(), "UAE-1ADF", 8) == 0;
}

std::optional<std::vector<uint8_t>>
ExtAdfConverter::ConvertToBuffer(const std::vector<uint8_t>& extData,
                                 std::vector<IpfTrackInfo>* outTracks)
{
    if (!IsExtAdf(extData) || extData.size() < kHeaderBytes) {
        Logger::Error("ExtAdf: not a UAE-1ADF image or truncated header");
        return std::nullopt;
    }

    // Track count is a big-endian word at offset 10 (bytes 8-9 reserved).
    const uint32_t numTracks =
        static_cast<uint32_t>(extData[10]) * 256 + static_cast<uint32_t>(extData[11]);
    if (numTracks == 0 || numTracks > 2 * 83) {
        Logger::Error("ExtAdf: implausible track count {}", numTracks);
        return std::nullopt;
    }

    const size_t descTableEnd = kHeaderBytes + static_cast<size_t>(numTracks) * kTrackDescBytes;
    if (extData.size() < descTableEnd) {
        Logger::Error("ExtAdf: descriptor table runs past end of file ({} tracks)", numTracks);
        return std::nullopt;
    }

    // Parse descriptors, mirroring read_header_ext2: data blobs are concatenated
    // after the table, each occupying `len` bytes.
    struct TrackDesc
    {
        uint8_t type;
        uint32_t len;    // bytes this track occupies in the file
        uint32_t bitlen; // decoded-sector-bits (AmigaDOS) or MFM-track-bits (raw)
        size_t offs;     // file offset of the track's data blob
    };
    std::vector<TrackDesc> descs(numTracks);
    size_t offs = descTableEnd;
    bool ddhd = false;
    for (uint32_t i = 0; i < numTracks; ++i) {
        const uint8_t* d = extData.data() + kHeaderBytes + static_cast<size_t>(i) * kTrackDescBytes;
        TrackDesc& t = descs[i];
        t.type = d[3];
        t.len = Be24(d + 5);
        t.bitlen = Be24(d + 9);
        t.offs = offs;
        if (t.len > 20000)
            ddhd = true;
        offs += t.len;
    }

    // Standard geometry follows the density: DD = 11 sec/track, HD = 22.
    const int numSecs = ddhd ? kSectorsPerTrack * 2 : kSectorsPerTrack;
    const size_t trackBytes = static_cast<size_t>(numSecs) * kSectorSize;
    const size_t adfSize = 80u * 2u * trackBytes; // 901120 (DD) / 1802240 (HD)

    std::vector<uint8_t> adf(adfSize, 0);
    int standardTracks = 0;
    int protectedTracks = 0;

    for (uint32_t i = 0; i < numTracks; ++i) {
        const TrackDesc& t = descs[i];

        IpfTrackInfo info;
        info.cylinder = i / 2;
        info.head = i % 2;
        info.mfmLengthBits = t.bitlen;

        // Bounds-check the blob against the file.
        const bool blobOk = t.offs <= extData.size() && t.len <= extData.size() - t.offs;
        if (!blobOk) {
            info.formatName = "Truncated";
            if (outTracks)
                outTracks->push_back(info);
            continue;
        }

        if (t.type == kTrackAmigaDos) {
            // Already-decoded AmigaDOS sectors: copy numSecs*512 bytes straight
            // into the ADF at this track's linear position. bitlen == secs*512*8.
            const size_t copyBytes = std::min<size_t>({trackBytes, t.len, (t.bitlen + 7) / 8});
            const size_t adfOffset = static_cast<size_t>(i) * trackBytes;
            if (adfOffset + copyBytes <= adf.size())
                std::copy_n(extData.begin() + t.offs, copyBytes, adf.begin() + adfOffset);
            info.sectorCount = numSecs;
            info.isStandard = true;
            info.formatName = "AmigaDOS";
            ++standardTracks;
            if (outTracks)
                outTracks->push_back(info);
            continue;
        }

        // Everything else is a raw MFM bitstream: recover whatever standard Amiga
        // sectors it contains. Non-Amiga protection yields none and stays blank.
        const size_t mfmBytes = std::min<size_t>(t.len, (t.bitlen + 7) / 8);
        auto track = AmigaMfmDecoder::DecodeTrack(extData.data() + t.offs, mfmBytes);

        info.sectorCount = static_cast<uint32_t>(track.sectors.size());
        info.isStandard = track.isStandard;

        if (track.isStandard) {
            info.formatName = "AmigaDOS";
            ++standardTracks;
        } else {
            // Generic labels only: naming the exact protection scheme would mean
            // running libdisk's ~200 trial decoders per track, which on a fully
            // protected disk (150+ raw tracks) freezes the load for over a minute.
            // The Track Map's standard-vs-protected picture doesn't need the name.
            info.isProtection = info.sectorCount == 0;
            info.formatName = info.sectorCount > 0 ? "Partial AmigaDOS" : "Protected / raw MFM";
            if (info.isProtection)
                ++protectedTracks;
        }

        // Place every checksum-valid sector at its logical block (trust the
        // trackdisk header's track number, as the IPF path does).
        for (const auto& sec : track.sectors) {
            if (!sec.checksumOk || sec.sectorNumber >= numSecs)
                continue;
            const size_t block = static_cast<size_t>(sec.trackNumber) * numSecs + sec.sectorNumber;
            const size_t offset = block * kSectorSize;
            if (offset + kSectorSize <= adf.size())
                std::copy(sec.data.begin(), sec.data.end(), adf.begin() + offset);
        }

        if (outTracks)
            outTracks->push_back(info);
    }

    Logger::Info("ExtAdf: {} tracks ({}), {} standard AmigaDOS, {} identified as copy-protection",
                 numTracks, ddhd ? "HD" : "DD", standardTracks, protectedTracks);

    return adf;
}

} // namespace adm::core
