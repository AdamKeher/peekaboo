#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../src/core/McpServer.h"
#include "../src/core/McpToolHandler.h"
#include "../src/core/McpUiTools.h"

using adm::core::McpHost;
using adm::core::McpToolHandler;
using adm::core::McpUiToolNames;
using adm::core::UiCaptureRequest;
using adm::core::UiCaptureResult;
using adm::core::UiDockRequest;
using adm::core::UiInputEvent;
using adm::core::UiLayoutSnapshot;
using adm::core::UiLogSnapshot;
using adm::core::UiWindowInfo;
using nlohmann::json;

namespace {

// A stand-in for the live ImGui layout. These tools are deliberately built on plain structs (see
// core/UiLayoutSnapshot.h) precisely so they can be exercised with no ImGui context at all.
UiLayoutSnapshot MakeLayout()
{
    UiLayoutSnapshot snap;
    snap.available = true;
    snap.mainDockId = 0xAD000001;

    // A registered window that exists and is the front tab of a panel.
    UiWindowInfo sprites;
    sprites.name = "Sprites";
    sprites.imguiId = "Sprites##debugger";
    sprites.group = "Debugger";
    sprites.registered = true;
    sprites.open = true;
    sprites.exists = true;
    sprites.visible = true;
    sprites.docked = true;
    sprites.isSelectedTab = true;
    sprites.dockNodeId = 0xBEEF0001;
    sprites.hostPanel = "Graphics";
    sprites.x = 10;
    sprites.y = 20;
    sprites.w = 300;
    sprites.h = 400;
    snap.windows.push_back(sprites);

    // Open, but never submitted - the case that separates the view's flag from ImGui's reality.
    UiWindowInfo cia;
    cia.name = "CIA";
    cia.imguiId = "CIA Registers##debugger";
    cia.group = "Debugger";
    cia.registered = true;
    cia.open = true;
    snap.windows.push_back(cia);

    // A draw-list overlay: an open flag with no window behind it.
    UiWindowInfo overlay;
    overlay.name = "Perf Overlay";
    overlay.group = "Performance";
    overlay.registered = true;
    overlay.isWindow = false;
    overlay.open = false;
    snap.windows.push_back(overlay);

    // A live window nobody registered (a document's file list).
    UiWindowInfo doc;
    doc.name = "workbench.adf";
    doc.imguiId = "workbench.adf";
    doc.exists = true;
    doc.visible = true;
    doc.open = true;
    snap.windows.push_back(doc);

    adm::core::UiPanelInfo panel;
    panel.name = "Graphics";
    panel.id = 0xBEEF0001;
    panel.open = true;
    panel.hosted = true;
    snap.panels.push_back(panel);

    adm::core::UiDockNodeInfo node;
    node.id = 0xBEEF0001;
    node.isLeaf = true;
    node.hasHostWindow = true;
    node.windows = {"Sprites##debugger"};
    node.selectedTab = "Sprites##debugger";
    snap.nodes.push_back(node);

    return snap;
}

// Every UI hook wired to a recording fake.
struct FakeUi
{
    UiLayoutSnapshot layout = MakeLayout();

    std::vector<std::pair<std::string, bool>> openCalls;
    std::vector<std::string> focusCalls;
    std::vector<UiDockRequest> dockCalls;
    std::vector<UiInputEvent> inputCalls;
    std::vector<std::string> loadedWorkspaces;
    std::vector<std::string> savedWorkspaces;

    // Capture is a two-step handshake: the first call queues, a later one collects.
    bool captureQueued = false;
    bool captureReady = false;
    UiCaptureRequest lastCapture;

