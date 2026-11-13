#include "DiskCompare.h"

namespace adm::core {

uint64_t HashSector(const uint8_t* data, size_t size)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

std::vector<TrackCompareResult> CompareDiskImages(const std::vector<uint8_t>& a,
                                                  const std::vector<uint8_t>& b, int trackCount,
                                                  int sectorsPerTrack, int bytesPerSector)
{
    std::vector<TrackCompareResult> results(trackCount > 0 ? trackCount : 0);
    const size_t sectorBytes = static_cast<size_t>(bytesPerSector);

    for (int track = 0; track < trackCount; ++track) {
        TrackCompareResult& result = results[track];
        for (int sector = 0; sector < sectorsPerTrack; ++sector) {
            const size_t offset =
                (static_cast<size_t>(track) * sectorsPerTrack + sector) * sectorBytes;
            const bool haveA = offset + sectorBytes <= a.size();
            const bool haveB = offset + sectorBytes <= b.size();
            const uint32_t bit = 1u << sector;
            if (!haveA || !haveB) {
                result.differingSectors |= bit;
                result.missingSectors |= bit;
                continue;
            }
            if (HashSector(a.data() + offset, sectorBytes) !=
                HashSector(b.data() + offset, sectorBytes))
                result.differingSectors |= bit;
        }
    }
    return results;
}

} // namespace adm::core
