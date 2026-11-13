// peekaboo_smoke — records, replays and verifies scenario files headlessly.
//
// The regression harness for the emulator itself: a scenario is a machine, a starting
// point, an input stream and a table of what every frame looked like and sounded like.
// `verify` replays one and reports the first frame that no longer matches.
//
// Built like emulator_validation_tests: no OpenGL, muted audio
// device, no window - both digest taps live in the host-side libretro callbacks precisely
// so that works.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/AmigaEmulator.h"
#include "core/MD5.h"
#include "core/ScenarioFile.h"
#include "core/ScenarioRunner.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

using namespace adm::core;

std::string Argument(int argc, char** argv, const std::string& name, const std::string& fallback)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (argv[i] == name)
            return argv[i + 1];
    return fallback;
}

bool HasFlag(int argc, char** argv, const std::string& name)
{
    for (int i = 1; i < argc; ++i)
        if (argv[i] == name)
            return true;
    return false;
}

uint64_t ArgumentU64(int argc, char** argv, const std::string& name, uint64_t fallback)
{
    const std::string text = Argument(argc, argv, name, "");
    if (text.empty())
        return fallback;
    try {
        return std::stoull(text);
    } catch (const std::exception&) {
        return fallback;
    }
}

void Usage()
{
    std::cout << R"(peekaboo_smoke — scenario record/replay and emulator smoke testing

  peekaboo_smoke <verb> <scenario.pkscn> [options]

Verbs
  info       Print a scenario's manifest. Needs no core.
  create     Make a cold-boot scenario with --frames frames of no input, from --adf.
  replay     Replay and print the digests observed. Does not compare.
  bless      Replay and write the observed digests into the scenario as its goldens.
  verify     Replay and compare against the goldens. The CTest entry point.
  selfcheck  Replay twice in one process and compare the runs against each other.
             Answers "is this scenario deterministic at all" with no goldens involved.

Options
  --core <path>          PuAE core (default: the bundled one)
  --system-dir <dir>     Kickstart / system files            (default: emulation)
  --adf <path>           Media to mount in DF0 (create, and any verb whose scenario
                         is cold-boot)
  --artifacts <dir>      Where a failure writes its PNGs     (default: smoke-artifacts)
  --frames <n>           create: how many frames to record   (default: 300)
  --model <id>           Machine preset id (A500, A1200, ...)  (default: A500)
  --video <PAL|NTSC>     Video standard                      (default: PAL)
  --notes <text>         create: manifest note
  --ram-interval <n>     Sample a RAM digest every n frames. 0 = off (default: 0)
  --keyframe-interval <n>  Capture a thumbnail every n frames (default: 50)
  --quiet                Only print the verdict
)";
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
}

std::string Upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

// Model ids are spelled "A500" / "A1200OG" in the table. Matched case-insensitively so a
// command line reading --model a500 does the obvious thing rather than failing on the shift
// key.
adm::AmigaModel ModelFromId(const std::string& id, bool& found)
{
    const std::string want = Upper(id);
    for (const auto& info : adm::GetAllAmigaModels())
        if (want == Upper(info.id)) {
            found = true;
            return info.model;
        }
    found = false;
    return adm::AmigaModel::A500;
}

struct MachineOptions
{
    std::filesystem::path corePath;
    std::filesystem::path systemDir;
    std::filesystem::path adfPath;
    std::string modelId;
    std::string videoStandard;
};

