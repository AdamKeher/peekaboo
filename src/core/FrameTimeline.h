#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "FrameHistory.h"
#include "FrameThumbnails.h"

namespace adm {
class AmigaEmulator;
}

namespace adm::core {

class UserPreferences;

/**
 * @brief Everything FrameTimeline is allowed to do to the machine.
 *
 * A bag of std::function hooks, deliberately shaped like GdbTargetOps / McpHost / ArHost. The
 * timeline lives in core and the transport it needs is on ui::EmulatorView, so routing through
 * hooks is also what keeps core from having to include a view.
 *
 * Stepping goes through the view rather than straight to AmigaEmulator::StepOneFrame() on
 * purpose - the view's own per-frame bookkeeping has to run for a replayed frame exactly as it
 * does for a live one. See FrameTimeline::ReplayRecordedFrame.
 *
 * All hooks are called on the main thread. A null hook means "there is no machine", which the
 * timeline treats the same way as no emulator being loaded.
 */
struct FrameTimelineOps
{
    /// The machine, or null when none is loaded.
    std::function<adm::AmigaEmulator*()> emulator;

    /// View-level transport.
    std::function<bool()> isRunning;
    std::function<bool()> isPaused;
    std::function<void(bool)> setPaused;
    std::function<void()> stepFrame;

    /// Arming policy the timeline cannot decide for itself: whether a human has a debugger
    /// window open, and whether an agent could reach in over MCP. See UpdateSettings().
    std::function<bool()> debuggerOpen;
    std::function<bool()> mcpRunning;

    /// Whether any floppy drive is bridged to a real, physical drive. A physical platter
    /// cannot be rewound, so frame history must not record while one is attached - see
    /// RecordFrame(). Optional; absent means "never".
    std::function<bool()> anyDriveBridged;
};

/**
 * @brief Per-frame save-state timeline: recording, seeking, rewind, A-B loop and playback.
 *
 * Extracted from Application, which had grown this whole mechanism inline. Owns the recorder,
 * the preview thumbnails and the entire transport state; Application keeps only thin forwarders
 * for the menu, console and MCP entry points that already existed.
 *
 * The recorder is armed only while something is looking at it - see UpdateSettings() for why
 * the machine's own state capture is armed on a wider condition than the recording is.
 */
class FrameTimeline
{
public:
    FrameTimeline() = default;

    FrameTimeline(const FrameTimeline&) = delete;
    FrameTimeline& operator=(const FrameTimeline&) = delete;

    /// Wire up the machine hooks and the preferences store. Both outlive this object.
    void Init(FrameTimelineOps ops, UserPreferences* prefs);

    /// Which way the timeline is played back. Ping-pong runs A->B then B->A, which is how you
    /// catch the frame a defect appears on without having to re-arm anything.
    enum class PlayMode { Forward = 0, Reverse = 1, PingPong = 2 };

    struct Playback
    {
        bool playing = false;
        PlayMode mode = PlayMode::Forward;
        double speed = 1.0;
        bool looping = false; // an A-B range is armed and bounds the playback
        uint64_t loopStart = 0;
        uint64_t loopEnd = 0;
    };

    // --- per-frame driving ---

    /// Keeps the recorder's settings in step with the preferences and the debugger's open
    /// state. Called once per host redraw - settings only, never recording.
    void UpdateSettings();

    /// Records one completed frame. Driven by AmigaEmulator's frame-complete hook, so it sees
    /// every frame of a catch-up batch rather than only the last one a redraw happens to
    /// observe. Sampling per redraw is what left holes in the timeline, and a hole is what
    /// strands a seek or stops a playback dead.
    void RecordFrame(uint64_t frame);

    /// Drives timeline playback one frame onward. Called once per drawn frame.
    void AdvancePlayback();

    // --- transport ---

    /// Seek to a recorded frame, taking the transport off play first: a hand-made seek is a
    /// hands-on move. Returns an error string, empty on success.
    std::string SeekTo(uint64_t frame);

    /// Step back `count` frames from wherever the timeline is parked.
    std::string Rewind(uint64_t count);

    /// Move one frame (delta +1 / -1) and stop playback. Error string, empty on success.
    std::string Step(int delta);

