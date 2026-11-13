#include "AudioModule.h"
#include "Logger.h"

#include <libopenmpt/libopenmpt.h>
#include <libopenmpt/libopenmpt_ext.h>

#include <algorithm>
#include <cstring>

namespace adm {
namespace core {

AudioModule::AudioModule()
{
    m_waveformBuffer.resize(kWaveformBufferSize * 2, 0.0f); // Stereo
}

AudioModule::~AudioModule() { Unload(); }

bool AudioModule::Load(const std::vector<uint8_t>& data, const std::string& filename)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    Unload();

    if (data.empty()) {
        Logger::Error("AudioModule: Empty data provided");
        return false;
    }

    // Create module from memory using extension API
    m_module_ext = openmpt_module_ext_create_from_memory(
        data.data(), data.size(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!m_module_ext) {
        Logger::Error("AudioModule: Failed to load module from {}", filename);
        return false;
    }

    m_module = openmpt_module_ext_get_module(m_module_ext);

    // Get some basic info for logging
    const char* title = openmpt_module_get_metadata(m_module, "title");
    double duration = openmpt_module_get_duration_seconds(m_module);

    Logger::Info("AudioModule: Loaded '{}' ({:.1f}s)", title ? title : filename, duration);

    if (title) {
        openmpt_free_string(title);
    }

    return true;
}

void AudioModule::Unload()
{
    // Note: Don't lock here if called from destructor after lock in Load
    if (m_module_ext) {
        openmpt_module_ext_destroy(m_module_ext);
        m_module_ext = nullptr;
        m_module = nullptr; // Invalidated by destroy
    } else if (m_module) {
        // Should not happen if loaded via ext, but safety check
        openmpt_module_destroy(m_module);
        m_module = nullptr;
    }
}

size_t AudioModule::Read(float* buffer, size_t frames)
{
    if (!m_module || !buffer || frames == 0) {
        return 0;
    }

    // Read interleaved stereo float samples
    size_t framesRead = openmpt_module_read_interleaved_float_stereo(
        m_module, static_cast<int32_t>(m_sampleRate), frames, buffer);

    // Copy to waveform buffer for visualization
    if (framesRead > 0) {
        size_t samplesToStore = std::min(framesRead * 2, m_waveformBuffer.size());
        std::memmove(m_waveformBuffer.data(), m_waveformBuffer.data() + samplesToStore,
                     (m_waveformBuffer.size() - samplesToStore) * sizeof(float));
        std::memcpy(m_waveformBuffer.data() + m_waveformBuffer.size() - samplesToStore, buffer,
                    samplesToStore * sizeof(float));
    }

    return framesRead;
}

double AudioModule::GetPositionSeconds() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_module) {
        return 0.0;
    }
    return openmpt_module_get_position_seconds(m_module);
}

void AudioModule::SetPositionSeconds(double pos)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_module) {
        return;
    }
    openmpt_module_set_position_seconds(m_module, pos);
}

void AudioModule::SetPositionOrderRow(int order, int row)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_module) {
        return;
    }
    openmpt_module_set_position_order_row(m_module, order, row);
}

double AudioModule::GetDurationSeconds() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_module) {
        return 0.0;
    }
    return openmpt_module_get_duration_seconds(m_module);
}

AudioModuleInfo AudioModule::GetInfo() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    AudioModuleInfo info;

    if (!m_module) {
        return info;
    }

    auto getMetadata = [this](const char* key) -> std::string {
        const char* value = openmpt_module_get_metadata(m_module, key);
        std::string result = value ? value : "";
        if (value) {
            openmpt_free_string(value);
        }
        return result;
    };

    info.title = getMetadata("title");
    info.artist = getMetadata("artist");
    info.tracker = getMetadata("tracker");
    info.durationSeconds = openmpt_module_get_duration_seconds(m_module);
    info.numChannels = openmpt_module_get_num_channels(m_module);
    info.numPatterns = openmpt_module_get_num_patterns(m_module);
    info.numSamples = openmpt_module_get_num_samples(m_module);
    info.numInstruments = openmpt_module_get_num_instruments(m_module);

    return info;
}

std::vector<std::string> AudioModule::GetSampleNames() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<std::string> names;

    if (!m_module) {
        return names;
    }

    int numSamples = openmpt_module_get_num_samples(m_module);
    names.reserve(numSamples);

    for (int i = 0; i < numSamples; ++i) {
        const char* name = openmpt_module_get_sample_name(m_module, i);
        names.push_back(name ? name : "");
        if (name) {
            openmpt_free_string(name);
        }
    }

    return names;
}

