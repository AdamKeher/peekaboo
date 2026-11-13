#include "core/AudioRecorder.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using adm::core::AudioRecorder;

namespace {

std::filesystem::path TempWav(const char* name)
{
    std::filesystem::path p = std::filesystem::temp_directory_path() / "adm_audio_tests";
    std::filesystem::create_directories(p);
    return p / name;
}

std::vector<uint8_t> ReadAll(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

uint32_t ReadU32(const std::vector<uint8_t>& b, size_t off)
{
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

uint16_t ReadU16(const std::vector<uint8_t>& b, size_t off)
{
    return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

AudioRecorder::Config MakeConfig(const std::filesystem::path& path, bool float32)
{
    AudioRecorder::Config config;
    config.path = path.string();
    config.sampleRate = 44100;
    config.channels = 2;
    config.float32 = float32;
    return config;
}

} // namespace

TEST(AudioRecorderTest, RejectsInvalidConfig)
{
    AudioRecorder rec;
    std::string error;

    AudioRecorder::Config noPath;
    noPath.sampleRate = 44100;
    EXPECT_FALSE(rec.Start(noPath, error));
    EXPECT_FALSE(error.empty());

    AudioRecorder::Config noRate;
    noRate.path = TempWav("invalid.wav").string();
    noRate.sampleRate = 0;
    EXPECT_FALSE(rec.Start(noRate, error));
    EXPECT_FALSE(rec.IsRecording());
}

TEST(AudioRecorderTest, WritesValidPcmWavHeader)
{
    const auto path = TempWav("pcm.wav");
    std::filesystem::remove(path);

    AudioRecorder rec;
    std::string error;
    ASSERT_TRUE(rec.Start(MakeConfig(path, /*float32=*/false), error)) << error;
    EXPECT_TRUE(rec.IsRecording());

    // A 441 Hz tone: exactly 100 samples per cycle at 44100, so the content is easy to reason
    // about if a later test wants to assert on the samples themselves.
    constexpr size_t kFrames = 4410;
    std::vector<int16_t> samples(kFrames * 2);
    for (size_t i = 0; i < kFrames; ++i) {
        const double t = static_cast<double>(i) / 44100.0;
        const auto v = static_cast<int16_t>(std::sin(2.0 * 3.14159265358979 * 441.0 * t) * 16000.0);
        samples[i * 2] = v;
        samples[i * 2 + 1] = v;
    }

    rec.PushInt16(samples.data(), kFrames);
    rec.Drain();
    rec.Stop();
    EXPECT_FALSE(rec.IsRecording());

    const auto bytes = ReadAll(path);
    ASSERT_GT(bytes.size(), 44u);
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "RIFF");
    EXPECT_EQ(std::string(bytes.begin() + 8, bytes.begin() + 12), "WAVE");
    EXPECT_EQ(ReadU16(bytes, 20), 1u);     // WAVE_FORMAT_PCM
    EXPECT_EQ(ReadU16(bytes, 22), 2u);     // channels
    EXPECT_EQ(ReadU32(bytes, 24), 44100u); // sample rate
    EXPECT_EQ(ReadU16(bytes, 34), 16u);    // bits per sample

    // Header sizes must be patched on close, or every player reports a zero-length file.
    const uint32_t dataBytes = kFrames * 2 * sizeof(int16_t);
    EXPECT_EQ(ReadU32(bytes, 40), dataBytes);
    EXPECT_EQ(ReadU32(bytes, 4), 36 + dataBytes);
    EXPECT_EQ(bytes.size(), 44 + dataBytes);

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".json");
}

TEST(AudioRecorderTest, WritesValidFloatWavHeader)
{
    const auto path = TempWav("float.wav");
    std::filesystem::remove(path);

    AudioRecorder rec;
    std::string error;
    ASSERT_TRUE(rec.Start(MakeConfig(path, /*float32=*/true), error)) << error;

    constexpr size_t kFrames = 512;
    std::vector<float> samples(kFrames * 2, 0.25f);
    rec.PushFloat(samples.data(), kFrames);
    rec.Drain();
    rec.Stop();

    const auto bytes = ReadAll(path);
    ASSERT_GT(bytes.size(), 44u);
    EXPECT_EQ(ReadU16(bytes, 20), 3u);  // WAVE_FORMAT_IEEE_FLOAT
    EXPECT_EQ(ReadU16(bytes, 34), 32u); // bits per sample
    EXPECT_EQ(ReadU32(bytes, 40), kFrames * 2 * sizeof(float));

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".json");
}

