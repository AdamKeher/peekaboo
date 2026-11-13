#include "ClipboardImage.h"

#include <cstddef>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#include <filesystem>

#include <stb_image_write.h> // declarations only; implementation in external/stb/stb_image_write_impl.cpp
#endif

namespace adm::core {

#ifdef _WIN32

// CF_DIB is a BITMAPINFOHEADER followed by the pixels, bottom row first, in BGR order, with every
// row padded out to a 4-byte boundary. Every Windows image consumer understands it, which the
// newer CF_DIBV5 cannot be relied on for.
bool CopyImageToClipboard(const uint8_t* rgb, int width, int height, std::string& outError)
{
    if (!rgb || width <= 0 || height <= 0) {
        outError = "no image to copy";
        return false;
    }

    const size_t srcStride = static_cast<size_t>(width) * 3;
    const size_t dstStride = (srcStride + 3) & ~static_cast<size_t>(3);
    const size_t pixelBytes = dstStride * static_cast<size_t>(height);

    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelBytes);
    if (!handle) {
        outError = "out of memory building the clipboard bitmap";
        return false;
    }

    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(handle));
    if (!header) {
        GlobalFree(handle);
        outError = "could not lock the clipboard bitmap";
        return false;
    }

    *header = BITMAPINFOHEADER{};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = width;
    header->biHeight = height; // positive: bottom-up
    header->biPlanes = 1;
    header->biBitCount = 24;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixelBytes);

    auto* dst = reinterpret_cast<uint8_t*>(header + 1);
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = rgb + static_cast<size_t>(height - 1 - y) * srcStride;
        uint8_t* row = dst + static_cast<size_t>(y) * dstStride;
        for (int x = 0; x < width; ++x, src += 3, row += 3) {
            row[0] = src[2];
            row[1] = src[1];
            row[2] = src[0];
        }
        // Zero the alignment padding rather than leaving whatever GlobalAlloc handed us.
        for (size_t pad = srcStride; pad < dstStride; ++pad)
            dst[static_cast<size_t>(y) * dstStride + pad] = 0;
    }
    GlobalUnlock(handle);

    if (!OpenClipboard(nullptr)) {
        GlobalFree(handle);
        outError = "another application is holding the clipboard open";
        return false;
    }
    EmptyClipboard();
    // On success the clipboard owns the handle and must not be freed here.
    if (!SetClipboardData(CF_DIB, handle)) {
        CloseClipboard();
        GlobalFree(handle);
        outError = "the clipboard rejected the image";
        return false;
    }
    CloseClipboard();
    return true;
}

#else

namespace {

// A per-call temp filename: two copies in flight would otherwise fight over one path.
std::filesystem::path MakeTempPngPath()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("peekaboo-clipboard-" + std::to_string(stamp) + ".png");
}

} // namespace

bool CopyImageToClipboard(const uint8_t* rgb, int width, int height, std::string& outError)
{
    if (!rgb || width <= 0 || height <= 0) {
        outError = "no image to copy";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path png = MakeTempPngPath();
    if (!stbi_write_png(png.string().c_str(), width, height, 3, rgb, width * 3)) {
        outError = "could not write the temporary PNG";
        return false;
    }

    // The path is one we just generated, so there is no injection surface in the quoting below.
#ifdef __APPLE__
    // osascript reads the file and exits, so the temporary can go straight afterwards.
    const std::string cmd = "/usr/bin/osascript -e 'set the clipboard to (read (POSIX file \"" +
                            png.string() + "\") as \xc2\xabclass PNGf\xc2\xbb)' >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    std::filesystem::remove(png, ec);
    if (rc != 0) {
        outError = "osascript could not put the image on the clipboard";
        return false;
    }
    return true;
#else
    // xclip has to be probed separately. The launch below ends in '&', so the shell forks and
    // exits 0 whether or not xclip exists - checking the return of that command can never report
    // a missing binary.
    if (std::system("command -v xclip >/dev/null 2>&1") != 0) {
        std::filesystem::remove(png, ec);
        outError = "could not run xclip - install xclip to copy images to the clipboard";
        return false;
    }

    // xclip backgrounds itself and serves the selection from memory for as long as it owns it, so
    // the call returns immediately - and the temporary is left in place rather than raced against.
    const std::string cmd =
        "xclip -selection clipboard -t image/png -i '" + png.string() + "' >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) {
        std::filesystem::remove(png, ec);
        outError = "could not start xclip";
        return false;
    }
    return true;
#endif
}

#endif

} // namespace adm::core