    void Wire(McpHost& host)
    {
        host.getUiLayout = [this]() { return layout; };

        host.setWindowOpen = [this](const std::string& w, bool open, std::string& err) {
            for (const auto& win : layout.windows) {
                if (win.name != w && win.imguiId != w)
                    continue;
                if (!win.isWindow) {
                    err = "'" + w + "' is a draw-list overlay, not a window";
                    return false;
                }
                openCalls.emplace_back(w, open);
                return true;
            }
            err = "unknown window: '" + w + "'";
            return false;
        };

        host.focusWindow = [this](const std::string& w, std::string& err) {
            focusCalls.push_back(w);
            (void)err;
            return true;
        };

        host.dockWindow = [this](const UiDockRequest& req, std::string& err) {
            dockCalls.push_back(req);
            (void)err;
            return true;
        };

        host.requestAppCapture = [this](const UiCaptureRequest& req, std::string& err) {
            if (captureQueued) {
                err = "a capture is already in flight";
                return false;
            }
            captureQueued = true;
            lastCapture = req;
            return true;
        };
        host.pollAppCapture = [this]() -> std::optional<UiCaptureResult> {
            if (!captureReady)
                return std::nullopt;
            captureReady = false;
            captureQueued = false;
            UiCaptureResult r;
            r.ok = true;
            r.png = {0x89, 'P', 'N', 'G'};
            r.width = 640;
            r.height = 480;
            r.sourceWidth = 1280;
            r.sourceHeight = 960;
            r.downscaled = true;
            return r;
        };
        host.windowRect = [this](const std::string& w, UiCaptureRequest& out, std::string& err) {
            for (const auto& win : layout.windows) {
                if (win.name != w && win.imguiId != w)
                    continue;
                if (!win.exists) {
                    err = "'" + w + "' has never been submitted";
                    return false;
                }
                out.wholeApp = false;
                out.x = win.x;
                out.y = win.y;
                out.w = win.w;
                out.h = win.h;
                return true;
            }
            err = "unknown window: '" + w + "'";
            return false;
        };

        host.listWorkspaces = []() { return std::vector<std::string>{"Filesystem", "Debugger"}; };
        host.loadWorkspace = [this](const std::string& n, std::string& err) {
            if (n != "Filesystem" && n != "Debugger") {
                err = "unknown workspace: '" + n + "'";
                return false;
            }
            loadedWorkspaces.push_back(n);
            return true;
        };
        host.saveWorkspace = [this](const std::string& n, std::string& err) {
            (void)err;
            savedWorkspaces.push_back(n);
            return true;
        };

        host.getUiLog = [](int limit, const std::string& level) {
            UiLogSnapshot snap;
            snap.available = true;
            snap.total = 3;
            snap.entries.push_back({"Info", "core loaded"});
            if (level.empty())
                snap.entries.push_back({"Warning", "no elf alongside adf"});
            (void)limit;
            return snap;
        };
        host.getImguiIni = []() { return std::string("[Window][Sprites##debugger]\nPos=10,20\n"); };

        host.injectUiInput = [this](const UiInputEvent& ev, std::string& err) {
            if (ev.kind == UiInputEvent::Kind::Key && ev.keyName != "F5") {
                err = "unknown key name: '" + ev.keyName + "'";
                return false;
            }
            inputCalls.push_back(ev);
            return true;
        };
        host.uiInputBusy = []() { return false; };
    }
};

json Call(McpToolHandler& h, const std::string& name, json args = json::object())
{
    return h.Handle(name, args);
}

// Find a window row in a list_windows result.
json FindWindow(const json& result, const std::string& name)
{
    for (const auto& w : result["windows"])
        if (w.value("name", std::string{}) == name)
            return w;
    return json{};
}

} // namespace

// --- manifest ---------------------------------------------------------------

TEST(McpUiToolsTest, ManifestDeclaresEveryOwnedTool)
{
    McpToolHandler handler(McpHost{});
    const json manifest = handler.Manifest();

    std::vector<std::string> declared;
    for (const auto& t : manifest)
        declared.push_back(t.value("name", std::string{}));

    for (const auto& name : McpUiToolNames())
        EXPECT_NE(std::find(declared.begin(), declared.end(), name), declared.end())
            << "tool '" << name << "' is in McpUiToolNames() but missing from the manifest";
}

TEST(McpUiToolsTest, EveryOwnedToolIsDispatchable)
{
    // A name in the list with no handler would fall through to "unknown tool", which is the exact
    // mismatch the paired list/dispatcher layout is meant to prevent.
    McpToolHandler handler(McpHost{});
    for (const auto& name : McpUiToolNames()) {
        const json r = Call(handler, name);
        ASSERT_TRUE(r.is_object()) << name;
        const std::string err = r.value("error", std::string{});
        EXPECT_EQ(err.find("unknown tool"), std::string::npos)
            << "tool '" << name << "' is declared but not dispatched";
    }
}

