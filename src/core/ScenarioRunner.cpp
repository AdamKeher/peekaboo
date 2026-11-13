#include "ScenarioRunner.h"

#include <chrono>
#include <ctime>

#include <stb_image_write.h> // declarations only; implementation in external/stb

#include "AmigaEmulator.h"

namespace adm::core {

namespace {

std::string UtcNow()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

const char* ResolutionName(AmigaEmulator::VideoResolution resolution)
{
    switch (resolution) {
        case AmigaEmulator::VideoResolution::Lores:
            return "Lores";
        case AmigaEmulator::VideoResolution::Hires:
            return "Hires";
        case AmigaEmulator::VideoResolution::SuperHires:
            return "SuperHires";
    }
    return "Hires";
}

// The machine as it stands, in the manifest's spelling. Used both to describe a recording
// and to check that a replay is happening on the same machine.
ScenarioManifest DescribeMachine(const AmigaEmulator& emu)
{
    ScenarioManifest m;
    m.created = UtcNow();
    m.coreVersion = emu.GetCoreVersion();
    m.model = GetAmigaModelInfo(emu.GetCurrentModel()).id;
    m.videoResolution = ResolutionName(emu.GetVideoResolution());
    m.videoStandard = "PAL";
    for (const auto& [key, value] : emu.GetUserVariables()) {
        m.userVariables[key] = value;
        if (key == "puae_video_standard")
            m.videoStandard = value;
    }
    return m;
}

} // namespace

std::vector<uint8_t> EncodeRgbPng(const std::vector<uint8_t>& rgb, int width, int height)
{
    std::vector<uint8_t> out;
    if (width <= 0 || height <= 0 ||
        rgb.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 3)
        return out;

    const auto writeCb = [](void* ctx, void* data, int size) {
        auto* vec = static_cast<std::vector<uint8_t>*>(ctx);
        const auto* bytes = static_cast<const uint8_t*>(data);
        vec->insert(vec->end(), bytes, bytes + size);
    };
    if (stbi_write_png_to_func(writeCb, &out, width, height, 3, rgb.data(), width * 3) == 0)
        out.clear();
    return out;
}

// ---------------------------------------------------------------- ScenarioRecorder

ScenarioRecorder::~ScenarioRecorder() { Abandon(); }

bool ScenarioRecorder::Start(AmigaEmulator& emu, Options options, std::string& error)
{
    error.clear();
    if (IsRecording()) {
        error = "already recording";
        return false;
    }
    if (!emu.IsGameLoaded()) {
        error = "no machine is running";
        return false;
    }
    // A physical platter cannot be rewound, so a scenario recorded against one could never
    // be replayed. Refusing here is the same rule the frame timeline already applies.
    if (options.anyDriveBridged && options.anyDriveBridged()) {
        error = "a drive is bridged to real hardware - a scenario cannot replay a physical disk";
        return false;
    }

    m_scenario.Clear();
    m_scenario.manifest = DescribeMachine(emu);
    m_scenario.manifest.notes = options.notes;
    m_scenario.manifest.coldBoot = options.coldBoot;
    if (options.media)
        m_scenario.manifest.media = options.media();

    m_hadInputCapture = emu.IsInputCaptureEnabled();
    m_hadStateCapture = emu.IsStateCaptureEnabled();
    m_hadFrameDigest = emu.IsFrameDigestEnabled();
    m_hadThumbnails = emu.IsThumbnailCaptureEnabled();
    m_thumbnailInterval = emu.GetThumbnailInterval();

    if (!options.coldBoot) {
        // Never SaveState() here. It serializes where retro_run() returned, three scanlines
        // into the next field, and PuAE always restores at vpos 0 - so those scanlines run
        // twice and the machine diverges immediately. SaveStateForRestore() hands back the
        // core's own vpos-0 capture, which is where a restore actually lands.
        emu.SetStateCaptureEnabled(true);
        m_scenario.seedState = emu.SaveStateForRestore();
        if (m_scenario.seedState.empty()) {
            if (!m_hadStateCapture)
                emu.SetStateCaptureEnabled(false);
            error = "could not capture a restorable save state for the seed";
            return false;
        }
    }

    emu.SetInputCaptureEnabled(true);
    m_options = std::move(options);
    m_emulator = &emu;
    return true;
}

void ScenarioRecorder::OnFrameComplete(uint64_t /*frame*/)
{
    if (!m_emulator)
        return;
    // The input as the core resolved it for the frame that just ran. Indexed by position:
    // frame numbers are not dense and the hole a mid-frame stop leaves would desynchronise
    // a replay that trusted them.
    m_scenario.input.push_back(m_emulator->LastFrameInput());
}

bool ScenarioRecorder::Stop(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    if (!m_emulator) {
        error = "not recording";
        return false;
    }
    if (m_scenario.input.empty()) {
        error = "nothing was recorded - no frame completed while recording";
        return false;
    }

    m_scenario.manifest.frameCount = m_scenario.input.size();
    if (!m_scenario.Save(path, error))
        return false;

    Disarm();
    return true;
}

void ScenarioRecorder::Abandon()
{
    if (!m_emulator)
        return;
    Disarm();
}

void ScenarioRecorder::Disarm()
{
    // Put the machine's capture flags back. State capture especially: PuAE's save_state()
    // mutates the machine on the way past, so leaving it armed is not free.
    if (m_emulator) {
        if (!m_hadInputCapture)
            m_emulator->SetInputCaptureEnabled(false);
        if (!m_hadStateCapture)
            m_emulator->SetStateCaptureEnabled(false);
        if (!m_hadFrameDigest)
            m_emulator->SetFrameDigestEnabled(false);
        if (!m_hadThumbnails)
            m_emulator->SetThumbnailCapture(false, m_thumbnailInterval);
    }
    m_emulator = nullptr;
    m_scenario.Clear();
    m_options = Options{};
}

// ------------------------------------------------------------------ ScenarioPlayer

ScenarioPlayer::Result
ScenarioPlayer::Replay(AmigaEmulator& emu, const ScenarioFile& scenario, const Options& options,
                       const std::function<void(uint64_t, uint64_t)>& progress)
{
    Result result;
    if (scenario.input.empty()) {
        result.error = "scenario has no frames";
        return result;
    }
    if (!emu.IsGameLoaded()) {
        result.error = "no machine is running";
        return result;
    }

    if (!scenario.seedState.empty()) {
        if (!emu.LoadState(scenario.seedState)) {
            result.error = "the core refused the scenario's seed state (different core build, "
                           "model or media?)";
            return result;
        }
    }

    const bool hadDigest = emu.IsFrameDigestEnabled();
    const bool hadThumbs = emu.IsThumbnailCaptureEnabled();
    const int thumbInterval = emu.GetThumbnailInterval();
    emu.SetFrameDigestEnabled(true);
    if (options.keyframeInterval > 0)
        emu.SetThumbnailCapture(true, 1);

    // Thumbnails are sampled inside VideoRefreshCallback but AFTER its m_acceptVideoRefresh
    // early-out, which a non-presenting step clears - so a headless replay that wants
    // pictures has to present. That costs nothing here: the GL upload below the thumbnail
    // guards itself on glad_glGenTextures, which is null with no context, so presenting
    // headless walks the framebuffer and stops. The digests do not care either way; they are
    // taken above the early-out.
    const bool present = options.present || options.keyframeInterval > 0;

    const uint64_t total = static_cast<uint64_t>(scenario.input.size());
    result.frames.reserve(scenario.input.size());

    emu.BeginInputReplay();
    for (uint64_t i = 0; i < total; ++i) {
        emu.SetReplayInput(scenario.input[static_cast<size_t>(i)]);
        // StepOneFrame, never Update(): Update() runs a catch-up batch against a wall clock
        // and would run an unknown number of frames per call.
        emu.StepOneFrame(present);

        const AmigaEmulator::FrameDigest digest = emu.GetLastFrameDigest();
        result.frames.push_back({digest.video, digest.audio, digest.audioSamples});

        if (options.ramInterval > 0 && (i % options.ramInterval) == 0)
            result.ram.push_back({i, emu.ComputeRamDigest()});

        if (options.keyframeInterval > 0 && (i % options.keyframeInterval) == 0) {
            std::vector<uint8_t> rgb;
            int w = 0, h = 0;
            if (emu.CopyThumbnail(rgb, w, h)) {
                std::vector<uint8_t> png = EncodeRgbPng(rgb, w, h);
                if (!png.empty())
                    result.keyframes.push_back({i, std::move(png)});
            }
        }

        if (progress && (i % 50) == 0)
            progress(i, total);
    }
    emu.EndInputReplay();

    if (!hadDigest)
        emu.SetFrameDigestEnabled(false);
    if (options.keyframeInterval > 0 && !hadThumbs)
        emu.SetThumbnailCapture(false, thumbInterval);

    result.framesRun = total;
    result.ok = true;
    if (progress)
        progress(total, total);
    return result;
}

// ---------------------------------------------------------------- CompareToGolden

ScenarioComparison CompareToGolden(const ScenarioGolden& golden, const ScenarioPlayer::Result& run)
{
    ScenarioComparison out;

    if (golden.Empty()) {
        out.matched = false;
        out.summary = "the scenario has no golden table - bless it first";
        return out;
    }
    if (golden.frames.size() != run.frames.size()) {
        out.matched = false;
        out.summary = "golden table covers " + std::to_string(golden.frames.size()) +
                      " frames, the replay produced " + std::to_string(run.frames.size());
        return out;
    }

    // First divergence wins, and the lanes are checked in the order that makes a report
    // readable: a picture that changed is the finding, and audio on a frame whose video
    // already diverged is a consequence rather than a second fault.
    for (size_t i = 0; i < golden.frames.size(); ++i) {
        const ScenarioFrameDigest& want = golden.frames[i];
        const ScenarioFrameDigest& got = run.frames[i];
        if (want.video != got.video) {
            out.matched = false;
            out.frame = i;
            out.lane = "video";
            out.expected = want.video;
            out.actual = got.video;
            break;
        }
        out.videoMatchedThrough = i + 1;
        if (want.audioSamples != got.audioSamples) {
            out.matched = false;
            out.frame = i;
            out.lane = "audio sample count";
            out.expected = want.audioSamples;
            out.actual = got.audioSamples;
            break;
        }
        if (want.audio != got.audio) {
            out.matched = false;
            out.frame = i;
            out.lane = "audio";
            out.expected = want.audio;
            out.actual = got.audio;
            break;
        }
        out.audioMatchedThrough = i + 1;
    }

    if (out.matched && !golden.ram.empty()) {
        // RAM samples are sparse and are matched by frame number, not position: a run asked
        // for a different interval than the bless would otherwise compare unrelated frames.
        for (const ScenarioRamDigest& want : golden.ram) {
            const ScenarioRamDigest* got = nullptr;
            for (const ScenarioRamDigest& candidate : run.ram)
                if (candidate.frame == want.frame) {
                    got = &candidate;
                    break;
                }
            if (!got)
                continue;
            ++out.ramSamplesChecked;
            if (got->digest != want.digest) {
                out.matched = false;
                out.frame = want.frame;
                out.lane = "ram";
                out.expected = want.digest;
                out.actual = got->digest;
                break;
            }
        }
    }

    if (out.matched) {
        out.summary = "all " + std::to_string(run.frames.size()) + " frames matched (" +
                      std::to_string(out.ramSamplesChecked) + " RAM samples checked)";
    } else if (out.lane.empty()) {
        out.summary = "did not match";
    } else {
        out.summary = "diverged at frame " + std::to_string(out.frame) + " (" + out.lane + ")";
    }
    return out;
}

} // namespace adm::core
