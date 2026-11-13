#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace adm::core {

/// A point in the recorded execution: which frame, and how many instructions into it.
struct TimePosition
{
    uint64_t frame = 0;
    uint32_t index = 0; // instructions executed since the start of that frame
    uint32_t pc = 0;    // the PC there, when known -- used to detect a divergent replay
};

/**
 * @brief Instruction-granularity reverse execution, built from pieces that already exist.
 *
 * Nothing here rewinds the CPU: the 68000 cannot run backwards. Stepping back means restoring a
 * savestate from before the point of interest and replaying forward to one instruction short of
 * it. Three existing mechanisms make that affordable:
 *
 *   - FrameHistory restores any recorded frame boundary (coarse seek);
 *   - a savestate can be taken at any moment, so a ladder of intra-frame checkpoints turns a
 *     whole-frame replay into a short one;
 *   - UAE's `t <n>` steps N instructions inside a single retro_run() call, so replay costs one
 *     emulator entry rather than N.
 *
 * Once `bs` and `bc` work, GDB provides reverse-step, reverse-next and reverse-finish on top,
 * with full DWARF awareness -- which is the whole reason to implement the low-level primitive
 * rather than a source-level one.
 *
 * **Replay must be verified, not assumed.** Controller input is recorded per frame and replays,
 * but disk DMA and the host-directory filesystem are not part of a savestate's world. If a
 * replay diverges, presenting the resulting machine state as history would be a lie, so every
 * seek checks the PC it lands on against the recorded trace and fails loudly instead.
 */
class TimeTravel
{
public:
    struct Deps
    {
        // Restore a recorded frame boundary. Returns false when that frame is no longer held.
        std::function<bool(uint64_t frame)> restoreFrame;
        // Oldest/newest frame still recorded.
        std::function<bool(uint64_t& oldest, uint64_t& newest)> recordedRange;
        // Run exactly n instructions. Returns false if the target could not be stepped.
        std::function<bool(uint32_t n)> replayInstructions;
        // Current PC, for divergence checks.
        std::function<uint32_t()> currentPc;
        // The PC recorded at (frame, index), if the trace still covers it. Returning false
        // means "unknown", which disables the divergence check rather than failing the seek.
        std::function<bool(uint64_t frame, uint32_t index, uint32_t& outPc)> recordedPc;
        // How many instructions the trace recorded for a frame. Needed as a single call:
        // discovering it by probing recordedPc one index at a time is quadratic, and with a
        // real trace query behind each probe it wedges the emulator rather than merely being
        // slow.
        std::function<bool(uint64_t frame, uint32_t& outCount)> frameInstructionCount;
        // Take/restore an opaque machine snapshot, for the intra-frame checkpoint ladder.
        std::function<std::vector<uint8_t>()> saveState;
        std::function<bool(const std::vector<uint8_t>&)> loadState;
    };

    void SetDeps(Deps deps) { m_deps = std::move(deps); }

    /// How many instructions between intra-frame checkpoints. Smaller is faster to step back
    /// but costs more memory; a PAL frame is ~15k instructions.
    void SetCheckpointStride(uint32_t instructions);
    [[nodiscard]] uint32_t CheckpointStride() const { return m_stride; }

    /// Drop every checkpoint. Call when the machine is reset or a new program is loaded.
    void Clear();

    /**
     * @brief Move to an exact position, restoring and replaying as needed.
     * @return false with a reason in outError; the machine is left at the last good state.
     */
    bool SeekTo(const TimePosition& target, std::string& outError);

    /// One instruction earlier than `io`. Updates `io` on success.
    bool StepBack(TimePosition& io, std::string& outError);

    /**
     * @brief Run backwards until one of `breakpoints` is the PC, or history runs out.
     * @return true if a breakpoint was reached (io updated), false otherwise -- check outError
     *         to tell "no more history" from a genuine failure.
     */
    bool ReverseContinue(const std::vector<uint32_t>& breakpoints, TimePosition& io,
                         std::string& outError);

    struct Reach
    {
        bool available = false;
        uint64_t oldestFrame = 0;
        uint64_t newestFrame = 0;
        std::string hint;
    };
    [[nodiscard]] Reach Reachable() const;

private:
    // A saved machine state at a known position, so a nearby step back is cheap.
    struct Checkpoint
    {
        TimePosition at;
        std::vector<uint8_t> state;
    };

    bool RestoreNearest(const TimePosition& target, TimePosition& landedAt, std::string& outError);
    void RememberCheckpoint(const TimePosition& at);

    Deps m_deps;
    uint32_t m_stride = 1024;
    std::vector<Checkpoint> m_checkpoints; // ascending by position
    size_t m_maxCheckpoints = 64;
};

} // namespace adm::core
