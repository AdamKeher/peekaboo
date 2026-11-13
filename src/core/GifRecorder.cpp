#include "GifRecorder.h"

#include <gif.h>

namespace adm {

GifRecorder::GifRecorder() : m_writer(std::make_unique<GifWriter>()) {}

GifRecorder::~GifRecorder() { End(); }

bool GifRecorder::Begin(const std::string& path, int width, int height, int delayCs)
{
    if (m_active)
        End();
    if (width <= 0 || height <= 0)
        return false;

    m_delayCs = delayCs > 0 ? delayCs : 2;
    if (!GifBegin(m_writer.get(), path.c_str(), static_cast<uint32_t>(width),
                  static_cast<uint32_t>(height), static_cast<uint32_t>(m_delayCs))) {
        return false;
    }

    m_width = width;
    m_height = height;
    m_active = true;
    return true;
}

bool GifRecorder::WriteFrame(const uint8_t* rgba, int width, int height)
{
    if (!m_active || !rgba)
        return false;
    if (width != m_width || height != m_height)
        return false;

    return GifWriteFrame(m_writer.get(), rgba, static_cast<uint32_t>(width),
                         static_cast<uint32_t>(height), static_cast<uint32_t>(m_delayCs));
}

bool GifRecorder::End()
{
    if (!m_active)
        return false;
    bool ok = GifEnd(m_writer.get());
    m_active = false;
    m_width = m_height = 0;
    return ok;
}

} // namespace adm
