#include "ImageLoader.h"

#include "Logger.h"

extern "C" {
#include <recoil.h>
}

namespace adm::core {

struct ImageLoader::Impl
{
    RECOIL* recoil = nullptr;

    Impl() { recoil = RECOIL_New(); }

    ~Impl()
    {
        if (recoil) {
            RECOIL_Delete(recoil);
        }
    }
};

ImageLoader::ImageLoader() : m_impl(std::make_unique<Impl>()) {}

ImageLoader::~ImageLoader() = default;

ImageLoader::ImageLoader(ImageLoader&&) noexcept = default;
ImageLoader& ImageLoader::operator=(ImageLoader&&) noexcept = default;

DecodedImage ImageLoader::Decode(const std::vector<uint8_t>& data,
                                 const std::string& filename) const
{
    DecodedImage result;

    if (!m_impl || !m_impl->recoil) {
        result.errorMessage = "ImageLoader not initialized";
        return result;
    }

    if (data.empty()) {
        result.errorMessage = "Empty input data";
        return result;
    }

    // RECOIL_Decode uses filename for format detection
    if (!RECOIL_Decode(m_impl->recoil, filename.c_str(), data.data(),
                       static_cast<int>(data.size()))) {
        result.errorMessage = "RECOIL failed to decode image (unsupported format or corrupt data)";
        Logger::Warn("Failed to decode image: {}", filename);
        return result;
    }

    result.width = RECOIL_GetWidth(m_impl->recoil);
    result.height = RECOIL_GetHeight(m_impl->recoil);

    if (result.width <= 0 || result.height <= 0) {
        result.errorMessage = "Invalid image dimensions";
        return result;
    }

    // RECOIL provides pixels in XRGB format (0x00RRGGBB)
    // Convert to RGBA8888 for OpenGL
    const int* srcPixels = RECOIL_GetPixels(m_impl->recoil);
    if (!srcPixels) {
        result.errorMessage = "Failed to get pixel data";
        return result;
    }

    const int numPixels = result.width * result.height;
    result.pixels.resize(numPixels * 4);

    for (int i = 0; i < numPixels; ++i) {
        int xrgb = srcPixels[i];
        result.pixels[i * 4 + 0] = (xrgb >> 16) & 0xFF; // R
        result.pixels[i * 4 + 1] = (xrgb >> 8) & 0xFF;  // G
        result.pixels[i * 4 + 2] = xrgb & 0xFF;         // B
        result.pixels[i * 4 + 3] = 0xFF;                // A (opaque)
    }

    // Check if RECOIL provides pixel density information for aspect ratio
    int xDensity = RECOIL_GetXPixelsPerMeter(m_impl->recoil);
    int yDensity = RECOIL_GetYPixelsPerMeter(m_impl->recoil);

    if (xDensity > 0 && yDensity > 0 && xDensity != yDensity) {
        // RECOIL provides pixel density - derive aspect ratio
        // PAR = yDensity / xDensity (inverse because higher X density = narrower pixels)
        result.pixelAspectRatio = static_cast<float>(yDensity) / static_cast<float>(xDensity);
    } else {
        // Fall back to heuristic detection based on resolution
        result.pixelAspectRatio = DetectPixelAspectRatio(result.width, result.height);
    }

    Logger::Info("Decoded image: {}x{}, PAR={:.2f}", result.width, result.height,
                 result.pixelAspectRatio);

    return result;
}

DecodedImage ImageLoader::Decode(const std::string& data, const std::string& filename) const
{
    std::vector<uint8_t> bytes(data.begin(), data.end());
    return Decode(bytes, filename);
}

float ImageLoader::DetectPixelAspectRatio(int width, int height)
{
    // Common Amiga resolutions and their pixel aspect ratios:
    //
    // NTSC (200 lines):
    //   320x200 lo-res:  PAR ~10:11 but displayed on 4:3 monitor = stretch width
    //   640x200 hi-res:  PAR ~5:11 = stretch height significantly
    //
    // PAL (256 lines):
    //   320x256 lo-res:  PAR ~1:1.25 = slightly stretch width
    //   640x256 hi-res:  PAR ~1:2.5 = stretch height
    //
    // Interlaced (400/512 lines):
    //   320x400/512:     PAR same as above but doubled height
    //   640x400/512:     Close to square pixels
    //
    // The goal is to display images as they appeared on an Amiga monitor.
    // We use approximate ratios that look correct on modern square-pixel displays.

    // Lo-res non-interlaced (NTSC)
    if (width == 320 && height == 200) {
        return 1.2f; // Slight horizontal stretch for proper 4:3
    }
    // Lo-res non-interlaced (PAL)
    if (width == 320 && height == 256) {
        return 1.0f; // PAL lo-res is close to square
    }
    // Lo-res interlaced (NTSC)
    if (width == 320 && height == 400) {
        return 1.2f;
    }
    // Lo-res interlaced (PAL)
    if (width == 320 && height == 512) {
        return 1.0f;
    }

    // Hi-res non-interlaced (NTSC) - very wide pixels
    if (width == 640 && height == 200) {
        return 0.6f; // Stretch height to correct
    }
    // Hi-res non-interlaced (PAL)
    if (width == 640 && height == 256) {
        return 0.5f; // Stretch height
    }
    // Hi-res interlaced - close to square
    if (width == 640 && (height == 400 || height == 512)) {
        return 1.0f;
    }

    // Overscan and other resolutions - use heuristics
    if (width <= 384 && height <= 280) {
        // Likely lo-res variant
        return 1.1f;
    }
    if (width >= 640 && height <= 280) {
        // Likely hi-res non-interlaced
        return 0.55f;
    }

    // Default: assume square pixels
    return 1.0f;
}

} // namespace adm::core