// A missing hook must report unavailability rather than crash or lie.
TEST(McpUiToolsTest, ToolsReportUnavailableWithoutHooks)
{
    McpToolHandler handler(McpHost{});

    for (const char* name :
         {"list_windows", "get_window_layout", "list_workspaces", "get_ui_log", "get_imgui_ini"}) {
        const json r = Call(handler, name);
        EXPECT_FALSE(r.value("available", true)) << name;
        EXPECT_TRUE(r.contains("hint")) << name;
    }

    // The ones that take arguments still refuse before validating them.
    EXPECT_FALSE(
        Call(handler, "open_window", json{{"window", "Sprites"}}).value("available", true));
    EXPECT_FALSE(
        Call(handler, "focus_window", json{{"window", "Sprites"}}).value("available", true));
    EXPECT_FALSE(
        Call(handler, "dock_window", json{{"window", "Sprites"}}).value("available", true));
    EXPECT_FALSE(Call(handler, "ui_click", json{{"x", 1}, {"y", 2}}).value("available", true));
}

// --- list_windows -----------------------------------------------------------

TEST(McpUiToolsTest, ListWindowsSeparatesOpenFlagFromLiveState)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    const json r = Call(handler, "list_windows");
    ASSERT_TRUE(r.value("available", false)) << r.dump();

    // Docked and on screen.
    const json sprites = FindWindow(r, "Sprites");
    ASSERT_FALSE(sprites.is_null());
    EXPECT_EQ(sprites["kind"], "window");
    EXPECT_TRUE(sprites["open"]);
    EXPECT_TRUE(sprites["exists"]);
    EXPECT_TRUE(sprites["visible"]);
    EXPECT_TRUE(sprites["docked"]);
    EXPECT_EQ(sprites["hostPanel"], "Graphics");
    EXPECT_EQ(sprites["imguiId"], "Sprites##debugger");
    EXPECT_EQ(sprites["rect"]["w"], 300.0);

    // Flagged open, but ImGui has never seen it - the disagreement must survive to the caller.
    const json cia = FindWindow(r, "CIA");
    ASSERT_FALSE(cia.is_null());
    EXPECT_TRUE(cia["open"]);
    EXPECT_FALSE(cia["exists"]);
    EXPECT_FALSE(cia.contains("rect"));

    // An overlay is reported as such, so "no window to screenshot" reads as a kind, not an error.
    const json overlay = FindWindow(r, "Perf Overlay");
    ASSERT_FALSE(overlay.is_null());
    EXPECT_EQ(overlay["kind"], "overlay");

    const json doc = FindWindow(r, "workbench.adf");
    ASSERT_FALSE(doc.is_null());
    EXPECT_EQ(doc["kind"], "unregistered");
}

TEST(McpUiToolsTest, ListWindowsFilters)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    const json byGroup = Call(handler, "list_windows", json{{"group", "debugger"}});
    ASSERT_TRUE(byGroup.value("available", false));
    EXPECT_EQ(byGroup["count"], 2u); // Sprites + CIA, case-insensitively matched
    EXPECT_GT(byGroup["filteredOut"], 0);

    const json registeredOnly = Call(handler, "list_windows", json{{"includeUnregistered", false}});
    EXPECT_TRUE(FindWindow(registeredOnly, "workbench.adf").is_null());

    const json openOnly = Call(handler, "list_windows", json{{"openOnly", true}});
    EXPECT_TRUE(FindWindow(openOnly, "Perf Overlay").is_null());
}

TEST(McpUiToolsTest, ListWindowsSurfacesSettlingLayout)
{
    FakeUi fake;
    fake.layout.layoutSettling = true;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    const json r = Call(handler, "list_windows");
    EXPECT_TRUE(r.value("layoutSettling", false));
    EXPECT_TRUE(r.contains("note"));
}

// --- layout -----------------------------------------------------------------

TEST(McpUiToolsTest, WindowLayoutReportsPanelsAndNodes)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    const json r = Call(handler, "get_window_layout");
    ASSERT_TRUE(r.value("available", false)) << r.dump();
    EXPECT_EQ(r["mainDockId"], "0xAD000001");
    ASSERT_EQ(r["panels"].size(), 1u);
    EXPECT_EQ(r["panels"][0]["name"], "Graphics");
    // A panel's id and the dock node it hosts are the same number.
    EXPECT_EQ(r["panels"][0]["dockNodeId"], "0xBEEF0001");
    ASSERT_EQ(r["nodes"].size(), 1u);
    EXPECT_EQ(r["nodes"][0]["selectedTab"], "Sprites##debugger");
}

TEST(McpUiToolsTest, WindowLayoutWarnsAboutHostlessNode)
{
    FakeUi fake;
    fake.layout.nodes[0].hasHostWindow = false;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    const json r = Call(handler, "get_window_layout");
    ASSERT_TRUE(r.contains("note")) << r.dump();
    EXPECT_NE(r["note"].get<std::string>().find("no host window"), std::string::npos);
}