    void SetPlaying(bool playing);
    void SetPlayMode(PlayMode mode);
    /// Playback rate as a multiple of the machine's own frame rate. Clamped to 1/8x..2x.
    void SetSpeed(double speed);
    Playback GetPlayback() const;

    /// True while the timeline owns the machine: playback is running or an A-B range is armed,
    /// so the emulator is held paused and its frames come from the recording.
    bool IsDrivingMachine() const { return m_playing || m_loopEnd != 0; }

    /// Hand the machine back to live running: stops playback and disarms any A-B range. Every
    /// path that resumes, steps or otherwise runs the machine has to call this first, or the
    /// transport keeps replaying frames into a machine that is running underneath it. No-op
    /// when the timeline is not driving. Does not itself unpause the emulator.
    void ReleaseMachine();

    /// Puts the recorded state back under the machine before it runs on its own again.
    void HandBackRecordedFrame();

    // --- A-B loop ---

    /// Replay a recorded frame range on repeat (ENGINE9000's `loop`). Pauses live running for
    /// the duration. Returns an error string, empty on success.
    std::string SetLoop(uint64_t a, uint64_t b);
    void ClearLoop();
    /// The armed loop range. False when no loop is running.
    bool GetLoop(uint64_t& a, uint64_t& b) const;

    /// Throw the recorded timeline away: states, previews, loop and playback all go. The
    /// recorder stays armed and starts filling again from the next frame, which is the point -
    /// it is how you drop everything before the run you actually care about.
    void Clear();

    // --- settings ---

    /// Apply the Frame History menu's settings. Persisted through UserPreferences.
    void ApplySettings(bool enabled, int budgetMB, bool persistInSession, bool thumbnails,
                       int thumbnailInterval);

    // --- queries ---

    /// The frame the timeline is parked on, or the newest recorded frame when live.
    uint64_t Position() const { return m_position; }
    /// Set by session restore, which puts a whole recorded timeline back at once.
    void SetPosition(uint64_t frame) { m_position = frame; }

    FrameHistoryStats Stats() const { return m_history.Stats(); }
    FrameThumbnailStats ThumbnailStats() const { return m_thumbs.Stats(); }

    /// Direct access to the recorder, for the session serializer and the range queries that
    /// are already FrameHistory's own API. Handed out by reference deliberately: FrameHistory
    /// is a self-contained component, not loose state this class is hiding.
    FrameHistory& History() { return m_history; }
    const FrameHistory& History() const { return m_history; }
    FrameThumbnails& Thumbnails() { return m_thumbs; }

    /// Set when playback stops itself (the timeline ran out, a replay could not be made).
    /// Surfaced once by the next Draw so a stall says why instead of just stopping.
    const std::string& StopReason() const { return m_stopReason; }
    void ClearStopReason() { m_stopReason.clear(); }

    /// True while a replayed step is in flight. The recorder must not treat a restored frame
    /// as newly run - doing so trims the recorded future and re-records it, so stepping back
    /// one frame would destroy everything ahead of it.
    bool IsReplayInProgress() const { return m_replayInProgress; }

    /// The machine no longer holds a re-run frame - it ran this one live.
    void ClearMachineHoldsReplayedFrame() { m_machineHoldsReplayedFrame = false; }

    /**
     * @brief Screen thumbnail for a recorded frame, for the scrub preview.
     *
     * Thumbnails are kept every N frames rather than every frame, so this answers with the
     * nearest shot at or before `frame` and reports which frame it is really from in
     * `gotFrame` - the preview must not imply an exactness it does not have. `rgb` comes back
     * top-row-first RGB888. False when nothing has been captured.
     */
    bool GetThumbnail(uint64_t frame, std::vector<uint8_t>& rgb, int& w, int& h,
                      uint64_t& gotFrame) const;

    /**
     * @brief Ranges of emulated RAM that differ between two recorded frames.
     *
     * ENGINE9000's `diff`. Answers "what did those frames actually touch?" without having to
     * guess an address first. Runs are reported against the live machine's bank layout, so
     * they are Amiga addresses, not save-state offsets.
     *
     * Returns an error string, empty on success. `out` is capped at `maxRuns`.
     */
    std::string DiffFrames(uint64_t a, uint64_t b, std::vector<FrameDiffRun>& out, size_t maxRuns,
                           bool& truncated) const;

private:
    /// The seek itself. The public SeekTo() takes the transport off play first; playback's own
    /// wraps and reverse steps come through here so they do not stop the playback that asked.
    std::string SeekInternal(uint64_t frame);

