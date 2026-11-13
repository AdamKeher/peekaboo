#include "McpUiTools.h"

#include <algorithm>
#include <cctype>

#include <fmt/format.h>

#include "HexUtil.h"
#include "McpServer.h"
#include "McpToolHandler.h"

namespace adm::core {

using nlohmann::json;

namespace {

json Err(const std::string& msg) { return json{{"error", msg}}; }

json Unavailable(const std::string& hint) { return json{{"available", false}, {"hint", hint}}; }

// Park the HTTP worker and re-run this tool next frame. The frame only advances once the dispatcher
// returns, so a tool that needs the UI to redraw cannot simply block here.
json Retry() { return json{{McpServer::kRetryKey, true}}; }

std::string NodeId(uint32_t id) { return fmt::format("0x{:08X}", id); }

std::string Lower(std::string v)
{
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

// What kind of thing a window-list row is, so a caller knows why a row has no geometry.
const char* KindOf(const UiWindowInfo& w)
{
    if (w.isPanel)
        return "panel";
    if (!w.isWindow)
        return "overlay";
    return w.registered ? "window" : "unregistered";
}

json RectJson(float x, float y, float w, float h)
{
    return json{{"x", x}, {"y", y}, {"w", w}, {"h", h}};
}

json WindowToJson(const UiWindowInfo& w)
{
    json j{{"name", w.name}, {"kind", KindOf(w)}, {"open", w.open}, {"exists", w.exists}};
    if (!w.imguiId.empty() && w.imguiId != w.name)
        j["imguiId"] = w.imguiId;
    if (!w.group.empty())
        j["group"] = w.group;
    if (w.exists) {
        j["visible"] = w.visible;
        if (w.collapsed)
            j["collapsed"] = true;
        if (w.focused)
            j["focused"] = true;
        j["docked"] = w.docked;
        if (w.docked) {
            j["dockNodeId"] = NodeId(w.dockNodeId);
            j["isSelectedTab"] = w.isSelectedTab;
            if (!w.hostPanel.empty())
                j["hostPanel"] = w.hostPanel;
        }
        j["rect"] = RectJson(w.x, w.y, w.w, w.h);
    }
    return j;
}

json NodeToJson(const UiDockNodeInfo& n)
{
    json j{{"id", NodeId(n.id)},
           {"isLeaf", n.isLeaf},
           {"hasHostWindow", n.hasHostWindow},
           {"rect", RectJson(n.x, n.y, n.w, n.h)}};
    if (n.parentId)
        j["parentId"] = NodeId(n.parentId);
    if (n.isCentral)
        j["isCentral"] = true;
    if (!n.isLeaf) {
        j["split"] = n.splitAxis;
        json kids = json::array();
        for (uint32_t c : n.childIds)
            kids.push_back(NodeId(c));
        j["children"] = kids;
    }
    if (!n.windows.empty()) {
        j["windows"] = n.windows;
        j["selectedTab"] = n.selectedTab;
    }
    return j;
}

std::string SettlingNote()
{
    return "A workspace layout is still settling (panels are being primed and saved tab selections "
           "re-asserted for up to 60 frames). Geometry read now is in flux and dock_window is "
           "refused; call again shortly.";
}

// --- window list ------------------------------------------------------------

json HandleListWindows(const json& args, const McpHost& host)
{
    if (!host.getUiLayout)
        return Unavailable("the UI introspection hooks are not wired up");
    const UiLayoutSnapshot snap = host.getUiLayout();
    if (!snap.available)
        return Unavailable("no live ImGui context");

    const std::string group = args.value("group", std::string{});
    const bool openOnly = args.value("openOnly", false);
    const bool includeUnregistered = args.value("includeUnregistered", true);

    json arr = json::array();
    int hidden = 0;
    for (const auto& w : snap.windows) {
        if (!includeUnregistered && !w.registered) {
            ++hidden;
            continue;
        }
        if (openOnly && !w.open) {
            ++hidden;
            continue;
        }
        if (!group.empty() && Lower(w.group) != Lower(group)) {
            ++hidden;
            continue;
        }
        arr.push_back(WindowToJson(w));
    }

    json j{{"available", true}, {"count", arr.size()}, {"filteredOut", hidden}, {"windows", arr}};
    if (snap.layoutSettling) {
        j["layoutSettling"] = true;
        j["note"] = SettlingNote();
    }
    return j;
}

json HandleGetWindowLayout(const McpHost& host)
{
    if (!host.getUiLayout)
        return Unavailable("the UI introspection hooks are not wired up");
    const UiLayoutSnapshot snap = host.getUiLayout();
    if (!snap.available)
        return Unavailable("no live ImGui context");

    json panels = json::array();
    for (const auto& p : snap.panels) {
        json pj{
            {"name", p.name}, {"dockNodeId", NodeId(p.id)}, {"open", p.open}, {"hosted", p.hosted}};
        if (p.permanent)
            pj["permanent"] = true;
        panels.push_back(std::move(pj));
    }

    json nodes = json::array();
    bool unhosted = false;
    for (const auto& n : snap.nodes) {
        nodes.push_back(NodeToJson(n));
        if (!n.hasHostWindow && !n.windows.empty())
            unhosted = true;
    }

    json j{{"available", true},
           {"mainDockId", NodeId(snap.mainDockId)},
           {"panels", panels},
           {"nodes", nodes}};

    std::string note;
    if (snap.layoutSettling) {
        j["layoutSettling"] = true;
        note = SettlingNote();
    }
    if (unhosted) {
        if (!note.empty())
            note += " ";
        note +=
            "At least one node holds windows but has no host window: its DockSpace has not been "
            "submitted, so those windows render as stray floating windows instead of tabs. That "
            "is the failure PrimePanelDockHosts repairs - the panel is probably a hidden tab.";
    }
    if (!note.empty())
        j["note"] = note;
    return j;
}

// --- open / close / focus ---------------------------------------------------

json HandleSetOpen(const json& args, const McpHost& host, bool open)
{
    if (!host.setWindowOpen)
        return Unavailable("the UI window hooks are not wired up");
    if (!args.contains("window") || !args["window"].is_string())
        return Err("window is required and must be a string");
    const std::string window = args["window"].get<std::string>();

    std::string err;
    if (!host.setWindowOpen(window, open, err))
        return Err(err);
    json j{{"ok", true}, {"window", window}, {"open", open}};
    if (open)
        j["note"] = "The window opens on the next frame. A screenshot taken immediately may not "
                    "show it yet - screenshot_window waits for it, screenshot_app does not.";
    return j;
}

json HandleFocusWindow(const json& args, const McpHost& host)
{
    if (!host.focusWindow)
        return Unavailable("the UI window hooks are not wired up");
    if (!args.contains("window") || !args["window"].is_string())
        return Err("window is required and must be a string");
    const std::string window = args["window"].get<std::string>();

    std::string err;
    if (!host.focusWindow(window, err))
        return Err(err);
    return json{{"ok", true}, {"window", window}};
}

// --- docking ----------------------------------------------------------------

json HandleDockWindow(const json& args, const McpHost& host)
{
    if (!host.dockWindow)
        return Unavailable("the UI window hooks are not wired up");
    if (!args.contains("window") || !args["window"].is_string())
        return Err("window is required and must be a string");

    UiDockRequest req;
    req.window = args["window"].get<std::string>();

    const std::string target = Lower(args.value("target", std::string("main")));
    if (target == "main")
        req.target = UiDockRequest::Target::Main;
    else if (target == "panel")
        req.target = UiDockRequest::Target::Panel;
    else if (target == "window")
        req.target = UiDockRequest::Target::Window;
    else if (target == "float")
        req.target = UiDockRequest::Target::Float;
    else
        return Err("target must be one of: main, panel, window, float");

    if (req.target == UiDockRequest::Target::Panel) {
        if (!args.contains("panel") || !args["panel"].is_string())
            return Err("target 'panel' requires a `panel` name");
        req.targetName = args["panel"].get<std::string>();
    } else if (req.target == UiDockRequest::Target::Window) {
        if (!args.contains("targetWindow") || !args["targetWindow"].is_string())
            return Err("target 'window' requires a `targetWindow` name");
        req.targetName = args["targetWindow"].get<std::string>();
    }

    const std::string dir = Lower(args.value("direction", std::string("tab")));
    if (dir == "tab")
        req.direction = UiDockRequest::Direction::Tab;
    else if (dir == "left")
        req.direction = UiDockRequest::Direction::Left;
    else if (dir == "right")
        req.direction = UiDockRequest::Direction::Right;
    else if (dir == "up")
        req.direction = UiDockRequest::Direction::Up;
    else if (dir == "down")
        req.direction = UiDockRequest::Direction::Down;
    else
        return Err("direction must be one of: tab, left, right, up, down");

    req.ratio = args.value("ratio", 0.5f);
    req.x = args.value("x", 0.0f);
    req.y = args.value("y", 0.0f);
    req.w = args.value("width", 0.0f);
    req.h = args.value("height", 0.0f);

    std::string err;
    if (!host.dockWindow(req, err))
        return Err(err);

    json j{{"ok", true}, {"window", req.window}, {"target", target}, {"direction", dir}};
    if (!req.targetName.empty())
        j["targetName"] = req.targetName;
    j["note"] =
        "The new layout is applied on the next frame and can take several frames to settle. "
        "Call get_window_layout again (or screenshot_app) to confirm where it landed rather "
        "than assuming this succeeded visually.";
    return j;
}

// --- screenshots ------------------------------------------------------------

// Shape a finished capture into the response both screenshot tools return. The image rides next to
// the metadata the caller cannot get from the picture itself - notably the downscale factor, since
// coordinates read off a scaled image are not the coordinates ui_click takes.
json CaptureResponse(const UiCaptureResult& result, json extra)
{
    if (!result.ok)
        return Err(result.error.empty() ? "capture failed" : result.error);

    json j = std::move(extra);
    j["image_png_base64"] = Base64Encode(result.png);
    j["width"] = result.width;
    j["height"] = result.height;
    if (result.downscaled) {
        j["sourceWidth"] = result.sourceWidth;
        j["sourceHeight"] = result.sourceHeight;
        j["downscaled"] = true;
        j["note"] = fmt::format(
            "Scaled down from {}x{} to keep the response small. Coordinates read off this image "
            "must be multiplied by {:.3f} before being passed to ui_click, which takes "
            "application coordinates.",
            result.sourceWidth, result.sourceHeight,
            static_cast<double>(result.sourceWidth) / result.width);
    }
    return j;
}

json HandleScreenshotApp(const json& args, const McpHost& host)
{
    if (!host.requestAppCapture || !host.pollAppCapture)
        return Unavailable("the application capture hooks are not wired up");

    const int maxWidth = args.value("maxWidth", 1280);
    if (maxWidth < 0)
        return Err("maxWidth must be >= 0 (0 means no downscaling)");

    // Poll first: on every retry after the first there is already a capture in flight, and the
    // result must be consumed exactly once - polling twice in one dispatch would throw the image
    // away and re-queue for ever.
    if (auto result = host.pollAppCapture())
        return CaptureResponse(*result, json::object());

    UiCaptureRequest req;
    req.wholeApp = true;
    req.maxWidth = maxWidth;
    std::string err;
    host.requestAppCapture(req, err);
    return Retry();
}

json HandleScreenshotWindow(const json& args, const McpHost& host)
{
    if (!host.requestAppCapture || !host.pollAppCapture || !host.windowRect)
        return Unavailable("the application capture hooks are not wired up");
    if (!args.contains("window") || !args["window"].is_string())
        return Err("window is required and must be a string");
    const std::string window = args["window"].get<std::string>();

    const int maxWidth = args.value("maxWidth", 1280);
    if (maxWidth < 0)
        return Err("maxWidth must be >= 0 (0 means no downscaling)");

    // Poll first, for the reason given in HandleScreenshotApp - and because redoing the focus and
    // rect work on every retry would re-read the geometry while the layout is still moving.
    if (auto result = host.pollAppCapture())
        return CaptureResponse(*result, json{{"window", window}});

    UiCaptureRequest req;
    std::string err;
    if (!host.windowRect(window, req, err))
        return Err(err);
    req.maxWidth = maxWidth;

    // A window that is a background dock tab has no pixels on screen at all, so capturing it
    // without raising it would return whatever is in front of it.
    if (args.value("focus", true) && host.focusWindow) {
        std::string focusErr;
        host.focusWindow(window, focusErr);
        // Give the focus a few frames to take effect before the shutter opens.
        req.delayFrames = 3;
    }

    std::string reqErr;
    host.requestAppCapture(req, reqErr);
    return Retry();
}

// --- workspaces -------------------------------------------------------------

json HandleListWorkspaces(const McpHost& host)
{
    if (!host.listWorkspaces)
        return Unavailable("the workspace hooks are not wired up");
    const auto names = host.listWorkspaces();
    json out{{"available", true}, {"count", names.size()}, {"workspaces", names}};
    if (host.getScreenProfile)
        out["screen_profile"] = host.getScreenProfile();
    if (host.workspaceHasOwnLayout) {
        json scaled = json::array();
        for (const auto& name : names)
            if (!host.workspaceHasOwnLayout(name))
                scaled.push_back(name);
        out["scaled"] = scaled;
    }
    return out;
}

json HandleSetScreenProfile(const json& args, const McpHost& host)
{
    if (!host.setScreenProfile)
        return Unavailable("the screen profile hooks are not wired up");
    if (!args.contains("profile") || !args["profile"].is_string())
        return Err("profile is required and must be \"hires\" or \"lores\"");
    const std::string profile = args["profile"].get<std::string>();
    std::string err;
    if (!host.setScreenProfile(profile, err))
        return Err(err);
    return json{{"ok", true},
                {"screen_profile", profile},
                {"note", "The current workspace reloads in this profile's layout (scaled from the "
                         "other profile's if it has none yet), and text sizes, window size and "
                         "table columns switch to this profile's. Wait for layoutSettling:false "
                         "before reading the layout."}};
}

json HandleLoadWorkspace(const json& args, const McpHost& host)
{
    if (!host.loadWorkspace)
        return Unavailable("the workspace hooks are not wired up");
    if (!args.contains("name") || !args["name"].is_string())
        return Err("name is required and must be a string");
    const std::string name = args["name"].get<std::string>();

    std::string err;
    if (!host.loadWorkspace(name, err))
        return Err(err);
    return json{{"ok", true},
                {"workspace", name},
                {"note", "The layout is applied on the next frame and then settles for up to 60 "
                         "frames while panels are primed and saved tab selections re-asserted. "
                         "list_windows and get_window_layout report layoutSettling:true until that "
                         "finishes, and dock_window is refused while it does."}};
}

json HandleSaveWorkspace(const json& args, const McpHost& host)
{
    if (!host.saveWorkspace)
        return Unavailable("the workspace hooks are not wired up");
    if (!args.contains("name") || !args["name"].is_string())
        return Err("name is required and must be a string");
    const std::string name = args["name"].get<std::string>();

    std::string err;
    if (!host.saveWorkspace(name, err))
        return Err(err);
    return json{{"ok", true}, {"workspace", name}};
}

// --- log / ini --------------------------------------------------------------

json HandleGetUiLog(const json& args, const McpHost& host)
{
    if (!host.getUiLog)
        return Unavailable("the UI log hook is not wired up");

    int limit = args.value("limit", 100);
    if (limit <= 0 || limit > 2000)
        limit = limit <= 0 ? 100 : 2000;
    const std::string level = args.value("level", std::string{});

    const UiLogSnapshot snap = host.getUiLog(limit, level);
    if (!snap.available)
        return Unavailable("the Log view has not been created yet");

    json arr = json::array();
    for (const auto& e : snap.entries)
        arr.push_back({{"level", e.level}, {"message", e.message}});
    return json{{"available", true},
                {"total", snap.total},
                {"truncated", snap.truncated},
                {"count", arr.size()},
                {"entries", arr}};
}

json HandleGetImguiIni(const McpHost& host)
{
    if (!host.getImguiIni)
        return Unavailable("the layout hook is not wired up");
    const std::string ini = host.getImguiIni();
    if (ini.empty())
        return Unavailable("no live ImGui context");
    return json{{"available", true}, {"bytes", ini.size()}, {"ini", ini}};
}

// --- input injection --------------------------------------------------------

json InjectAndReport(const McpHost& host, const UiInputEvent& ev, json result)
{
    std::string err;
    if (!host.injectUiInput(ev, err))
        return Err(err);
    result["ok"] = true;
    return result;
}

json HandleUiClick(const json& args, const McpHost& host)
{
    if (!host.injectUiInput)
        return Unavailable("the UI input hooks are not wired up");
    if (!args.contains("x") || !args.contains("y"))
        return Err("x and y are required (application coordinates)");

    UiInputEvent ev;
    const std::string button = Lower(args.value("button", std::string("left")));
    if (button == "left")
        ev.button = 0;
    else if (button == "right")
        ev.button = 1;
    else if (button == "middle")
        ev.button = 2;
    else
        return Err("button must be one of: left, right, middle");

    const std::string action = Lower(args.value("action", std::string("click")));
    if (action == "click")
        ev.kind = UiInputEvent::Kind::MouseClick;
    else if (action == "move")
        ev.kind = UiInputEvent::Kind::MouseMove;
    else if (action == "down")
        ev.kind = UiInputEvent::Kind::MouseDown;
    else if (action == "up")
        ev.kind = UiInputEvent::Kind::MouseUp;
    else
        return Err("action must be one of: click, move, down, up");

    ev.x = args.value("x", 0.0f);
    ev.y = args.value("y", 0.0f);
    ev.ctrl = args.value("ctrl", false);
    ev.shift = args.value("shift", false);
    ev.alt = args.value("alt", false);
    ev.holdFrames = std::clamp(args.value("holdFrames", 2), 1, 600);

    return InjectAndReport(host, ev,
                           json{{"action", action}, {"button", button}, {"x", ev.x}, {"y", ev.y}});
}

json HandleUiPressKey(const json& args, const McpHost& host)
{
    if (!host.injectUiInput)
        return Unavailable("the UI input hooks are not wired up");
    if (!args.contains("key") || !args["key"].is_string())
        return Err("key is required and must be a string (e.g. \"F5\", \"Enter\", \"A\")");

    UiInputEvent ev;
    ev.kind = UiInputEvent::Kind::Key;
    ev.keyName = args["key"].get<std::string>();
    ev.ctrl = args.value("ctrl", false);
    ev.shift = args.value("shift", false);
    ev.alt = args.value("alt", false);
    ev.super = args.value("super", false);
    ev.holdFrames = std::clamp(args.value("holdFrames", 2), 1, 600);

    return InjectAndReport(host, ev, json{{"key", ev.keyName}});
}

json HandleUiTypeText(const json& args, const McpHost& host)
{
    if (!host.injectUiInput)
        return Unavailable("the UI input hooks are not wired up");
    if (!args.contains("text") || !args["text"].is_string())
        return Err("text is required and must be a string");

    UiInputEvent ev;
    ev.kind = UiInputEvent::Kind::Text;
    ev.text = args["text"].get<std::string>();
    if (ev.text.size() > 4096)
        return Err("text is capped at 4096 characters");

    return InjectAndReport(host, ev, json{{"text", ev.text}, {"length", ev.text.size()}});
}

// --- manifest ---------------------------------------------------------------

const std::vector<std::string> kToolNames = {
    "list_windows",       "get_window_layout", "open_window",    "close_window",
    "focus_window",       "dock_window",       "screenshot_app", "screenshot_window",
    "list_workspaces",    "load_workspace",    "save_workspace", "get_ui_log",
    "get_imgui_ini",      "ui_click",          "ui_press_key",   "ui_type_text",
    "set_screen_profile",
};

// Repeated in several descriptions: the single most important thing to know about this group.
const char* kHostNotGuest = "This is Peekaboo's OWN user interface, not the emulated Amiga. ";

} // namespace

const std::vector<std::string>& McpUiToolNames() { return kToolNames; }

void AppendMcpUiToolManifest(json& tools)
{
    auto tool = [&](const char* name, const std::string& desc, json properties = json::object(),
                    json required = json::array()) {
        // A call site that writes `{}` for a parameterless tool hands us a null, not an empty
        // object (nlohmann reads an empty initializer list as null). MCP clients validate
        // inputSchema.properties as a record and reject the whole tools/list over one null, so
        // normalise here rather than trusting every call site to spell it json::object().
        if (!properties.is_object())
            properties = json::object();
        json schema = {{"type", "object"}, {"properties", properties}};
        if (!required.empty())
            schema["required"] = required;
        tools.push_back({{"name", name}, {"description", desc}, {"inputSchema", schema}});
    };

    const json windowSchema = {
        {"type", "string"},
        {"description", "window name as shown in the View menu (\"Sprites\", \"CIA\", \"Blitter "
                        "Operations\"), or its ImGui id. Both resolve; list_windows shows each."}};

    tool(
        "list_windows",
        std::string(kHostNotGuest) +
            "Every window Peekaboo knows about: the View-menu registry, the user's panels, and any "
            "other live ImGui window (an open document's file list, a viewer, a dialog) marked "
            "kind:\"unregistered\". Each row carries `open` (the owning view's flag) alongside the "
            "live ImGui state - `exists`, `visible`, `focused`, `docked`, `dockNodeId`, "
            "`hostPanel`, `isSelectedTab` and the on-screen `rect`. Those two halves disagree in "
            "exactly the interesting cases: open:true with exists:false means the view has never "
            "been submitted, and exists:true with visible:false means it is a background dock tab "
            "with no pixels on screen. kind:\"overlay\" rows (the perf overlay and graph) are "
            "drawn into the emulator window's draw list, so they own an open flag but have no "
            "window to focus, dock or screenshot. Use get_window_layout for the dock tree itself.",
        {{"group",
          {{"type", "string"},
           {"description", "only windows in this View-menu submenu, e.g. \"Debugger\""}}},
         {"openOnly",
          {{"type", "boolean"}, {"description", "only windows whose open flag is set"}}},
         {"includeUnregistered",
          {{"type", "boolean"},
           {"description", "include live windows that are not in the View menu (default true)"}}}});

    tool("get_window_layout",
         std::string(kHostNotGuest) +
             "The dock tree: the main dockspace and every panel's nested dockspace, as a flat node "
             "list. Each node reports its split axis, rect, children, the windows docked in it in "
             "tab order, and which one is the selected tab - this is the tool that answers \"what "
             "is docked to what\". `panels` lists each PanelView with the dockspace id it hosts "
             "(a panel's id and its dock node id are the same number). A node with windows but "
             "hasHostWindow:false is the classic failure: its DockSpace was never submitted, so "
             "everything inside it renders as stray floating windows instead of tabs.");

    tool(
        "open_window",
        std::string(kHostNotGuest) +
            "Open a window by writing through its View-menu flag, exactly as ticking the menu item "
            "would. Takes effect on the next frame. Fails for a name that is not in the registry "
            "and for the perf overlay/graph, which are draw-list overlays rather than windows.",
        {{"window", windowSchema}}, json::array({"window"}));

    tool("close_window",
         std::string(kHostNotGuest) +
             "Close a window by clearing its View-menu flag. Closing a panel also closes every "
             "window docked inside it, the same as clicking the panel's close button.",
         {{"window", windowSchema}}, json::array({"window"}));

    tool("focus_window",
         std::string(kHostNotGuest) +
             "Bring a window to the front, selecting its tab AND the tab of every panel that "
             "contains it. That second part matters: a window nested in a panel's dockspace stays "
             "invisible if only its own inner tab is selected, because the outer panel is still "
             "showing a different tab. Use this before reading a window's rect, or let "
             "screenshot_window do it for you.",
         {{"window", windowSchema}}, json::array({"window"}));

    tool("dock_window",
         std::string(kHostNotGuest) +
             "Move a window in the dock tree. `target` selects where: \"main\" the main dockspace, "
             "\"panel\" a named PanelView's dockspace, \"window\" the node another window already "
             "occupies, or \"float\" to undock entirely (optionally to x/y/width/height). "
             "`direction` is \"tab\" to join the target node as another tab, or left/right/up/down "
             "to split it, taking `ratio` of the space. Refused while a workspace load is still "
             "settling, and refused for a panel whose dockspace has no host window yet (open or "
             "focus that panel first) - both would otherwise produce a layout that silently snaps "
             "back or leaves the window floating. The change lands on the next frame and can take "
             "several frames to settle, so confirm with get_window_layout rather than assuming.",
         {{"window", windowSchema},
          {"target",
           {{"type", "string"},
            {"enum", json::array({"main", "panel", "window", "float"})},
            {"description", "where to dock it (default \"main\")"}}},
          {"panel", {{"type", "string"}, {"description", "panel name, when target is \"panel\""}}},
          {"targetWindow",
           {{"type", "string"}, {"description", "other window, when target is \"window\""}}},
          {"direction",
           {{"type", "string"},
            {"enum", json::array({"tab", "left", "right", "up", "down"})},
            {"description", "join as a tab, or split the target node (default \"tab\")"}}},
          {"ratio",
           {{"type", "number"},
            {"description", "fraction of the target node given to the new split, 0.05-0.95 "
                            "(default 0.5)"}}},
          {"x", {{"type", "number"}, {"description", "float position"}}},
          {"y", {{"type", "number"}, {"description", "float position"}}},
          {"width", {{"type", "number"}, {"description", "float size"}}},
          {"height", {{"type", "number"}, {"description", "float size"}}}},
         json::array({"window"}));

    tool("screenshot_app",
         std::string(kHostNotGuest) +
             "Capture the whole Peekaboo window as a PNG - menus, docks, panels, every view, and "
             "the emulator image composited in place. This is NOT the `screenshot` tool: that one "
             "reads the emulator's own texture at Amiga resolution with no application chrome, and "
             "is what you want when debugging the guest. Use this one when debugging Peekaboo "
             "itself. The capture is taken at the end of the frame, so the call parks for a frame "
             "before returning. Anything queued by open_window, dock_window or ui_click will not "
             "appear in a capture requested in the same breath - let a frame pass, or use "
             "screenshot_window, which waits. Downscaled to `maxWidth` (default 1280) because a "
             "full-resolution capture of a large window is a multi-megabyte PNG; when it is "
             "downscaled the response says so and gives the factor needed to turn image "
             "coordinates back into the application coordinates ui_click expects.",
         {{"maxWidth",
           {{"type", "integer"},
            {"description", "downscale so the image is at most this wide; 0 disables scaling "
                            "(default 1280)"}}}});

    tool(
        "screenshot_window",
        std::string(kHostNotGuest) +
            "Capture one window out of the application, cropped to its rectangle. Focuses it first "
            "by default and lets the layout settle for a few frames, because a window that is a "
            "background dock tab has no pixels on screen and would otherwise come back showing "
            "whatever is in front of it. Pass focus:false to capture strictly what is on screen "
            "now. The crop is taken from the composited application framebuffer, so anything "
            "overlapping the window - a popup, a floating window on top - is in the image too. "
            "Returns the same `image_png_base64` shape as screenshot_app, with the same maxWidth "
            "downscaling.",
        {{"window", windowSchema},
         {"focus",
          {{"type", "boolean"},
           {"description", "raise the window before capturing (default true)"}}},
         {"maxWidth",
          {{"type", "integer"},
           {"description", "downscale so the image is at most this wide; 0 disables (default "
                           "1280)"}}}},
        json::array({"window"}));

    tool(
        "list_workspaces",
        std::string(kHostNotGuest) +
            "Named window layouts saved in workspaces.json. A workspace stores every window's open "
            "flag, the user's panels and their ids, and the full ImGui dock layout, so loading one "
            "reproduces a layout exactly - which is the fastest way to get back to a layout bug. "
            "Every workspace has one layout per screen profile (hires/lores); screen_profile is "
            "the live one, and `scaled` lists workspaces with no layout of their own in it yet, "
            "which load the other profile's scaled to the window.");

    tool("load_workspace",
         std::string(kHostNotGuest) +
             "Switch to a saved layout. The ini is applied on the next frame and then settles for "
             "up to 60 frames while panel dockspaces are primed and saved tab selections "
             "re-asserted; list_windows and get_window_layout report layoutSettling:true "
             "throughout, dock_window is refused, and a screenshot taken too early catches the "
             "layout mid-move.",
         {{"name", {{"type", "string"}, {"description", "workspace name"}}}},
         json::array({"name"}));

    tool("save_workspace",
         std::string(kHostNotGuest) +
             "Save the current layout under a name, overwriting that workspace's layout for the "
             "live "
             "screen profile only (the other profile's layout is kept). Captures window open "
             "flags, panels and the live ImGui dock layout. Useful as a checkpoint before "
             "experimenting with dock_window.",
         {{"name", {{"type", "string"}, {"description", "workspace name"}}}},
         json::array({"name"}));

    tool("set_screen_profile",
         std::string(kHostNotGuest) +
             "Switch the screen profile: hires for large screens, lores for small ones (a laptop, "
             "Remote Desktop). Each profile keeps its own workspace layouts, text sizes, main "
             "window rectangle and table column widths; this is the View > Screen Profile menu. "
             "Records the choice in user_prefs.json.",
         {{"profile",
           {{"type", "string"},
            {"enum", json::array({"hires", "lores"})},
            {"description", "the profile to switch to"}}}},
         json::array({"profile"}));

    tool(
        "get_ui_log",
        std::string(kHostNotGuest) +
            "Tail Peekaboo's own log - the contents of the Log window, i.e. what Peekaboo itself "
            "reports about loading cores, mounting images, workspace restores and MCP calls. "
            "Nothing the guest program printed appears here; guest output comes from the kprintf "
            "tools and the debugger console. Newest entries last.",
        {{"limit",
          {{"type", "integer"},
           {"description", "how many of the most recent entries to return, 1-2000 (default 100)"}}},
         {"level",
          {{"type", "string"},
           {"enum", json::array({"Info", "Warning", "Error", "Debug"})},
           {"description", "only entries at this level"}}}});

    tool(
        "get_imgui_ini",
        std::string(kHostNotGuest) +
            "The live window layout in imgui.ini format - window positions, sizes, collapsed and "
            "docked state, and the whole dock node tree with its splits and selected tabs. "
            "Peekaboo keeps no imgui.ini on disk (layouts live in workspaces.json), so this is the "
            "only way to see what ImGui actually holds. Diffable: capture it before and after a "
            "workspace load to see exactly which window moved.");

    tool("ui_click",
         std::string(kHostNotGuest) +
             "Click, move or drag the mouse inside Peekaboo's own interface. This is NOT "
             "click_mouse, which presses the emulated Amiga's mouse button - nothing here reaches "
             "the guest. Coordinates are application coordinates, the same space list_windows "
             "reports rects in; read them off a screenshot_app capture, remembering to undo any "
             "downscale factor the capture reported. There is no way to click a widget by name - "
             "the ImGui test engine is not built in - so the loop is: screenshot, find the "
             "coordinate, click, screenshot again. The synthetic cursor position is re-asserted "
             "every frame while the click plays out, so it overrides the real mouse; do not drive "
             "the physical mouse at the same time. Build a drag from action \"down\", then "
             "\"move\", "
             "then \"up\".",
         {{"x", {{"type", "number"}, {"description", "application x coordinate"}}},
          {"y", {{"type", "number"}, {"description", "application y coordinate"}}},
          {"button",
           {{"type", "string"},
            {"enum", json::array({"left", "right", "middle"})},
            {"description", "default \"left\""}}},
          {"action",
           {{"type", "string"},
            {"enum", json::array({"click", "move", "down", "up"})},
            {"description", "\"click\" presses and releases; down/move/up build a drag "
                            "(default \"click\")"}}},
          {"holdFrames",
           {{"type", "integer"},
            {"description", "frames to hold a click before releasing, 1-600 (default 2)"}}},
          {"ctrl", {{"type", "boolean"}}},
          {"shift", {{"type", "boolean"}}},
          {"alt", {{"type", "boolean"}}}},
         json::array({"x", "y"}));

    tool(
        "ui_press_key",
        std::string(kHostNotGuest) +
            "Press a key in Peekaboo's own interface, for its keyboard shortcuts. This is NOT "
            "press_key, which types on the emulated Amiga's keyboard. Key names are ImGui's: "
            "\"A\"-\"Z\", \"0\"-\"9\", \"F1\"-\"F24\", \"Enter\", \"Escape\", \"Space\", \"Tab\", "
            "\"Backspace\", \"Delete\", \"LeftArrow\", \"RightArrow\", \"UpArrow\", \"DownArrow\", "
            "\"Home\", \"End\", \"PageUp\", \"PageDown\", \"Insert\", \"Keypad0\"-\"Keypad9\". "
            "Modifiers are held around the key press. The key goes to whatever currently has "
            "keyboard focus, so focus_window first if it matters.",
        {{"key", {{"type", "string"}, {"description", "ImGui key name, e.g. \"F5\""}}},
         {"holdFrames",
          {{"type", "integer"}, {"description", "frames to hold the key, 1-600 (default 2)"}}},
         {"ctrl", {{"type", "boolean"}}},
         {"shift", {{"type", "boolean"}}},
         {"alt", {{"type", "boolean"}}},
         {"super", {{"type", "boolean"}}}},
        json::array({"key"}));

    tool(
        "ui_type_text",
        std::string(kHostNotGuest) +
            "Type text into Peekaboo's own interface - a search box, a filter field, the debugger "
            "console's input. This is NOT type_text, which types on the emulated Amiga's keyboard. "
            "The text goes to whatever ImGui widget has keyboard focus, so click into the field "
            "first with ui_click. Capped at 4096 characters.",
        {{"text", {{"type", "string"}, {"description", "text to type (max 4096 characters)"}}}},
        json::array({"text"}));
}

std::optional<json> DispatchMcpUiTool(const std::string& name, const json& args,
                                      const McpHost& host)
{
    if (name == "list_windows")
        return HandleListWindows(args, host);
    if (name == "get_window_layout")
        return HandleGetWindowLayout(host);
    if (name == "open_window")
        return HandleSetOpen(args, host, true);
    if (name == "close_window")
        return HandleSetOpen(args, host, false);
    if (name == "focus_window")
        return HandleFocusWindow(args, host);
    if (name == "dock_window")
        return HandleDockWindow(args, host);
    if (name == "screenshot_app")
        return HandleScreenshotApp(args, host);
    if (name == "screenshot_window")
        return HandleScreenshotWindow(args, host);
    if (name == "list_workspaces")
        return HandleListWorkspaces(host);
    if (name == "load_workspace")
        return HandleLoadWorkspace(args, host);
    if (name == "save_workspace")
        return HandleSaveWorkspace(args, host);
    if (name == "set_screen_profile")
        return HandleSetScreenProfile(args, host);
    if (name == "get_ui_log")
        return HandleGetUiLog(args, host);
    if (name == "get_imgui_ini")
        return HandleGetImguiIni(host);
    if (name == "ui_click")
        return HandleUiClick(args, host);
    if (name == "ui_press_key")
        return HandleUiPressKey(args, host);
    if (name == "ui_type_text")
        return HandleUiTypeText(args, host);
    return std::nullopt;
}

} // namespace adm::core
