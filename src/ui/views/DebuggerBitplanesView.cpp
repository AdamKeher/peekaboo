#include "DebuggerBitplanesView.h"
#include <imgui.h>
#include "../IconsFontAwesome6.h"
#include "BitplanesView.h"
#include "EmulatorView.h"
#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

DebuggerBitplanesView::DebuggerBitplanesView(EmulatorView* emulatorView)
    : m_emulatorView(emulatorView), m_bitplanesView(std::make_unique<BitplanesView>(emulatorView))
{
}

DebuggerBitplanesView::~DebuggerBitplanesView() = default;

void DebuggerBitplanesView::Clear()
{
    if (m_bitplanesView)
        m_bitplanesView->Clear();
}

void DebuggerBitplanesView::SetOnJumpToMemory(std::function<void(uint32_t)> cb)
{
    if (m_bitplanesView)
        m_bitplanesView->SetOnJumpToMemory(std::move(cb));
}

void DebuggerBitplanesView::SetAddressContext(const AddressActionContext* ctx)
{
    if (m_bitplanesView)
        m_bitplanesView->SetAddressContext(ctx);
}

void DebuggerBitplanesView::SetUserPreferences(core::UserPreferences* preferences)
{
    if (m_bitplanesView)
        m_bitplanesView->SetUserPreferences(preferences);
}

core::BitplaneLayout DebuggerBitplanesView::GetBitplaneLayout()
{
    if (m_bitplanesView)
        return m_bitplanesView->GetBitplaneLayout();
    return {};
}

void DebuggerBitplanesView::SetActiveBlit(const core::BlitRecord* rec)
{
    if (m_bitplanesView)
        m_bitplanesView->SetActiveBlit(rec);
}

void DebuggerBitplanesView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void DebuggerBitplanesView::Draw()
{
    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(900, 540), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }

    if (ImGui::Begin(ICON_FA_LAYER_GROUP " Bitplanes###Bitplanes##debugger", &m_open, flags)) {
        if (m_turboSuppressed) {
            DrawTurboSuppressedPlaceholder();
        } else if (m_bitplanesView) {
            m_bitplanesView->Draw();
        }
    }
    ImGui::End();
}

} // namespace adm::ui
