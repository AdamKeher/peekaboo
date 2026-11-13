#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../ui/BlitOverlayController.h"
#include "../ui/CopperRasterController.h"
#include "../ui/FontManager.h"
#include "../ui/MainLayout.h"
#include "../ui/PanelManager.h"
#include "../ui/TextureManager.h"
#include "../ui/UnsavedChangesDialog.h"
#include "../ui/ViewerLauncher.h"
#include "../ui/WorkspaceLayout.h"
#include "../ui/views/AboutView.h"
#include "../ui/views/AudioPlayer.h"
#include "../ui/views/OnboardingView.h"
#include "../ui/views/PreferencesView.h"

// Windows API macro pollution: glad/glad.h (pulled in by TextureManager.h) includes
// windows.h which #defines CreateDirectory/DeleteFile/MoveFile to their A/W variants.
// Undef them here so the method declarations below use their real names.
#ifdef _WIN32
#undef CreateDirectory
#undef DeleteFile
#undef MoveFile
#endif

#include "../ui/BlitOverlaySettings.h"
#include "../ui/UiInputInjector.h"
#include "../ui/UiIntrospect.h"
#include "../ui/WorkspaceTransitionManager.h"
#include "../ui/views/BlitterVisOverlay.h"
#include "../ui/views/CopperRasterOverlay.h"
#include "AdfWrapper.h"
#include "AppCapture.h"
#include "AudioEngine.h"
#include "AudioLogSnapshot.h"
#include "BlitLogSnapshot.h"
#include "BlitProfiler.h"
#include "BlitScreenMapper.h"
#include "BootBlockLibrary.h"
#include "BootBlockPreview.h"
#include "BrainFile.h"
#include "CompiledProgram.h"
#include "CpuTraceHistory.h"
#include "DataTypeManager.h"
#include "DebugSession.h"
#include "DefinitionManager.h"
#include "DiskDocument.h"
#include "DiskDocumentInfo.h"
#include "DiskLogSnapshot.h"
#include "DiskProfiler.h"
#include "DocumentTransfer.h"
#include "DosBreakpoints.h"
#include "ElfHunkMap.h"
#include "EmulatorStateStore.h"
#include "ExecIntrospect.h"
#include "FileOperationService.h"
#include "FrameHistory.h"
#include "FrameThumbnails.h"
#include "FrameTimeline.h"
#include "GdbServer.h"
#include "ImageLoader.h"
#include "IpfTypes.h"
#include "LaunchOptions.h"
#include "LibraryCallTracer.h"
#include "LineCoverage.h"
#include "LogSnapshots.h"
#include "McpServer.h"
#include "McpToolHandler.h"
#include "MemorySearcher.h"
#include "MemorySnapshotStore.h"
#include "PeekabooHttpServer.h"
#include "RegLogSnapshot.h"
#include "RegWriteProfiler.h"
#include "ScenarioRequest.h"
#include "ScenarioRunner.h"
#include "SessionSerializer.h"
#include "TimeTravel.h"
#include "UserPreferences.h"
#include "WorkspaceManager.h"
// FloppyDrivesView::Config and ::Status are used by value below, so a forward
// declaration is not enough. The header is light: <functional>/<string>/<vector>.
#include "../ui/views/FloppyDrivesView.h"

// Forward declarations to avoid header includes
struct ImGuiIO;
struct GLFWwindow;

namespace adm {
namespace ui {
class AudioPlayer;
class FileListView;
class FileViewer;
class HunkViewer;
class ImageViewer;
class BlockEditorView;
class ValidationView;
class BitmapView;
class BlockMapView;
class NDOSView;
class ChangeMapView;
class BootBlockView;
class DmsView;
class IdentifyView;
class AdvancedFindView;
class TosecBrowserView;
class FloppyDiskView;
class EmulatorView;
class BreakpointsView;
class DebuggerView;
class LogView;
class MemoryMapView;
class BlitterView;
class RegisterOpsView;
class DiskIoView;
class ExocetView;
class ExocetControlView;
class ExocetMemoryView;
class ExocetDiskMapView;
class ExocetMusicView;
class AudioPlayer;
class BlitterMintermView;
class DmaStatsView;
class PanelView;
class McpActivityView;
} // namespace ui
class AmigaEmulator;
} // namespace adm

namespace adm {
namespace core {

class DiskSearcher;
class ExocetSession;
class DebugFrame;
class DebugInline;
class DebugLines;
class DebugSymbols;

/**
 * @brief Central application controller that manages views and disk operations.
 *
 * Supports multiple open ADF files simultaneously. Each open file gets a
 * DiskDocument with its own FileListView. Shared views (hex editor, bitmap, etc.)
 * reflect the currently focused/active document.
 */
class Application
{
public:
    Application();
    ~Application();
    void Shutdown();

    // Non-copyable
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * @brief Initialize fonts and views. Call after ImGui context is created.
     * @param io ImGui IO reference for font loading.
     * @param window GLFW window handle.
     * @param scale UI scale factor.
     */
    void Initialize(ImGuiIO& io, GLFWwindow* window, float scale = 1.0f);

    /**
     * @brief Toggle the application window fullscreen state.
     * @param fullscreen True to enable fullscreen, false to disable.
     */
    void ToggleFullscreen(bool fullscreen);

    // Called from GLFW pos/size callbacks to keep the stored windowed geometry current.
    void OnWindowMoved(int x, int y)
    {
        m_windowX = x;
        m_windowY = y;
    }
    void OnWindowResized(int w, int h)
    {
        m_windowWidth = w;
        m_windowHeight = h;
    }

    std::vector<ui::WindowState> GetWindowStates();

    /**
     * @brief Load an ADF file and update all views.
     * @param path Path to the ADF file.
     * @return true if the file was loaded successfully.
     */
    bool LoadAdf(const std::string& path);
    /**
     * @brief Load a document from the debugger console / MCP (non-interactive).
     *
     * Mirrors LoadAdf but never pops the save prompt: if the active document has
     * unsaved changes it is only replaced when @p force is true, otherwise the
     * load is refused and this returns false.
     */
    bool LoadDocumentExternal(const std::string& path, bool force, bool reloadIfOpen = false);
    bool LoadAdfFromTosec(std::vector<uint8_t> data, const std::string& filename);

    /**
     * @brief Open a remote file as a placeholder document and download it in the
     * background. The tab appears immediately showing progress; when the download
     * completes the volume is mounted and the tab switches to the normal disk view.
     * @param req       What to download (URL, expected file type).
     * @param source    Provenance shown in the tab (TOSEC / demozoo / pouet / ...).
     */
    void StartDownloadDocument(const core::DownloadRequest& req,
                               const core::DemosceneSource& source);

    /**
     * @brief Gets the fallback layout placement for a new FileListView window.
     */
    void GetDefaultFileListViewPlacement(uint32_t& outDockId, float& outPosX, float& outPosY,
                                         float& outSizeX, float& outSizeY,
                                         bool& outHasPosSize) const;

