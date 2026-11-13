#include "FfmpegRecorder.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>

#include <fmt/format.h>

#include "CaptureFrame.h"
#include "Logger.h"

#ifndef _WIN32
#include <pthread.h>
#include <csignal>
#endif

namespace adm::core {

namespace {

// Frames the writer thread may fall behind by before the emulation thread waits for it. A
// second at 50 fps, about 60 MB at 720x574: enough to ride out an encoder keyframe or a disk
// stall without holding the machine up. Fewer for big frames - a 4K one is 25 MB - so the queue
// stays within about 256 MB.
constexpr size_t kQueueDepth = 50;
constexpr size_t kQueueBytes = 256u << 20;

bool IsFourTwoZero(VideoCaptureCodec codec)
{
    return codec == VideoCaptureCodec::H264 || codec == VideoCaptureCodec::H265;
}

std::filesystem::path PathFromUtf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string PathUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string Trim(std::string s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

} // namespace

// --- FfmpegTools ---------------------------------------------------------------------------

namespace {

// The probe runs whatever executable the user picked: one that never exits (a GUI program, a
// wrong file) is stopped after `ms`, so the probe - and a UI waiting on it - always ends.
int RunWithTimeout(Subprocess& p, const std::vector<std::string>& args,
                   const std::function<void(const std::string&)>& onLine, int ms, bool& timedOut)
{
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    timedOut = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(m);
        if (!cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return done; })) {
            timedOut = true;
            p.Cancel();
        }
    });
    const int rc = p.Run(args, onLine);
    {
        std::lock_guard<std::mutex> lock(m);
        done = true;
    }
    cv.notify_all();
    watchdog.join();
    return rc;
}

} // namespace

FfmpegTools FfmpegTools::Probe(const std::string& configuredPath, int timeoutMs)
{
    FfmpegTools tools;
    if (!configuredPath.empty()) {
        std::error_code ec;
        const std::filesystem::path p = PathFromUtf8(configuredPath);
        if (!std::filesystem::is_regular_file(p, ec)) {
            tools.error = "FFmpeg was not found at " + configuredPath + ".";
            return tools;
        }
        tools.executable = configuredPath;
    } else {
        tools.executable = "ffmpeg";
    }

    Subprocess list;
    std::vector<std::string> lines;
    bool timedOut = false;
    const int rc = RunWithTimeout(
        list, {tools.executable, "-hide_banner", "-encoders"},
        [&](const std::string& line) { lines.push_back(line); }, timeoutMs, timedOut);
    if (timedOut) {
        tools.error = (configuredPath.empty() ? std::string("FFmpeg") : configuredPath) +
                      " did not answer within " + std::to_string(timeoutMs / 1000) +
                      " s; it may not be FFmpeg.";
        return tools;
    }
    if (rc == -1) {
        tools.error = configuredPath.empty()
                          ? "FFmpeg is not installed, or not on PATH. Set its location below."
                          : "FFmpeg at " + configuredPath + " would not start.";
        return tools;
    }
    if (rc != 0) {
        tools.error = "FFmpeg would not list its encoders (exit code " + std::to_string(rc) + ").";
        return tools;
    }
    // " V....D libx264              libx264 H.264 / AVC ..." - a six-character flag field, then
    // the encoder's name.
    for (const auto& line : lines) {
        std::istringstream in(line);
        std::string flags, name;
        if (!(in >> flags >> name))
            continue;
        if (flags.size() == 6 && (flags[0] == 'V' || flags[0] == 'A' || flags[0] == 'S'))
            tools.encoders.insert(name);
    }

    // "ffmpeg version 9.0-essentials_build-www.gyan.dev Copyright (c) 2000-2026 ..." becomes
    // "FFmpeg 9.0-essentials_build-www.gyan.dev": the banner does not fit the window.
    Subprocess version;
    RunWithTimeout(
        version, {tools.executable, "-version"},
        [&](const std::string& line) {
            if (!tools.version.empty())
                return;
            std::istringstream in(line);
            std::string word, number;
            if (in >> word >> word >> number && word == "version")
                tools.version = "FFmpeg " + number;
            else
                tools.version = Trim(line);
        },
        timeoutMs, timedOut);
    tools.found = !tools.encoders.empty();
    if (!tools.found)
        tools.error = "FFmpeg listed no encoders.";
    return tools;
}

std::string FfmpegTools::UnavailableReason(VideoCaptureCodec codec) const
{
    const auto& info = GetVideoCaptureCodecInfo(codec);
    if (!info.needsFfmpeg)
        return {};
    if (!found)
        return error.empty() ? "FFmpeg was not found." : error;
    if (info.ffmpegEncoder && !encoders.count(info.ffmpegEncoder))
        return fmt::format("This FFmpeg was built without the {} encoder.", info.ffmpegEncoder);
    return {};
}

