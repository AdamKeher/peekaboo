#include "AppCapture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glad/glad.h>

#include <stb_image_write.h> // declarations only; implementation in external/stb/stb_image_write_impl.cpp

namespace adm::core {

namespace {

// Area-average downscale, RGB8. stb_image_resize is not vendored (external/stb carries only
// stb_image.h and stb_image_write.h), and a box filter is the right answer for UI screenshots
// anyway: it keeps text legible at 2:1 without the ringing a sharper kernel introduces.
// Handles non-integer ratios (1920 -> 1280) by averaging over fractional source spans.
std::vector<uint8_t> BoxDownscale(const std::vector<uint8_t>& src, int sw, int sh, int dw, int dh)
{
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * 3);
    const float xr = static_cast<float>(sw) / dw;
    const float yr = static_cast<float>(sh) / dh;

    for (int dy = 0; dy < dh; ++dy) {
        const int y0 = static_cast<int>(dy * yr);
        const int y1 = std::max(y0 + 1, std::min(sh, static_cast<int>((dy + 1) * yr)));
        for (int dx = 0; dx < dw; ++dx) {
            const int x0 = static_cast<int>(dx * xr);
            const int x1 = std::max(x0 + 1, std::min(sw, static_cast<int>((dx + 1) * xr)));

            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; ++y) {
                const uint8_t* row = src.data() + (static_cast<size_t>(y) * sw + x0) * 3;
                for (int x = x0; x < x1; ++x, row += 3) {
                    r += row[0];
                    g += row[1];
                    b += row[2];
                    ++n;
                }
            }
            uint8_t* out = dst.data() + (static_cast<size_t>(dy) * dw + dx) * 3;
            out[0] = static_cast<uint8_t>(r / n);
            out[1] = static_cast<uint8_t>(g / n);
            out[2] = static_cast<uint8_t>(b / n);
        }
    }
    return dst;
}

bool EncodePng(const std::vector<uint8_t>& rgb, int w, int h, std::vector<uint8_t>& out)
{
    auto writeCb = [](void* ctx, void* data, int size) {
        auto* vec = static_cast<std::vector<uint8_t>*>(ctx);
        const auto* bytes = static_cast<const uint8_t*>(data);
        vec->insert(vec->end(), bytes, bytes + size);
    };
    out.clear();
    return stbi_write_png_to_func(writeCb, &out, w, h, 3, rgb.data(), w * 3) != 0 && !out.empty();
}

} // namespace

bool AppCapture::Request(const UiCaptureRequest& req)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending)
        return false;
    m_pending = req;
    m_result.reset();
    m_requestedAt = std::chrono::steady_clock::now();
    return true;
}

bool AppCapture::HasPending() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.has_value();
}

std::optional<UiCaptureResult> AppCapture::Take()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_result) {
        if (m_pending && std::chrono::steady_clock::now() - m_requestedAt > kDeadline) {
            m_pending.reset();
            UiCaptureResult timedOut;
            timedOut.error = "the application did not render a frame within 8s, so there was "
                             "nothing to capture (is the window minimised?)";
            return timedOut;
        }
        return std::nullopt;
    }
    auto out = std::move(*m_result);
    m_result.reset();
    return out;
}

void AppCapture::Service(int fbW, int fbH, float displayW, float displayH)
{
    UiCaptureRequest req;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_pending)
            return;
        // Let the requested number of frames pass first: a focus or dock change made by the tool
        // that queued this capture has not been drawn yet.
        if (m_pending->delayFrames > 0) {
            --m_pending->delayFrames;
            return;
        }
        req = *m_pending;
    }

    UiCaptureResult result;
    auto finish = [&]() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.reset();
        m_result = std::move(result);
    };

    if (fbW <= 0 || fbH <= 0) {
        result.error = "application framebuffer has no size (window minimised?)";
        finish();
        return;
    }

    // ImGui coordinates are points; the framebuffer is pixels. On a scaled display the two differ,
    // so convert the requested rect rather than assuming 1:1.
    const float sx = displayW > 0.0f ? fbW / displayW : 1.0f;
    const float sy = displayH > 0.0f ? fbH / displayH : 1.0f;

    int x = 0, y = 0, w = fbW, h = fbH;
    if (!req.wholeApp) {
        x = static_cast<int>(std::floor(req.x * sx));
        y = static_cast<int>(std::floor(req.y * sy));
        w = static_cast<int>(std::ceil(req.w * sx));
        h = static_cast<int>(std::ceil(req.h * sy));
        // Clamp into the framebuffer: a window can hang off the edge of the app window.
        x = std::clamp(x, 0, fbW - 1);
        y = std::clamp(y, 0, fbH - 1);
        w = std::clamp(w, 1, fbW - x);
        h = std::clamp(h, 1, fbH - y);
    }

    // glReadPixels' origin is bottom-left, so the requested top-edge y becomes this bottom edge.
    const int glY = fbH - (y + h);

    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, glY, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    // Flip to top-row-first, which is what PNG wants.
    const size_t stride = static_cast<size_t>(w) * 3;
    std::vector<uint8_t> flipped(rgb.size());
    for (int row = 0; row < h; ++row)
        std::copy_n(rgb.data() + static_cast<size_t>(h - 1 - row) * stride, stride,
                    flipped.data() + static_cast<size_t>(row) * stride);

    result.sourceWidth = w;
    result.sourceHeight = h;

    int outW = w, outH = h;
    if (req.maxWidth > 0 && w > req.maxWidth) {
        outW = req.maxWidth;
        outH = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * outW / w)));
        flipped = BoxDownscale(flipped, w, h, outW, outH);
        result.downscaled = true;
    }

    result.width = outW;
    result.height = outH;
    result.ok = EncodePng(flipped, outW, outH, result.png);
    if (!result.ok)
        result.error = "PNG encode failed";
    finish();
}

} // namespace adm::core
