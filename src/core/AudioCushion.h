#pragma once

#include <cstdint>
#include <limits>

namespace adm::core {

// How much emulated audio the output ring may hold ahead of the sound card.
//
// Every frame in the ring is latency against the picture: the frame it belongs to is already on
// screen. The producer (retro_run) and the sound card run at the same rate, so whatever the ring
// holds it keeps - and a catch-up burst after a main-thread stall used to refill it past the
// target for good, playing the sound that much later than the picture from then on. One session
// with music-synced visuals logged a 123 ms stall and the ring peaking at 182 ms.
//
// Two rules, both dropping the OLDEST queued audio (the audio whose picture was shown longest ago):
//  - when output starts, or resumes after an underrun, keep exactly the target;
//  - when the ring's lowest level over a window of output never came down to the target, the
//    excess is no longer cushion: drop it, back to half a block below the target - where a fresh
//    start leaves the floor - so the trigger has half a block of hysteresis.
//
// Between those, PaceFactor() keeps the ring where a fresh start leaves it, the way WinUAE does
// (docorrection/sound_setadjust in od-win32/sounddep/sound.cpp): the emulator runs fractionally
// faster or slower so the sound card itself holds the level. Drift between the host clock and the
// sound card then never builds up into latency or into an underrun.
//
// Pure arithmetic, so it can be tested without a sound card; the device callback owns the state.
class AudioCushion
{
public:
    // Largest pace correction, as WinUAE (ADJUST_LIMIT, 6 per mille): no audible tempo change,
    // and far too little to chase a core that produces the wrong amount of audio - letting the
    // ring drive the emulator unbounded ran a VPOSW-afflicted demo at 91 fps.
    static constexpr double kPaceLimit = 0.006;
    // Correction per block of error: a block too deep drains in ~4 s.
    static constexpr double kPaceGain = 0.005;

    // target: frames the ring must hold before output starts. block: frames the core delivers
    // per emulated frame. period: frames the sound card takes per callback. window: output
    // frames over which the floor is measured.
    void Reset(uint32_t target, uint32_t block, uint32_t period, uint32_t window)
    {
        m_target = target;
        m_block = block < target ? block : target;
        m_period = period < m_block ? period : m_block;
        m_window = window;
        m_minTarget = m_maxTarget = target;
        m_decayFrames = 0;
        m_quietFrames = 0;
        m_skipDebt = 0;
        RestartWindow();
    }

    // Output is starting with `available` frames queued. Returns the frames to drop first.
    uint32_t OnPrime(uint32_t available)
    {
        RestartWindow();
        return available > m_target ? available - m_target : 0;
    }

    // A primed callback found `available` frames queued and is about to consume `frames` of them.
    // Returns the frames to drop before consuming.
    uint32_t OnCallback(uint32_t available, uint32_t frames)
    {
        if (available < m_windowMin)
            m_windowMin = available;
        m_windowFrames += frames;
        if (m_windowFrames < m_window)
            return 0;
        const uint32_t floor = m_windowMin;
        RestartWindow();
        return floor > m_target ? floor - (m_target - m_block / 2) : 0;
    }

    uint32_t Target() const { return m_target; }

    // Push output (WasapiPushOutput): the cushion is the sound card's own queue, written as each
    // block arrives, so there is nothing to drop from - excess is skipped from the blocks still
    // to come instead. The target adapts: how much cushion a session needs depends on how evenly
    // the host loop delivers frames (a 60 Hz loop lands 50 Hz frames 17 or 33 ms apart, a 144 Hz
    // one 14 or 21), which nothing here can know in advance.
    struct PushPlan
    {
        uint32_t silence = 0; // write first: the queue ran dry, so rebuild the cushion
        uint32_t skip = 0;    // drop from the block's start: the queue is deeper than it needs
        bool starved = false; // it ran dry while playing - the sound card went silent
    };

