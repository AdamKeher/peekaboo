#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adm::core {

// Peekaboo's own window layout, described without any ImGui types. Self-contained on purpose (see
// BlitLogSnapshot.h for the same contract): the MCP UI tools and their tests consume these structs
// without pulling in imgui.h / imgui_internal.h, which stay behind src/ui/UiIntrospect.h.
//
// Everything here describes the HOST application - Peekaboo's windows, docks and panels - not the
// emulated Amiga. The guest-facing tools (screenshot, click_mouse, ...) are a separate family.

// One row of the window list: a registry entry from Application::GetWindowStates(), a PanelView, or
// a live ImGui window that belongs to neither (a document's FileListView, a dialog, a file viewer).
struct UiWindowInfo
{
    std::string name;    // the key callers address it by (the View-menu label for registry entries)
    std::string imguiId; // ImGui identity, i.e. the Begin() label's "###" tail
    std::string group;   // View-menu submenu ("Debugger", "Performance", ...), empty for top level

    // How this entry is known to the app.
    bool registered = false; // present in Application::GetWindowStates() - can be opened/closed
    bool isPanel = false;    // a PanelView, i.e. a container hosting its own DockSpace
    bool isWindow = true;    // false for draw-list overlays (perf overlay/graph): no ImGui window

    // Open flag as the owning view holds it. For unregistered windows this mirrors `exists`.
    bool open = false;

    // Live ImGui state. All false / zero when the window has never been submitted this session.
    bool exists = false;    // ImGui knows a window by this id
    bool visible = false;   // submitted and actually drawn this frame (not a hidden dock tab)
    bool collapsed = false; // title bar only
    bool focused = false;   // the focused window, or inside the focused dock node
    bool docked = false;
    bool isSelectedTab = false; // when docked: the front tab of its node
    uint32_t dockNodeId = 0;    // ImGuiID of the node it is docked in, 0 when floating
    std::string hostPanel;      // name of the PanelView whose DockSpace ultimately hosts it

    // Screen rect in ImGui coordinates (points, not framebuffer pixels).
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// One node of a dock tree. Leaf nodes hold windows; split nodes hold two children.
struct UiDockNodeInfo
{
    uint32_t id = 0;
    uint32_t parentId = 0;
    std::string splitAxis; // "x" (side by side), "y" (stacked), or "none" for a leaf
    bool isLeaf = false;
    bool isCentral = false; // the passthru central node of a dockspace
    // A node whose DockSpace was never submitted has no host window, so anything docked inside it
    // renders as a stray floating window. This is the failure PrimePanelDockHosts exists to repair,
    // so it is reported rather than hidden.
    bool hasHostWindow = false;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    std::vector<uint32_t> childIds;
    std::vector<std::string> windows; // imguiIds of the windows docked in this node, tab order
    std::string selectedTab;          // imguiId of the front tab, empty when the node is empty
};

// A PanelView: a user-created container window whose DockSpace id is the panel's own id.
struct UiPanelInfo
{
    std::string name;
    uint32_t id = 0; // doubles as the DockSpace ImGuiID
    bool open = false;
    bool permanent = false; // the Documents panel cannot be closed
    bool hosted = false;    // its DockSpace has a host window (see UiDockNodeInfo::hasHostWindow)
};

// The whole picture: every window, every panel, and the dock trees rooted at the main dockspace and
// at each panel.
struct UiLayoutSnapshot
{
    bool available = false; // false when there is no live ImGui context to read
    uint32_t mainDockId = 0;
    // True while a workspace load is still settling (panel priming / tab re-assertion). Geometry
    // read during that window is in flux, and layout mutations are refused.
    bool layoutSettling = false;
    std::vector<UiWindowInfo> windows;
    std::vector<UiPanelInfo> panels;
    std::vector<UiDockNodeInfo> nodes;
};

// --- Requests -------------------------------------------------------------

// Where to put a window. Exactly one of the target forms is used, chosen by `kind`.
struct UiDockRequest
{
    enum class Target {
        Panel,  // into a PanelView's DockSpace, addressed by panel name
        Window, // next to / onto another window
        Main,   // the main viewport dockspace
        Float,  // undock entirely
    };

    enum class Direction {
        Tab, // same node, as another tab
        Left,
        Right,
        Up,
        Down,
    };

    std::string window; // the window to move (registry name or imguiId)
    Target target = Target::Main;
    std::string targetName; // panel name or other window's name, per `target`
    Direction direction = Direction::Tab;
    float ratio = 0.5f;               // split size for the directional forms
    float x = 0, y = 0, w = 0, h = 0; // Float only; all zero means "leave where it lands"
};

// A deferred capture of the application framebuffer. Rects are in ImGui coordinates; AppCapture
// scales them to framebuffer pixels itself, so DPI never leaks into the caller.
struct UiCaptureRequest
{
    bool wholeApp = true;             // false -> crop to x/y/w/h
    float x = 0, y = 0, w = 0, h = 0; // ImGui coordinates
    int maxWidth = 1280;              // downscale so a 4K app capture is not a multi-MB PNG
    // Frames to let pass before capturing. Focusing a window or moving it in the dock tree does not
    // take effect until it is next submitted, and a layout change can take several frames to
    // settle, so a capture that follows one has to wait rather than shoot the stale frame.
    int delayFrames = 0;
};

struct UiCaptureResult
{
    bool ok = false;
    std::string error;
    std::vector<uint8_t> png;
    int width = 0, height = 0;             // of the returned image
    int sourceWidth = 0, sourceHeight = 0; // of the region before downscaling
    bool downscaled = false;
};

// One host-side input event, applied to Peekaboo's own ImGui between the GLFW backend's NewFrame
// and ImGui::NewFrame. Nothing here reaches the emulated Amiga.
struct UiInputEvent
{
    enum class Kind {
        MouseMove,
        MouseClick,
        MouseDown,
        MouseUp,
        Key,
        Text,
    };

    Kind kind = Kind::MouseMove;
    float x = 0, y = 0;  // ImGui coordinates, for the mouse kinds
    int button = 0;      // 0 left, 1 right, 2 middle
    int key = 0;         // resolved ImGuiKey, for Kind::Key
    std::string keyName; // what the caller asked for, kept for error messages
    bool ctrl = false, shift = false, alt = false, super = false;
    std::string text;   // for Kind::Text
    int holdFrames = 1; // how long a click/key stays held
};

// One line of Peekaboo's own log, as held by LogView.
struct UiLogEntry
{
    std::string level; // "Info" | "Warning" | "Error" | "Debug"
    std::string message;
};

struct UiLogSnapshot
{
    bool available = false;
    int total = 0; // entries held before any limit was applied
    bool truncated = false;
    std::vector<UiLogEntry> entries; // oldest first
};

} // namespace adm::core