// --- command lines -------------------------------------------------------------------------

std::vector<std::string> FfmpegRecorder::EncodeArgs(const Config& config, int outWidth,
                                                    int outHeight,
                                                    const std::filesystem::path& videoTemp)
{
    std::vector<std::string> args = {config.executable,
                                     "-hide_banner",
                                     "-loglevel",
                                     "error",
                                     "-y",
                                     "-f",
                                     "rawvideo",
                                     "-pix_fmt",
                                     "rgb24",
                                     "-s",
                                     fmt::format("{}x{}", outWidth, outHeight),
                                     "-framerate",
                                     std::to_string(config.fps),
                                     "-i",
                                     "pipe:0",
                                     "-an"};

    std::vector<std::string> filters;
    if (config.aspectNum > 0 && config.aspectDen > 0) {
        // The pixel shape that shows the whole frame at the display aspect.
        long long num = static_cast<long long>(config.aspectNum) * outHeight;
        long long den = static_cast<long long>(config.aspectDen) * outWidth;
        const long long g = std::gcd(num, den);
        if (g > 0) {
            num /= g;
            den /= g;
        }
        filters.push_back(fmt::format("setsar={}/{}", num, den));
    }
    // 4:2:0 needs even dimensions; pad rather than drop a row or column.
    if (IsFourTwoZero(config.codec) && ((outWidth | outHeight) & 1))
        filters.push_back("pad=ceil(iw/2)*2:ceil(ih/2)*2");
    if (!filters.empty()) {
        std::string joined;
        for (const auto& f : filters)
            joined += (joined.empty() ? "" : ",") + f;
        args.push_back("-vf");
        args.push_back(joined);
    }

    const std::string crf = std::to_string(std::clamp(config.quality, 0, 51));
    switch (config.codec) {
        case VideoCaptureCodec::H264:
            // yuv420p for players; each lores pixel is already a 2x2 block, so it survives.
            args.insert(args.end(), {"-c:v", "libx264", "-preset", "veryfast", "-crf", crf,
                                     "-pix_fmt", "yuv420p"});
            break;
        case VideoCaptureCodec::H265:
            args.insert(args.end(), {"-c:v", "libx265", "-preset", "veryfast", "-crf", crf,
                                     "-pix_fmt", "yuv420p", "-x265-params", "log-level=error"});
            break;
        case VideoCaptureCodec::Ffv1:
            // bgr0 keeps every colour exact; every frame a keyframe, for editing.
            args.insert(args.end(), {"-c:v", "ffv1", "-level", "3", "-slices", "4", "-slicecrc",
                                     "1", "-g", "1", "-pix_fmt", "bgr0"});
            break;
        default:
            break;
    }
    // Matroska takes all three, and is only the intermediate.
    args.insert(args.end(), {"-f", "matroska", PathUtf8(videoTemp)});
    return args;
}

std::vector<std::string> FfmpegRecorder::MuxArgs(const Config& config,
                                                 const std::filesystem::path& videoTemp,
                                                 const std::filesystem::path& audioTemp,
                                                 bool haveAudio)
{
    std::vector<std::string> args = {
        config.executable, "-hide_banner", "-loglevel", "error",
        "-nostdin",        "-y",           "-i",        PathUtf8(videoTemp)};
    if (haveAudio) {
        args.insert(args.end(), {"-f", "s16le", "-ar", std::to_string(config.sampleRate), "-ac",
                                 std::to_string(config.channels), "-i", PathUtf8(audioTemp)});
    }
    args.insert(args.end(), {"-map", "0:v"});
    if (haveAudio)
        args.insert(args.end(), {"-map", "1:a"});
    args.insert(args.end(), {"-c:v", "copy"});

    if (config.codec == VideoCaptureCodec::Ffv1) {
        if (haveAudio)
            args.insert(args.end(), {"-c:a", "flac"});
    } else {
        if (haveAudio)
            args.insert(args.end(), {"-c:a", "aac", "-b:a", "192k"});
        // The index at the front, so a browser or an upload can start playing before the end.
        args.insert(args.end(), {"-movflags", "+faststart"});
        // Apple's players only recognise HEVC in MP4 under this tag.
        if (config.codec == VideoCaptureCodec::H265)
            args.insert(args.end(), {"-tag:v", "hvc1"});
    }
    args.push_back(config.path);
    return args;
}

// --- recording -----------------------------------------------------------------------------

