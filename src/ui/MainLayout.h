#pragma once

#include <imgui.h>
#include <functional>
#include <string>
#include <vector>

#include "../core/DiskDocumentInfo.h"
#include "../core/ScreenProfile.h"

namespace adm {
namespace ui {

struct WindowState
{
    // Menu label, and the stable key everything else addresses this window by: workspaces save
    // open-state under it (Application::SaveWorkspace) and the MCP UI tools accept it as `window`.
    std::string name;
    bool* openPtr;
    bool separatorAfter = false;
    std::string groupName = ""; // If non-empty, window is grouped under a submenu with this name
    std::string icon = "";      // Optional FontAwesome icon glyph shown before the name in menus

    // The window's ImGui identity - the part of its Begin() label after "###", e.g. "Sprites" is
    // menu-labelled "Sprites" but Begins as ICON_FA_GHOST " Sprites###Sprites##debugger". Empty
    // means the label carries no "###" and `name` is already the identity. Pass this (not `name`)
    // to ImGui::FindWindowByName: ImHashStr restarts its CRC at "###", so hashing the id tail
    // alone matches the full label. Several entries genuinely differ - "Block Change Map" Begins
    // as "Change Map", "Validation" as "Disk Validation Results" - so this cannot be derived.
    std::string imguiId = "";

    // False for entries that are toggles rather than windows: the perf overlay and graph are drawn
    // into the emulator window's draw list, so they have an open flag but no ImGui window to find,
    // focus, dock or screenshot. Lets the UI tools say "toggle" instead of "window not found".
    bool isWindow = true;
};

struct ControllerInfo
{
    int id;
    std::string name;
    std::string type; // "Standard Joystick" or "CD32 Controller"
};

struct EmulatorConfigurationInfo
{
    std::string name;
    std::string path;
};

struct PanelInfo
{
    std::string name;
    bool* openPtr = nullptr;
    std::function<void()> onRename;
    std::function<void()> onDelete;
};

struct PanelMenuState
{
    std::vector<PanelInfo> panels;
    std::function<void()> onNewPanel;
};

struct WorkspaceMenuState
{
    std::vector<std::string> names;
    std::string selectedWorkspace;
    float transitionDuration = 0.6f;
    std::function<void(const std::string&)> onSave;
    std::function<void(const std::string&)> onLoad;
    std::function<void(const std::string&)> onDelete;
    std::function<void(const std::string&, const std::string&)> onRename;
    std::function<void(float)> onDurationChange;

    // Screen profile. A workspace in scaledNames has no layout of
    // its own for this profile yet and loads the other profile's, scaled to fit.
    core::ScreenProfile screenProfile = core::ScreenProfile::Hires;
    std::vector<std::string> scaledNames;
    // First launch: no profile chosen yet, so ask once, offering the one suited to this screen.
    bool askScreenProfile = false;
    core::ScreenProfile suggestedProfile = core::ScreenProfile::Hires;
    std::function<void(core::ScreenProfile)> onScreenProfile;
};

struct EditMenuState
{
    // Enabled states
    bool canNewFolder = false;
    bool canDelete = false;
    bool canScrub = false;
    bool canRename = false;
    bool canPermissions = false;
    bool canRestore = false;
    bool canExport = false;

    // Callbacks
    std::function<void()> onNewFolder;
    std::function<void()> onDelete;
    std::function<void()> onScrub;
    std::function<void()> onRename;
    std::function<void()> onPermissions;
    std::function<void()> onRestore;
    std::function<void()> onExport;
};

/**
 * @brief Main application layout with menu bar.
 *
 * Provides the File and View menus for the application.
 */
class MainLayout
{
public:
    using OpenFileCallback = std::function<void(const std::string&)>;
    using SaveCallback = std::function<bool()>;
    using CloseCallback = std::function<void()>;
    using ShowBootBlockCallback = std::function<void()>;
    using ShowAboutCallback = std::function<void()>;
    using WriteDiskCallback = std::function<void()>;
    using ReadFloppyCallback = std::function<void()>;
    using RunAdfCallback = std::function<void()>;
    using TogglePauseCallback = std::function<void()>;
    using ToggleTurboCallback = std::function<void()>;
    using StepFrameCallback = std::function<void()>;
    using StepInstructionCallback = std::function<void()>;
    using SelectDocumentCallback = std::function<void(uint32_t)>;
    using ShowPreferencesCallback = std::function<void()>;
    using SearchCallback = std::function<void()>;

