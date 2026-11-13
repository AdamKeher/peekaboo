#pragma once

#include <imgui.h>
#include <cstdint>
#include <functional>
#include <memory>

namespace adm::ui {

class EmulatorView;
class SpritesView;

class DebuggerSpritesView
{
public:
    explicit DebuggerSpritesView(EmulatorView* emulatorView);
    ~DebuggerSpritesView();

    void Draw();
    void Clear();
    bool* GetOpenPtr() { return &m_open; }
    void Restore(bool open);
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }

    void DrawOverlay(ImDrawList* dl, ImVec2 screenMin, ImVec2 screenMax);
    void SetOnJumpToMemory(std::function<void(uint32_t)> cb);

    void SetAllBBoxes(bool show);

    // Persisted panel layout (Scanlines panel width + bottom detail-panel height)
    float GetScanlinesPanelWidth() const;
    void SetScanlinesPanelWidth(float w);
    float GetDetailPanelHeight() const;
    void SetDetailPanelHeight(float h);

private:
    EmulatorView* m_emulatorView;
    bool m_open = false;
    bool m_noFocus = false;
    bool m_turboSuppressed = false;

    std::unique_ptr<SpritesView> m_spritesView;
};

} // namespace adm::ui
