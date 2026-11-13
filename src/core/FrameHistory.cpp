#include "FrameHistory.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "ByteStream.h"
#include "FrameInputCodec.h"

namespace adm::core {

void EncodeDelta(const uint8_t* base, const uint8_t* cur, size_t size, std::vector<uint8_t>& out)
{
    size_t pos = 0;
    while (pos < size) {
        // Skip the identical stretch. A whole blob is scanned every frame, so this walks
        // eight bytes at a time before falling back to bytes inside the mismatching word.
        size_t runStart = pos;
        while (runStart + sizeof(uint64_t) <= size) {
            uint64_t a = 0, b = 0;
            std::memcpy(&a, base + runStart, sizeof(uint64_t));
            std::memcpy(&b, cur + runStart, sizeof(uint64_t));
            if (a != b)
                break;
            runStart += sizeof(uint64_t);
        }
        while (runStart < size && base[runStart] == cur[runStart])
            ++runStart;
        if (runStart >= size)
            break;

        // Extend the changed run, absorbing identical gaps too short to be worth a
        // second run header.
        size_t runEnd = runStart + 1;
        size_t gap = 0;
        for (size_t i = runStart + 1; i < size; ++i) {
            if (base[i] == cur[i]) {
                if (++gap >= FrameHistory::kMinRunGap)
                    break;
            } else {
                gap = 0;
                runEnd = i + 1;
            }
        }

        PutVarint(out, runStart - pos);
        PutVarint(out, runEnd - runStart);
        out.insert(out.end(), cur + runStart, cur + runEnd);
        pos = runEnd;
    }
}

bool DecodeDelta(const uint8_t* delta, size_t deltaSize, uint8_t* inout, size_t size)
{
    const uint8_t* p = delta;
    const uint8_t* const end = delta + deltaSize;
    size_t pos = 0;
    while (p < end) {
        uint64_t skip = 0, len = 0;
        if (!GetVarint(p, end, skip) || !GetVarint(p, end, len))
            return false;
        if (skip > size - pos)
            return false;
        pos += static_cast<size_t>(skip);
        if (len > size - pos || static_cast<uint64_t>(end - p) < len)
            return false;
        std::memcpy(inout + pos, p, static_cast<size_t>(len));
        p += len;
        pos += static_cast<size_t>(len);
    }
    return true;
}

void FrameHistory::SetEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (!enabled)
        Clear();
}

void FrameHistory::SetBudgetBytes(size_t bytes)
{
    m_budget = bytes;
    m_blobTooLarge = m_blobSize > 0 && m_blobSize > m_budget;
    EvictToBudget();
}

void FrameHistory::SetKeyframeInterval(int frames) { m_keyframeInterval = std::max(1, frames); }

size_t FrameHistory::EntryBytes(const Entry& e) const
{
    return sizeof(Entry) + e.payload.capacity() + e.input.keysDown.capacity() * sizeof(uint16_t) +
           e.input.keyEvents.capacity() * sizeof(FrameInput::KeyEvent);
}

int FrameHistory::FindEntry(uint64_t frame) const
{
    // Entries are appended in ascending frame order, so a binary search is valid.
    const auto it = std::lower_bound(m_entries.begin(), m_entries.end(), frame,
                                     [](const Entry& e, uint64_t f) { return e.frame < f; });
    if (it == m_entries.end() || it->frame != frame)
        return -1;
    return static_cast<int>(it - m_entries.begin());
}

int FrameHistory::FindBase(int index) const
{
    for (int i = index; i >= 0; --i) {
        if (m_entries[static_cast<size_t>(i)].keyframe)
            return i;
    }
    return -1;
}