    /**
     * @brief Bundles every input the main menu bar needs for a frame.
     *
     * Replaces what used to be a ~50-parameter Draw() signature. Build one each frame and pass
     * it to Draw(). Small UI lists are stored by value; copying them once per frame is cheap.
     */
    struct DrawContext
    {
        // File menu
        OpenFileCallback onOpenFile;
        std::function<void(int, const std::string&)> onMountHostDirectory;
        std::vector<std::string> recentFiles;
        std::vector<std::string> recentDirectories;
        std::vector<std::string> availableFonts;
        std::string currentFont;
        std::function<void(const std::string&)> onFontSelected;
        SaveCallback onSave;
        SaveCallback onSaveAs;
        CloseCallback onClose;
        CloseCallback onCloseAll;
        std::function<void()> onRequestExit;
        std::function<void()> onSaveSession;
        std::function<void()> onOpenSession;
        std::function<void()> onSaveSessionAs;

        // Search menu
        SearchCallback onFind;
        SearchCallback onAdvancedFind;
        SearchCallback onFindNext;
        SearchCallback onFindPrev;
        bool* showAdvancedFind = nullptr;

        // Status
        bool hasUnsavedChanges = false;
        bool hasFile = false;
        bool hasDiskImage = false;
        bool canRun = false;

        // New disk
        std::vector<std::string> diskTemplates;
        std::function<void(const std::string&)> onNewDisk;
        std::vector<std::string> bootBlockNames;
        std::function<void(const std::string&, const std::string&, bool, const std::string&, bool)>
            onNewDiskAdvanced;

        // Disk menu
        // Format acts on the active image. The label and filesystem come back from Application
        // so the dialog opens on what the disk is now rather than on a blank form.
        bool canFormatDisk = false;
        // Only the enable state is refreshed every frame. The dialog's seed comes back through
        // getFormatDiskInfo on the click, because reading the label means reading the root block.
        const char* formatDiskUnavailableReason = ""; // tooltip while disabled
        struct FormatDiskInfo
        {
            std::string target;      // document name, shown in the confirmation
            std::string volumeLabel; // current volume label
            bool isOFS = true;       // current filesystem
        };
        std::function<FormatDiskInfo()> getFormatDiskInfo;
        std::function<void(const std::string& label, bool useOFS)> onFormatDisk;
        ShowBootBlockCallback onShowBootBlockView;
        ShowAboutCallback onShowAbout;
        WriteDiskCallback onWriteDisk;
        ReadFloppyCallback onReadFloppy;
        ReadFloppyCallback onFormatFloppy;
        ReadFloppyCallback onCompareFloppy;
        WriteDiskCallback onGwInfo;
        WriteDiskCallback onGwBandwidth;
        WriteDiskCallback onGwRPM;

        // Edit menu / windows / panels
        EditMenuState editState;
        std::vector<WindowState> windows;
        PanelMenuState panelState;

        // Emulator menu
        std::vector<ControllerInfo> connectedControllers;
        std::function<void(const std::string&, const std::string&)> onControllerSelected;
        RunAdfCallback onRunAdf;
        TogglePauseCallback onTogglePause;
        ToggleTurboCallback onToggleTurbo;
        // Counterparts to the toolbar's Stop / Reset / Eject / Step Over buttons, which used to
        // be the only way to reach these.
        std::function<void()> onStopEmulator;
        std::function<void()> onResetEmulator;
        std::function<void()> onEjectEmulatorDisk;
        std::function<void()> onLaunchWhdload;
        std::function<void()> onCopyFileToGuest;
        // Empty enables the item; otherwise it is disabled and this is the tooltip.
        std::string copyFileToGuestUnavailableReason;
        std::function<void()> onStepOver;
        StepFrameCallback onStepFrame;
        // Step *backwards* one frame through the recorded frame history, re-running the
        // frame so its debugger telemetry is regenerated. Null when nothing is recorded.
        std::function<void()> onStepFrameBack;

