#pragma once

#include <imgui.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "../../core/FfmpegRecorder.h"
#include "imgui_hex.h"

#include "../../core/AmigaEmulator.h"
#include "../../core/DiskDocumentInfo.h"
#include "../../core/DiskSearcher.h"
#include "../../core/GifRecorder.h"
#include "../../core/HardfileInspector.h"
#include "../../core/RasterFrameMap.h"
#include "../../core/WhdloadLaunch.h"
#include "../EmulatorFilter.h"

// Forward declarations to avoid header includes
struct ImGuiIO;
struct GLFWwindow;

namespace adm::core {
class UserPreferences;
}

namespace adm::ui {

class EmulatorFilter;

class EmulatorView
{
public:
    EmulatorView();
    ~EmulatorView();

    void Draw();

    // Build GL resources that must be created up front (the display-filter shader/FBO).
    // Call once at startup, while the GL context is current and no ImGui frame is in flight.
    void InitGraphics();

    // Core management
    bool LoadCore(const std::string& corePath);
    bool IsCoreLoaded() const;
    void UnloadCore();

    // Game/Disk management
    bool StartEmpty();
    // fileExt selects the temp-file extension PuAE sees ("adf" for decoded images,
    // "ipf" or "scp" to hand a raw preservation image to the core's native CAPS /
    // flux readers).
    bool LoadADF(const std::vector<uint8_t>& adfData, uint32_t documentId = 0,
                 const std::string& fileExt = "adf");
    bool RunExecutable(const std::vector<uint8_t>& exeData, const std::string& name,
                       uint32_t documentId = 0);

    // Window management
    void Open() { m_isOpen = true; }
    void Restore()
    {
        m_isOpen = true;
        m_noFocusOnAppearing = true;
    }
    void Close() { m_isOpen = false; }
    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }
    bool IsRunning() const;
    bool IsPaused() const { return m_isPaused; }
    // True while the current free-run is a transient step-over (run-to-return), as
    // opposed to a sustained Play/continue. Set only by StepOver and self-cleared the
    // moment execution re-pauses, so it distinguishes the two indistinguishable
    // m_isPaused==false states for views that treat them differently.
    bool IsStepOverRun() const { return m_runIsStepOver; }
    bool IsTurbo() const { return m_isTurbo; }
    // True while any slot is backed by live FloppyBridge hardware. Turbo is refused
    // in that state: the bridge has to be serviced in real time to stay in step with
    // the platter's actual rotation, so running the CPU 10x desyncs the drive.
    bool AnyDriveBridged() const
    {
        for (const auto& d : m_drives)
            if (d.isBridged)
                return true;
        return false;
    }
    void TogglePause();
    void ToggleTurbo();
    void StepFrame();
    void EndFrame();
    void StepInstruction();
    // Free-run until the emulated frame counter reaches targetFrame, then auto-pause
    // (and fire the debug-break notification, like a breakpoint stop). When turbo is
    // true, warp mode is enabled for the run and restored when the target is reached.
    // Returns false if not running or targetFrame is already at/behind the current
    // frame. A breakpoint hit before the target cancels the pending run.
    bool RunUntilFrame(uint64_t targetFrame, bool turbo);
    // True while a RunUntilFrame target is pending.
    bool IsRunUntilFramePending() const { return m_runUntilFrame.has_value(); }
    // Returns true if execution was resumed asynchronously (a subroutine call is
    // being run to its return address — the caller should report state on the next
    // debug break). Returns false if it completed synchronously as a single step.
    bool StepOver();
    void Stop();
    void Reset();
    void Eject();
    void SetMuted(bool mute);
    bool IsMuted() const { return m_isMuted; }
    void ReleaseMouse();

    // Controller type
    void SetControllerType(const std::string& name, const std::string& type);
    std::vector<adm::AmigaEmulator::ConnectedController> GetConnectedControllers() const;
    void RefreshConnectedControllers();

    // Keyboard joystick: cursor keys + A/S act as a Standard Joystick
    void SetKeyboardJoystickEnabled(bool enabled);
    bool IsKeyboardJoystickEnabled() const { return m_keyboardJoystickEnabled; }

    // Model configuration
    void SetModel(adm::AmigaModel model);
    std::string GetCurrentModelId() const;
    // Empty when the preset can boot; otherwise why not (a missing CD32/CDTV ROM).
    std::string GetModelUnavailableReason(adm::AmigaModel model) const;
    // Why the running machine is on the core's AROS Kickstart - its own is not in the ROM folder -
    // or empty. Shown above the picture.
    std::string MissingKickstartNotice();
    // Where that notice's Find ROMs button goes (the setup wizard)
    void SetOnFindSystemFiles(std::function<void()> cb) { m_onFindSystemFiles = std::move(cb); }
    // The folder the core loads Kickstarts from. Out of line: a caller that has seen <windows.h>
    // cannot name AmigaEmulator::GetSystemDirectory (it becomes GetSystemDirectoryA).
    std::string GetRomDirectory() const;
    // A core option below the model preset (the chipset revisions), saved with the other
    // emulator settings. Takes effect at the next power-on.
    void SetMachineSetting(const std::string& key, const std::string& value);
    // Cold restart of a running machine, so options the core only reads at retro_init (the
    // model, the chipset revisions) take effect. The same power cycle SetModel performs.
    bool RestartMachine(std::string& outError);
    // Serial and parallel ports: applied to the running machine at its next frame, and saved.
    void SetSerialPort(const core::SerialPortTarget& target);
    void SetParallelOutput(const std::filesystem::path& path);
    void SetMidiDevices(int outDevice, int inDevice);

