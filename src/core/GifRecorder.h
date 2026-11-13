#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Forward-declare the vendored gif-h writer so its (heavy, non-inline-friendly) header stays
// confined to GifRecorder.cpp and out of every translation unit that records frames.
struct GifWriter;

namespace adm {

// Thin RAII wrapper around the vendored gif-h encoder. Accumulates RGBA frames into an animated
// GIF on disk. All frames must share the width/height passed to Begin().
class GifRecorder
{
public:
    GifRecorder();
    ~GifRecorder();

    GifRecorder(const GifRecorder&) = delete;
    GifRecorder& operator=(const GifRecorder&) = delete;

    // Open the output file and write the GIF header. delayCs is the per-frame delay in
    // hundredths of a second (e.g. 2 ≈ 50 fps). Returns false if the file could not be created.
    bool Begin(const std::string& path, int width, int height, int delayCs);

    // Append one frame. rgba must point to width*height*4 bytes, top row first. Returns false if
    // recording is not active or the dimensions don't match Begin().
    bool WriteFrame(const uint8_t* rgba, int width, int height);

    // Finalize the file and release resources. Safe to call when not active.
    bool End();

    bool IsActive() const { return m_active; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }

private:
    std::unique_ptr<GifWriter> m_writer;
    bool m_active = false;
    int m_width = 0;
    int m_height = 0;
    int m_delayCs = 2;
};

} // namespace adm