    /**
     * @brief Run a single frame of UI drawing.
     */
    void RunFrame();

    /**
     * @brief Apply any pending workspace layout. Must be called BEFORE ImGui::NewFrame().
     */
    void ApplyPendingWorkspaceLayout();
    // Applies the text-size preferences (interface zoom, per-role fonts) and any Ctrl+wheel
    // steps from last frame. Called before ImGui::NewFrame: the style must not change mid-frame.
    void ApplyPendingFontSettings();

    /**
     * @brief Render the icon toolbar.
     * @param dpi_scale The current DPI scale factor.
     */
    void RenderToolbar(float dpi_scale);

    /**
     * @brief Get the list of available fonts for the View menu.
     */
    const std::vector<std::string>& GetAvailableFonts() const;

    /**
     * @brief Get the current font name.
     */
    std::string GetCurrentFontName() const;

    /**
     * @brief Set the current font by name.
     */
    void SetCurrentFont(const std::string& fontName);

    /**
     * @brief Get the recent files list.
     */
    const std::vector<std::string>& GetRecentFiles() const;
    const std::vector<std::string>& GetRecentDirectories() const;

    /**
     * @brief Get the user preferences.
     */
    UserPreferences& GetUserPrefs() { return m_userPrefs; }

    /**
     * @brief Get the workspace manager.
     */
    WorkspaceManager& GetWorkspaceManager() { return m_workspaceManager; }

    /**
     * @brief Get the workspace transition manager.
     */
    ui::WorkspaceTransitionManager& GetWorkspaceTransitionManager() { return m_transitionManager; }

    /**
     * @brief Get the Font Manager
     */
    ui::FontManager& GetFontManager() { return m_fontManager; }

    /**
     * @brief Save the current window layout as a named workspace.
     */
    void SaveWorkspace(const std::string& name);

    /**
     * @brief Restore a previously saved workspace by name.
     */
    void LoadWorkspace(const std::string& name);

    // --- Screen profile ---

    /// The profile suggested for the screen @p window is on: its monitor's work area in interface
    /// units, i.e. divided by the DPI factor the style is scaled by and by the interface zoom.
    static ScreenProfile SuggestScreenProfile(GLFWwindow* window, float uiScale);

    /// The profile in use - chosen, or on a first launch the suggested one until the user picks.
    ScreenProfile GetScreenProfile() const { return m_workspaceLayout.Profile(); }

    /// True until the user has picked a profile; the first launch asks.
    bool IsScreenProfileUnchosen() const { return m_userPrefs.Get().screenProfile.empty(); }

    /// The profile to start in, before the first workspace load. Does not record a choice.
    void InitScreenProfile(ScreenProfile profile) { m_workspaceLayout.SetProfile(profile); }

    /**
     * @brief Make @p profile the live one and record it as the user's choice.
     *
     * Stashes the outgoing profile's text sizes, window rectangle, splitters and table columns,
     * restores the incoming one's, and calls the screen-profile-changed callback (which reloads
     * the current workspace in the new profile's layout) when the profile actually changed.
     */
    void SetScreenProfile(ScreenProfile profile);

    /// Called after the live profile changes; main() reloads the selected workspace.
    void SetOnScreenProfileChanged(std::function<void()> cb)
    {
        m_onScreenProfileChanged = std::move(cb);
    }

    // Panel management
    void CreatePanel(const std::string& name);
    void DeletePanel(uint32_t id);
    void RenamePanel(uint32_t id, const std::string& newName);
    void RequestNewPanel();
    void RequestRenamePanel(uint32_t id);
    const std::vector<std::unique_ptr<ui::PanelView>>& GetPanels() const
    {
        return m_panelManager.Panels();
    }

    // Serviced by main.cpp's render loop: the capture has to read the default framebuffer after
    // the final draw and before the swap, and the injector has to run after the GLFW backend's
    // NewFrame and before ImGui::NewFrame. Neither moment is reachable from RunFrame.
    core::AppCapture& GetAppCapture() { return m_appCapture; }
    ui::UiInputInjector& GetUiInputInjector() { return m_uiInput; }

    /** Save panel layout to user_prefs so it can be restored on the next launch. */
    void SaveSessionState();

    /** ImGui name of the window focused this session, for persisting across launches. */
    const std::string& GetFocusedWindowName() const { return m_lastFocusedWindow; }

    /**
     * @brief Set callback for when File > Open is triggered.
     */
    void SetOnOpenFileRequested(std::function<void(const std::string&)> callback);

    /**
     * @brief Save the active document's ADF to its original file.
     * @return true on success.
     */
    bool SaveAdf();

    /**
     * @brief Save the active document's ADF to a new file.
     * @return true on success.
     */
    bool SaveAdfAs();

    /**
     * @brief Check if any document has unsaved changes.
     * @return true if there are unsaved changes.
     */
    bool HasUnsavedChanges() const;

    /**
     * @brief Check if an ADF file is currently loaded (active document exists).
     * @return true if a file is loaded.
     */
    bool HasFileLoaded() const;
    bool HasDiskImageLoaded() const;
    bool HasRunnableMedia() const;

    /**
     * @brief Close the active document.
     */
    void CloseAdf();

    /**
     * @brief Close all open documents.
     */
    void CloseAllAdf();

    /**
     * @brief Restore a recorded frame and re-run the frame that followed it.
     *
     * The re-run is the point: because the frame is actually executed again, every
     * per-frame capture path (blitter, copper, DMA record, CPU trace, source-level bands)
     * repopulates for a frame that has already gone past. Enabling the CPU trace and then
     * seeking back is how you get a trace for a glitch you have already watched happen.
     *
     * Returns an error string, empty on success.
     */
    std::string SeekToRecordedFrame(uint64_t frame) { return m_timeline.SeekTo(frame); }

    /// Step back `count` frames from wherever the timeline is parked.
    std::string RewindFrames(uint64_t count) { return m_timeline.Rewind(count); }

    /// True while the timeline owns the machine: playback is running or an A-B range is
    /// armed, so the emulator is held paused and its frames come from the recording.
    bool IsTimelineDrivingMachine() const { return m_timeline.IsDrivingMachine(); }

    /// Hand the machine back to live running: stops playback and disarms any A-B range.
    /// Every path that resumes, steps or otherwise runs the machine has to call this first,
    /// or the transport keeps replaying frames into a machine that is running underneath it.
    /// No-op when the timeline is not driving. Does not itself unpause the emulator.
    void ReleaseTimelineMachine() { m_timeline.ReleaseMachine(); }

    /// Replay a recorded frame range on repeat (ENGINE9000's `loop`). Pauses live running
    /// for the duration. Returns an error string, empty on success.
    std::string SetFrameLoop(uint64_t a, uint64_t b) { return m_timeline.SetLoop(a, b); }
    void ClearFrameLoop() { m_timeline.ClearLoop(); }
    /// The armed loop range. False when no loop is running.
    bool GetFrameLoop(uint64_t& a, uint64_t& b) const { return m_timeline.GetLoop(a, b); }

