#include "MainLayout.h"
#include "../core/AmigaEmulator.h"
#include "../core/PrivateFeatures.h"
#include "IconsFontAwesome6.h"
#include "MenuIcons.h"
#include "TableLayout.h"

#include <fmt/core.h>
#include <imgui.h>
#include <nfd.h>
#include <algorithm>
#include <filesystem>

namespace adm {
namespace ui {

// Initialize static members
bool MainLayout::s_showFileNotFound = false;
std::string MainLayout::s_fileNotFoundPath;
bool MainLayout::s_notFoundPathIsDirectory = false;
bool MainLayout::s_showSaveWorkspaceDialog = false;
char MainLayout::s_workspaceNameBuf[128] = {};
bool MainLayout::s_showSaveEmulatorConfigurationDialog = false;
bool MainLayout::s_showRenameEmulatorConfigurationDialog = false;
bool MainLayout::s_showUpdateEmulatorConfigurationDialog = false;
bool MainLayout::s_showEmulatorConfigurationErrorDialog = false;
char MainLayout::s_emulatorConfigurationNameBuf[128] = {};
char MainLayout::s_renameEmulatorConfigurationNameBuf[128] = {};
std::string MainLayout::s_renameEmulatorConfigurationPath;
std::string MainLayout::s_updateEmulatorConfigurationPath;
std::string MainLayout::s_updateEmulatorConfigurationName;
std::string MainLayout::s_emulatorConfigurationError;
bool MainLayout::s_workspaceSwitcherOpen = false;
int MainLayout::s_workspaceSwitcherIndex = -1;
bool MainLayout::s_showNewDiskDialog = false;
int MainLayout::s_newDiskTemplateIdx = 0;
char MainLayout::s_newDiskLabel[64] = "Empty";
bool MainLayout::s_newDiskBootable = false;
int MainLayout::s_newDiskBootBlockIdx = 0;
bool MainLayout::s_newDiskEmptyStartup = false;
bool MainLayout::s_showFormatDiskDialog = false;
char MainLayout::s_formatDiskLabel[64] = "Empty";
bool MainLayout::s_formatDiskUseOFS = true;
std::string MainLayout::s_formatDiskTarget;

// Draws a menu row that toggles a registered window. The Disk and Debug menus list their windows
// in a deliberate order rather than iterating the registry, so they address each one by name.
//
// `label` overrides the registry name for display only - the name is the key workspaces save
// open-state under and the MCP UI tools address, so it never changes. `shortcut` is hint text.
// A name that is not registered draws nothing: MCP Activity only exists when the MCP server does.
static void WindowToggle(const std::vector<WindowState>& windows, const char* name,
                         const char* label = nullptr, const char* shortcut = nullptr)
{
    for (const auto& w : windows) {
        if (w.name != name)
            continue;
        MenuItemIcon(w.icon.c_str(), label ? label : w.name.c_str(), shortcut, w.openPtr);
        return;
    }
}

bool MainLayout::Draw(const DrawContext& ctx)
{
    // Alias the context fields so the menu-building body below reads naturally. (The previous
    // signature spelled these out as ~50 separate parameters.)
    const auto& onOpenFile = ctx.onOpenFile;
    const auto& onMountHostDirectory = ctx.onMountHostDirectory;
    const auto& recentFiles = ctx.recentFiles;
    const auto& recentDirectories = ctx.recentDirectories;
    const auto& availableFonts = ctx.availableFonts;
    const auto& currentFont = ctx.currentFont;
    const auto& onFontSelected = ctx.onFontSelected;
    const auto& onSave = ctx.onSave;
    const auto& onSaveAs = ctx.onSaveAs;
    const auto& onClose = ctx.onClose;
    const auto& onFind = ctx.onFind;
    const auto& onAdvancedFind = ctx.onAdvancedFind;
    const auto& onFindNext = ctx.onFindNext;
    const auto& onFindPrev = ctx.onFindPrev;
    const bool hasUnsavedChanges = ctx.hasUnsavedChanges;
    const bool hasFile = ctx.hasFile;
    const bool hasDiskImage = ctx.hasDiskImage;
    const bool canRun = ctx.canRun;
    const auto& diskTemplates = ctx.diskTemplates;
    const auto& onNewDisk = ctx.onNewDisk;
    const auto& onFormatDisk = ctx.onFormatDisk;
    const auto& onShowBootBlockView = ctx.onShowBootBlockView;
    const auto& onShowAbout = ctx.onShowAbout;
    const auto& editState = ctx.editState;
    const auto& windows = ctx.windows;
    const auto& connectedControllers = ctx.connectedControllers;
    const auto& onControllerSelected = ctx.onControllerSelected;
    const auto& onWriteDisk = ctx.onWriteDisk;
    const auto& onReadFloppy = ctx.onReadFloppy;
    const auto& onFormatFloppy = ctx.onFormatFloppy;
    const auto& onCompareFloppy = ctx.onCompareFloppy;
    const auto& onGwInfo = ctx.onGwInfo;
    const auto& onGwBandwidth = ctx.onGwBandwidth;
    const auto& onGwRPM = ctx.onGwRPM;
    const auto& onRunAdf = ctx.onRunAdf;
    const auto& onTogglePause = ctx.onTogglePause;
    const auto& onToggleTurbo = ctx.onToggleTurbo;
    const auto& onStopEmulator = ctx.onStopEmulator;
    const auto& onResetEmulator = ctx.onResetEmulator;
    const auto& onEjectEmulatorDisk = ctx.onEjectEmulatorDisk;
    const auto& onLaunchWhdload = ctx.onLaunchWhdload;
    const auto& onStepOver = ctx.onStepOver;
    const auto& onStepFrame = ctx.onStepFrame;
    const auto& onStepFrameBack = ctx.onStepFrameBack;
    const auto& frameHistory = ctx.frameHistory;
    const auto& onFrameHistoryChanged = ctx.onFrameHistoryChanged;
    const auto& onFrameHistoryClear = ctx.onFrameHistoryClear;
    const auto& onCoreVerboseLogChanged = ctx.onCoreVerboseLogChanged;
    const auto& onScreenshotToClipboard = ctx.onScreenshotToClipboard;
    const auto& onSaveScreenshot = ctx.onSaveScreenshot;
    const auto& onStepInstruction = ctx.onStepInstruction;
    const bool isPaused = ctx.isPaused;
    const bool isTurbo = ctx.isTurbo;
    const bool isEmulatorRunning = ctx.isEmulatorRunning;
    const bool emulatorMouseCaptured = ctx.emulatorMouseCaptured;
    const auto& currentModelId = ctx.currentModelId;
    const auto& onConfigSelected = ctx.onConfigSelected;
    const auto& onCloseAll = ctx.onCloseAll;
    const auto& onRequestExit = ctx.onRequestExit;
    bool* const showAdvancedFind = ctx.showAdvancedFind;
    const auto& onUaeConfigSelected = ctx.onUaeConfigSelected;
    const auto& currentUaeConfigName = ctx.currentUaeConfigName;
    const auto& customConfigurations = ctx.customConfigurations;
    const auto& currentCustomConfigurationName = ctx.currentCustomConfigurationName;
    const auto& onSaveCustomConfiguration = ctx.onSaveCustomConfiguration;
    const auto& onUpdateCustomConfiguration = ctx.onUpdateCustomConfiguration;
    const auto& onLoadCustomConfiguration = ctx.onLoadCustomConfiguration;
    const auto& onDeleteCustomConfiguration = ctx.onDeleteCustomConfiguration;
    const auto& onRenameCustomConfiguration = ctx.onRenameCustomConfiguration;
    const auto& workspaceState = ctx.workspaces;
    const auto& documents = ctx.documents;
    const auto& onSelectDocument = ctx.onSelectDocument;
    const auto& onShowPreferences = ctx.onShowPreferences;
    const auto& onTextSize = ctx.onTextSize;
    const auto& onShowAppearance = ctx.onShowAppearance;
    const auto& panelState = ctx.panelState;
    const auto& bootBlockNames = ctx.bootBlockNames;
    const auto& onNewDiskAdvanced = ctx.onNewDiskAdvanced;
    const bool keyboardJoystickEnabled = ctx.keyboardJoystickEnabled;
    const auto& onKeyboardJoystickToggled = ctx.onKeyboardJoystickToggled;
    const auto& onSaveSession = ctx.onSaveSession;
    const auto& onOpenSession = ctx.onOpenSession;
    const auto& onSaveSessionAs = ctx.onSaveSessionAs;

    bool openDialog = false;
    bool shouldExit = false;

    // Shortcut handling helpers

    auto& io = ImGui::GetIO();
    // Define primary modifier (Ctrl on Win/Linux, Cmd on Mac)
#ifdef __APPLE__
    bool isModPressed = io.KeySuper;
    // Strict check: Ensure physical Ctrl is NOT pressed (unless we wanted valid Ctrl+Cmd
    // combos, which we don't for these)
    bool otherModPressed = io.KeyCtrl;
#else
    bool isModPressed = io.KeyCtrl;
    // Strict check: Ensure Super is NOT pressed
    bool otherModPressed = io.KeySuper;
#endif

    auto IsShortcutPressed = [&](ImGuiKey key, bool wantMod = true, bool wantShift = false,
                                 bool wantAlt = false) {
        // While the emulator has captured the mouse/keyboard, every key (other than F12,
        // handled by EmulatorView to release capture) must reach the Amiga, not the host UI.
        if (emulatorMouseCaptured)
            return false;
        if (s_workspaceSwitcherOpen)
            return false;
        if (isModPressed != wantMod)
            return false;
        if (otherModPressed)
            return false;
        if (io.KeyShift != wantShift)
            return false;
        if (io.KeyAlt != wantAlt)
            return false;

        return ImGui::IsKeyPressed(key, false);
    };

    // Global actions (handled before Menu to allow shortcuts to work even if menu closed)
    // Only allow search shortcuts if a file is loaded
    bool triggerOpen = IsShortcutPressed(ImGuiKey_O, true, false, false);
    bool triggerSave = IsShortcutPressed(ImGuiKey_S, true, false, false);
    bool triggerFind = hasDiskImage && IsShortcutPressed(ImGuiKey_F, true, false, false);
    bool triggerAdvancedFind = hasDiskImage && IsShortcutPressed(ImGuiKey_F, true, true, false);
    bool triggerFindNext = hasDiskImage && IsShortcutPressed(ImGuiKey_F3, false, false, false);
    bool triggerFindPrev = hasDiskImage && IsShortcutPressed(ImGuiKey_F3, false, true, false);
    bool triggerTogglePause =
        isEmulatorRunning && IsShortcutPressed(ImGuiKey_F5, false, false, false);
    bool triggerToggleTurbo =
        isEmulatorRunning && IsShortcutPressed(ImGuiKey_F5, false, true, false); // Shift+F5
    bool triggerStepFrame =
        isEmulatorRunning && IsShortcutPressed(ImGuiKey_F6, false, false, false); // F6
    bool triggerStepFrameBack =
        isEmulatorRunning && IsShortcutPressed(ImGuiKey_F6, false, true, false); // Shift+F6
    bool triggerStepInstruction =
        isEmulatorRunning && IsShortcutPressed(ImGuiKey_F7, false, false, false); // F7
    bool triggerExit =
        IsShortcutPressed(ImGuiKey_F4, false, false, true); // Alt+F4 (keep Alt explicit)
    // Edit-menu shortcuts. F2 is a function key and needs no extra guard, but a bare Delete is a
    // real typing key: it is consumed by the AR console input, the vendored text editor and the
    // boot block view, and is forwarded to the Amiga keyboard. IsShortcutPressed already bails
    // while the emulator has captured input; WantTextInput covers every other focused field.
    bool triggerRename = editState.canRename && IsShortcutPressed(ImGuiKey_F2, false, false, false);
    bool triggerDelete = editState.canDelete && !io.WantTextInput &&
                         IsShortcutPressed(ImGuiKey_Delete, false, false, false);
    // Interface zoom. Shift is allowed on Ctrl+= because '+' is Shift+= on most layouts.
    bool triggerTextLarger = IsShortcutPressed(ImGuiKey_Equal, true, false, false) ||
                             IsShortcutPressed(ImGuiKey_Equal, true, true, false) ||
                             IsShortcutPressed(ImGuiKey_KeypadAdd, true, false, false);
    bool triggerTextSmaller = IsShortcutPressed(ImGuiKey_Minus, true, false, false) ||
                              IsShortcutPressed(ImGuiKey_KeypadSubtract, true, false, false);
    bool triggerTextReset = IsShortcutPressed(ImGuiKey_0, true, false, false) ||
                            IsShortcutPressed(ImGuiKey_Keypad0, true, false, false);

    // Workspace Switcher Logic (Ctrl+W) - suppressed while the emulator owns the keyboard.
    if (!workspaceState.names.empty() && !emulatorMouseCaptured) {
        bool wPressed = ImGui::IsKeyPressed(ImGuiKey_W, false);
        bool shiftPressed = io.KeyShift;

        const int wsCount = (int)workspaceState.names.size();
        auto cycleSwitcher = [&](bool reverse) {
            s_workspaceSwitcherIndex += reverse ? -1 : 1;
            if (s_workspaceSwitcherIndex < 0)
                s_workspaceSwitcherIndex = wsCount - 1;
            else if (s_workspaceSwitcherIndex >= wsCount)
                s_workspaceSwitcherIndex = 0;
        };

        if (!s_workspaceSwitcherOpen) {
            if (wPressed && isModPressed && !otherModPressed && !io.KeyAlt) {
                s_workspaceSwitcherOpen = true;
                auto it = std::find(workspaceState.names.begin(), workspaceState.names.end(),
                                    workspaceState.selectedWorkspace);
                if (it != workspaceState.names.end())
                    s_workspaceSwitcherIndex = (int)std::distance(workspaceState.names.begin(), it);
                else
                    s_workspaceSwitcherIndex = 0;

                // Move immediately
                cycleSwitcher(shiftPressed);
            }
        } else {
            // Switcher is open
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                s_workspaceSwitcherOpen = false;
            }

            if (s_workspaceSwitcherOpen && wPressed) {
                cycleSwitcher(shiftPressed);
            }

            if (s_workspaceSwitcherOpen && !isModPressed) {
                // Confirm selection
                if (workspaceState.onLoad && s_workspaceSwitcherIndex >= 0 &&
                    s_workspaceSwitcherIndex < (int)workspaceState.names.size()) {
                    workspaceState.onLoad(workspaceState.names[s_workspaceSwitcherIndex]);
                }
                s_workspaceSwitcherOpen = false;
            }
        }
    }