bool FrameHistory::Record(uint64_t frame, const std::vector<uint8_t>& blob, const FrameInput& input)
{
    if (!m_enabled || blob.empty())
        return false;

    // A different blob size is a different machine (model switch, memory change): the
    // states already held describe something that no longer exists.
    if (!m_entries.empty() && blob.size() != m_blobSize)
        Clear();

    m_blobSize = blob.size();
    m_blobTooLarge = blob.size() > m_budget;
    if (m_blobTooLarge)
        return false;

    if (!m_entries.empty() && frame <= m_entries.back().frame)
        return false;

    const auto t0 = std::chrono::steady_clock::now();

    Entry e;
    e.frame = frame;
    e.input = input;

    // m_lastBlob mirrors the newest entry, so a delta costs one scan rather than an
    // O(keyframe interval) reconstruction. A size mismatch means it is unusable and the
    // frame has to anchor a new group.
    const bool needKeyframe = m_entries.empty() || m_lastBlob.size() != blob.size() ||
                              m_sinceKeyframe >= m_keyframeInterval;
    if (needKeyframe) {
        e.keyframe = true;
        e.payload = blob;
    } else {
        EncodeDelta(m_lastBlob.data(), blob.data(), blob.size(), e.payload);
        // A frame that changed nearly everything encodes larger than the blob it came
        // from; keep the blob instead and re-anchor the group on it.
        if (e.payload.size() >= blob.size()) {
            e.keyframe = true;
            e.payload = blob;
        }
    }
    e.payload.shrink_to_fit();

    if (!e.keyframe) {
        ++m_deltaSamples;
        m_deltaBytesTotal += e.payload.size();
    }
    m_sinceKeyframe = e.keyframe ? 0 : m_sinceKeyframe + 1;

    m_bytesUsed += EntryBytes(e);
    m_entries.push_back(std::move(e));
    m_lastBlob = blob;
    EvictToBudget();

    const auto t1 = std::chrono::steady_clock::now();
    m_recordMsTotal += std::chrono::duration<double, std::milli>(t1 - t0).count();
    ++m_recordSamples;
    return true;
}

std::vector<uint8_t> FrameHistory::Reconstruct(uint64_t frame) const
{
    const int idx = FindEntry(frame);
    if (idx < 0)
        return {};
    const int base = FindBase(idx);
    if (base < 0)
        return {};

    std::vector<uint8_t> out = m_entries[static_cast<size_t>(base)].payload;
    if (out.size() != m_blobSize)
        return {};
    for (int i = base + 1; i <= idx; ++i) {
        const Entry& e = m_entries[static_cast<size_t>(i)];
        if (!DecodeDelta(e.payload.data(), e.payload.size(), out.data(), out.size()))
            return {};
    }
    return out;
}

void FrameHistory::InvalidateCursor() const
{
    m_cursorValid = false;
    m_cursorFrame = 0;
}

bool FrameHistory::ReconstructInto(uint64_t frame, std::vector<uint8_t>& out) const
{
    const int idx = FindEntry(frame);
    if (idx < 0)
        return false;

    // The frame already in the cursor, or the entry straight after it: one delta decode
    // rather than a walk back to the keyframe. Entry order is the timeline's order, so
    // idx-1 is the right test even across a gap in frame numbers.
    if (m_cursorValid && m_cursorBlob.size() == m_blobSize) {
        if (m_cursorFrame == frame) {
            out = m_cursorBlob;
            return true;
        }
        if (idx > 0 && m_entries[static_cast<size_t>(idx - 1)].frame == m_cursorFrame &&
            !m_entries[static_cast<size_t>(idx)].keyframe) {
            const Entry& e = m_entries[static_cast<size_t>(idx)];
            if (DecodeDelta(e.payload.data(), e.payload.size(), m_cursorBlob.data(),
                            m_cursorBlob.size())) {
                m_cursorFrame = frame;
                out = m_cursorBlob;
                return true;
            }
            // A half-applied delta has left the cursor holding neither frame.
            InvalidateCursor();
        }
    }

    out = Reconstruct(frame);
    if (out.size() != m_blobSize || out.empty()) {
        InvalidateCursor();
        return false;
    }
    m_cursorBlob = out;
    m_cursorFrame = frame;
    m_cursorValid = true;
    return true;
}

bool FrameHistory::GetInput(uint64_t frame, FrameInput& out) const
{
    const int idx = FindEntry(frame);
    if (idx < 0)
        return false;
    out = m_entries[static_cast<size_t>(idx)].input;
    return true;
}

bool FrameHistory::Has(uint64_t frame) const { return FindEntry(frame) >= 0; }

bool FrameHistory::PreviousFrame(uint64_t frame, uint64_t& prev) const
{
    const int idx = FindEntry(frame);
    if (idx <= 0)
        return false;
    prev = m_entries[static_cast<size_t>(idx) - 1].frame;
    return true;
}

bool FrameHistory::NextFrame(uint64_t frame, uint64_t& next) const
{
    const int idx = FindEntry(frame);
    if (idx < 0 || static_cast<size_t>(idx) + 1 >= m_entries.size())
        return false;
    next = m_entries[static_cast<size_t>(idx) + 1].frame;
    return true;
}