    /// Throw the recorded timeline away: states, previews, loop and playback all go. The
    /// recorder stays armed and starts filling again from the next frame, which is the
    /// point - it is how you drop everything before the run you actually care about.
    void ClearFrameTimeline() { m_timeline.Clear(); }

    /// Which way the timeline is played back. Ping-pong runs A->B then B->A, which is how
    /// you catch the frame a defect appears on without having to re-arm anything.
    using TimelinePlayMode = core::FrameTimeline::PlayMode;

    /// Transport over the recorded timeline. Playback replays recorded frames instead of
    /// running the machine live, bounded by the armed loop when there is one and by the
    /// whole recorded range when there is not.
    void SetTimelinePlaying(bool playing) { m_timeline.SetPlaying(playing); }
    void SetTimelinePlayMode(TimelinePlayMode mode) { m_timeline.SetPlayMode(mode); }
    /// Playback rate as a multiple of the machine's own frame rate. Clamped to 1/8x..2x.
    void SetTimelineSpeed(double speed) { m_timeline.SetSpeed(speed); }
    /// Move one frame (delta +1 / -1) and stop playback. Error string, empty on success.
    std::string StepTimeline(int delta) { return m_timeline.Step(delta); }

    using TimelinePlayback = core::FrameTimeline::Playback;
    TimelinePlayback GetTimelinePlayback() const { return m_timeline.GetPlayback(); }

    /// Recorded range, memory used and measured cost, for the HISTORY command and the UI.
    core::FrameHistoryStats GetFrameHistoryStats() const { return m_timeline.Stats(); }
    /// The frame the timeline is parked on, or the newest recorded frame when live.
    uint64_t GetFrameHistoryPosition() const { return m_timeline.Position(); }

    /// Current settings + live state for the Emulator -> Frame History menu.
    ui::MainLayout::DrawContext::FrameHistoryMenu GetFrameHistoryMenuState() const;
    /// Apply the menu's settings. Persisted through UserPreferences.
    // Emulator -> "Log core output". Lets PUAE's own log through into the Log window; see
    // AmigaEmulator::SetCoreLogVerbose for why it is off by default.
    void SetCoreVerboseLog(bool enabled);
    bool IsCoreVerboseLog() const { return m_userPrefs.Get().coreVerboseLog; }

    void SetFrameHistorySettings(bool enabled, int budgetMB, bool persistInSession, bool thumbnails,
                                 int thumbnailInterval)
    {
        m_timeline.ApplySettings(enabled, budgetMB, persistInSession, thumbnails,
                                 thumbnailInterval);
    }

    /**
     * @brief Screen thumbnail for a recorded frame, for the scrub preview.
     *
     * Thumbnails are kept every N frames rather than every frame, so this answers with the
     * nearest shot at or before `frame` and reports which frame it is really from in
     * `gotFrame` - the preview must not imply an exactness it does not have. `rgb` comes
     * back top-row-first RGB888. False when nothing has been captured.
     */
    bool GetFrameThumbnail(uint64_t frame, std::vector<uint8_t>& rgb, int& w, int& h,
                           uint64_t& gotFrame) const
    {
        return m_timeline.GetThumbnail(frame, rgb, w, h, gotFrame);
    }

    /// Thumbnails held, memory used and how well the deltas are compressing them.
    core::FrameThumbnailStats GetFrameThumbnailStats() const { return m_timeline.ThumbnailStats(); }

    /**
     * @brief Ranges of emulated RAM that differ between two recorded frames.
     *
     * ENGINE9000's `diff`. Answers "what did those frames actually touch?" without having
     * to guess an address first. Runs are reported against the live machine's bank layout,
     * so they are Amiga addresses, not save-state offsets.
     *
     * Returns an error string, empty on success. `out` is capped at `maxRuns`.
     */
    std::string DiffRecordedFrames(uint64_t a, uint64_t b, std::vector<core::FrameDiffRun>& out,
                                   size_t maxRuns, bool& truncated) const
    {
        return m_timeline.DiffFrames(a, b, out, maxRuns, truncated);
    }

    /**
     * @brief Save the current debug session (disks, machine snapshot, breakpoints, view
     * state, notes) to its existing .peek file. Falls back to Save Session As the first time
     * (no file chosen yet).
     */
    void SaveSession();

    /**
     * @brief Save the current debug session to a new .peek file, prompting with a native
     * file dialog pre-populated with the current session / active disk name.
     */
    void SaveSessionAs();

    /**
     * @brief Open a .peek debug session, replacing the current documents and machine state.
     * Prompts for a file with a native file dialog.
     */
    void OpenSession();

    /**
     * @brief Open a .peek session directly from a path (no dialog). Used by OpenSession, the
     * Open Recents menu and drag-and-drop. Adds the path to recent files on success.
     */
    void OpenSessionFromPath(const std::string& path);

    /** @brief Free-text notes attached to the current session (shown in the Notes panel). */
    std::string& SessionNotes() { return m_sessionNotes; }

    /**
     * @brief Request application exit, prompting for unsaved changes if needed.
     */
    void RequestExit();

    /**
     * @brief Returns true once the user has confirmed exit through any unsaved-changes dialogs.
     */
    bool WantsToExit() const { return m_wantsToExit; }

    /**
     * @brief Check if a specific block has been modified in the active document.
     * @param block The block number to check.
     * @return true if the block has been modified.
     */
    bool IsBlockModified(uint32_t block) const;

    /**
     * @brief Import files from host to ADF.
     * @param destinationSector Target directory sector (0 for root).
     */
    void ImportFiles(uint32_t destinationSector);

    /**
     * @brief Create a new directory in ADF.
     * @param parentSector Parent directory sector.
     * @param name Directory name.
     */
    void CreateDirectory(uint32_t parentSector, const std::string& name);

    /**
     * @brief Delete multiple files or directories.
     * @param items List of {parentSecondary, name} pairs to delete.
     */
    void DeleteFiles(const std::vector<std::pair<uint32_t, std::string>>& items);

    /**
     * @brief Scrub (securely delete) multiple files or directories.
     * @param items List of {startSector, name} pairs to scrub. Note: Using startSector unique ID.
     */
    void ScrubFiles(const std::vector<std::pair<uint32_t, std::string>>& items);

    /**
     * @brief Export selected files/directories to host filesystem.
     * @param nodes Vector of FileNodes to export.
     */
    void ExportFiles(const std::vector<FileNode>& nodes);

    // Binds a document's drag-and-drop and paste callbacks to m_documentTransfer.
    void SetupCrossDocumentTransfer(DiskDocument* document);

