#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace adm::ui {

// A recorded frame's screen, from the frame history's thumbnails, for a scrub preview in a
// tooltip: the frame scrubber's seek bar and the Exocet timeline. The store keeps a shot every N
// frames, so this shows the nearest at or before the frame asked for and says which frame it is
// really from when that is not the one asked for. One texture, decoded and uploaded again only
// when the frame asked for changes: a drag asks on every mouse move, and a thumbnail costs a
// delta chain plus an upload.
class FramePreview
{
public:
    // The thumbnail at or before `frame` as top-row-first RGB888, and the frame it is from
    using Source = std::function<bool(uint64_t frame, std::vector<uint8_t>& rgb, int& w, int& h,
                                      uint64_t& gotFrame)>;

    FramePreview() = default;
    FramePreview(const FramePreview&) = delete;
    FramePreview& operator=(const FramePreview&) = delete;
    ~FramePreview();

    // The recorded range now. A change can mean the timeline was trimmed or re-recorded, so the
    // cached image may no longer belong to the frame it is filed under.
    void SetRange(uint64_t first, uint64_t last);

    // Draws it `width` wide at the Amiga display's 4:3, as one group: the image, and under it the
    // frame it is from when that is another. False, drawing nothing, when there is no shot.
    bool Draw(uint64_t frame, const Source& source, float width);

private:
    unsigned int m_tex = 0;        // GLuint
    uint64_t m_frame = UINT64_MAX; // frame the cached image was requested for
    uint64_t m_gotFrame = 0;       // frame it actually came from
    int m_w = 0, m_h = 0;
    uint64_t m_first = 0, m_last = 0;
    std::vector<uint8_t> m_rgb;
};

} // namespace adm::ui