    // UAE config file
    bool LoadUaeConfig(const std::string& configPath);
    bool RunHostExecutable(const std::string& programPath, std::string& outError);
    bool RunExecutable(const std::vector<uint8_t>& exeData, const std::string& filename,
                       std::string& outError);
    void ClearUaeConfig();
    bool HasUaeConfig() const;
    std::string GetUaeConfigName() const;

    // Named PEEKaboo configurations persist the complete machine setup, including
    // core-variable overrides, floppy state/media, and host-directory drives.
    std::vector<std::filesystem::path> GetCustomConfigurations() const;
    std::string GetCurrentCustomConfigurationName() const;
    bool SaveCustomConfiguration(const std::string& name, std::string& outError);
    // Overwrite an existing configuration with the machine setup that is live right now.
    bool UpdateCustomConfiguration(const std::string& configPath, std::string& outError);
    bool LoadCustomConfiguration(const std::string& configPath, std::string& outError);
    bool DeleteCustomConfiguration(const std::string& configPath, std::string& outError);
    bool RenameCustomConfiguration(const std::string& configPath, const std::string& newName,
                                   std::string& outError);

    void SetConfiguredHostMounts(const std::array<core::HostDirectoryMount, 2>& mounts,
                                 bool persist = true);
    const std::array<core::HostDirectoryMount, 2>& GetConfiguredHostMounts() const;
    void SetHardDriveControllerRoms(const core::HardDriveControllerRoms& roms, bool persist = true);
    bool ApplyHostMountsAndRestart(std::string& outError);
    bool StartHostMountProfile(const core::EmulatorLaunchProfile& profile, std::string& outError);
    // Boots a WHDLoad archive, game directory, .slave or .info from Peekaboo's generated config:
    // the core's embedded WHDLoad on WHDLoad:, the game on DH0: (transiently - the saved DH0 is
    // untouched), saves on WHDSaves:. Apply and Restart goes back to the saved drives.
    bool LaunchWhdload(const std::filesystem::path& input, std::string& outError);
    // Boots a hard-disk image document from DH0 for this run only: read-only, bootable, ahead of
    // whatever DH1 holds, with the saved DH0 configuration neither replaced nor inherited from.
    // Apply and Restart goes back to the saved drives, as after a WHDLoad launch.
    bool RunHardfile(const std::filesystem::path& image, std::string& outError);
    void PickWhdloadArchive();
    // Asks for a host file and copies it onto whichever DH directory can take it. The host half
    // of host-guest integration: AmigaDOS sees it the moment the copy lands, so nothing has to
    // be restarted.
    void PickFileToCopyToGuest();
    // Empty when a file could be copied right now, otherwise why it could not - the tooltip on
    // the disabled menu item.
    std::string GetCopyToGuestUnavailableReason() const;
    void DrawWhdloadSettingsMenu();
    void DrawCdDriveMenu();
    void DrawCdActivity();
    // The CD Drive window: which disc CD0 holds, what the machine has, insert/eject/recents.
    void DrawCdDriveWindow();
    core::WhdloadSettings GetWhdloadSettings() const;
    void SetWhdloadSettings(const core::WhdloadSettings& settings);
    // The game DH0 is running, when the effective profile is a WHDLoad launch.
    std::optional<core::HostDirectoryMount> GetActiveWhdloadGame() const;
    // Where WHDSaves: lives. Persistent (next to the preferences), unlike the core's per-session
    // save directory, so saved games survive a restart.
    static std::filesystem::path WhdloadSavesDirectory();

