#pragma once

#include <functional>
#include <memory>

#include "../../core/AudioLogSnapshot.h"
#include "../../core/PaulaStateSnapshot.h"
#include "AddressActions.h"

namespace adm::core {
class UserPreferences;
} // namespace adm::core

namespace adm::ui {

class EmulatorView;
class AudioChannelsView;

// Window shell for the Paula channel view. Mirrors DebuggerBitplanesView: the window plumbing
// lives here, the content lives in AudioChannelsView.
class DebuggerAudioView
{
public:
    explicit DebuggerAudioView(EmulatorView* emulatorView);
    ~DebuggerAudioView();

    void Draw();
    void Clear();
    bool* GetOpenPtr() { return &m_open; }
    void Restore(bool open);
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }

    void SetAddressContext(const AddressActionContext* ctx);
    void SetUserPreferences(core::UserPreferences* preferences);
    void SetOnGetAudioStats(std::function<core::AudioLogSnapshot()> cb);

    // Valid whether or not the window is open — the PAULA console command and the
    // get_paula_state MCP tool read the chipset directly, not the window's scroll state.
    core::PaulaStateSnapshot BuildSnapshot() const;

private:
    EmulatorView* m_emulatorView;
    bool m_open = false;
    bool m_noFocus = false;
    bool m_turboSuppressed = false;
    bool m_wasOpen = false;

    std::unique_ptr<AudioChannelsView> m_audioView;
};

} // namespace adm::ui