    /**
     * @brief Access to the BlockEditorView for search operations.
     */
    ui::BlockEditorView* GetHexEditorView() const { return m_hexEditorView.get(); }
    ui::BitmapView* GetBitmapView() const { return m_bitmapView.get(); }
    ui::BlockMapView* GetBlockMapView() const { return m_blockMapView.get(); }
    ui::NDOSView* GetNdosView() const { return m_ndosView.get(); }
    ui::ChangeMapView* GetChangeMapView() const { return m_changeMapView.get(); }
    ui::EmulatorView* GetEmulatorView() const { return m_emulatorView.get(); }
    ui::DebuggerView* GetDebuggerView() const { return m_debuggerView.get(); }
    ui::MemoryMapView* GetMemoryMapView() const { return m_memoryMapView.get(); }
    ui::BlitterView* GetBlitterView() const { return m_blitterView.get(); }
    ui::RegisterOpsView* GetRegisterOpsView() const { return m_registerOpsView.get(); }
    ui::DiskIoView* GetDiskIoView() const { return m_diskIoView.get(); }
    ui::ExocetView* GetExocetView() const { return m_exocetView.get(); }
    core::ExocetSession* GetExocetSession() const { return m_exocet.get(); }
    ui::BlitterMintermView* GetBlitterMintermView() const { return m_blitterMintermView.get(); }
    ui::DmaStatsView* GetDmaStatsView() const { return m_dmaStatsView.get(); }

    /// True while the picture on screen is a Picasso96 (RTG) screen rather than the chipset's.
    /// The raster, sprite and blit overlays all sit on beam geometry an RTG frame does not
    /// have, so each checks this before drawing. Reads a handful of plain ints out of the core
    /// on whichever thread asks; one frame of staleness across the emulation thread is
    /// harmless, and the answer is false on any core without the export (pre-1.99.0).
    bool IsRtgDisplayActive() const;

    // ── Search context dispatch ──────────────────────────────────────────────────
    enum class SearchContext { Disk, Memory };
    SearchContext GetSearchContext() const { return m_searchContext; }

    // These dispatch Find/FindNext/FindPrevious to the correct view based on the
    // last focused view (Hex View → Disk, Emulator Memory tab → Memory).
    void TriggerFind();
    void TriggerFindNext();
    void TriggerFindPrevious();

    // Update the MemorySearcher with fresh regions from the emulator.
    void UpdateMemorySearcherRegions();
    ui::IdentifyView* GetIdentifyView() const { return m_identifyView.get(); }
    ui::LogView* GetLogView() const { return m_logView.get(); }
    ui::McpActivityView* GetMcpActivityView() const { return m_mcpActivityView.get(); }
    ui::TosecBrowserView* GetTosecBrowserView() const { return m_tosecBrowserView.get(); }
    ui::FloppyDrivesView* GetFloppyDrivesView() const { return m_floppyDrivesView.get(); }

    /**
     * @brief Open the TOSEC Browser window.
     */
    void ShowTosecBrowser();
    // Opens the Live Floppy Drive window (FloppyBridge physical drive support).
    void ShowFloppyDrives();

    // Live physical floppy drive (FloppyBridge). These sit on Application rather than in
    // FloppyDrivesView because attaching has to keep the emulator, the EmulatorView drive
    // model and the session record in step - the view only knows about the hardware.
    // AttachLiveFloppy returns false if the request was rejected outright; a true return
    // means queued, since the core opens the device on its next emulated vertical blank.
    bool AttachLiveFloppy(const ui::FloppyDrivesView::Config& cfg);
    void DetachLiveFloppy(int driveSlot);
    ui::FloppyDrivesView::Status GetLiveFloppyStatus(int driveSlot) const;

    /**
     * @brief Handle request to create a new disk from a template.
     * @param templateName Name of the template (filename with extension).
     */
    void OnNewDiskRequested(const std::string& templateName);

    /**
     * @brief Handle request to create a new disk via the New Disk dialog.
     * @param templateName Filesystem template (filename with extension).
     * @param label Volume label (may be empty).
     * @param bootable If true, install the selected boot block onto blocks 0 & 1.
     * @param bootBlockName Name of the library boot block to install (when bootable).
     * @param emptyStartup If true, create an empty S/startup-sequence.
     */
    void OnNewDiskAdvanced(const std::string& templateName, const std::string& label, bool bootable,
                           const std::string& bootBlockName, bool emptyStartup);

    /**
     * @brief Reformat the active disk image in place, destroying everything on it.
     *
     * Wraps AdfHandler::Format, which remounts the volume afterwards. The image is rewritten
     * in memory only — the file on disk is untouched until the document is saved — so the
     * confirmation the UI puts in front of this is about the open document, not the file.
     *
     * @param label Volume label for the new filesystem (may be empty).
     * @param useOFS True for OFS, false for FFS.
     */
    void OnFormatDisk(const std::string& label, bool useOFS);

    /// True when the active document is a floppy image that is mounted and not write-protected.
    bool CanFormatDisk() const;

    /// Why Format Disk Image is disabled right now (nothing open, a hard-disk image, or
    /// write-protected); "" when CanFormatDisk() is true. Cheap enough to ask every frame.
    const char* GetFormatDiskUnavailableReason() const;

    /// The active document's display name (filename, or "Untitled" for an in-memory disk).
    std::string GetActiveDocumentDisplayName() const;

    /// The active image's volume label, or empty when there is no mounted image.
    std::string GetActiveVolumeLabel() const;

    /// True when the active image is OFS rather than FFS. Defaults to OFS with nothing mounted.
    bool IsActiveVolumeOFS() const;

    /**
     * @brief Get available disk templates.
     */
    const std::vector<std::string>& GetDiskTemplates() const { return m_diskTemplates; }

    /**
     * @brief Get the names of boot blocks in the library.
     */
    std::vector<std::string> GetBootBlockNames() const;

    /**
     * @brief Validate the currently loaded disk filesystem.
     */
    void ValidateDisk();

    /**
     * @brief Open the Boot Block Viewer window.
     */
    void ShowBootBlockView();

    /**
     * @brief Install a boot block (two sectors) onto the loaded disk's blocks 0 & 1.
     */
    void InstallBootBlock(const std::vector<uint8_t>& sector0, const std::vector<uint8_t>& sector1);

    /**
     * @brief Start previewing a boot block in the isolated preview emulator.
     */
    void StartBootBlockPreview(const std::vector<uint8_t>& sector0,
                               const std::vector<uint8_t>& sector1);
    void StopBootBlockPreview();
    void DestroyBootBlockPreview();

    /**
     * @brief Drive the preview emulator each frame (update/mute/teardown). Called from RunFrame.
     */
    void UpdateBootBlockPreview();
    void ShowAdvancedFind();
    bool* GetShowAdvancedFindPtr() { return &m_showAdvancedFind; }
    void ShowAbout();
    void ShowLog();
    void ShowPreferences();
    // Preferences, opened on the Appearance tab.
    void ShowAppearancePreferences();
    // Preferences, opened on the System Files tab (the ROM folder, finding the user's ROMs).
    void ShowSystemFilesPreferences();
    // The setup wizard (Help > Setup Wizard).
    void ShowSetupWizard();
    // On a first start, once the screen profile is chosen: the setup wizard, unless the ROM
    // folder already holds a machine's Kickstart (then nothing is asked, and never again).
    void MaybeStartOnboarding();
    // Interface zoom one stop larger (+1), smaller (-1), or back to 100% (0).
    void StepUiScale(int direction);

