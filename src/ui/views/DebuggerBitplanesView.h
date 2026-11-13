#pragma once

#include <imgui.h>
#include <functional>
#include <memory>

#include "../../core/BitplaneLayout.h"
#include "AddressActions.h"

namespace adm::core {
struct BlitRecord;
class UserPreferences;
} // namespace adm::core

namespace adm::ui {

class EmulatorView;
class BitplanesView;

class DebuggerBitplanesView
{
public:
    explicit DebuggerBitplanesView(EmulatorView* emulatorView);
    ~DebuggerBitplanesView();

    void Draw();
    void Clear();
    bool* GetOpenPtr() { return &m_open; }
    void Restore(bool open);
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }

    void SetOnJumpToMemory(std::function<void(uint32_t)> cb);
    void SetAddressContext(const AddressActionContext* ctx);
    void SetUserPreferences(core::UserPreferences* preferences);

    core::BitplaneLayout GetBitplaneLayout();

    // Forward the frame's active blit (nullptr = none) to the underlying view for highlighting.
    void SetActiveBlit(const core::BlitRecord* rec);

private:
    EmulatorView* m_emulatorView;
    bool m_open = false;
    bool m_noFocus = false;
    bool m_turboSuppressed = false;

    std::unique_ptr<BitplanesView> m_bitplanesView;
};

} // namespace adm::ui
