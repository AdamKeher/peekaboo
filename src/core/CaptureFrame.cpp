#include "CaptureFrame.h"

#include <cstring>

namespace adm::core {

namespace {

size_t BytesPerPixel(CaptureLayout layout)
{
    return layout == CaptureLayout::BgrxTopDown ? 4u : 3u;
}

size_t RowBytes(CaptureLayout layout, int outWidth)
{
    const size_t raw = static_cast<size_t>(outWidth) * BytesPerPixel(layout);
    return layout == CaptureLayout::BgrBottomUpPadded ? ((raw + 3u) & ~size_t{3}) : raw;
}

} // namespace

size_t CaptureFrameBytes(CaptureLayout layout, int outWidth, int outHeight)
{
    if (outWidth <= 0 || outHeight <= 0)
        return 0;
    return RowBytes(layout, outWidth) * static_cast<size_t>(outHeight);
}

void ConvertCaptureFrame(const uint8_t* rgba, int width, int height, int scaleX, int scaleY,
                         CaptureLayout layout, uint8_t* out)
{
    const int outWidth = width * scaleX;
    const size_t rowBytes = RowBytes(layout, outWidth);
    const bool bottomUp = layout == CaptureLayout::BgrBottomUpPadded;

    // Each source row is converted once, sx wide, then copied for the other sy-1 output rows.
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = rgba + static_cast<size_t>(bottomUp ? height - 1 - y : y) * width * 4u;
        uint8_t* dst = out + static_cast<size_t>(y) * scaleY * rowBytes;
        uint8_t* p = dst;
        for (int x = 0; x < width; ++x) {
            const uint8_t r = src[x * 4 + 0];
            const uint8_t g = src[x * 4 + 1];
            const uint8_t b = src[x * 4 + 2];
            for (int k = 0; k < scaleX; ++k) {
                switch (layout) {
                    case CaptureLayout::BgrBottomUpPadded:
                        *p++ = b;
                        *p++ = g;
                        *p++ = r;
                        break;
                    case CaptureLayout::BgrxTopDown:
                        *p++ = b;
                        *p++ = g;
                        *p++ = r;
                        *p++ = 0; // ZMBV compares whole pixels; a stray alpha reads as a change
                        break;
                    case CaptureLayout::RgbTopDown:
                        *p++ = r;
                        *p++ = g;
                        *p++ = b;
                        break;
                }
            }
        }
        // BI_RGB row padding stays zero.
        std::memset(p, 0, static_cast<size_t>(dst + rowBytes - p));
        for (int k = 1; k < scaleY; ++k)
            std::memcpy(dst + static_cast<size_t>(k) * rowBytes, dst, rowBytes);
    }
}

} // namespace adm::core