// Builds the machine a scenario expects and mounts its media. The emulator is heap
// allocated deliberately: the class is several megabytes and a stack local is a stack
// overflow waiting for a smaller default stack than this one.
std::unique_ptr<adm::AmigaEmulator> BuildMachine(const MachineOptions& options, std::string& error)
{
    bool modelFound = false;
    const adm::AmigaModel model = ModelFromId(options.modelId, modelFound);
    if (!modelFound) {
        error = "unknown model id \"" + options.modelId + "\"";
        return nullptr;
    }

    auto emu = std::make_unique<adm::AmigaEmulator>();
    // Silence the host playback device. LoadADF() below calls InitAudio(), which opens a
    // real miniaudio device and plays the run out of the machine's speakers - a 5000-frame
    // demo scenario is 100 seconds of music every time the suite runs. Muting cannot move a
    // golden: the audio digest is taken in WriteAudioSamples(), upstream of the ring buffer
    // and the device, and is deliberately outside every audio-device guard so a headless run
    // still produces it.
    emu->SetMuted(true);
    emu->SetSystemDirectory(std::filesystem::absolute(options.systemDir).string());
    emu->SelectModelPreset(model);
    emu->SetUserVariable("puae_video_standard", options.videoStandard);

    const std::string corePath = std::filesystem::absolute(options.corePath).string();
    // LoadCore only null-checks the libretro entry points, while the adm_* exports are
    // dereferenced unconditionally per frame - so preflight the whole set and say which
    // build this is rather than crashing on a null later.
    if (!adm::AmigaEmulator::VerifyCore(corePath, error))
        return nullptr;
    if (!emu->LoadCore(corePath)) {
        error = "failed to load the PuAE core: " + corePath;
        return nullptr;
    }

    const std::vector<uint8_t> media = ReadFile(options.adfPath);
    if (media.empty()) {
        error = "cannot read media: " + options.adfPath.string();
        return nullptr;
    }
    const std::string ext = options.adfPath.extension().string().empty()
                                ? "adf"
                                : options.adfPath.extension().string().substr(1);
    if (!emu->LoadADF(media, ext)) {
        error = "the core refused the media: " + options.adfPath.string();
        return nullptr;
    }

    // None of this is part of a scenario and all of it costs time per frame.
    emu->SetCpuTraceEnabled(false);
    emu->SetDebugDma(0);
    return emu;
}

std::vector<ScenarioMedia> DescribeMedia(const std::filesystem::path& adfPath)
{
    const std::vector<uint8_t> bytes = ReadFile(adfPath);
    if (bytes.empty())
        return {};
    ScenarioMedia item;
    item.drive = 0;
    item.name = adfPath.filename().string();
    item.size = bytes.size();
    item.md5 = MD5::Compute(bytes);
    return {item};
}

void PrintManifest(const ScenarioFile& scn)
{
    const ScenarioManifest& m = scn.manifest;
    std::cout << "  frames        " << m.frameCount << "\n"
              << "  seed          " << (m.coldBoot ? "cold boot" : "save state")
              << (scn.seedState.empty() ? ""
                                        : " (" + std::to_string(scn.seedState.size()) + " bytes)")
              << "\n"
              << "  model         " << m.model << " " << m.videoStandard << " " << m.videoResolution
              << "\n"
              << "  core          " << m.coreVersion << "\n"
              << "  created       " << m.created << "\n";
    if (!m.appCommit.empty())
        std::cout << "  app           " << m.appCommit << "\n";
    for (const auto& item : m.media)
        std::cout << "  DF" << item.drive << "           " << item.name << "  " << item.size
                  << " bytes  md5 " << item.md5 << "\n";
    std::cout << "  goldens       "
              << (scn.golden.Empty()
                      ? std::string("none - run bless")
                      : std::to_string(scn.golden.frames.size()) + " frames, " +
                            std::to_string(scn.golden.ram.size()) + " RAM samples, " +
                            std::to_string(scn.golden.keyframes.size()) + " keyframes")
              << "\n";
    if (!m.notes.empty())
        std::cout << "  notes         " << m.notes << "\n";
}