    // ---- CD0 ----
    struct CdDriveStatus
    {
        std::filesystem::path source; // what was picked: an image, or a .7z/.zip of one
        std::filesystem::path image;  // the image the machine is given
        std::string inMachine;        // what the running machine has in CD0 ("" = empty)
        bool hasDrive = false;        // the running machine has a CD drive
        bool restartPending = false;  // a disc is chosen but the machine has no drive until restart
        bool extracting = false;
        std::string lastError;
        int led = 0;
    };
    // Puts a disc in CD0: straight away when the machine has a drive, otherwise at the next
    // power-on. An archive is extracted into the cache first, in the background.
    bool InsertCd(const std::filesystem::path& input, std::string& outError);
    void EjectCd();
    void PickCdImage();
    CdDriveStatus GetCdStatus() const;
    // Re-selects the last disc at startup without extracting anything.
    void RestoreCdImage(const std::string& source);
    // Persistent cache the disc archives unpack into, beside the preferences.
    static std::filesystem::path CdCacheDirectory();
    bool HasPendingHostMountRestart() const;
    void OpenHardDriveEditor() { m_hardDriveEditorOpen = true; }
    bool* GetHardDriveEditorOpenPtr() { return &m_hardDriveEditorOpen; }
    void OpenCdDriveWindow() { m_cdDriveWindowOpen = true; }
    bool* GetCdDriveWindowOpenPtr() { return &m_cdDriveWindowOpen; }
    void PickHostDirectory(int deviceIndex);
    void MountHostDirectory(int deviceIndex, const std::string& path);
    // Point DH0/DH1 at a hardfile image and open the Hard Drives window on that slot. The change is
    // pending until Apply and Restart, like every other mount change.
    void PickHardfile(int deviceIndex);
    void MountHardfile(int deviceIndex, const std::string& path);
    void DrawHardDriveEditor(bool embedded = false);
    void DrawHardfilePartitionTable(int slot, const core::HostDirectoryMount& mount);
    // Also takes the saved volume up
    void SetUserPreferences(core::UserPreferences* prefs);
    void SetOnOpenHostDirectory(std::function<void(const core::HostDirectoryMount&)> callback)
    {
        m_onOpenHostDirectory = std::move(callback);
    }
    void SetOnOpenHardfileDocument(std::function<void(const std::filesystem::path&)> callback)
    {
        m_onOpenHardfileDocument = std::move(callback);
    }

    // Drive management
    struct DriveState
    {
        bool enabled = false;
        bool hasDisk = false;
        uint32_t documentId = 0; // 0 if not from a managed document
        std::string diskName;
        std::string sourcePath;
        // "adf", "ipf" or "scp" — selects the boot temp-file extension
        std::string fileExt = "adf";
        std::vector<uint8_t> pendingData; // ADF data for deferred insertion
        // Backed by a real, spinning floppy over FloppyBridge rather than an image.
        // A bridged slot has no documentId and no data, so nothing else in this struct
        // distinguishes it from an empty file-backed drive - hence this flag.
        // bridgeHardware/bridgeComPort are for display only; the authority on whether the
        // device actually opened is AmigaEmulator::GetBridgeStatus().
        bool isBridged = false;
        std::string bridgeHardware;
        std::string bridgeComPort;
        // The request behind isBridged, kept so it can be (re)applied to the core. A
        // slot can be assigned to live hardware while the machine is stopped, and the
        // assignment survives Stop(): every start path ends in FlushPendingDisks(),
        // which hands this to AmigaEmulator::AttachBridgeDrive() alongside the queued
        // images. Set whenever isBridged is; the two are cleared together.
        std::optional<adm::AmigaEmulator::BridgeConfig> bridgeConfig;
        // Write protection for whatever is in the slot. A slot needs its own copy
        // because MountFromFileDialog() inserts raw bytes with no documentId; when
        // there is a document, Application keeps the two equal.
        bool writeProtected = true;
    };

    void MountDisk(int drive, const std::vector<uint8_t>& data, const std::string& name,
                   uint32_t documentId = 0, const std::string& fileExt = "adf",
                   bool writeProtected = true);

    // Write-protects the slot, in the core as well when it is running. With a
    // document mounted, Application keeps this equal to the document's own flag.
    void SetDriveWriteProtect(int drive, bool writeProtected);
    // Ask for an ADF (or a ZIP holding one) with the native file dialog and mount it.
    // Shared by the emulator's Drives menu and the Floppy Drives window; blocks while
    // the dialog is up.
    void MountFromFileDialog(int drive);

    // Power on the machine using the current drive configuration: boot DF0's disk if
    // present (the Amiga boot drive), otherwise start to Kickstart. FlushPendingDisks
    // then populates the remaining drives. Returns false if the core could not start.
    bool PowerOn();
    void EjectDrive(int drive);
    void SetDriveEnabled(int drive, bool enabled);
    // Attach or detach a live FloppyBridge drive. Both the emulator and this view's
    // drive model have to move together, so every caller goes through here rather
    // than poking AmigaEmulator directly - the console LIVE command and the Live
    // Floppy window both do, which is what keeps the turbo/save-state guards and the
    // session record honest whichever route was used.
    // Attach returns false if the request was rejected outright; true means queued,
    // since the core opens the device on its next emulated vertical blank. Any number
    // of slots can be live at once - each is its own device on its own port.
    //
    // The machine does not have to be running. With no core loaded the assignment is
    // recorded here and applied by FlushPendingDisks() when it starts; IsBridgePending()
    // tells the two states apart. The assignment also survives Stop(), so a drive
    // attached once stays attached across start/stop until it is detached.
    bool AttachBridgeDrive(int drive, int driverIndex, const std::string& comPort, int bridgeMode,
                           int bridgeDensity, int driveCable, bool autoCache, bool smartSpeed,
                           bool allowWrites = false);
    void DetachBridgeDrive(int drive);
    // True while a slot is assigned to live hardware but the request has not reached
    // the core - the machine is stopped, or was stopped since the attach.
    bool IsBridgePending(int drive) const;