    // Lets the push target move between `minTarget` (where Reset() put it) and `maxTarget`: up a
    // period on every starve, down half a period after `decayFrames` played without one.
    void SetPushRange(uint32_t maxTarget, uint32_t decayFrames)
    {
        m_maxTarget = maxTarget > m_target ? maxTarget : m_target;
        m_decayFrames = decayFrames;
    }

    // A block of `frames` is about to be written with `queued` frames waiting. `playing` is false
    // when nothing was written for a while (a pause, a stop), so an empty queue is a fresh start.
    PushPlan PlanPush(uint32_t queued, uint32_t frames, bool playing)
    {
        PushPlan plan;
        if (queued == 0) {
            if (playing) {
                plan.starved = true;
                m_target = m_target + m_period < m_maxTarget ? m_target + m_period : m_maxTarget;
                m_quietFrames = 0;
            }
            if (frames < m_target)
                plan.silence = m_target - frames;
            RestartWindow();
            m_skipDebt = 0;
        } else if (playing) {
            m_quietFrames += frames;
            if (m_decayFrames && m_quietFrames >= m_decayFrames) {
                const uint32_t lowered = m_target - m_period / 2;
                m_target = lowered > m_minTarget ? lowered : m_minTarget;
                m_quietFrames = 0;
            }
            // As OnCallback: a queue that never came down to the target over a whole window is
            // carrying a block or more it does not need. Bring its floor back to a block below.
            if (queued < m_windowMin)
                m_windowMin = queued;
            m_windowFrames += frames;
            if (m_windowFrames >= m_window) {
                if (m_windowMin > m_target)
                    m_skipDebt = m_windowMin - (m_target - m_block);
                RestartWindow();
            }
        }
        // Pay the excess off at most half a block per write, so each cut stays small.
        uint32_t skip = m_skipDebt < frames / 2 ? m_skipDebt : frames / 2;
        // A catch-up burst after a stall: the frames past two blocks above the target are audio
        // whose picture is long gone.
        const uint64_t after = uint64_t(queued) + plan.silence + frames - skip;
        const uint64_t cap = uint64_t(m_target) + 2ull * m_block;
        if (after > cap) {
            const uint64_t over = after - cap;
            skip += over < frames - skip ? static_cast<uint32_t>(over) : frames - skip;
        }
        m_skipDebt -= m_skipDebt < skip ? m_skipDebt : skip;
        plan.skip = skip;
        return plan;
    }

    // Where the queue sits just before a write, on average, once a block lands every frame:
    // a block above it after the write, so half a block below the target in between.
    double PushSetpoint() const { return m_target - m_block / 2.0; }

    // The mean occupancy a fresh start settles at: output starts at the target, and between two
    // blocks the sound card takes block/period pulls, so the level saws down by block - period.
    double Setpoint() const { return m_target - (m_block - m_period) / 2.0; }

    // Multiplier for the wall-clock time the emulator is owed, from the ring's smoothed level.
    // Above the setpoint the emulator is producing too far ahead of the sound card: slow down.
    double PaceFactor(double meanLevel) const { return PaceFactor(meanLevel, Setpoint()); }

    double PaceFactor(double meanLevel, double setpoint) const
    {
        if (m_block == 0)
            return 1.0;
        double adjust = -kPaceGain * (meanLevel - setpoint) / m_block;
        if (adjust > kPaceLimit)
            adjust = kPaceLimit;
        if (adjust < -kPaceLimit)
            adjust = -kPaceLimit;
        return 1.0 + adjust;
    }

private:
    void RestartWindow()
    {
        m_windowMin = std::numeric_limits<uint32_t>::max();
        m_windowFrames = 0;
    }

    uint32_t m_target = 0;
    uint32_t m_block = 0;
    uint32_t m_period = 0;
    uint32_t m_window = 0;
    uint32_t m_windowMin = std::numeric_limits<uint32_t>::max();
    uint32_t m_windowFrames = 0;
    // Push output only.
    uint32_t m_minTarget = 0;
    uint32_t m_maxTarget = 0;
    uint32_t m_decayFrames = 0;
    uint32_t m_quietFrames = 0;
    uint32_t m_skipDebt = 0;
};

} // namespace adm::core
