#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../../core/DiskDocumentInfo.h"

namespace adm::ui {

// The four emulated floppy slots, DF0-DF3, in one place: what each is backed by
// and the controls to change it. A slot is backed either by a disk image (an open
// document or a file) or by a real, spinning floppy over FloppyBridge
// (Greaseweazle / DrawBridge / SuperCard Pro / XCopy Standalone).
//
// Layout: the drive list on top; below it the settings of the highlighted drive -
// image controls (mount an open document or a file, eject, enable) or the live
// drive's hardware configuration with Attach/Detach, on Disk image / Live drive tabs.
//
// Several slots can be live at once (one device per serial port). The window owns
// no hardware and no documents itself - every action is a callback into
// Application, which forwards to EmulatorView/AmigaEmulator. Attaching a live
// drive does not need the machine running: with it stopped the slot is recorded
// as pending and the device is opened when the emulator starts. Attaching while
// running is asynchronous - the core opens the device on its next emulated
// vertical blank - which is why the list says "opening" until a poll says
// otherwise.
//
// A sibling of FloppyDiskView, not a replacement: that window images a physical
// disk to a file with the `gw` CLI and stays useful.
class FloppyDrivesView
{
public:
    // Mirrors AmigaEmulator::BridgeStatus plus the host-side drive model, kept
    // separate so this view does not pull in the emulator header.
    struct Status
    {
        // Whether the *loaded* core exports the bridge API. False also when no core
        // is loaded, so it says nothing about support until IsCoreLoaded() is true.
        bool supported = false;
        // The slot exists in the machine's configuration (DF2/DF3 default to off).
        bool enabled = true;
        // File-backed slots: whether an image / document is mounted, and its name.
        bool imageMounted = false;
        std::string imageName;
        // Host side: the slot is assigned to live hardware (this is what Detach undoes).
        bool assigned = false;
        // Assigned, but the request has not reached the core - the machine is stopped.
        bool pending = false;
        // Core side, meaningful only when assigned && !pending.
        bool configured = false;
        bool opened = false;
        bool diskInDrive = false;
        bool writeProtected = false;
        bool motor = false;
        // Set from the emulator's own per-drive telemetry, so it is true for image-backed
        // slots as well as live hardware.
        bool writing = false;
        int cylinder = 0;
        std::string driverName;
        std::string comPort;
        std::string lastError;
    };

    // One slot's live-drive configuration.
    struct Config
    {
        int hardwareType = 1; // index into the driver list; 1 = Greaseweazle
        std::string comPort;
        int bridgeMode = 0;
        int bridgeDensity = 0;
        int driveCable = 0;
        int driveSlot = 0; // DF0-DF3 - which slot this configuration belongs to
        // Read-ahead: cache tracks in the background whenever the drive is idle, so a
        // track the guest asks for next is already in memory instead of costing a
        // revolution to capture.
        bool autoCache = true;
        // Let the bridge serve an evenly-timed track ten times faster than the platter
        // turns. Only applies to tracks whose measured bitcell timing is uniform, so
        // timing-based protection still reads at real speed.
        bool smartSpeed = true;
        // Let the emulated machine write to the physical disk. Off by default and not
        // taken from the disk's own tab: writing real media cannot be undone. The
        // padlock on the chip shows the result and is not clickable for a live slot -
        // the tab and this flag are the only two things that decide it.
        bool allowWrites = false;
    };

    FloppyDrivesView();
    ~FloppyDrivesView() = default;

    void Draw();

    void Open();
    void Close() { m_isOpen = false; }
    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    // --- live drives ---

    // Hardware types the core's vendored drivers support, in hardwareType order.
    void SetOnGetDriverList(std::function<std::vector<std::string>()> cb)
    {
        m_onGetDriverList = std::move(cb);
    }
    // Serial ports visible right now. Called on open and on Refresh only - never
    // per frame, because enumeration walks the OS device list.
    void SetOnEnumComPorts(std::function<std::vector<std::string>()> cb)
    {
        m_onEnumComPorts = std::move(cb);
    }
    // Returns false if the request was rejected outright (no core support, bad
    // slot, no port chosen, port already used by another live slot). True means
    // recorded - queued for the next vertical blank if the machine is running,
    // pending until it starts otherwise.
    void SetOnAttach(std::function<bool(const Config&)> cb) { m_onAttach = std::move(cb); }
    void SetOnDetach(std::function<void(int driveSlot)> cb) { m_onDetach = std::move(cb); }
    // Whether the PuAE core DLL is loaded. Everything the core can tell us about
    // bridge support is meaningless until it is - see Draw().
    void SetOnIsCoreLoaded(std::function<bool()> cb) { m_onIsCoreLoaded = std::move(cb); }
    // Fired when a slot's live configuration is edited; cfg.driveSlot says which.
    void SetOnSettingsChanged(std::function<void(const Config&)> cb)
    {
        m_onSettingsChanged = std::move(cb);
    }

