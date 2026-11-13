#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace adm::core {

// Health of one audio capture tap. Mirrors AudioRecorder::Stats without dragging the recorder
// (or AmigaEmulator) into the console and MCP layers.
struct AudioTapStatus
{
    bool recording = false;
    std::string path;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint64_t framesWritten = 0;
    uint64_t framesDroppedStaging = 0; // capture ring overflowed — recording is unreliable
    double seconds = 0.0;
};

// Running state of the emulator's audio path, produced on demand by Application and consumed by
// the AR console AUDIO command and the MCP get_audio_stats tool.
//
// Every counter here answers a question the emulator previously could not: overruns, underruns and
// resampler losses were all silent drops. `held` in particular is the direct signature of the
// underrun fill — when the ring runs dry the device callback repeats the last sample as DC, which
// is audible as crunch rather than as a gap.
struct AudioLogSnapshot
{
    bool available = false; // emulator present

    // Configuration
    bool initialized = false; // host audio device is open
    bool muted = false;
    bool resampling = false; // core rate != device rate
    std::string backend;     // "wasapi-push" or "miniaudio"
    double coreSampleRate = 0.0;
    uint32_t deviceSampleRate = 0;
    double targetFps = 0.0;

    // Producer side (main thread, inside retro_run)
    uint64_t framesProduced = 0;
    uint64_t framesDroppedOverrun = 0; // ring was full — emulated audio thrown away

    // Consumer side (audio thread)
    uint64_t framesConsumed = 0;
    uint64_t framesHeld = 0;             // output frames faked by repeating the last sample
    uint64_t framesDroppedResampler = 0; // input frames the resampler never consumed
    uint64_t deviceCallbacks = 0;
    uint64_t underrunCallbacks = 0;

    // Ring occupancy, sampled per device callback
    uint32_t ringSize = 0;
    uint32_t ringLevel = 0;
    uint32_t ringLevelMin = 0;
    uint32_t ringLevelMax = 0;
    uint32_t ringTarget = 0;    // frames held before output starts (the cushion)
    double ringLevelMean = 0.0; // mean occupancy at callback entry while playing
    uint64_t framesTrimmed = 0; // stale audio dropped to hold the ring at its target
    uint64_t trimEvents = 0;
    uint32_t ringLevelMinPlaying = 0; // lowest occupancy at callback entry while playing
    double pacePpm = 0.0;             // pace correction holding the ring at its setpoint

    // Latency against the picture: the ring plus what the sound card queues behind it. The
    // display's own delay (a refresh or two) is not included.
    uint32_t devicePeriodFrames = 0;
    uint32_t deviceBufferFrames = 0;
    double latencyMs = 0.0;

    // Producer jitter — frames delivered per retro_run(). Steady is ~882 at 44.1 kHz PAL; a guest
    // whose blocks vary widely is the shape we are hunting.
    uint32_t lastBlockFrames = 0;
    uint32_t minBlockFrames = 0;
    uint32_t maxBlockFrames = 0;
    uint64_t blockCount = 0;
    double meanBlockFrames = 0.0;

    // Emulated time Update() declined to run. This audio is never generated at all, so it leaves a
    // hole that no amount of host-side buffering can cover — a distinct fault from a ring underrun.
    uint64_t timeDiscardEvents = 0;
    uint64_t timeDiscardedFrames = 0;
    uint64_t timeDiscardElapsedClamp = 0;     // host hitch longer than 100 ms
    uint64_t timeDiscardAccumulatorClamp = 0; // accumulator collapsed to a single frame
    uint64_t timeDiscardFrameStopCap = 0;     // run-until-frame cap

    // Wall-clock spacing of deliveries from the core, which separates "the main thread stalled"
    // from "the core delivered uneven blocks".
    double produceGapMeanMs = 0.0;
    double produceGapMaxMs = 0.0;
    uint64_t produceStalls = 0; // gaps longer than the device's own callback period
    // Delivery gaps by size: [0] below produceGapEdgesMs[0], ..., [7] at or above the last edge.
    std::array<double, 7> produceGapEdgesMs{};
    std::array<uint64_t, 8> produceGapHist{};

    // Emulation cost. If updateMaxMs is close to produceGapMaxMs the stall is emulation itself; if
    // it is far below, the main thread was blocked by something else in the frame.
    double updateMeanMs = 0.0;
    double updateMaxMs = 0.0;
    uint64_t updateSlow = 0;

    // Ground truth for frame length. A PAL frame is 312 x 227 = 70,824 CCK. retroRunCalls above
    // frameBoundaries means retro_run is returning mid-frame rather than completing a frame.
    uint64_t retroRunCalls = 0;
    uint64_t frameBoundaries = 0;
    double frameCckMean = 0.0;
    uint32_t frameCckMin = 0;
    uint32_t frameCckMax = 0;

    AudioTapStatus coreTap;
    AudioTapStatus deviceTap;
};

// Start or stop audio capture. Either tap may be selected independently; `stop` ignores the rest.
// For a two-tap capture the device WAV is written to "<path>" with ".device" inserted before the
// extension, so one request yields a matched pair.
struct AudioRecordRequest
{
    bool stop = false;
    bool tapCore = false;
    bool tapDevice = false;
    std::string path;
    double seconds = 0.0; // >0 stops automatically after this much audio
};

struct AudioRecordResult
{
    bool ok = false;
    std::string message;
};

} // namespace adm::core
