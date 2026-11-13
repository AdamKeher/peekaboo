#pragma once

#include <imgui.h>
#include <array>
#include <cstdint>
#include <functional>

#include "../../core/AmigaCustom.h"
#include "../../core/AmigaEmulator.h"
#include "../../core/AudioLogSnapshot.h"
#include "../../core/PaulaStateSnapshot.h"
#include "../../core/UserPreferences.h"
#include "AddressActions.h"

namespace adm::ui {

class EmulatorView;

// Paula's four voices: a 2x2 oscilloscope grid fed by the core-side tap in audio.c newsample(),
// per-channel mute/solo backed by PuAE's audio_channel_mask, low-pass filter control, and the
// AUDxLC/LEN/PER/VOL register block.
//
// Deliberately a sibling of BitplanesView rather than of AudioPlayer: this reads the chipset, not
// the host mixer. AudioPlayer is the tracker/MOD player and shares nothing with it.
class AudioChannelsView
{
public:
    explicit AudioChannelsView(EmulatorView* emulatorView);

    void Draw();
    void Clear();

    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }
    void SetUserPreferences(core::UserPreferences* preferences);

    // Host audio-path health, shown as the one-line status bar. Same provider the AR console's
    // AUDIO command uses, so the window can answer "is the crackle Paula or my sound card?".
    void SetOnGetAudioStats(std::function<core::AudioLogSnapshot()> cb)
    {
        m_onGetAudioStats = std::move(cb);
    }

    // Called by the window shell when the Audio window opens or closes. Capturing costs the core
    // a call per emitted sample, so it is only armed while something is looking.
    void SetCaptureActive(bool active);

    // Chipset snapshot for the PAULA console command and the get_paula_state MCP tool. Computed
    // from the same data the window draws, and valid whether or not the window is open.
    core::PaulaStateSnapshot BuildSnapshot() const;

private:
    static constexpr int kChannels = 4;
    // Rolling scope history per channel. Wide enough for several frames at the fastest periods,
    // so the widest time base still has real samples behind it rather than a single frame padded.
    static constexpr int kScopeHistory = 4096;
    // Frames of silence pushed when a channel emits nothing, so the trace visibly scrolls to flat
    // instead of freezing on the last thing it played.
    static constexpr int kSilencePush = 128;
    static constexpr float kPeakDecayRate = 0.985f;

    EmulatorView* m_emulatorView;
    core::UserPreferences* m_userPreferences = nullptr;
    const AddressActionContext* m_addrCtx = nullptr;
    std::function<core::AudioLogSnapshot()> m_onGetAudioStats;

    // ---- user controls (mirrored into UserPreferences::AudioPrefs) ----
    uint8_t m_channelMask = 0x0F;
    uint8_t m_preSoloMask = 0x0F; // mask to restore when a solo is toggled back off
    int m_soloChannel = -1;
    int m_filterMode = 1;  // 0 off, 1 emulated, 2 on
    int m_filterModel = 0; // 0 auto, 1 A500, 2 A1200
    int m_timeBaseSamples = 512;
    float m_yZoom = 1.0f;
    bool m_showRegisters = true;
    bool m_frozen = false;

    bool m_captureActive = false;
    bool m_prefsLoaded = false;

    // ---- rolling scope state ----
    struct ChannelScope
    {
        std::array<float, kScopeHistory> samples{}; // -1..1, post-volume
        int writePos = 0;
        int valid = 0;          // samples written so far, capped at kScopeHistory
        int lastFrameCount = 0; // samples captured in the most recent frame
        float peak = 0.0f;      // decaying peak-hold for the VU meter
        float framePeak = 0.0f;
        float frameRms = 0.0f;
        uint16_t per = 0;
        uint16_t vol = 0;
        bool dmaOn = false;
    };
    std::array<ChannelScope, kChannels> m_scopes{};
    uint64_t m_lastHistoryFrame = UINT64_MAX;

    // ---- per-frame register cache ----
    core::Custom m_regs{};
    bool m_regsValid = false;
    uint64_t m_lastRegsFrame = UINT64_MAX;

    adm::AmigaEmulator* Emulator() const;

    void EnsureFrameData();
    void PushFrame();
    void ApplyChannelMask();
    void ApplyFilterSettings();
    void LoadPreferences();
    void SavePreferences();

    // What a click on a scope cell asked for. Returned rather than applied in place so the
    // mask edit happens once, after the whole grid has drawn.
    enum class ScopeAction {
        None,
        Toggle,
        Solo,
    };

    // Mute/solo edits shared by the toolbar buttons and the scope-cell clicks.
    void ToggleChannel(int channel);
    void SoloChannel(int channel);

    void DrawToolbar();
    void DrawScopeGrid();
    ScopeAction DrawScope(int channel, ImVec2 pos, ImVec2 size);
    void DrawRegisterTable();
    void DrawStatusBar();
    void DrawPtrLink(const char* idSuffix, uint32_t addr);
};

} // namespace adm::ui
