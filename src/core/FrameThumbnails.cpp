#include "FrameThumbnails.h"

#include <algorithm>

#include "FrameHistory.h" // EncodeDelta / DecodeDelta

namespace adm::core {

size_t FrameThumbnails::ShotBytes(const Shot& s) { return sizeof(Shot) + s.payload.capacity(); }

void FrameThumbnails::SetBudgetBytes(size_t bytes)
{
    m_budget = bytes;
    EvictToBudget();
}

int FrameThumbnails::FindAtOrBefore(uint64_t frame) const
{
    // Shots are appended in ascending frame order, so a binary search is valid.
    const auto it = std::upper_bound(m_shots.begin(), m_shots.end(), frame,
                                     [](uint64_t f, const Shot& s) { return f < s.frame; });
    if (it == m_shots.begin())
        return -1;
    return static_cast<int>((it - 1) - m_shots.begin());
}

int FrameThumbnails::FindBase(int index) const
{
    for (int i = index; i >= 0; --i) {
        if (m_shots[static_cast<size_t>(i)].keyframe)
            return i;
    }
    return -1;
}

std::vector<uint8_t> FrameThumbnails::Reconstruct(int index) const
{
    const int base = FindBase(index);
    if (base < 0)
        return {};
    std::vector<uint8_t> out = m_shots[static_cast<size_t>(base)].payload;
    if (out.size() != m_imageBytes)
        return {};
    for (int i = base + 1; i <= index; ++i) {
        const Shot& s = m_shots[static_cast<size_t>(i)];
        if (!DecodeDelta(s.payload.data(), s.payload.size(), out.data(), out.size()))
            return {};
    }
    return out;
}

bool FrameThumbnails::Record(uint64_t frame, const uint8_t* rgb, int w, int h)
{
    if (!rgb || w <= 0 || h <= 0)
        return false;
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    if (bytes > m_budget)
        return false;

    // A geometry change is a different screen: the held shots cannot be delta bases for it
    // and would decode to garbage at the new dimensions.
    if (!m_shots.empty() && (w != m_width || h != m_height))
        Clear();
    m_width = w;
    m_height = h;
    m_imageBytes = bytes;

    if (!m_shots.empty() && frame <= m_shots.back().frame)
        return false;

    Shot s;
    s.frame = frame;

    const bool needKeyframe =
        m_shots.empty() || m_lastImage.size() != bytes || m_sinceKeyframe >= kKeyframeInterval;
    if (needKeyframe) {
        s.keyframe = true;
        s.payload.assign(rgb, rgb + bytes);
    } else {
        EncodeDelta(m_lastImage.data(), rgb, bytes, s.payload);
        // A screen that changed nearly everywhere encodes larger than the image itself;
        // store the image and re-anchor the group on it.
        if (s.payload.size() >= bytes) {
            s.keyframe = true;
            s.payload.assign(rgb, rgb + bytes);
        }
    }
    s.payload.shrink_to_fit();
    m_sinceKeyframe = s.keyframe ? 0 : m_sinceKeyframe + 1;

    m_bytesUsed += ShotBytes(s);
    m_shots.push_back(std::move(s));
    m_lastImage.assign(rgb, rgb + bytes);
    EvictToBudget();
    return true;
}

bool FrameThumbnails::Get(uint64_t frame, std::vector<uint8_t>& out, int& w, int& h) const
{
    const int idx = FindAtOrBefore(frame);
    if (idx < 0 || m_shots[static_cast<size_t>(idx)].frame != frame)
        return false;
    out = Reconstruct(idx);
    if (out.empty())
        return false;
    w = m_width;
    h = m_height;
    return true;
}

bool FrameThumbnails::GetNearest(uint64_t frame, std::vector<uint8_t>& out, int& w, int& h,
                                 uint64_t& gotFrame) const
{
    if (m_shots.empty())
        return false;
    int idx = FindAtOrBefore(frame);
    if (idx < 0)
        idx = 0; // before everything held - the oldest shot is the closest there is
    out = Reconstruct(idx);
    if (out.empty())
        return false;
    gotFrame = m_shots[static_cast<size_t>(idx)].frame;
    w = m_width;
    h = m_height;
    return true;
}

bool FrameThumbnails::Range(uint64_t& oldest, uint64_t& newest) const
{
    if (m_shots.empty())
        return false;
    oldest = m_shots.front().frame;
    newest = m_shots.back().frame;
    return true;
}

void FrameThumbnails::TrimAfter(uint64_t frame)
{
    while (!m_shots.empty() && m_shots.back().frame > frame) {
        m_bytesUsed -= ShotBytes(m_shots.back());
        m_shots.pop_back();
    }
    if (m_shots.empty()) {
        m_lastImage.clear();
        m_lastImage.shrink_to_fit();
        m_sinceKeyframe = 0;
        return;
    }
    m_lastImage = Reconstruct(static_cast<int>(m_shots.size()) - 1);
    // Re-anchor the keyframe cadence on what survived, so the next shot does not force a
    // whole image it does not need.
    m_sinceKeyframe = 0;
    for (int i = static_cast<int>(m_shots.size()) - 1; i >= 0; --i) {
        if (m_shots[static_cast<size_t>(i)].keyframe)
            break;
        ++m_sinceKeyframe;
    }
}

void FrameThumbnails::PruneBefore(uint64_t frame)
{
    // Whole groups only: a delta whose keyframe has been dropped cannot be decoded. The
    // group containing `frame` therefore stays, even though part of it is older.
    while (m_shots.size() > 1) {
        size_t next = 0;
        for (size_t i = 1; i < m_shots.size(); ++i) {
            if (m_shots[i].keyframe) {
                next = i;
                break;
            }
        }
        if (next == 0 || m_shots[next].frame > frame)
            return;
        for (size_t i = 0; i < next; ++i)
            m_bytesUsed -= ShotBytes(m_shots[i]);
        m_shots.erase(m_shots.begin(), m_shots.begin() + static_cast<ptrdiff_t>(next));
    }
}

void FrameThumbnails::EvictToBudget()
{
    while (m_bytesUsed > m_budget) {
        size_t next = 0;
        for (size_t i = 1; i < m_shots.size(); ++i) {
            if (m_shots[i].keyframe) {
                next = i;
                break;
            }
        }
        if (next == 0)
            return; // one group left - keep it rather than lose the previews entirely
        for (size_t i = 0; i < next; ++i)
            m_bytesUsed -= ShotBytes(m_shots[i]);
        m_shots.erase(m_shots.begin(), m_shots.begin() + static_cast<ptrdiff_t>(next));
    }
}

void FrameThumbnails::Clear()
{
    m_shots.clear();
    m_lastImage.clear();
    m_lastImage.shrink_to_fit();
    m_bytesUsed = 0;
    m_imageBytes = 0;
    m_width = 0;
    m_height = 0;
    m_sinceKeyframe = 0;
}

FrameThumbnailStats FrameThumbnails::Stats() const
{
    FrameThumbnailStats s;
    s.count = m_shots.size();
    s.bytesUsed = m_bytesUsed;
    s.bytesBudget = m_budget;
    s.imageBytes = m_imageBytes;
    s.width = m_width;
    s.height = m_height;
    for (const Shot& shot : m_shots) {
        if (shot.keyframe)
            ++s.keyframeCount;
    }
    if (!m_shots.empty()) {
        s.oldestFrame = m_shots.front().frame;
        s.newestFrame = m_shots.back().frame;
        const double whole = static_cast<double>(m_imageBytes) * static_cast<double>(s.count);
        if (whole > 0.0)
            s.compressionRatio = static_cast<double>(m_bytesUsed) / whole;
    }
    return s;
}

} // namespace adm::core
