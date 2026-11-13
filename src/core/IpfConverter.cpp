#include "IpfConverter.h"

#include "AmigaMfmDecoder.h"
#include "CapsImageLoader.h"
#include "LibdiskClassifier.h"
#include "Logger.h"

#include <algorithm>

namespace adm::core {

namespace {

// Standard Amiga DD geometry: 80 cyl x 2 heads x 11 sectors x 512 bytes.
constexpr int kSectorsPerTrack = AmigaMfmDecoder::kSectorsPerTrack;
constexpr int kSectorSize = AmigaMfmDecoder::kSectorSize;
constexpr size_t kAdfSize = 80 * 2 * kSectorsPerTrack * kSectorSize; // 901120

} // namespace

std::optional<std::vector<uint8_t>>
IpfConverter::ConvertToBuffer(const std::vector<uint8_t>& ipfData,
                              std::vector<IpfTrackInfo>* outTracks)
{
    Logger::Info("Decoding IPF image ({} bytes) via capsimg", ipfData.size());

    auto decoded = CapsImageLoader::Instance().Decode(ipfData);
    if (!decoded) {
        // CapsImageLoader::LastError() carries the user-facing reason (missing
        // plugin, lock failure, etc.); the caller surfaces it in the UI.
        Logger::Error("IPF decode failed: {}", CapsImageLoader::Instance().LastError());
        return std::nullopt;
    }

    std::vector<uint8_t> adf(kAdfSize, 0);
    int standardTracks = 0;
    int protectedTracks = 0;

    // libdisk identifies named copy-protection formats. Created once per decode;
    // a no-op (Available()==false) when libdisk wasn't built.
    LibdiskClassifier classifier;

    for (const auto& raw : decoded->tracks) {
        auto track = AmigaMfmDecoder::DecodeTrack(raw.mfm.data(), raw.mfm.size());

        IpfTrackInfo info;
        info.cylinder = raw.cylinder;
        info.head = raw.head;
        info.mfmLengthBits = raw.lengthBits;
        info.weakCount = raw.weakCount;
        info.trackType = raw.trackType;
        info.sectorCount = static_cast<uint32_t>(track.sectors.size());
        info.isStandard = track.isStandard;

        // Name the track format. Standard tracks are AmigaDOS by definition, so
        // skip the (relatively costly) full probe for them; only run libdisk on
        // the non-standard tracks where protection schemes live.
        if (track.isStandard) {
            info.formatName = "AmigaDOS";
        } else if (classifier.Available() && !raw.mfm.empty()) {
            info.formatName =
                classifier.Classify(raw.mfm.data(), raw.mfm.size(), raw.cylinder * 2 + raw.head);
            // Anything matched that isn't an AmigaDOS variant or an unformatted
            // track is a copy-protection / custom loader format.
            info.isProtection = !info.formatName.empty() &&
                                info.formatName.rfind("AmigaDOS", 0) != 0 &&
                                info.formatName.rfind("Unformatted", 0) != 0;
            if (info.isProtection)
                ++protectedTracks;
        }

        // Place every checksum-valid sector into the ADF at its logical block.
        // The trackdisk header carries the physical track number (cyl*2+head),
        // which we trust for placement so a mis-ordered lock still lands right.
        for (const auto& sec : track.sectors) {
            if (!sec.checksumOk || sec.sectorNumber >= kSectorsPerTrack)
                continue;
            const size_t block =
                static_cast<size_t>(sec.trackNumber) * kSectorsPerTrack + sec.sectorNumber;
            const size_t offset = block * kSectorSize;
            if (offset + kSectorSize <= adf.size())
                std::copy(sec.data.begin(), sec.data.end(), adf.begin() + offset);
        }

        if (track.isStandard)
            ++standardTracks;

        if (outTracks)
            outTracks->push_back(info);
    }

    Logger::Info("IPF: {} of {} tracks standard AmigaDOS, {} identified as copy-protection",
                 standardTracks, decoded->tracks.size(), protectedTracks);

    return adf;
}

} // namespace adm::core