    // Bookkeeping only, for the paths that already know the emulator side is done
    // (or gone). Attaching also drops any mounted image, because one slot cannot be
    // backed by both a spinning platter and a file.
    void SetDriveBridged(int drive, bool bridged, const std::string& hardware = {},
                         const std::string& comPort = {});
    const DriveState& GetDriveState(int drive) const;
    void SetDriveDiskName(int drive, const std::string& name);
    void FlushPendingDisks();

    /**
     * @brief Check if a document ID is currently loaded in any drive.
     */
    bool IsDocumentLoaded(uint32_t documentId) const;

    // Fetch fresh memory regions directly from the emulator (does not require Memory tab open)
    std::vector<adm::AmigaEmulator::MemoryRegion> GetFreshMemoryRegions() const
    {
        if (!IsRunning())
            return {};
        return m_emulator->GetMemoryRegions();
    }

    // Allocation-free signature of the current memory bank layout. Changes only when banks are
    // added/removed/resized; 0 when not running. Lets callers skip rebuilding region-derived data
    // every frame.
    uint64_t GetMemoryRegionsSignature() const
    {
        if (!IsRunning())
            return 0;
        return m_emulator->GetMemoryRegionsSignature();
    }

    // Monotonic emulated-frame counter, advancing only when retro_run() crosses a real frame
    // boundary. Returns 0 when not running. Debugger views cache expensive per-frame work
    // (decodes, texture uploads, string formatting) keyed on this so that a paused emulator —
    // the common debugging state — costs nothing to keep on screen.
    uint64_t GetEmulatorFrameCount() const
    {
        if (!IsRunning())
            return 0;
        return m_emulator->GetTotalFrameCount();
    }

    bool GetCpuState(adm::AmigaEmulator::CpuState& state) const
    {
        if (!IsRunning())
            return false;
        return m_emulator->GetCpuState(state);
    }

    bool GetCpuExtState(adm::core::CpuExtendedState& state) const
    {
        if (!IsRunning())
            return false;
        return m_emulator->GetCpuExtState(state);
    }

    int GetBplptEvents(adm::AmigaEmulator::BplptEvent* out, int maxEvents) const
    {
        if (!IsRunning())
            return 0;
        return m_emulator->GetBplptEvents(out, maxEvents);
    }

    int GetSprptEvents(adm::AmigaEmulator::SprptEvent* out, int maxEvents) const
    {
        if (!IsRunning())
            return 0;
        return m_emulator->GetSprptEvents(out, maxEvents);
    }

    int GetBlitEvents(adm::AmigaEmulator::BlitEvent* out, int maxEvents) const
    {
        if (!IsRunning())
            return 0;
        return m_emulator->GetBlitEvents(out, maxEvents);
    }

    // Destination words the blitter wrote, for the window named by a BlitEvent's
    // dataOff/dataWords. Returns 0 once the core ring has recycled that window.
    int GetBlitData(uint32_t off, uint32_t count, adm::AmigaEmulator::BlitWord* out) const
    {
        if (!IsRunning())
            return 0;
        return m_emulator->GetBlitData(off, count, out);
    }

    bool IsBlitDataCaptureEnabled() const
    {
        return m_emulator && m_emulator->IsBlitDataCaptureEnabled();
    }

    // Custom-register writes (CPU + Copper) captured during the most recent frame.
    int GetRegWriteEvents(adm::AmigaEmulator::RegWriteEvent* out, int maxEvents) const
    {
        if (!IsRunning())
            return 0;
        return m_emulator->GetRegWriteEvents(out, maxEvents);
    }

    bool HasRegWriteLog() const { return m_emulator && m_emulator->HasRegWriteLog(); }

    // Disk DMA transfers captured since the last core-ring clear. The ring is not
    // drained per frame, so callers dedup on DiskDmaEvent::seq.
    int GetDiskDmaEvents(adm::AmigaEmulator::DiskDmaEvent* out, int maxEvents) const
    {
        if (!IsRunning())
            return 0;
        const int n = m_emulator->GetDiskDmaEventCount();
        const int count = std::min(n, maxEvents);
        if (out && count > 0)
            std::memcpy(out, m_emulator->GetDiskDmaEvents(),
                        static_cast<size_t>(count) * sizeof(adm::AmigaEmulator::DiskDmaEvent));
        return count;
    }

    int GetDiskDmaPayload(uint32_t off, uint32_t len, uint8_t* out) const
    {
        if (!IsRunning())
            return 0;
        return m_emulator->GetDiskDmaPayload(off, len, out);
    }

    bool HasDiskDmaLog() const { return IsRunning() && m_emulator->HasDiskDmaLog(); }

