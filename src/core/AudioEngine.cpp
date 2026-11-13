#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "AudioEngine.h"
#include "AudioModule.h"
#include "Logger.h"

#include <cstring>

namespace adm {
namespace core {

// Miniaudio callback function
static void data_callback(ma_device* device, void* output, const void* /*input*/,
                          ma_uint32 frameCount)
{
    AudioEngine* engine = static_cast<AudioEngine*>(device->pUserData);
    if (engine) {
        engine->FillBuffer(static_cast<float*>(output), frameCount);
    }
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() { Shutdown(); }

bool AudioEngine::Initialize()
{
    if (m_initialized) {
        return true;
    }

    m_device = new ma_device;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = m_sampleRate;
    config.dataCallback = data_callback;
    config.pUserData = this;

    if (ma_device_init(nullptr, &config, m_device) != MA_SUCCESS) {
        Logger::Error("Failed to initialize audio device");
        delete m_device;
        m_device = nullptr;
        return false;
    }

    // Get actual sample rate from device
    m_sampleRate = m_device->sampleRate;

    m_initialized = true;
    Logger::Info("Audio engine initialized (sample rate: {} Hz)", m_sampleRate);
    return true;
}

void AudioEngine::Shutdown()
{
    if (!m_initialized) {
        return;
    }

    Stop();

    if (m_device) {
        ma_device_uninit(m_device);
        delete m_device;
        m_device = nullptr;
    }

    m_initialized = false;
    Logger::Info("Audio engine shutdown");
}

void AudioEngine::Play(AudioModule* module)
{
    if (!m_initialized || !m_device) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    // Stop current playback
    if (m_playing.load()) {
        ma_device_stop(m_device);
    }

    m_currentModule = module;
    m_paused.store(false);
    m_playing.store(true);

    if (ma_device_start(m_device) != MA_SUCCESS) {
        Logger::Error("Failed to start audio device");
        m_playing.store(false);
        m_currentModule = nullptr;
    }
}

void AudioEngine::Pause()
{
    if (!m_initialized || !m_device || !m_playing.load()) {
        return;
    }

    m_paused.store(true);
    ma_device_stop(m_device);
}

void AudioEngine::Resume()
{
    if (!m_initialized || !m_device || !m_paused.load()) {
        return;
    }

    m_paused.store(false);
    ma_device_start(m_device);
}

void AudioEngine::Stop()
{
    if (!m_initialized || !m_device) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_device) {
        ma_device_stop(m_device);
    }

    m_playing.store(false);
    m_paused.store(false);
    m_currentModule = nullptr;
}

void AudioEngine::FillBuffer(float* output, uint32_t frameCount)
{
    // If not playing or paused, output silence
    if (!m_playing.load() || m_paused.load() || !m_currentModule) {
        std::memset(output, 0, frameCount * 2 * sizeof(float));
        return;
    }

    // Read from module
    size_t framesRead = m_currentModule->Read(output, frameCount);

    // If we reached end of module, stop
    if (framesRead < frameCount) {
        // Fill remainder with silence
        std::memset(output + framesRead * 2, 0, (frameCount - framesRead) * 2 * sizeof(float));

        // Signal end of playback (will be picked up on next iteration)
        m_playing.store(false);
    }
}

} // namespace core
} // namespace adm
