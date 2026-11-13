#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "FrameInput.h"

namespace adm::core {

// One contiguous range of emulated RAM that differs between two recorded frames.
struct FrameDiffRun
{
    uint32_t address = 0;
    uint32_t length = 0;
    std::string region; // "Chip RAM", "Slow RAM", "Fast RAM", ...
};

struct FrameHistoryStats
{
    bool recording = false;
    bool empty = true;
    uint64_t oldestFrame = 0;
    uint64_t newestFrame = 0;
    size_t frameCount = 0;
    size_t keyframeCount = 0;
    size_t bytesUsed = 0;
    size_t bytesBudget = 0;
    size_t blobSize = 0;        // uncompressed savestate size for this machine config
    uint64_t evictedFrames = 0; // frames dropped so far to stay under budget
    // Places where the recorded frame numbers are not consecutive. A frame is missing
    // whenever the machine never completed one under that number - a mid-frame breakpoint
    // stop is the usual cause. The timeline still replays across a gap (consecutive
    // entries are consecutive machine states), but the frame numbers jump.
    size_t gapCount = 0;
    double avgDeltaBytes = 0.0; // mean encoded size of a non-keyframe
    double avgRecordMs = 0.0;   // mean wall time spent encoding a frame
    // How far back the budget reaches at the observed amortised cost per frame.
    // 0 until something has been recorded.
    uint64_t estimatedCapacityFrames = 0;
    // Set when a single savestate is larger than the whole budget, which makes
    // recording impossible. Carries the explanation for the UI.
    std::string hint;
};

/**
 * @brief A rolling timeline of per-frame machine states — the "state buffer".
 *
 * Modelled on ENGINE9000's state buffer: one save state per frame, stored as periodic
 * keyframes plus deltas against the preceding frame, capped by a byte budget rather than
 * a frame count. That is what makes stepping *backwards* possible, and with it the more
 * valuable trick of restoring frame N-1 and re-running frame N with capture enabled, so
 * blitter/copper/DMA/CPU-trace telemetry can be produced for a frame that has already
 * gone past.
 *
 * A delta is a list of changed byte runs carrying their new contents, not an XOR: PUAE
 * savestate blobs are structurally stable frame to frame for a fixed machine config, so
 * the runs line up and literals cost the same as XOR values while decoding to a plain
 * memcpy. Runs separated by a short identical gap are merged, because the per-run header
 * costs more than copying a few unchanged bytes.
 *
 * Eviction drops whole keyframe groups from the front, never an individual entry: a delta
 * whose base has been evicted cannot be reconstructed.
 *
 * The class holds only bytes — it never touches the emulator. Callers feed it blobs from
 * AmigaEmulator::SaveState() and hand Reconstruct()'s result back to LoadState().
 */
class FrameHistory
{
public:
    static constexpr size_t kDefaultBudgetBytes = static_cast<size_t>(256) * 1024 * 1024;
    static constexpr int kDefaultKeyframeInterval = 60;
    // Runs closer together than this are merged into one; below it the run header costs
    // more than copying the unchanged bytes between them.
    static constexpr size_t kMinRunGap = 8;

    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled; }

    // Budget in bytes. Shrinking evicts immediately.
    void SetBudgetBytes(size_t bytes);
    size_t BudgetBytes() const { return m_budget; }

    // Frames between keyframes. Clamped to at least 1. Takes effect from the next frame,
    // so an existing timeline stays reconstructable.
    void SetKeyframeInterval(int frames);
    int KeyframeInterval() const { return m_keyframeInterval; }

    // Append one frame's machine state. Returns false (recording nothing) when disabled,
    // when `blob` is empty, when `frame` is not newer than the newest recorded frame, or
    // when a single blob cannot fit the budget. A blob whose size differs from the
    // timeline's clears the timeline first — that is a model switch or a disk swap, and
    // the older states no longer describe this machine.
    bool Record(uint64_t frame, const std::vector<uint8_t>& blob, const FrameInput& input);

    // Rebuild the full savestate for a recorded frame. Empty if the frame is not held.
    std::vector<uint8_t> Reconstruct(uint64_t frame) const;

    // Same, into a caller-owned buffer, and cheap when frames are asked for in timeline
    // order. Playback restores a state per frame, so a full rebuild - up to a whole
    // keyframe interval of delta decodes, plus a megabyte-scale allocation - every frame
    // is the difference between a loop that keeps up and one that does not. A cursor
    // holds the last blob rebuilt: asking for the entry straight after it costs a single
    // delta decode. Any other frame falls back to a full rebuild and re-seeds the cursor.
    // Returns false when the frame is not held or the timeline is malformed.
    bool ReconstructInto(uint64_t frame, std::vector<uint8_t>& out) const;

    // The input recorded with a frame. False if the frame is not held.
    bool GetInput(uint64_t frame, FrameInput& out) const;

