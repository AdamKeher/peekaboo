#include "FrameDigest.h"

namespace adm::core {

uint64_t HashFramebuffer(const void* data, unsigned width, unsigned height, size_t pitch,
                         unsigned bytesPerPixel, uint32_t pixelFormat)
{
    if (!data || width == 0 || height == 0 || bytesPerPixel == 0)
        return 0;

    uint64_t hash = kFnvOffsetBasis;
    hash = Fnv1a64Value(width, hash);
    hash = Fnv1a64Value(height, hash);
    hash = Fnv1a64Value(bytesPerPixel, hash);
    hash = Fnv1a64Value(pixelFormat, hash);

    const auto* rows = static_cast<const uint8_t*>(data);
    const size_t rowBytes = static_cast<size_t>(width) * bytesPerPixel;
    // A pitch narrower than a row would mean the caller has the geometry wrong; hashing off
    // the end of the buffer is not the way to find that out.
    const size_t stride = pitch >= rowBytes ? pitch : rowBytes;
    for (unsigned y = 0; y < height; ++y)
        hash = Fnv1a64(rows + static_cast<size_t>(y) * stride, rowBytes, hash);

    return hash;
}

} // namespace adm::core
