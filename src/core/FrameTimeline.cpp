#include "FrameTimeline.h"

#include <algorithm>
#include <cmath>

#include "AmigaEmulator.h"
#include "Logger.h"
#include "UaeSavestate.h"
#include "UserPreferences.h"

namespace adm::core {

void FrameTimeline::Init(FrameTimelineOps ops, UserPreferences* prefs)
{
    m_ops = std::move(ops);
    m_prefs = prefs;
}

// The machine, or null when nothing is loaded. Every path here tolerates null: the timeline
// outlives any particular emulator instance.
adm::AmigaEmulator* FrameTimeline::Machine() const
{
    return m_ops.emulator ? m_ops.emulator() : nullptr;
}

// A machine that is loaded *and* running. Seeking into a stopped emulator is not an error
// worth reporting, it is just nothing to do.
bool FrameTimeline::RunningMachine() const
{
    return m_ops.isRunning && m_ops.isRunning() && Machine() != nullptr;
}

void FrameTimeline::SetPaused(bool paused)
{
    if (m_ops.setPaused)
        m_ops.setPaused(paused);
}

bool FrameTimeline::DebuggerOpen() const { return m_ops.debuggerOpen && m_ops.debuggerOpen(); }

bool FrameTimeline::McpRunning() const { return m_ops.mcpRunning && m_ops.mcpRunning(); }

void FrameTimeline::ApplySettings(bool enabled, int budgetMB, bool persistInSession,
                                  bool thumbnails, int thumbnailInterval)
{
    m_prefs->SetFrameHistoryEnabled(enabled);
    m_prefs->SetFrameHistoryBudgetMB(budgetMB);
    m_prefs->SetFrameHistoryPersistInSession(persistInSession);
    m_prefs->SetFrameHistoryThumbnails(thumbnails);
    m_prefs->SetFrameHistoryThumbnailInterval(thumbnailInterval);
    if (!thumbnails)
        m_thumbs.Clear();
    // UpdateFrameHistory pushes these into the recorder on the next emulated frame; a
    // shrunken budget takes effect there rather than being applied twice.
}

void FrameTimeline::UpdateSettings()
{
    auto* emu = Machine();
    if (!emu)
        return;

    const auto& prefs = m_prefs->Get();
    // Recording a save state every frame is only worth paying for while something is
    // looking at it, so it follows the debugger rather than running for the whole session.
    const bool arm = prefs.frameHistoryEnabled && DebuggerOpen();

    // The core's vpos-0 state capture, armed on the same principle but a wider condition.
    //
    // It cannot be armed on demand. A restorable state only exists at a frame boundary the
    // core has already gone past, so whoever asks for one has to have been paying for it
    // beforehand; arming at the moment of the ask hands back a mid-field state for the first
    // one, which is the exact failure this whole mechanism exists to prevent. So it is armed
    // whenever an ask is plausible rather than only when one has happened:
    //
    //   - a debugger window is open, i.e. there is a human here who can save a state. This
    //     also covers the timeline recording, which asks once per frame: `arm` above cannot
    //     be true unless a debugger window is open, so it adds no term of its own;
    //   - the MCP server is up, i.e. an agent can call save_state at any moment without
    //     any UI being open at all - this is how the emulator is usually driven under test;
    //   - something has already asked once, which is the best predictor that it will again.
    //
    // What is left paying nothing is a plain run with no debugger, no timeline and nobody able
    // to reach in.
    emu->SetStateCaptureEnabled(DebuggerOpen() || McpRunning() ||
                                emu->RestorableStateWasRequested());

    if (arm != m_history.IsEnabled()) {
        m_history.SetEnabled(arm);
        emu->SetInputCaptureEnabled(arm);
        m_position = 0;
        if (!arm)
            m_thumbs.Clear();
    }
    // Kept in step with arming so the capture stops the moment the timeline does; a
    // thumbnail nobody can seek to is pure cost.
    emu->SetThumbnailCapture(arm && prefs.frameHistoryThumbnails,
                             prefs.frameHistoryThumbnailInterval);
    if (!arm)
        return;

    m_history.SetBudgetBytes(static_cast<size_t>(std::max(1, prefs.frameHistoryBudgetMB)) * 1024 *
                             1024);
    m_history.SetKeyframeInterval(prefs.frameHistoryKeyframeInterval);
    // A slice of the same budget rather than a separate knob: previews are worth a few
    // percent of what the states cost, and never more than the timeline they annotate.
    m_thumbs.SetBudgetBytes(
        std::min(core::FrameThumbnails::kDefaultBudgetBytes,
                 static_cast<size_t>(std::max(1, prefs.frameHistoryBudgetMB)) * 1024 * 1024 / 8));
}

// One completed emulated frame. Driven by AmigaEmulator's frame-complete hook rather than
// sampled once per redraw: Update() runs a catch-up batch of up to a dozen frames when the
// host misses a beat, and sampling caught only the last of them. Every frame the sampling
// missed was a hole in the timeline, and a hole is what makes a seek land on a frame that
// cannot be reconstructed and a playback stop dead in the middle of a range.
void FrameTimeline::RecordFrame(uint64_t frame)
{
    if (!m_history.IsEnabled())
        return;
    auto* emu = Machine();
    if (!emu)
        return;

    // A frame produced by a replay is one the timeline already holds. Recording it would
    // trim the recorded future and put back an identical entry, so a step backwards would
    // silently throw away everything ahead of it.
    if (m_replayInProgress)
        return;
    // Only whole frames go in. A mid-frame breakpoint return is not a state the timeline
    // can seek to, and recording it would put an entry at a frame boundary that is not one.
    if (emu->IsPartialFrame())
        return;
    // Playback replays frames the timeline already holds, for as long as it runs.
    if (m_playing)
        return;

    uint64_t oldest = 0, newest = 0;
    if (m_history.Range(oldest, newest) && frame <= newest) {
        // Play resumed from behind the newest recorded frame: the recorded future is now
        // a different future, so it goes, exactly as ENGINE9000 trims on seek-release.
        m_history.TrimAfter(frame > 0 ? frame - 1 : 0);
        m_thumbs.TrimAfter(frame > 0 ? frame - 1 : 0);
    }

    // Frame history exists to be rewound into, and a physical platter cannot be rewound:
    // restoring an earlier frame would leave the emulated machine expecting a head
    // position and disk contents the real drive no longer has. Stop recording rather than
    // build a timeline that cannot be replayed.
    if (m_ops.anyDriveBridged && m_ops.anyDriveBridged())
        return;

    std::vector<uint8_t> blob = emu->SaveStateForRestore();
    if (blob.empty())
        return;
    m_history.Record(frame, blob, emu->LastFrameInput());
    m_position = frame;
    // The machine ran this frame live, so it is no longer holding a re-run to hand back.
    m_machineHoldsReplayedFrame = false;

    // The emulator samples a thumbnail on its own cadence; pair whatever it produced with
    // the frame that just completed, then keep the previews inside the timeline's own
    // range so a scrub can never land on a picture whose frame has already been evicted.
    const uint64_t thumbSeq = emu->GetThumbnailSeq();
    if (thumbSeq != m_lastThumbSeq) {
        m_lastThumbSeq = thumbSeq;
        int tw = 0, th = 0;
        if (emu->CopyThumbnail(m_thumbScratch, tw, th))
            m_thumbs.Record(frame, m_thumbScratch.data(), tw, th);
    }
    if (m_history.Range(oldest, newest))
        m_thumbs.PruneBefore(oldest);
}

bool FrameTimeline::GetThumbnail(uint64_t frame, std::vector<uint8_t>& rgb, int& w, int& h,
                                 uint64_t& gotFrame) const
{
    return m_thumbs.GetNearest(frame, rgb, w, h, gotFrame);
}

// A seek asked for from outside - the bar, GOTO, goto_frame, a rewind - takes the machine
// off playback. Playback's own moves go straight to SeekInternal(), or every
// wrap and every reverse step would stop the thing that asked for it.
std::string FrameTimeline::SeekTo(uint64_t frame)
{
    SetPlaying(false);
    m_stopReason.clear();
    return SeekInternal(frame);
}

std::string FrameTimeline::SeekInternal(uint64_t frame)
{
    if (!RunningMachine())
        return "emulator not running";
    auto* emu = Machine();
    if (!emu)
        return "emulator not available";

    uint64_t oldest = 0, newest = 0;
    if (!m_history.Range(oldest, newest))
        return "nothing recorded yet - open a debugger window and let the machine run";
    // Re-running a frame needs the state that preceded it, so the oldest frame held can be
    // a base but never a destination.
    uint64_t first = 0;
    if (!m_history.FirstSeekable(first))
        return "only one frame is recorded - there is nothing to seek to yet";
    if (frame < first || frame > newest)
        return fmt::format("frame {} is outside the seekable range {}-{}", frame, first, newest);

    // Land on a frame that exists. Frame numbers are not always consecutive - a mid-frame
    // breakpoint stop never completes a frame under its number - and re-running a frame
    // that was never recorded would run it with no input at all and call the result that
    // frame, which is worse than landing a frame or two away and saying so.
    uint64_t target = frame;
    if (!m_history.Has(target)) {
        uint64_t nearest = 0;
        if (!m_history.NearestFrame(frame, nearest))
            return fmt::format("frame {} is not recorded", frame);
        target = std::clamp(nearest, first, newest);
    }

    // The state to restore is the entry before the target on the timeline, which is not
    // necessarily target-1.
    uint64_t base = 0;
    if (!m_history.PreviousFrame(target, base))
        return fmt::format("frame {} has no recorded frame before it to re-run from", target);

    // Into a member buffer rather than a fresh vector: playback restores a state per frame,
    // and a save state is megabytes, so allocating one per frame is a per-frame malloc of
    // the whole machine. ReconstructInto also keeps a cursor, which turns the common case -
    // the entry straight after the last one restored - into a single delta decode.
    if (!m_history.ReconstructInto(base, m_seekStateScratch) || m_seekStateScratch.empty())
        return fmt::format("frame {} could not be reconstructed", base);

    // Pause first: an unpaused machine would run straight past the frame we just went to
    // the trouble of restoring.
    SetPaused(true);

    if (!emu->LoadState(m_seekStateScratch))
        return "the core rejected the restored state";
    // The machine holds `base` from here on. If the re-run below fails, the playhead has to
    // say so rather than claim the frame that was asked for.
    m_position = base;

    return ReplayRecordedFrame(target);
}

std::string FrameTimeline::ReplayRecordedFrame(uint64_t frame)
{
    auto* emu = Machine();
    if (!emu)
        return "emulator not available";

    core::FrameInput input;
    if (!m_history.GetInput(frame, input))
        return fmt::format("frame {} is not recorded", frame);

    // StepFrame() is a silent no-op on a running machine. Left unchecked that advances the
    // playhead over a machine that never moved, and everything after it - the picture, the
    // telemetry, the next frame replayed on top - describes a different moment.
    if (m_ops.isPaused && !m_ops.isPaused())
        SetPaused(true);

    // The save state does not carry the host-side frame counter, so re-base it: the re-run
    // has to land on `frame`, not on wherever the machine had got to.
    emu->SetTotalFrameCount(frame > 0 ? frame - 1 : 0);

    // Held for the whole step rather than consumed by the first frame it produces: a
    // filtered breakpoint can make StepFrame() complete several core runs, and every one
    // after the first would otherwise be recorded on top of the timeline being replayed.
    m_replayInProgress = true;
    emu->BeginInputReplay();
    emu->SetReplayInput(input);
    // Goes through the view's frame step rather than emu->StepOneFrame() so a replayed
    // frame ends exactly where a manual Step Frame would: that path drains log-mode
    // watchpoint and filtered-breakpoint stops and carries the frame to its boundary,
    // which a raw StepOneFrame would leave truncated.
    if (m_ops.stepFrame)
        m_ops.stepFrame();
    emu->EndInputReplay();
    m_replayInProgress = false;

    // Verify rather than assume. Forward playback deliberately re-runs the next frame on
    // top of whatever the last one left behind, so a miss here does not show up as one bad
    // frame - it rebases every frame after it, silently.
    // Whatever happened, the machine now holds a frame it re-ran rather than the recorded
    // state itself, so anything that resumes from here has to be handed the recording back.
    m_machineHoldsReplayedFrame = true;

    const uint64_t landed = emu->GetTotalFrameCount();
    if (landed != frame || emu->IsPartialFrame()) {
        m_position = landed;
        return fmt::format("frame {} did not re-run cleanly - the machine stopped at {}{}", frame,
                           landed, emu->IsPartialFrame() ? " part-way through a frame" : "");
    }

    m_position = frame;
    return {};
}

std::string FrameTimeline::SetLoop(uint64_t a, uint64_t b)
{
    if (a == 0 && b == 0) {
        ClearLoop();
        return {};
    }
    if (a > b)
        std::swap(a, b);

    uint64_t oldest = 0, newest = 0;
    if (!m_history.Range(oldest, newest))
        return "nothing recorded yet - open a debugger window and let the machine run";
    uint64_t first = 0;
    if (!m_history.FirstSeekable(first))
        return "only one frame is recorded - there is nothing to loop over yet";
    // Both ends must be replayable, and the first frame of a loop needs a base to run from.
    if (a < first || b > newest) {
        return fmt::format("loop range {}-{} is outside the seekable range {}-{}", a, b, first,
                           newest);
    }
    // Snap the ends onto frames that were actually recorded, the same way a seek does.
    // Frames inside the range may be missing without any of this mattering: playback walks
    // the timeline entry by entry, so a hole is stepped over rather than ending the lap.
    uint64_t sa = a, sb = b;
    if ((!m_history.Has(a) && !m_history.NearestFrame(a, sa)) ||
        (!m_history.Has(b) && !m_history.NearestFrame(b, sb))) {
        return fmt::format("loop range {}-{} does not line up with any recorded frame", a, b);
    }
    sa = std::clamp(sa, first, newest);
    sb = std::clamp(sb, first, newest);
    if (sa > sb)
        std::swap(sa, sb);

    // Arming has side effects - it pauses the machine and starts playing - so they have to
    // come back off if the range turns out not to be reachable. A loop left "running" over
    // a range it never got to replays forward from wherever the machine happened to be.
    const uint64_t prevStart = m_loopStart;
    const uint64_t prevEnd = m_loopEnd;
    const bool prevPlaying = m_playing;

    m_loopStart = sa;
    m_loopEnd = sb;
    m_loopLastTick = {};
    m_loopAccum = 0.0;
    m_pingPongBackwards = false;
    m_stopReason.clear();
    // Arming a range is a request to watch it, so it starts playing.
    m_playing = true;
    // A loop replays the timeline; letting the machine also run live would fight it.
    SetPaused(true);

    const std::string err = SeekInternal(sa);
    if (!err.empty()) {
        m_loopStart = prevStart;
        m_loopEnd = prevEnd;
        m_playing = prevPlaying;
        return err;
    }
    return {};
}

void FrameTimeline::Clear()
{
    ClearLoop();
    m_machineHoldsReplayedFrame = false;
    m_history.Clear();
    m_thumbs.Clear();
    m_position = 0;
    // Nothing is parked on a recorded frame any more, so the next frame the machine runs is
    // an ordinary live one rather than something the recorder should skip.
    m_replayInProgress = false;
}

void FrameTimeline::ClearLoop()
{
    m_loopStart = 0;
    m_loopEnd = 0;
    m_loopLastTick = {};
    m_loopAccum = 0.0;
    m_pingPongBackwards = false;
    m_stopReason.clear();
    // Clearing the range stops the replay and leaves the machine parked where it stands;
    // resuming live running is the toolbar's job, not something to do behind the user.
    m_playing = false;
}

// The single way to hand the machine back to live running. Every path that runs the machine
// - resume, step, run-until, reset - goes through this first, or the transport carries on
// replaying recorded frames into a machine that is running underneath it: the picture comes
// from one timeline and the playhead and telemetry from another.
void FrameTimeline::ReleaseMachine()
{
    // Before the loop is disarmed, not after: the handback needs to know the machine is
    // still parked on a replayed frame. It is deliberately outside the guard below, because
    // a plain seek leaves the machine on a re-run too without ever arming a loop.
    HandBackRecordedFrame();
    if (!IsDrivingMachine())
        return;
    ClearLoop();
}

// Put the recorded state back under the machine before it runs on its own again.
//
// What playback leaves behind is not the frame it is showing: it is that frame *re-run* on
// top of a restored predecessor. Because PUAE's save state carries no raster position (see
// Move()), a re-run covers a slightly different window of the frame than the live run
// did, so the machine ends up holding registers and RAM that never coexisted - roughly a
// frame's worth of divergence, plus a beam phase that does not match its own chipset state.
// Watching that is fine; it is one frame off and invisible. Continuing from it is not: press
// Play in the middle of an A-B loop and the demo runs on from a machine that is subtly
// self-inconsistent, which is what made the picture break up and the demo fail to carry on.
//
// The recorded state for the same frame is the best machine available - it was captured live,
// and its registers and RAM were captured together. Resuming from it is then an ordinary
// "load a save state and press play", which is the path PUAE is built for.
void FrameTimeline::HandBackRecordedFrame()
{
    if (!m_machineHoldsReplayedFrame)
        return;
    // One attempt only. If it cannot be done the machine keeps the re-run, which is what it
    // would have had anyway - never a reason to block the user from resuming.
    m_machineHoldsReplayedFrame = false;

    if (!RunningMachine())
        return;
    auto* emu = Machine();
    if (!emu)
        return;
    const uint64_t frame = m_position;
    if (!m_history.Has(frame))
        return;
    if (!m_history.ReconstructInto(frame, m_seekStateScratch) || m_seekStateScratch.empty()) {
        Logger::Warn("Timeline handback: frame {} could not be reconstructed - resuming from the "
                     "re-run instead",
                     frame);
        return;
    }
    if (!emu->LoadState(m_seekStateScratch)) {
        Logger::Warn("Timeline handback: the core rejected the recorded state for frame {} - "
                     "resuming from the re-run instead",
                     frame);
        return;
    }
    // The save state does not carry the host-side frame counter, so re-base it or every
    // frame-keyed consumer drifts from here on.
    emu->SetTotalFrameCount(frame);
    Logger::Info("Timeline handback: machine resumed from recorded frame {}", frame);
}

bool FrameTimeline::GetLoop(uint64_t& a, uint64_t& b) const
{
    if (!m_loopEnd)
        return false;
    a = m_loopStart;
    b = m_loopEnd;
    return true;
}

bool FrameTimeline::Bounds(uint64_t& lo, uint64_t& hi) const
{
    uint64_t oldest = 0, newest = 0, first = 0;
    // The oldest frame held is a base to replay from, never a destination.
    if (!m_history.Range(oldest, newest) || !m_history.FirstSeekable(first))
        return false;
    if (m_loopEnd) {
        // Eviction retires the front of the timeline while a long loop runs, so an armed
        // range can outlive the frames it names. Clamp rather than hand playback bounds it
        // cannot reach.
        lo = std::max(m_loopStart, first);
        hi = std::min(m_loopEnd, newest);
        return lo <= hi;
    }
    lo = first;
    hi = newest;
    return lo <= hi;
}

// One frame along the timeline. `delta` is a direction in time, not in play order: +1 is
// always later and -1 always earlier, so the transport's step buttons keep meaning what they
// say while a reverse or ping-pong playback is running. The play mode only decides what
// happens at the ends - wrap, or turn around.
std::string FrameTimeline::Move(int delta)
{
    uint64_t lo = 0, hi = 0;
    if (!Bounds(lo, hi))
        return "nothing recorded yet - open a debugger window and let the machine run";

    // Eviction can move the floor out from under a playback that has been running a while,
    // and a re-run that did not land can leave the playhead on a frame never recorded.
    uint64_t pos = std::clamp(m_position, lo, hi);
    if (!m_history.Has(pos)) {
        uint64_t snapped = 0;
        if (!m_history.NearestFrame(pos, snapped))
            return "the timeline no longer holds the frame playback was on";
        pos = std::clamp(snapped, lo, hi);
    }

    // Timeline adjacency, not frame-number adjacency: a missing frame number is stepped over
    // rather than treated as the end of the recording.
    uint64_t target = pos;
    uint64_t neighbour = 0;
    // Set only by the plain "step to the very next recorded frame" branch below - never by
    // a wrap or a ping-pong turnaround. Gates the fast path further down: those other moves
    // land somewhere the live machine is not already sitting, so they still need a restore.
    bool advancingToNextFrame = false;
    if (delta >= 0) {
        if (pos < hi && m_history.NextFrame(pos, neighbour) && neighbour <= hi) {
            target = neighbour;
            advancingToNextFrame = true;
        } else if (m_mode == PlayMode::PingPong) {
            // Turn around rather than wrap: crossing the same boundary in both directions is
            // how you find the frame a defect appears on.
            m_pingPongBackwards = true;
            target = (m_history.PreviousFrame(pos, neighbour) && neighbour >= lo) ? neighbour : lo;
        } else {
            target = lo;
        }
    } else {
        if (pos > lo && m_history.PreviousFrame(pos, neighbour) && neighbour >= lo) {
            target = neighbour;
        } else if (m_mode == PlayMode::PingPong) {
            m_pingPongBackwards = false;
            target = (m_history.NextFrame(pos, neighbour) && neighbour <= hi) ? neighbour : hi;
        } else {
            target = hi;
        }
    }

    // Historical note on why every move used to restore first, including a plain step
    // forward onto the entry the machine is already sitting on: PUAE's save state does not
    // carry the raster position, so a restore used to resume as if at the top of a field
    // while the states here are captured a few lines into the next one.
    // A chained re-run therefore covered a
    // slightly different window than the live run and the error compounded - measured on
    // Enchanted Glitch / Cosmic Orbs as ~220 bytes/frame, unrecognisable over a 350-frame
    // A-B lap. That is why §2.1 made every move restore, at a measured cost of ~40 ms
    // (retro_unserialize) versus ~8 ms for a bare re-run - roughly half speed, and the whole
    // reason playback topped out around 17 fps.
    //
    // Core 1.33.0 (§4.4.9) fixed the underlying defect: capture is now taken at a genuine
    // CPU instruction boundary, and a restore round trip is byte-exact. That retires the
    // compounding-error argument for the one case where "restore" was never really a round
    // trip to begin with - a plain forward step continues the machine that is already
    // running, rather than reloading and re-deriving it. So when the live machine can be
    // proven (not assumed - checked against its own frame counter, the same discipline
    // ReplayRecordedFrame uses below) to already be sitting on `pos`, step straight to the
    // next frame without paying for a restore that would just reproduce the state that is
    // already loaded.
    if (advancingToNextFrame && pos == m_position && m_machineHoldsReplayedFrame) {
        auto* emu = RunningMachine() ? Machine() : nullptr;
        if (emu && emu->GetTotalFrameCount() == pos && !emu->IsPartialFrame())
            return ReplayRecordedFrame(target);
        // Anything unproven falls through to the restoring path below rather than risking a
        // frame built on a machine state that turned out not to be what was expected.
    }

    // Every other move restores the recorded state first: a seek, a wrap, a ping-pong
    // turnaround or a reverse step all land the machine somewhere it is not already sitting,
    // so there is nothing to continue from and a restore is the only way to get there.
    return SeekInternal(target);
}

void FrameTimeline::AdvancePlayback()
{
    if (!m_playing || !RunningMachine())
        return;
    auto* emu = Machine();
    if (!emu)
        return;
    uint64_t lo = 0, hi = 0;
    if (!Bounds(lo, hi)) {
        m_playing = false;
        m_stopReason = "the recorded timeline no longer holds anything to play";
        Logger::Info("Timeline playback stopped: {}", m_stopReason);
        return;
    }

    // A replayed frame is stepped by hand, so none of AmigaEmulator::Update()'s wall-clock
    // pacing applies to it: left alone, playback advances once per UI redraw and runs at the
    // host's refresh rate rather than the machine's. Pace it against the emulated frame rate
    // times the transport's own speed. The emulator's speed multiplier deliberately does not
    // come into it - turbo is about running live, and while the timeline drives the machine
    // the machine is paused.
    double fps = emu->GetTargetFps();
    if (!(fps > 1.0))
        fps = 50.0;
    const double frameDuration = 1.0 / (fps * (m_speed > 0.0 ? m_speed : 1.0));

    const auto now = std::chrono::steady_clock::now();
    if (m_loopLastTick.time_since_epoch().count() == 0)
        m_loopLastTick = now;
    m_loopAccum += std::chrono::duration<double>(now - m_loopLastTick).count();
    m_loopLastTick = now;
    // At most one replayed frame per redraw - a replay costs a state restore plus a full
    // frame re-run, so working off a backlog would only fall further behind. A host that
    // cannot keep up plays back slow; it never skips frames, because the frame it skipped
    // would be the one worth seeing.
    if (m_loopAccum > 2.0 * frameDuration)
        m_loopAccum = 2.0 * frameDuration;
    if (m_loopAccum < frameDuration)
        return;
    m_loopAccum -= frameDuration;

    // Which way playback is travelling. The step buttons pass their own direction; this is
    // the only place the play mode turns into one.
    const bool backwards =
        m_mode == PlayMode::Reverse || (m_mode == PlayMode::PingPong && m_pingPongBackwards);
    const std::string err = Move(backwards ? -1 : 1);
    if (!err.empty()) {
        // Stop rather than retry every redraw: whatever failed will fail again, and a
        // transport that says it is playing while standing still is the worst of both. The
        // reason is surfaced instead of dropped, because a silent stall is what makes this
        // feel haunted rather than broken.
        m_playing = false;
        m_stopReason = err;
        Logger::Info("Timeline playback stopped: {}", err);
    }
}

void FrameTimeline::SetPlaying(bool playing)
{
    if (playing == m_playing)
        return;
    m_playing = playing;
    // Start the pacing clock fresh, or the time spent paused would be banked as a burst.
    m_loopLastTick = {};
    m_loopAccum = 0.0;
    if (playing) {
        m_stopReason.clear();
        // Playback drives the machine from the timeline, so live running has to stop.
        SetPaused(true);
    }
}

void FrameTimeline::SetPlayMode(PlayMode mode)
{
    m_mode = mode;
    m_pingPongBackwards = false;
}

void FrameTimeline::SetSpeed(double speed)
{
    m_speed = std::clamp(speed, 0.125, 2.0);
    // Re-pace from now: a slot measured against the old speed means nothing under the new one.
    m_loopLastTick = {};
    m_loopAccum = 0.0;
}

std::string FrameTimeline::Step(int delta)
{
    if (delta == 0)
        return {};
    if (!RunningMachine())
        return "emulator not running";
    uint64_t lo = 0, hi = 0;
    if (!Bounds(lo, hi))
        return "nothing recorded yet - open a debugger window and let the machine run";

    // Stepping is a hands-on move, so it takes the transport off play rather than fighting
    // it. The armed A-B range stays armed - stepping through a loop is a normal thing to
    // want to do.
    SetPlaying(false);
    m_stopReason.clear();
    SetPaused(true);
    return Move(delta > 0 ? 1 : -1);
}

FrameTimeline::Playback FrameTimeline::GetPlayback() const
{
    Playback p;
    p.playing = m_playing;
    p.mode = m_mode;
    p.speed = m_speed;
    p.looping = m_loopEnd != 0;
    p.loopStart = m_loopStart;
    p.loopEnd = m_loopEnd;
    return p;
}

std::string FrameTimeline::Rewind(uint64_t count)
{
    uint64_t oldest = 0, newest = 0;
    if (!m_history.Range(oldest, newest))
        return "nothing recorded yet - open a debugger window and let the machine run";
    // The oldest frame held is a base to re-run from, never a destination.
    uint64_t floor = 0;
    if (!m_history.FirstSeekable(floor))
        return "only one frame is recorded - there is nothing to step back to yet";

    uint64_t from = std::min(m_position ? m_position : newest, newest);
    if (!m_history.Has(from)) {
        uint64_t snapped = 0;
        if (!m_history.NearestFrame(from, snapped))
            return "nothing recorded yet - open a debugger window and let the machine run";
        from = std::clamp(snapped, floor, newest);
    }
    if (from <= floor)
        return fmt::format("already at the oldest recorded frame ({})", floor);

    // Walk entries rather than subtracting frame numbers: a frame number that was never
    // recorded is not a frame you stepped over, and counting it lands the rewind short.
    uint64_t target = from;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t prev = 0;
        if (!m_history.PreviousFrame(target, prev) || prev < floor)
            break;
        target = prev;
    }
    if (target == from)
        return fmt::format("already at the oldest recorded frame ({})", floor);

