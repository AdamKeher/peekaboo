#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "../core/BlitDestImage.h"
#include "../core/BlitProfiler.h"

namespace adm::ui {

// Renders "what the blit actually wrote" as a picture: a per-blit thumbnail for a table cell,
// and a full-size version for a tooltip.
//
// A blit writes one bitplane's worth of bits, so there is no palette to apply. Set bits, clear
// bits and never-written pixels get three fixed colours instead, which keeps the modulo gap and
// any rows the blit skipped visible rather than silently reading as background.
//
// Textures are cached by the identity of the captured word buffer, so a row redrawn every frame
// with unchanged data uploads nothing. Entries not touched for a few draws are released, which
// bounds the cache to roughly the number of rows on screen.
class BlitDestPreview
{
public:
    ~BlitDestPreview();

    // Releases every cached texture. Call when the GL context is going away.
    void Clear();

    // Call once per frame, before any Thumbnail()/Tooltip() call, so unused entries age out.
    void BeginFrame();

    // Draws a thumbnail scaled to fit within maxW x maxH, preserving aspect. Returns false
    // (drawing nothing) when the blit has no captured data.
    bool Thumbnail(const adm::core::BlitRecord& rec, float maxW, float maxH);

    // Draws the full bitmap plus a line of geometry, sized to fit within maxW x maxH.
    // Intended for use inside a tooltip or a detail panel.
    void Detail(const adm::core::BlitRecord& rec, float maxW, float maxH);

    // True when this blit has destination words to show.
    static bool HasData(const adm::core::BlitRecord& rec)
    {
        return rec.destWords && !rec.destWords->empty();
    }

private:
    struct Entry
    {
        unsigned int texId = 0;
        int width = 0, height = 0;
        uint32_t baseAddr = 0;
        uint32_t rowStride = 0;
        bool truncated = false;
        uint64_t lastUsedFrame = 0;
    };

    // Builds (or returns the cached) texture for this blit's destination data.
    const Entry* Acquire(const adm::core::BlitRecord& rec);

    std::unordered_map<const void*, Entry> m_cache;
    std::vector<uint8_t> m_rgbaScratch;
    uint64_t m_frame = 0;

    // Entries untouched for this many BeginFrame() calls are released. Small enough to keep
    // the cache near the visible row count, large enough that scrolling back a row or two
    // does not re-upload.
    static constexpr uint64_t kEvictAfterFrames = 8;
};

} // namespace adm::ui
