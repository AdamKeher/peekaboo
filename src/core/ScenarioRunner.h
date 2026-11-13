#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ScenarioFile.h"

namespace adm {
class AmigaEmulator;
}

namespace adm::core {

/**
 * @brief Records a live session into a scenario file.
 *
 * Start() takes the seed and arms the machine; OnFrameComplete() appends one frame's input
 * and must be called for EVERY completed frame - Update() runs a catch-up batch of up to a
 * dozen frames per host redraw, and sampling once per redraw would keep only the last of
 * each batch. AmigaEmulator::SetOnFrameComplete is the hook that fires for all of them.
 *
 * Recording is refused outright in the two cases where the result could not be replayed:
 * while any drive is bridged to real hardware (a physical platter cannot be rewound - the
 * same rule FrameTimeline::RecordFrame applies), and when the machine is not running.
 */
class ScenarioRecorder
{
public:
    struct Options
    {
        // false takes a save-state seed, so the scenario starts from this exact moment.
        // true records from wherever the machine is with no seed, and the replay cold-boots
        // the media instead - which is what makes a scenario small enough to commit.
        bool coldBoot = false;
        std::string notes;
        // What is mounted, for the manifest. Optional; a scenario with no media recorded
        // simply cannot check that a replay is running against the same disk.
        std::function<std::vector<ScenarioMedia>()> media;
        // True while any drive is bridged to a real physical drive. Optional; absent means
        // "never".
        std::function<bool()> anyDriveBridged;
    };

    ~ScenarioRecorder();

    bool Start(AmigaEmulator& emu, Options options, std::string& error);
    // One completed frame. Cheap: it copies the input the core already resolved.
    void OnFrameComplete(uint64_t frame);
    // Writes the file and disarms. Fails (and keeps recording) if nothing was captured.
    bool Stop(const std::filesystem::path& path, std::string& error);
    // Disarms without writing anything.
    void Abandon();

    bool IsRecording() const { return m_emulator != nullptr; }
    size_t FrameCount() const { return m_scenario.input.size(); }
    const ScenarioManifest& Manifest() const { return m_scenario.manifest; }

private:
    void Disarm();

    AmigaEmulator* m_emulator = nullptr;
    ScenarioFile m_scenario;
    Options m_options;
    // What the machine's capture flags were before Start armed them, so stopping puts the
    // machine back rather than leaving state capture running - which is not passive.
    bool m_hadInputCapture = false;
    bool m_hadStateCapture = false;
    bool m_hadFrameDigest = false;
    bool m_hadThumbnails = false;
    int m_thumbnailInterval = 1;
};

/**
 * @brief Replays a scenario and reports what the machine actually did.
 *
 * The caller is responsible for having built the right machine and mounted the right media
 * first - that differs between the app (which already has a document open) and the headless
 * runner (which loads from a path), and it is not the player's business.
 *
 * Everything the replay observes is returned rather than compared. Comparison against a
 * golden table is a separate step, so that `bless` and `verify` run identical emulation.
 */
class ScenarioPlayer
{
public:
    struct Options
    {
        // Present frames to OpenGL. Off for a headless run; the digests are captured before
        // the presentation path either way.
        bool present = false;
        // Sample a RAM digest every N frames. 0 = never. Expensive: it walks every writable
        // bank.
        uint64_t ramInterval = 0;
        // Capture a thumbnail every N frames. 0 = never. Thumbnails rather than full frames
        // because CaptureScreenshotPNG needs a GL context and a headless replay has none,
        // while the thumbnail path reads the core's framebuffer directly.
        uint64_t keyframeInterval = 0;
    };

    struct Result
    {
        bool ok = false;
        std::string error;
        std::vector<ScenarioFrameDigest> frames;
        std::vector<ScenarioRamDigest> ram;
        std::vector<ScenarioKeyframe> keyframes;
        uint64_t framesRun = 0;
    };

    // `progress` is called with (framesDone, framesTotal) every so often; optional.
    static Result Replay(AmigaEmulator& emu, const ScenarioFile& scenario, const Options& options,
                         const std::function<void(uint64_t, uint64_t)>& progress = {});
};

/**
 * @brief The first place a replay stopped matching its golden table.
 *
 * First divergence, not a count: once one frame differs every frame after it is running on a
 * machine that has already gone somewhere else, so the tail says nothing.
 */
struct ScenarioComparison
{
    bool matched = true;
    uint64_t frame = 0;
    std::string lane; // "video", "audio", "audio sample count", "ram"
    uint64_t expected = 0;
    uint64_t actual = 0;
    // How far each lane got before anything diverged.
    uint64_t videoMatchedThrough = 0;
    uint64_t audioMatchedThrough = 0;
    uint64_t ramSamplesChecked = 0;
    std::string summary;
};

ScenarioComparison CompareToGolden(const ScenarioGolden& golden, const ScenarioPlayer::Result& run);

// PNG-encode a top-row-first RGB888 image. Returns empty on failure.
std::vector<uint8_t> EncodeRgbPng(const std::vector<uint8_t>& rgb, int width, int height);

} // namespace adm::core
