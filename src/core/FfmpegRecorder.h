#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Subprocess.h"
#include "VideoCaptureSink.h"

namespace adm::core {

// Where FFmpeg is and what it can encode. FFmpeg is not bundled; the capture window greys out
// each codec with the reason when this says it cannot be used.
struct FfmpegTools
{
    bool found = false;
    std::string executable; // what to run: the configured path, or "ffmpeg" from PATH
    std::string version;    // "FFmpeg 9.0-essentials_build-www.gyan.dev"
    std::string error;      // why it was not found
    std::set<std::string> encoders;

    // Runs `<ffmpeg> -hide_banner -encoders`. A few hundred milliseconds: call it off the UI
    // thread. An empty `configuredPath` looks on PATH. A program that has not answered in
    // `timeoutMs` is stopped, and the probe says so.
    static FfmpegTools Probe(const std::string& configuredPath, int timeoutMs = 5000);
    // Empty when the codec can be used, otherwise why not.
    std::string UnavailableReason(VideoCaptureCodec codec) const;
};

// A capture encoded by FFmpeg.
//
// While recording, frames go down a pipe to `ffmpeg -f rawvideo` writing a video-only temporary
// file, through a writer thread with a bounded queue so an encoder hiccup does not stall the
// emulation thread (a queue that stays full does block it, which keeps the streams in step
// rather than dropping frames). The sound goes to a temporary raw PCM file. End() hands both to
// a finishing thread that waits for the encoder, muxes the two into the final file with the
// video copied, and deletes the temporaries.
class FfmpegRecorder final : public VideoCaptureSink
{
public:
    struct Config
    {
        std::string path; // the final file
        std::string executable;
        VideoCaptureCodec codec = VideoCaptureCodec::H264;
        int quality = 18; // CRF, for H.264/H.265
        int width = 0;
        int height = 0;
        int scaleX = 1;
        int scaleY = 1;
        int aspectNum = 0; // display aspect, as AviRecorder takes it
        int aspectDen = 0;
        int fps = 50;
        uint32_t sampleRate = 0; // 0: no sound
        uint32_t channels = 2;
    };

    FfmpegRecorder() = default;
    ~FfmpegRecorder() override; // waits for a finishing mux

    bool Begin(const Config& config, std::string& outError);

    bool WriteVideoFrame(const uint8_t* rgba, int width, int height) override;
    void WriteAudio(const int16_t* interleaved, size_t frames) override;
    bool End() override;

    bool IsActive() const override { return m_active.load(); }
    bool IsFinishing() const override { return m_finishing.load(); }
    bool HasFailed() const override { return m_failed.load(); }
    std::string ResultMessage() const override;

    uint32_t FramesWritten() const override { return m_frames.load(); }
    uint64_t BytesWritten() const override;
    const std::string& Path() const override { return m_config.path; }
    int OutputWidth() const override { return m_outWidth; }
    int OutputHeight() const override { return m_outHeight; }

    // The ffmpeg command lines, for the tests and the log.
    static std::vector<std::string> EncodeArgs(const Config& config, int outWidth, int outHeight,
                                               const std::filesystem::path& videoTemp);
    static std::vector<std::string> MuxArgs(const Config& config,
                                            const std::filesystem::path& videoTemp,
                                            const std::filesystem::path& audioTemp, bool haveAudio);

private:
    void WriterLoop();
    void FinishLoop();
    void Fail(const std::string& why);
    std::string LogTail() const;

    Config m_config;
    int m_outWidth = 0;
    int m_outHeight = 0;
    size_t m_frameBytes = 0;
    size_t m_queueDepth = 0; // frames, from their size
    std::filesystem::path m_videoTemp;
    std::filesystem::path m_audioTemp;
    std::filesystem::path m_logFile;

    PipeProcess m_encoder;
    std::ofstream m_audio;
    uint64_t m_audioFrames = 0;

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_finishing{false};
    std::atomic<bool> m_failed{false};
    std::atomic<uint32_t> m_frames{0};

    // The writer thread's queue, and a pool of frame buffers so a steady recording allocates
    // nothing.
    mutable std::mutex m_mutex;
    std::condition_variable m_queued;
    std::condition_variable m_taken;
    std::deque<std::vector<uint8_t>> m_queue;
    std::vector<std::vector<uint8_t>> m_pool;
    bool m_stopping = false;
    std::string m_result;
    std::thread m_writer;
    std::thread m_finisher;

    mutable std::chrono::steady_clock::time_point m_sizeCheckedAt{};
    mutable uint64_t m_sizeCache = 0;
};

} // namespace adm::core