bool FrameHistory::NearestFrame(uint64_t frame, uint64_t& out) const
{
    if (m_entries.empty())
        return false;
    const auto it = std::lower_bound(m_entries.begin(), m_entries.end(), frame,
                                     [](const Entry& e, uint64_t f) { return e.frame < f; });
    if (it == m_entries.end()) {
        out = m_entries.back().frame;
        return true;
    }
    if (it == m_entries.begin() || it->frame == frame) {
        out = it->frame;
        return true;
    }
    const uint64_t after = it->frame;
    const uint64_t before = (it - 1)->frame;
    // Ties go to the older frame: a scrub that lands between two entries is nearly always
    // reaching for the one it has already passed.
    out = (after - frame) < (frame - before) ? after : before;
    return true;
}

bool FrameHistory::FirstSeekable(uint64_t& out) const
{
    if (m_entries.size() < 2)
        return false;
    out = m_entries[1].frame;
    return true;
}

bool FrameHistory::Range(uint64_t& oldest, uint64_t& newest) const
{
    if (m_entries.empty())
        return false;
    oldest = m_entries.front().frame;
    newest = m_entries.back().frame;
    return true;
}

void FrameHistory::TrimAfter(uint64_t frame)
{
    InvalidateCursor();
    while (!m_entries.empty() && m_entries.back().frame > frame) {
        m_bytesUsed -= EntryBytes(m_entries.back());
        m_entries.pop_back();
    }
    if (m_entries.empty()) {
        m_lastBlob.clear();
        m_lastBlob.shrink_to_fit();
        m_sinceKeyframe = 0;
        return;
    }

    m_lastBlob = Reconstruct(m_entries.back().frame);
    // Re-anchor the keyframe cadence on what is still stored, so the next recorded frame
    // does not force a keyframe it does not need.
    m_sinceKeyframe = 0;
    for (int i = static_cast<int>(m_entries.size()) - 1; i >= 0; --i) {
        if (m_entries[static_cast<size_t>(i)].keyframe)
            break;
        ++m_sinceKeyframe;
    }
}

void FrameHistory::EvictToBudget()
{
    while (m_bytesUsed > m_budget) {
        // Everything before the second keyframe is one complete group. Individual entries
        // can never be dropped: a delta whose base is gone cannot be reconstructed.
        size_t next = 0;
        for (size_t i = 1; i < m_entries.size(); ++i) {
            if (m_entries[i].keyframe) {
                next = i;
                break;
            }
        }
        if (next == 0)
            return; // one group left - keep it rather than lose the timeline entirely

        for (size_t i = 0; i < next; ++i)
            m_bytesUsed -= EntryBytes(m_entries[i]);
        m_entries.erase(m_entries.begin(), m_entries.begin() + static_cast<ptrdiff_t>(next));
        m_evictedFrames += next;
    }
}

void FrameHistory::Clear()
{
    m_entries.clear();
    m_lastBlob.clear();
    m_lastBlob.shrink_to_fit();
    InvalidateCursor();
    m_cursorBlob.clear();
    m_cursorBlob.shrink_to_fit();
    m_bytesUsed = 0;
    m_blobSize = 0;
    m_sinceKeyframe = 0;
    m_blobTooLarge = false;
    m_deltaSamples = 0;
    m_deltaBytesTotal = 0;
    m_recordSamples = 0;
    m_recordMsTotal = 0.0;
}

std::vector<uint8_t> FrameHistory::Serialize() const
{
    std::vector<uint8_t> out;
    // Deltas dominate; reserving what is already accounted for avoids a dozen reallocations
    // of a buffer that can run to hundreds of megabytes.
    out.reserve(m_bytesUsed + 64);

    PutLE<uint32_t>(out, kSerialMagic);
    PutLE<uint32_t>(out, kSerialVersion);
    PutLE<uint64_t>(out, static_cast<uint64_t>(m_blobSize));
    PutLE<uint32_t>(out, static_cast<uint32_t>(m_keyframeInterval));
    PutLE<uint64_t>(out, static_cast<uint64_t>(m_entries.size()));

    for (const Entry& e : m_entries) {
        PutLE<uint64_t>(out, e.frame);
        PutLE<uint8_t>(out, e.keyframe ? 1u : 0u);
        PutLE<uint64_t>(out, static_cast<uint64_t>(e.payload.size()));
        out.insert(out.end(), e.payload.begin(), e.payload.end());

        WriteFrameInput(out, e.input);
    }
    return out;
}