    // Full machine-state snapshot for the .peek session format. SaveState returns an empty
    // vector when not running or unsupported; LoadState returns false (caller falls back to a
    // fresh boot) on size mismatch or core rejection.
    std::vector<uint8_t> SaveState() const
    {
        if (!IsRunning())
            return {};
        return m_emulator->SaveState();
    }
    // The one to use for anything that will be restored later - see
    // AmigaEmulator::SaveStateForRestore(). A plain SaveState() serializes mid-field and comes
    // back with the chipset rewound behind the CPU.
    std::vector<uint8_t> SaveStateForRestore() const
    {
        if (!IsRunning())
            return {};
        return m_emulator->SaveStateForRestore();
    }
    bool LoadState(const std::vector<uint8_t>& data) const
    {
        if (!IsRunning())
            return false;
        return m_emulator->LoadState(data);
    }
    size_t GetStateSize() const
    {
        if (!IsRunning())
            return 0;
        return m_emulator->GetStateSize();
    }

    // Force the paused state to a specific value (used when restoring a session).
    void SetPausedState(bool paused)
    {
        if (m_isPaused == paused)
            return;
        if (!paused)
            NotifyMachineWillRun();
        m_isPaused = paused;
        if (m_isPaused)
            ReleaseMouse();
    }

    adm::AmigaEmulator* GetEmulator() const { return m_emulator.get(); }

    void RequestRedraw() const
    {
        if (m_emulator)
            m_emulator->RequestRedraw();
    }

private:
    void HandleInput();
    // Remote-desktop mouse capture (see HandleInput): put the hidden cursor back in the middle of
    // the window, and turn this frame's cursor position into Amiga mouse motion.
    void RecentreRemoteCursor();
    void ReadRemoteMouseDelta(int16_t& deltaX, int16_t& deltaY);
    // Shared writer behind SaveCustomConfiguration/UpdateCustomConfiguration; stages the
    // media so a failed write leaves the configuration already on disk untouched.
    bool WriteCustomConfiguration(const std::string& name, bool allowOverwrite,
                                  std::string& outError);

    std::unique_ptr<adm::AmigaEmulator> m_emulator;
    GLFWwindow* m_window = nullptr;
    void DrawEmulatorContent();
    void DrawFullscreenOverlay();
    void DrawFullscreenExitButton();
    void StepEmulator();
    // Whether the last host frame advanced the machine. A frame that did not (paused, hidden)
    // restarts the emulator's frame clock, so resuming does not run the gap as catch-up. A main
    // thread stall draws no frame at all, so its time is still caught up.
    bool m_advancedLastFrame = false;
    bool m_advancedThisFrame = false;
    // After an Update(): pause + notify if a debug break fired or a RunUntilFrame target
    // was reached, cancelling any pending run-until (and its auto-turbo). Shared by the
    // docked, fullscreen and run-when-hidden stepping paths.
    void HandlePostStepStop();
    // After a manual frame step: record any filtered breaks (log-mode watchpoints, DOS
    // breakpoints whose predicate did not match) and carry the frame through to its
    // boundary, so a step advances a whole frame and its watch hits are not dropped.
    void FinishSteppedFrame();
    void RenderEmulatorImage();
    void DrawToolbar();
    void DrawDriveMenu();
    void DrawDiskActivity();
    void DrawHardDriveActivity();
    void DrawPowerLed();
    void PersistNormalizedHostMounts();
    void DrawPerfOverlay(ImDrawList* dl, ImVec2 imgMin, ImVec2 imgMax);

    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;
    bool m_suppressNextFocus = false;
    bool m_isPaused = false;
    bool m_runIsStepOver = false;
    bool m_isMuted = false;
    // The volume's slider over the mute button, up while the mouse rests on either
    bool m_volumeOpen = false;
    void DrawVolumeSlider(ImVec2 buttonMin, ImVec2 buttonMax);
    bool m_isTurbo = false;
    // RunUntilFrame target (auto-pause frame); empty when no run-until is pending.
    std::optional<uint64_t> m_runUntilFrame;
    // True when RunUntilFrame enabled turbo itself, so it is restored on completion.
    bool m_runUntilTurboAuto = false;
    bool m_showPerfOverlay = false;
    bool m_showPerfGraph = false;
    // Pixel magnifier ("loupe") over the emulator display. Toggled from the toolbar magnify
    // button and the Display window "Magnifier" checkbox (both bind to this single flag).
    bool m_showMagnifier = false;

    // The eyedropper (BeginPixelPick): who the pixel goes to, the frame read back to find it,
    // and the frame the pick ended on - its click must not capture the mouse after it
    std::function<void(bool, uint32_t)> m_pixelPick;
    std::vector<uint8_t> m_pixelPickFrame;
    int m_pixelPickEndFrame = -2;
    void DrawPixelPick(ImVec2 imgMin, ImVec2 imgMax, bool hovered);

    // Display post-process filters (Emulator -> Display Filters window). Independent,
    // combinable; intensities are dialled from the Filters window.
    EmulatorFilterParams m_filterParams;
    bool m_filtersWindowOpen = false;
    std::unique_ptr<EmulatorFilter> m_filter; // GL resources built up front in InitGraphics()
    void DrawFiltersWindow();