    // --- images ---

    // Open disk documents, for the "mount an open document" list.
    void SetOnGetDocuments(std::function<std::vector<DiskDocumentInfo>()> cb)
    {
        m_onGetDocuments = std::move(cb);
    }
    void SetOnMountDocument(std::function<void(int driveSlot, uint32_t docId)> cb)
    {
        m_onMountDocument = std::move(cb);
    }
    // Ask for a file with the native dialog and mount it. Blocks while the dialog is up.
    void SetOnMountFile(std::function<void(int driveSlot)> cb) { m_onMountFile = std::move(cb); }
    void SetOnEject(std::function<void(int driveSlot)> cb) { m_onEject = std::move(cb); }
    // Toggles write protection on whatever backs the slot. Live drives are excluded:
    // the tab on the physical disk decides, and the bridge only reports it.
    void SetOnSetWriteProtect(std::function<void(int driveSlot, bool writeProtected)> cb)
    {
        m_onSetWriteProtect = std::move(cb);
    }
    void SetOnSetEnabled(std::function<void(int driveSlot, bool enabled)> cb)
    {
        m_onSetEnabled = std::move(cb);
    }

    // --- shared ---

    // Per-slot status. Called once per drawn frame per slot; cheap.
    void SetOnGetStatus(std::function<Status(int driveSlot)> cb) { m_onGetStatus = std::move(cb); }
    // Whether the emulator is running. Decides whether an attach opens the device
    // now or is held until the machine starts.
    void SetOnIsRunning(std::function<bool()> cb) { m_onIsRunning = std::move(cb); }
    // Fired when the user highlights another slot, so the choice can be remembered.
    void SetOnSelectedSlotChanged(std::function<void(int driveSlot)> cb)
    {
        m_onSelectedSlotChanged = std::move(cb);
    }

    // Restore persisted live configuration for one slot. Deliberately config only -
    // a slot is never re-attached automatically at startup, because live hardware
    // state cannot be restored and silently spinning up a drive would be wrong.
    void SetSlotConfig(int slot, const Config& cfg);
    const Config& GetSlotConfig(int slot) const;

    // Which slot's row is highlighted (and whose settings are shown).
    void SetSelectedSlot(int slot);
    int GetSelectedSlot() const { return m_selectedSlot; }

private:
    void DrawDriveList();
    void DrawSlotPanel(int slot);
    void DrawImagePanel(int slot);
    void DrawLivePanel(int slot);
    void RefreshComPorts();
    void SaveSettings(int slot);
    // Another slot is assigned to this port: one device cannot be opened twice.
    int PortUsedByOtherSlot(int slot, const std::string& port) const;
    const char* BackingLabel(int slot, bool* outIsWarn, bool* outIsError) const;

    bool m_isOpen = false;
    Config m_slots[4];
    int m_selectedSlot = 0;

    std::vector<std::string> m_drivers;
    std::vector<std::string> m_comPorts;
    bool m_listsLoaded = false;
    // Tracked so the lists can be re-read the moment the core appears: until then
    // they are the host-side stand-ins, not what the core actually reports.
    bool m_coreLoaded = false;

    // This frame's status for each slot, sampled once at the top of Draw().
    Status m_status[4];

    // Per slot: the time an open was requested of the core, or < 0 when none is in
    // flight. Cleared once the slot reports opened or an error, so the UI can say
    // "opening" during the ~1 s device open rather than flashing "failed". Set when
    // Attach is pressed on a running machine, and again when a pending slot is
    // handed to the core by an emulator start.
    double m_awaitingSince[4];
    bool m_wasPending[4];
    std::string m_lastActionMessage[4];
    bool m_lastActionWasError[4];

    std::function<std::vector<std::string>()> m_onGetDriverList;
    std::function<std::vector<std::string>()> m_onEnumComPorts;
    std::function<bool(const Config&)> m_onAttach;
    std::function<void(int)> m_onDetach;
    std::function<Status(int)> m_onGetStatus;
    std::function<bool()> m_onIsRunning;
    std::function<bool()> m_onIsCoreLoaded;
    std::function<void(const Config&)> m_onSettingsChanged;
    std::function<void(int)> m_onSelectedSlotChanged;
    std::function<std::vector<DiskDocumentInfo>()> m_onGetDocuments;
    std::function<void(int, uint32_t)> m_onMountDocument;
    std::function<void(int)> m_onMountFile;
    std::function<void(int)> m_onEject;
    std::function<void(int, bool)> m_onSetWriteProtect;
    std::function<void(int, bool)> m_onSetEnabled;
};

} // namespace adm::ui
