#pragma once

#include <cstddef>
#include <cstdint>

namespace adm::core {

// FNV-1a 64. The question a scenario asks of a frame is "is this the same as last time",
// not "is this a preimage", so the bar is a fast, stable, dependency-free mix - and it has
// to stay bit-for-bit stable, because the values it produces are written into files that
// outlive the build that wrote them.
inline constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
inline constexpr uint64_t kFnvPrime = 0x100000001b3ull;

inline uint64_t Fnv1a64(const void* data, size_t size, uint64_t hash = kFnvOffsetBasis)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= kFnvPrime;
    }
    return hash;
}

// Fold a scalar in, little-endian, so the mix is the same on any host.
inline uint64_t Fnv1a64Value(uint64_t value, uint64_t hash = kFnvOffsetBasis)
{
    for (int i = 0; i < 8; ++i) {
        hash ^= static_cast<uint8_t>(value >> (8 * i));
        hash *= kFnvPrime;
    }
    return hash;
}

/**
 * @brief Digest one framebuffer exactly as the core delivered it.
 *
 * Row by row over `width * bytesPerPixel`, never over `pitch`: the padding at the end of a
 * row is an allocator detail and is not part of the picture, so hashing it would make the
 * digest depend on something the emulated machine does not control.
 *
 * Dimensions and pixel format are folded in first, so a resolution or format change reads
 * as a divergence rather than silently re-framing the comparison.
 */
uint64_t HashFramebuffer(const void* data, unsigned width, unsigned height, size_t pitch,
                         unsigned bytesPerPixel, uint32_t pixelFormat);

/**
 * @brief Accumulates the audio one frame produced.
 *
 * The core can hand over several blocks in a frame, or none. Samples are folded in as they
 * arrive and the total is counted separately: a fault that changes how many samples a frame
 * produces - one write to $DFF02A permanently halves PuAE's rate - deserves to be reported
 * as itself rather than as an opaque digest mismatch.
 */
class AudioDigestAccumulator
{
public:
    void AddSamples(const int16_t* data, size_t frames)
    {
        if (data && frames)
            m_hash = Fnv1a64(data, frames * 2 * sizeof(int16_t), m_hash);
        m_frames += frames;
    }

    uint64_t Hash() const { return m_hash; }
    uint32_t Frames() const { return static_cast<uint32_t>(m_frames); }

    void Reset()
    {
        m_hash = kFnvOffsetBasis;
        m_frames = 0;
    }

private:
    uint64_t m_hash = kFnvOffsetBasis;
    size_t m_frames = 0;
};

} // namespace adm::core