    /**
     * @brief Identify the currently loaded disk via ScreenScraper API.
     */
    void IdentifyDisk();

    /**
     * @brief Open the Floppy Disk I/O window on its Write tab.
     */
    void ShowWriteDisk();

    /**
     * @brief Open the Floppy Disk I/O window on its Read tab.
     */
    void ShowReadDisk();

    /// Open the Floppy Disk I/O window on its Format tab, which blanks a real disk.
    void ShowFormatFloppy();

    /// Open the Floppy Disk I/O window on its Compare tab, which checks a real disk against the
    /// open ADF.
    void ShowCompareFloppy();

    void TriggerGwInfo();
    void TriggerGwBandwidth();
    void TriggerGwRPM();

    /**
     * @brief Mount a completed GreaseWeazle floppy read as a new document. Called by
     * FloppyDiskView's read-complete callback.
     */
    void OnFloppyDiskRead(const std::vector<uint8_t>& buffer);
    void RunAdf();
    void MountDocumentToDrive(int drive, uint32_t docId);

    // Write protection. These two are the only mutators - everything else reads the
    // flag - so the padlock in the document window, the one on the drive chip and the
    // row in the emulator's Drives menu cannot drift apart.
    void SetDocumentWriteProtected(uint32_t docId, bool writeProtected);
    void SetDriveWriteProtected(int drive, bool writeProtected);
    // Pushes a document's current flag into the views that gate on it.
    void ApplyDocumentWriteProtection(DiskDocument* doc);
    /**
     * @brief Ask for a disk image and mount it in a drive as an open document.
     *
     * The Floppy Drives window's "Mount file..." goes through here rather than
     * straight into the emulator, so a file mounted from that window opens a tab
     * in Documents like any other image - browsable while the machine reads it.
     */
    void MountFileToDriveAsDocument(int drive);
    void TogglePauseEmulator();
    void ToggleTurboEmulator();
    void SetEmulatorMuted(bool mute);

    // Health of the emulator's audio path, for the AR console AUDIO command, the MCP
    // get_audio_stats tool and the --record-audio startup flag. Session-scoped, not per frame.
    // Both of these forward to m_logSnapshots; they stay on Application because main.cpp
    // calls them for the startup flag.
    core::AudioLogSnapshot MakeAudioLogSnapshot() const;

    // Starts or stops the audio debug taps, for the AR console AUDREC command, the MCP
    // start_audio_record / stop_audio_record tools and the --record-audio startup flag.
    core::AudioRecordResult ApplyAudioRecordRequest(const core::AudioRecordRequest& req);
    // Records a session to a scenario file or replays one, for the AR console SCEN command
    // and the --play-scenario startup flag.
    core::ScenarioResult ApplyScenarioRequest(const core::ScenarioRequest& req);
    // What is in the emulated drives, for a scenario manifest: file-backed slots only,
    // identified by content MD5. A slot bridged to real hardware describes nothing.
    std::vector<core::ScenarioMedia> DescribeMountedMedia() const;
    void StepFrameEmulator();
    // Step back one recorded frame (Shift+F6). Pauses first, like StepFrameEmulator.
    void StepFrameBackEmulator();
    void StepInstructionEmulator();
    // The Emulator menu's counterparts to the toolbar's Stop / Reset / Eject / Step Over
    // buttons, which were the only way to reach these actions.
    void StopEmulator();
    void ResetEmulator();
    void EjectEmulatorDisk();
    void StepOverEmulator();
    bool IsEmulatorPaused() const;
    bool IsEmulatorTurbo() const;
    bool IsEmulatorMuted() const;
    bool IsEmulatorRunning() const;

    /**
     * @brief Get the current bootblock info from the active document.
     */
    const BootBlockInfo& GetBootBlockInfo() const;

    // Edit Menu Proxy Methods (operate on active document's FileListView)
    void TriggerNewFolder();
    void TriggerDelete();
    void TriggerScrub();
    void TriggerRename();
    void TriggerPermissions();
    void TriggerRestore();
    void TriggerExport();

    bool CanCreateNewFolder() const;
    bool CanDelete() const;
    bool CanScrub() const;
    bool CanRename() const;
    bool CanSetPermissions() const;
    bool CanRestore() const;
    bool CanExport() const;

    /**
     * @brief Get information about all open documents for the View menu.
     */
    std::vector<ui::DiskDocumentInfo> GetDiskDocumentInfos() const;

    /**
     * @brief Switch the active document by its unique ID.
     * @param id The ID of the document to activate.
     */
    /// Resolve a document id for the Floppy Disk I/O window; 0 means the active document.
    const DiskDocument* FindWriteDocument(uint32_t id) const;

    void SwitchActiveDocumentById(uint32_t id);

    /**
     * @brief Get the active document (may be nullptr).
     */
    DiskDocument* GetActiveDocument() const { return m_activeDocument; }

    /**
     * @brief Get the number of open documents.
     */
    size_t GetDocumentCount() const { return m_documents.size(); }

private:
    void SetupSharedCallbacks();
    // Per-collaborator wiring, called by SetupSharedCallbacks() in this order.
    void WireFrameTimeline();
    void WireHexEditorCallbacks();
    void WireFindNavigationCallbacks();
    void WireDebuggerCallbacks();
    void WireEmulatorOverlayCallbacks();
    void WireAddressContexts();
    void WireFindDataCallbacks();
    void WireBlockGridCallbacks();
    void WireMediaCallbacks();

    // RunFrame phases, executed in this order each frame (the fullscreen early-out sits
    // between the first two).
    void RunFramePumpIntegrations();
    void RunFrameDrawWorkspace();
    void RunFrameDrawDocuments();
    void RunFrameDrawSharedViews();
    void RunFrameHarvestTelemetry();
    void RunFrameFinishFrame();
    void SetupDocumentCallbacks(DiskDocument* doc);
    void LoadFonts(ImGuiIO& io, float scale = 1.0f);
    void UpdateDocumentViews(DiskDocument* doc, bool preserveNavigation = false);
    void PopulateDataTypes(DiskDocument* doc, FileNode& node);
    void ScanDiskTemplates();
    void SwitchActiveDocument(DiskDocument* doc);
    void CloseDocument(DiskDocument* doc);
    void QueueCloseDocument(DiskDocument* doc);
    void LoadLogoTexture();
    void ProcessDiskWriteEvents();

    // Arms/disarms the disk DMA breakpoint through BreakpointsView, so it shares the
    // window, session persistence and re-arm-on-reset with the other special breaks.
    // access < 0 disarms. Returns false when the core does not support it.
    bool ArmDiskBreak(int access, int cyl, int side, int sector, uint32_t addrLo,
                      uint32_t addrHi) const;