bool WriteArtifact(const std::filesystem::path& dir, const std::string& name,
                   const std::vector<uint8_t>& bytes)
{
    if (bytes.empty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream file(dir / name, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

std::string FrameTag(uint64_t frame)
{
    std::string digits = std::to_string(frame);
    return std::string(digits.size() < 6 ? 6 - digits.size() : 0, '0') + digits;
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    // A crash dialog in CI is a hang, not a failure.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (argc < 2 || HasFlag(argc, argv, "--help") || HasFlag(argc, argv, "-h")) {
        Usage();
        return argc < 2 ? 2 : 0;
    }

    const std::string verb = argv[1];
    if (argc < 3) {
        std::cerr << "error: " << verb << " needs a scenario path\n";
        return 2;
    }
    const std::filesystem::path scenarioPath = argv[2];
    const bool quiet = HasFlag(argc, argv, "--quiet");

    MachineOptions machine;
    machine.corePath = Argument(argc, argv, "--core", adm::AmigaEmulator::GetDefaultCorePath());
    machine.systemDir = Argument(argc, argv, "--system-dir", "emulation");
    machine.adfPath = Argument(argc, argv, "--adf", "");
    machine.modelId = Argument(argc, argv, "--model", "A500");
    machine.videoStandard = Argument(argc, argv, "--video", "PAL");

    const std::filesystem::path artifacts = Argument(argc, argv, "--artifacts", "smoke-artifacts");

    ScenarioPlayer::Options playOptions;
    playOptions.present = false;
    playOptions.ramInterval = ArgumentU64(argc, argv, "--ram-interval", 0);
    playOptions.keyframeInterval =
        ArgumentU64(argc, argv, "--keyframe-interval", ScenarioFile::kDefaultKeyframeInterval);

    std::string error;

    // ---- create: a cold-boot scenario of N frames with no input -------------------
    if (verb == "create") {
        const uint64_t frames = ArgumentU64(argc, argv, "--frames", 300);
        if (machine.adfPath.empty()) {
            std::cerr << "error: create needs --adf\n";
            return 2;
        }
        auto emu = BuildMachine(machine, error);
        if (!emu) {
            std::cerr << "error: " << error << "\n";
            return 3;
        }

        ScenarioFile scn;
        scn.manifest.created.clear();
        scn.manifest.coreVersion = emu->GetCoreVersion();
        scn.manifest.model = machine.modelId;
        scn.manifest.videoStandard = machine.videoStandard;
        scn.manifest.videoResolution = "Hires";
        scn.manifest.coldBoot = true;
        scn.manifest.notes = Argument(argc, argv, "--notes", "");
        scn.manifest.media = DescribeMedia(machine.adfPath);
        scn.manifest.userVariables = {{"puae_video_standard", machine.videoStandard}};
        // A cold-boot scenario with nobody at the keyboard: the frames are what the machine
        // does on its own, which is exactly what a boot regression looks like.
        scn.input.assign(static_cast<size_t>(frames), FrameInput{});
        // Fill in the fields DescribeMachine would have set had a machine recorded this.
        {
            ScenarioRecorder probe;
            ScenarioRecorder::Options opts;
            opts.coldBoot = true;
            std::string probeError;
            if (probe.Start(*emu, opts, probeError)) {
                scn.manifest.videoResolution = probe.Manifest().videoResolution;
                scn.manifest.created = probe.Manifest().created;
                probe.Abandon();
            }
        }

        // Serialize writes the count from the input stream; keep the in-memory manifest in
        // step so what is printed below is what was saved.
        scn.manifest.frameCount = scn.input.size();
        if (!scn.Save(scenarioPath, error)) {
            std::cerr << "error: " << error << "\n";
            return 3;
        }
        std::cout << "created " << scenarioPath.string() << "\n";
        PrintManifest(scn);
        std::cout << "next: bless it, then verify it\n";
        return 0;
    }

    // ---- everything else loads the scenario first ---------------------------------
    ScenarioFile scn;
    if (!scn.Load(scenarioPath, error)) {
        std::cerr << "error: " << error << "\n";
        return 2;
    }

    if (verb == "info") {
        std::cout << scenarioPath.string() << "\n";
        PrintManifest(scn);
        return 0;
    }

    if (verb != "replay" && verb != "bless" && verb != "verify" && verb != "selfcheck") {
        std::cerr << "error: unknown verb \"" << verb << "\"\n";
        Usage();
        return 2;
    }

    // A cold-boot scenario has to be told what to boot; a seeded one carries its machine.
    if (machine.adfPath.empty() && !scn.manifest.media.empty())
        machine.adfPath = scn.manifest.media.front().name;
    if (!scn.manifest.model.empty())
        machine.modelId = scn.manifest.model;
    if (!scn.manifest.videoStandard.empty())
        machine.videoStandard = scn.manifest.videoStandard;

    if (machine.adfPath.empty()) {
        std::cerr << "error: this scenario names no media; pass --adf\n";
        return 2;
    }

    auto emu = BuildMachine(machine, error);
    if (!emu) {
        std::cerr << "error: " << error << "\n";
        return 3;
    }

    // The machine that was actually built, checked against the one the scenario expects.
    {
        ScenarioManifest actual;
        actual.model = machine.modelId;
        actual.videoStandard = machine.videoStandard;
        actual.videoResolution = scn.manifest.videoResolution;
        actual.media = DescribeMedia(machine.adfPath);
        std::string why;
        if (!scn.MachineMatches(actual, why)) {
            std::cerr << "error: this is not the machine the scenario was recorded on\n  " << why
                      << "\n";
            return 2;
        }
    }

    const std::string runningCore = emu->GetCoreVersion();
    if (!quiet && !scn.manifest.coreVersion.empty() && scn.manifest.coreVersion != runningCore)
        std::cout << "note: scenario recorded on core " << scn.manifest.coreVersion << ", running "
                  << runningCore << "\n";

    const auto progress = [&](uint64_t done, uint64_t total) {
        if (quiet || total == 0)
            return;
        std::cout << "\r  " << done << " / " << total << " frames" << std::flush;
        if (done == total)
            std::cout << "\n";
    };

    if (verb == "selfcheck") {
        // Two runs, one process, nothing else changed. A difference here is the scenario
        // (or the machine) being nondeterministic, and no golden table can fix that.
        const ScenarioPlayer::Result first =
            ScenarioPlayer::Replay(*emu, scn, playOptions, progress);
        if (!first.ok) {
            std::cerr << "error: " << first.error << "\n";
            return 3;
        }
        // One machine at a time. The libretro callbacks route through a single static
        // instance pointer inside AmigaEmulator, so a second live emulator steals the
        // callbacks from the first and the process dies on the way out. (Two SIMULTANEOUS
        // emulators need DuplicateCoreToTemp() to give each its own copy of the core's C
        // globals; sequential runs just need the first one gone.)
        emu.reset();

        auto second = BuildMachine(machine, error);
        if (!second) {
            std::cerr << "error: " << error << "\n";
            return 3;
        }
        const ScenarioPlayer::Result again =
            ScenarioPlayer::Replay(*second, scn, playOptions, progress);
        if (!again.ok) {
            std::cerr << "error: " << again.error << "\n";
            return 3;
        }

        ScenarioGolden asGolden;
        asGolden.frames = first.frames;
        asGolden.ram = first.ram;
        const ScenarioComparison cmp = CompareToGolden(asGolden, again);
        if (cmp.matched) {
            std::cout << "DETERMINISTIC: two runs agree over " << again.frames.size()
                      << " frames\n";
            return 0;
        }
        std::cout << "NONDETERMINISTIC at frame " << cmp.frame << " (" << cmp.lane << ")\n"
                  << "  run 1  " << std::hex << cmp.expected << "\n"
                  << "  run 2  " << cmp.actual << std::dec << "\n"
                  << "  video agreed through " << cmp.videoMatchedThrough << " frames\n"
                  << "\nThis scenario cannot be blessed as it stands.\n";
        return 1;
    }

    const ScenarioPlayer::Result run = ScenarioPlayer::Replay(*emu, scn, playOptions, progress);
    if (!run.ok) {
        std::cerr << "error: " << run.error << "\n";
        return 3;
    }

    if (verb == "replay") {
        std::cout << "replayed " << run.framesRun << " frames\n";
        // The geometry the core actually delivered, and the crop the app would apply to it.
        // A digest is only as meaningful as the buffer it was taken over, so say what that
        // buffer was.
        const adm::AmigaEmulator::DisplayGeometry geo = emu->GetDisplayGeometry();
        std::cout << "  framebuffer   " << emu->GetWidth() << "x" << emu->GetHeight()
                  << "   native " << geo.fullW << "x" << geo.fullH << "   crop " << geo.cropX << ","
                  << geo.cropY << "\n";
        if (!quiet) {
            const size_t show = std::min<size_t>(run.frames.size(), 10);
            std::cout << std::hex;
            for (size_t i = 0; i < show; ++i)
                std::cout << "  frame " << std::dec << i << "  video " << std::hex
                          << run.frames[i].video << "  audio " << run.frames[i].audio << std::dec
                          << "  samples " << run.frames[i].audioSamples << "\n";
            if (run.frames.size() > show)
                std::cout << "  ... " << (run.frames.size() - show) << " more\n";
        }
        return 0;
    }

    if (verb == "bless") {
        scn.golden.frames = run.frames;
        scn.golden.ram = run.ram;
        scn.golden.keyframes = run.keyframes;
        if (!scn.Save(scenarioPath, error)) {
            std::cerr << "error: " << error << "\n";
            return 3;
        }
        std::cout << "blessed " << scenarioPath.string() << ": " << scn.golden.frames.size()
                  << " frames, " << scn.golden.ram.size() << " RAM samples, "
                  << scn.golden.keyframes.size() << " keyframes\n";
        return 0;
    }

    // ---- verify --------------------------------------------------------------------
    const ScenarioComparison cmp = CompareToGolden(scn.golden, run);
    if (cmp.matched) {
        std::cout << "OK: " << cmp.summary << "\n";
        return 0;
    }

    std::cout << "DIVERGED at frame " << cmp.frame << " of " << run.frames.size() << " ("
              << cmp.lane << ")\n";
    if (!cmp.lane.empty()) {
        std::cout << "  golden  " << std::hex << cmp.expected << "\n"
                  << "  actual  " << cmp.actual << std::dec << "\n";
    }
    std::cout << "  video matched through " << cmp.videoMatchedThrough << ", audio through "
              << cmp.audioMatchedThrough << ", " << cmp.ramSamplesChecked
              << " RAM samples checked\n";
    if (!scn.manifest.coreVersion.empty())
        std::cout << "  core recorded " << scn.manifest.coreVersion << ", running " << runningCore
                  << "\n";
    if (!cmp.summary.empty() && cmp.lane.empty())
        std::cout << "  " << cmp.summary << "\n";

    // Artifacts: what it looks like now, and the nearest blessed picture to compare it
    // against. The golden is the nearest keyframe rather than this exact frame - storing a
    // PNG per frame is not worth it, and the pair is enough to see what happened.
    const std::filesystem::path outDir = artifacts / scenarioPath.stem();
    for (auto it = run.keyframes.rbegin(); it != run.keyframes.rend(); ++it) {
        if (it->frame > cmp.frame)
            continue;
        if (WriteArtifact(outDir, "frame-" + FrameTag(it->frame) + "-actual.png", it->png))
            std::cout << "  artifact: "
                      << (outDir / ("frame-" + FrameTag(it->frame) + "-actual.png")).string()
                      << "\n";
        break;
    }
    if (const ScenarioKeyframe* golden = scn.golden.NearestKeyframe(cmp.frame)) {
        if (WriteArtifact(outDir, "frame-" + FrameTag(golden->frame) + "-golden.png", golden->png))
            std::cout << "  artifact: "
                      << (outDir / ("frame-" + FrameTag(golden->frame) + "-golden.png")).string()
                      << "\n";
    }

    return 1;
}
