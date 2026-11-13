#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace adm::core {

/**
 * @brief Result of decoding an image via RECOIL.
 */
struct DecodedImage
{
    std::vector<uint8_t> pixels; // RGBA8888 format
    int width = 0;
    int height = 0;
    float pixelAspectRatio = 1.0f; // For non-square pixels (e.g., Amiga lo-res)
    std::string errorMessage;

    [[nodiscard]] bool IsValid() const { return !pixels.empty() && width > 0 && height > 0; }
};

/**
 * @brief Decodes retro computer image formats using the RECOIL library.
 *
 * RECOIL (Retro Computer Image Library) supports 500+ image formats from
 * various retro computers including Amiga IFF ILBM, HAM, EHB modes.
 */
class ImageLoader
{
public:
    ImageLoader();
    ~ImageLoader();

    // Non-copyable
    ImageLoader(const ImageLoader&) = delete;
    ImageLoader& operator=(const ImageLoader&) = delete;

    // Movable
    ImageLoader(ImageLoader&&) noexcept;
    ImageLoader& operator=(ImageLoader&&) noexcept;

    /**
     * @brief Decode image from raw file data.
     * @param data Raw file bytes.
     * @param filename Filename with extension (RECOIL uses extension for format hints).
     * @return Decoded image data or error information.
     */
    [[nodiscard]] DecodedImage Decode(const std::vector<uint8_t>& data,
                                      const std::string& filename) const;

    /**
     * @brief Decode image from string data.
     * @param data Raw file data as string.
     * @param filename Filename with extension.
     * @return Decoded image data or error information.
     */
    [[nodiscard]] DecodedImage Decode(const std::string& data, const std::string& filename) const;

private:
    /**
     * @brief Detect pixel aspect ratio from image dimensions.
     *
     * Common Amiga resolutions have non-square pixels:
     * - 320x200/256: PAR ~2.0 (tall pixels, stretch width)
     * - 640x200/256: PAR ~0.5 (wide pixels, stretch height)
     * - 640x400/512: PAR ~1.0 (square pixels)
     */
    [[nodiscard]] static float DetectPixelAspectRatio(int width, int height);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace adm::core