    // Multi-document state
    std::vector<std::unique_ptr<DiskDocument>> m_documents;
    std::vector<DiskDocument*> m_deferredCloseDocuments;
    // Documents created by the "Run" callback, which fires while RunFrame iterates m_documents;
    // inserting there would reallocate the vector under the loop, so they queue here and RunFrame
    // finishes the open (insert, activate, boot the ADF) after the loop.
    struct DeferredRunDocument
    {
        std::unique_ptr<DiskDocument> doc;
        std::vector<uint8_t> adfBytes;
        std::string filename;
    };
    std::vector<DeferredRunDocument> m_deferredRunDocuments;

    // Per-frame scratch buffers (were function-local statics — process-global state that
    // would break if Application were ever instantiated twice, e.g. in tests).
    std::vector<AmigaEmulator::CopperExecEvent> m_copperEventScratch;
    std::vector<AmigaEmulator::IdleEvent> m_idleEventScratch;
    std::vector<uint8_t> m_trackScratch;
    // Identifiers for closed documents whose identification is still in progress; cleaned up once
    // done
    std::vector<std::unique_ptr<core::DiskIdentifier>> m_orphanedIdentifiers;
    uint32_t m_nextDocId = 1;
    DiskDocument* m_activeDocument = nullptr;
    // The compile-and-run target: its debug info, its hunk map, and whether it has loaded yet.
    // Init()ed in the constructor body.
    core::CompiledProgram m_compiledProgram;
    // Used to detect when identification transitions to Done so the TOSEC Browser can refresh
    DiskDocument* m_prevTrackedDoc = nullptr;
    core::IdentifyState m_prevIdentifyState = core::IdentifyState::Idle;

    // Search context
    SearchContext m_searchContext = SearchContext::Disk;
    std::unique_ptr<core::MemorySearcher> m_memorySearcher;
    // Memory-bank-layout signature the MemorySearcher regions were last built from. Lets
    // UpdateMemorySearcherRegions() skip the per-frame rebuild + sort while the layout is
    // unchanged. 0 means "not running" (regions cleared); UINT64_MAX is the initial never-built
    // sentinel.
    uint64_t m_memSearcherRegionSig = UINT64_MAX;

    // Global state (shared across documents)
    UserPreferences m_userPrefs;
    WorkspaceManager m_workspaceManager;
    ui::WorkspaceTransitionManager m_transitionManager;
    DefinitionManager m_definitionManager;
    DataTypeManager m_dataTypeManager;
    // Opens the per-file viewer windows a document can spawn. Init()ed in the constructor
    // body.
    ui::ViewerLauncher m_viewerLauncher;

    // Copying and moving files between two open documents. Init()ed in the constructor body.
    core::DocumentTransfer m_documentTransfer;
    AudioEngine m_audioEngine;
    ui::FontManager m_fontManager;
    // The style before any DPI or interface-zoom scaling; every zoom change rescales from it, as
    // ScaleAllSizes compounds and cannot be undone.
    ImGuiStyle m_baseStyle;
    float m_dpiStyleScale = 1.0f;
    std::optional<UserPreferences::FontPrefs> m_appliedFonts;
    // A screen profile switch restored that profile's own column widths: the font change it also
    // brings must not auto-fit them away (see ApplyPendingFontSettings).
    bool m_skipFontAutofit = false;
    std::function<void()> m_onScreenProfileChanged;
    ui::TextureManager m_textureManager;
    std::vector<std::string> m_diskTemplates;
    BrainFile m_brainFile;
    BootBlockLibrary m_bootBlockLibrary;

    // Shared views (reflect the active document)
    std::unique_ptr<ui::BlockEditorView> m_hexEditorView;
    std::unique_ptr<ui::BitmapView> m_bitmapView;
    std::unique_ptr<ui::BlockMapView> m_blockMapView;
    std::unique_ptr<ui::NDOSView> m_ndosView;
    std::unique_ptr<ui::ChangeMapView> m_changeMapView;
    std::unique_ptr<ui::ValidationView> m_validationView;
    std::unique_ptr<ui::BootBlockView> m_bootBlockView;
    std::unique_ptr<ui::AboutView> m_aboutView;
    std::unique_ptr<ui::PreferencesView> m_preferencesView;
    std::unique_ptr<ui::OnboardingView> m_onboardingView;
    std::unique_ptr<ui::IdentifyView> m_identifyView;
    std::unique_ptr<ui::FloppyDiskView> m_floppyDiskView;
    std::unique_ptr<ui::FloppyDrivesView> m_floppyDrivesView;
    std::unique_ptr<ui::EmulatorView> m_emulatorView;

    // Isolated second emulator instance backing the Boot Block Viewer's live-preview tab.
    // Created lazily on first preview; torn down when the Boot Block Viewer closes. Owns the
    // machine mechanics; UpdateBootBlockPreview() below supplies the view-facing policy.
    core::BootBlockPreview m_bootBlockPreview;
    // Rate-limiter for UpdateDocumentViews() calls triggered by disk write events
    std::chrono::steady_clock::time_point m_lastWriteRedirectUpdate{};
    // ORDER MATTERS: m_memoryMapView and m_registerOpsView (below) hold raw pointers into
    // m_debuggerView's AddressActionContext, so they must be DECLARED AFTER it (members are
    // destroyed in reverse declaration order). Moving m_debuggerView below either of them
    // silently creates a dangling pointer used every frame.
    std::unique_ptr<ui::DebuggerView> m_debuggerView;
    std::unique_ptr<ui::MemoryMapView> m_memoryMapView;
    std::unique_ptr<core::BlitProfiler> m_blitProfiler;
    std::unique_ptr<core::RegWriteProfiler> m_regWriteProfiler;
    std::unique_ptr<core::DiskProfiler> m_diskProfiler;
    std::unique_ptr<ui::BlitterView> m_blitterView;
    std::unique_ptr<ui::RegisterOpsView> m_registerOpsView;
    std::unique_ptr<ui::DiskIoView> m_diskIoView;

    // Exocet: the active document's sync pack, its kit, edits, push and
    // proofing, and the window over them. The session is fed the document and the block every
    // frame (UpdateExocet); the MCP tools use the same session.
    std::unique_ptr<core::ExocetSession> m_exocet;
    std::unique_ptr<ui::ExocetView> m_exocetView;
    std::unique_ptr<ui::ExocetControlView> m_exocetControlView;
    std::unique_ptr<ui::ExocetMemoryView> m_exocetMemoryView;
    std::unique_ptr<ui::ExocetDiskMapView> m_exocetDiskMapView;
    std::unique_ptr<ui::ExocetMusicView> m_exocetMusicView;
    // what the Music window plays (ExocetSession hooks.listen): one at a time, stopped when closed
    std::unique_ptr<ui::AudioPlayer> m_exocetListen;
    void SetupExocet();
    void UpdateExocet();
    // What ships beside the executable (cmake/ExocetBundle.cmake), for when the preferences name
    // no Python or tools directory: found once, at startup, where the working directory is it.
    std::string m_exocetBundledPython, m_exocetBundledTools;

    // Reads the three profilers above plus the emulator to build the AR console / MCP log
    // snapshots. Init()ed in the constructor body, once its sources exist.
    core::LogSnapshots m_logSnapshots;

