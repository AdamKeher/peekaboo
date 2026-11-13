#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

// Forward declarations
struct ma_device;

namespace adm {
namespace core {

class AudioModule;

/**
 * @brief Audio playback engine using miniaudio.
 *
 * Provides cross-platform audio output for tracker music playback.
 * Thread-safe for control operations from main thread while audio
 * callback runs on audio thread.
 */
class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    // Non-copyable
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /**
     * @brief Initialize the audio device.
     * @return true if initialization succeeded.
     */
    bool Initialize();

    /**
     * @brief Shutdown and release audio resources.
     */
    void Shutdown();

    /**
     * @brief Start playing an audio module.
     * @param module The module to play (ownership retained by caller).
     */
    void Play(AudioModule* module);

    /**
     * @brief Pause playback.
     */
    void Pause();

    /**
     * @brief Resume paused playback.
     */
    void Resume();

    /**
     * @brief Stop playback completely.
     */
    void Stop();

    /**
     * @brief Check if audio is currently playing.
     */
    bool IsPlaying() const { return m_playing.load(); }

    /**
     * @brief Check if audio is paused.
     */
    bool IsPaused() const { return m_paused.load(); }

    /**
     * @brief Check if the engine is initialized.
     */
    bool IsInitialized() const { return m_initialized; }

    /**
     * @brief Get the sample rate.
     */
    uint32_t GetSampleRate() const { return m_sampleRate; }

    /**
     * @brief Fill the audio buffer. Called from miniaudio callback.
     * @param output Output buffer (interleaved stereo float).
     * @param frameCount Number of frames to fill.
     */
    void FillBuffer(float* output, uint32_t frameCount);

private:
    ma_device* m_device = nullptr;
    AudioModule* m_currentModule = nullptr;
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_paused{false};
    std::mutex m_mutex;
    bool m_initialized = false;
    uint32_t m_sampleRate = 48000;
};

} // namespace core
} // namespace adm