    // Animated-GIF capture (Emulator -> Screenshots & Capture -> GIF & Video Capture). Capture
    // records the next m_gifTargetFrames emulated frames straight to disk via the gif-h encoder.
    bool m_gifWindowOpen = false;
    adm::GifRecorder m_gifRecorder;
    std::string m_gifPath;
    int m_gifTargetFrames = 50;
    int m_gifFramesCaptured = 0;
    std::string m_gifStatus;
    // Video capture. The recorder itself lives in AmigaEmulator (both streams have to be written
    // from the frame loop); these are what the window needs to drive it, loaded from and saved to
    // the user's preferences.
    void DrawVideoCaptureSection();
    void LoadVideoCaptureSettings();
    void SaveVideoCaptureSettings();
    void StartFfmpegProbe();
    bool m_videoSettingsLoaded = false;
    core::VideoCaptureCodec m_videoCodec = core::VideoCaptureCodec::Zmbv;
    bool m_videoCodecChosen = false; // until the user or a saved preference has picked one
    int m_videoQuality = -1;         // CRF; -1 takes the codec's default
    std::string m_videoPath;
    std::string m_videoStatus;
    std::string m_ffmpegPath; // configured; empty looks on PATH
    // FFmpeg is found off the UI thread: listing its encoders takes a few hundred milliseconds.
    std::future<core::FfmpegTools> m_ffmpegProbe;
    bool m_ffmpegProbeAgain = false; // the path changed while a probe ran: probe it when done
    core::FfmpegTools m_ffmpegTools;
    bool m_ffmpegProbed = false;
    bool m_videoExocet = false; // the Exocet video rather than the frame alone; this session's
    std::function<std::string()> m_exocetVideoUnavailable;
    std::function<std::string(AmigaEmulator::VideoCaptureOptions&)> m_setUpExocetVideo;
    // Capture can start/resume the emulator: the recorder can't open until the first frame
    // exists, so the click sets m_gifPendingStart and recording begins once a frame is ready.
    bool m_gifPendingStart = false;
    void DrawGifCaptureWindow();
    // Writes one frame to the in-progress GIF when recording; finalizes after the target count.
    void CaptureGifFrameIfRecording();
    // Aspect-corrected output size for a captured frame (keeps native width, derives height from
    // the display aspect ratio so the GIF isn't vertically squished).
    void GetGifOutputSize(int srcW, int srcH, int& outW, int& outH) const;

    // Still capture (Emulator -> Screenshots). Grabs the current display texture and applies the
    // same aspect correction GIF capture uses, so a saved or pasted still has square pixels.
    // Returns false with `outError` filled in when there is no frame or the write failed.
    bool CaptureScreenshotRGB(std::vector<uint8_t>& rgb, int& w, int& h, std::string& outError);
    bool m_needsTwoRowToolbar = false;
    bool m_runWhenHidden = true;
    bool m_mouseCaptured = false;
    bool m_suppressCaptureClick = false;
    // Captured under Remote Desktop: the cursor is hidden and clipped to the window rather than
    // disabled, and motion is the difference between successive cursor positions. The counts
    // are logged on release.
    bool m_remoteMouse = false;
    double m_remoteLastX = 0.0;
    double m_remoteLastY = 0.0;
    int m_remoteWobble = 0;
    int m_remoteWarps = 0;
    int m_remoteJumpsDropped = 0;
    bool m_keyboardJoystickEnabled = false;

    // Keyboard forwarding state, indexed by RetroKey code (max value 317). Members rather than
    // function-local statics: statics were shared across instances and never reset on core
    // Stop/Start. Cleared in Stop().
    static constexpr size_t kRetroKeyStates = 512;
    std::array<bool, kRetroKeyStates> m_prevKeys{};
    bool m_keysActive = false;

    // Drive state (DF0:-DF3:)
    DriveState m_drives[4];
    core::UserPreferences* m_userPreferences = nullptr;
    bool m_hardDriveEditorOpen = false;
    bool m_cdDriveWindowOpen = false;
    // Which DH chip the Hard Drives window is showing. In memory only: the old
    // hardDriveGroupsOpen preference described collapsing headers that no longer exist.
    int m_selectedHardDrive = 0;
    int m_rwWarningMount = -1;
    std::string m_hostMountUiError;
    std::optional<std::filesystem::path> m_selectedBaseUaeConfig;
    // The missing-Kickstart notice: its text as last worked out, when, and the one hidden
    std::function<void()> m_onFindSystemFiles;
    std::string m_kickNotice;
    std::chrono::steady_clock::time_point m_kickNoticeChecked{};
    std::string m_kickNoticeHidden;
    std::string m_selectedCustomConfigurationName;
    std::function<void(const core::HostDirectoryMount&)> m_onOpenHostDirectory;
    std::function<void(const std::filesystem::path&)> m_onOpenHardfileDocument;
    // CD0: the disc the user chose and a background archive extraction.
    std::filesystem::path m_cdSource;
    std::string m_cdError;
    struct CdExtractionJob
    {
        std::thread worker;
        std::atomic<bool> finished{false};
        // Ejected (or another disc restored) while it ran: the extraction still lands in the
        // cache, but the disc is not put in the drive
        std::atomic<bool> cancelled{false};
        bool ok = false;
        std::filesystem::path archive;
        std::filesystem::path directory;
        std::filesystem::path staging;
    };
    std::unique_ptr<CdExtractionJob> m_cdExtraction;
    void PollCdExtraction();
    void ApplyCdImage(const std::filesystem::path& source, const std::filesystem::path& image);
    // What the Hard Drives window last read out of each slot's hardfile, keyed by path, time and
    // size so the RDB is parsed again only when the file changes.
    struct HardfileInspection
    {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime{};
        std::uintmax_t size = 0;
        std::optional<core::HardfileInfo> info;
        std::string error;
    };
    std::array<HardfileInspection, 2> m_hardfileInspection;
    // GetModelUnavailableReason() answers from this rather than from the ROM directory each
    // time: the Emulator > Model menu asks for every preset on every frame it is open, and a
    // CD32 answer is several file stats. Refilled when the ROM directory changes and every
    // couple of seconds, so a ROM dropped into the directory shows up without a restart.
    struct ModelAvailabilityCache
    {
        std::string romDirectory;
        std::chrono::steady_clock::time_point refreshed{};
        std::unordered_map<int, std::string> reasons; // keyed by AmigaModel
    };
    mutable ModelAvailabilityCache m_modelAvailability;