    // Main Menu Bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (MenuItemIcon(ICON_FA_FOLDER_OPEN, "Open Disk Image...", "Ctrl+O") || triggerOpen) {
                openDialog = true;
                triggerOpen = false; // Consumed
            }

            if (BeginMenuIcon(ICON_FA_CLOCK, "Open Recents")) {
                if (recentFiles.empty()) {
                    ImGui::MenuItem("No recent files", nullptr, false, false);
                } else {
                    for (const auto& file : recentFiles) {
                        if (ImGui::MenuItem(file.c_str())) {
                            if (!std::filesystem::exists(file)) {
                                s_showFileNotFound = true;
                                s_fileNotFoundPath = file;
                                s_notFoundPathIsDirectory = false;
                            } else {
                                if (onOpenFile)
                                    onOpenFile(file);
                            }
                        }
                    }
                }
                ImGui::EndMenu();
            }

            if (BeginMenuIcon(ICON_FA_FOLDER_OPEN, "Mount Host Directory")) {
                for (int device = 0; device < 2; ++device) {
                    const std::string deviceName = fmt::format("DH{}", device);
                    if (!ImGui::BeginMenu(deviceName.c_str()))
                        continue;
                    if (MenuItemIcon(ICON_FA_FOLDER_OPEN, "Choose Directory...") &&
                        onMountHostDirectory)
                        onMountHostDirectory(device, {});
                    ImGui::Separator();
                    if (BeginMenuIcon(ICON_FA_CLOCK, "Recents")) {
                        if (recentDirectories.empty()) {
                            ImGui::MenuItem("No recent directories", nullptr, false, false);
                        } else {
                            for (const auto& directory : recentDirectories) {
                                if (!ImGui::MenuItem(directory.c_str()))
                                    continue;
                                if (!std::filesystem::is_directory(directory)) {
                                    s_showFileNotFound = true;
                                    s_fileNotFoundPath = directory;
                                    s_notFoundPathIsDirectory = true;
                                } else if (onMountHostDirectory) {
                                    onMountHostDirectory(device, directory);
                                }
                            }
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            // One entry, not two: picking a template creates the disk straight away, and
            // Custom... opens the dialog that does the same thing plus label, bootable and
            // boot-block choices. They used to sit adjacent under near-identical names.
            if (BeginMenuIcon(ICON_FA_CIRCLE_PLUS, "New Disk")) {
                if (diskTemplates.empty()) {
                    ImGui::MenuItem("No templates found", nullptr, false, false);
                } else {
                    for (const auto& templateName : diskTemplates) {
                        if (ImGui::MenuItem(templateName.c_str())) {
                            if (onNewDisk) {
                                onNewDisk(templateName);
                            }
                        }
                    }
                }
                ImGui::Separator();
                if (MenuItemIcon(ICON_FA_GEARS, "Custom...")) {
                    s_showNewDiskDialog = true;
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            // Save menu items
            if (MenuItemIcon(ICON_FA_FLOPPY_DISK, "Save Disk Image", "Ctrl+S", false,
                             hasUnsavedChanges) ||
                triggerSave) {
                // Gate on hasUnsavedChanges so the Ctrl+S shortcut behaves the same
                // whether the menu is open or closed (the closed-menu path below also
                // requires unsaved changes).
                if (onSave && hasUnsavedChanges)
                    onSave();
                triggerSave = false;
            }
            if (MenuItemIcon(ICON_FA_FILE_EXPORT, "Save Disk Image As...", nullptr, false,
                             hasDiskImage)) {
                if (onSaveAs)
                    onSaveAs();
            }

            ImGui::Separator();

            if (MenuItemIcon(ICON_FA_FOLDER_OPEN, "Open Session...")) {
                if (onOpenSession)
                    onOpenSession();
            }
            if (MenuItemIcon(ICON_FA_FLOPPY_DISK, "Save Session", nullptr, false, hasFile)) {
                if (onSaveSession)
                    onSaveSession();
            }
            if (MenuItemIcon(ICON_FA_FILE_EXPORT, "Save Session As...", nullptr, false, hasFile)) {
                if (onSaveSessionAs)
                    onSaveSessionAs();
            }

            ImGui::Separator();

            if (MenuItemIcon(ICON_FA_XMARK, "Close Document", nullptr, false, hasFile)) {
                if (onClose)
                    onClose();
            }

            if (MenuItemIcon(ICON_FA_XMARK, "Close All Documents", nullptr, false, hasFile)) {
                if (onCloseAll)
                    onCloseAll();
            }

            ImGui::Separator();

            if (MenuItemIcon(ICON_FA_POWER_OFF, "Exit", "Alt+F4") || triggerExit) {
                if (hasUnsavedChanges) {
                    if (onRequestExit)
                        onRequestExit();
                } else {
                    shouldExit = true;
                }
                triggerExit = false;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            // Two bands: what creates or changes an entry, then what destroys or recovers one.
            if (MenuItemIcon(ICON_FA_FOLDER_PLUS, "New Folder", nullptr, false,
                             editState.canNewFolder)) {
                if (editState.onNewFolder)
                    editState.onNewFolder();
            }

            if (MenuItemIcon(ICON_FA_PEN, "Rename", "F2", false, editState.canRename) ||
                triggerRename) {
                if (editState.onRename)
                    editState.onRename();
                triggerRename = false;
            }

            if (MenuItemIcon(ICON_FA_LOCK, "Permissions", nullptr, false,
                             editState.canPermissions)) {
                if (editState.onPermissions)
                    editState.onPermissions();
            }

            ImGui::Separator();

            if (MenuItemIcon(ICON_FA_TRASH, "Delete", "Del", false, editState.canDelete) ||
                triggerDelete) {
                if (editState.onDelete)
                    editState.onDelete();
                triggerDelete = false;
            }

            if (MenuItemIcon(ICON_FA_ERASER, "Scrub", nullptr, false, editState.canScrub)) {
                if (editState.onScrub)
                    editState.onScrub();
            }

            if (MenuItemIcon(ICON_FA_ROTATE_LEFT, "Restore", nullptr, false,
                             editState.canRestore)) {
                if (editState.onRestore)
                    editState.onRestore();
            }

            ImGui::Separator();

            if (MenuItemIcon(ICON_FA_FILE_EXPORT, "Export...", nullptr, false,
                             editState.canExport)) {
                if (editState.onExport)
                    editState.onExport();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Search")) {
            if (MenuItemIcon(ICON_FA_MAGNIFYING_GLASS, "Find...", "Ctrl+F", false, hasDiskImage) ||
                triggerFind) {
                if (onFind)
                    onFind();
                triggerFind = false;
            }
            if (MenuItemIcon(ICON_FA_CHEVRON_RIGHT, "Find Next", "F3", false, hasDiskImage) ||
                triggerFindNext) {
                if (onFindNext)
                    onFindNext();
                triggerFindNext = false;
            }
            if (MenuItemIcon(ICON_FA_CHEVRON_LEFT, "Find Previous", "Shift+F3", false,
                             hasDiskImage) ||
                triggerFindPrev) {
                if (onFindPrev)
                    onFindPrev();
                triggerFindPrev = false;
            }
            ImGui::Separator();
            bool isAdvancedFindOpen = showAdvancedFind ? *showAdvancedFind : false;
            if (MenuItemIcon(ICON_FA_MAGNIFYING_GLASS, "Advanced Find...", "Ctrl+Shift+F",
                             isAdvancedFindOpen, hasDiskImage)) {
                if (showAdvancedFind) {
                    *showAdvancedFind = !*showAdvancedFind;
                    if (*showAdvancedFind && onAdvancedFind) {
                        // We still call the callback to let Application know,
                        // effectively `onAdvancedFind` handles SetFocus now?
                        // No, I moved the logic here in MainLayout.
                        // But MainLayout doesn't have access to m_advancedFindView directly to call
                        // SetFocus. So I should call the callback, and the callback in
                        // Application.cpp should handle SetFocus if it's being opened.
                        onAdvancedFind();
                    }
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Emulator")) {
            if (MenuItemIcon(ICON_FA_GEARS, "Preferences...")) {
                if (onShowPreferences)
                    onShowPreferences();
            }
            ImGui::Separator();
            if (MenuItemIcon(ICON_FA_PLAY, "Run", nullptr, false, canRun)) {
                if (onRunAdf)
                    onRunAdf();
            }
            if (MenuItemIcon(isPaused ? ICON_FA_PLAY : ICON_FA_PAUSE, isPaused ? "Resume" : "Pause",
                             "F5", false, isEmulatorRunning) ||
                triggerTogglePause) {
                if (onTogglePause)
                    onTogglePause();
                triggerTogglePause = false;
            }
            if (MenuItemIcon(ICON_FA_STOP, "Stop", nullptr, false, isEmulatorRunning)) {
                if (onStopEmulator)
                    onStopEmulator();
            }
            if (MenuItemIcon(ICON_FA_ARROW_ROTATE_RIGHT, "Reset", nullptr, false,
                             isEmulatorRunning)) {
                if (onResetEmulator)
                    onResetEmulator();
            }
            if (MenuItemIcon(ICON_FA_EJECT, "Eject Disk", nullptr, false, isEmulatorRunning)) {
                if (onEjectEmulatorDisk)
                    onEjectEmulatorDisk();
            }
            if (MenuItemIcon(ICON_FA_BOX_ARCHIVE, "Launch WHDLoad Archive...")) {
                if (onLaunchWhdload)
                    onLaunchWhdload();
            }
            ImGui::SetItemTooltip("Boot an .lha/.lzx WHDLoad install on DH0 with the core's own "
                                  "WHDLoad. Opening or dropping one does the same.");
            {
                const bool canCopy = ctx.copyFileToGuestUnavailableReason.empty();
                if (MenuItemIcon(ICON_FA_UPLOAD, "Copy File to Guest...", nullptr, false,
                                 canCopy)) {
                    if (ctx.onCopyFileToGuest)
                        ctx.onCopyFileToGuest();
                }
                ImGui::SetItemTooltip(
                    "%s", canCopy ? "Copy a file from this machine onto a mounted DH: directory. "
                                    "AmigaDOS sees it straight away."
                                  : ctx.copyFileToGuestUnavailableReason.c_str());
            }

            ImGui::Separator();

            if (MenuItemIcon(ICON_FA_FORWARD_FAST, "Turbo", "Shift+F5", isTurbo,
                             isEmulatorRunning) ||
                triggerToggleTurbo) {
                if (onToggleTurbo)
                    onToggleTurbo();
                triggerToggleTurbo = false;
            }
            if (MenuItemIcon(ICON_FA_FORWARD_STEP, "Step Frame", "F6", false, isEmulatorRunning) ||
                triggerStepFrame) {
                if (onStepFrame)
                    onStepFrame();
                triggerStepFrame = false;
            }
            if (MenuItemIcon(ICON_FA_BACKWARD_STEP, "Step Frame Back", "Shift+F6", false,
                             isEmulatorRunning) ||
                triggerStepFrameBack) {
                if (onStepFrameBack)
                    onStepFrameBack();
                triggerStepFrameBack = false;
            }
            if (MenuItemIcon(ICON_FA_ANGLE_RIGHT, "Step Instruction", "F7", false,
                             isEmulatorRunning) ||
                triggerStepInstruction) {
                if (onStepInstruction)
                    onStepInstruction();
                triggerStepInstruction = false;
            }
            if (MenuItemIcon(ICON_FA_ANGLES_RIGHT, "Step Over", nullptr, false,
                             isEmulatorRunning)) {
                if (onStepOver)
                    onStepOver();
            }

            // Sits with the step controls because it is what makes Step Frame Back possible.
            if (BeginMenuIcon(ICON_FA_ROTATE_LEFT, "Frame History")) {
                bool enabled = frameHistory.enabled;
                int budgetMB = frameHistory.budgetMB;
                bool persist = frameHistory.persistInSession;
                bool thumbs = frameHistory.thumbnails;
                int thumbInterval = frameHistory.thumbnailInterval;
                bool changed = false;

                if (ImGui::MenuItem("Record frames", nullptr, &enabled))
                    changed = true;
                ImGui::SetItemTooltip("Keeps a save state per frame so you can step backwards.\n"
                                      "Off by default; also on the Frame Scrubber's record "
                                      "button.\n"
                                      "Only records while a debugger window is open.");

                if (ImGui::BeginMenu("Memory budget")) {
                    for (const int mb : {64, 128, 256, 512, 1024, 2048, 4096}) {
                        char label[24];
                        if (mb >= 2048)
                            snprintf(label, sizeof(label), "%d GB", mb / 1024);
                        else
                            snprintf(label, sizeof(label), "%d MB", mb);
                        if (ImGui::MenuItem(label, nullptr, budgetMB == mb)) {
                            budgetMB = mb;
                            changed = true;
                        }
                    }
                    ImGui::EndMenu();
                }

                if (ImGui::MenuItem("Scrub previews", nullptr, &thumbs))
                    changed = true;
                ImGui::SetItemTooltip(
                    "Grabs a small screen image every few frames so dragging the frame\n"
                    "seek bar shows the picture at that frame, not just a number.");

                if (ImGui::BeginMenu("Preview every", thumbs)) {
                    for (const int n : {1, 5, 10, 25, 50}) {
                        char label[32];
                        snprintf(label, sizeof(label), n == 1 ? "%d frame" : "%d frames", n);
                        if (ImGui::MenuItem(label, nullptr, thumbInterval == n)) {
                            thumbInterval = n;
                            changed = true;
                        }
                    }
                    ImGui::EndMenu();
                }

                if (ImGui::MenuItem("Clear timeline", nullptr, false, frameHistory.frames > 0)) {
                    if (onFrameHistoryClear)
                        onFrameHistoryClear();
                }
                ImGui::SetItemTooltip("Throws away every recorded frame.\nRecording carries "
                                      "on from the next frame.");

                if (ImGui::MenuItem("Save timeline in session", nullptr, &persist))
                    changed = true;
                ImGui::SetItemTooltip("Writes the whole recorded timeline into the .peek file.\n"
                                      "Off by default - a full timeline is hundreds of MB.");

                ImGui::Separator();
                if (!frameHistory.recording) {
                    ImGui::TextDisabled(enabled ? "Idle - open a debugger window" : "Off");
                } else {
                    ImGui::TextDisabled("Holding %llu frames (~%.0fs)",
                                        static_cast<unsigned long long>(frameHistory.frames),
                                        static_cast<double>(frameHistory.frames) / 50.0);
                    ImGui::TextDisabled("Budget reaches ~%.0fs",
                                        static_cast<double>(frameHistory.reachFrames) / 50.0);
                    if (thumbs) {
                        ImGui::TextDisabled("%zu previews (%.1f MB)", frameHistory.thumbnailCount,
                                            static_cast<double>(frameHistory.thumbnailBytes) /
                                                (1024.0 * 1024.0));
                    }
                }

                if (changed && onFrameHistoryChanged)
                    onFrameHistoryChanged(enabled, budgetMB, persist, thumbs, thumbInterval);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (BeginMenuIcon(ICON_FA_GEARS, "Configurations")) {
                const auto& models = adm::GetAllAmigaModels();
                for (const auto& info : models) {
                    bool isSelected = currentUaeConfigName.empty() &&
                                      currentCustomConfigurationName.empty() &&
                                      (std::string(info.id) == currentModelId);
                    const std::string unavailable = ctx.modelUnavailableReason
                                                        ? ctx.modelUnavailableReason(info.id)
                                                        : std::string();
                    ImGui::BeginDisabled(!unavailable.empty());
                    if (ImGui::MenuItem(info.displayName, nullptr, isSelected)) {
                        if (onConfigSelected)
                            onConfigSelected(info.id);
                    }
                    ImGui::EndDisabled();
                    if (!unavailable.empty() &&
                        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Unavailable: %s.\nSet the ROM directory in Emulator > "
                                          "Preferences > Kickstart ROM.",
                                          unavailable.c_str());
                }

                // User .uae config files from ./uae/configs/ — rescanned each time the
                // submenu popup appears so configs added at runtime show up.
                static std::vector<std::filesystem::path> uaeConfigs;
                if (ImGui::IsWindowAppearing()) {
                    uaeConfigs.clear();
                    std::error_code ec;
                    std::filesystem::path configDir = "./uae/configs";
                    if (std::filesystem::exists(configDir, ec)) {
                        for (auto& entry : std::filesystem::directory_iterator(configDir, ec)) {
                            if (entry.is_regular_file() && entry.path().extension() == ".uae")
                                uaeConfigs.push_back(entry.path());
                        }
                        std::sort(uaeConfigs.begin(), uaeConfigs.end());
                    }
                }

                if (!uaeConfigs.empty()) {
                    ImGui::SeparatorText("User Configs");
                    for (auto& p : uaeConfigs) {
                        std::string name = p.stem().string();
                        bool isSelected = (name == currentUaeConfigName);
                        if (ImGui::MenuItem(name.c_str(), nullptr, isSelected)) {
                            if (onUaeConfigSelected)
                                onUaeConfigSelected(p.string());
                        }
                    }
                }

                ImGui::Separator();
                if (ImGui::BeginMenu("Custom")) {
                    if (MenuItemIcon(ICON_FA_FLOPPY_DISK, "Save Current As...")) {
                        s_emulatorConfigurationNameBuf[0] = '\0';
                        s_emulatorConfigurationError.clear();
                        s_showSaveEmulatorConfigurationDialog = true;
                    }
                    if (!customConfigurations.empty()) {
                        ImGui::Separator();
                        for (const auto& configuration : customConfigurations) {
                            const bool selected =
                                configuration.name == currentCustomConfigurationName;
                            const std::string label = (selected ? ICON_FA_CHECK "  " : "    ") +
                                                      configuration.name + "##emu_config_" +
                                                      configuration.path;
                            if (ImGui::BeginMenu(label.c_str())) {
                                if (MenuItemIcon(ICON_FA_FOLDER_OPEN, "Load")) {
                                    std::string error;
                                    if (onLoadCustomConfiguration &&
                                        !onLoadCustomConfiguration(configuration.path, error)) {
                                        s_emulatorConfigurationError = std::move(error);
                                        s_showEmulatorConfigurationErrorDialog = true;
                                    }
                                }
                                if (MenuItemIcon(ICON_FA_FLOPPY_DISK, "Update from Current")) {
                                    s_updateEmulatorConfigurationPath = configuration.path;
                                    s_updateEmulatorConfigurationName = configuration.name;
                                    s_emulatorConfigurationError.clear();
                                    s_showUpdateEmulatorConfigurationDialog = true;
                                }
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("Replace this configuration with the "
                                                      "machine setup that is running now.");
                                }
                                if (MenuItemIcon(ICON_FA_PEN, "Rename...")) {
                                    std::snprintf(s_renameEmulatorConfigurationNameBuf,
                                                  sizeof(s_renameEmulatorConfigurationNameBuf),
                                                  "%s", configuration.name.c_str());
                                    s_renameEmulatorConfigurationPath = configuration.path;
                                    s_emulatorConfigurationError.clear();
                                    s_showRenameEmulatorConfigurationDialog = true;
                                }
                                ImGui::Separator();
                                ImGui::BeginDisabled(selected);
                                if (MenuItemIcon(ICON_FA_TRASH, "Delete")) {
                                    std::string error;
                                    if (onDeleteCustomConfiguration &&
                                        !onDeleteCustomConfiguration(configuration.path, error)) {
                                        s_emulatorConfigurationError = std::move(error);
                                        s_showEmulatorConfigurationErrorDialog = true;
                                    }
                                }
                                if (selected &&
                                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                    ImGui::SetTooltip("Switch to another configuration before "
                                                      "deleting this one.");
                                ImGui::EndDisabled();
                                ImGui::EndMenu();
                            }
                            if (ImGui::IsItemClicked()) {
                                std::string error;
                                if (onLoadCustomConfiguration &&
                                    !onLoadCustomConfiguration(configuration.path, error)) {
                                    s_emulatorConfigurationError = std::move(error);
                                    s_showEmulatorConfigurationErrorDialog = true;
                                }
                            }
                        }
                    }
                    ImGui::EndMenu();
                }

                ImGui::EndMenu();
            }

            if (BeginMenuIcon(ICON_FA_GAMEPAD, "Controller")) {
                if (ImGui::MenuItem("Keyboard Joystick", "Arrows + A/S", keyboardJoystickEnabled)) {
                    if (onKeyboardJoystickToggled)
                        onKeyboardJoystickToggled(!keyboardJoystickEnabled);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Use the cursor keys as a joystick;\nA = fire button 1, S = fire button 2");
                }
                ImGui::Separator();

                if (connectedControllers.empty()) {
                    ImGui::MenuItem("No controllers detected", nullptr, false, false);
                } else {
                    for (const auto& controller : connectedControllers) {
                        if (ImGui::BeginMenu(controller.name.c_str())) {
                            bool isJoystick = (controller.type == "Standard Joystick");
                            bool isCD32 = (controller.type == "CD32 Controller");

                            if (ImGui::MenuItem("Standard Joystick", nullptr, isJoystick)) {
                                if (onControllerSelected)
                                    onControllerSelected(controller.name, "Standard Joystick");
                            }
                            if (ImGui::MenuItem("CD32 Controller", nullptr, isCD32)) {
                                if (onControllerSelected)
                                    onControllerSelected(controller.name, "CD32 Controller");
                            }
                            ImGui::EndMenu();
                        }
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (BeginMenuIcon(ICON_FA_MICROCHIP, "Performance")) {
                for (const auto& window : windows) {
                    if (window.groupName == "Performance") {
                        ImGui::MenuItem(window.name.c_str(), nullptr, window.openPtr);
                    }
                }
                ImGui::EndMenu();
            }

            for (const auto& window : windows) {
                if (window.groupName == "Filters") {
                    MenuItemIcon(ICON_FA_EYE, window.name.c_str(), nullptr, window.openPtr);
                }
            }
            if (BeginMenuIcon(ICON_FA_CAMERA, "Screenshots & Capture")) {
                if (MenuItemIcon(ICON_FA_COPY, "Screenshot to Clipboard", nullptr, false,
                                 isEmulatorRunning)) {
                    if (onScreenshotToClipboard)
                        onScreenshotToClipboard();
                }
                if (MenuItemIcon(ICON_FA_IMAGE, "Save Screenshot...", nullptr, false,
                                 isEmulatorRunning)) {
                    if (onSaveScreenshot)
                        onSaveScreenshot();
                }
                ImGui::Separator();
                // The GIF & Video Capture window is the only "Capture" group member; it lives here
                // rather than under View -> Windows, which skips the group for exactly this reason.
                for (const auto& window : windows) {
                    if (window.groupName == "Capture") {
                        MenuItemIcon(ICON_FA_FILM, window.name.c_str(), nullptr, window.openPtr);
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            {
                bool verbose = ctx.coreVerboseLog;
                if (MenuItemIcon(ICON_FA_FILE_LINES, "Log core output", nullptr, &verbose)) {
                    if (onCoreVerboseLogChanged)
                        onCoreVerboseLogChanged(verbose);
                }
                ImGui::SetItemTooltip(
                    "Sends PUAE's own log to the Log window - save state chunks, disk\n"
                    "events, chipset warnings. Off by default: it is several lines a\n"
                    "frame and it is written from the emulation thread.");
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Disk")) {
            // The Disk menu owns every hard- and floppy-disk window outright: they carry
            // groupName "Disk", which View skips, so this is their only menu home. (The
            // debugger's Disk I/O window is not one of these - it lives under Debug.)
            auto diskWindow = [&](const char* name, const char* label = nullptr) {
                WindowToggle(windows, name, label);
            };

            if (BeginMenuIcon(ICON_FA_FLOPPY_DISK, "Disk Images")) {
                if (documents.empty()) {
                    ImGui::MenuItem("No disk images open", nullptr, false, false);
                } else {
                    for (const auto& doc : documents) {
                        const char* icon = "   ";
                        if (doc.isLoadedInEmulator) {
                            icon = isPaused ? ICON_FA_PAUSE : ICON_FA_PLAY;
                        }

                        std::string label = fmt::format("{}  {}", icon, doc.name);
                        if (ImGui::MenuItem(label.c_str(), nullptr, doc.isActive)) {
                            if (onSelectDocument) {
                                onSelectDocument(doc.id);
                            }
                        }
                    }
                }
                ImGui::EndMenu();
            }

            // Reformats the active image in place. Destructive, so it opens a dialog rather
            // than acting on the click - and the dialog is where the label and the OFS/FFS
            // choice are made. Disabled, not hidden, when there is nothing it could format.
            if (MenuItemIcon(ICON_FA_ERASER, "Format Disk Image...", nullptr, false,
                             ctx.canFormatDisk)) {
                const auto info =
                    ctx.getFormatDiskInfo ? ctx.getFormatDiskInfo() : DrawContext::FormatDiskInfo{};
                s_formatDiskTarget = info.target;
                snprintf(s_formatDiskLabel, sizeof(s_formatDiskLabel), "%s",
                         info.volumeLabel.c_str());
                s_formatDiskUseOFS = info.isOFS;
                s_showFormatDiskDialog = true;
            }
            if (!ctx.canFormatDisk && ctx.formatDiskUnavailableReason &&
                ctx.formatDiskUnavailableReason[0] &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", ctx.formatDiskUnavailableReason);
            }

            ImGui::Separator();

            // What the emulated machine has attached, alongside the images open in the app.
            // Neither is an imaging tool - those live under Physical Disks further down.
            diskWindow("Floppy Drives");
            ImGui::SetItemTooltip(
                "The emulated machine's four floppy drives, DF0-DF3. Back a slot with a\n"
                "disk image, or with a real, spinning floppy over a Greaseweazle,\n"
                "DrawBridge, SuperCard Pro or XCopy board - several at once, one device\n"
                "per port. Live drives can be attached with the emulator stopped; they\n"
                "open on start.");
            diskWindow("Hard Drives");
            diskWindow("CD Drive");
            ImGui::SetItemTooltip(
                "CD0: which disc is in the drive, what the running machine has, and\n"
                "insert / eject / recent discs. A CD32 or CDTV has the drive built in;\n"
                "any other machine gets one when it powers on with a disc chosen.");

            ImGui::Separator();

            diskWindow("Block");
            diskWindow("Block Details");

            ImGui::Separator();

            // The four whole-disk maps, kept together and off the top level - they answer the
            // same question (what is where on this disk) and are browsed rather than acted on.
            if (BeginMenuIcon(ICON_FA_MAP, "Maps")) {
                diskWindow("Block Type Map");
                diskWindow("Disk Bitmap");
                diskWindow("Block Change Map");
                diskWindow("NDOS Map");
                ImGui::EndMenu();
            }

            ImGui::Separator();

            // One row each: the window is the tool, and it runs itself from its own Validate /
            // Identify button. Neither opening it nor the menu starts any work.
            diskWindow("Validation", "Disk Validator");
            diskWindow("Identify Disk Results", "Disk Identifier");
            // Always enabled — the viewer doubles as the boot block library browser, which is
            // useful without a disk loaded. An action rather than the window's toggle: it
            // re-reads blocks 0/1 off the active disk on the way in.
            if (MenuItemIcon(ICON_FA_MICROCHIP, "Boot Block Viewer")) {
                if (onShowBootBlockView)
                    onShowBootBlockView();
            }

            ImGui::Separator();

            // Everything here drives a real disk on a Greaseweazle-class USB board - imaging one
            // to a file, writing one back, and the board's own diagnostics. Nothing here touches
            // the emulated machine; that is Floppy Drives, at the top.
            if (BeginMenuIcon(ICON_FA_SCREWDRIVER_WRENCH, "Physical Disks")) {
                // Each opens the same Floppy Disk I/O window, pre-selecting the matching tab - the
                // Write tab itself disables the write action when there's nothing to write.
                if (MenuItemIcon(ICON_FA_FLOPPY_DISK, "Read from Disk...", nullptr, false)) {
                    if (onReadFloppy)
                        onReadFloppy();
                }
                if (MenuItemIcon(ICON_FA_UPLOAD, "Write to Disk...", nullptr, false)) {
                    if (onWriteDisk)
                        onWriteDisk();
                }
                // Blanks a real disk, which is a write of an empty filesystem - so it is a tab
                // of the same window, not a dialog of its own.
                if (MenuItemIcon(ICON_FA_ERASER, "Format Disk...", nullptr, false)) {
                    if (onFormatFloppy)
                        onFormatFloppy();
                }
                if (MenuItemIcon(ICON_FA_CODE_COMPARE, "Compare with Disk...", nullptr, false)) {
                    if (onCompareFloppy)
                        onCompareFloppy();
                }
                diskWindow("Floppy Disk I/O");

                ImGui::Separator();

                if (ImGui::BeginMenu("Greaseweazle")) {
                    if (ImGui::MenuItem("RPM")) {
                        if (onGwRPM)
                            onGwRPM();
                    }
                    if (ImGui::MenuItem("Info")) {
                        if (onGwInfo)
                            onGwInfo();
                    }
                    if (ImGui::MenuItem("Bandwidth")) {
                        if (onGwBandwidth)
                            onGwBandwidth();
                    }
                    ImGui::EndMenu();
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();

            diskWindow("TOSEC Browser");
            ImGui::EndMenu();
        }

        // Every window in the "Debugger" registry group, which View skips. Listed explicitly
        // rather than iterated: 25 windows sorted alphabetically was a wall, so the ones reached
        // constantly sit at the top level and the specialist clusters fold into submenus.
        if (ImGui::BeginMenu("Debug")) {
            WindowToggle(windows, "CPU");
            WindowToggle(windows, "Disassembly");
            WindowToggle(windows, "Call Stack");
            WindowToggle(windows, "Breakpoints");
            WindowToggle(windows, "Debugger Console", nullptr, "Shift-F12");

            ImGui::Separator();

            if (BeginMenuIcon(ICON_FA_MEMORY, "Memory")) {
                WindowToggle(windows, "Memory");
                WindowToggle(windows, "Memory Map");
                WindowToggle(windows, "Annotations");
                ImGui::EndMenu();
            }
            if (BeginMenuIcon(ICON_FA_SLIDERS, "Chipset")) {
                WindowToggle(windows, "Custom Registers");
                WindowToggle(windows, "Register Operations");
                WindowToggle(windows, "Copper");
                WindowToggle(windows, "DMA Stats");
                WindowToggle(windows, "CIA");
                ImGui::EndMenu();
            }
            if (BeginMenuIcon(ICON_FA_DESKTOP, "Graphics")) {
                WindowToggle(windows, "Display");
                WindowToggle(windows, "Bitplanes");
                WindowToggle(windows, "Sprites");
                WindowToggle(windows, "Blitter Operations");
                WindowToggle(windows, "Blitter Minterm");
                WindowToggle(windows, "Bartman Bitmaps");
                ImGui::EndMenu();
            }
            if (BeginMenuIcon(ICON_FA_VOLUME_HIGH, "Audio & Disk")) {
                WindowToggle(windows, "Audio");
                WindowToggle(windows, "Disk I/O");
                ImGui::EndMenu();
            }

            ImGui::Separator();

            WindowToggle(windows, "Frame Scrubber");
            WindowToggle(windows, "Profile");
            WindowToggle(windows, "Trainer");

            ImGui::Separator();

            // Only registered when the MCP server is present; WindowToggle draws nothing if not.
            WindowToggle(windows, "MCP Activity");
            ImGui::EndMenu();
        }

        // Tools for making a production, not for looking inside one: Exocet's sync tracker.
        // Hidden unless private features are on (core/PrivateFeatures.h).
        if (core::PrivateFeatures() && ImGui::BeginMenu("Exocet")) {
            WindowToggle(windows, "Exocet Control", "Exocet");
            WindowToggle(windows, "Exocet", "Exocet Grid");
            WindowToggle(windows, "Exocet Memory");
            WindowToggle(windows, "Exocet Music");
            WindowToggle(windows, "Exocet Disk Map");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (BeginMenuIcon(ICON_FA_FONT, "Text Size")) {
                if (MenuItemIcon(ICON_FA_PLUS, "Larger", "Ctrl+=") && onTextSize)
                    onTextSize(+1);
                if (MenuItemIcon(ICON_FA_MINUS, "Smaller", "Ctrl+-") && onTextSize)
                    onTextSize(-1);
                if (MenuItemIcon(ICON_FA_ROTATE_LEFT, "Reset", "Ctrl+0") && onTextSize)
                    onTextSize(0);
                ImGui::Separator();
                if (MenuItemIcon(ICON_FA_SLIDERS, "Appearance...") && onShowAppearance)
                    onShowAppearance();
                ImGui::EndMenu();
            }
            if (BeginMenuIcon(ICON_FA_EYE, "Workspaces")) {
                if (MenuItemIcon(ICON_FA_FLOPPY_DISK, "Save Current As...")) {
                    if (!workspaceState.selectedWorkspace.empty()) {
                        std::snprintf(s_workspaceNameBuf, sizeof(s_workspaceNameBuf), "%s",
                                      workspaceState.selectedWorkspace.c_str());
                    } else {
                        s_workspaceNameBuf[0] = '\0';
                    }
                    s_showSaveWorkspaceDialog = true;
                }
                if (!workspaceState.names.empty()) {
                    ImGui::Separator();
                    for (const auto& wsName : workspaceState.names) {
                        bool isSelected = (wsName == workspaceState.selectedWorkspace);
                        const bool scaled = std::find(workspaceState.scaledNames.begin(),
                                                      workspaceState.scaledNames.end(),
                                                      wsName) != workspaceState.scaledNames.end();
                        std::string label = (isSelected ? ICON_FA_CHECK "  " : "    ") + wsName +
                                            (scaled ? "  (scaled)" : "") + "##ws_" + wsName;
                        if (ImGui::BeginMenu(label.c_str(), true)) {
                            if (MenuItemIcon(ICON_FA_FOLDER_OPEN, "Load", nullptr, false)) {
                                if (workspaceState.onLoad)
                                    workspaceState.onLoad(wsName);
                            }
                            if (MenuItemIcon(ICON_FA_FLOPPY_DISK, "Update")) {
                                if (workspaceState.onSave)
                                    workspaceState.onSave(wsName);
                            }
                            ImGui::Separator();
                            if (MenuItemIcon(ICON_FA_TRASH, "Delete")) {
                                if (workspaceState.onDelete)
                                    workspaceState.onDelete(wsName);
                            }
                            ImGui::EndMenu();
                        }
                        if (scaled)
                            ImGui::SetItemTooltip(
                                "No %s layout saved yet: loads the %s one, scaled to this "
                                "screen.\nUpdate saves it as this workspace's %s layout.",
                                core::ScreenProfileLabel(workspaceState.screenProfile),
                                core::ScreenProfileLabel(
                                    core::OtherScreenProfile(workspaceState.screenProfile)),
                                core::ScreenProfileLabel(workspaceState.screenProfile));
                        // If the menu name itself is clicked, load the workspace
                        if (ImGui::IsItemClicked()) {
                            if (workspaceState.onLoad)
                                workspaceState.onLoad(wsName);
                        }
                    }
                }
                ImGui::Separator();
                float duration = workspaceState.transitionDuration;
                ImGui::Indent(8.0f);
                ImGui::TextDisabled("Transition Speed");
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::SliderFloat("##duration", &duration, 0.0f, 2.0f, "%.1fs")) {
                    if (workspaceState.onDurationChange)
                        workspaceState.onDurationChange(duration);
                }
                ImGui::Unindent(8.0f);
                ImGui::EndMenu();
            }

            if (BeginMenuIcon(ICON_FA_DESKTOP, "Screen Profile")) {
                for (core::ScreenProfile profile : core::kAllScreenProfiles) {
                    const bool isSelected = profile == workspaceState.screenProfile;
                    if (ImGui::MenuItem(core::ScreenProfileLabel(profile), nullptr, isSelected) &&
                        !isSelected && workspaceState.onScreenProfile)
                        workspaceState.onScreenProfile(profile);
                }
                ImGui::EndMenu();
            }

            if (BeginMenuIcon(ICON_FA_WINDOW_MAXIMIZE, "Panels")) {
                for (const auto& p : panelState.panels) {
                    if (ImGui::BeginMenu(p.name.c_str())) {
                        ImGui::MenuItem("Show", nullptr, p.openPtr);
                        ImGui::Separator();
                        if (MenuItemIcon(ICON_FA_PEN, "Rename...") && p.onRename)
                            p.onRename();
                        if (MenuItemIcon(ICON_FA_TRASH, "Delete") && p.onDelete)
                            p.onDelete();
                        ImGui::EndMenu();
                    }
                }
                if (!panelState.panels.empty())
                    ImGui::Separator();
                if (MenuItemIcon(ICON_FA_PLUS, "New Panel...") && panelState.onNewPanel)
                    panelState.onNewPanel();
                ImGui::EndMenu();
            }

            ImGui::Separator();

            // Every window without a menu home of its own. The skipped groups each have one:
            // Performance, Filters and Capture under Emulator, Disk under Disk, Search under
            // Search, Debugger under the top-level Debug menu, Exocet under its own. What
            // is left is a short enough list to sit directly in View rather than behind a
            // "Windows" submenu.
            std::string currentGroup;
            bool groupMenuOpen = false;
            for (const auto& window : windows) {
                if (window.groupName == "Performance" || window.groupName == "Debugger" ||
                    window.groupName == "Filters" || window.groupName == "Capture" ||
                    window.groupName == "Disk" || window.groupName == "Search" ||
                    window.groupName == "Exocet")
                    continue;

                if (window.groupName != currentGroup) {
                    if (!currentGroup.empty() && groupMenuOpen)
                        ImGui::EndMenu();
                    currentGroup = window.groupName;
                    if (!currentGroup.empty())
                        groupMenuOpen = ImGui::BeginMenu(currentGroup.c_str());
                    else
                        groupMenuOpen = false;
                }
                if (currentGroup.empty() || groupMenuOpen) {
                    MenuItemIcon(window.icon.c_str(), window.name.c_str(), nullptr, window.openPtr);
                    if (window.separatorAfter)
                        ImGui::Separator();
                }
            }
            if (!currentGroup.empty() && groupMenuOpen)
                ImGui::EndMenu();

            // Walks the whole registry, so it still closes debugger and disk windows too.
            if (ImGui::MenuItem("Hide All")) {
                for (const auto& window : windows) {
                    if (window.openPtr)
                        *window.openPtr = false;
                }
            }

            ImGui::Separator();

            // Column layout persists now, so a table someone has hidden columns out of stays
            // that way across restarts. ImGui's own header context menu offers "reset order"
            // and "size to fit" but deliberately no full reset (upstream calls it noisy), which
            // leaves this as the only way back to the defaults.
            if (MenuItemIcon(ICON_FA_TABLE_LIST, "Reset Table Columns"))
                GlobalTableLayout().ResetAll();
            ImGui::SetItemTooltip("Restore every table's column order, width and visibility to "
                                  "its default, and forget the saved layout.");

            ImGui::Separator();

            if (BeginMenuIcon(ICON_FA_FONT, "Fonts")) {
                for (const auto& fontName : availableFonts) {
                    bool isSelected = (fontName == currentFont);
                    if (ImGui::MenuItem(fontName.c_str(), nullptr, isSelected)) {
                        if (onFontSelected) {
                            onFontSelected(fontName);
                        }
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (MenuItemIcon(ICON_FA_MAP, "Setup Wizard") && ctx.onShowSetupWizard)
                ctx.onShowSetupWizard();
            if (MenuItemIcon(ICON_FA_CIRCLE_INFO, "About")) {
                if (onShowAbout) {
                    onShowAbout();
                }
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    // Process shortcuts if they weren't consumed by the menu (e.g. menu was closed)
    // We already handled them by OR-ing them into the menu items above, but the menu items
    // code block is ONLY executed if BeginMenu returns true (i.e. menu is open).
    // So we need to handle them triggers OUTSIDE the menu blocks if the menu is closed.

    // NOTE: This logic below is actually WRONG if we only rely on the OR above.
    // The OR above only works if the menu is visible.
    // We must execute the action if the trigger is true, regardless of menu state.

    if (triggerOpen) {
        openDialog = true;
    }

    if (triggerSave && onSave && hasUnsavedChanges) {
        onSave();
    }

    if (triggerExit) {
        if (hasUnsavedChanges) {
            if (onRequestExit)
                onRequestExit();
        } else {
            shouldExit = true;
        }
    }

    // Toggle Advanced Find
    if (triggerAdvancedFind) {
        if (showAdvancedFind) {
            *showAdvancedFind = !*showAdvancedFind;
            if (*showAdvancedFind && onAdvancedFind) {
                onAdvancedFind();
            }
        }
        triggerAdvancedFind = false;
    }

    if (triggerFind && onFind)
        onFind();
    if (triggerFindNext && onFindNext)
        onFindNext();
    if (triggerFindPrev && onFindPrev)
        onFindPrev();

    if (triggerRename && editState.onRename)
        editState.onRename();
    if (triggerDelete && editState.onDelete)
        editState.onDelete();

    if (onTextSize) {
        if (triggerTextLarger)
            onTextSize(+1);
        else if (triggerTextSmaller)
            onTextSize(-1);
        else if (triggerTextReset)
            onTextSize(0);
    }

    if (triggerTogglePause && onTogglePause)
        onTogglePause();

    if (triggerToggleTurbo && onToggleTurbo)
        onToggleTurbo();

    if (triggerStepFrame && onStepFrame)
        onStepFrame();

    if (triggerStepFrameBack && onStepFrameBack)
        onStepFrameBack();

    if (triggerStepInstruction && onStepInstruction)
        onStepInstruction();

    if (s_workspaceSwitcherOpen) {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(1.0f); // Solid background for the switcher

        ImGui::SetNextWindowSize(ImVec2(750, 0), ImGuiCond_Always);

        ImGui::Begin("##workspace_switcher", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 10.0f));

        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Switch Workspace...");
        ImGui::Separator();
        ImGui::Spacing();

        for (int i = 0; i < (int)workspaceState.names.size(); ++i) {
            bool isSelected = (i == s_workspaceSwitcherIndex);
            bool isActive = (workspaceState.names[i] == workspaceState.selectedWorkspace);

            std::string label = (isActive ? ICON_FA_CHECK "  " : "    ") + workspaceState.names[i];
            if (ImGui::Selectable(label.c_str(), isSelected, 0, ImVec2(0, 40))) {
                if (workspaceState.onLoad)
                    workspaceState.onLoad(workspaceState.names[i]);
                s_workspaceSwitcherOpen = false;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        // Get window rect for the dimming effect before ending
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();

        ImGui::PopStyleVar(2);
        ImGui::End();

        // Draw the "grey wash" in the foreground list, but only AROUND the window
        auto drawList = ImGui::GetForegroundDrawList(viewport);
        ImVec2 vPos = viewport->Pos;
        ImVec2 vSize = viewport->Size;
        ImU32 washCol = IM_COL32(120, 120, 120, 70);

        // Top
        drawList->AddRectFilled(vPos, ImVec2(vPos.x + vSize.x, winPos.y), washCol);
        // Bottom
        drawList->AddRectFilled(ImVec2(vPos.x, winPos.y + winSize.y),
                                ImVec2(vPos.x + vSize.x, vPos.y + vSize.y), washCol);
        // Left
        drawList->AddRectFilled(ImVec2(vPos.x, winPos.y), ImVec2(winPos.x, winPos.y + winSize.y),
                                washCol);
        // Right
        drawList->AddRectFilled(ImVec2(winPos.x + winSize.x, winPos.y),
                                ImVec2(vPos.x + vSize.x, winPos.y + winSize.y), washCol);
    }

    // Handle file-not-found dialog
    if (s_showFileNotFound) {
        ImGui::OpenPopup("Path Not Found");
        s_showFileNotFound = false;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Path Not Found", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Dummy(ImVec2(400.0f, 0.0f));
        ImGui::Text("The %s no longer exists:", s_notFoundPathIsDirectory ? "directory" : "file");
        ImGui::Spacing();
        ImGui::TextWrapped("%s", s_fileNotFoundPath.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            s_fileNotFoundPath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Save Workspace dialog ────────────────────────────────────────────────────
    if (s_showSaveWorkspaceDialog) {
        ImGui::OpenPopup("Save Workspace");
        s_showSaveWorkspaceDialog = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save Workspace", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Dummy(ImVec2(300.0f, 0.0f));
        ImGui::Text("Workspace name:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        bool hitEnter = ImGui::InputText("##wsname", s_workspaceNameBuf, sizeof(s_workspaceNameBuf),
                                         ImGuiInputTextFlags_EnterReturnsTrue |
                                             ImGuiInputTextFlags_AutoSelectAll);
        ImGui::Spacing();
        bool nameEmpty = (s_workspaceNameBuf[0] == '\0');
        if (hitEnter && !nameEmpty) {
            if (workspaceState.onSave)
                workspaceState.onSave(std::string(s_workspaceNameBuf));
            ImGui::CloseCurrentPopup();
        }
        ImGui::BeginDisabled(nameEmpty);
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            if (workspaceState.onSave)
                workspaceState.onSave(std::string(s_workspaceNameBuf));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Screen profile prompt (first launch) ─────────────────────────────────────
    // Asked once, before anything else is set up: the layouts are made for a size of screen, and
    // a first run should not open on ones that do not fit. The suggested profile is already live
    // behind the prompt; either button records the choice, so it never shows again. Re-opened
    // until then: the first frames load a workspace, whose settling can close popups.
    if (workspaceState.askScreenProfile && !ImGui::IsPopupOpen("Screen Profile"))
        ImGui::OpenPopup("Screen Profile");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Screen Profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const core::ScreenProfile suggested = workspaceState.suggestedProfile;
        const float em = ImGui::GetFontSize();
        ImGui::Dummy(ImVec2(em * 26.0f, 0.0f));
        ImGui::Text("This screen suits the %s profile.", core::ScreenProfileLabel(suggested));
        ImGui::Spacing();
        ImGui::PushTextWrapPos(em * 26.0f);
        ImGui::TextDisabled("Peekaboo keeps separate window layouts and text sizes for large "
                            "and small screens. Switch any time from View > Screen Profile.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Spacing();
        for (core::ScreenProfile profile : {suggested, core::OtherScreenProfile(suggested)}) {
            if (profile != suggested)
                ImGui::SameLine();
            const std::string label = std::string("Use ") + core::ScreenProfileLabel(profile);
            if (ImGui::Button(label.c_str(), ImVec2(em * 8.0f, 0.0f))) {
                if (workspaceState.onScreenProfile)
                    workspaceState.onScreenProfile(profile);
                ImGui::CloseCurrentPopup();
            }
            if (profile == suggested)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndPopup();
    }

    // ── Save emulator configuration dialog ──────────────────────────────────────
    if (s_showSaveEmulatorConfigurationDialog) {
        ImGui::OpenPopup("Save Emulator Configuration");
        s_showSaveEmulatorConfigurationDialog = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save Emulator Configuration", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Dummy(ImVec2(360.0f, 0.0f));
        ImGui::Text("Configuration name:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool hitEnter = ImGui::InputText("##emuconfigname", s_emulatorConfigurationNameBuf,
                                               sizeof(s_emulatorConfigurationNameBuf),
                                               ImGuiInputTextFlags_EnterReturnsTrue |
                                                   ImGuiInputTextFlags_AutoSelectAll);
        if (!s_emulatorConfigurationError.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                               s_emulatorConfigurationError.c_str());
        }
        ImGui::Spacing();
        const bool nameEmpty = s_emulatorConfigurationNameBuf[0] == '\0';
        const bool saveRequested =
            (hitEnter && !nameEmpty) || (!nameEmpty && ImGui::Button("Save", ImVec2(120, 0)));
        if (saveRequested && onSaveCustomConfiguration) {
            std::string error;
            if (onSaveCustomConfiguration(s_emulatorConfigurationNameBuf, error)) {
                s_emulatorConfigurationError.clear();
                ImGui::CloseCurrentPopup();
            } else {
                s_emulatorConfigurationError = std::move(error);
            }
        }
        if (!hitEnter) {
            if (nameEmpty)
                ImGui::BeginDisabled();
            // The Save button was already emitted above when enabled.
            if (nameEmpty)
                ImGui::Button("Save", ImVec2(120, 0));
            if (nameEmpty)
                ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            s_emulatorConfigurationError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (s_showRenameEmulatorConfigurationDialog) {
        ImGui::OpenPopup("Rename Emulator Configuration");
        s_showRenameEmulatorConfigurationDialog = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Rename Emulator Configuration", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Dummy(ImVec2(360.0f, 0.0f));
        ImGui::Text("Configuration name:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool hitEnter = ImGui::InputText(
            "##renameemuconfigname", s_renameEmulatorConfigurationNameBuf,
            sizeof(s_renameEmulatorConfigurationNameBuf),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (!s_emulatorConfigurationError.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                               s_emulatorConfigurationError.c_str());
        }
        ImGui::Spacing();
        const bool nameEmpty = s_renameEmulatorConfigurationNameBuf[0] == '\0';
        bool renameRequested = hitEnter && !nameEmpty;
        if (!nameEmpty) {
            if (ImGui::Button("Rename", ImVec2(120, 0)))
                renameRequested = true;
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("Rename", ImVec2(120, 0));
            ImGui::EndDisabled();
        }
        if (renameRequested && onRenameCustomConfiguration) {
            std::string error;
            if (onRenameCustomConfiguration(s_renameEmulatorConfigurationPath,
                                            s_renameEmulatorConfigurationNameBuf, error)) {
                s_emulatorConfigurationError.clear();
                s_renameEmulatorConfigurationPath.clear();
                ImGui::CloseCurrentPopup();
            } else {
                s_emulatorConfigurationError = std::move(error);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            s_emulatorConfigurationError.clear();
            s_renameEmulatorConfigurationPath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (s_showUpdateEmulatorConfigurationDialog) {
        ImGui::OpenPopup("Update Emulator Configuration");
        s_showUpdateEmulatorConfigurationDialog = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Update Emulator Configuration", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Dummy(ImVec2(400.0f, 0.0f));
        ImGui::TextWrapped("Replace \"%s\" with the current machine setup? Its saved model, "
                           "settings, drives and disk media will be overwritten.",
                           s_updateEmulatorConfigurationName.c_str());
        if (!s_emulatorConfigurationError.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                               s_emulatorConfigurationError.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("Update", ImVec2(120, 0)) && onUpdateCustomConfiguration) {
            std::string error;
            if (onUpdateCustomConfiguration(s_updateEmulatorConfigurationPath, error)) {
                s_emulatorConfigurationError.clear();
                s_updateEmulatorConfigurationPath.clear();
                s_updateEmulatorConfigurationName.clear();
                ImGui::CloseCurrentPopup();
            } else {
                s_emulatorConfigurationError = std::move(error);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            s_emulatorConfigurationError.clear();
            s_updateEmulatorConfigurationPath.clear();
            s_updateEmulatorConfigurationName.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (s_showEmulatorConfigurationErrorDialog) {
        ImGui::OpenPopup("Emulator Configuration Error");
        s_showEmulatorConfigurationErrorDialog = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Emulator Configuration Error", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Dummy(ImVec2(400.0f, 0.0f));
        ImGui::TextWrapped("%s", s_emulatorConfigurationError.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            s_emulatorConfigurationError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── New Disk dialog ──────────────────────────────────────────────────────────
    if (s_showNewDiskDialog) {
        ImGui::OpenPopup("New Disk");
        s_showNewDiskDialog = false;
        if (s_newDiskTemplateIdx >= static_cast<int>(diskTemplates.size()))
            s_newDiskTemplateIdx = 0;
        if (s_newDiskBootBlockIdx >= static_cast<int>(bootBlockNames.size()))
            s_newDiskBootBlockIdx = 0;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("New Disk", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Dummy(ImVec2(360.0f, 0.0f));

        // Filesystem (existing ADF templates)
        ImGui::Text("Filesystem:");
        ImGui::SetNextItemWidth(-1.0f);
        const char* fsPreview = diskTemplates.empty() ? "No templates found"
                                                      : diskTemplates[s_newDiskTemplateIdx].c_str();
        if (ImGui::BeginCombo("##fs", fsPreview)) {
            for (int i = 0; i < static_cast<int>(diskTemplates.size()); ++i) {
                bool sel = (i == s_newDiskTemplateIdx);
                if (ImGui::Selectable(diskTemplates[i].c_str(), sel))
                    s_newDiskTemplateIdx = i;
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::Text("Volume label:");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##label", s_newDiskLabel, sizeof(s_newDiskLabel));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool haveBootBlocks = !bootBlockNames.empty();
        if (!haveBootBlocks)
            ImGui::BeginDisabled();
        ImGui::Checkbox("Bootable", &s_newDiskBootable);
        if (!haveBootBlocks) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(library empty)");
            s_newDiskBootable = false;
        }

        if (s_newDiskBootable) {
            ImGui::Text("Boot block:");
            ImGui::SetNextItemWidth(-1.0f);
            const char* bbPreview = bootBlockNames[s_newDiskBootBlockIdx].c_str();
            if (ImGui::BeginCombo("##bb", bbPreview)) {
                for (int i = 0; i < static_cast<int>(bootBlockNames.size()); ++i) {
                    bool sel = (i == s_newDiskBootBlockIdx);
                    if (ImGui::Selectable(bootBlockNames[i].c_str(), sel))
                        s_newDiskBootBlockIdx = i;
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::Checkbox("Create empty S/startup-sequence", &s_newDiskEmptyStartup);
            ImGui::TextDisabled("Installs the boot block; a self-booting disk also needs");
            ImGui::TextDisabled("its system files / matching track data.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool canCreate =
            !diskTemplates.empty() && onNewDiskAdvanced && (!s_newDiskBootable || haveBootBlocks);
        ImGui::BeginDisabled(!canCreate);
        if (ImGui::Button("Create...", ImVec2(120, 0))) {
            std::string templateName = diskTemplates[s_newDiskTemplateIdx];
            std::string bbName =
                (s_newDiskBootable && haveBootBlocks) ? bootBlockNames[s_newDiskBootBlockIdx] : "";
            onNewDiskAdvanced(templateName, std::string(s_newDiskLabel), s_newDiskBootable, bbName,
                              s_newDiskBootable && s_newDiskEmptyStartup);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Format Disk dialog ───────────────────────────────────────────────────────────────────────
    // This is the confirmation, not a form with one in front of it: nothing is destroyed until
    // Format is pressed, and the label/filesystem it will apply are both on screen while the
    // warning is.
    if (s_showFormatDiskDialog) {
        ImGui::OpenPopup("Format Disk");
        s_showFormatDiskDialog = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Format Disk", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Dummy(ImVec2(380.0f, 0.0f));

        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION
                           "  Everything on this disk will be erased.");
        ImGui::TextWrapped("%s", s_formatDiskTarget.empty()
                                     ? "The active disk image is reformatted in place."
                                     : s_formatDiskTarget.c_str());
        ImGui::TextDisabled("The image is rewritten in memory - the file on disk changes");
        ImGui::TextDisabled("only when you save it.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Volume label:");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##formatlabel", s_formatDiskLabel, sizeof(s_formatDiskLabel));

        ImGui::Spacing();
        ImGui::Text("Filesystem:");
        if (ImGui::RadioButton("OFS", s_formatDiskUseOFS))
            s_formatDiskUseOFS = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("FFS", !s_formatDiskUseOFS))
            s_formatDiskUseOFS = false;

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::BeginDisabled(!ctx.canFormatDisk || !onFormatDisk);
        if (ImGui::Button("Format", ImVec2(120, 0))) {
            onFormatDisk(std::string(s_formatDiskLabel), s_formatDiskUseOFS);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Handle file open dialog
    if (openDialog) {
        nfdchar_t* outPath;
        nfdresult_t result = NFD_OpenDialog(&outPath, nullptr, 0, nullptr);

        // Reset ImGui input state because NFD_OpenDialog blocks and can miss key-up events
        ImGui::GetIO().ClearInputKeys();

        if (result == NFD_OKAY) {
            if (onOpenFile) {
                onOpenFile(std::string(outPath));
            }
            NFD_FreePath(outPath);
        } else if (result == NFD_CANCEL) {
            // User cancelled
        } else {
            fmt::print(stderr, "Error: {}\n", NFD_GetError());
        }
    }

    return shouldExit;
}

} // namespace ui
} // namespace adm
