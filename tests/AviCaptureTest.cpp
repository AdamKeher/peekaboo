#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/AmigaEmulator.h"
#include "core/FfmpegRecorder.h"
#include "core/Subprocess.h"

// The muxer itself is covered by AviRecorderTest. This is the wiring: a real machine, recorded
// through AmigaEmulator, producing a file with both streams in it. The two halves arrive from
// different places - the picture at the frame boundary, the audio as the core delivers it - so
// only an end-to-end capture shows they line up.
namespace {

#ifdef _WIN32
constexpr const char* kCoreName = "puae_libretro.dll";
#else
constexpr const char* kCoreName = "puae_libretro.dylib";
#endif

std::vector<uint8_t> ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

uint32_t U32(const std::vector<uint8_t>& b, size_t at)
{
    if (at + 4 > b.size())
        return 0;
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

size_t Find(const std::vector<uint8_t>& b, const char* tag, size_t from = 0)
{
    for (size_t i = from; i + 4 <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, tag, 4) == 0)
            return i;
    }
    return std::string::npos;
}

std::filesystem::path OutputPath(const char* name)
{
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "adm-avi-capture-test";
    std::filesystem::create_directories(dir, ec);
    return dir / name;
}

adm::AmigaEmulator::VideoCaptureOptions UncompressedOptions()
{
    adm::AmigaEmulator::VideoCaptureOptions options;
    options.codec = adm::core::VideoCaptureCodec::Uncompressed;
    return options;
}

std::unique_ptr<adm::AmigaEmulator> BootA500()
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / kCoreName;
    if (!std::filesystem::is_regular_file(corePath))
        return nullptr;
    auto emulator = std::make_unique<adm::AmigaEmulator>();
    // Muted: StartEmpty() opens a real host playback device. See EmulatorWarmResetTest.
    // Muting silences the device, not the core tap the recorder takes, so the film still has
    // sound in it.
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    if (!emulator->SelectModelPreset(adm::AmigaModel::A500))
        return nullptr;
    if (!emulator->LoadCore(corePath.string()) || !emulator->StartEmpty())
        return nullptr;
    // A frame has to exist before the size is known - and it has to have been PRESENTED.
    // StepOneFrame(false) clears m_acceptVideoRefresh, so a non-presenting step captures
    // nothing at all and CaptureFrameRGBA comes back empty. See the smoke harness notes.
    for (int i = 0; i < 30; ++i)
        emulator->StepOneFrame(true);
    return emulator;
}

