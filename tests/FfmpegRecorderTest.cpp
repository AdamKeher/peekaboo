#include "core/FfmpegRecorder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "core/Subprocess.h"

// The FFmpeg half of video capture. The command lines are checked as data; the recordings are
// made with the real ffmpeg and read back with ffprobe, and skipped on a machine without it.
namespace {

using adm::core::FfmpegRecorder;
using adm::core::FfmpegTools;
using adm::core::VideoCaptureCodec;

bool Contains(const std::vector<std::string>& args, const std::string& a, const std::string& b)
{
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == a && args[i + 1] == b)
            return true;
    }
    return false;
}

bool Has(const std::vector<std::string>& args, const std::string& a)
{
    return std::find(args.begin(), args.end(), a) != args.end();
}

std::filesystem::path OutputDir()
{
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "adm-ffmpeg-test";
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string PathUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::vector<std::string> RunTool(const std::vector<std::string>& args, int& rc)
{
    std::vector<std::string> lines;
    adm::core::Subprocess process;
    rc = process.Run(args, [&](const std::string& line) { lines.push_back(line); });
    return lines;
}

// "key=value" lines from ffprobe -show_entries ... -of default=noprint_wrappers=1
std::string Value(const std::vector<std::string>& lines, const std::string& key,
                  size_t occurrence = 0)
{
    for (const auto& line : lines) {
        if (line.rfind(key + "=", 0) == 0) {
            if (occurrence == 0)
                return line.substr(key.size() + 1);
            --occurrence;
        }
    }
    return {};
}

FfmpegRecorder::Config BaseConfig(VideoCaptureCodec codec, const std::string& name)
{
    FfmpegRecorder::Config config;
    config.path = PathUtf8(OutputDir() / name);
    config.executable = "ffmpeg";
    config.codec = codec;
    config.quality = 18;
    config.width = 64;
    config.height = 48;
    config.scaleX = 1;
    config.scaleY = 2;
    config.aspectNum = 12544; // 720/574, the PAL shape
    config.aspectDen = 10000;
    config.fps = 50;
    config.sampleRate = 44100;
    config.channels = 2;
    return config;
}

std::vector<uint8_t> Frame(int w, int h, int n)
{
    std::vector<uint8_t> f(static_cast<size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = f.data() + (static_cast<size_t>(y) * w + x) * 4;
            p[0] = static_cast<uint8_t>((x + n) * 9);
            p[1] = static_cast<uint8_t>(y * 5 + n);
            p[2] = static_cast<uint8_t>((x ^ y) * 3);
        }
    return f;
}

bool WaitForFinish(const FfmpegRecorder& recorder)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (recorder.IsFinishing()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

bool FfmpegAvailable()
{
    static const bool available = FfmpegTools::Probe("").found;
    return available;
}

// --- command lines ------------------------------------------------------------------------

TEST(FfmpegRecorder, EncodeArgsDeclareThePixelShapeAndTheCodec)
{
    auto config = BaseConfig(VideoCaptureCodec::H264, "x.mp4");
    config.width = 720;
    config.height = 287;
    const auto args = FfmpegRecorder::EncodeArgs(config, 720, 574, "v.mkv");
    EXPECT_TRUE(Contains(args, "-f", "rawvideo"));
    EXPECT_TRUE(Contains(args, "-pix_fmt", "rgb24"));
    EXPECT_TRUE(Contains(args, "-s", "720x574"));
    EXPECT_TRUE(Contains(args, "-framerate", "50"));
    EXPECT_TRUE(Contains(args, "-i", "pipe:0"));
    EXPECT_TRUE(Contains(args, "-c:v", "libx264"));
    EXPECT_TRUE(Contains(args, "-crf", "18"));
    EXPECT_TRUE(Contains(args, "-pix_fmt", "yuv420p"));
    // 12544/10000 over 720/574: (12544*574)/(10000*720) = 7200256/7200000 = 28126/28125, square
    // to within the rounding of the aspect.
    EXPECT_TRUE(Contains(args, "-vf", "setsar=28126/28125"))
        << "the display shape must reach the file";
    EXPECT_TRUE(Contains(args, "-f", "matroska"));
    EXPECT_EQ(args.back(), "v.mkv");
}

TEST(FfmpegRecorder, OddSizesArePaddedOnlyForFourTwoZeroCodecs)
{
    auto h264 = BaseConfig(VideoCaptureCodec::H264, "x.mp4");
    h264.aspectNum = h264.aspectDen = 0;
    const auto a = FfmpegRecorder::EncodeArgs(h264, 721, 287, "v.mkv");
    EXPECT_TRUE(Contains(a, "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2"));

    auto ffv1 = BaseConfig(VideoCaptureCodec::Ffv1, "x.mkv");
    ffv1.aspectNum = ffv1.aspectDen = 0;
    const auto b = FfmpegRecorder::EncodeArgs(ffv1, 721, 287, "v.mkv");
    EXPECT_FALSE(Has(b, "-vf")) << "lossless RGB has no even-size rule";
    EXPECT_TRUE(Contains(b, "-c:v", "ffv1"));
    EXPECT_TRUE(Contains(b, "-pix_fmt", "bgr0")) << "FFV1 must keep exact RGB";
}

TEST(FfmpegRecorder, MuxArgsPickTheAudioCodecAndContainerFlags)
{
    const auto h265 = FfmpegRecorder::MuxArgs(BaseConfig(VideoCaptureCodec::H265, "x.mp4"), "v.mkv",
                                              "a.pcm", true);
    EXPECT_TRUE(Contains(h265, "-c:v", "copy")) << "the mux must not re-encode the video";
    EXPECT_TRUE(Contains(h265, "-c:a", "aac"));
    EXPECT_TRUE(Contains(h265, "-tag:v", "hvc1"));
    EXPECT_TRUE(Contains(h265, "-movflags", "+faststart"));
    EXPECT_TRUE(Contains(h265, "-f", "s16le"));
    EXPECT_TRUE(Contains(h265, "-ar", "44100"));
    EXPECT_TRUE(Contains(h265, "-map", "1:a"));

    const auto ffv1 = FfmpegRecorder::MuxArgs(BaseConfig(VideoCaptureCodec::Ffv1, "x.mkv"), "v.mkv",
                                              "a.pcm", true);
    EXPECT_TRUE(Contains(ffv1, "-c:a", "flac")) << "a lossless film gets lossless sound";
    EXPECT_FALSE(Has(ffv1, "-movflags"));

    const auto silent = FfmpegRecorder::MuxArgs(BaseConfig(VideoCaptureCodec::H264, "x.mp4"),
                                                "v.mkv", "a.pcm", false);
    EXPECT_FALSE(Has(silent, "1:a"));
    EXPECT_FALSE(Has(silent, "s16le"));
}

TEST(FfmpegRecorder, ToolsExplainWhyACodecCannotBeUsed)
{
    FfmpegTools missing;
    missing.error = "FFmpeg is not installed, or not on PATH.";
    EXPECT_EQ(missing.UnavailableReason(VideoCaptureCodec::H264), missing.error);
    EXPECT_TRUE(missing.UnavailableReason(VideoCaptureCodec::Zmbv).empty())
        << "the built-in codecs never need FFmpeg";

    FfmpegTools partial;
    partial.found = true;
    partial.encoders = {"libx264", "ffv1"};
    EXPECT_TRUE(partial.UnavailableReason(VideoCaptureCodec::H264).empty());
    const auto why = partial.UnavailableReason(VideoCaptureCodec::H265);
    EXPECT_NE(why.find("libx265"), std::string::npos) << why;

    const auto nowhere = FfmpegTools::Probe(PathUtf8(OutputDir() / "no-ffmpeg-here.exe"));
    EXPECT_FALSE(nowhere.found);
    EXPECT_FALSE(nowhere.error.empty());
}

// APP-11: a "FFmpeg" that never answers (a GUI program, a wrong file) is stopped, and the probe
// says so, rather than holding whoever waits on it
TEST(FfmpegRecorder, AProbeOfAProgramThatNeverAnswersTimesOut)
{
#ifdef _WIN32
    const auto program = OutputDir() / "never-answers.bat";
    std::ofstream(program) << "@ping -n 30 127.0.0.1 >nul\r\n";
#else
    const auto program = OutputDir() / "never-answers.sh";
    std::ofstream(program) << "#!/bin/sh\nsleep 30\n";
    std::filesystem::permissions(program, std::filesystem::perms::owner_all);
#endif
    const auto t0 = std::chrono::steady_clock::now();
    const auto tools = FfmpegTools::Probe(PathUtf8(program), 500);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_FALSE(tools.found);
    EXPECT_NE(tools.error.find("did not answer"), std::string::npos) << tools.error;
    EXPECT_LT(seconds, 5.0);
}

TEST(FfmpegRecorder, ARecorderThatCannotStartSaysSo)
{
    auto config = BaseConfig(VideoCaptureCodec::H264, "never.mp4");
    config.executable = "definitely-not-an-ffmpeg-binary";
    FfmpegRecorder recorder;
    std::string error;
    EXPECT_FALSE(recorder.Begin(config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(recorder.IsActive());
}

// --- real recordings ----------------------------------------------------------------------

TEST(FfmpegRecorder, ProbeFindsTheInstalledEncoders)
{
    if (!FfmpegAvailable())
        GTEST_SKIP() << "ffmpeg is not on PATH";
    const auto tools = FfmpegTools::Probe("");
    EXPECT_TRUE(tools.found);
    EXPECT_TRUE(tools.encoders.count("ffv1")) << "ffv1 is part of every ffmpeg build";
    EXPECT_EQ(tools.version.rfind("FFmpeg ", 0), 0u) << tools.version;
    EXPECT_EQ(tools.version.find("Copyright"), std::string::npos)
        << "the banner is too long for the window: " << tools.version;
}

// FFV1 is lossless, so the film has to decode to exactly the frames given - scaled, not blended.
TEST(FfmpegRecorder, Ffv1RecordsEveryPixelExactlyWithSound)
{
    if (!FfmpegAvailable())
        GTEST_SKIP() << "ffmpeg is not on PATH";
    constexpr int kFrames = 20;
    auto config = BaseConfig(VideoCaptureCodec::Ffv1, "lossless.mkv");
    std::error_code ec;
    std::filesystem::remove(
        std::filesystem::path(std::u8string(config.path.begin(), config.path.end())), ec);

    std::vector<std::vector<uint8_t>> frames;
    FfmpegRecorder recorder;
    std::string error;
    ASSERT_TRUE(recorder.Begin(config, error)) << error;
    std::vector<int16_t> audio(882 * 2);
    for (int f = 0; f < kFrames; ++f) {
        frames.push_back(Frame(64, 48, f));
        ASSERT_TRUE(recorder.WriteVideoFrame(frames.back().data(), 64, 48)) << "frame " << f;
        for (size_t i = 0; i < audio.size(); ++i)
            audio[i] = static_cast<int16_t>((i / 2 + f * 882) % 100 * 50);
        recorder.WriteAudio(audio.data(), 882);
    }
    ASSERT_TRUE(recorder.End());
    ASSERT_TRUE(WaitForFinish(recorder)) << "the mux never finished";
    EXPECT_FALSE(recorder.HasFailed()) << recorder.ResultMessage();
    EXPECT_NE(recorder.ResultMessage().find("Saved 20 frames"), std::string::npos)
        << recorder.ResultMessage();

    int rc = 0;
    const auto info =
        RunTool({"ffprobe", "-v", "error", "-show_entries",
                 "stream=codec_name,width,height,sample_rate,channels,nb_read_frames",
                 "-count_frames", "-of", "default=noprint_wrappers=1", recorder.Path()},
                rc);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(Value(info, "codec_name", 0), "ffv1");
    EXPECT_EQ(Value(info, "width"), "64");
    EXPECT_EQ(Value(info, "height"), "96") << "lines doubled";
    EXPECT_EQ(Value(info, "codec_name", 1), "flac");
    EXPECT_EQ(Value(info, "sample_rate"), "44100");
    EXPECT_EQ(Value(info, "nb_read_frames", 0), "20");

    const auto raw = OutputDir() / "lossless.raw";
    RunTool({"ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i",
             recorder.Path(), "-map", "0:v", "-f", "rawvideo", "-pix_fmt", "rgb24", PathUtf8(raw)},
            rc);
    ASSERT_EQ(rc, 0);
    std::ifstream in(raw, std::ios::binary);
    const std::vector<uint8_t> decoded((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
    const size_t outFrame = 64u * 96u * 3u;
    ASSERT_EQ(decoded.size(), outFrame * kFrames);
    for (int f = 0; f < kFrames; ++f)
        for (int y = 0; y < 96; ++y)
            for (int x = 0; x < 64; ++x) {
                const uint8_t* want = frames[f].data() + (static_cast<size_t>(y / 2) * 64 + x) * 4;
                const uint8_t* got =
                    decoded.data() + f * outFrame + (static_cast<size_t>(y) * 64 + x) * 3;
                if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2])
                    FAIL() << "frame " << f << " pixel " << x << "," << y << " differs";
            }

    // Nothing left behind but the film.
    const auto dir = OutputDir();
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const auto name = entry.path().filename().string();
        EXPECT_EQ(name.find("lossless.capture-"), std::string::npos) << "left behind: " << name;
    }
}

TEST(FfmpegRecorder, H264RecordsAPlayableMp4WithTheDisplayShape)
{
    if (!FfmpegAvailable())
        GTEST_SKIP() << "ffmpeg is not on PATH";
    const auto tools = FfmpegTools::Probe("");
    if (!tools.UnavailableReason(VideoCaptureCodec::H264).empty())
        GTEST_SKIP() << tools.UnavailableReason(VideoCaptureCodec::H264);

    auto config = BaseConfig(VideoCaptureCodec::H264, "playable.avi"); // wrong extension on purpose
    config.width = 160;
    config.height = 64;
    config.aspectNum = 25000; // 2.5 displayed from a 160x128 frame: pixels twice as wide as tall
    config.aspectDen = 10000;
    FfmpegRecorder recorder;
    std::string error;
    ASSERT_TRUE(recorder.Begin(config, error)) << error;
    EXPECT_EQ(std::filesystem::path(recorder.Path()).extension(), ".mp4")
        << "the container follows the codec";
    std::vector<int16_t> audio(882 * 2, 0);
    for (int f = 0; f < 25; ++f) {
        const auto frame = Frame(160, 64, f);
        ASSERT_TRUE(recorder.WriteVideoFrame(frame.data(), 160, 64));
        recorder.WriteAudio(audio.data(), 882);
    }
    ASSERT_TRUE(recorder.End());
    ASSERT_TRUE(WaitForFinish(recorder));
    ASSERT_FALSE(recorder.HasFailed()) << recorder.ResultMessage();

    int rc = 0;
    const auto info = RunTool({"ffprobe", "-v", "error", "-show_entries",
                               "stream=codec_name,width,height,sample_aspect_ratio,"
                               "display_aspect_ratio,pix_fmt",
                               "-of", "default=noprint_wrappers=1", recorder.Path()},
                              rc);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(Value(info, "codec_name", 0), "h264");
    EXPECT_EQ(Value(info, "pix_fmt"), "yuv420p");
    EXPECT_EQ(Value(info, "width"), "160");
    EXPECT_EQ(Value(info, "height"), "128");
    EXPECT_EQ(Value(info, "display_aspect_ratio"), "5:2") << "the shape must survive the mux";
    EXPECT_EQ(Value(info, "codec_name", 1), "aac");
}

} // namespace
