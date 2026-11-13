#include "AudioRecorder.h"

#include "Logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace adm::core {

namespace {

void PutU32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void PutU16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

constexpr size_t kHeaderBytes = 44;

} // namespace

AudioRecorder::~AudioRecorder() { Stop(); }

bool AudioRecorder::Start(const Config& config, std::string& outError)
{
    if (IsRecording()) {
        outError = "already recording to " + m_config.path;
        return false;
    }
    if (config.path.empty()) {
        outError = "no output path";
        return false;
    }
    if (config.sampleRate == 0) {
        outError = "sample rate is zero (is the emulator running?)";
        return false;
    }
    if (config.channels == 0) {
        outError = "channel count is zero";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(config.path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);

    FILE* file = std::fopen(config.path.c_str(), "wb");
    if (!file) {
        outError = "cannot open " + config.path + " for writing";
        return false;
    }

    m_config = config;
    m_file = file;
    m_framesWritten = 0;
    m_blockCount = 0;
    m_blockReadPos = 0;
    m_blockLog.clear();
    m_stagingDropped.store(0, std::memory_order_relaxed);

    m_staging.assign(kStagingFrames * m_config.channels, 0.0f);
    m_blocks.assign(kMaxBlocks, BlockInfo{});
    m_writePos.store(0, std::memory_order_relaxed);
    m_readPos.store(0, std::memory_order_relaxed);
    m_blockWritePos.store(0, std::memory_order_relaxed);

    WriteHeader();

    // Published last: producers must not see a half-built recorder.
    m_recording.store(true, std::memory_order_release);
    return true;
}

void AudioRecorder::Stop()
{
    if (!m_recording.exchange(false, std::memory_order_acq_rel))
        return;

    // Producers have stopped pushing; take whatever they staged before writing the header back.
    Drain();

    PatchHeader();

    if (m_file) {
        std::fclose(static_cast<FILE*>(m_file));
        m_file = nullptr;
    }

    WriteSidecar();

    const uint64_t dropped = m_stagingDropped.load(std::memory_order_relaxed);
    if (dropped > 0) {
        Logger::LogMessage(LogLevel::Warning,
                           "AudioRecorder: " + std::to_string(dropped) +
                               " frames were lost in the capture staging ring for " +
                               m_config.path + " — treat this recording as unreliable");
    }
}

void AudioRecorder::PushFloat(const float* interleaved, size_t frames)
{
    if (!IsRecording() || !interleaved || frames == 0)
        return;

    const size_t channels = m_config.channels;
    size_t writePos = m_writePos.load(std::memory_order_relaxed);
    const size_t readPos = m_readPos.load(std::memory_order_acquire);

    size_t written = 0;
    for (; written < frames; ++written) {
        const size_t nextWrite = (writePos + 1) % kStagingFrames;
        if (nextWrite == readPos)
            break;
        std::memcpy(&m_staging[writePos * channels], &interleaved[written * channels],
                    channels * sizeof(float));
        writePos = nextWrite;
    }

    m_writePos.store(writePos, std::memory_order_release);
    if (written < frames)
        m_stagingDropped.fetch_add(frames - written, std::memory_order_relaxed);
}

void AudioRecorder::PushInt16(const int16_t* interleaved, size_t frames)
{
    if (!IsRecording() || !interleaved || frames == 0)
        return;

    constexpr float kScale = 1.0f / 32768.0f;
    const size_t channels = m_config.channels;
    size_t writePos = m_writePos.load(std::memory_order_relaxed);
    const size_t readPos = m_readPos.load(std::memory_order_acquire);

    size_t written = 0;
    for (; written < frames; ++written) {
        const size_t nextWrite = (writePos + 1) % kStagingFrames;
        if (nextWrite == readPos)
            break;
        for (size_t ch = 0; ch < channels; ++ch) {
            m_staging[writePos * channels + ch] =
                static_cast<float>(interleaved[written * channels + ch]) * kScale;
        }
        writePos = nextWrite;
    }

    m_writePos.store(writePos, std::memory_order_release);
    if (written < frames)
        m_stagingDropped.fetch_add(frames - written, std::memory_order_relaxed);
}

void AudioRecorder::PushBlock(const BlockInfo& info)
{
    if (!IsRecording())
        return;

    const size_t pos = m_blockWritePos.load(std::memory_order_relaxed);
    m_blocks[pos % kMaxBlocks] = info;
    m_blockWritePos.store(pos + 1, std::memory_order_release);
}

void AudioRecorder::Drain()
{
    if (!m_file)
        return;

    const size_t channels = m_config.channels;
    size_t readPos = m_readPos.load(std::memory_order_relaxed);
    const size_t writePos = m_writePos.load(std::memory_order_acquire);

    size_t available =
        (writePos >= readPos) ? (writePos - readPos) : (kStagingFrames - readPos + writePos);

    if (available > 0) {
        const size_t bytesPerSample = m_config.float32 ? 4u : 2u;
        m_convert.resize(available * channels * bytesPerSample);
        uint8_t* out = m_convert.data();

        for (size_t i = 0; i < available; ++i) {
            for (size_t ch = 0; ch < channels; ++ch) {
                const float sample = m_staging[readPos * channels + ch];
                if (m_config.float32) {
                    std::memcpy(out, &sample, sizeof(float));
                    out += 4;
                } else {
                    const float clamped = std::clamp(sample, -1.0f, 1.0f);
                    const int32_t scaled = static_cast<int32_t>(clamped * 32767.0f);
                    PutU16(out, static_cast<uint16_t>(static_cast<int16_t>(scaled)));
                    out += 2;
                }
            }
            readPos = (readPos + 1) % kStagingFrames;
        }

        std::fwrite(m_convert.data(), 1, m_convert.size(), static_cast<FILE*>(m_file));
        m_readPos.store(readPos, std::memory_order_release);
        m_framesWritten += available;
    }

    // Block metadata rides along. It is a plain ring, so a burst larger than kMaxBlocks loses the
    // oldest entries; the sidecar reports the true count so gaps are visible.
    const size_t blockWrite = m_blockWritePos.load(std::memory_order_acquire);
    if (blockWrite > m_blockReadPos) {
        const size_t total = blockWrite - m_blockReadPos;
        const size_t take = std::min(total, kMaxBlocks);
        for (size_t i = blockWrite - take; i < blockWrite; ++i)
            m_blockLog.push_back(m_blocks[i % kMaxBlocks]);
        m_blockCount += total;
        m_blockReadPos = blockWrite;
    }

    if (m_config.maxSeconds > 0.0 && IsRecording()) {
        const double seconds =
            static_cast<double>(m_framesWritten) / static_cast<double>(m_config.sampleRate);
        if (seconds >= m_config.maxSeconds)
            Stop();
    }
}

AudioRecorder::Stats AudioRecorder::GetStats() const
{
    Stats stats;
    stats.recording = IsRecording();
    stats.path = m_config.path;
    stats.sampleRate = m_config.sampleRate;
    stats.channels = m_config.channels;
    stats.framesWritten = m_framesWritten;
    stats.framesDroppedStaging = m_stagingDropped.load(std::memory_order_relaxed);
    stats.blocks = m_blockCount;
    stats.seconds = m_config.sampleRate ? static_cast<double>(m_framesWritten) /
                                              static_cast<double>(m_config.sampleRate)
                                        : 0.0;
    return stats;
}

void AudioRecorder::WriteHeader()
{
    // Sizes are patched by PatchHeader() once the capture length is known.
    uint8_t header[kHeaderBytes] = {};
    const uint16_t format = m_config.float32 ? 3 : 1; // WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM
    const uint16_t bitsPerSample = m_config.float32 ? 32 : 16;
    const uint16_t blockAlign = static_cast<uint16_t>(m_config.channels * (bitsPerSample / 8));

    std::memcpy(header + 0, "RIFF", 4);
    PutU32(header + 4, 0);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    PutU32(header + 16, 16);
    PutU16(header + 20, format);
    PutU16(header + 22, static_cast<uint16_t>(m_config.channels));
    PutU32(header + 24, m_config.sampleRate);
    PutU32(header + 28, m_config.sampleRate * blockAlign);
    PutU16(header + 32, blockAlign);
    PutU16(header + 34, bitsPerSample);
    std::memcpy(header + 36, "data", 4);
    PutU32(header + 40, 0);

    std::fwrite(header, 1, sizeof(header), static_cast<FILE*>(m_file));
}

void AudioRecorder::PatchHeader()
{
    if (!m_file)
        return;

    const uint16_t bitsPerSample = m_config.float32 ? 32 : 16;
    const uint32_t blockAlign = m_config.channels * (bitsPerSample / 8u);
    const uint32_t dataBytes = static_cast<uint32_t>(m_framesWritten * blockAlign);

    uint8_t buf[4];
    FILE* file = static_cast<FILE*>(m_file);

    bool ok = std::fseek(file, 4, SEEK_SET) == 0;
    PutU32(buf, 36 + dataBytes);
    ok = ok && std::fwrite(buf, 1, 4, file) == 4;

    ok = ok && std::fseek(file, 40, SEEK_SET) == 0;
    PutU32(buf, dataBytes);
    ok = ok && std::fwrite(buf, 1, 4, file) == 4;

    std::fseek(file, 0, SEEK_END);

    // A silently mis-sized header is exactly the failure the audio-validation harness would
    // then chase as a phantom capture bug — say so instead.
    if (!ok)
        Logger::LogMessage(LogLevel::Warning,
                           "AudioRecorder: failed to patch WAV header sizes (disk full or "
                           "read-only?) — declared lengths in " +
                               m_config.path + " are wrong");
}

void AudioRecorder::WriteSidecar()
{
    const std::string path = m_config.path + ".json";
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        Logger::LogMessage(LogLevel::Warning, "AudioRecorder: cannot write sidecar " + path);
        return;
    }

    std::fprintf(file, "{\n");
    std::fprintf(file, "  \"wav\": \"%s\",\n", m_config.path.c_str());
    std::fprintf(file, "  \"sampleRate\": %u,\n", m_config.sampleRate);
    std::fprintf(file, "  \"channels\": %u,\n", m_config.channels);
    std::fprintf(file, "  \"format\": \"%s\",\n", m_config.float32 ? "f32" : "s16");
    std::fprintf(file, "  \"framesWritten\": %llu,\n",
                 static_cast<unsigned long long>(m_framesWritten));
    std::fprintf(file, "  \"seconds\": %.6f,\n",
                 m_config.sampleRate ? static_cast<double>(m_framesWritten) /
                                           static_cast<double>(m_config.sampleRate)
                                     : 0.0);
    std::fprintf(file, "  \"stagingDropped\": %llu,\n",
                 static_cast<unsigned long long>(m_stagingDropped.load(std::memory_order_relaxed)));
    std::fprintf(file, "  \"blockCount\": %llu,\n", static_cast<unsigned long long>(m_blockCount));
    std::fprintf(file, "  \"blocksLogged\": %zu,\n", m_blockLog.size());
    std::fprintf(file, "  \"blocks\": [\n");
    for (size_t i = 0; i < m_blockLog.size(); ++i) {
        const BlockInfo& b = m_blockLog[i];
        std::fprintf(file,
                     "    {\"frame\": %llu, \"frames\": %u, \"ring\": %u, \"held\": %u, "
                     "\"dropped\": %u, \"cck\": %u}%s\n",
                     static_cast<unsigned long long>(b.frameIndex), b.frames, b.ringLevel, b.held,
                     b.dropped, b.cck, (i + 1 == m_blockLog.size()) ? "" : ",");
    }
    std::fprintf(file, "  ]\n}\n");
    std::fclose(file);
}

} // namespace adm::core