    // Overlay callback (invoked each frame after the emulator image is drawn)
    std::function<void(ImDrawList*, ImVec2, ImVec2)> m_onDrawEmulatorOverlay;

    // Raster-frame ("adjusted window") mode. The emulator picture is shrunk and inset inside a
    // larger rect standing for the whole beam, so the offscreen colour clocks around it have
    // somewhere to be drawn. Adapted from the ENGINE9000 debugger by alpine9000 — see
    // core/RasterFrameMap.h and PATCHES.md.
    bool m_rasterFrameMode = false;
    adm::core::RasterFrameMap m_rasterFrameMap;

    // Invoked after the image with (drawList, rasterMin, rasterMax, imgMin, imgMax). Only fires
    // while raster-frame mode is on and the map is valid.
    std::function<void(ImDrawList*, ImVec2, ImVec2, ImVec2, ImVec2)> m_onDrawRasterOverlay;
    // Called when the emulator is reset or a new game is loaded
    std::function<void()> m_onEmulatorReset;

    // Called to start the emulator (mount the active document and power on) when GIF capture is
    // requested while nothing is running. Wired by Application to RunAdf().
    std::function<void()> m_onStartEmulator;
    // Fired immediately before the machine is about to run under its own steam - resume,
    // any step, run-until. See SetOnBeforeMachineRuns().
    std::function<void()> m_onBeforeMachineRuns;
    void NotifyMachineWillRun()
    {
        if (m_onBeforeMachineRuns)
            m_onBeforeMachineRuns();
    }

    // Fired when a CPU breakpoint fires and the emulator auto-pauses
    std::function<void()> m_onDebugBreak;
    std::function<bool()> m_debugBreakFilter;

    // Returns the list of open documents for the drive "Images" submenu
    std::function<std::vector<ui::DiskDocumentInfo>()> m_getDocumentInfos;
    // Called when the user picks a document to mount from the drive menu
    std::function<void(int drive, uint32_t docId)> m_onMountDocument;
    std::function<void(int drive, bool writeProtected)> m_onSetWriteProtect;
    // Optional predicate: when set and returns true, left-click won't capture the mouse
    std::function<bool()> m_mouseCaptureBlocker;

    // Fullscreen state.
    // Fullscreen is presented as a separate overlay window covering the main viewport; the
    // dockable "Amiga Emulator" window is never moved, resized or undocked. This keeps the
    // user's layout (dock node, floating position and size) intact across fullscreen toggles.
    bool m_isFullscreen = false;
    std::function<void(bool)> m_onFullscreenToggle;
    bool m_enteringFullscreen = false;

    // Fullscreen exit button state
    float m_mouseHoverTopTimer = 0.0f;
    float m_mouseAwayTimer = 0.0f;
    float m_exitButtonY = -60.0f; // Start hidden (above screen)
    bool m_showExitButton = false;

public:
    void SetOnFullscreenToggle(std::function<void(bool)> callback)
    {
        m_onFullscreenToggle = std::move(callback);
    }

    void SetOnDrawEmulatorOverlay(std::function<void(ImDrawList*, ImVec2, ImVec2)> cb)
    {
        m_onDrawEmulatorOverlay = std::move(cb);
    }

    // Raster-frame mode: inset the picture inside the full-beam rect. Off by default, so the
    // ordinary aspect-fit path is completely untouched when nobody has asked for the raster.
    void SetRasterFrameMode(bool on) { m_rasterFrameMode = on; }
    bool IsRasterFrameMode() const { return m_rasterFrameMode && m_rasterFrameMap.valid; }

    // Fed each frame by Application, which owns the DMA record the raster extent comes from.
    void SetRasterFrameMap(const adm::core::RasterFrameMap& map) { m_rasterFrameMap = map; }
    const adm::core::RasterFrameMap& GetRasterFrameMap() const { return m_rasterFrameMap; }

