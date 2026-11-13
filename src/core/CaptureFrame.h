#pragma once

#include <cstddef>
#include <cstdint>

namespace adm::core {

// The pixel layouts the capture writers store, and the one conversion they share: RGBA, top row
// first, from the core's framebuffer, enlarged by whole-number factors - replicated, never
// interpolated, so an Amiga pixel stays a sharp block.
enum class CaptureLayout {
    BgrBottomUpPadded, // BI_RGB: rows bottom-up, 3 bytes a pixel, rows padded to 4 bytes
    BgrxTopDown,       // ZMBV: rows top-down, 4 bytes a pixel, fourth byte zero
    RgbTopDown,        // ffmpeg -f rawvideo -pix_fmt rgb24
};

// Bytes one output frame takes in `layout`.
size_t CaptureFrameBytes(CaptureLayout layout, int outWidth, int outHeight);

// Converts `rgba` (width x height) into `out`, which must hold CaptureFrameBytes for
// width*scaleX x height*scaleY.
void ConvertCaptureFrame(const uint8_t* rgba, int width, int height, int scaleX, int scaleY,
                         CaptureLayout layout, uint8_t* out);

} // namespace adm::core
