#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace adm::core {

// Sector-by-sector comparison of two floppy images - a disk read off a physical drive against an
// open ADF. Each sector is reduced to a 64-bit hash and the hashes are compared, so the result
// says which sectors differ without holding on to either image.
struct TrackCompareResult
{
    // Bit n set: sector n of this track differs. Bit n of `missing` set: one image is too short
    // to have sector n at all, which counts as a difference too.
    uint32_t differingSectors = 0;
    uint32_t missingSectors = 0;

    bool Matches() const { return differingSectors == 0; }
};

// FNV-1a, 64-bit. Not cryptographic - two sectors written by the same drive either decoded to
// the same bytes or did not, and a collision is not something anyone is trying to provoke here.
uint64_t HashSector(const uint8_t* data, size_t size);

// Compares `trackCount` tracks of `sectorsPerTrack` x `bytesPerSector` bytes. A sector either
// image lacks is reported as differing and missing.
std::vector<TrackCompareResult> CompareDiskImages(const std::vector<uint8_t>& a,
                                                  const std::vector<uint8_t>& b, int trackCount,
                                                  int sectorsPerTrack, int bytesPerSector);

} // namespace adm::core