    // Stop-at-entry handshake and reverse-execution position for the GDB server. Init()ed in
    // the constructor body; SetupGdbServer() below binds its hooks to this.
    core::DebugSession m_debugSession;

    // The two overlays drawn over the emulator picture. Both are fed from the emulator's own
    // records rather than by the display window, so they are wired here; Init()ed in the
    // constructor body.
    ui::CopperRasterController m_copperRasterCtl;
    ui::BlitOverlayController m_blitOverlay;
    std::unique_ptr<ui::BlitterMintermView> m_blitterMintermView;

    // Core-run serial last forwarded to the debugger. This advances on frames, instruction steps,
    // and partial breakpoint returns, while remaining fixed across redundant ImGui redraws.
    uint64_t m_lastDebuggerRun = UINT64_MAX;

    // Pushes the debugger's memory locks into the core's protection table when they change.
    // Revision-gated rather than per-frame: the table only moves when the user adds, removes,
    // enables or re-resolves a lock, which is rare compared to 50 frames a second.
    void SyncMemoryProtections();
    // Looks for a trainer file matching the active document once its MD5 is known. Reports what
    // it found and never enables anything - see DebuggerTrainerView for why.
    void MaybeDiscoverTrainer();
    uint64_t m_lastLockRevision = 0;
    // Resolve a blit reference (lRef / startCycle / scrub position) to a record and push it to
    // the Blitter Minterm window when its Sync toggle is on. UINT32_MAX = no selection (ignored).
    void SyncMintermToBlit(uint32_t ref);
    std::unique_ptr<ui::DmaStatsView> m_dmaStatsView;
    std::unique_ptr<ui::LogView> m_logView;
    std::unique_ptr<ui::AdvancedFindView> m_advancedFindView;
    bool m_showAdvancedFind = false;
    std::unique_ptr<ui::TosecBrowserView> m_tosecBrowserView;

    // Permanent Documents panel — always present, hosts FileListViews by default
    static constexpr ImGuiID kDocumentsPanelDockId = 0xAD001234;
    std::unique_ptr<ui::PanelView> m_documentsPanel;

    // Dynamic panel windows
    ui::PanelManager m_panelManager;

    // Block View context menus
    void OnClearBlock(uint32_t block);
    void OnSaveBlocks(const std::set<uint32_t>& blocks);
    void OnInsertBinary(uint32_t startBlock);

    // Font state
    std::vector<std::string> m_availableFonts;

    // Callbacks
    std::function<void(const std::string&)> m_onOpenFileRequested;

    // Deferred update flag
    bool m_pendingViewUpdate = false;

    // Saving and restoring a workspace: window visibility, panels, and the ImGui dock tree.
    // Init()ed in the constructor body, after m_documentsPanel exists.
    ui::WorkspaceLayout m_workspaceLayout;

    // After restoring a .peek, force the restored documents into the Documents panel for this
    // many frames (the saved-layout reload otherwise leaves them floating). Counts down each frame.
    int m_restoreDockFrames = 0;

    // Last frame's emulator fullscreen state, to catch the frame the workspace comes back.
    bool m_wasFullscreen = false;

    float m_uiScale = 1.0f;

    // Focused-window persistence: restore the last-focused window on launch (so an arbitrary
    // window doesn't grab focus), and track the live focus so it can be saved at exit.
    void UpdateFocusedWindowTracking();
    std::string m_pendingFocusWindow; // window to refocus on startup; cleared once applied
    int m_focusRestoreDelay = 0;      // frames to wait before applying (let the layout settle)
    std::string m_lastFocusedWindow;  // ImGui name of the currently focused top-level window

    // Toolbar logo texture
    uint32_t m_logoTexture = 0; // GLuint
    float m_logoAspect = 1.0f;  // width / height

    // Error modal state
    bool m_showErrorModal = false;
    std::string m_errorMessage;
    void DrawErrorModal();

    // Virus alert modal state
    bool m_showVirusAlert = false;
    void DrawVirusAlertModal();

    // Unified unsaved-changes confirmation queue (Close Document, Close All, Exit)
    ui::UnsavedChangesDialog m_unsavedChanges;
    bool m_wantsToExit = false;

    // Local HTTP server for Peekaboo Chrome extension
    PeekabooHttpServer m_httpServer{
        GetLaunchOptions().httpPort.value_or(PeekabooHttpServer::kPort)};

    // Local MCP server for Claude Desktop integration (Claude reads/annotates emulator state).
    core::McpServer m_mcpServer;
    // GDB remote-serial-protocol server (opt-in) -- see SetupGdbServer().
    core::GdbServer m_gdbServer;
    // Deferred capture of Peekaboo's own framebuffer, serviced from main.cpp at end-of-frame.
    core::AppCapture m_appCapture;
    // Synthetic input into Peekaboo's own ImGui, applied from main.cpp before ImGui::NewFrame.
    ui::UiInputInjector m_uiInput;
    // The main viewport's dockspace id, recorded when RunFrame submits it. DockSpaceOverViewport
    // derives it from the viewport, so it is not knowable until then - and the UI tools need it as
    // the root of the dock tree.
    ImGuiID m_mainDockId = 0;
    // Everything the UI introspection/mutation helpers need about the current window registry.
    ui::UiSources MakeUiSources();
    // Named machine-state snapshots used by the save_state / load_state MCP tools.
    core::EmulatorStateStore m_stateStore;
    // Named raw memory snapshots for the snapshot_memory / diff_snapshots tools.
    core::MemorySnapshotStore m_memorySnapshots;
    // Rolling instruction history behind get_cpu_trace / find_in_trace / library-call tracing.
    // Off by default; configure_cpu_trace_history turns it on.
    core::CpuTraceHistory m_cpuTraceHistory;
    // The per-frame save-state timeline: recording, seeking, rewind, A-B loop and playback.
    // Extracted from this class. Wired in
    // SetupSharedCallbacks(); everything above forwards to it.
    core::FrameTimeline m_timeline;
    // Recording is armed from the console and fed by the frame-complete fan-out; it holds
    // no machine of its own.
    core::ScenarioRecorder m_scenarioRecorder;
    std::string m_scenarioPath; // where SCEN STOP will write
    // LVO tables + argument metadata for trace_library_calls and add_dos_breakpoint.
    core::LibraryCallTracer m_libraryTracer;
    // Library-entry breakpoints with a string predicate (add_dos_breakpoint).
    core::DosBreakpoints m_dosBreakpoints;
    // Accessors onto emulated memory in the shape ExecIntrospect/SegListResolver expect.
    core::exec::Readers MakeExecReaders();

    // The debug info to use for the running program: the compiled target's when one is loaded,
    // otherwise the active document's sibling .elf. Supplies the document half; CompiledProgram
    // decides which wins.
    core::ActiveDebugInfo GetActiveDebugInfo() const;

