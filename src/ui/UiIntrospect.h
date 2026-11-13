#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../core/UiLayoutSnapshot.h"

namespace adm::ui {

// Reads and mutates Peekaboo's own ImGui window/dock state. This is the only place outside the
// views that touches imgui_internal.h - core/ talks to it through the plain structs in
// core/UiLayoutSnapshot.h, so the MCP UI tools and their tests never see an ImGui type.
//
// Every function here must be called on the main thread, inside a frame, and BEFORE the frame's
// windows are submitted. Application::RunFrame drains MCP requests at exactly that point.

// One addressable window, as Application::GetWindowStates() knows it.
struct UiRegistryEntry
{
    std::string name;
    std::string imguiId; // Begin() label's "###" tail; empty means `name` is the identity
    std::string group;
    bool* openPtr = nullptr;
    bool isWindow = true; // false for draw-list overlays that own a flag but no window
};

// One PanelView. `id` is simultaneously the panel's identity and its DockSpace ImGuiID.
struct UiPanelEntry
{
    std::string name;
    uint32_t id = 0;
    bool* openPtr = nullptr;
    bool permanent = false;
};

// Everything Application must hand over for a layout read or mutation.
struct UiSources
{
    std::vector<UiRegistryEntry> windows;
    std::vector<UiPanelEntry> panels;
    uint32_t mainDockId = 0;
    bool layoutSettling = false; // a workspace load is still priming panels / re-asserting tabs
};

// The ImGui identity of a Begin() label: the part after the last "###", or the whole label when it
// has none. ImHashStr restarts its CRC at "###", so both hash alike - this is for display.
std::string ImGuiIdTail(const char* label);

// True when there is a live ImGui context with a current frame to read.
bool HasLiveUiContext();

// The full picture: registry windows, panels, unregistered live windows, and the dock trees rooted
// at the main dockspace and at each panel.
core::UiLayoutSnapshot BuildLayoutSnapshot(const UiSources& src);

// Bring a window to the front, including selecting the tab of every PanelView that contains it.
// Lifted verbatim from the copies that used to live in DebuggerCustomView and MemoryMapView; both
// now call this. Selecting the inner tab is not enough on its own - see the comment in the body.
void FocusContainingPanels(const char* imguiId);

// Screen rect of a window in ImGui coordinates. False when ImGui has no window by that id.
bool FindWindowRect(const char* imguiId, float& x, float& y, float& w, float& h);

// Move a window in the dock tree. Returns false with outError set for an unknown window/panel, a
// panel whose DockSpace has no host window yet, or a target window that is not itself docked.
bool ApplyDockRequest(const core::UiDockRequest& req, const UiSources& src, std::string& outError);

// The live layout in imgui.ini format. Peekaboo never writes an imgui.ini (main.cpp clears
// io.IniFilename), so this is the only way to see the layout ImGui actually holds.
std::string SaveImGuiIni();

} // namespace adm::ui
