#include "TimeTravel.h"

#include <algorithm>
#include <cstdio>

namespace adm::core {

namespace {

// Ordering on positions: earlier frame first, then earlier instruction within it.
bool Before(const TimePosition& a, const TimePosition& b)
{
    if (a.frame != b.frame)
        return a.frame < b.frame;
    return a.index < b.index;
}

} // namespace

void TimeTravel::SetCheckpointStride(uint32_t instructions)
{
    m_stride = instructions == 0 ? 1u : instructions;
}

void TimeTravel::Clear() { m_checkpoints.clear(); }

TimeTravel::Reach TimeTravel::Reachable() const
{
    Reach reach;
    if (!m_deps.recordedRange || !m_deps.restoreFrame || !m_deps.replayInstructions) {
        reach.hint = "reverse execution needs frame history and instruction stepping";
        return reach;
    }
    if (!m_deps.recordedRange(reach.oldestFrame, reach.newestFrame)) {
        reach.hint = "no frames are recorded yet; enable frame history";
        return reach;
    }
    reach.available = true;
    return reach;
}

void TimeTravel::RememberCheckpoint(const TimePosition& at)
{
    if (!m_deps.saveState)
        return;
    // One per position; re-seeking the same place should not grow the ladder.
    for (const Checkpoint& cp : m_checkpoints) {
        if (cp.at.frame == at.frame && cp.at.index == at.index)
            return;
    }
    Checkpoint cp;
    cp.at = at;
    cp.state = m_deps.saveState();
    if (cp.state.empty())
        return;
    m_checkpoints.push_back(std::move(cp));
    std::sort(m_checkpoints.begin(), m_checkpoints.end(),
              [](const Checkpoint& a, const Checkpoint& b) { return Before(a.at, b.at); });
    // Evict the oldest: stepping back tends to walk backwards from the newest position, so the
    // most recent checkpoints are the ones about to be needed again.
    while (m_checkpoints.size() > m_maxCheckpoints)
        m_checkpoints.erase(m_checkpoints.begin());
}

bool TimeTravel::RestoreNearest(const TimePosition& target, TimePosition& landedAt,
                                std::string& outError)
{
    // Prefer an intra-frame checkpoint at or before the target: replaying from one costs a few
    // hundred instructions instead of a whole frame.
    const Checkpoint* best = nullptr;
    for (const Checkpoint& cp : m_checkpoints) {
        if (Before(cp.at, target) || (cp.at.frame == target.frame && cp.at.index == target.index)) {
            if (!best || Before(best->at, cp.at))
                best = &cp;
        }
    }
    if (best && m_deps.loadState && m_deps.loadState(best->state)) {
        landedAt = best->at;
        return true;
    }

    if (!m_deps.restoreFrame) {
        outError = "no way to restore a frame";
        return false;
    }
    if (!m_deps.restoreFrame(target.frame)) {
        outError = "frame " + std::to_string(target.frame) +
                   " is no longer in the recorded "
                   "history";
        return false;
    }
    landedAt = TimePosition{target.frame, 0, 0};
    return true;
}

bool TimeTravel::SeekTo(const TimePosition& target, std::string& outError)
{
    if (!m_deps.replayInstructions) {
        outError = "no way to replay instructions";
        return false;
    }

    TimePosition at{};
    if (!RestoreNearest(target, at, outError))
        return false;

    if (at.frame != target.frame) {
        outError = "landed in frame " + std::to_string(at.frame) + " while seeking frame " +
                   std::to_string(target.frame);
        return false;
    }
    if (at.index > target.index) {
        outError = "overshot: restored at instruction " + std::to_string(at.index) +
                   " but wanted " + std::to_string(target.index);
        return false;
    }

    // Replay forward, dropping a checkpoint every stride so the next step back is cheap.
    while (at.index < target.index) {
        const uint32_t remaining = target.index - at.index;
        const uint32_t chunk = std::min(remaining, m_stride);
        if (!m_deps.replayInstructions(chunk)) {
            outError = "replay failed after " + std::to_string(at.index) + " instructions";
            return false;
        }
        at.index += chunk;
        if (at.index < target.index)
            RememberCheckpoint(at);
    }

    // Verify rather than assume. A savestate does not carry the host filesystem or in-flight
    // disk DMA, so a replay can legitimately diverge; presenting that as history would be a
    // lie, and a silently wrong machine state is far worse than a refusal.
    if (m_deps.currentPc && m_deps.recordedPc) {
        uint32_t expected = 0;
        if (m_deps.recordedPc(target.frame, target.index, expected)) {
            const uint32_t actual = m_deps.currentPc();
            if (actual != expected) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "replay diverged: expected PC $%08X at frame %llu instruction %u, "
                              "got $%08X. Execution is not reproducible from this savestate.",
                              expected, static_cast<unsigned long long>(target.frame), target.index,
                              actual);
                outError = buf;
                return false;
            }
        }
    }

    RememberCheckpoint(at);
    return true;
}

bool TimeTravel::StepBack(TimePosition& io, std::string& outError)
{
    TimePosition target = io;
    if (target.index > 0) {
        target.index -= 1;
    } else {
        // Crossing a frame boundary backwards: land at the end of the previous frame. The
        // instruction count for it is unknown here, so the caller supplies it via recordedPc
        // probing -- walk back to the last index the trace still knows about.
        if (target.frame == 0) {
            outError = "already at the start of recorded history";
            return false;
        }
        uint64_t oldest = 0, newest = 0;
        if (m_deps.recordedRange && m_deps.recordedRange(oldest, newest) &&
            target.frame - 1 < oldest) {
            outError = "no more reverse history";
            return false;
        }
        target.frame -= 1;
        target.index = 0;
        uint32_t count = 0;
        if (m_deps.frameInstructionCount && m_deps.frameInstructionCount(target.frame, count) &&
            count > 0) {
            target.index = count - 1; // the last instruction of that frame
        } else {
            outError = "the trace does not cover frame " + std::to_string(target.frame) +
                       ", so there is no instruction to step back to";
            return false;
        }
    }

    if (!SeekTo(target, outError))
        return false;
    if (m_deps.currentPc)
        target.pc = m_deps.currentPc();
    io = target;
    return true;
}

bool TimeTravel::ReverseContinue(const std::vector<uint32_t>& breakpoints, TimePosition& io,
                                 std::string& outError)
{
    if (breakpoints.empty()) {
        outError = "no breakpoints to reverse-continue to";
        return false;
    }
    // Bounded so a miss cannot spin forever; the caller reports "no more history" on false.
    constexpr int kMaxSteps = 2'000'000;
    TimePosition cursor = io;
    for (int i = 0; i < kMaxSteps; ++i) {
        if (!StepBack(cursor, outError))
            return false;
        const uint32_t pc = cursor.pc;
        if (std::find(breakpoints.begin(), breakpoints.end(), pc) != breakpoints.end()) {
            io = cursor;
            return true;
        }
    }
    outError = "reverse-continue gave up after " + std::to_string(kMaxSteps) + " instructions";
    return false;
}

} // namespace adm::core