// --- open / close / focus ---------------------------------------------------

TEST(McpUiToolsTest, OpenAndCloseWriteThroughTheFlag)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    EXPECT_TRUE(Call(handler, "open_window", json{{"window", "Sprites"}}).value("ok", false));
    EXPECT_TRUE(Call(handler, "close_window", json{{"window", "CIA"}}).value("ok", false));
    ASSERT_EQ(fake.openCalls.size(), 2u);
    EXPECT_EQ(fake.openCalls[0], std::make_pair(std::string("Sprites"), true));
    EXPECT_EQ(fake.openCalls[1], std::make_pair(std::string("CIA"), false));
}

// The menu name and the ImGui id differ for most debugger windows; both must address the window.
TEST(McpUiToolsTest, WindowsResolveByMenuNameOrImGuiId)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    EXPECT_TRUE(Call(handler, "open_window", json{{"window", "CIA"}}).value("ok", false));
    EXPECT_TRUE(Call(handler, "open_window", json{{"window", "CIA Registers##debugger"}})
                    .value("ok", false));
}

TEST(McpUiToolsTest, UnknownWindowAndOverlayAreRejected)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    const json missing = Call(handler, "open_window", json{{"window", "Nope"}});
    ASSERT_TRUE(missing.contains("error"));
    EXPECT_NE(missing["error"].get<std::string>().find("unknown window"), std::string::npos);

    const json overlay = Call(handler, "open_window", json{{"window", "Perf Overlay"}});
    ASSERT_TRUE(overlay.contains("error"));
    EXPECT_NE(overlay["error"].get<std::string>().find("overlay"), std::string::npos);

    EXPECT_TRUE(Call(handler, "open_window").contains("error")); // window is required
}

TEST(McpUiToolsTest, FocusPassesThrough)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    EXPECT_TRUE(Call(handler, "focus_window", json{{"window", "Sprites"}}).value("ok", false));
    ASSERT_EQ(fake.focusCalls.size(), 1u);
    EXPECT_EQ(fake.focusCalls[0], "Sprites");
}

// --- docking ----------------------------------------------------------------

TEST(McpUiToolsTest, DockWindowParsesTargetAndDirection)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    const json r = Call(handler, "dock_window",
                        json{{"window", "Sprites"},
                             {"target", "panel"},
                             {"panel", "Graphics"},
                             {"direction", "right"},
                             {"ratio", 0.3}});
    ASSERT_TRUE(r.value("ok", false)) << r.dump();
    ASSERT_EQ(fake.dockCalls.size(), 1u);
    EXPECT_EQ(fake.dockCalls[0].window, "Sprites");
    EXPECT_EQ(fake.dockCalls[0].target, UiDockRequest::Target::Panel);
    EXPECT_EQ(fake.dockCalls[0].targetName, "Graphics");
    EXPECT_EQ(fake.dockCalls[0].direction, UiDockRequest::Direction::Right);
    EXPECT_FLOAT_EQ(fake.dockCalls[0].ratio, 0.3f);
}

TEST(McpUiToolsTest, DockWindowValidatesArguments)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    EXPECT_TRUE(Call(handler, "dock_window", json{{"window", "Sprites"}, {"target", "sideways"}})
                    .contains("error"));
    // target 'panel' without naming a panel is unsatisfiable, so it must be caught here.
    EXPECT_TRUE(Call(handler, "dock_window", json{{"window", "Sprites"}, {"target", "panel"}})
                    .contains("error"));
    EXPECT_TRUE(Call(handler, "dock_window",
                     json{{"window", "Sprites"}, {"target", "main"}, {"direction", "sideways"}})
                    .contains("error"));
    EXPECT_TRUE(fake.dockCalls.empty());
}

TEST(McpUiToolsTest, DockWindowRefusedWhileLayoutSettles)
{
    // The real hook refuses; the tool must surface that rather than report success.
    McpHost host;
    host.dockWindow = [](const UiDockRequest&, std::string& err) {
        err = "a workspace layout is still settling; retry in a moment";
        return false;
    };
    McpToolHandler handler(std::move(host));

    const json r = Call(handler, "dock_window", json{{"window", "Sprites"}});
    ASSERT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("settling"), std::string::npos);
}

// --- screenshots ------------------------------------------------------------

