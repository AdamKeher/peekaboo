#pragma once

#include <memory>
#include <string>
#include <vector>

struct ImVec2;

namespace adm {
namespace core {
class AudioEngine;
class AudioModule;
} // namespace core
} // namespace adm

namespace adm {
namespace ui {

/**
 * @brief Audio player window for tracker music playback.
 *
 * Provides playback controls, seek bar, and waveform visualization
 * for tracker music files (MOD, XM, S3M, IT, etc.)
 */
class AudioPlayer
{
public:
    /**
     * @brief Construct an audio player.
     * @param engine Shared audio engine (not owned).
     */
    explicit AudioPlayer(core::AudioEngine* engine);
    ~AudioPlayer();

    // Non-copyable
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    /**
     * @brief Set the module to play.
     * @param module The audio module (ownership transferred).
     */
    void SetModule(std::unique_ptr<core::AudioModule> module);

    /**
     * @brief Set the window title.
     * @param title The title string.
     */
    void SetTitle(const std::string& title);

    /**
     * @brief Open the player window.
     */
    void Open();

    /**
     * @brief Draw the player window.
     */
    void Draw();

    /**
     * @brief Check if the window is open.
     */
    [[nodiscard]] bool IsOpen() const { return m_isOpen; }

private:
    void DrawPlaybackControls();
    void DrawSeekBar();
    void DrawWaveform();
    void DrawModuleInfo();
    void DrawSamplesTab();
    void DrawPatternsTab();
    void DrawOrderList(int currentOrder, int numOrders);
    std::string FormatTime(double seconds);

    core::AudioEngine* m_engine;
    std::unique_ptr<core::AudioModule> m_module;
    std::string m_title = "Audio Player";
    bool m_isOpen = false;

    // Seek bar state - pause while dragging to avoid audio glitches
    bool m_isDraggingSeekBar = false;
    bool m_wasPlayingBeforeSeek = false;

    // Cached data (populated once when module is set)
    std::vector<std::string> m_sampleNames;
    std::vector<std::string> m_channelNames;
    int m_numChannels = 0;

    // Pattern view state
    int m_lastScrolledRow = -1;
    bool m_followPlayback = true;

    // Waveform visualization
    std::vector<float> m_waveformBuffer;
    static constexpr int kWaveformSamples = 256;
};

} // namespace ui
} // namespace adm
