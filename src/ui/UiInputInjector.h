#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "../core/UiLayoutSnapshot.h"

namespace adm::ui {

// Resolve a key name ("F5", "Enter", "A", "LeftArrow", "Keypad0") to an ImGuiKey,
// case-insensitively. The table is ImGui's own - every named key is compared against
// ImGui::GetKeyName - so it cannot drift out of step with the ImGui version in use. False when the
// name matches nothing.
bool ParseImGuiKeyName(const std::string& name, int& outKey);

// Every name ParseImGuiKeyName accepts, for error messages and the tool's schema.
std::vector<std::string> ImGuiKeyNames();

// Injects synthetic input into Peekaboo's OWN ImGui. Nothing here reaches the emulated Amiga -
// that is what the guest-side click_mouse / press_key / type_text tools are for.
//
// Apply() must run between the GLFW backend's NewFrame and ImGui::NewFrame (main.cpp). The backend
// pushes the real cursor position during its NewFrame, and ImGui's event queue is last-writer-wins,
// so injecting after it is what makes a synthetic position stick for the frame.
//
// A press and its release cannot share a frame - a widget only sees a click if it is held across at
// least one full frame - so events are played out over successive frames by Apply().
class UiInputInjector
{
public:
    // Queue one event. Events play in order, one press/release cycle at a time.
    void Push(const core::UiInputEvent& ev);

    // True while queued events are still playing. The MCP tool waits on this so a follow-up
    // screenshot sees the result of the click rather than the frame before it.
    bool Busy() const;

    // Called once per frame on the main thread, after ImGui_ImplGlfw_NewFrame() and before
    // ImGui::NewFrame().
    void Apply();

private:
    struct Active
    {
        core::UiInputEvent ev;
        int framesLeft = 0;
        bool started = false;
    };

    mutable std::mutex m_mutex;
    std::deque<core::UiInputEvent> m_queue;
    Active m_active;
    bool m_hasActive = false;
    // Once a synthetic position is set it must be re-asserted every frame it is needed, because the
    // backend overwrites it from the real cursor each NewFrame.
    bool m_holdMouse = false;
    float m_mouseX = 0.0f, m_mouseY = 0.0f;
};

} // namespace adm::ui
