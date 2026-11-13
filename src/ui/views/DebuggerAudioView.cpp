#include "DebuggerAudioView.h"

#include <imgui.h>

#include "../IconsFontAwesome6.h"
#include "AudioChannelsView.h"
#include "EmulatorView.h"
#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

DebuggerAudioView::DebuggerAudioView(EmulatorView* emulatorView)
    : m_emulatorView(emulatorView), m_audioView(std::make_unique<AudioChannelsView>(emulatorView))
{
}

DebuggerAudioView::~DebuggerAudioView() = default;

void DebuggerAudioView::Clear()
{
    if (m_audioView)
        m_audioView->Clear();
}

void DebuggerAudioView::SetAddressContext(const AddressActionContext* ctx)
{
    if (m_audioView)
        m_audioView->SetAddressContext(ctx);
}

void DebuggerAudioView::SetUserPreferences(core::UserPreferences* preferences)
{
    if (m_audioView)
        m_audioView->SetUserPreferences(preferences);
}

void DebuggerAudioView::SetOnGetAudioStats(std::function<core::AudioLogSnapshot()> cb)
{
    if (m_audioView)
        m_audioView->SetOnGetAudioStats(std::move(cb));
}

core::PaulaStateSnapshot DebuggerAudioView::BuildSnapshot() const
{
    if (!m_audioView)
        return {};
    return m_audioView->BuildSnapshot();
}

void DebuggerAudioView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void DebuggerAudioView::Draw()
{
    // Capturing costs the core a call per emitted sample, so arm it only while the window is up.
    // Driven from the open flag rather than from Begin(), so a collapsed or fully occluded tab
    // still stops paying for samples nobody is looking at.
    if (m_open != m_wasOpen) {
        if (m_audioView)
            m_audioView->SetCaptureActive(m_open);
        m_wasOpen = m_open;
    }

    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(880, 620), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }

    if (ImGui::Begin(ICON_FA_VOLUME_HIGH " Audio###Audio##debugger", &m_open, flags)) {
        if (m_turboSuppressed) {
            DrawTurboSuppressedPlaceholder();
        } else if (m_audioView) {
            m_audioView->Draw();
        }
    }
    ImGui::End();
}

} // namespace adm::ui