// The capture tools are stateless: the first call queues and asks to be retried, a later call
// collects. Nothing is remembered between dispatches.
TEST(McpUiToolsTest, ScreenshotAppQueuesThenCollects)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    const json first = Call(handler, "screenshot_app", json{{"maxWidth", 800}});
    EXPECT_TRUE(first.value(adm::core::McpServer::kRetryKey, false)) << first.dump();
    EXPECT_TRUE(fake.captureQueued);
    EXPECT_TRUE(fake.lastCapture.wholeApp);
    EXPECT_EQ(fake.lastCapture.maxWidth, 800);

    // Still nothing to collect - the frame has not been rendered yet.
    EXPECT_TRUE(Call(handler, "screenshot_app").value(adm::core::McpServer::kRetryKey, false));

    fake.captureReady = true;
    const json done = Call(handler, "screenshot_app");
    ASSERT_TRUE(done.contains("image_png_base64")) << done.dump();
    EXPECT_EQ(done["width"], 640);
    EXPECT_TRUE(done.value("downscaled", false));
    // The scale factor must be stated: coordinates read off the image are not click coordinates.
    ASSERT_TRUE(done.contains("note"));
    EXPECT_NE(done["note"].get<std::string>().find("2.000"), std::string::npos);
}

TEST(McpUiToolsTest, ScreenshotWindowCropsAndFocuses)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    const json first = Call(handler, "screenshot_window", json{{"window", "Sprites"}});
    EXPECT_TRUE(first.value(adm::core::McpServer::kRetryKey, false)) << first.dump();
    EXPECT_FALSE(fake.lastCapture.wholeApp);
    EXPECT_FLOAT_EQ(fake.lastCapture.w, 300.0f);
    // A background tab has no pixels on screen, so it is raised and given time to appear.
    ASSERT_EQ(fake.focusCalls.size(), 1u);
    EXPECT_GT(fake.lastCapture.delayFrames, 0);
}

// Regression: the collect path used to poll the capture a second time on its way to building the
// response. The first poll consumed the result, the second found nothing and queued a fresh
// capture, so the tool retried until the request timed out and no image ever came back.
TEST(McpUiToolsTest, ScreenshotWindowCollectsExactlyOnce)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    ASSERT_TRUE(Call(handler, "screenshot_window", json{{"window", "Sprites"}})
                    .value(adm::core::McpServer::kRetryKey, false));

    fake.captureReady = true;
    const json done = Call(handler, "screenshot_window", json{{"window", "Sprites"}});
    ASSERT_TRUE(done.contains("image_png_base64")) << done.dump();
    EXPECT_EQ(done["window"], "Sprites");
    EXPECT_FALSE(done.value(adm::core::McpServer::kRetryKey, false));
    // Nothing may be left queued: a re-request here is the bug this guards.
    EXPECT_FALSE(fake.captureQueued);
}

TEST(McpUiToolsTest, ScreenshotWindowHonoursFocusFalse)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    Call(handler, "screenshot_window", json{{"window", "Sprites"}, {"focus", false}});
    EXPECT_TRUE(fake.focusCalls.empty());
    EXPECT_EQ(fake.lastCapture.delayFrames, 0);
}

TEST(McpUiToolsTest, ScreenshotWindowRejectsUnsubmittedWindow)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    // "CIA" is flagged open but has never been submitted, so it has no rect to crop to.
    const json r = Call(handler, "screenshot_window", json{{"window", "CIA"}});
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_FALSE(fake.captureQueued);
}

TEST(McpUiToolsTest, CaptureFailureIsReportedNotRetriedForever)
{
    McpHost host;
    host.requestAppCapture = [](const UiCaptureRequest&, std::string&) { return true; };
    host.pollAppCapture = []() -> std::optional<UiCaptureResult> {
        UiCaptureResult r;
        r.ok = false;
        r.error = "the application did not render a frame within 8s";
        return r;
    };
    McpToolHandler handler(std::move(host));

    const json r = Call(handler, "screenshot_app");
    ASSERT_TRUE(r.contains("error")) << r.dump();
    EXPECT_FALSE(r.value(adm::core::McpServer::kRetryKey, false));
}

// --- workspaces / log / ini -------------------------------------------------

TEST(McpUiToolsTest, WorkspaceTools)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    const json list = Call(handler, "list_workspaces");
    ASSERT_TRUE(list.value("available", false));
    EXPECT_EQ(list["count"], 2u);

    EXPECT_TRUE(Call(handler, "load_workspace", json{{"name", "Debugger"}}).value("ok", false));
    ASSERT_EQ(fake.loadedWorkspaces.size(), 1u);
    EXPECT_EQ(fake.loadedWorkspaces[0], "Debugger");

    EXPECT_TRUE(Call(handler, "load_workspace", json{{"name", "Nope"}}).contains("error"));

    EXPECT_TRUE(Call(handler, "save_workspace", json{{"name", "mcp-test"}}).value("ok", false));
    ASSERT_EQ(fake.savedWorkspaces.size(), 1u);
    EXPECT_EQ(fake.savedWorkspaces[0], "mcp-test");
}

