#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Forward declaration
// Forward declaration
struct openmpt_module;
struct openmpt_module_ext;

namespace adm {
namespace core {

/**
 * @brief Metadata about a tracker music module.
 */
struct AudioModuleInfo
{
    std::string title;
    std::string artist;
    std::string tracker;
    double durationSeconds = 0.0;
    int numChannels = 0;
    int numPatterns = 0;
    int numSamples = 0;
    int numInstruments = 0;
};

/**
 * @brief Current playback position information.
 */
struct PlaybackPosition
{
    int currentOrder = 0;
    int currentPattern = 0;
    int currentRow = 0;
    int numOrders = 0;
    int numRows = 0; // Rows in current pattern
};

/**
 * @brief Wrapper for libopenmpt module playback.
 *
 * Handles loading and decoding of tracker music files (MOD, XM, S3M, IT, etc.)
 * Thread-safe for concurrent calls from audio and main threads.
 */
class AudioModule
{
public:
    AudioModule();
    ~AudioModule();

    // Non-copyable
    AudioModule(const AudioModule&) = delete;
    AudioModule& operator=(const AudioModule&) = delete;

    /**
     * @brief Load a module from raw file data.
     * @param data The file data.
     * @param filename Original filename (used for format hints).
     * @return true if loading succeeded.
     */
    bool Load(const std::vector<uint8_t>& data, const std::string& filename);

    /**
     * @brief Unload the module and free resources.
     */
    void Unload();

    /**
     * @brief Check if a module is loaded.
     */
    bool IsLoaded() const { return m_module != nullptr; }

    /**
     * @brief Read audio samples from the module.
     * @param buffer Output buffer for interleaved stereo float samples.
     * @param frames Number of frames to read.
     * @return Number of frames actually read.
     */
    size_t Read(float* buffer, size_t frames);

    /**
     * @brief Get current playback position in seconds.
     */
    double GetPositionSeconds() const;

    /**
     * @brief Set playback position.
     * @param pos Position in seconds.
     */
    void SetPositionSeconds(double pos);

    /**
     * @brief Set playback position by order and row.
     * @param order Order index.
     * @param row Row index.
     */
    void SetPositionOrderRow(int order, int row);

    /**
     * @brief Get total duration in seconds.
     */
    double GetDurationSeconds() const;

    /**
     * @brief Get module metadata.
     */
    AudioModuleInfo GetInfo() const;

    /**
     * @brief Get sample names.
     * @return Vector of sample names indexed by sample number (1-based in tracker, 0-based here).
     */
    std::vector<std::string> GetSampleNames() const;

    /**
     * @brief Get current waveform data for visualization.
     * @param numSamples Number of samples to return.
     * @return Waveform samples (mono, normalized -1 to 1).
     */
    std::vector<float> GetCurrentWaveform(int numSamples);

    /**
     * @brief Get current VU levels for all channels.
     * @return Vector of VU levels (0.0 to ~2.0, can exceed 1.0) for each channel.
     */
    std::vector<float> GetChannelVULevels() const;

    /**
     * @brief Get current playback position.
     * @return Playback position info (order, pattern, row).
     */
    PlaybackPosition GetPlaybackPosition() const;

    /**
     * @brief Get the pattern number at a given order position.
     * @param order Order index.
     * @return Pattern number.
     */
    int GetOrderPattern(int order) const;

    /**
     * @brief Get number of rows in a pattern.
     * @param pattern Pattern index.
     * @return Number of rows.
     */
    int GetPatternNumRows(int pattern) const;

    /**
     * @brief Get formatted pattern cell data.
     * @param pattern Pattern index.
     * @param row Row index.
     * @param channel Channel index.
     * @return Formatted string (e.g., "C-5 01 v64 ...").
     */
    std::string GetPatternRowChannel(int pattern, int row, int channel) const;

    /**
     * @brief Get channel names.
     * @return Vector of channel names.
     */
    std::vector<std::string> GetChannelNames() const;

    /**
     * @brief Set the sample rate for decoding.
     * @param sampleRate Sample rate in Hz.
     */
    void SetSampleRate(uint32_t sampleRate) { m_sampleRate = sampleRate; }

    /**
     * @brief Set the mute status of a channel.
     * @param channel Channel index (0-based).
     * @param mute True to mute, false to unmute.
     */
    void SetChannelMute(int channel, bool mute);

    /**
     * @brief Check if a channel is muted.
     * @param channel Channel index (0-based).
     * @return True if muted.
     */
    bool IsChannelMuted(int channel) const;

private:
    openmpt_module* m_module = nullptr;
    openmpt_module_ext* m_module_ext = nullptr;
    uint32_t m_sampleRate = 48000;
    mutable std::mutex m_mutex;

    // Waveform buffer for visualization
    std::vector<float> m_waveformBuffer;
    static constexpr size_t kWaveformBufferSize = 2048;
};

} // namespace core
} // namespace adm