TEST(AudioRecorderTest, StatsTrackWrittenFrames)
{
    const auto path = TempWav("stats.wav");
    std::filesystem::remove(path);

    AudioRecorder rec;
    std::string error;
    ASSERT_TRUE(rec.Start(MakeConfig(path, false), error)) << error;

    std::vector<int16_t> samples(882 * 2, 0);
    rec.PushInt16(samples.data(), 882);
    rec.Drain();

    const auto stats = rec.GetStats();
    EXPECT_TRUE(stats.recording);
    EXPECT_EQ(stats.framesWritten, 882u);
    EXPECT_EQ(stats.framesDroppedStaging, 0u);
    EXPECT_NEAR(stats.seconds, 882.0 / 44100.0, 1e-9);

    rec.Stop();
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".json");
}

TEST(AudioRecorderTest, MaxSecondsStopsCaptureAutomatically)
{
    const auto path = TempWav("limited.wav");
    std::filesystem::remove(path);

    AudioRecorder rec;
    std::string error;
    auto config = MakeConfig(path, false);
    config.maxSeconds = 0.01; // 441 frames
    ASSERT_TRUE(rec.Start(config, error)) << error;

    std::vector<int16_t> samples(1000 * 2, 0);
    rec.PushInt16(samples.data(), 1000);
    rec.Drain();

    EXPECT_FALSE(rec.IsRecording()) << "capture should stop itself once maxSeconds is reached";

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".json");
}

TEST(AudioRecorderTest, PushIsIgnoredWhenNotRecording)
{
    AudioRecorder rec;
    std::vector<int16_t> samples(64 * 2, 0);
    // Must not crash or allocate — the producer calls this on the hot path regardless of state.
    rec.PushInt16(samples.data(), 64);
    rec.PushBlock({});
    rec.Drain();
    EXPECT_EQ(rec.GetStats().framesWritten, 0u);
}

TEST(AudioRecorderTest, SidecarRecordsBlockAccounting)
{
    const auto path = TempWav("sidecar.wav");
    const auto sidecar = path.string() + ".json";
    std::filesystem::remove(path);
    std::filesystem::remove(sidecar);

    AudioRecorder rec;
    std::string error;
    ASSERT_TRUE(rec.Start(MakeConfig(path, false), error)) << error;

    std::vector<int16_t> samples(128 * 2, 0);
    for (int i = 0; i < 3; ++i) {
        AudioRecorder::BlockInfo block;
        block.frameIndex = static_cast<uint64_t>(i) * 128;
        block.frames = 128;
        block.ringLevel = 900;
        block.held = (i == 1) ? 17u : 0u; // one starved block, to prove held frames survive
        rec.PushInt16(samples.data(), 128);
        rec.PushBlock(block);
    }
    rec.Drain();
    rec.Stop();

    std::string json;
    {
        std::ifstream in(sidecar);
        ASSERT_TRUE(in.good());
        json.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    EXPECT_NE(json.find("\"blockCount\": 3"), std::string::npos) << json;
    EXPECT_NE(json.find("\"held\": 17"), std::string::npos) << json;
    EXPECT_NE(json.find("\"framesWritten\": 384"), std::string::npos) << json;

    std::filesystem::remove(path);
    std::filesystem::remove(sidecar);
}

// The device tap's producer is the miniaudio callback thread while Drain() runs on the main
// thread, so the staging ring must survive genuine concurrent use.
TEST(AudioRecorderTest, SurvivesConcurrentProducerAndDrain)
{
    const auto path = TempWav("threaded.wav");
    std::filesystem::remove(path);

    AudioRecorder rec;
    std::string error;
    ASSERT_TRUE(rec.Start(MakeConfig(path, true), error)) << error;

    constexpr size_t kBlocks = 400;
    constexpr size_t kBlockFrames = 256;

    std::atomic<bool> producing{true};
    std::thread producer([&] {
        std::vector<float> samples(kBlockFrames * 2, 0.5f);
        for (size_t i = 0; i < kBlocks; ++i)
            rec.PushFloat(samples.data(), kBlockFrames);
        producing.store(false);
    });

    // Drain concurrently, the way Application::RunFrame does, until the producer is finished and
    // everything it staged has been written.
    while (producing.load() || rec.GetStats().framesWritten < kBlocks * kBlockFrames) {
        rec.Drain();
        std::this_thread::yield();
    }
    producer.join();
    rec.Drain();
    rec.Stop();

    const auto stats = rec.GetStats();
    EXPECT_EQ(stats.framesDroppedStaging, 0u)
        << "staging ring overflowed; a lost capture looks exactly like the bug being hunted";
    EXPECT_EQ(stats.framesWritten, kBlocks * kBlockFrames);

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".json");
}