FfmpegRecorder::~FfmpegRecorder()
{
    End();
    if (m_finisher.joinable())
        m_finisher.join();
    if (m_writer.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_queued.notify_all();
        m_writer.join();
    }
}

bool FfmpegRecorder::Begin(const Config& config, std::string& outError)
{
    outError.clear();
    if (m_active || m_finishing) {
        outError = "a recording is already in progress";
        return false;
    }
    const auto& info = GetVideoCaptureCodecInfo(config.codec);
    if (!info.needsFfmpeg) {
        outError = "that codec is not encoded by FFmpeg";
        return false;
    }
    if (config.width <= 0 || config.height <= 0 || config.fps <= 0 || config.scaleX < 1 ||
        config.scaleY < 1) {
        outError = "the frame size is not known yet";
        return false;
    }
    if (config.executable.empty()) {
        outError = "FFmpeg was not found";
        return false;
    }

    m_config = config;
    // The container follows the codec: ffmpeg picks the muxer from the extension.
    std::filesystem::path finalPath = PathFromUtf8(config.path);
    finalPath.replace_extension(info.extension);
    m_config.path = PathUtf8(finalPath);

    m_outWidth = config.width * config.scaleX;
    m_outHeight = config.height * config.scaleY;
    m_frameBytes = CaptureFrameBytes(CaptureLayout::RgbTopDown, m_outWidth, m_outHeight);
    m_queueDepth =
        std::clamp<size_t>(kQueueBytes / std::max<size_t>(m_frameBytes, 1), 4, kQueueDepth);

    std::error_code ec;
    const auto dir = finalPath.parent_path();
    if (!dir.empty())
        std::filesystem::create_directories(dir, ec);
    const auto stem = finalPath.stem().u8string();
    const auto sibling = [&](const char* suffix) {
        auto name = stem;
        const std::string s = suffix;
        name.append(s.begin(), s.end());
        return dir / std::filesystem::path(name);
    };
    m_videoTemp = sibling(".capture-video.tmp.mkv");
    m_audioTemp = sibling(".capture-audio.tmp.pcm");
    m_logFile = sibling(".capture-ffmpeg.log");

    const auto args = EncodeArgs(m_config, m_outWidth, m_outHeight, m_videoTemp);
    if (!m_encoder.Start(args, m_logFile, outError))
        return false;

    m_audio.open(m_audioTemp, std::ios::binary | std::ios::trunc);
    if (!m_audio) {
        m_encoder.Kill();
        outError = "could not create " + PathUtf8(m_audioTemp);
        return false;
    }

    m_audioFrames = 0;
    m_frames = 0;
    m_failed = false;
    m_stopping = false;
    m_result.clear();
    m_queue.clear();
    m_sizeCache = 0;
    m_active = true;
    m_writer = std::thread([this] { WriterLoop(); });

    std::string commandLine;
    for (const auto& a : args)
        commandLine += (commandLine.empty() ? "" : " ") + a;
    Logger::Info("Capture: {}", commandLine);
    return true;
}

bool FfmpegRecorder::WriteVideoFrame(const uint8_t* rgba, int width, int height)
{
    if (!m_active || m_failed || !rgba)
        return false;
    if (width != m_config.width || height != m_config.height)
        return false;

    std::vector<uint8_t> buffer;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        // Back-pressure: past the queue depth the machine waits for the encoder rather than
        // dropping frames, which would put the sound out of step.
        m_taken.wait(lock, [&] { return m_queue.size() < m_queueDepth || m_failed.load(); });
        if (m_failed)
            return false;
        if (!m_pool.empty()) {
            buffer = std::move(m_pool.back());
            m_pool.pop_back();
        }
    }
    buffer.resize(m_frameBytes);
    ConvertCaptureFrame(rgba, width, height, m_config.scaleX, m_config.scaleY,
                        CaptureLayout::RgbTopDown, buffer.data());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(buffer));
    }
    m_queued.notify_one();
    ++m_frames;
    return true;
}

void FfmpegRecorder::WriteAudio(const int16_t* interleaved, size_t frames)
{
    if (!m_active || !interleaved || frames == 0 || m_config.sampleRate == 0)
        return;
    // Little-endian int16 is what -f s16le reads, and what every host this builds for stores.
    if (m_failed)
        return;
    m_audio.write(reinterpret_cast<const char*>(interleaved),
                  static_cast<std::streamsize>(frames * m_config.channels * sizeof(int16_t)));
    if (!m_audio) {
        Fail("could not write the sound to " + PathUtf8(m_audioTemp) + " (is the disk full?)");
        return;
    }
    m_audioFrames += frames;
}

