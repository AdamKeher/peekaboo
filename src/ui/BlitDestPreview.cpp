#include "BlitDestPreview.h"

#include <glad/glad.h>

#include <algorithm>
#include <cmath>

namespace adm::ui {

namespace {

// Three fixed colours, because a blit is one bitplane and carries no palette of its own:
// a set bit, a clear bit, and a pixel this blit never touched (the modulo gap, or a row it
// skipped). "Untouched" is transparent so it reads as a hole rather than as a colour.
constexpr uint8_t kSetRGBA[4] = {235, 235, 235, 255};
constexpr uint8_t kClearRGBA[4] = {24, 30, 44, 255};
constexpr uint8_t kUntouchedRGBA[4] = {0, 0, 0, 0};

// The texture's own GL_NEAREST filter is not enough: the ImGui backend binds a linear sampler
// object for every draw, and a bound sampler overrides the texture's parameters. Switching it
// around the image (and back afterwards, so nothing else in the window inherits it) is what
// actually keeps blitted pixels sharp — the same pattern BitplanesView and the magnifier use.
void ImageNearest(unsigned int texId, const ImVec2& size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
    ImGui::Image((ImTextureID)(uintptr_t)texId, size);
    dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
}

} // namespace

BlitDestPreview::~BlitDestPreview() { Clear(); }

void BlitDestPreview::Clear()
{
    for (auto& [key, entry] : m_cache) {
        if (entry.texId)
            glDeleteTextures(1, &entry.texId);
    }
    m_cache.clear();
    m_rgbaScratch.clear();
    m_rgbaScratch.shrink_to_fit();
}

void BlitDestPreview::BeginFrame()
{
    ++m_frame;
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (m_frame - it->second.lastUsedFrame > kEvictAfterFrames) {
            if (it->second.texId)
                glDeleteTextures(1, &it->second.texId);
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

const BlitDestPreview::Entry* BlitDestPreview::Acquire(const adm::core::BlitRecord& rec)
{
    if (!HasData(rec))
        return nullptr;

    // The captured word buffer is allocated fresh per blit per frame, so its address is a
    // sufficient (and free) identity for the cache.
    const void* key = static_cast<const void*>(rec.destWords.get());
    if (auto it = m_cache.find(key); it != m_cache.end()) {
        it->second.lastUsedFrame = m_frame;
        return &it->second;
    }

    const adm::core::BlitDestImage img =
        adm::core::BuildBlitDestImage(rec, rec.destWords->data(), rec.destWords->size());
    if (!img.valid)
        return nullptr;

    m_rgbaScratch.resize(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4);
    for (size_t i = 0; i < img.pixels.size(); ++i) {
        const uint8_t* src = img.pixels[i] == adm::core::BlitDestImage::kSet     ? kSetRGBA
                             : img.pixels[i] == adm::core::BlitDestImage::kClear ? kClearRGBA
                                                                                 : kUntouchedRGBA;
        std::copy(src, src + 4, m_rgbaScratch.begin() + static_cast<ptrdiff_t>(i * 4));
    }

    Entry entry;
    glGenTextures(1, &entry.texId);
    glBindTexture(GL_TEXTURE_2D, entry.texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 m_rgbaScratch.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    entry.width = img.width;
    entry.height = img.height;
    entry.baseAddr = img.baseAddr;
    entry.rowStride = img.rowStride;
    entry.truncated = rec.destTruncated;
    entry.lastUsedFrame = m_frame;

    return &m_cache.emplace(key, entry).first->second;
}

// Fits w x h inside maxW x maxH without upscaling past whole-pixel multiples, so a 16x4 blit
// still reads as blocks rather than a smear.
static ImVec2 FitSize(int w, int h, float maxW, float maxH)
{
    const float scale = std::min(maxW / static_cast<float>(w), maxH / static_cast<float>(h));
    if (scale >= 1.0f) {
        const float snapped = std::max(1.0f, std::floor(scale));
        return {static_cast<float>(w) * snapped, static_cast<float>(h) * snapped};
    }
    return {std::max(1.0f, static_cast<float>(w) * scale),
            std::max(1.0f, static_cast<float>(h) * scale)};
}

bool BlitDestPreview::Thumbnail(const adm::core::BlitRecord& rec, float maxW, float maxH)
{
    const Entry* entry = Acquire(rec);
    if (!entry)
        return false;

    const ImVec2 size = FitSize(entry->width, entry->height, maxW, maxH);
    ImageNearest(entry->texId, size);
    return true;
}

void BlitDestPreview::Detail(const adm::core::BlitRecord& rec, float maxW, float maxH)
{
    const Entry* entry = Acquire(rec);
    if (!entry) {
        ImGui::TextDisabled("No destination data captured for this blit.");
        return;
    }

    const ImVec2 size = FitSize(entry->width, entry->height, maxW, maxH);

    // A checkerboard behind the image so transparent "never written" pixels are visibly a
    // hole rather than whatever happens to be behind the tooltip.
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float kCheck = 8.0f;
    dl->PushClipRect(p0, {p0.x + size.x, p0.y + size.y}, true);
    for (float y = 0; y < size.y; y += kCheck) {
        for (float x = 0; x < size.x; x += kCheck) {
            const bool odd = (static_cast<int>(x / kCheck) + static_cast<int>(y / kCheck)) & 1;
            dl->AddRectFilled({p0.x + x, p0.y + y}, {p0.x + x + kCheck, p0.y + y + kCheck},
                              odd ? IM_COL32(58, 58, 62, 255) : IM_COL32(44, 44, 48, 255));
        }
    }
    dl->PopClipRect();

    ImageNearest(entry->texId, size);

    ImGui::Text("%dx%d px  from $%06X  stride %u bytes  (%zu words)", entry->width, entry->height,
                entry->baseAddr, entry->rowStride, rec.destWords->size());
    if (entry->truncated)
        ImGui::TextColored({1.0f, 0.75f, 0.2f, 1.0f}, "Capture truncated — showing the start "
                                                      "of the blit only.");
}

} // namespace adm::ui
