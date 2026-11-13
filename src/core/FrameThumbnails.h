#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace adm::core {

struct FrameThumbnailStats
{
    size_t count = 0;        // thumbnails held
    size_t keyframeCount = 0;
    size_t bytesUsed = 0;
    size_t bytesBudget = 0;
    size_t imageBytes = 0;   // uncompressed size of one thumbnail
    int width = 0;
    int height = 0;
    uint64_t oldestFrame = 0;
    uint64_t newestFrame = 0;
    double compressionRatio = 0.0; // bytes held / bytes if every shot were stored whole
};

/**
 * @brief Periodic screen thumbnails alongside the frame-history timeline.
 *
 * The state buffer can seek to any recorded frame, but a save state is opaque: dragging the
 * seek bar tells you a number, not where in the demo you are. This keeps a small RGB image
 * every N frames so the scrub gesture can show the picture under the cursor without
 * restoring and re-running a frame for every pixel of mouse travel.
 *
 * Storage mirrors FrameHistory: periodic whole images (keyframes) plus byte-run deltas
 * against the preceding shot, using the same codec. Consecutive screens usually share large
 * static regions - borders, backgrounds, an unchanged panel - so the delta is typically a
 * small fraction of the image, and a completely static stretch costs almost nothing. Deltas
 * decode to a plain memcpy, which matters because a scrub decodes on the UI thread.
 *
 * Eviction drops whole keyframe groups from the front, never a single shot: a delta whose
 * base is gone cannot be decoded.
 *
 * Like FrameHistory, this holds only bytes - the caller feeds it images and decides when.
 */
class FrameThumbnails
{
public:
    static constexpr size_t kDefaultBudgetBytes = static_cast<size_t>(24) * 1024 * 1024;
    // Shots between whole images. Every group must be small enough that a scrub decodes
    // quickly; 16 keeps a worst-case decode at 16 memcpy passes over a ~60 KB buffer.
    static constexpr int kKeyframeInterval = 16;

    void SetBudgetBytes(size_t bytes);
    size_t BudgetBytes() const { return m_budget; }

    // Store one thumbnail. `rgb` is w*h*3 bytes, top row first. A different geometry means
    // a resolution change, which clears what is held - the older shots no longer describe
    // the same screen and cannot be delta-chained against the new ones.
    bool Record(uint64_t frame, const uint8_t* rgb, int w, int h);

    // Exact frame. False when that frame has no thumbnail.
    bool Get(uint64_t frame, std::vector<uint8_t>& out, int& w, int& h) const;

    // The shot at or before `frame`, falling back to the oldest held when `frame` predates
    // all of them. `gotFrame` reports which frame the image is actually from, so the UI can
    // say so rather than implying an exactness it does not have. False when nothing is held.
    bool GetNearest(uint64_t frame, std::vector<uint8_t>& out, int& w, int& h,
                    uint64_t& gotFrame) const;

    bool Range(uint64_t& oldest, uint64_t& newest) const;
    size_t Count() const { return m_shots.size(); }

    // Mirror the timeline's edits: play-on-from-behind trims the recorded future, and
    // eviction retires its oldest frames.
    void TrimAfter(uint64_t frame);
    void PruneBefore(uint64_t frame);

    void Clear();
    FrameThumbnailStats Stats() const;

private:
    struct Shot
    {
        uint64_t frame = 0;
        bool keyframe = false;
        std::vector<uint8_t> payload;
    };

    int FindAtOrBefore(uint64_t frame) const;
    int FindBase(int index) const;
    void EvictToBudget();
    std::vector<uint8_t> Reconstruct(int index) const;
    static size_t ShotBytes(const Shot& s);

    std::deque<Shot> m_shots;
    // The newest shot already decoded, so Record costs one scan instead of replaying a
    // whole group to find its base.
    std::vector<uint8_t> m_lastImage;
    size_t m_budget = kDefaultBudgetBytes;
    size_t m_bytesUsed = 0;
    size_t m_imageBytes = 0;
    int m_width = 0;
    int m_height = 0;
    int m_sinceKeyframe = 0;
};

} // namespace adm::core