    /// Re-execute `frame` from its recorded input, assuming the machine already holds the state
    /// that preceded it. Sets the frame counter so the re-run lands on `frame`. Returns an
    /// error string (empty on success): the step is a no-op on an unpaused machine, and a
    /// breakpoint can stop it part-way, so the caller has to know.
    std::string ReplayRecordedFrame(uint64_t frame);

    /// The frames playback may move between: the armed loop when there is one, the whole
    /// recorded range otherwise. False when the timeline holds nothing to play.
    bool Bounds(uint64_t& lo, uint64_t& hi) const;

    /// One frame in `delta`'s direction, wrapping or bouncing at the bounds per the play mode.
    /// `delta` is the direction to travel along the timeline, not a play-mode-relative one:
    /// +1 is always later, -1 always earlier. Returns an error string, empty on success.
    std::string Move(int delta);

    /// Where playback goes next, given the play mode and which leg a ping-pong is on. Returns
    /// false when playback should stop (nothing left in that direction).
    bool NextPlaybackFrame(uint64_t lo, uint64_t hi, uint64_t pos, uint64_t& out);

    /// The machine, or null when nothing is loaded.
    adm::AmigaEmulator* Machine() const;
    /// A machine that is loaded *and* running.
    bool RunningMachine() const;
    void SetPaused(bool paused);
    bool DebuggerOpen() const;
    bool McpRunning() const;

    FrameTimelineOps m_ops;
    UserPreferences* m_prefs = nullptr;

    /// Rolling per-frame save-state timeline behind rewind / frame seeking. Armed only while a
    /// debugger window is open - see UpdateSettings().
    FrameHistory m_history;
    /// Periodic screen thumbnails alongside the timeline, so scrubbing shows where in the
    /// program you are. Follows the timeline's own trimming and eviction.
    FrameThumbnails m_thumbs;
    /// Last thumbnail sequence pulled from the emulator; a change means a new capture.
    uint64_t m_lastThumbSeq = 0;
    /// Reused so a capture does not allocate every interval.
    std::vector<uint8_t> m_thumbScratch;
    /// Frame the timeline is currently parked on. 0 means "live", i.e. following the newest
    /// recorded frame rather than sitting somewhere behind it.
    uint64_t m_position = 0;
    /// Reconstructed save state handed to the core by a seek. Held rather than allocated per
    /// call because playback restores one every frame and a save state is megabytes.
    std::vector<uint8_t> m_seekStateScratch;
    /// True while the machine holds a frame the timeline *re-ran* rather than the recorded
    /// state for that frame. Those differ, and only the recording is a machine worth
    /// continuing from. See HandBackRecordedFrame().
    bool m_machineHoldsReplayedFrame = false;

    /// Active LOOP range; 0 end means no loop. While armed the emulator is paused and the
    /// range is replayed from the timeline rather than run live.
    uint64_t m_loopStart = 0;
    uint64_t m_loopEnd = 0;
    /// Wall-clock pacing for the replay. A hand-stepped frame bypasses the emulator's own frame
    /// timing, so without this the loop plays back at whatever rate the UI redraws at.
    std::chrono::steady_clock::time_point m_loopLastTick{};
    double m_loopAccum = 0.0;

    /// Transport state. Playback is what drives the machine while it is on - the emulator stays
    /// paused and the frames come from the timeline.
    bool m_playing = false;
    PlayMode m_mode = PlayMode::Forward;
    double m_speed = 1.0;             // 1/8x .. 2x of the machine's frame rate
    bool m_pingPongBackwards = false; // which leg a ping-pong playback is on
    /// Held for the whole of a replayed step. Scoped rather than a one-shot flag: a replay can
    /// complete several core runs (a filtered breakpoint mid-frame), and a flag consumed by the
    /// first of them leaves the rest recording over the timeline.
    bool m_replayInProgress = false;
    std::string m_stopReason;
};

} // namespace adm::core