TEST(AviCapture, RecordsARunningMachineToAPlayableFile)
{
    auto emulator = BootA500();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core or ROM is not present in emulation/";

    const auto path = OutputPath("machine.avi");
    std::string error;
    ASSERT_TRUE(emulator->StartVideoRecording(path.string(), UncompressedOptions(), error))
        << error;
    EXPECT_TRUE(emulator->IsVideoRecording());

    constexpr int kFrames = 25;
    for (int i = 0; i < kFrames; ++i)
        emulator->StepOneFrame(true);

    EXPECT_GE(emulator->GetVideoFramesRecorded(), 1u) << "no picture reached the file";
    ASSERT_TRUE(emulator->StopVideoRecording());
    EXPECT_FALSE(emulator->IsVideoRecording());

    const auto bytes = ReadAll(path);
    ASSERT_GT(bytes.size(), 1024u) << "the file is too small to hold a frame";
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), 4), "RIFF");
    EXPECT_EQ(U32(bytes, 4) + 8u, bytes.size()) << "the RIFF size should cover the whole file";

    const size_t avih = Find(bytes, "avih");
    ASSERT_NE(avih, std::string::npos);
    const uint32_t totalFrames = U32(bytes, avih + 24);
    const uint32_t width = U32(bytes, avih + 40);
    const uint32_t height = U32(bytes, avih + 44);
    EXPECT_GT(totalFrames, 0u);
    EXPECT_EQ(totalFrames, emulator->GetVideoFramesRecorded());

    // The core hands over 720x287 for a PAL field. Written like that it plays squashed to 2.5:1
    // and blurs when scaled up, so every field line is doubled - and the true shape is declared.
    EXPECT_EQ(width, 720u);
    EXPECT_EQ(height, 574u) << "PAL field lines should be doubled";
    const size_t vprp = Find(bytes, "vprp");
    ASSERT_NE(vprp, std::string::npos) << "no display aspect was written";
    const uint32_t frameAspect = U32(bytes, vprp + 8 + 20);
    const double aspect =
        static_cast<double>(frameAspect >> 16) / static_cast<double>(frameAspect & 0xFFFF);
    // The film is shown the shape the emulator window draws it - the core's reported aspect,
    // which is also what the GIF recorder stretches to.
    EXPECT_NEAR(aspect, emulator->GetAspectRatio(), 0.001);

    // Both streams, and audio that actually arrived - a silent file would still have the header.
    EXPECT_EQ(U32(bytes, avih + 32), 2u) << "dwStreams: the machine has sound";
    EXPECT_NE(Find(bytes, "00db"), std::string::npos) << "no video chunk";
    EXPECT_NE(Find(bytes, "01wb"), std::string::npos) << "no audio chunk";

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// A recording must not outlive the machine: stopping the emulator closes the file so it is left
// complete rather than as a stub with a zero frame count in its header.
TEST(AviCapture, StoppingTheMachineClosesTheFile)
{
    auto emulator = BootA500();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core or ROM is not present in emulation/";

    const auto path = OutputPath("interrupted.avi");
    std::string error;
    ASSERT_TRUE(emulator->StartVideoRecording(path.string(), UncompressedOptions(), error))
        << error;
    for (int i = 0; i < 10; ++i)
        emulator->StepOneFrame(true);
    const uint32_t recorded = emulator->GetVideoFramesRecorded();
    ASSERT_GT(recorded, 0u);

    emulator->UnloadCore();
    EXPECT_FALSE(emulator->IsVideoRecording()) << "the recording should have been closed";

    const auto bytes = ReadAll(path);
    ASSERT_GT(bytes.size(), 1024u);
    const size_t avih = Find(bytes, "avih");
    ASSERT_NE(avih, std::string::npos);
    EXPECT_EQ(U32(bytes, avih + 24), recorded)
        << "the header should carry the frames that were written, not zero";
    EXPECT_NE(Find(bytes, "idx1"), std::string::npos) << "the index should have been written";

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// The built-in lossless codec on a real machine: the same shape, a fraction of the size.
TEST(AviCapture, ZmbvRecordsTheMachineAtAFractionOfTheSize)
{
    auto emulator = BootA500();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core or ROM is not present in emulation/";

    const auto path = OutputPath("machine-zmbv.avi");
    adm::AmigaEmulator::VideoCaptureOptions options;
    options.codec = adm::core::VideoCaptureCodec::Zmbv;
    std::string error;
    ASSERT_TRUE(emulator->StartVideoRecording(path.string(), options, error)) << error;
    for (int i = 0; i < 50; ++i)
        emulator->StepOneFrame(true);
    ASSERT_TRUE(emulator->StopVideoRecording());
    EXPECT_EQ(emulator->GetVideoFramesRecorded(), 50u);
    EXPECT_NE(emulator->GetVideoCaptureStatus().find("Saved 50 frames"), std::string::npos)
        << emulator->GetVideoCaptureStatus();

    const auto bytes = ReadAll(path);
    const size_t strf = Find(bytes, "strf");
    ASSERT_NE(strf, std::string::npos);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data() + strf + 24), 4), "ZMBV");
    const size_t avih = Find(bytes, "avih");
    EXPECT_EQ(U32(bytes, avih + 40), 720u);
    EXPECT_EQ(U32(bytes, avih + 44), 574u);
    // A second of uncompressed PAL is 62 MB; the Kickstart screen barely changes.
    EXPECT_LT(bytes.size(), 2u * 1024u * 1024u) << "ZMBV should be tiny here";

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// An FFmpeg codec finishes after Stop, in the background: the emulator reports it as finishing,
// then - once PollVideoCapture has collected it - where it was saved.
TEST(AviCapture, FfmpegCaptureFinishesInTheBackgroundAndSaysWhereItSaved)
{
    const auto tools = adm::core::FfmpegTools::Probe("");
    if (!tools.UnavailableReason(adm::core::VideoCaptureCodec::H264).empty())
        GTEST_SKIP() << tools.UnavailableReason(adm::core::VideoCaptureCodec::H264);
    auto emulator = BootA500();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core or ROM is not present in emulation/";

    const auto requested = OutputPath("machine-h264.avi"); // the codec decides the extension
    const auto expected = OutputPath("machine-h264.mp4");
    std::error_code ec;
    std::filesystem::remove(expected, ec);

    adm::AmigaEmulator::VideoCaptureOptions options;
    options.codec = adm::core::VideoCaptureCodec::H264;
    options.ffmpegExecutable = tools.executable;
    std::string error;
    ASSERT_TRUE(emulator->StartVideoRecording(requested.string(), options, error)) << error;
    for (int i = 0; i < 50; ++i)
        emulator->StepOneFrame(true);
    ASSERT_TRUE(emulator->StopVideoRecording());
    EXPECT_FALSE(emulator->IsVideoRecording());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (emulator->IsVideoFinishing() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_FALSE(emulator->IsVideoFinishing()) << "the mux never finished";
    emulator->PollVideoCapture();
    const std::string status = emulator->GetVideoCaptureStatus();
    EXPECT_NE(status.find("Saved 50 frames"), std::string::npos) << status;
    EXPECT_NE(status.find("machine-h264.mp4"), std::string::npos) << status;
    EXPECT_EQ(emulator->GetVideoFramesRecorded(), 50u);

    std::vector<std::string> lines;
    adm::core::Subprocess probe;
    const int rc =
        probe.Run({"ffprobe", "-v", "error", "-show_entries", "stream=codec_name,width,height",
                   "-of", "default=noprint_wrappers=1", expected.string()},
                  [&](const std::string& line) { lines.push_back(line); });
    ASSERT_EQ(rc, 0) << "ffprobe could not read " << expected.string();
    const auto has = [&](const std::string& line) {
        return std::find(lines.begin(), lines.end(), line) != lines.end();
    };
    EXPECT_TRUE(has("codec_name=h264"));
    EXPECT_TRUE(has("codec_name=aac")) << "the machine's sound should be in the film";
    EXPECT_TRUE(has("width=720"));
    EXPECT_TRUE(has("height=574"));

    std::filesystem::remove(expected, ec);
}

// The window quotes this size (and, for uncompressed, the data rate it implies) before a
// recording starts, so it has to be the size the recording will actually have.
TEST(AviCapture, TheSizeQuotedBeforeRecordingIsTheSizeRecorded)
{
    auto emulator = BootA500();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core or ROM is not present in emulation/";

    int w = 0, h = 0, srcW = 0, srcH = 0;
    ASSERT_TRUE(emulator->GetVideoCaptureSize(w, h, srcW, srcH));
    EXPECT_EQ(srcW, 720);
    EXPECT_EQ(srcH, 287);
    EXPECT_EQ(w, 720);
    EXPECT_EQ(h, 574);

    const auto path = OutputPath("quoted-size.avi");
    adm::AmigaEmulator::VideoCaptureOptions options;
    options.codec = adm::core::VideoCaptureCodec::Zmbv;
    std::string error;
    ASSERT_TRUE(emulator->StartVideoRecording(path.string(), options, error)) << error;
    emulator->StepOneFrame(true);
    ASSERT_TRUE(emulator->StopVideoRecording());
    const auto bytes = ReadAll(path);
    const size_t avih = Find(bytes, "avih");
    ASSERT_NE(avih, std::string::npos);
    EXPECT_EQ(U32(bytes, avih + 40), static_cast<uint32_t>(w));
    EXPECT_EQ(U32(bytes, avih + 44), static_cast<uint32_t>(h));

    std::error_code ec;
    std::filesystem::remove(path, ec);

    // On the heap: the class is several megabytes.
    const auto stopped = std::make_unique<adm::AmigaEmulator>();
    EXPECT_FALSE(stopped->GetVideoCaptureSize(w, h, srcW, srcH)) << "no machine, no size";
}

TEST(AviCapture, RefusesToRecordAMachineThatIsNotRunning)
{
    auto emulator = std::make_unique<adm::AmigaEmulator>();
    std::string error;
    EXPECT_FALSE(emulator->StartVideoRecording(OutputPath("never.avi").string(),
                                               UncompressedOptions(), error));
    EXPECT_FALSE(error.empty());
}

} // namespace