    // (drawList, rasterMin, rasterMax, imgMin, imgMax)
    void SetOnDrawRasterOverlay(std::function<void(ImDrawList*, ImVec2, ImVec2, ImVec2, ImVec2)> cb)
    {
        m_onDrawRasterOverlay = std::move(cb);
    }

    void SetOnEmulatorReset(std::function<void()> cb) { m_onEmulatorReset = std::move(cb); }

    void SetOnStartEmulator(std::function<void()> cb) { m_onStartEmulator = std::move(cb); }

    // The Exocet video, the capture window's option: why it
    // cannot be recorded now (empty: it can), and what sets a recording's options up to compose it
    // - empty, or why it could not.
    void SetExocetVideo(std::function<std::string()> unavailable,
                        std::function<std::string(AmigaEmulator::VideoCaptureOptions&)> setUp)
    {
        m_exocetVideoUnavailable = std::move(unavailable);
        m_setUpExocetVideo = std::move(setUp);
    }

    // Fired just before anything makes the machine run: resume, step frame/instruction/over,
    // end-frame, run-until, and SetPausedState(false). The frame-history transport uses it
    // to take the machine off timeline playback, which otherwise carries on replaying
    // recorded frames into a machine that is running live underneath it. Every route in
    // (toolbar, hotkey, console, MCP) comes through here, which is the point of having it
    // rather than a call at each call site.
    void SetOnBeforeMachineRuns(std::function<void()> cb) { m_onBeforeMachineRuns = std::move(cb); }

    // Called when a CPU breakpoint fires and the emulator auto-pauses.
    void SetOnDebugBreak(std::function<void()> cb) { m_onDebugBreak = std::move(cb); }
    // Consulted when a debug break fires, before the emulator pauses. Returning true means
    // the break was handled without stopping (a log-mode watchpoint recorded its hit), so
    // execution continues. Returning false pauses as usual.
    void SetDebugBreakFilter(std::function<bool()> cb) { m_debugBreakFilter = std::move(cb); }

    void SetGetDocumentInfos(std::function<std::vector<ui::DiskDocumentInfo>()> cb)
    {
        m_getDocumentInfos = std::move(cb);
    }
    void SetOnMountDocument(std::function<void(int, uint32_t)> cb)
    {
        m_onMountDocument = std::move(cb);
    }
    void SetOnSetWriteProtect(std::function<void(int, bool)> cb)
    {
        m_onSetWriteProtect = std::move(cb);
    }

    // When this returns true, left-click will not capture the mouse (e.g. when an overlay is
    // active)
    void SetMouseCaptureBlocker(std::function<bool()> cb) { m_mouseCaptureBlocker = std::move(cb); }

    bool IsFullscreen() const { return m_isFullscreen; }
    bool IsMouseCaptured() const { return m_mouseCaptured; }
    void SetFullscreen(bool fullscreen);

    void SetFocus() { m_shouldFocus = true; }
    void SuppressNextFocus() { m_suppressNextFocus = true; }
    void SetWindow(GLFWwindow* window) { m_window = window; }

    bool m_shouldFocus = false;

    bool* GetShowPerfOverlayPtr() { return &m_showPerfOverlay; }

    // Pixel magnifier shared state (toolbar button + Display window checkbox bind to the same
    // flag).
    bool* GetShowMagnifierPtr() { return &m_showMagnifier; }
    bool IsMagnifierEnabled() const { return m_showMagnifier; }
    bool* GetShowPerfGraphPtr() { return &m_showPerfGraph; }

    // The eyedropper: the next left click on the picture hands that pixel, as 0xRRGGBBFF, to
    // `done(true, rgba)`; Esc or a right click hands it false. An empty `done` cancels the pick
    // in progress (handing it false); a new pick does too, before it starts. The mouse is not
    // captured while it is up.
    void BeginPixelPick(std::function<void(bool picked, uint32_t rgba)> done);
    bool IsPixelPicking() const { return static_cast<bool>(m_pixelPick); }
    void SetShowPerfOverlay(bool v) { m_showPerfOverlay = v; }
    void SetShowPerfGraph(bool v) { m_showPerfGraph = v; }

    // Display filters: the Filters window edits the params in place; prefs load/save copy
    // the whole struct. The window-open flag is what the Emulator menu's "Display Filters"
    // item toggles.
    bool* GetFiltersWindowOpenPtr() { return &m_filtersWindowOpen; }

    // The GIF & Video Capture window's open flag — toggled by the Emulator menu item.
    bool* GetGifWindowOpenPtr() { return &m_gifWindowOpen; }

    // Emulator -> Screenshots. Both act on the frame currently on screen and report their own
    // failures to the log, so the menu can call them and forget.
    void CopyScreenshotToClipboard();
    void SaveScreenshotToFile();
    const EmulatorFilterParams& GetFilterParams() const { return m_filterParams; }
    void SetFilterParams(const EmulatorFilterParams& p) { m_filterParams = p; }
    void SetRunWhenHidden(bool v) { m_runWhenHidden = v; }
};

} // namespace adm::ui
