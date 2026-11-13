#pragma once

#include <functional>

#include <imgui.h>

#include "../../core/HexUtil.h"
#include "BreakpointsView.h"

namespace adm::ui {

// Draws the shared "Set/Clear breakpoint" item and the Read/Write/R+W watchpoint
// submenu for `addr`, mutating the Breakpoints window so the AR console's address
// menu and the disassembly view's right-click menu stay identical. Must be called
// between ImGui::BeginPopup/EndPopup. `onChanged` (optional) fires after any
// mutation so dependent views can refresh.
inline void DrawBreakpointMenuItems(BreakpointsView* bps, uint32_t addr,
                                    const std::function<void()>& onChanged = {})
{
    if (!bps)
        return;

    auto notify = [&]() {
        if (onChanged)
            onChanged();
    };

    // Breakpoint (PC) — toggle.
    auto [hasBp, bpEnabled] = bps->GetBreakpointStatus(addr);
    (void)bpEnabled;
    if (ImGui::MenuItem(hasBp ? "Clear breakpoint" : "Set breakpoint")) {
        bps->ToggleBreakpointAt(addr);
        notify();
    }

    // Watchpoint submenu — Read / Write / Read+Write (checkmark = active).
    if (ImGui::BeginMenu("Watchpoint")) {
        const std::string key = core::FormatAddr8(addr);
        const char* types[] = {"Read", "Write", "R+W"};
        for (const char* t : types) {
            bool on = false;
            for (const auto& e : bps->GetEntries())
                if (e.address == key && e.type == t) {
                    on = true;
                    break;
                }
            if (ImGui::MenuItem(t, nullptr, on)) {
                if (on)
                    bps->RemoveEntryAt(addr, t);
                else
                    bps->AddBreakpointAt(addr, t);
                notify();
            }
        }
        ImGui::EndMenu();
    }
}

} // namespace adm::ui