bool FrameHistory::Deserialize(const uint8_t* data, size_t size, size_t expectedBlobSize)
{
    InvalidateCursor();
    Clear();
    if (!data || size < 28)
        return false;

    ByteReader r(data, size);
    if (r.Get<uint32_t>() != kSerialMagic || r.Get<uint32_t>() != kSerialVersion)
        return false;

    const size_t blobSize = static_cast<size_t>(r.Get<uint64_t>());
    const uint32_t keyframeInterval = r.Get<uint32_t>();
    const uint64_t count = r.Get<uint64_t>();
    if (!r.Ok() || blobSize == 0)
        return false;
    // A timeline recorded on a different machine holds states this core cannot apply.
    if (expectedBlobSize != 0 && blobSize != expectedBlobSize)
        return false;

    std::deque<Entry> entries;
    for (uint64_t i = 0; i < count; ++i) {
        Entry e;
        e.frame = r.Get<uint64_t>();
        e.keyframe = r.Get<uint8_t>() != 0;
        const uint64_t payloadLen = r.Get<uint64_t>();
        if (!r.Ok() || payloadLen > r.Remaining())
            return false;
        if (const uint8_t* p = r.Take(static_cast<size_t>(payloadLen)))
            e.payload.assign(p, p + payloadLen);
        else
            return false;
        // A keyframe is the whole blob by definition; anything else cannot be a base.
        if (e.keyframe && e.payload.size() != blobSize)
            return false;

        if (!ReadFrameInput(r, e.input))
            return false;

        // Frames must ascend, and the first entry has to be a keyframe or nothing after it
        // can be reconstructed.
        if (i == 0 ? !e.keyframe : e.frame <= entries.back().frame)
            return false;
        entries.push_back(std::move(e));
    }
    if (!r.Ok())
        return false;

    m_blobSize = blobSize;
    m_keyframeInterval = std::max(1, static_cast<int>(keyframeInterval));
    m_entries = std::move(entries);
    for (const Entry& e : m_entries)
        m_bytesUsed += EntryBytes(e);

    // Rebuild the derived state the recorder needs to carry on appending.
    m_sinceKeyframe = 0;
    for (int i = static_cast<int>(m_entries.size()) - 1; i >= 0; --i) {
        if (m_entries[static_cast<size_t>(i)].keyframe)
            break;
        ++m_sinceKeyframe;
    }
    if (!m_entries.empty()) {
        m_lastBlob = Reconstruct(m_entries.back().frame);
        if (m_lastBlob.size() != m_blobSize) {
            Clear();
            return false;
        }
    }
    EvictToBudget();
    return true;
}

FrameHistoryStats FrameHistory::Stats() const
{
    FrameHistoryStats s;
    s.recording = m_enabled;
    s.empty = m_entries.empty();
    s.frameCount = m_entries.size();
    s.bytesUsed = m_bytesUsed;
    s.bytesBudget = m_budget;
    s.blobSize = m_blobSize;
    s.evictedFrames = m_evictedFrames;

    if (!m_entries.empty()) {
        s.oldestFrame = m_entries.front().frame;
        s.newestFrame = m_entries.back().frame;
        uint64_t expected = m_entries.front().frame;
        for (const Entry& e : m_entries) {
            if (e.keyframe)
                ++s.keyframeCount;
            if (e.frame != expected)
                ++s.gapCount;
            expected = e.frame + 1;
        }
        // Amortised cost per frame, keyframes included - that is what actually decides
        // how far back the budget reaches.
        const double perFrame =
            static_cast<double>(m_bytesUsed) / static_cast<double>(s.frameCount);
        if (perFrame > 0.0) {
            s.estimatedCapacityFrames =
                static_cast<uint64_t>(static_cast<double>(m_budget) / perFrame);
        }
    }
    if (m_deltaSamples > 0) {
        s.avgDeltaBytes =
            static_cast<double>(m_deltaBytesTotal) / static_cast<double>(m_deltaSamples);
    }
    if (m_recordSamples > 0)
        s.avgRecordMs = m_recordMsTotal / static_cast<double>(m_recordSamples);

    if (m_blobTooLarge) {
        s.hint = "a single save state is larger than the whole frame-history budget - "
                 "raise the budget or the timeline cannot record";
    }
    return s;
}

} // namespace adm::core