    bool Has(uint64_t frame) const;

    // Timeline adjacency, which is NOT frame-number adjacency: a frame can be missing (a
    // mid-frame breakpoint never completes one) while the entries either side of the hole
    // are still consecutive machine states. Anything that walks the timeline has to use
    // these rather than frame +/- 1, or a single missing frame strands it.
    bool PreviousFrame(uint64_t frame, uint64_t& prev) const;
    bool NextFrame(uint64_t frame, uint64_t& next) const;
    // The recorded frame closest to `frame`, preferring the older one on a tie. False when
    // the timeline is empty. Used to land a seek on something that actually exists rather
    // than refusing a frame number the user had every reason to believe in.
    bool NearestFrame(uint64_t frame, uint64_t& out) const;
    // The oldest frame that can be seeked to: the second entry held, because re-running a
    // frame needs the state that preceded it. False when fewer than two are held.
    bool FirstSeekable(uint64_t& out) const;
    // Oldest and newest recorded frame. False when the timeline is empty.
    bool Range(uint64_t& oldest, uint64_t& newest) const;
    size_t FrameCount() const { return m_entries.size(); }

    // Drop every frame after `frame`. ENGINE9000 trims the timeline ahead when you seek
    // back and then play on, so the recorded future cannot contradict what just ran.
    void TrimAfter(uint64_t frame);

    // Forget everything (reset, model change, new document).
    void Clear();

    FrameHistoryStats Stats() const;

    // Persist the whole timeline (keyframes, deltas and per-frame input) as one byte
    // stream, and load one back. ENGINE9000's toolbar Save/Restore keeps the timeline, not
    // just a single state, so a hard-won debugging context survives the session.
    //
    // Deserialize replaces whatever is held. It refuses a stream whose save-state size
    // differs from `expectedBlobSize` (pass 0 to skip the check): resuming a timeline
    // recorded on a different machine would hand the core states it cannot apply.
    // Returns false on a malformed or mismatched stream, leaving the timeline empty.
    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const uint8_t* data, size_t size, size_t expectedBlobSize);

    // Stream format identity, so a caller can reject an old file before parsing it.
    static constexpr uint32_t kSerialMagic = 0x48464B50; // "PKFH"
    static constexpr uint32_t kSerialVersion = 1;

private:
    // One recorded frame. A keyframe carries the whole blob; every other entry carries a
    // delta against the frame immediately before it.
    struct Entry
    {
        uint64_t frame = 0;
        bool keyframe = false;
        std::vector<uint8_t> payload;
        FrameInput input;
    };

    // Index of the entry for `frame`, or -1.
    int FindEntry(uint64_t frame) const;
    // Index of the newest keyframe at or before `index`, or -1 if the base is missing.
    int FindBase(int index) const;
    // Evict whole keyframe groups from the front until the budget is met. Keeps the last
    // remaining group even when it alone exceeds the budget.
    void EvictToBudget();
    size_t EntryBytes(const Entry& e) const;

    std::deque<Entry> m_entries;

    // The newest entry, already reconstructed. Keeping it turns Record into one scan
    // plus one copy instead of an O(keyframe interval) rebuild of the previous frame.
    std::vector<uint8_t> m_lastBlob;

    // Rolling cursor for ReconstructInto(): the frame it holds and that frame's blob.
    // Purely a cache - dropping it only costs speed - so every mutation of m_entries
    // invalidates it rather than trying to keep it in step.
    void InvalidateCursor() const;
    mutable std::vector<uint8_t> m_cursorBlob;
    mutable uint64_t m_cursorFrame = 0;
    mutable bool m_cursorValid = false;
    size_t m_budget = kDefaultBudgetBytes;
    int m_keyframeInterval = kDefaultKeyframeInterval;
    bool m_enabled = false;
    size_t m_bytesUsed = 0;
    size_t m_blobSize = 0; // 0 until the first frame is recorded
    uint64_t m_evictedFrames = 0;
    int m_sinceKeyframe = 0;
    bool m_blobTooLarge = false;

    // Rolling measurement, so the UI can report how far back the budget actually reaches
    // for the content being run rather than a guess.
    uint64_t m_deltaSamples = 0;
    uint64_t m_deltaBytesTotal = 0;
    uint64_t m_recordSamples = 0;
    double m_recordMsTotal = 0.0;
};

// Delta codec, exposed for tests. `base` and `cur` must be the same length.
// EncodeDelta appends to `out`; DecodeDelta applies `delta` onto `inout` in place and
// returns false if the stream is malformed or runs past the end of the buffer.
void EncodeDelta(const uint8_t* base, const uint8_t* cur, size_t size, std::vector<uint8_t>& out);
bool DecodeDelta(const uint8_t* delta, size_t deltaSize, uint8_t* inout, size_t size);

} // namespace adm::core
