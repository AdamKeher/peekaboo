#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace adm::core {

// Single-producer / single-consumer WAV capture tap.
//
// Peekaboo records emulator audio at two independent points so an artifact can be blamed on a
// specific layer (see docs — "the three-layer model"):
//
//   Tap B  the libretro audio_batch_cb payload, before Peekaboo's ring buffer. What PuAE
//          synthesised. Produced on the main thread inside retro_run, so it is wall-clock
//          independent and works headless.
//   Tap C  what miniaudio hands the sound card. Produced on the audio callback thread.
//
// Clean at B but corrupt at C means the fault is Peekaboo's host audio path; corrupt at B means
// it is emulation. Neither producer may touch the filesystem, so samples are staged in a lock-free
// ring and written by Drain() on the main thread.
class AudioRecorder
{
public:
    // How a captured block reached the tap. Written alongside the WAV as a sidecar JSON so the
    // analyser can attribute a glitch in the waveform to the block that produced it.
    struct BlockInfo
    {
        uint64_t frameIndex = 0; // first output frame this block covers
        uint32_t frames = 0;     // block size — the producer-jitter metric at tap B
        uint32_t ringLevel = 0;  // ring fill at the time of the block (tap C only)
        uint32_t held = 0;       // frames filled by repeating the last sample (tap C only)
        uint32_t dropped = 0;    // frames discarded (overrun at tap B; resampler loss, or stale
                                 // audio trimmed to hold the ring at its target, at tap C)
        uint32_t cck = 0;        // chipset cycles this emulated frame took (tap B only)
    };

    struct Config
    {
        std::string path;        // .wav to write; the sidecar is <path>.json
        uint32_t sampleRate = 0; // 0 is rejected
        uint32_t channels = 2;
        bool float32 = false;    // true: 32-bit IEEE float; false: 16-bit PCM
        double maxSeconds = 0.0; // >0 stops automatically after this much audio
    };

    struct Stats
    {
        bool recording = false;
        std::string path;
        uint32_t sampleRate = 0;
        uint32_t channels = 2;
        uint64_t framesWritten = 0;
        uint64_t framesDroppedStaging = 0; // capture ring overflowed — the recorder itself lagged
        uint64_t blocks = 0;
        double seconds = 0.0;
    };

    AudioRecorder() = default;
    ~AudioRecorder();

    AudioRecorder(const AudioRecorder&) = delete;
    AudioRecorder& operator=(const AudioRecorder&) = delete;

    bool Start(const Config& config, std::string& outError);
    void Stop();

    // Safe to call from the producer thread; cheap when not recording.
    bool IsRecording() const { return m_recording.load(std::memory_order_acquire); }

    // Producer side. Interleaved, `frames` * channels values. Never blocks, never allocates.
    void PushFloat(const float* interleaved, size_t frames);
    void PushInt16(const int16_t* interleaved, size_t frames);
    void PushBlock(const BlockInfo& info);

    // Consumer side — main thread only.
    void Drain();

    Stats GetStats() const;

private:
    void WriteHeader();
    void PatchHeader();
    void WriteSidecar();

    // Staging ring. Sized so a stalled main thread has ~5 s of slack at 48 kHz stereo before the
    // capture itself starts dropping — a dropped capture would look exactly like the bug we are
    // hunting, so overflow is counted and reported rather than silently tolerated.
    static constexpr size_t kStagingFrames = 262144;

    std::vector<float> m_staging;
    std::atomic<size_t> m_writePos{0};
    std::atomic<size_t> m_readPos{0};
    std::atomic<uint64_t> m_stagingDropped{0};
    std::atomic<bool> m_recording{false};

    // Block metadata is produced under the same SPSC discipline as the samples.
    static constexpr size_t kMaxBlocks = 65536;
    std::vector<BlockInfo> m_blocks;
    std::atomic<size_t> m_blockWritePos{0};
    size_t m_blockReadPos = 0;
    std::vector<BlockInfo> m_blockLog;

    Config m_config;
    void* m_file = nullptr; // FILE*, kept opaque so callers do not need <cstdio>
    uint64_t m_framesWritten = 0;
    uint64_t m_blockCount = 0;
    std::vector<uint8_t> m_convert; // reusable staging for sample-format conversion
};

} // namespace adm::core