void AudioModule::SetChannelMute(int channel, bool mute)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_module_ext) {
        return;
    }

    openmpt_module_ext_interface_interactive interactive;
    std::memset(&interactive, 0, sizeof(interactive));
    if (openmpt_module_ext_get_interface(m_module_ext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
                                         &interactive, sizeof(interactive))) {
        if (interactive.set_channel_mute_status) {
            interactive.set_channel_mute_status(m_module_ext, channel, mute ? 1 : 0);
        }
    }
}

bool AudioModule::IsChannelMuted(int channel) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_module_ext) {
        return false;
    }

    openmpt_module_ext_interface_interactive interactive;
    std::memset(&interactive, 0, sizeof(interactive));
    if (openmpt_module_ext_get_interface(m_module_ext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
                                         &interactive, sizeof(interactive))) {
        if (interactive.get_channel_mute_status) {
            return interactive.get_channel_mute_status(m_module_ext, channel) == 1;
        }
    }
    return false;
}

std::vector<float> AudioModule::GetCurrentWaveform(int numSamples)
{
    std::vector<float> result(numSamples, 0.0f);

    if (numSamples <= 0) {
        return result;
    }

    // Downsample from stereo buffer to mono output
    size_t sourceSize = m_waveformBuffer.size() / 2; // Stereo to mono
    float step = static_cast<float>(sourceSize) / numSamples;

    for (int i = 0; i < numSamples; ++i) {
        size_t srcIdx = static_cast<size_t>(i * step);
        if (srcIdx * 2 + 1 < m_waveformBuffer.size()) {
            // Mix stereo to mono
            result[i] = (m_waveformBuffer[srcIdx * 2] + m_waveformBuffer[srcIdx * 2 + 1]) * 0.5f;
        }
    }

    return result;
}

std::vector<float> AudioModule::GetChannelVULevels() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<float> levels;

    if (!m_module) {
        return levels;
    }

    int numChannels = openmpt_module_get_num_channels(m_module);
    levels.reserve(numChannels);

    for (int i = 0; i < numChannels; ++i) {
        // Get mono VU level for this channel (returns 0.0 to ~2.0)
        float vu = openmpt_module_get_current_channel_vu_mono(m_module, i);
        levels.push_back(vu);
    }

    return levels;
}

PlaybackPosition AudioModule::GetPlaybackPosition() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    PlaybackPosition pos;

    if (!m_module) {
        return pos;
    }

    pos.currentOrder = openmpt_module_get_current_order(m_module);
    pos.currentPattern = openmpt_module_get_current_pattern(m_module);
    pos.currentRow = openmpt_module_get_current_row(m_module);
    pos.numOrders = openmpt_module_get_num_orders(m_module);
    pos.numRows = openmpt_module_get_pattern_num_rows(m_module, pos.currentPattern);

    return pos;
}

int AudioModule::GetOrderPattern(int order) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_module) {
        return 0;
    }

    return openmpt_module_get_order_pattern(m_module, order);
}

int AudioModule::GetPatternNumRows(int pattern) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_module) {
        return 0;
    }

    return openmpt_module_get_pattern_num_rows(m_module, pattern);
}

std::string AudioModule::GetPatternRowChannel(int pattern, int row, int channel) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_module) {
        return "";
    }

    // Get formatted pattern data for this cell
    // width=0 means use default width, pad=true for consistent spacing
    const char* formatted =
        openmpt_module_format_pattern_row_channel(m_module, pattern, row, channel, 0, 1);

    std::string result = formatted ? formatted : "";
    if (formatted) {
        openmpt_free_string(formatted);
    }

    return result;
}

std::vector<std::string> AudioModule::GetChannelNames() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<std::string> names;

    if (!m_module) {
        return names;
    }

    int numChannels = openmpt_module_get_num_channels(m_module);
    names.reserve(numChannels);

    for (int i = 0; i < numChannels; ++i) {
        const char* name = openmpt_module_get_channel_name(m_module, i);
        if (name && name[0] != '\0') {
            names.push_back(name);
        } else {
            // Default name if empty
            names.push_back("Ch " + std::to_string(i + 1));
        }
        if (name) {
            openmpt_free_string(name);
        }
    }

    return names;
}

} // namespace core
} // namespace adm