    // Called from the debug-break filter. Returns true when the stop belonged to a DOS
    // breakpoint whose name predicate did not match, so execution should silently resume.
    bool ConsumeDosBreak();
    std::unique_ptr<core::McpToolHandler> m_mcpHandler;
    std::unique_ptr<ui::McpActivityView> m_mcpActivityView;
    // Builds the MCP tool handler + activity view and starts the server if enabled in prefs.
    void SetupMcpServer();
    // Wire GdbTargetOps onto the emulator and start the server if the pref is on.
    // Why the machine stopped, for the GDB stop reply.
    ui::BreakpointsView* BreakpointsViewPtr();
    void SetupGdbServer();
    // Watches for the launched program's seglist to appear and halts it at its entry point.
    // Called once per frame; a no-op unless a stop-on-entry launch is in flight.
    // Generates a random token for gating the local MCP HTTP API.
    static std::string GenerateMcpToken();

    // Unified background download manager shared by all remote sources.
    core::DownloadManager m_downloadManager;
    void OnPeekabooLoadRequested(const core::PeekabooLoadEvent& ev);
    void OnPeekabooRunRequested(const PeekabooRunRequest& request);

    // Poll in-flight download documents; convert + mount completed ones in place.
    void PollDownloads();

    // Which source format ConvertToAdfBytes detected for a given buffer.
    enum class AdfSourceKind { Adf, Dms, Zip, Executable, Ipf, ExtAdf, Scp };

    // Detection result reported back by ConvertToAdfBytes, used by callers that
    // need to track the on-disk provenance (e.g. local opens populating the DMS
    // viewer or remembering the originating ZIP entry).
    struct AdfDecodeInfo
    {
        AdfSourceKind kind = AdfSourceKind::Adf;
        // Set whenever the bytes came out of a ZIP. The kind then describes the
        // *member's* own format, so a zipped IPF reports Ipf, not Zip.
        std::string zipEntryName;
        std::vector<core::IpfTrackInfo> ipfTracks; // set when kind == Ipf, ExtAdf or Scp
        // Original, undecoded image bytes for flux/track-level formats the emulator
        // can run natively (IPF, UAE-1ADF, SCP). The decoded ADF is fine for browsing
        // but cannot carry copy protection, so the emulator needs the raw image
        // instead.
        std::vector<uint8_t> rawImageBytes; // set when kind == Ipf, ExtAdf or Scp
    };

    // Convert raw downloaded/in-memory bytes into mountable ADF bytes, handling
    // HUNK executables, DMS, ZIP, and raw ADF. Returns nullopt on failure. When
    // outInfo is non-null it receives the detected source kind (set as soon as the
    // format is recognised, even if the subsequent decode fails). A ZIP re-enters
    // this function with its member; zipDepth caps that at one level.
    std::optional<std::vector<uint8_t>>
    ConvertToAdfBytes(std::vector<uint8_t> bytes, const std::string& filename, std::string fileType,
                      bool& outIsExecutable, AdfDecodeInfo* outInfo = nullptr, int zipDepth = 0);

    // Core loader shared by LoadAdf (interactive) and LoadDocumentExternal
    // (console/MCP). Handles the "replace the active tab" step — closing the
    // current document first, prompting to save if it is dirty — then hands off
    // to LoadAdfInternal to actually mount the new document.
    bool LoadAdfImpl(const std::string& path, bool force, bool interactive, bool reloadIfOpen);
    // Reads, decodes and mounts the file at path as a brand-new document, then
    // makes it active. Assumes any document being replaced has already been dealt
    // with by the caller.
    bool LoadAdfInternal(const std::string& path);

    // Shared post-mount document setup (services, searcher, identifier, bootblock).
    // Used by LoadAdf, LoadAdfFromTosec, and the download-completion path.
    void FinalizeMountedDocument(DiskDocument* doc);

    // What the disk identifier should hash for this document: null when it is backed
    // by a real file and can be hashed from disk, otherwise the bytes to hash in its
    // place. Must be attached wherever an identifier is created - the automatic TOSEC
    // pass on mount hashes long before the user opens the Disk Identifier window.
    static std::shared_ptr<const std::vector<uint8_t>> MakeIdentifySource(const DiskDocument* doc);

    // Creates a new in-memory, unsaved document from raw ADF bytes (no backing file).
    // The tab shows displayName and is marked modified; Save redirects to Save As.
    // Returns the registered, active document, or nullptr on mount failure.
    DiskDocument* CreateInMemoryDiskDocument(std::vector<uint8_t> adfBytes,
                                             const std::string& displayName);

    // Reads a disk template image from images/<templateName> into memory.
    // Surfaces an error modal and returns nullopt if the template is missing.
    std::optional<std::vector<uint8_t>> ReadDiskTemplate(const std::string& templateName);

    // .peek session helpers
    // Gathers the current session (documents, snapshot, breakpoints, view state, notes).
    core::PeekSession BuildPeekSession();
    // Restores a parsed session: recreates documents, reloads the snapshot-owner disk,
    // unserializes the machine state, re-injects breakpoints and restores the view state.
    // Returns false only on a fatal error; a failed machine-snapshot restore is non-fatal
    // (the disk boots fresh and everything else is still restored).
    bool RestorePeekSession(core::PeekSession& session);
    // Creates an in-memory document from captured ADF/IPF bytes (mirrors LoadAdfFromTosec)
    // and returns it; the document keeps the id assigned here.
    DiskDocument* CreateDocumentFromPeek(const core::PeekDocument& pd);
    bool
    OpenHostDirectoryDocument(const std::string& path, bool readOnly = true,
                              std::optional<core::HostMountDevice> assignedDevice = std::nullopt,
                              const std::string& volumeLabel = {});
    void UpdateHostDirectoryDocument(DiskDocument* doc);
    bool RecoverRestoredHostMount(DiskDocument* doc, std::string& outError);
    // Hard-disk images as documents: opened whole into memory, one partition shown at a time.
    bool LoadHardfileDocument(std::unique_ptr<DiskDocument> doc, const std::string& path,
                              bool compressed);
    void RefreshPartitionChoices(DiskDocument* doc);
    void SelectDocumentPartition(DiskDocument* doc, int index);
    void UpdateHardfileDocumentRuntime(DiskDocument* doc, core::DiskInfo& diskInfo);
    void ConfigureDocumentSearch(DiskDocument* doc);
    void FinalizeDocumentIdentity(DiskDocument* doc);
    // Builds and writes the session to path, updating m_currentSessionPath on success.
    bool WriteSessionTo(const std::string& path);

    // Free-text notes for the current session, persisted in the .peek file.
    std::string m_sessionNotes;
    // Path of the .peek the session was last saved to / opened from ("" = unsaved); drives
    // the quick "Save Session" action.
    std::string m_currentSessionPath;

    // Bring the application window to the foreground (used when a remote load arrives).
    void FocusWindow();

    // Window management
    GLFWwindow* m_window = nullptr;
    int m_windowX = 0;
    int m_windowY = 0;
    int m_windowWidth = 0;
    int m_windowHeight = 0;

    // Empty bootblock info for when no document is active
    static const BootBlockInfo s_emptyBootBlockInfo;
};

} // namespace core
} // namespace adm
