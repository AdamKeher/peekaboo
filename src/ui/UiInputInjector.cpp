#include "UiInputInjector.h"

#include <algorithm>
#include <cctype>

#include <imgui.h>

namespace adm::ui {

using core::UiInputEvent;

namespace {

void ApplyModifiers(ImGuiIO& io, const UiInputEvent& ev, bool down)
{
    if (ev.ctrl)
        io.AddKeyEvent(ImGuiMod_Ctrl, down);
    if (ev.shift)
        io.AddKeyEvent(ImGuiMod_Shift, down);
    if (ev.alt)
        io.AddKeyEvent(ImGuiMod_Alt, down);
    if (ev.super)
        io.AddKeyEvent(ImGuiMod_Super, down);
}

} // namespace

bool ParseImGuiKeyName(const std::string& name, int& outKey)
{
    auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };
    const std::string want = lower(name);
    if (want.empty())
        return false;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const char* n = ImGui::GetKeyName(static_cast<ImGuiKey>(k));
        if (n && lower(n) == want) {
            outKey = k;
            return true;
        }
    }
    return false;
}

std::vector<std::string> ImGuiKeyNames()
{
    std::vector<std::string> names;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        if (const char* n = ImGui::GetKeyName(static_cast<ImGuiKey>(k)))
            names.emplace_back(n);
    return names;
}

void UiInputInjector::Push(const UiInputEvent& ev)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(ev);
}

bool UiInputInjector::Busy() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hasActive || !m_queue.empty();
}

void UiInputInjector::Apply()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!ImGui::GetCurrentContext())
        return;
    ImGuiIO& io = ImGui::GetIO();

    // Re-assert a held cursor position first: the GLFW backend has just overwritten MousePos from
    // the real cursor, and a click has to land where it was aimed even if the user's mouse is
    // elsewhere.
    if (m_holdMouse)
        io.AddMousePosEvent(m_mouseX, m_mouseY);

    if (!m_hasActive) {
        if (m_queue.empty()) {
            // Nothing left to play: stop pinning the cursor and hand it back to the user.
            m_holdMouse = false;
            return;
        }
        m_active = Active{m_queue.front(), 0, false};
        m_queue.pop_front();
        m_hasActive = true;
    }

    UiInputEvent& ev = m_active.ev;

    if (!m_active.started) {
        m_active.started = true;
        m_active.framesLeft = ev.holdFrames > 0 ? ev.holdFrames : 1;

        switch (ev.kind) {
            case UiInputEvent::Kind::MouseMove:
                m_holdMouse = true;
                m_mouseX = ev.x;
                m_mouseY = ev.y;
                io.AddMousePosEvent(ev.x, ev.y);
                break;

            case UiInputEvent::Kind::MouseClick:
            case UiInputEvent::Kind::MouseDown:
                m_holdMouse = true;
                m_mouseX = ev.x;
                m_mouseY = ev.y;
                io.AddMousePosEvent(ev.x, ev.y);
                ApplyModifiers(io, ev, true);
                io.AddMouseButtonEvent(ev.button, true);
                break;

            case UiInputEvent::Kind::MouseUp:
                io.AddMouseButtonEvent(ev.button, false);
                break;

            case UiInputEvent::Kind::Key:
                ApplyModifiers(io, ev, true);
                io.AddKeyEvent(static_cast<ImGuiKey>(ev.key), true);
                break;

            case UiInputEvent::Kind::Text:
                io.AddInputCharactersUTF8(ev.text.c_str());
                m_active.framesLeft = 1;
                break;
        }
        return;
    }

    if (--m_active.framesLeft > 0)
        return;

    // Hold expired - release whatever this event pressed.
    switch (ev.kind) {
        case UiInputEvent::Kind::MouseClick:
            io.AddMouseButtonEvent(ev.button, false);
            ApplyModifiers(io, ev, false);
            break;
        case UiInputEvent::Kind::Key:
            io.AddKeyEvent(static_cast<ImGuiKey>(ev.key), false);
            ApplyModifiers(io, ev, false);
            break;
        default:
            // MouseMove, MouseDown and MouseUp are one-shot: MouseDown deliberately stays held so a
            // drag can be built from down / move / up.
            break;
    }
    m_hasActive = false;
}

} // namespace adm::ui
