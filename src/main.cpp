// clang-format off
#include <glad/glad.h>
#include <GLFW/glfw3.h>
// clang-format on
#include <fmt/core.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <nfd.h>
#include <stb_image.h>

#include "core/LaunchOptions.h"
#include "core/Logger.h"
#include "core/PrivateFeatures.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <objc/message.h>
#include <objc/objc-runtime.h>

// Helper to set the Dock icon on macOS when running the binary directly
void set_macos_dock_icon(const char* imagePath)
{
    id nsApp = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"),
                                              sel_registerName("sharedApplication"));

    // Transform process into a foreground application
    // 0 = NSApplicationActivationPolicyRegular
    ((void (*)(id, SEL, long))objc_msgSend)(nsApp, sel_registerName("setActivationPolicy:"), 0);

    id nsImageClass = (id)objc_getClass("NSImage");
    id nsStringClass = (id)objc_getClass("NSString");

    id pathStr = ((id(*)(id, SEL, const char*))objc_msgSend)(
        nsStringClass, sel_registerName("stringWithUTF8String:"), imagePath);
    id imageAlloc = ((id(*)(id, SEL))objc_msgSend)(nsImageClass, sel_registerName("alloc"));
    id image = ((id(*)(id, SEL, id))objc_msgSend)(
        imageAlloc, sel_registerName("initWithContentsOfFile:"), pathStr);

    if (image) {
        ((void (*)(id, SEL, id))objc_msgSend)(nsApp, sel_registerName("setApplicationIconImage:"),
                                              image);
    } else {
        adm::core::Logger::Warn("Startup: Failed to load Dock icon from: {}", imagePath);
    }

    ((void (*)(id, SEL, bool))objc_msgSend)(nsApp, sel_registerName("activateIgnoringOtherApps:"),
                                            true);
}
#endif

#include "core/AmigaEmulator.h"
#include "core/Application.h"
#include "core/Constants.h"
#include "ui/MainLayout.h"
#include "ui/TableLayout.h"
#include "ui/views/BitmapView.h"
#include "ui/views/BlitterMintermView.h"
#include "ui/views/BlitterView.h"
#include "ui/views/BlockEditorView.h"
#include "ui/views/BlockMapView.h"
#include "ui/views/ChangeMapView.h"
#include "ui/views/DebuggerView.h"
#include "ui/views/DiskIoView.h"
#include "ui/views/DmaStatsView.h"
#include "ui/views/DmsView.h"
#include "ui/views/EmulatorView.h"
#include "ui/views/ExocetView.h"
#include "ui/views/IdentifyView.h"
#include "ui/views/LogView.h"
#include "ui/views/McpActivityView.h"
#include "ui/views/MemoryMapView.h"
#include "ui/views/NDOSView.h"
#include "ui/views/PanelView.h"
#include "ui/views/RegisterOpsView.h"
#include "ui/views/TosecBrowserView.h"

// Default ImGui layout when no imgui.ini exists
static const char* kDefaultImGuiLayout = R"(
[Window][WindowOverViewport_11111111]
Pos=0,32
Size=1280,688
Collapsed=0