void FfmpegRecorder::WriterLoop()
{
#ifndef _WIN32
    // A write to an encoder that has died raises SIGPIPE, which would end the whole program.
    // Blocked on this thread only, the write fails with EPIPE instead.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
#endif
    for (;;) {
        std::vector<uint8_t> buffer;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queued.wait(lock, [&] { return !m_queue.empty() || m_stopping; });
            if (m_queue.empty())
                break; // stopping, and everything queued has gone
            buffer = std::move(m_queue.front());
            m_queue.pop_front();
        }
        const bool ok = !m_failed && m_encoder.Write(buffer.data(), buffer.size());
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pool.push_back(std::move(buffer));
        }
        m_taken.notify_one();
        if (!ok && !m_failed)
            Fail("FFmpeg stopped taking frames");
    }
}

void FfmpegRecorder::Fail(const std::string& why)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_result.empty())
            m_result = why;
    }
    m_failed = true;
    m_taken.notify_all();
    Logger::Error("Capture: {}", why);
}

bool FfmpegRecorder::End()
{
    if (!m_active.exchange(false))
        return false;
    m_finishing = true;
    if (m_finisher.joinable())
        m_finisher.join();
    m_finisher = std::thread([this] { FinishLoop(); });
    return true;
}

void FfmpegRecorder::FinishLoop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_queued.notify_all();
    if (m_writer.joinable())
        m_writer.join();

    // Closing the pipe tells the encoder the film is over; it flushes and exits.
    const int encodeRc = m_encoder.Finish();
    m_audio.close();
    const uint32_t frames = m_frames.load();

    std::error_code ec;
    std::string result;
    bool keepTemps = false;
    if (m_failed || encodeRc != 0) {
        std::string why;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            why = m_result;
        }
        if (why.empty())
            why = fmt::format("FFmpeg failed (exit code {})", encodeRc);
        const std::string tail = LogTail();
        result = tail.empty() ? why + "." : why + ": " + tail;
        std::filesystem::remove(m_videoTemp, ec);
        std::filesystem::remove(m_audioTemp, ec);
        // The log stays, so the failure can be read in full.
    } else {
        const bool haveAudio = m_audioFrames > 0;
        Subprocess mux;
        std::string output;
        const int muxRc = mux.Run(
            MuxArgs(m_config, m_videoTemp, m_audioTemp, haveAudio),
            [&](const std::string& line) { output += (output.empty() ? "" : " / ") + line; });
        if (muxRc != 0) {
            // The encoded video is kept: it is the expensive part, and can be muxed by hand.
            keepTemps = true;
            result = fmt::format("FFmpeg could not write {} ({}). The encoded video was kept as "
                                 "{}.",
                                 m_config.path, output.empty() ? "no message" : output,
                                 PathUtf8(m_videoTemp));
        } else {
            result = fmt::format("Saved {} frames to {}", frames, m_config.path);
        }
        if (!keepTemps) {
            std::filesystem::remove(m_videoTemp, ec);
            std::filesystem::remove(m_audioTemp, ec);
            std::filesystem::remove(m_logFile, ec);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result = result;
    }
    Logger::Info("Capture: {}", result);
    m_finishing = false;
}

std::string FfmpegRecorder::ResultMessage() const
{
    if (m_active || m_finishing)
        return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_result;
}

uint64_t FfmpegRecorder::BytesWritten() const
{
    // What is on disk: the encoded video so far plus the sound. A stat, so not every frame.
    const auto now = std::chrono::steady_clock::now();
    if (now - m_sizeCheckedAt > std::chrono::milliseconds(500)) {
        m_sizeCheckedAt = now;
        std::error_code ec;
        uint64_t size = 0;
        if (m_active || m_finishing) {
            const auto v = std::filesystem::file_size(m_videoTemp, ec);
            size += ec ? 0 : v;
            size += m_audioFrames * m_config.channels * sizeof(int16_t);
        } else {
            const auto f = std::filesystem::file_size(PathFromUtf8(m_config.path), ec);
            size = ec ? 0 : f;
        }
        m_sizeCache = size;
    }
    return m_sizeCache;
}

std::string FfmpegRecorder::LogTail() const
{
    std::ifstream in(m_logFile, std::ios::binary);
    if (!in)
        return {};
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff start = std::max<std::streamoff>(0, size - 1500);
    in.seekg(start);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        line = Trim(line);
        if (!line.empty())
            lines.push_back(line);
    }
    std::string tail;
    for (size_t i = lines.size() > 3 ? lines.size() - 3 : 0; i < lines.size(); ++i)
        tail += (tail.empty() ? "" : " / ") + lines[i];
    return tail;
}

} // namespace adm::core