TEST(McpUiToolsTest, ScreenProfileTools)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    std::string profile = "hires";
    host.getScreenProfile = [&profile]() { return profile; };
    host.setScreenProfile = [&profile](const std::string& p, std::string& err) {
        if (p != "hires" && p != "lores") {
            err = "unknown screen profile";
            return false;
        }
        profile = p;
        return true;
    };
    // Only Debugger has a layout of its own in the live profile.
    host.workspaceHasOwnLayout = [](const std::string& n) { return n == "Debugger"; };
    McpToolHandler handler(std::move(host));

    json list = Call(handler, "list_workspaces");
    EXPECT_EQ(list["screen_profile"], "hires");
    EXPECT_EQ(list["scaled"], json::array({"Filesystem"}));

    EXPECT_TRUE(Call(handler, "set_screen_profile", json{{"profile", "lores"}}).value("ok", false));
    EXPECT_EQ(profile, "lores");
    EXPECT_EQ(Call(handler, "list_workspaces")["screen_profile"], "lores");

    EXPECT_TRUE(Call(handler, "set_screen_profile", json{{"profile", "4k"}}).contains("error"));
    EXPECT_TRUE(Call(handler, "set_screen_profile").contains("error"));
    EXPECT_EQ(profile, "lores");
}

TEST(McpUiToolsTest, UiLogAndIni)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    const json log = Call(handler, "get_ui_log");
    ASSERT_TRUE(log.value("available", false));
    EXPECT_EQ(log["total"], 3);
    EXPECT_EQ(log["entries"][0]["level"], "Info");

    const json filtered = Call(handler, "get_ui_log", json{{"level", "Error"}});
    EXPECT_EQ(filtered["count"], 1u);

    const json ini = Call(handler, "get_imgui_ini");
    ASSERT_TRUE(ini.value("available", false));
    EXPECT_NE(ini["ini"].get<std::string>().find("[Window][Sprites##debugger]"), std::string::npos);
}

// --- input injection --------------------------------------------------------

TEST(McpUiToolsTest, UiClickParsesActionAndButton)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    ASSERT_TRUE(Call(handler, "ui_click",
                     json{{"x", 100}, {"y", 200}, {"button", "right"}, {"action", "down"}})
                    .value("ok", false));
    ASSERT_EQ(fake.inputCalls.size(), 1u);
    EXPECT_EQ(fake.inputCalls[0].kind, UiInputEvent::Kind::MouseDown);
    EXPECT_EQ(fake.inputCalls[0].button, 1);
    EXPECT_FLOAT_EQ(fake.inputCalls[0].x, 100.0f);

    EXPECT_TRUE(
        Call(handler, "ui_click", json{{"x", 1}, {"y", 2}, {"button", "elbow"}}).contains("error"));
    EXPECT_TRUE(Call(handler, "ui_click", json{{"y", 2}}).contains("error")); // x required
}

TEST(McpUiToolsTest, UiKeyAndText)
{
    FakeUi fake;
    McpHost host;
    fake.Wire(host);
    McpToolHandler handler(std::move(host));

    ASSERT_TRUE(
        Call(handler, "ui_press_key", json{{"key", "F5"}, {"ctrl", true}}).value("ok", false));
    ASSERT_EQ(fake.inputCalls.size(), 1u);
    EXPECT_EQ(fake.inputCalls[0].keyName, "F5");
    EXPECT_TRUE(fake.inputCalls[0].ctrl);

    // An unresolvable key name is the hook's call to reject, and the tool must pass it on.
    EXPECT_TRUE(Call(handler, "ui_press_key", json{{"key", "Wibble"}}).contains("error"));

    ASSERT_TRUE(Call(handler, "ui_type_text", json{{"text", "hello"}}).value("ok", false));
    EXPECT_EQ(fake.inputCalls.back().kind, UiInputEvent::Kind::Text);
    EXPECT_EQ(fake.inputCalls.back().text, "hello");

    EXPECT_TRUE(
        Call(handler, "ui_type_text", json{{"text", std::string(5000, 'x')}}).contains("error"));
}