[Window][Documents##panel_AD001234]
Pos=0,32
Size=400,400
Collapsed=0
DockId=0x00000001,0

[Window][Block]
Pos=402,32
Size=500,400
Collapsed=0
DockId=0x00000002,0

[Window][Block Details]
Pos=0,434
Size=902,286
Collapsed=0
DockId=0x00000007,0

[Window][Block Type Map]
Pos=904,32
Size=376,688
Collapsed=0
DockId=0x00000009,0

[Window][Disk Bitmap]
Pos=904,32
Size=376,688
Collapsed=0
DockId=0x00000009,1

[Window][NDOS Map]
Pos=904,32
Size=376,688
Collapsed=0
DockId=0x00000009,2

[Window][Change Map]
Pos=1524,22
Size=376,1066
Collapsed=0
DockId=0x00000009,3

[Window][DMS Information]
Pos=0,516
Size=400,284
Collapsed=0
DockId=0x0000000A,1

[Window][Identify Disk]
Pos=0,516
Size=400,284
Collapsed=0
DockId=0x0000000A,0

[Table][0x6A9A5C90,6]
Column 0  Weight=2.0461 Sort=0v
Column 1  Weight=0.5208
Column 2  Weight=0.3667
Column 3  Weight=0.5155
Column 4  Weight=1.2967
Column 5  Weight=1.2542

[Table][0xBE9496E9,8]
Column 0  Weight=1.0000
Column 1  Weight=1.0000
Column 2  Weight=1.0000
Column 3  Weight=1.0000
Column 4  Weight=1.0000
Column 5  Weight=1.0000
Column 6  Weight=1.0000
Column 7  Weight=1.0000

[Docking][Data]
DockSpace         ID=0x08BD597D Window=0x1BBC0F80 Pos=0,32 Size=1280,688 Split=X
  DockNode        ID=0x00000008 Parent=0x08BD597D SizeRef=902,688 Split=Y
    DockNode      ID=0x00000006 Parent=0x00000008 SizeRef=1280,400 Split=X
      DockNode    ID=0x00000001 Parent=0x00000006 SizeRef=400,400 Selected=0xBEA19C1B
      DockNode    ID=0x00000002 Parent=0x00000006 SizeRef=500,400 CentralNode=1 Selected=0x8B16F26B
    DockNode      ID=0x00000007 Parent=0x00000008 SizeRef=1280,286 Selected=0x7C51A130
  DockNode        ID=0x00000009 Parent=0x08BD597D SizeRef=376,688 Selected=0x1D712532
DockSpace         ID=0xAD001234 Window=0xBEA19C1B Pos=0,32 Size=400,400


[Window][###AudioPlayer]
Pos=0,0
Size=980,1035
Collapsed=0

)";

static void LoadDefaultLayout()
{
    // Establish a deterministic fallback. A remembered workspace, when present, is loaded
    // after Application initialization and replaces this layout before the first frame.
    // The user's stored column layout is merged over the two [Table] sections the fallback
    // carries, so columns survive a plain quit-and-relaunch with no workspace involved.
    const std::string layout = adm::ui::GlobalTableLayout().ApplyTo(kDefaultImGuiLayout);
    ImGui::LoadIniSettingsFromMemory(layout.c_str(), layout.size());
}

// GLFW reports some conditions once per call for as long as they last, and the ImGui GLFW
// backend queries every monitor's video mode from NewFrame on every frame. So a display device
// replaced underneath us - routine over Remote Desktop, where the virtual display is torn down
// and recreated - makes glfwGetVideoMode fail at vsync rate until GLFW re-polls monitors on
// WM_DISPLAYCHANGE, burying the log in identical warnings. Collapse consecutive duplicates and
// report the count once the burst stops. Main thread only, like every other GLFW call here.
static std::string g_lastGlfwError;
static int g_glfwErrorRepeats = 0;
static std::chrono::steady_clock::time_point g_lastGlfwErrorAt;

static void FlushGlfwErrorRepeats()
{
    if (g_glfwErrorRepeats == 0)
        return;
    adm::core::Logger::Warn("GLFW: previous error repeated {} more time{}", g_glfwErrorRepeats,
                            g_glfwErrorRepeats == 1 ? "" : "s");
    g_glfwErrorRepeats = 0;
    g_lastGlfwError.clear();
}

static void glfw_error_callback(int error, const char* description)
{
    // Deliberately not glfwGetTime(): this can fire before glfwInit succeeds, and calling back
    // into GLFW uninitialised would raise GLFW_NOT_INITIALIZED and re-enter this callback.
    g_lastGlfwErrorAt = std::chrono::steady_clock::now();

    std::string message = fmt::format("GLFW: Error {}: {}", error, description);
    if (message == g_lastGlfwError) {
        ++g_glfwErrorRepeats;
        return;
    }
    FlushGlfwErrorRepeats();
    g_lastGlfwError = std::move(message);
    adm::core::Logger::Warn("{}", g_lastGlfwError);
}

// Global state for window close handling
static adm::core::Application* g_app = nullptr;
static bool g_pendingWindowClose = false;

// The Emulator menu's controller list derives from GLFW joystick state, which only changes on
// connect/disconnect (and when the user reassigns a controller type). Rebuild it on those events
// instead of polling all 16 joystick slots — with their string allocations and config-map lookups —
// every frame. Starts dirty so the list is built on the first frame.
static bool g_controllersDirty = true;

static void glfw_joystick_callback(int /*jid*/, int /*event*/)
{
    g_controllersDirty = true;
    if (g_app && g_app->GetEmulatorView())
        g_app->GetEmulatorView()->RefreshConnectedControllers();
}

static void glfw_window_close_callback(GLFWwindow* window)
{
    if (g_app && g_app->HasUnsavedChanges()) {
        // Prevent immediate close, let the main loop handle the dialog
        glfwSetWindowShouldClose(window, GLFW_FALSE);
        g_pendingWindowClose = true;
    }
}

int main(int argc, char** argv)
{
    // Check for help argument early
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help" || arg == "/?") {
            fmt::print("Usage: peekaboo.exe [options] [filename]\n\n");
            fmt::print("Options:\n");
            fmt::print("  -ADF=\"path\"    Load an ADF file. Can be specified multiple times.\n");
            fmt::print("  --run          Immediately run the loaded disk in the emulator.\n");
            fmt::print("  --turbo        Start the emulator in turbo mode.\n");
            fmt::print("  --mute         Start the emulator with audio muted.\n");
            fmt::print("  --dh0-dir PATH Mount a host directory as DH0.\n");
            fmt::print("  --dh1-dir PATH Mount a host directory as DH1.\n");
            fmt::print("  --dh0-ro       Make the command-line DH0 mount read-only.\n");
            fmt::print("  --dh1-ro       Make the command-line DH1 mount read-only.\n");
            fmt::print("  --play-scenario PATH  Replay a .pkscn scenario file.\n");
            fmt::print("\nRunning several instances side by side (each defaults to a plain "
                       "launch):\n");
            fmt::print(
                "  --profile DIR     Keep user_prefs.json, workspaces, layouts, annotations,\n"
                "                    locks and saved states in DIR\n"
                "                    (created if missing) instead of next to the exe.\n");
            fmt::print("  --http-port N     Local HTTP API port (default 7474).\n");
            fmt::print("  --mcp-port N      MCP tool server port (default: the prefs' mcpPort).\n");
            fmt::print("  --mcp-token T     MCP token (default: the prefs' mcpToken).\n");
            fmt::print("  --gdb-port N      GDB server port (default: the prefs' gdbPort).\n");
            fmt::print("  --silent          No sound at all: host mute and Paula's channel mask\n"
                       "                    held off from the first frame.\n");
            fmt::print("  --model ID        Select a machine preset (e.g. A500) before running.\n");
            fmt::print("  --minimized       Start with the window minimised.\n");
            fmt::print("  --hidden          Never show the window (MCP and HTTP still work).\n");
            fmt::print("  Ports, token and --silent are this launch's only; nothing is saved.\n");
            fmt::print("  -h, --help, /? Show this help message.\n");
            return 0;
        }
    }
    // Per-launch overrides. Read before Application exists: its constructor opens the
    // preferences, the state files and the HTTP server.
    {
        auto& launch = adm::core::GetLaunchOptions();
        auto intArg = [](const std::string& v, std::optional<int>& out) {
            const int n = std::atoi(v.c_str());
            if (n > 0 && n < 65536)
                out = n;
        };
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const bool hasValue = i + 1 < argc;
            if (arg == "--profile" && hasValue) {
                launch.profileDir = std::filesystem::absolute(std::filesystem::u8path(argv[++i]));
            } else if (arg == "--http-port" && hasValue) {
                intArg(argv[++i], launch.httpPort);
            } else if (arg == "--mcp-port" && hasValue) {
                intArg(argv[++i], launch.mcpPort);
            } else if (arg == "--gdb-port" && hasValue) {
                intArg(argv[++i], launch.gdbPort);
            } else if (arg == "--mcp-token" && hasValue) {
                launch.mcpToken = argv[++i];
            } else if (arg == "--model" && hasValue) {
                launch.model = argv[++i];
            } else if (arg == "--silent") {
                launch.silent = true;
            } else if (arg == "--minimized") {
                launch.minimized = true;
            } else if (arg == "--hidden") {
                launch.hidden = true;
            } else if (arg == "--private-features") {
                launch.privateFeatures = true;
            }
        }
        if (!launch.profileDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(launch.profileDir, ec);
            if (ec) {
                adm::core::Logger::Error("Startup: cannot make the profile directory {}: {}",
                                         launch.profileDir.string(), ec.message());
                return 1;
            }
        }
    }

    // Set CWD to executable directory to ensure assets are found
    if (argc > 0) {
        try {
            std::filesystem::path exePath =
                std::filesystem::canonical(std::filesystem::path(argv[0]));
            if (std::filesystem::exists(exePath)) {
                std::filesystem::path exeDir = exePath.parent_path();
                std::filesystem::current_path(exeDir);
                adm::core::Logger::Info("Startup: Executable path: {}", exePath.string());
                adm::core::Logger::Info("Startup: Working directory set to: {}",
                                        std::filesystem::current_path().string());
            }
        } catch (const std::exception& e) {
            adm::core::Logger::Error("Startup: Failed to set CWD to executable directory: {}",
                                     e.what());
        }
    }

    // Verify the emulator core up front. If a core file is present but is not our patched PuAE
    // fork, fail fast with a clear message rather than crashing later on a missing symbol. A
    // genuinely absent core is fine here — the disk-manager features work without it, and the
    // emulator path reports its own error when first used.
    {
        std::string corePath = adm::AmigaEmulator::GetDefaultCorePath();
        std::string coreError;
        if (std::filesystem::exists(corePath) &&
            !adm::AmigaEmulator::VerifyCore(corePath, coreError)) {
            adm::core::Logger::Error("Startup: Fatal: {}", coreError);
            return 1;
        }
    }

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    // GL 3.3 + GLSL 330
    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // Initialize Application early to load preferences
    adm::core::Application app;
    g_app = &app;
    const auto& prefs = app.GetUserPrefs().Get();

    // Create window.
    //
    // Saved geometry is not trusted. Windows reports a minimized window as being at
    // (-32000, -32000) with a zero client size, and if that ever reaches the prefs file
    // the next launch creates a 0x0 window parked off every monitor: no visible window,
    // no way back except editing JSON by hand. Fall back to the defaults instead.
    constexpr int kMinWindowW = 320;
    constexpr int kMinWindowH = 240;
    int startW = prefs.windowWidth;
    int startH = prefs.windowHeight;
    int startX = prefs.windowX;
    int startY = prefs.windowY;
    if (startW < kMinWindowW || startH < kMinWindowH) {
        adm::core::Logger::Warn("Startup: ignoring saved window size {}x{} (too small); "
                                "falling back to 1280x720",
                                startW, startH);
        startW = 1280;
        startH = 720;
        startX = -1; // a bogus size usually comes with a bogus position
        startY = -1;
    }

    const auto& launch = adm::core::GetLaunchOptions();
    if (prefs.maximized && !launch.minimized && !launch.hidden) {
        glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    }
    if (launch.hidden) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    GLFWwindow* window = glfwCreateWindow(
        startW, startH,
        fmt::format("{} {}", adm::core::constants::AppName, adm::core::constants::AppVersion)
            .c_str(),
        nullptr, nullptr);

    if (window == nullptr) {
        return 1;
    }

    // Set application icon
    {
        std::filesystem::path iconPath = "assets/logo/peekaboo_icon.png";
        if (std::filesystem::exists(iconPath)) {
            int w, h, channels;
            unsigned char* pixels = stbi_load(iconPath.string().c_str(), &w, &h, &channels, 4);
            if (pixels) {
#ifndef _WIN32
                // Set GLFW window icon (works on Linux/Windows; no-op on macOS)
                GLFWimage glfwIcon;
                glfwIcon.width = w;
                glfwIcon.height = h;
                glfwIcon.pixels = pixels;
                glfwSetWindowIcon(window, 1, &glfwIcon);
#endif

#ifdef __APPLE__
                // On macOS, also set the Dock icon via NSApplication
                std::filesystem::path absPath = std::filesystem::absolute(iconPath);
                set_macos_dock_icon(absPath.string().c_str());
#endif
                stbi_image_free(pixels);
            }
        }
    }

    // Restore the position only if the title bar would land on a monitor that is
    // actually connected. Unplugging a second display, or a saved off-screen position,
    // would otherwise put the window somewhere the user cannot reach it.
    if (startX != -1 && startY != -1) {
        bool onAMonitor = false;
        int monitorCount = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
        for (int i = 0; i < monitorCount && !onAMonitor; ++i) {
            int mx = 0, my = 0, mw = 0, mh = 0;
            glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
            // Require a usable strip of the title bar to be inside the work area, not
            // merely one corner pixel.
            const int titleBarY = startY;
            if (startX + kMinWindowW > mx && startX < mx + mw && titleBarY >= my &&
                titleBarY < my + mh) {
                onAMonitor = true;
            }
        }
        if (onAMonitor) {
            glfwSetWindowPos(window, startX, startY);
        } else {
            adm::core::Logger::Warn("Startup: saved window position ({}, {}) is not on any "
                                    "connected monitor; letting the OS place the window",
                                    startX, startY);
        }
    }

    if (launch.minimized && !launch.hidden)
        glfwIconifyWindow(window);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Set up window close callback to handle unsaved changes
    glfwSetWindowCloseCallback(window, glfw_window_close_callback);

    // Flag the Emulator menu's controller list for rebuild whenever a joystick is plugged
    // in or removed (fires on the main thread during glfwPollEvents).
    glfwSetJoystickCallback(glfw_joystick_callback);

    // Keep Application's stored windowed geometry up to date so ToggleFullscreen can
    // restore to the exact position/size the window had before going fullscreen.
    // Minimizing fires both callbacks with the placeholder geometry Windows uses for an
    // iconified window ((-32000, -32000), 0x0). Recording that would persist a window
    // the next launch cannot show, so ignore events while iconified or degenerate.
    glfwSetWindowPosCallback(window, [](GLFWwindow* win, int x, int y) {
        if (g_app && glfwGetWindowMonitor(win) == nullptr &&
            !glfwGetWindowAttrib(win, GLFW_ICONIFIED) && x > -30000 && y > -30000)
            g_app->OnWindowMoved(x, y);
    });
    glfwSetWindowSizeCallback(window, [](GLFWwindow* win, int w, int h) {
        if (g_app && glfwGetWindowMonitor(win) == nullptr &&
            !glfwGetWindowAttrib(win, GLFW_ICONIFIED) && w > 0 && h > 0)
            g_app->OnWindowResized(w, h);
    });

    // Detect DPI scale
    float x_scale, y_scale;
    glfwGetWindowContentScale(window, &x_scale, &y_scale);
    float ui_scale = x_scale; // Use x_scale as the master scale
    adm::core::Logger::Info("Startup: Detected DPI scale: {:.2f} (x={:.2f}, y={:.2f})", ui_scale,
                            x_scale, y_scale);

    // Initialize Glad
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        adm::core::Logger::Error("Startup: Failed to initialize GLAD");
        return 1;
    }

    // Initialize Native File Dialog
    if (NFD_Init() != NFD_OKAY) {
        adm::core::Logger::Error("Startup: Failed to initialize NFD");
        return 1;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Layouts are owned by WorkspaceManager. Do not let ImGui load or overwrite imgui.ini
    // implicitly when the application starts or exits.
    io.IniFilename = nullptr;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Screen profile: the user's choice, or on a first launch the one suited to this screen,
    // confirmed by a prompt on the first frame. Settled before any layout or column store is
    // read, so the first frame already fits.
    const auto chosenProfile =
        adm::core::ParseScreenProfile(app.GetUserPrefs().Get().screenProfile);
    const adm::core::ScreenProfile startProfile =
        chosenProfile ? *chosenProfile
                      : adm::core::Application::SuggestScreenProfile(
                            window, app.GetUserPrefs().Get().fonts.uiScale);
    adm::ui::GlobalTableLayout().SetFile(adm::ui::TableLayoutStore::FileFor(startProfile));

    // Load a deterministic fallback; saved workspaces are restored below. Table column layout
    // is read first so it can be merged into every layout applied from here on.
    adm::ui::GlobalTableLayout().Load();
    LoadDefaultLayout();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Initialize Application UI
    app.Initialize(io, window, ui_scale);
    app.InitScreenProfile(startProfile);

    // A first start, or a remembered workspace that is gone, opens the Tosec workspace: the
    // browser to pick a disk from and the emulator to run it. Without a workspace the only layout
    // is the fallback above, which leaves today's windows floating where they fall.
    std::string selectedWorkspace = app.GetUserPrefs().Get().activeWorkspace;
    if (selectedWorkspace.empty() || !app.GetWorkspaceManager().FindWorkspace(selectedWorkspace))
        selectedWorkspace = "Tosec";
    if (app.GetWorkspaceManager().FindWorkspace(selectedWorkspace)) {
        app.LoadWorkspace(selectedWorkspace);
    } else {
        selectedWorkspace.clear();
    }
    // A new screen profile brings its own layout of the workspace on screen.
    app.SetOnScreenProfileChanged([&app, &selectedWorkspace]() {
        if (selectedWorkspace.empty())
            return;
        const std::string name = selectedWorkspace;
        app.GetWorkspaceTransitionManager().StartTransition(
            name, [&app, name]() { app.LoadWorkspace(name); });
    });

    // File open callback
    auto onOpenFile = [&](const std::string& path) { app.LoadAdf(path); };

    // Argument Parsing
    bool runRequested = false;
    bool turboRequested = false;
    bool muteRequested = false;
    // Audio debug capture, for scripted runs of the audio validation harness.
    std::string recordAudioPath;
    std::string recordAudioTap = "both";
    double recordAudioSeconds = 10.0;
    bool exitAfterRecord = false;
    std::string playScenarioPath;
    std::vector<std::string> filesToLoad;
    std::array<std::string, 2> commandLineHostDirectories;
    std::array<bool, 2> commandLineHostReadOnly = {false, false};

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg.rfind("--dh0-dir=", 0) == 0 || arg.rfind("--dh1-dir=", 0) == 0) {
            const int device = arg[4] == '0' ? 0 : 1;
            commandLineHostDirectories[device] = arg.substr(10);
        } else if ((arg == "--dh0-dir" || arg == "--dh1-dir") && i + 1 < argc) {
            const int device = arg[4] == '0' ? 0 : 1;
            commandLineHostDirectories[device] = argv[++i];
        } else if (arg == "--dh0-ro" || arg == "--dh1-ro") {
            commandLineHostReadOnly[arg[4] == '0' ? 0 : 1] = true;
        } else if (arg.rfind("-ADF=", 0) == 0) {
            std::string path = arg.substr(5);
            // Remove surrounding quotes if present
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
                path = path.substr(1, path.size() - 2);
            }
            filesToLoad.push_back(path);
        } else if (arg == "--run") {
            runRequested = true;
        } else if (arg == "--turbo") {
            turboRequested = true;
        } else if (arg == "--mute") {
            muteRequested = true;
        } else if (arg.rfind("--record-audio=", 0) == 0) {
            recordAudioPath = arg.substr(15);
        } else if (arg == "--record-audio" && i + 1 < argc) {
            recordAudioPath = argv[++i];
        } else if (arg.rfind("--record-tap=", 0) == 0) {
            recordAudioTap = arg.substr(13);
        } else if (arg.rfind("--record-seconds=", 0) == 0) {
            recordAudioSeconds = std::atof(arg.substr(17).c_str());
        } else if (arg.rfind("--play-scenario=", 0) == 0) {
            playScenarioPath = arg.substr(16);
        } else if (arg == "--play-scenario" && i + 1 < argc) {
            playScenarioPath = argv[++i];
        } else if (arg == "--exit-after-record") {
            exitAfterRecord = true;
        } else if (arg == "--profile" || arg == "--http-port" || arg == "--mcp-port" ||
                   arg == "--gdb-port" || arg == "--mcp-token" || arg == "--model") {
            ++i; // read before Application was made
        } else if (i == 1 && arg.rfind("-", 0) != 0) {
            // Legacy/Drag-and-drop: First argument is a file if not a flag
            filesToLoad.push_back(arg);
        }
    }

    // Silence and the machine come before any document is loaded or run, so the first frame of
    // the first run is already silent and on the chosen model.
    if (auto* emulatorView = app.GetEmulatorView()) {
        if (launch.silent) {
            emulatorView->GetEmulator()->SetForceSilent(true);
            emulatorView->SetMuted(true);
        }
        if (!launch.model.empty()) {
            bool found = false;
            for (const auto& info : adm::GetAllAmigaModels()) {
                if (launch.model == info.id) {
                    emulatorView->SetModel(info.model);
                    found = true;
                }
            }
            if (!found)
                adm::core::Logger::Error("Startup: unknown machine model '{}'", launch.model);
        }
    }

    // Load gathered files
    for (const auto& path : filesToLoad) {
        if (std::filesystem::exists(path)) {
            app.LoadAdf(path);
        } else {
            adm::core::Logger::Error("Startup: File not found: {}", path);
        }
    }

    bool hasCommandLineHostMount = false;
    if (auto* emulator = app.GetEmulatorView()) {
        auto mounts = emulator->GetConfiguredHostMounts();
        for (int device = 0; device < 2; ++device) {
            if (commandLineHostDirectories[device].empty())
                continue;
            mounts[device].enabled = true;
            mounts[device].hostDirectory =
                std::filesystem::absolute(commandLineHostDirectories[device]);
            mounts[device].volumeLabel = device == 0 ? "DH0" : "DH1";
            mounts[device].readOnly = commandLineHostReadOnly[device];
            mounts[device].bootPriority = device == 0 ? 0 : -128;
            hasCommandLineHostMount = true;
        }
        if (hasCommandLineHostMount)
            emulator->SetConfiguredHostMounts(mounts, false);
    }

    if (turboRequested) {
        app.ToggleTurboEmulator();
    }

    if (muteRequested) {
        app.SetEmulatorMuted(true);
    }

    if (runRequested) {
        if (hasCommandLineHostMount && filesToLoad.empty()) {
            std::string error;
            if (!app.GetEmulatorView()->ApplyHostMountsAndRestart(error))
                adm::core::Logger::Error("Startup: Failed to start host mounts: {}", error);
        } else {
            app.RunAdf();
        }
    }

    // Audio capture has to be armed after the emulator has started, because the core tap needs the
    // core's sample rate and the device tap needs an open audio device.
    if (!recordAudioPath.empty()) {
        adm::core::AudioRecordRequest request;
        request.path = recordAudioPath;
        request.seconds = recordAudioSeconds;
        if (recordAudioTap == "core") {
            request.tapCore = true;
        } else if (recordAudioTap == "device") {
            request.tapDevice = true;
        } else {
            request.tapCore = true;
            request.tapDevice = true;
        }

        const auto result = app.ApplyAudioRecordRequest(request);
        if (result.ok)
            adm::core::Logger::Info("Startup: {}", result.message);
        else
            adm::core::Logger::Error("Startup: audio recording failed: {}", result.message);
    }

    // Scenario playback, once the machine exists. Replay drives the machine frame by frame
    // itself, so this returns only when the scenario has finished.
    if (!playScenarioPath.empty()) {
        adm::core::ScenarioRequest request;
        request.verb = adm::core::ScenarioRequest::Verb::Play;
        request.path = playScenarioPath;
        const auto result = app.ApplyScenarioRequest(request);
        if (result.ok)
            adm::core::Logger::Info("Startup: {}", result.message);
        else
            adm::core::Logger::Error("Startup: scenario playback failed: {}", result.message);
    }

    adm::core::Logger::Info("Startup: Starting main loop...");

    // Main loop
    bool shouldExit = false;
    // Separate from shouldExit, which MainLayout::Draw() overwrites every iteration.
    bool captureComplete = false;
    double lastFrameTime = glfwGetTime();

    // Host-loop cost accounting for the audio diagnostics. The emulator is driven from this loop,
    // so if an iteration cannot fit inside the display's frame budget the emulator runs below real
    // time and audio underruns no matter how well the ring buffer is tuned.
    double loopWallStart = glfwGetTime();
    double loopUiTotal = 0.0;
    double loopSwapTotal = 0.0;
    uint64_t loopIterations = 0;

    // Cached controller list for the Emulator menu, rebuilt only when g_controllersDirty is set
    // (joystick connect/disconnect or a controller-type reassignment) rather than every frame.
    std::vector<adm::ui::ControllerInfo> cachedControllers;

    // The setup wizard is offered once a launch, when the screen profile is settled
    bool onboardingAsked = false;

    while (!glfwWindowShouldClose(window) && !shouldExit && !captureComplete &&
           !app.WantsToExit()) {
        double currentTime = glfwGetTime();
        float deltaTime = static_cast<float>(currentTime - lastFrameTime);
        lastFrameTime = currentTime;

        // Clamp deltaTime to avoid jumps after pauses/loading
        if (deltaTime > 0.1f)
            deltaTime = 0.1f;
        if (deltaTime <= 0.0f)
            deltaTime = 1.0f / 60.0f;

        const double iterStart = glfwGetTime();

        glfwPollEvents();

        // Report a collapsed burst of GLFW errors once it has gone quiet, so the count lands
        // near the errors it counts rather than waiting for the next distinct error or exit.
        if (g_glfwErrorRepeats > 0 &&
            std::chrono::steady_clock::now() - g_lastGlfwErrorAt > std::chrono::seconds(1)) {
            FlushGlfwErrorRepeats();
        }

        // Scripted audio capture: the recorder stops itself once --record-seconds is reached, so
        // this just waits for it to finish and then quits, giving the harness a run that ends on
        // its own with finalised WAV files.
        if (exitAfterRecord && !recordAudioPath.empty()) {
            const auto stats = app.MakeAudioLogSnapshot();
            if (stats.available && !stats.coreTap.recording && !stats.deviceTap.recording &&
                (stats.coreTap.framesWritten > 0 || stats.deviceTap.framesWritten > 0)) {
                // One line per cause, so a scripted run records why the stream had holes
                // rather than only that it did.
                adm::core::Logger::Info(
                    "AudioDiag: producer blocks={} frames={} size min/mean/max={}/{:.1f}/{} "
                    "gap mean/max={:.1f}/{:.1f} ms stalls={}",
                    stats.blockCount, stats.framesProduced, stats.minBlockFrames,
                    stats.meanBlockFrames, stats.maxBlockFrames, stats.produceGapMeanMs,
                    stats.produceGapMaxMs, stats.produceStalls);
                adm::core::Logger::Info(
                    "AudioDiag: emulation update mean/max={:.1f}/{:.1f} ms slow(>40ms)={} of {}",
                    stats.updateMeanMs, stats.updateMaxMs, stats.updateSlow, stats.blockCount);
                adm::core::Logger::Info(
                    "AudioDiag: discarded frames={} events={} (hitch={} accum={} cap={})",
                    stats.timeDiscardedFrames, stats.timeDiscardEvents,
                    stats.timeDiscardElapsedClamp, stats.timeDiscardAccumulatorClamp,
                    stats.timeDiscardFrameStopCap);
                adm::core::Logger::Info(
                    "AudioDiag: consumer callbacks={} underruns={} held={} droppedResampler={} "
                    "droppedOverrun={} ring min/max={}/{}",
                    stats.deviceCallbacks, stats.underrunCallbacks, stats.framesHeld,
                    stats.framesDroppedResampler, stats.framesDroppedOverrun, stats.ringLevelMin,
                    stats.ringLevelMax);
                adm::core::Logger::Info("Startup: audio capture complete, exiting");
                captureComplete = true;
            }
        }

        // Apply deferred workspace layout before ImGui processes docking state
        app.ApplyPendingWorkspaceLayout();
        app.ApplyPendingFontSettings();

        // Update ImGui DeltaTime manually before NewFrame
        io.DeltaTime = deltaTime;

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();

        // Synthetic input for Peekaboo's own UI (the ui_click / ui_press_key / ui_type_text MCP
        // tools). It has to land after the GLFW backend's NewFrame - which has just pushed the
        // real cursor position - and before ImGui::NewFrame consumes the event queue, so that an
        // injected position wins for this frame.
        app.GetUiInputInjector().Apply();
        // Overwrite what ImGui_ImplGlfw just set to ensure our stable deltaTime is used
        io.DeltaTime = deltaTime;
        ImGui::NewFrame();

        // While the emulator owns the mouse, hide the position from ImGui so widgets
        // throughout the frame don't highlight on hover even though the cursor is invisible.
        // io.MouseDelta (already computed by NewFrame) stays valid for the emulator view.
        ImVec2 capturedMousePos = io.MousePos;
        bool mouseHiddenForCapture = false;
        if (auto* emuView = app.GetEmulatorView(); emuView && emuView->IsMouseCaptured()) {
            io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
            mouseHiddenForCapture = true;
        }

        // Handle pending window close (from clicking X button with unsaved changes)
        if (g_pendingWindowClose) {
            g_pendingWindowClose = false;
            app.RequestExit();
        }

        // Create Edit Menu State
        adm::ui::EditMenuState editState;

        // Populate enabled states
        editState.canNewFolder = app.CanCreateNewFolder();
        editState.canDelete = app.CanDelete();
        editState.canScrub = app.CanScrub();
        editState.canRename = app.CanRename();
        editState.canPermissions = app.CanSetPermissions();
        editState.canRestore = app.CanRestore();
        editState.canExport = app.CanExport();

        // Populate callbacks
        editState.onNewFolder = [&]() { app.TriggerNewFolder(); };
        editState.onDelete = [&]() { app.TriggerDelete(); };
        editState.onScrub = [&]() { app.TriggerScrub(); };
        editState.onRename = [&]() { app.TriggerRename(); };
        editState.onPermissions = [&]() { app.TriggerPermissions(); };
        editState.onRestore = [&]() { app.TriggerRestore(); };
        editState.onExport = [&]() { app.TriggerExport(); };

        // Populate controller list (only when joystick state or type config changed).
        if (g_controllersDirty) {
            g_controllersDirty = false;
            cachedControllers.clear();
            if (auto* emuView = app.GetEmulatorView()) {
                auto controllers = emuView->GetConnectedControllers();
                for (const auto& c : controllers) {
                    adm::ui::ControllerInfo info;
                    info.id = c.id;
                    info.name = c.name;
                    info.type = (c.type == adm::ControllerType::CD32Controller)
                                    ? "CD32 Controller"
                                    : "Standard Joystick";
                    cachedControllers.push_back(info);
                }
            }
        }
        const std::vector<adm::ui::ControllerInfo>& uiControllers = cachedControllers;

        // Build panel menu state
        adm::ui::PanelMenuState panelState;
        for (const auto& panel : app.GetPanels()) {
            uint32_t panelId = panel->GetId();
            adm::ui::PanelInfo info;
            info.name = panel->GetName();
            info.openPtr = panel->GetOpenStatePtr();
            info.onRename = [&app, panelId]() { app.RequestRenamePanel(panelId); };
            info.onDelete = [&app, panelId]() { app.DeletePanel(panelId); };
            panelState.panels.push_back(std::move(info));
        }
        panelState.onNewPanel = [&]() { app.RequestNewPanel(); };

        // Build workspace menu state
        adm::ui::WorkspaceMenuState wsState;
        for (const auto& ws : app.GetWorkspaceManager().GetWorkspaces())
            wsState.names.push_back(ws.name);
        wsState.selectedWorkspace = selectedWorkspace;
        wsState.transitionDuration = app.GetUserPrefs().Get().transitionDuration;
        wsState.screenProfile = app.GetScreenProfile();
        for (const auto& name : wsState.names)
            if (!app.GetWorkspaceManager().HasLayout(name, wsState.screenProfile))
                wsState.scaledNames.push_back(name);
        // A scenario replay drives the UI by coordinates; a modal would swallow its clicks.
        wsState.askScreenProfile = app.IsScreenProfileUnchosen() && playScenarioPath.empty();
        wsState.suggestedProfile = startProfile;
        // The setup wizard, once: after the screen profile prompt, never over a scenario replay
        // or a window nobody sees
        if (!onboardingAsked && !wsState.askScreenProfile && !app.IsScreenProfileUnchosen()) {
            onboardingAsked = true;
            if (playScenarioPath.empty() && !adm::core::GetLaunchOptions().hidden)
                app.MaybeStartOnboarding();
        }
        wsState.onScreenProfile = [&](adm::core::ScreenProfile p) { app.SetScreenProfile(p); };

        wsState.onDurationChange = [&](float d) {
            app.GetUserPrefs().SetTransitionDuration(d);
            app.GetWorkspaceTransitionManager().SetDuration(d);
        };
        wsState.onSave = [&](const std::string& n) {
            app.SaveWorkspace(n);
            selectedWorkspace = n;
        };
        wsState.onLoad = [&](const std::string& n) {
            app.GetWorkspaceTransitionManager().StartTransition(
                n, [&app, n]() { app.LoadWorkspace(n); });
            selectedWorkspace = n;
        };
        wsState.onDelete = [&](const std::string& n) {
            app.GetWorkspaceManager().DeleteWorkspace(n);
            if (selectedWorkspace == n)
                selectedWorkspace.clear();
        };
        wsState.onRename = [&](const std::string& o, const std::string& n) {
            app.GetWorkspaceManager().RenameWorkspace(o, n);
            if (selectedWorkspace == o)
                selectedWorkspace = n;
        };

        // Draw Menu and check if we should exit (skip in fullscreen emulator mode)
        bool emulatorFullscreen = app.GetEmulatorView() && app.GetEmulatorView()->IsFullscreen();
        if (emulatorFullscreen) {
            shouldExit = false;
        } else {
            adm::ui::MainLayout::DrawContext ctx;
            // File menu
            ctx.onOpenFile = onOpenFile;
            ctx.onMountHostDirectory = [&](int device, const std::string& directory) {
                if (auto* emulator = app.GetEmulatorView()) {
                    if (directory.empty())
                        emulator->PickHostDirectory(device);
                    else
                        emulator->MountHostDirectory(device, directory);
                }
            };
            ctx.recentFiles = app.GetRecentFiles();
            ctx.recentDirectories = app.GetRecentDirectories();
            ctx.availableFonts = app.GetAvailableFonts();
            ctx.currentFont = app.GetCurrentFontName();
            ctx.onFontSelected = [&](const std::string& fontName) { app.SetCurrentFont(fontName); };
            ctx.onSave = [&]() { return app.SaveAdf(); };
            ctx.onSaveAs = [&]() { return app.SaveAdfAs(); };
            ctx.onClose = [&]() { app.CloseAdf(); };
            ctx.onCloseAll = [&]() { app.CloseAllAdf(); };
            ctx.onRequestExit = [&]() { app.RequestExit(); };
            ctx.onSaveSession = [&]() { app.SaveSession(); };
            ctx.onOpenSession = [&]() { app.OpenSession(); };
            ctx.onSaveSessionAs = [&]() { app.SaveSessionAs(); };
            // Search menu
            ctx.onFind = [&]() { app.TriggerFind(); };
            ctx.onAdvancedFind = [&]() { app.ShowAdvancedFind(); };
            ctx.onFindNext = [&]() { app.TriggerFindNext(); };
            ctx.onFindPrev = [&]() { app.TriggerFindPrevious(); };
            ctx.showAdvancedFind = app.GetShowAdvancedFindPtr();
            // Status
            ctx.hasUnsavedChanges = app.HasUnsavedChanges();
            ctx.hasFile = app.HasFileLoaded();
            ctx.hasDiskImage = app.HasDiskImageLoaded();
            // Powering on without media is valid and boots to the Kickstart screen.
            ctx.canRun = true;
            // New disk
            ctx.diskTemplates = app.GetDiskTemplates();
            ctx.onNewDisk = [&](const std::string& name) { app.OnNewDiskRequested(name); };
            ctx.bootBlockNames = app.GetBootBlockNames();
            ctx.onNewDiskAdvanced = [&](const std::string& templateName, const std::string& label,
                                        bool bootable, const std::string& bootBlockName,
                                        bool emptyStartup) {
                app.OnNewDiskAdvanced(templateName, label, bootable, bootBlockName, emptyStartup);
            };
            // Disk menu. Only the enable state is asked every frame: the dialog's seed (the
            // current label and filesystem) reads the volume's root block, so it is fetched on
            // the click through getFormatDiskInfo.
            ctx.canFormatDisk = app.CanFormatDisk();
            ctx.formatDiskUnavailableReason =
                ctx.canFormatDisk ? "" : app.GetFormatDiskUnavailableReason();
            ctx.getFormatDiskInfo = [&]() {
                adm::ui::MainLayout::DrawContext::FormatDiskInfo info;
                info.target = app.GetActiveDocumentDisplayName();
                info.volumeLabel = app.GetActiveVolumeLabel();
                info.isOFS = app.IsActiveVolumeOFS();
                return info;
            };
            ctx.onFormatDisk = [&](const std::string& label, bool useOFS) {
                app.OnFormatDisk(label, useOFS);
            };
            ctx.onShowBootBlockView = [&]() { app.ShowBootBlockView(); };
            ctx.onShowAbout = [&]() { app.ShowAbout(); };
            ctx.onWriteDisk = [&]() { app.ShowWriteDisk(); };
            ctx.onReadFloppy = [&]() { app.ShowReadDisk(); };
            ctx.onFormatFloppy = [&]() { app.ShowFormatFloppy(); };
            ctx.onCompareFloppy = [&]() { app.ShowCompareFloppy(); };
            ctx.onGwInfo = [&]() { app.TriggerGwInfo(); };
            ctx.onGwBandwidth = [&]() { app.TriggerGwBandwidth(); };
            ctx.onGwRPM = [&]() { app.TriggerGwRPM(); };
            // Edit menu / windows / panels
            ctx.editState = editState;
            ctx.windows = app.GetWindowStates();
            ctx.panelState = panelState;
            // Emulator menu
            ctx.connectedControllers = uiControllers;
            ctx.onControllerSelected = [&](const std::string& name, const std::string& type) {
                app.GetUserPrefs().SetControllerType(name, type);
                app.GetEmulatorView()->SetControllerType(name, type);
                g_controllersDirty = true; // reflect the new type in the cached menu list
            };
            ctx.onRunAdf = [&]() { app.RunAdf(); };
            ctx.onTogglePause = [&]() { app.TogglePauseEmulator(); };
            ctx.onToggleTurbo = [&]() { app.ToggleTurboEmulator(); };
            ctx.onStopEmulator = [&]() { app.StopEmulator(); };
            ctx.onResetEmulator = [&]() { app.ResetEmulator(); };
            ctx.onEjectEmulatorDisk = [&]() { app.EjectEmulatorDisk(); };
            ctx.onLaunchWhdload = [&]() {
                if (auto* emuView = app.GetEmulatorView())
                    emuView->PickWhdloadArchive();
            };
            if (auto* emuView = app.GetEmulatorView()) {
                ctx.copyFileToGuestUnavailableReason = emuView->GetCopyToGuestUnavailableReason();
                ctx.onCopyFileToGuest = [&]() {
                    if (auto* view = app.GetEmulatorView())
                        view->PickFileToCopyToGuest();
                };
            }
            ctx.onStepOver = [&]() { app.StepOverEmulator(); };
            ctx.onStepFrame = [&]() { app.StepFrameEmulator(); };
            ctx.onStepFrameBack = [&]() { app.StepFrameBackEmulator(); };
            ctx.frameHistory = app.GetFrameHistoryMenuState();
            ctx.onFrameHistoryClear = [&]() { app.ClearFrameTimeline(); };
            ctx.onFrameHistoryChanged = [&](bool enabled, int budgetMB, bool persist, bool thumbs,
                                            int thumbInterval) {
                app.SetFrameHistorySettings(enabled, budgetMB, persist, thumbs, thumbInterval);
            };
            ctx.coreVerboseLog = app.IsCoreVerboseLog();
            ctx.onCoreVerboseLogChanged = [&](bool enabled) { app.SetCoreVerboseLog(enabled); };
            ctx.onScreenshotToClipboard = [&]() {
                if (auto* emuView = app.GetEmulatorView())
                    emuView->CopyScreenshotToClipboard();
            };
            ctx.onSaveScreenshot = [&]() {
                if (auto* emuView = app.GetEmulatorView())
                    emuView->SaveScreenshotToFile();
            };
            ctx.onStepInstruction = [&]() { app.StepInstructionEmulator(); };
            ctx.isPaused = app.IsEmulatorPaused();
            ctx.isTurbo = app.IsEmulatorTurbo();
            ctx.isEmulatorRunning = app.IsEmulatorRunning();
            ctx.emulatorMouseCaptured =
                app.GetEmulatorView() && app.GetEmulatorView()->IsMouseCaptured();
            ctx.currentModelId =
                app.GetEmulatorView() ? app.GetEmulatorView()->GetCurrentModelId() : "A500";
            ctx.modelUnavailableReason = [&](const std::string& modelId) -> std::string {
                auto* emuView = app.GetEmulatorView();
                if (!emuView)
                    return {};
                for (const auto& info : adm::GetAllAmigaModels()) {
                    if (modelId == info.id)
                        return emuView->GetModelUnavailableReason(info.model);
                }
                return {};
            };
            ctx.onConfigSelected = [&](const std::string& modelId) {
                if (auto* emuView = app.GetEmulatorView()) {
                    const auto& models = adm::GetAllAmigaModels();
                    for (const auto& info : models) {
                        if (std::string(info.id) == modelId) {
                            emuView->SetModel(info.model);
                            if (app.GetDebuggerView()) {
                                app.GetDebuggerView()->MarkMemoryRegionsDirty();
                            }
                            break;
                        }
                    }
                }
            };
            ctx.onUaeConfigSelected = [&](const std::string& configPath) {
                if (auto* emuView = app.GetEmulatorView())
                    emuView->LoadUaeConfig(configPath);
            };
            ctx.currentUaeConfigName =
                app.GetEmulatorView() ? app.GetEmulatorView()->GetUaeConfigName() : std::string{};
            if (auto* emuView = app.GetEmulatorView()) {
                for (const auto& path : emuView->GetCustomConfigurations())
                    ctx.customConfigurations.push_back({path.stem().string(), path.string()});
                ctx.currentCustomConfigurationName = emuView->GetCurrentCustomConfigurationName();
            }
            ctx.onSaveCustomConfiguration = [&](const std::string& name, std::string& error) {
                auto* emuView = app.GetEmulatorView();
                return emuView && emuView->SaveCustomConfiguration(name, error);
            };
            ctx.onUpdateCustomConfiguration = [&](const std::string& path, std::string& error) {
                auto* emuView = app.GetEmulatorView();
                return emuView && emuView->UpdateCustomConfiguration(path, error);
            };
            ctx.onLoadCustomConfiguration = [&](const std::string& path, std::string& error) {
                auto* emuView = app.GetEmulatorView();
                const bool loaded = emuView && emuView->LoadCustomConfiguration(path, error);
                if (loaded && app.GetDebuggerView())
                    app.GetDebuggerView()->MarkMemoryRegionsDirty();
                return loaded;
            };
            ctx.onDeleteCustomConfiguration = [&](const std::string& path, std::string& error) {
                auto* emuView = app.GetEmulatorView();
                return emuView && emuView->DeleteCustomConfiguration(path, error);
            };
            ctx.onRenameCustomConfiguration = [&](const std::string& path,
                                                  const std::string& newName, std::string& error) {
                auto* emuView = app.GetEmulatorView();
                const bool renamed =
                    emuView && emuView->RenameCustomConfiguration(path, newName, error);
                if (renamed && app.GetDebuggerView())
                    app.GetDebuggerView()->MarkMemoryRegionsDirty();
                return renamed;
            };
            ctx.keyboardJoystickEnabled =
                app.GetEmulatorView() && app.GetEmulatorView()->IsKeyboardJoystickEnabled();
            ctx.onKeyboardJoystickToggled = [&](bool enabled) {
                if (auto* emuView = app.GetEmulatorView())
                    emuView->SetKeyboardJoystickEnabled(enabled);
                app.GetUserPrefs().SetKeyboardJoystick(enabled);
            };
            ctx.onShowPreferences = [&]() { app.ShowPreferences(); };
            ctx.onTextSize = [&](int direction) { app.StepUiScale(direction); };
            ctx.onShowAppearance = [&]() { app.ShowAppearancePreferences(); };
            ctx.onShowSetupWizard = [&]() { app.ShowSetupWizard(); };
            // View menu
            ctx.workspaces = wsState;
            ctx.documents = app.GetDiskDocumentInfos();
            ctx.onSelectDocument = [&](uint32_t id) { app.SwitchActiveDocumentById(id); };

            shouldExit = adm::ui::MainLayout::Draw(ctx);
        }

        // Dockspace - Moved to Application::RunFrame to allow Toolbar to reserve space
        // ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        // Draw all views
        app.RunFrame();

        // Update transition progress
        app.GetWorkspaceTransitionManager().Update(io.DeltaTime);

        // Restore the real mouse position before EndFrame() (inside Render) snapshots it
        // into MousePosPrev — an invalid prev position forces io.MouseDelta to zero on the
        // next frame, which would stall mouse movement sent to the emulator while captured.
        if (mouseHiddenForCapture) {
            io.MousePos = capturedMousePos;
            // The GLFW backend re-applies ImGui's cursor every frame, putting a hidden cursor
            // back to normal unless ImGui asks for none. A disabled cursor (local capture) is
            // left alone by the backend; a hidden one (Remote Desktop capture) is not.
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        }

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Handle workspace transitions
        app.GetWorkspaceTransitionManager().OnRenderEnd(display_w, display_h);

        // Overlay text during transition
        if (app.GetWorkspaceTransitionManager().IsShowingName()) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            ImFont* font48 = app.GetFontManager().GetFont("amiga-demo-pixel-type-8x8_48");
            ImFont* font24 = app.GetFontManager().GetFont("amiga-demo-pixel-type-8x8_24");

            if (!font48)
                font48 = ImGui::GetFont();
            if (!font24)
                font24 = ImGui::GetFont();

            std::string wsName = app.GetWorkspaceTransitionManager().GetPendingWorkspace();
            std::string trName = app.GetWorkspaceTransitionManager().GetCurrentTransitionName();
            std::string trAuthor = app.GetWorkspaceTransitionManager().GetCurrentTransitionAuthor();

            // Replace punctuation with spaces
            std::replace_if(
                wsName.begin(), wsName.end(), [](unsigned char c) { return std::ispunct(c); }, ' ');
            std::replace_if(
                trName.begin(), trName.end(), [](unsigned char c) { return std::ispunct(c); }, ' ');
            std::replace_if(
                trAuthor.begin(), trAuthor.end(), [](unsigned char c) { return std::ispunct(c); },
                ' ');

            // Convert to uppercase
            std::transform(wsName.begin(), wsName.end(), wsName.begin(), ::toupper);
            std::transform(trName.begin(), trName.end(), trName.begin(), ::toupper);
            std::transform(trAuthor.begin(), trAuthor.end(), trAuthor.begin(), ::toupper);

            std::string trFull = trName;
            if (!trAuthor.empty()) {
                trFull += " BY " + trAuthor;
            }

            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            ImVec2 displaySize = ImGui::GetIO().DisplaySize;

            ImVec2 textSize1 =
                font48->CalcTextSizeA(font48->LegacySize, FLT_MAX, 0.0f, wsName.c_str());
            ImVec2 textSize2 =
                font24->CalcTextSizeA(font24->LegacySize, FLT_MAX, 0.0f, trFull.c_str());

            float marginX = 40.0f;
            float marginY = 40.0f;

            ImVec2 pos1(displaySize.x - textSize1.x - marginX,
                        displaySize.y - textSize1.y - textSize2.y - marginY - 10.0f);
            ImVec2 pos2(displaySize.x - textSize2.x - marginX,
                        displaySize.y - textSize2.y - marginY);

            auto drawOutlinedText = [&](ImFont* font, const std::string& text, ImVec2 pos,
                                        ImU32 col, ImU32 outlineCol, float outlineWidth) {
                // Draw outline by offsetting in 8 directions
                for (float dx = -outlineWidth; dx <= outlineWidth; dx += outlineWidth) {
                    for (float dy = -outlineWidth; dy <= outlineWidth; dy += outlineWidth) {
                        if (dx != 0.0f || dy != 0.0f) {
                            drawList->AddText(font, font->LegacySize,
                                              ImVec2(pos.x + dx, pos.y + dy), outlineCol,
                                              text.c_str());
                        }
                    }
                }
                drawList->AddText(font, font->LegacySize, pos, col, text.c_str());
            };

            // Fade based on name display progress (0→1 over 2x transition duration).
            // Full opacity for first 60%, then fade to 0 by 100%.
            float progress = app.GetWorkspaceTransitionManager().GetNameProgress();
            float alpha = 1.0f;
            if (progress > 0.6f) {
                alpha = std::clamp(1.0f - (progress - 0.6f) / 0.4f, 0.0f, 1.0f);
            }

            // Use squared alpha for a more "physical" looking fade out
            float displayAlpha = alpha * alpha;

            ImU32 whiteAlpha = IM_COL32(255, 255, 255, (int)(displayAlpha * 255.0f));
            ImU32 blackAlpha = IM_COL32(0, 0, 0, (int)(displayAlpha * 255.0f));

            drawOutlinedText(font48, wsName, pos1, whiteAlpha, blackAlpha, 2.0f);
            drawOutlinedText(font24, trFull, pos2, whiteAlpha, blackAlpha, 2.0f);

            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        // Service any pending application screenshot here, after the last draw and before the
        // swap: this is the only point in the loop where the default framebuffer holds the image
        // that is about to be presented. The MCP tools cannot do this themselves - they run at the
        // top of the frame, when the back buffer is post-swap and undefined.
        app.GetAppCapture().Service(display_w, display_h, io.DisplaySize.x, io.DisplaySize.y);

        const double preSwap = glfwGetTime();
        glfwSwapBuffers(window);
        const double iterEnd = glfwGetTime();

        // "UI" here is everything in the iteration except the buffer swap: ImGui, the views, and
        // the emulator's own retro_run batch. The swap is separated out because with vsync on it
        // is where the loop parks waiting for the display.
        loopUiTotal += preSwap - iterStart;
        loopSwapTotal += iterEnd - preSwap;
        ++loopIterations;
    }

    if (!recordAudioPath.empty() && loopIterations > 0) {
        const double wall = glfwGetTime() - loopWallStart;
        const auto stats = app.MakeAudioLogSnapshot();
        const double audioSeconds =
            stats.coreSampleRate > 0.0
                ? static_cast<double>(stats.framesProduced) / stats.coreSampleRate
                : 0.0;
        adm::core::Logger::Info(
            "AudioDiag: host loop {} iters in {:.2f}s = {:.1f} fps | work {:.1f} ms/iter, "
            "swap-wait {:.1f} ms/iter",
            loopIterations, wall, loopIterations / wall, loopUiTotal / loopIterations * 1000.0,
            loopSwapTotal / loopIterations * 1000.0);
        adm::core::Logger::Info(
            "AudioDiag: REALTIME RATIO {:.1f}% - produced {:.2f}s of audio in {:.2f}s of wall "
            "time. Below 100% the sound card always outruns the emulator and underruns are "
            "unavoidable.",
            wall > 0.0 ? audioSeconds / wall * 100.0 : 0.0, audioSeconds, wall);
        // A PAL frame is 882 audio frames at 44.1 kHz. If the core delivers fewer, its emulated
        // frame is shorter than a standard PAL frame, so pacing retro_run at the load-time fps
        // advances the machine slower than real time.
        if (auto* emulatorView = app.GetEmulatorView()) {
            if (auto* emu = emulatorView->GetEmulator()) {
                const auto geometry = emu->GetDisplayGeometry();
                const double expected =
                    stats.targetFps > 1.0 ? stats.coreSampleRate / stats.targetFps : 882.0;
                adm::core::Logger::Info(
                    "AudioDiag: pacing targetFps={:.4f} expects {:.0f} frames/emulated-frame, core "
                    "delivers {:.1f} ({:.1f}%). Implied real frame rate {:.1f} Hz. Display {}x{} "
                    "(yAdjust {}).",
                    stats.targetFps, expected, stats.meanBlockFrames,
                    expected > 0.0 ? stats.meanBlockFrames / expected * 100.0 : 0.0,
                    expected > 0.0 ? stats.targetFps * expected / stats.meanBlockFrames : 0.0,
                    geometry.fullW, geometry.fullH, geometry.frameYAdjust);
                // A PAL frame is 312 lines x 227 CCK = 70,824. If the core reports that and still
                // emits far fewer than 882 audio frames, the frame length is fine and the audio
                // generation is short; if the CCK count itself is low, the machine really is
                // running short frames.
                adm::core::Logger::Info(
                    "AudioDiag: frames retro_run={} boundaries={} | CCK/frame mean={:.0f} "
                    "min={} max={} (PAL = 70824, i.e. {:.1f} lines of 227)",
                    stats.retroRunCalls, stats.frameBoundaries, stats.frameCckMean,
                    stats.frameCckMin, stats.frameCckMax, stats.frameCckMean / 227.0);
                // 44100 / (audio frames per emulated frame) is the frame rate the core's audio
                // generator believes it is running at. If that disagrees with the measured CCK
                // frame length, the chipset and the audio clock have diverged inside the core.
                adm::core::Logger::Info(
                    "AudioDiag: audio thinks {:.2f} Hz, chipset says {:.2f} Hz -> core owes "
                    "{:.0f} samples/frame but emits {:.1f} (short by {:.1f}%)",
                    stats.coreSampleRate / stats.meanBlockFrames,
                    stats.frameCckMean > 0.0 ? 3546895.0 / stats.frameCckMean : 0.0, expected,
                    stats.meanBlockFrames,
                    expected > 0.0 ? (1.0 - stats.meanBlockFrames / expected) * 100.0 : 0.0);

                adm::core::Custom regs;
                if (emu->GetCustomRegisters(regs)) {
                    adm::core::Logger::Info(
                        "AudioDiag: BEAMCON0=${:04X} DIWSTRT=${:04X} DIWSTOP=${:04X} "
                        "DDFSTRT=${:04X} DDFSTOP=${:04X} BPLCON0=${:04X} DMACON=${:04X}",
                        regs.beamcon0, regs.diwstrt, regs.diwstop, regs.ddfstrt, regs.ddfstop,
                        regs.bplcon0, regs.dmaconr);
                }
            }
        }
    }

    // Remember only the workspace identity. Its saved panels, visibility, and ImGui layout
    // remain untouched, so temporary changes made during this session are discarded. Table
    // column layout is the one deliberate exception - see the TableLayoutStore save below.
    app.GetUserPrefs().SetActiveWorkspace(selectedWorkspace);

    // Save window state
    if (window) {
        int x, y, w, h;
        glfwGetWindowPos(window, &x, &y);
        glfwGetWindowSize(window, &w, &h);
        int maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED);
        app.GetUserPrefs().SaveWindowCoords(x, y, w, h, maximized);

        if (app.GetHexEditorView()) {
            app.GetUserPrefs().SetShowBlock(*app.GetHexEditorView()->GetOpenStatePtr());
            app.GetUserPrefs().SetShowBlockDetails(
                *app.GetHexEditorView()->GetBlockDetailsOpenStatePtr());
            app.GetUserPrefs().SetBlockBytesPerLine(app.GetHexEditorView()->GetBytesPerLine());
            app.GetUserPrefs().SetBlockShowTrack(app.GetHexEditorView()->GetShowTrack());
        }
        if (app.GetBitmapView()) {
            app.GetUserPrefs().SetShowDiskBitmap(app.GetBitmapView()->IsOpen());
        }
        if (app.GetBlockMapView()) {
            app.GetUserPrefs().SetShowBlockTypeMap(app.GetBlockMapView()->IsOpen());
        }
        if (app.GetNdosView()) {
            app.GetUserPrefs().SetShowNdosMap(app.GetNdosView()->IsOpen());
        }
        if (app.GetChangeMapView()) {
            app.GetUserPrefs().SetShowBlockChangeMap(app.GetChangeMapView()->IsOpen());
        }
        if (app.GetIdentifyView()) {
            app.GetUserPrefs().SetShowIdentifyDisk(app.GetIdentifyView()->IsOpen());
        }
        if (app.GetLogView()) {
            app.GetUserPrefs().SetShowLog(app.GetLogView()->IsOpen());
        }
        if (app.GetMcpActivityView()) {
            app.GetUserPrefs().SetShowMcpActivity(*app.GetMcpActivityView()->GetOpenStatePtr());
        }
        if (app.GetEmulatorView()) {
            app.GetUserPrefs().SetShowEmulator(app.GetEmulatorView()->IsOpen());
            app.GetUserPrefs().SetShowPerfOverlay(*app.GetEmulatorView()->GetShowPerfOverlayPtr());
            app.GetUserPrefs().SetShowPerfGraph(*app.GetEmulatorView()->GetShowPerfGraphPtr());
            const auto& fp = app.GetEmulatorView()->GetFilterParams();
            adm::core::UserPreferences::Preferences::DisplayFilterPrefs df;
            df.windowOpen = *app.GetEmulatorView()->GetFiltersWindowOpenPtr();
            df.scanlines = fp.scanlines;
            df.scanlineStrength = fp.scanlineStrength;
            df.crt = fp.crt;
            df.maskStrength = fp.maskStrength;
            df.glowStrength = fp.glowStrength;
            df.curvature = fp.curvature;
            df.curveAmount = fp.curveAmount;
            df.vignetteStrength = fp.vignetteStrength;
            df.colorGrade = fp.colorGrade;
            df.gamma = fp.gamma;
            df.contrast = fp.contrast;
            df.saturation = fp.saturation;
            df.warmth = fp.warmth;
            app.GetUserPrefs().SetDisplayFilters(df);
        }
        if (app.GetDebuggerView()) {
            auto* dbg = app.GetDebuggerView();
            app.GetUserPrefs().SetShowDebugCpu(*dbg->GetCpuOpenPtr());
            app.GetUserPrefs().SetShowDebugCustom(*dbg->GetCustomOpenPtr());
            app.GetUserPrefs().SetShowDebugDisassembly(*dbg->GetDisassemblyOpenPtr());
            app.GetUserPrefs().SetShowDebugCopper(*dbg->GetCopperOpenPtr());
            app.GetUserPrefs().SetShowDebugMemory(*dbg->GetMemoryOpenPtr());
            app.GetUserPrefs().SetShowDebugDisplay(*dbg->GetDisplayOpenPtr());
            app.GetUserPrefs().SetShowDebugSprites(*dbg->GetSpritesOpenPtr());
            app.GetUserPrefs().SetShowDebugBitplanes(*dbg->GetBitplanesOpenPtr());
            {
                // The overlay's settings are already saved on every change (Application
                // ::SetBlitOverlaySettings writes them through); this catches the case
                // where the panel was never touched this session.
                const auto& bs = dbg->GetBlitOverlaySettings();
                app.GetUserPrefs().SetBlitOverlayPrefs(
                    {bs.enabled, bs.useProvenance, bs.outlineAll, bs.opacity, bs.decayFrames});
            }
            // Owned by the emulator rather than the view (the AR console and MCP can change them
            // too), so save the emulator's copy.
            if (auto* ev = app.GetEmulatorView()) {
                if (auto* emu = ev->GetEmulator()) {
                    app.GetUserPrefs().SetPaletteVis(emu->GetPaletteVisEnabled());
                    app.GetUserPrefs().SetBplcon1Scroll(emu->GetBplcon1ScrollEnabled());
                    {
                        const auto cb = emu->GetCopperBlock();
                        const auto pb = emu->GetBplPtrBlock();
                        adm::core::UserPreferences::Preferences::BlockPrefs bp;
                        bp.copperEnabled = cb.enabled;
                        bp.copperStart = cb.startLine;
                        bp.copperEnd = cb.endLine;
                        bp.bplptrMask = static_cast<int>(pb.mask);
                        bp.bplptrStart = pb.startLine;
                        bp.bplptrEnd = pb.endLine;
                        bp.blitterWrites = emu->GetBlitterWritesEnabled();
                        app.GetUserPrefs().SetBlockPrefs(bp);
                    }
                }
            }
            {
                const auto& rs = dbg->GetRasterSettings();
                adm::core::UserPreferences::Preferences::RasterPrefs rp;
                rp.enabled = rs.enabled;
                rp.showCopper = rs.showCopper;
                rp.showWaitSpans = rs.showWaitSpans;
                rp.showCopperWanted = rs.showCopperWanted;
                rp.showBlanking = rs.showBlanking;
                rp.showGeometryEvents = rs.showGeometryEvents;
                rp.channelMask = static_cast<int>(rs.channelMask);
                rp.opacity = rs.opacity;
                app.GetUserPrefs().SetRasterPrefs(rp);
            }
            app.GetUserPrefs().SetShowBartmanBitmaps(*dbg->GetBartmanOpenPtr());
            app.GetUserPrefs().SetShowDebugCia(*dbg->GetCIAOpenPtr());
            app.GetUserPrefs().SetShowDebugAudio(*dbg->GetAudioOpenPtr());
            app.GetUserPrefs().SetShowDebugCallStack(*dbg->GetCallStackOpenPtr());
            app.GetUserPrefs().SetShowDebugProfile(*dbg->GetProfileOpenPtr());
            app.GetUserPrefs().SetShowFrameScrubber(*dbg->GetScrubberOpenPtr());
            app.GetUserPrefs().SetFrameScrubberSettings(dbg->GetScrubberTickUnit(),
                                                        dbg->GetScrubberShowInstrNumbers(),
                                                        dbg->GetScrubberHeatMap());
            app.GetUserPrefs().SetSpritesPanelSizes(dbg->GetSpritesScanlinesPanelWidth(),
                                                    dbg->GetSpritesDetailPanelHeight());
            app.GetUserPrefs().SetMemoryBytesPerLine(dbg->GetMemoryBytesPerLine());
            app.GetUserPrefs().SetShowDebugConsole(*dbg->GetConsoleOpenPtr());
            app.GetUserPrefs().SetShowBreakpoints(*dbg->GetBreakpointsOpenPtr());
            app.GetUserPrefs().SetShowAnnotations(*dbg->GetAnnotationsOpenPtr());
            app.GetUserPrefs().SetShowDebugTrainer(*dbg->GetTrainerOpenPtr());
        }
        if (app.GetMemoryMapView()) {
            auto* mm = app.GetMemoryMapView();
            app.GetUserPrefs().SetShowMemoryMap(mm->IsOpen());
            app.GetUserPrefs().SetMemMapPanelSizes(mm->GetLeftColumnWidth(),
                                                   mm->GetPreviewHeight());
        }
        if (app.GetBlitterView()) {
            app.GetUserPrefs().SetShowBlitter(app.GetBlitterView()->IsOpen());
            app.GetUserPrefs().SetShowRegisterOps(app.GetRegisterOpsView()->IsOpen());
            app.GetUserPrefs().SetRegisterOpsFilters(app.GetRegisterOpsView()->GetSourceFilter(),
                                                     app.GetRegisterOpsView()->GetChangedOnly());
        }
        if (app.GetDiskIoView()) {
            app.GetUserPrefs().SetShowDiskIo(app.GetDiskIoView()->IsOpen());
        }
        // Hidden, it was never shown: its saved state is left for a launch that shows it
        if (app.GetExocetView() && adm::core::PrivateFeatures()) {
            app.GetUserPrefs().SetShowExocet(app.GetExocetView()->IsOpen());
        }
        if (app.GetBlitterMintermView()) {
            app.GetUserPrefs().SetShowBlitterMinterm(app.GetBlitterMintermView()->IsOpen());
        }
        if (app.GetDmaStatsView()) {
            app.GetUserPrefs().SetShowDmaStats(app.GetDmaStatsView()->IsOpen());
        }
        if (app.GetTosecBrowserView()) {
            app.GetUserPrefs().SetShowTosecBrowser(app.GetTosecBrowserView()->IsOpen());
        }
        // The Floppy Drives window's flag was restored at startup but never written
        // back, so it could not stay open across runs.
        if (app.GetFloppyDrivesView()) {
            app.GetUserPrefs().SetShowLiveFloppy(app.GetFloppyDrivesView()->IsOpen());
        }
    }

    // Table column layout, while the ImGui context that holds it still exists. Unlike the dock
    // layout this is not gated on saving a workspace - columns are adjusted in passing and the
    // user should not have to remember to record that.
    adm::ui::GlobalTableLayout().Capture();
    adm::ui::GlobalTableLayout().Save();

    app.Shutdown();

    // Cleanup
    FlushGlfwErrorRepeats();
    g_app = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    NFD_Quit();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