        // Frame-history (rewind) settings shown under the Emulator menu, plus the live
        // state so the menu can say how far back the timeline currently reaches.
        struct FrameHistoryMenu
        {
            bool enabled = true;
            int budgetMB = 256;
            bool persistInSession = false;
            bool recording = false;   // armed right now (a debugger window is open)
            uint64_t frames = 0;      // frames currently held
            uint64_t reachFrames = 0; // frames the budget reaches at the measured rate
            bool thumbnails = true;   // capture periodic screens for the scrub preview
            int thumbnailInterval = 10;
            size_t thumbnailCount = 0; // previews held
            size_t thumbnailBytes = 0; // what they cost after delta compression
        };
        FrameHistoryMenu frameHistory;
        std::function<void(bool enabled, int budgetMB, bool persistInSession, bool thumbnails,
                           int thumbnailInterval)>
            onFrameHistoryChanged;
        std::function<void()> onFrameHistoryClear;
        // Emulator -> "Log core output": lets PUAE's own log through to the Log window.
        bool coreVerboseLog = false;
        std::function<void(bool)> onCoreVerboseLogChanged;
        // Emulator -> Screenshots. Both act on the frame currently on screen; enabled only
        // while the emulator is running.
        std::function<void()> onScreenshotToClipboard;
        std::function<void()> onSaveScreenshot;
        StepInstructionCallback onStepInstruction;
        bool isPaused = false;
        bool isTurbo = false;
        bool isEmulatorRunning = false;
        // True while the emulator owns the mouse/keyboard (EmulatorView::IsMouseCaptured()).
        // Suppresses every menu-bar keyboard shortcut below so captured keys (F3/F5/F6/F7/etc.)
        // reach the Amiga instead of the host UI; only F12 (handled by EmulatorView itself)
        // releases capture.
        bool emulatorMouseCaptured = false;
        std::string currentModelId = "A500";
        // Empty when a preset can boot, otherwise why not (a missing CD32/CDTV ROM).
        std::function<std::string(const std::string& modelId)> modelUnavailableReason;
        std::function<void(const std::string&)> onConfigSelected;
        std::function<void(const std::string&)> onUaeConfigSelected;
        std::string currentUaeConfigName;
        std::vector<EmulatorConfigurationInfo> customConfigurations;
        std::string currentCustomConfigurationName;
        std::function<bool(const std::string&, std::string&)> onSaveCustomConfiguration;
        std::function<bool(const std::string&, std::string&)> onUpdateCustomConfiguration;
        std::function<bool(const std::string&, std::string&)> onLoadCustomConfiguration;
        std::function<bool(const std::string&, std::string&)> onDeleteCustomConfiguration;
        std::function<bool(const std::string&, const std::string&, std::string&)>
            onRenameCustomConfiguration;
        bool keyboardJoystickEnabled = false;
        std::function<void(bool)> onKeyboardJoystickToggled;
        ShowPreferencesCallback onShowPreferences;
        // Interface text size: +1 larger, -1 smaller, 0 back to 100%.
        std::function<void(int)> onTextSize;
        std::function<void()> onShowAppearance;
        // Help > Setup Wizard: finding the user's own ROMs and Workbench
        std::function<void()> onShowSetupWizard;

        // View menu
        WorkspaceMenuState workspaces;
        std::vector<DiskDocumentInfo> documents;
        SelectDocumentCallback onSelectDocument;
    };

    /**
     * @brief Draw the main menu bar and any pending dialogs.
     * @param ctx All callbacks and state the menu bar needs this frame.
     * @return true if the application should exit.
     */
    static bool Draw(const DrawContext& ctx);

private:
    // Static state for the New Disk dialog
    static bool s_showNewDiskDialog;
    static int s_newDiskTemplateIdx;
    static char s_newDiskLabel[64];
    static bool s_newDiskBootable;
    static int s_newDiskBootBlockIdx;
    static bool s_newDiskEmptyStartup;

    // Static state for the Format Disk dialog
    static bool s_showFormatDiskDialog;
    static char s_formatDiskLabel[64];
    static bool s_formatDiskUseOFS;
    static std::string s_formatDiskTarget;

    // Static state for file-not-found dialog
    static bool s_showFileNotFound;
    static std::string s_fileNotFoundPath;
    static bool s_notFoundPathIsDirectory;

    // Static state for workspace dialogs
    static bool s_showSaveWorkspaceDialog;
    static char s_workspaceNameBuf[128];

    // Static state for custom emulator configuration dialogs
    static bool s_showSaveEmulatorConfigurationDialog;
    static bool s_showRenameEmulatorConfigurationDialog;
    static bool s_showUpdateEmulatorConfigurationDialog;
    static bool s_showEmulatorConfigurationErrorDialog;
    static char s_emulatorConfigurationNameBuf[128];
    static char s_renameEmulatorConfigurationNameBuf[128];
    static std::string s_renameEmulatorConfigurationPath;
    static std::string s_updateEmulatorConfigurationPath;
    static std::string s_updateEmulatorConfigurationName;
    static std::string s_emulatorConfigurationError;

    // Static state for workspace switcher
    static bool s_workspaceSwitcherOpen;
    static int s_workspaceSwitcherIndex;
};

} // namespace ui
} // namespace adm
