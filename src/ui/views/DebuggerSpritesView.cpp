#include "DebuggerSpritesView.h"
#include <imgui.h>
#include "../IconsFontAwesome6.h"
#include "EmulatorView.h"
#include "SpritesView.h"
#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

DebuggerSpritesView::DebuggerSpritesView(EmulatorView* emulatorView)
    : m_emulatorView(emulatorView), m_spritesView(std::make_unique<SpritesView>(emulatorView))
{
}

DebuggerSpritesView::~DebuggerSpritesView() = default;

void DebuggerSpritesView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void DebuggerSpritesView::Draw()
{
    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(700, 460), ImGuiCond_FirstUseEver);

    // The content lays out its own child sections (table / histogram / detail), each with its
    // own scrollbar, and sizes them to fill the window — so the outer window never needs to
    // scroll. Suppress its scrollbar to avoid a redundant one on the right edge.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }

    if (ImGui::Begin(ICON_FA_GHOST " Sprites###Sprites##debugger", &m_open, flags)) {
        if (m_turboSuppressed) {
            DrawTurboSuppressedPlaceholder();
        } else if (m_spritesView) {
            m_spritesView->Draw();
        }
    }
    ImGui::End();
}

void DebuggerSpritesView::Clear() { m_spritesView->Clear(); }

void DebuggerSpritesView::DrawOverlay(ImDrawList* dl, ImVec2 screenMin, ImVec2 screenMax)
{
    if (m_spritesView)
        m_spritesView->DrawOverlay(dl, screenMin, screenMax);
}

void DebuggerSpritesView::SetOnJumpToMemory(std::function<void(uint32_t)> cb)
{
    if (m_spritesView)
        m_spritesView->SetOnJumpToMemory(std::move(cb));
}

void DebuggerSpritesView::SetAllBBoxes(bool show)
{
    if (m_spritesView)
        m_spritesView->SetAllBBoxes(show);
}

float DebuggerSpritesView::GetScanlinesPanelWidth() const
{
    return m_spritesView ? m_spritesView->GetScanlinesPanelWidth() : 0.0f;
}

void DebuggerSpritesView::SetScanlinesPanelWidth(float w)
{
    if (m_spritesView)
        m_spritesView->SetScanlinesPanelWidth(w);
}

float DebuggerSpritesView::GetDetailPanelHeight() const
{
    return m_spritesView ? m_spritesView->GetDetailPanelHeight() : 0.0f;
}

void DebuggerSpritesView::SetDetailPanelHeight(float h)
{
    if (m_spritesView)
        m_spritesView->SetDetailPanelHeight(h);
}

} // namespace adm::ui