    // The public seek takes the transport off play; any armed A-B range stays armed, since
    // stepping back through a loop is a normal thing to want to do.
    return SeekTo(target);
}

std::string FrameTimeline::DiffFrames(uint64_t a, uint64_t b, std::vector<core::FrameDiffRun>& out,
                                      size_t maxRuns, bool& truncated) const
{
    out.clear();
    truncated = false;

    if (!m_history.Has(a) || !m_history.Has(b)) {
        uint64_t oldest = 0, newest = 0;
        if (!m_history.Range(oldest, newest))
            return "nothing recorded yet - open a debugger window and let the machine run";
        return fmt::format("both frames must be recorded (held range is {}-{})", oldest, newest);
    }

    const std::vector<uint8_t> blobA = m_history.Reconstruct(a);
    const std::vector<uint8_t> blobB = m_history.Reconstruct(b);
    if (blobA.empty() || blobB.empty())
        return "one of the frames could not be reconstructed";
    if (blobA.size() != blobB.size())
        return "the two frames were recorded from different machine configurations";

    const auto images = core::ExtractRamRegions(blobA.data(), blobA.size());
    if (images.empty())
        return "the save state carries no readable RAM images";

    // The blob records a bank's contents but not where it lives, so the base addresses come
    // from the running machine.
    auto* emu = Machine();
    if (!emu)
        return "emulator not available";
    const auto banks = emu->GetMemoryRegions();

    auto bankNameFor = [](const std::string& chunk) -> const char* {
        if (chunk == "CRAM")
            return "Chip RAM";
        if (chunk == "BRAM")
            return "Slow RAM";
        if (chunk == "FRAM")
            return "Fast RAM";
        if (chunk == "ZRAM")
            return "Z3 Fast RAM";
        return nullptr;
    };

    for (const auto& img : images) {
        const char* wanted = bankNameFor(img.chunk);
        if (!wanted)
            continue; // A3000 RAM and friends: not present on the models this app runs

        // Match on name and size; a machine with several boards of one kind reports them in
        // the same order the chunks were written.
        int seen = 0;
        uint32_t base = 0;
        bool found = false;
        for (const auto& bank : banks) {
            if (bank.name != wanted || bank.size != img.size)
                continue;
            if (seen++ != img.board)
                continue;
            base = bank.amigaAddr;
            found = true;
            break;
        }
        if (!found)
            continue; // the bank is not in the live layout; reporting a guessed address
                      // would be worse than reporting nothing

        const uint8_t* pa = blobA.data() + img.blobOffset;
        const uint8_t* pb = blobB.data() + img.blobOffset;
        size_t i = 0;
        while (i < img.size) {
            if (pa[i] == pb[i]) {
                ++i;
                continue;
            }
            const size_t start = i;
            while (i < img.size && pa[i] != pb[i])
                ++i;
            if (out.size() >= maxRuns) {
                truncated = true;
                return {};
            }
            out.push_back(core::FrameDiffRun{base + static_cast<uint32_t>(start),
                                             static_cast<uint32_t>(i - start), wanted});
        }
    }
    return {};
}

} // namespace adm::core
