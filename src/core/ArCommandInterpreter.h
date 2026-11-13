#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "AmigaCustom.h"
#include "ArConsoleTypes.h"
#include "ArMemoryAccess.h"
#include "Assembler.h"
#include "AudioLogSnapshot.h"
#include "BlitLogSnapshot.h"
#include "CpuExtState.h"
#include "Disassembler.h"
#include "DiskLogSnapshot.h"
#include "DisplayMasks.h"
#include "ExecIntrospect.h"
#include "FrameHistory.h"
#include "HostMountService.h"
#include "HostPorts.h"
#include "KprintfBreakpoints.h"
#include "LibraryCallAnnotator.h"
#include "MachineConfig.h"
#include "PaulaStateSnapshot.h"
#include "ProfileSnapshot.h"
#include "RegLogSnapshot.h"
#include "ScenarioRequest.h"
#include "SegListResolver.h"
#include "WhdloadLaunch.h"

namespace adm::core {

class ExocetSession;
class MemoryAnnotations;
class MemoryLocks;

struct ArCpuState
{
    uint32_t d[8] = {};
    uint32_t a[8] = {};
    uint32_t pc = 0;
    uint16_t sr = 0;
};

// Host callbacks the interpreter uses to reach the emulator, breakpoint store,
// file dialogs etc. All hooks are optional; missing hooks degrade to errors.
struct ArHost
{
    struct DocumentInfo
    {
        std::string path;
        std::string kind = "ADF";
        std::optional<HostMountDevice> assignedDevice;
    };

    // Emulator state
    std::function<bool()> isRunning;
    std::function<bool()> isPaused;
    std::function<void()> pause;
    std::function<void()> resume; // arm trace + un-pause
    std::function<bool()> stepInto;
    std::function<bool()> stepOver;
    std::function<bool(ArCpuState&)> getCpuState;
    // Stack pointers, control, MMU and FPU registers (core 1.102.0). Optional.
    std::function<bool(CpuExtendedState&)> getCpuExtState;
    std::function<bool(const std::string& reg, uint32_t value)> setCpuRegister;
    std::function<bool(uint32_t addr, Disassembler::Line&)> disassembleOne;
    std::function<bool(const std::string& cmd, std::string& out)> execUaeCommand;

    // Breakpoints / memory watchpoints (kept in sync with the Breakpoints window)
    std::function<bool(uint32_t addr)> addBreakpoint; // false if already set
    // Conditional / smart PC breakpoint (BS ... IF/N/ONESHOT). reg/oper are UAE tokens
    // (e.g. "D0", "=="). null -> the BS command falls back to a plain addBreakpoint.
    std::function<bool(uint32_t addr, bool condEnabled, const std::string& reg,
                       const std::string& oper, uint32_t value, uint32_t mask, int ignoreCount,
                       bool oneShot)>
        addSmartBreakpoint;
    std::function<bool(uint32_t addr)> removeBreakpoint;                     // false if not found
    std::function<std::vector<std::pair<uint32_t, bool>>()> listBreakpoints; // addr, enabled
    std::function<void()> clearAllBreakpoints;
    std::function<bool(uint32_t addr)> addWatchpoint;
    std::function<bool(uint32_t addr)> removeWatchpoint;
    std::function<std::vector<uint32_t>()> listWatchpoints;
    std::function<void()> clearAllWatchpoints;

    // Copper
    std::function<bool(int list /*1|2*/, uint32_t& lcAddr)> getCopperLc;
    std::function<std::string(uint16_t w1, uint16_t w2)> disasmCopperIns;

    // Custom chip registers: name for a byte offset within $DFF000 ("" if unknown)
    std::function<std::string(uint32_t offset)> customRegName;
    // CHIPREGS: short description for a $DFF000 byte offset ("" if unknown).
    std::function<std::string(uint32_t offset)> customRegDesc;
    // DCHIP: bit-field breakdown for a register offset, MSB-first. Each field
    // covers bits [lo..hi] (hi==lo for a single bit). Empty if no definition.
    struct RegBitField
    {
        int hi = 0;
        int lo = 0;
        std::string name;
    };
    std::function<std::vector<RegBitField>(uint32_t offset)> customRegBits;

    // File dialogs (nullptr -> host paths only)
    std::function<std::optional<std::string>()> requestOpenFile;
    std::function<std::optional<std::string>()> requestSaveFile;

    // Fired after any command that may have mutated core state
    std::function<void()> onStateChanged;

    // Load a document (ADF/EXE/DMS etc.) into the application — works without emulator running.
    // Replaces the open document; force replaces it even when it has unsaved changes.
    std::function<bool(const std::string& path, bool force)> loadDocument;
    // Returns the recent-files list for the LIST command
    std::function<std::vector<std::string>()> getRecentFiles;

    // Future `A` command backend (see Assembler.h); null -> "not installed"
    std::shared_ptr<Assembler> assembler;

    // Start emulator with the effective media profile (same as F5/Run button); null -> unavailable.
    std::function<void()> startEmulator;
    // Stop the running emulator session.
    std::function<void()> stopEmulator;
    // Turbo/warp mode: null -> unavailable.
    std::function<void()> toggleTurbo;
    std::function<bool()> isTurbo;
    // Step one full emulator frame (only when paused): null -> unavailable.
    std::function<void()> stepFrame;
    std::function<void()> endFrame; // complete the current mid-execution frame
    // Run until the emulated frame counter reaches targetFrame, then auto-pause (RF
    // command). turbo enables warp mode for the run. Returns false if not running or the
    // target is at/behind the current frame. null -> unavailable.
    std::function<bool(uint64_t targetFrame, bool turbo)> runUntilFrame;
    // Reset the running emulator session (RESET command).
    std::function<void()> resetEmulator;

    // Returns names/paths of all currently open documents (DOCS command, no arg).
    std::function<std::vector<std::string>()> getOpenDocuments;
    std::function<std::vector<DocumentInfo>()> getOpenDocumentInfo;
    // Stops any running emulation and boots the document at 0-based index (DOCS N).
    std::function<void(int idx)> runDocumentByIndex;
    // Closes the open document at 0-based index (CLOSE dN).
    std::function<void(int idx)> closeDocumentByIndex;

    // Directory-backed hard-drive configuration. Mutations are pending until APPLY.
    std::function<std::array<HostDirectoryMount, 2>()> getConfiguredHostMounts;
    std::function<std::array<HostDirectoryMount, 2>()> getEffectiveHostMounts;
    std::function<bool(const HostDirectoryMount&, std::string&)> setHostMount;
    std::function<bool(std::string&)> applyHostMounts;
    std::function<bool(HostMountDevice)> openHostMount;
    std::function<bool(HostMountDevice)> refreshHostMount;
    // Boot ROMs for the A590/A2091 and A4091 SCSI cards (HD ROM).
    std::function<HardDriveControllerRoms()> getHardDriveControllerRoms;
    std::function<bool(const HardDriveControllerRoms&, std::string&)> setHardDriveControllerRoms;
    // PEEKaboo model id of the selected machine ("A1200"), for controller warnings.
    std::function<std::string()> getModelId;
    // Machine presets for MODEL. A preset whose ROMs are missing is listed but refused.
    struct ModelInfo
    {
        std::string id;
        std::string name;
        bool available = true;
        std::string unavailableReason;
    };
    std::function<std::vector<ModelInfo>()> listModels;
    std::function<bool(const std::string& id, std::string& error)> selectModel;

    // WHDLoad launches (WHD).
    struct WhdloadStatus
    {
        std::string volumeLabel;
        std::string path;
        bool archive = false;
    };
    std::function<bool(const std::string& path, std::string& error)> launchWhdload;
    // Empty when the running machine is not a WHDLoad launch.
    std::function<std::optional<WhdloadStatus>()> getWhdloadStatus;
    std::function<WhdloadSettings()> getWhdloadSettings;
    std::function<void(const WhdloadSettings&)> setWhdloadSettings;
    std::function<std::string()> getWhdloadSavesDirectory;
    // Cold restart with the saved drives, ending a WHDLoad launch.
    std::function<bool(std::string& error)> endWhdload;

    // CD0 (CD).
    struct CdStatus
    {
        std::string source;    // image or archive chosen
        std::string image;     // image the machine is given
        std::string inMachine; // what the running machine has in CD0
        bool running = false;
        bool hasDrive = false;
        bool restartPending = false;
        bool extracting = false;
        std::string error;
    };
    std::function<CdStatus()> getCdStatus;
    std::function<bool(const std::string& path, std::string& error)> insertCd;
    std::function<void()> ejectCd;

    // Machine configuration below the model preset (MACHINE). Settings are core option keys
    // (puae_chipset, ...) with their configured value; `running` is what the machine has.
    struct MachineStatus
    {
        bool supported = false; // the core has the options (1.86.0)
        std::map<std::string, std::string> settings;
        std::optional<MachineConfig> running;
    };
    std::function<MachineStatus()> getMachineStatus;
    // Stores one setting for the next power-on.
    std::function<void(const std::string& key, const std::string& value)> setMachineSetting;
    // Cold restart of a running machine with the configured settings.
    std::function<bool(std::string& error)> restartMachine;

    // Serial and parallel ports (SERIAL, PARALLEL). Changes apply to a running machine at once.
    struct HostPortsInfo
    {
        bool supported = false;
        SerialPortTarget serial;
        std::string parallelPath;
        std::optional<HostPortsStatus> status; // empty when the machine is not running
        std::vector<std::string> devices;
    };
    std::function<HostPortsInfo()> getHostPorts;
    std::function<void(const SerialPortTarget&)> setSerialPort;
    std::function<void(const std::string& path)> setParallelOutput;
    // MIDI (MIDI): out -2 none, -1 system mapper; in -1 none.
    struct MidiInfo
    {
        bool supported = false;
        int outDevice = -2;
        int inDevice = -1;
        std::vector<MidiDevice> outputs;
        std::vector<MidiDevice> inputs;
        std::optional<HostPortsStatus> status;
    };
    std::function<MidiInfo()> getMidi;
    std::function<void(int outDevice, int inDevice)> setMidiDevices;
    // Network (NETWORK): core options applied at the next power-on, like MACHINE's.
    struct NetworkInfo
    {
        bool supported = false; // bsdsocket.library
        bool bsdsocket = false;
        bool cardSupported = false;     // A2065 and uaenet.device
        std::string a2065 = "disabled"; // disabled, slirp or slirp_inbound
        bool uaenet = false;
    };
    std::function<NetworkInfo()> getNetwork;
    // A puae_* option and its core value, for the next power-on.
    std::function<void(const std::string& key, const std::string& value)> setNetworkSetting;
    // Expansion sound card (SOUNDCARD): a core option applied at the next power-on.
    struct SoundCardInfo
    {
        bool supported = false;
        std::string board = "disabled"; // disabled, toccata, prelude, prelude1200, uaesnd_z2/_z3
    };
    std::function<SoundCardInfo()> getSoundCard;
    std::function<void(const std::string& board)> setSoundCard;

    // Break-on-bootblock-entry (FB). All null -> "FB unavailable" (old core).
    std::function<void(bool on)> armBootblockBreak;
    std::function<bool()> isBootblockArmed;
    std::function<bool()> consumeBootblockHit; // read-clear: did FB cause this break?
    std::function<uint32_t()> bootblockEntry;

    // Break-on-CPU-exception (EB). null -> "EB unavailable" (old core).
    std::function<void(uint64_t mask)> armExceptionBreak;
    // read-clear: returns the firing vector if an exception caused this break, else nullopt.
    std::function<std::optional<int>()> consumeExceptionHit;

    // Break-on-beam-position (BL). null -> "BL unavailable" (old core).
    std::function<void(int vpos, int hpos)> armBeamBreak;
    std::function<bool()> consumeBeamHit; // read-clear: did a beam break fire?

    // Break-on-copper-instruction (BC): toggle a Copper-source watchpoint at a chip-RAM address.
    std::function<void(uint32_t addr)> toggleCopperBreakpoint;

    // Break-on-KPrintF-text (BK/BKS/BKD/BKDA): traps matched host-side against the program's
    // serial debug output. addKprintf returns the new rule id, or 0 when the rule is a duplicate
    // or the regex does not compile. All null -> the BK commands report unavailable.
    std::function<int(const std::string& text, KprintfMatch match, bool caseSensitive,
                      bool oneShot)>
        addKprintfBreakpoint;
    std::function<bool(int id)> removeKprintfBreakpoint;
    std::function<void()> clearKprintfBreakpoints;
    std::function<std::vector<KprintfBreakpoint>()> listKprintfBreakpoints;
    // read-clear: the KPrintF line that caused this break, so the banner can name it.
    std::function<std::optional<KprintfHit>()> consumeKprintfHit;

    // Memory watchpoint hit (MS). read-clear: returns the access that triggered this
    // break (if any), so the break banner can name the writing instruction.
    // null -> unavailable (old core); a watchpoint break then just looks like a PC break.
    struct WatchpointHit
    {
        uint32_t pc = 0;         // instruction that performed the access
        uint32_t addr = 0;       // accessed address
        uint32_t val = 0;        // value written (0 for reads)
        int rwi = 0;             // bit0=read, bit1=write, bit2=instruction fetch
        int size = 0;            // access width in bytes (1/2/4)
        uint32_t accessMask = 0; // UAE MW_MASK_* of the triggering agent (0 if unknown)
    };
    std::function<std::optional<WatchpointHit>()> consumeWatchpointHit;

    // VERSION command — both optional; missing -> "unknown"
    std::function<std::string()> getAppVersion;
    std::function<std::string()> getCoreVersion;

    // FRAME command — total emulator frames since last reset; null -> unavailable
    std::function<uint64_t()> getFrameCount;

    // STATE command — whole-machine snapshots, the same store the save_state/load_state MCP
    // tools use. Save/load go through EmulatorStateOps so both front ends apply identical
    // core-build and mounted-document checks. All null -> the command reports unavailable.
    struct StateEntry
    {
        std::string name;
        size_t bytes = 0;
        uint64_t frame = 0;
        bool persisted = false;
        std::string created;
    };
    // Returns an error string (empty on success); `outWarning` reports a partial success,
    // such as a persisted snapshot that could only be kept in memory.
    std::function<std::string(const std::string& name, bool persist, std::string& outWarning)>
        saveState;
    std::function<std::string(const std::string& name)> loadState;
    std::function<bool(const std::string& name)> deleteState; // false if there was no such one
    std::function<std::vector<StateEntry>()> listStates;

    // ---- Frame history: HISTORY / REWIND / GOTO / DIFF / LOOP ----
    // The rolling per-frame save-state timeline. All hooks optional; missing ones make the
    // commands report the timeline as unavailable.
    struct FrameHistoryInfo
    {
        bool available = false;
        FrameHistoryStats stats;
        uint64_t position = 0;  // frame the timeline is parked on
        uint64_t loopStart = 0; // armed LOOP range; loopEnd 0 means no loop
        uint64_t loopEnd = 0;
        bool playing = false;   // the timeline is replaying frames rather than running live
        int playMode = 0;       // 0 forward, 1 reverse, 2 ping-pong
        double playSpeed = 1.0; // share of the machine's own frame rate
    };
    std::function<FrameHistoryInfo()> frameHistoryInfo;
    // Each returns an error string, empty on success.
    std::function<std::string(uint64_t frame)> frameHistorySeek;
    std::function<std::string(uint64_t count)> frameHistoryRewind;
    std::function<std::string(uint64_t a, uint64_t b)> frameHistoryLoop; // 0,0 clears
    std::function<void()> frameHistoryClear; // throw the whole recording away
    // Transport over the recorded timeline: play/pause, one-frame steps, direction, speed.
    std::function<void(bool playing)> frameHistoryPlay;
    std::function<std::string(int delta)> frameHistoryStep;
    std::function<void(int mode)> frameHistoryPlayMode;
    std::function<void(double speed)> frameHistoryPlaySpeed;
    std::function<std::string(uint64_t a, uint64_t b, std::vector<FrameDiffRun>& out,
                              size_t maxRuns, bool& truncated)>
        frameHistoryDiff;

    // PROFILE / CALLSTACK commands — live profiler snapshot for the requested view.
    // null -> the commands report the profiler is unavailable.
    std::function<ProfileSnapshot(const ProfileQuery&)> getProfile;

    // BLIT command — the current frame's blitter operations (mirrors the Blitter Operations
    // window). `limit` <= 0 returns all blits. null -> the command reports unavailable.
    // COV command -- did a source line execute, in which of its copies, and how often. Returns
    // the same JSON the MCP line_coverage tool does. null -> the command reports unavailable.
    std::function<nlohmann::json(const std::string& file, uint32_t line, uint64_t frame)>
        lineCoverage;
    // Whether the session trace ring is retaining instructions, and a way to turn it on. COV
    // needs it, and making the user discover that separately is a poor trade.
    std::function<bool()> isTraceHistory;
    std::function<void(bool)> setTraceHistory;

    std::function<BlitLogSnapshot(int limit)> getBlitLog;
    std::function<RegLogSnapshot(int limit)> getRegLog;

    // EXO command - the Exocet window's session (core/ExocetQuery.h: the same answers the
    // Exocet MCP tools give). null -> the command reports Exocet unavailable.
    std::function<ExocetSession*()> exocetSession;

    // DSKLOG command — the session's disk DMA traffic (mirrors the Disk I/O window).
    // null -> the command reports unavailable.
    std::function<DiskLogSnapshot(const DiskLogQuery&)> getDiskLog;

    // LIVE command - FloppyBridge physical drive. The scriptable sibling of the
    // Live Floppy Drive window; both route through EmulatorView::AttachBridgeDrive.
    // null -> the command reports unavailable.
    struct FloppyBridgeAttach
    {
        int drive = 0;
        int driverIndex = 1;
        std::string comPort;
        int bridgeMode = 0;
        int bridgeDensity = 0;
        int driveCable = 0;
        // Background read-ahead and smart speed. Both default on - see
        // AmigaEmulator::BridgeConfig; LIVE's optional 6th argument turns them off.
        bool autoCache = true;
        bool smartSpeed = true;
        // Off by default here too, and the LIVE field for it is optional and last, so
        // no existing command line can turn it on by accident.
        bool allowWrites = false;
    };
    struct FloppyBridgeStatus
    {
        bool bridged = false;
        // Assigned to live hardware but not yet handed to the core: the machine is
        // stopped, so the device opens when it next starts.
        bool pending = false;
        bool motor = false;
        int cyl = -1;
        bool media = false;
        // Write protection, split three ways on purpose. hostProtected is what the
        // padlock asked for; coreProtected is drive_writeprotected() asked of the core
        // live, which is what the emulated machine actually sees on CIAA /WPRO; drift
        // counts the frames the two disagreed. The three should read prot=on core=on
        // drift=0 on a locked slot - anything else is the bug, not the display.
        bool hostProtected = false;
        bool coreProtected = false;
        uint32_t protectDrift = 0;
        // Live slots only. allowWrites is the attach-time gate; bridgeProtected is what
        // the driver reports to the core, which already folds the gate, the device's
        // capability and the disk's tab together. Both false on a file-backed slot.
        bool allowWrites = false;
        bool bridgeProtected = false;
    };
    std::function<std::vector<std::string>()> fbDriverList;
    std::function<std::vector<std::string>()> fbComPorts;
    std::function<bool(const FloppyBridgeAttach&)> fbAttach;
    std::function<void(int drive)> fbDetach;
    std::function<FloppyBridgeStatus(int drive)> fbStatus;

    // AUDIO command — health of the emulator's audio path (ring level, underruns, producer
    // jitter). null -> the command reports unavailable.
    std::function<AudioLogSnapshot()> getAudioStats;

    // AUDREC command — start/stop the audio debug taps.
    // null -> the command reports unavailable.
    std::function<AudioRecordResult(const AudioRecordRequest&)> audioRecord;

    // SCEN command — record a session to a scenario file, or replay one. The same files
    // peekaboo_smoke verifies.
    // null -> the command reports unavailable.
    std::function<ScenarioResult(const ScenarioRequest&)> scenario;

    // PAULA command — chipset-side audio state (the four voices' registers, DMA, mute mask and
    // filter). The sibling of getAudioStats: that one measures the host path, this one the Amiga.
    // null -> the command reports unavailable.
    std::function<PaulaStateSnapshot()> getPaulaState;

    // BDSK command — arm/disarm the disk DMA breakpoint. access < 0 disarms; 0 any,
    // 1 read, 2 write. cyl/side/sector < 0 mean "any"; addrHi == 0 means "any address".
    // Returns false when the core does not support it.
    std::function<bool(int access, int cyl, int side, int sector, uint32_t addrLo, uint32_t addrHi)>
        armDiskBreak;

    // Read-and-clear: the disk transfer that tripped the disk breakpoint, if any.
    struct DiskBreakHit
    {
        bool isWrite = false;
        uint8_t drive = 0, cyl = 0, side = 0;
        uint32_t pc = 0, dskpt = 0, bytes = 0;
    };
    std::function<std::optional<DiskBreakHit>()> consumeDiskHit;

    // BPL / SPR / PF / DISPLAY commands — the debug display masks (which bitplanes, sprites
    // and playfields Denise is allowed to draw). Shared with the Display window and the MCP
    // display-mask tools; masking is cosmetic and never changes emulated behaviour. Both null
    // -> the commands report unavailable.
    std::function<DisplayMasks()> getDisplayMasks;
    std::function<void(const DisplayMasks&)> setDisplayMasks;

    // Custom-register snapshot, so DISPLAY can report the decoded screen mode alongside the
    // masks. null -> DISPLAY prints the masks only.
    std::function<bool(Custom&)> getCustomRegisters;

    // TRACE command — the per-instruction CPU trace buffer that feeds the profiler.
    // Both null -> TRACE reports unavailable.
    std::function<void(bool on)> setCpuTrace;
    std::function<bool()> isCpuTrace;

    // Memory annotations store (TM/TMS/TMD + disassembly comments). null -> the
    // annotation commands report unavailable and disasm lines carry no remarks.
    MemoryAnnotations* annotations = nullptr;

    // Memory lock / trainer store (PR). null -> PR reports unavailable. This is the same
    // object the Trainer window and the register windows mutate, which is what makes a lock
    // set in the UI show up in the PR listing and vice versa.
    MemoryLocks* locks = nullptr;
    // Segment bases for resolving seglist anchors; empty when no AmigaDOS process is running.
    std::function<std::vector<AmigaSegment>()> getSegments;
    // True when the loaded PuAE core exports the protection hooks. Null or false -> the PR
    // command warns that locks will have no effect until the core is rebuilt, rather than
    // letting the user think a lock is live when nothing is enforcing it.
    std::function<bool()> supportsProtections;

    // Disk I/O (RT/RS/RB/WT/WDS/WDB). Both null -> commands report unavailable.
    // readDiskTrack: returns bytes read (5632 for a full track) or -1 on error.
    std::function<int(int drive, int track, uint8_t* buf, int len)> readDiskTrack;
    // writeDiskBytes: patches the disk image at the given ADF byte offset and re-inserts.
    std::function<bool(int drive, uint32_t byteOffset, const uint8_t* data, uint32_t len)>
        writeDiskBytes;
};

// Action Replay 5 style monitor: parses lines from the type-over terminal,
// dispatches commands, and runs the interactive display/edit modes.
class ArCommandInterpreter
{
public:
    enum class Mode {
        None,
        MemHex,      // M/MM/MMM: Return writes back edits + emits next line
        MemAscii,    // N/NN/NNN
        MemBin,      // Y/YY/YYY: Return writes back edits + emits next line
        Disasm,      // D/DD/DDD: Return emits next instruction
        CustomRegs,  // E: Return writes edit + emits next register
        CopperList,  // C: Return emits next block of copper instructions
        ScrollHex,   // MQ: Tick() pages lines until end/Escape
        ScrollAscii, // NQ
        Assemble,    // A: type over emitted disasm lines, Return assembles in place
    };

    enum class LineKind {
        Blank,
        Command,
        MemHexEdit,
        MemAsciiEdit,
        MemBinEdit,
        RegisterEdit,
        CustomRegEdit,
        AddressedListing, // disasm/copper line: continue listing in mode, else no-op
    };

    ArCommandInterpreter(ArMemoryAccess* mem, ArHost host);

    // Output sink — every emitted line goes through this.
    std::function<void(const ArLine&)> emitLine;
    // Optional sink for refreshing the line currently being executed in place
    // (set per ExecuteLine call by the view). Falls back to emitLine when unset.
    std::function<void(const ArLine&)> rewriteLine;
    // Clears the console (same action as the Clear toolbar button). Set by the view.
    std::function<void()> clearScreen;

    // Return pressed on lineText.
    void ExecuteLine(const std::string& lineText);
    // Escape pressed.
    void CancelInteractive();
    // Per-frame: pages MQ/NQ scrolling dumps.
    void Tick();

    Mode GetMode() const { return m_mode; }
    bool IsScrolling() const { return m_mode == Mode::ScrollHex || m_mode == Mode::ScrollAscii; }
    // Column where the cursor should land on the last emitted interactive line.
    int PreferredCursorColumn() const;

    // Prints the AR-style break banner (registers + disasm at PC).
    void OnDebugBreak();

    // --- pure helpers, exposed for unit tests ---
    static LineKind ClassifyLine(const std::string& line);
    // Hex by default; $/0x hex, ! decimal, % binary.
    static std::optional<uint32_t> ParseNumber(const std::string& token);
    static std::vector<std::string> Tokenize(const std::string& s);
    // Splits "<string|bytes>,<addr-part>" at the last comma outside quotes.
    static bool SplitAtLastComma(const std::string& s, std::string& left, std::string& right);
    // Parses a quoted string or hex byte list into bytes.
    static bool ParsePattern(const std::string& s, std::vector<uint8_t>& out);

    static constexpr int kMemBytesPerLine = 16;
    static constexpr int kMemBinMaxBytes = 8; // Y/YY/YYY: max bytes per line
    static std::string FormatMemHexText(uint32_t addr, const uint8_t* bytes,
                                        const std::vector<bool>& valid);
    // Binary (0/1) memory line: "AAAAAAAA: bbbbbbbb bbbbbbbb ..." (count groups).
    static std::string FormatMemBinText(uint32_t addr, const uint8_t* bytes,
                                        const std::vector<bool>& valid, int count);
    // Parses a binary memory line; sets count to the number of byte groups found.
    static bool ParseMemBinLine(const std::string& line, uint32_t& addr,
                                uint8_t bytes[kMemBinMaxBytes], int& count);
    // asciiChars/asciiPresent (optional) receive the ASCII tail so edits made
    // there can be committed too.
    static bool ParseMemHexLine(const std::string& line, uint32_t& addr,
                                uint8_t bytes[kMemBytesPerLine], bool provided[kMemBytesPerLine],
                                char* asciiChars = nullptr, bool* asciiPresent = nullptr);
    static std::string FormatMemAsciiText(uint32_t addr, const uint8_t* bytes,
                                          const std::vector<bool>& valid, int64_t displayOffset);
    static bool ParseMemAsciiLine(const std::string& line, uint32_t& addr,
                                  char chars[kMemBytesPerLine]);

private:
    using Handler = void (ArCommandInterpreter::*)(const std::string& args);

    void Emit(const ArLine& line);
    void Emit(const std::string& text, ArColor color = ArColor::Default);
    void Error(const std::string& text);
    void Info(const std::string& text);
    // Emits a Highlight-colored table header followed by an ASCII rule of matching width.
    void EmitTableHead(const std::string& header);
    void RewriteCurrent(const ArLine& line);

    // Pure line builders shared by emission and in-place refresh
    ArLine MakeMemHexArLine(uint32_t addr) const;
    ArLine MakeMemAsciiArLine(uint32_t addr) const;
    ArLine MakeMemBinArLine(uint32_t addr, int count) const;
    ArLine MakeCustomRegArLine(uint32_t offset) const;
    // Colored disasm line + next-instruction address; false if disasm failed
    bool MakeDisasmArLine(uint32_t addr, ArLine& out, uint32_t& nextAddr) const;
    // M-format line with bytes in [hlStart, hlEnd) highlighted (F/FS -L dumps)
    ArLine MakeMemHexArLineHighlighted(uint32_t addr, uint32_t hlStart, uint32_t hlEnd) const;

    bool RequireRunning();
    void StateChanged();
    // Runs a UAE debugger command and splits the captured console output into
    // lines (trailing CR stripped). Empty if execUaeCommand is unavailable.
    std::vector<std::string> CaptureUaeLines(const std::string& cmd);

    // Line-kind handlers
    void HandleCommand(const std::string& line);
    void HandleMemHexEdit(const std::string& line);
    void HandleMemAsciiEdit(const std::string& line);
    void HandleMemBinEdit(const std::string& line);
    void HandleRegisterEdit(const std::string& line);
    void HandleCustomRegEdit(const std::string& line);
    void HandleAddressedListing();
    void HandleAssembleLine(const std::string& line);

    // Interactive emission
    void EmitMemHexLine(uint32_t addr);
    void EmitMemAsciiLine(uint32_t addr);
    void EmitMemBinLine(uint32_t addr);
    bool EmitDisasmLine(uint32_t addr); // false if disassembly failed
    void EmitCustomRegLine(uint32_t offset);
    bool EmitCopperLine(uint32_t addr); // false at end-of-list marker
    void EmitRegisters(const ArCpuState& st);
    // "NAME=VALUE" pairs, four to a line, for the registers in `ids` this CPU has; the rest are
    // listed on one "Not on this 68000:" line. Returns how many were present.
    int EmitExtRegisterPairs(const CpuExtendedState& ext, std::initializer_list<CpuRegId> ids);
    bool SetExtRegister(const CpuExtendedState& ext, const CpuRegDef& def, uint32_t value,
                        std::string& error);
    void EmitDisasmAtPc();

    // Commands
    void CmdMem(const std::string& args, int lines, bool ascii); // M/MM/MMM, N/NN/NNN
    void CmdMemBin(const std::string& args, int lines);          // Y/YY/YYY
    void CmdBinSize(const std::string& args);                    // YS
    void CmdMemQuick(const std::string& args, bool ascii);       // MQ/NQ
    void CmdAsciiOffset(const std::string& args);                // NO
    void CmdDisasm(const std::string& args, int lines);          // D/DD/DDD
    void CmdRegisters(const std::string& args);                  // R
    void CmdCustomRegs(const std::string& args);                 // E
    void CmdCopper(const std::string& args);                     // C
    void CmdGo(const std::string& args);                         // G/X
    void CmdStart(const std::string& args);                      // START
    void CmdStop(const std::string& args);                       // STOP
    void CmdStep(const std::string& args, bool over);            // ST/TR
    void CmdTraceUntil(const std::string& args);                 // TA
    void CmdBreakSet(const std::string& args);                   // BS
    void CmdBreakDelete(const std::string& args);                // BD
    void CmdBreakList(const std::string& args);                  // B
    void CmdBreakDeleteAll(const std::string& args);             // BDA
    void CmdKprintfSet(const std::string& args);                 // BKS
    void CmdKprintfDelete(const std::string& args);              // BKD
    void CmdKprintfList(const std::string& args);                // BK
    void CmdKprintfDeleteAll(const std::string& args);           // BKDA
    void CmdWatchSet(const std::string& args);                   // MS
    void CmdWatchDelete(const std::string& args);                // MD
    void CmdWatchList(const std::string& args);                  // MW
    void CmdWatchDeleteAll(const std::string& args);             // MDA
    void CmdFind(const std::string& args, bool caseInsensitive); // F/FS
    void CmdFindRelative(const std::string& args);               // FR
    void CmdFindAccess(const std::string& args, bool quick);     // FA/FAQ
    void CmdCompare(const std::string& args);                    // COMP
    void CmdTransfer(const std::string& args);                   // TRANS
    void CmdExchange(const std::string& args);                   // EX
    void CmdFill(const std::string& args);                       // O
    void CmdWriteString(const std::string& args);                // WS
    void CmdMemCode(const std::string& args);                    // MEMCODE
    void CmdAdd(const std::string& args);                        // ADD
    void CmdLoadMem(const std::string& args);                    // LM
    void CmdSaveMem(const std::string& args);                    // SM
    void CmdAssemble(const std::string& args);                   // A
    void CmdBreakBootblock(const std::string& args);             // TRB/FB
    void CmdExceptionBreak(const std::string& args);             // EB
    void CmdBeamBreak(const std::string& args);                  // BL
    void CmdCopperBreak(const std::string& args);                // BC
    void CmdProtect(const std::string& args);                    // PR/PROTECT
    void CmdTrainerStart(const std::string& args);               // TS
    void CmdTrainerContinue(const std::string& args);            // T
    void CmdTrainerExit(const std::string& args);                // TX
    void CmdTrainerFind(const std::string& args, bool remove);   // TF/TFD
    void CmdDeepStart(const std::string& args);                  // TDS
    void CmdDeepChanged(const std::string& args);                // TDC
    void CmdDeepList(const std::string& args);                   // TD
    void CmdDeepInspect(const std::string& args);                // TDI
    void CmdDeepDelete(const std::string& args);                 // TDD
    void CmdDeepExit(const std::string& args);                   // TDX
    void CmdLoad(const std::string& args);                       // LOAD
    void CmdList(const std::string& args);                       // LIST
    void CmdReset(const std::string& args);                      // RESET
    void CmdTurbo(const std::string& args);                      // TURBO/WARP
    void CmdStepFrame(const std::string& args);                  // SF
    void CmdState(const std::string& args);                      // STATE
    void CmdRunUntilFrame(const std::string& args);              // RUNTO
    void CmdEndFrame(const std::string& args);                   // EF
    void CmdPause(const std::string& args);                      // PAUSE
    void CmdDocs(const std::string& args);                       // DOCS
    void CmdHardDrives(const std::string& args);                 // HD
    void CmdModel(const std::string& args);                      // MODEL
    void CmdWhdload(const std::string& args);                    // WHD
    void CmdCd(const std::string& args);                         // CD
    void CmdMachine(const std::string& args);                    // MACHINE
    void CmdSerial(const std::string& args);                     // SERIAL
    void CmdParallel(const std::string& args);                   // PARALLEL
    void CmdMidi(const std::string& args);                       // MIDI
    void CmdNetwork(const std::string& args);                    // NETWORK
    void CmdSoundCard(const std::string& args);                  // SOUNDCARD
    void CmdCloseDoc(const std::string& args);                   // CLOSE
    void CmdHelp(const std::string& args);                       // HELP/?
    void CmdKeys(const std::string& args);                       // KEYS
    void CmdClear(const std::string& args);                      // CLS/CLEAR
    void CmdVersion(const std::string& args);                    // VERSION
    void CmdFrame(const std::string& args);                      // FRAME
    void CmdHistory(const std::string& args);                    // HISTORY
    void CmdRewind(const std::string& args);                     // REWIND
    void CmdGoto(const std::string& args);                       // GOTO
    void CmdFrameDiff(const std::string& args);                  // DIFF
    void CmdLoop(const std::string& args);                       // LOOP
    void CmdReadTrack(const std::string& args);                  // RT
    void CmdReadSectors(const std::string& args);                // RS
    void CmdReadBytes(const std::string& args);                  // RB
    void CmdWriteTrack(const std::string& args);                 // WT
    void CmdWriteSectors(const std::string& args);               // WDS
    void CmdWriteBytes(const std::string& args);                 // WDB
    void CmdBootCheck(const std::string& args);                  // BOOTCHK
    void CmdDataCheck(const std::string& args);                  // DATACHK
    void CmdBamCheck(const std::string& args);                   // BAMCHK
    void CmdCalc(const std::string& args);                       // ?
    void CmdCrc(const std::string& args, bool crc32);            // CRC16/CRC32
    void CmdCia(const std::string& args);                        // W
    void CmdPalette(const std::string& args);                    // PALETTE
    void CmdStack(const std::string& args);                      // STACK
    void CmdCall(const std::string& args);                       // CALL
    void CmdProfile(const std::string& args);                    // PROFILE/PROF
    void CmdCallStack(const std::string& args);                  // CALLSTACK/CS
    void CmdBlitLog(const std::string& args);                    // BLIT/BLITLOG
    void CmdRegLog(const std::string& args);                     // REGLOG/REGS
    void CmdAudioStats(const std::string& args);                 // AUDIO
    void CmdAudioRecord(const std::string& args);                // AUDREC
    void CmdScenario(const std::string& args);                   // SCEN
    void CmdPaulaState(const std::string& args);                 // PAULA
    void CmdDiskLog(const std::string& args);                    // DSKLOG
    void CmdFloppyBridge(const std::string& args);               // LIVE
    // EXO/EXOCET: the Exocet sync tracker (ArCommandExocet.cpp)
    void CmdExocet(const std::string& args);
    void EmitExocetJson(const nlohmann::json& r);
    void CmdDiskBreak(const std::string& args);      // BDSK
    void CmdTrace(const std::string& args);          // TRACE
    void CmdCoverage(const std::string& args);       // COV
    void CmdCpuExtRegs(char kind);                   // RC/RF/RM
    void CmdExecBase(const std::string& args);       // EXECBASE
    void CmdAvail(const std::string& args);          // AVAIL
    void CmdLibraries(const std::string& args);      // LIBRARIES
    void CmdResources(const std::string& args);      // RESOURCES
    void CmdDevices(const std::string& args);        // DEVICES
    void CmdTasks(const std::string& args);          // TASKS
    void CmdPorts(const std::string& args);          // PORTS
    void CmdInterrupts(const std::string& args);     // INTERRUPTS
    void CmdExceptions(const std::string& args);     // EXCEPTIONS
    void CmdChipRegs(const std::string& args);       // CHIPREGS
    void CmdDChip(const std::string& args);          // DCHIP
    void CmdKickVer(const std::string& args);        // KICKVER
    void CmdTagMem(const std::string& args);         // TM
    void CmdTagLabel(const std::string& args);       // TL
    void CmdTagMemShow(const std::string& args);     // TMS
    void CmdTagMemDelete(const std::string& args);   // TMD
    void CmdTagBlock(const std::string& args);       // TB
    void CmdTagBlockDelete(const std::string& args); // TBD

    // BPL/SPR/PF address the same kind of toggle group, so they share one handler; this
    // describes which one is being addressed.
    struct DisplayMaskGroup
    {
        const char* cmd;               // console command name ("BPL")
        const char* title;             // status heading ("Bitplanes")
        const char* item;              // per-item prefix ("BPL" -> BPL1..BPL8)
        int count;                     // number of toggles in the group
        int firstIndex;                // index printed for bit 0 (BPL1, SPR0, PF1)
        uint8_t allMask;               // mask with every toggle on
        uint8_t DisplayMasks::* field; // the DisplayMasks member this group edits
    };
    static const DisplayMaskGroup kBitplaneGroup;
    static const DisplayMaskGroup kSpriteGroup;
    static const DisplayMaskGroup kPlayfieldGroup;

    void CmdDisplayMask(const std::string& args, const DisplayMaskGroup& group); // BPL/SPR/PF
    void CmdDisplay(const std::string& args);                                    // DISPLAY
    // "Bitplanes   $FF  BPL1:on BPL2:on ..." status line for one group.
    void EmitDisplayMaskLine(const DisplayMaskGroup& group, uint8_t mask);
    // Notes the composed bitplane mask when hiding a playfield also hides planes.
    void EmitEffectiveBitplaneNote(const DisplayMasks& masks);
    // Reads the masks through the host; emits an error and returns false if unavailable.
    bool GetDisplayMasksOrError(DisplayMasks& out);

    // Shared parser for the `<addr> "<text>"` form of TM/TL/TB.
    bool ParseTagArgs(const std::string& args, const char* cmd, const char* what, uint32_t& addrOut,
                      std::string& textOut);

    // Exec-structure command helpers (EXECBASE/LIBRARIES/TASKS/...).
    // ExecBaseAddr: *($00000004), 0 if unreadable. Emits an error and returns 0
    // when the OS structures aren't available.
    uint32_t ExecBaseAddr();
    // Accessors over m_mem in the shape ExecIntrospect/SegListResolver expect.
    exec::Readers ExecReaders() const;
    std::string ReadCString(uint32_t addr, size_t maxLen = 80) const;
    // Walks an Exec doubly-linked List at listAddr, calling fn for each node
    // (stops at the tail or after maxNodes for safety).
    void ForEachExecNode(uint32_t listAddr, int maxNodes,
                         const std::function<void(uint32_t node)>& fn) const;
    // Shared "Address Version Open Name" listing for LIBRARIES/DEVICES.
    void DumpLibraryList(uint32_t listAddr, const char* emptyMsg);
    // Resolves a custom-register name (case-insensitive) to its $DFF000 byte
    // offset, or nullopt if unknown.
    std::optional<uint32_t> ResolveCustomReg(const std::string& name) const;

    // Search helpers
    void ReportMatches(const std::vector<uint32_t>& matches);
    bool ResolveSearchRange(const std::vector<std::string>& tokens, size_t firstTokenIdx,
                            std::vector<std::pair<uint32_t, uint32_t>>& ranges);

    // Disk I/O helpers
    // ParseDiskAddr: like ParseNumber but also accepts T<n>/S<n> (track/sector offsets from
    // m_diskBufAddr). Returns nullopt if the token is unparseable or the buffer isn't set.
    std::optional<uint32_t> ParseDiskAddr(const std::string& token) const;
    // AllocateDiskBuffer: find room at the top of the largest writable RAM region.
    uint32_t AllocateDiskBuffer(size_t size) const;
    // DiskReadRange/DiskWriteRange: byte-level disk I/O bridging to/from Amiga memory.
    bool DiskReadRange(uint32_t diskByteOffset, uint32_t numBytes, uint32_t amigaDest);
    bool DiskWriteRange(uint32_t diskByteOffset, uint32_t numBytes, uint32_t amigaSrc);

    // Trainer helpers
    bool IsGluedTrainerCommand(const std::string& cmd, const std::string& args); // "TS6"/"T5"
    void EmitTrainerAddresses(const std::vector<uint32_t>& addrs);
    // Trainer status/announcement line (mode changes, prompts). Non-blue so blue
    // stays reserved for memory addresses.
    void EmitTrainerStatus(const std::string& text);
    // "<prefix><hex>" with the hex tail address-coloured (blue) and prefix neutral.
    void EmitLabeledAddress(const std::string& prefix, uint32_t addr, int hexDigits);
    bool DeepSnapshot(); // snapshot all writable regions; false if none

    ArMemoryAccess* m_mem = nullptr;
    ArHost m_host;

    // Amiga library-call annotation (e.g. "  ; exec/DoIO()"); lazily loaded from
    // assets/amiga_lvos.json on first disassembly. Returns the comment suffix for
    // a JSR/JMP (aN,-$xxx) line, resolving the target library from the live a6.
    mutable LibraryCallAnnotator m_libAnnotator;
    mutable bool m_libAnnotatorLoaded = false;
    std::string LibraryCallSuffix(const std::string& mnemonic) const;

    // Disk constants (Amiga DD: 80 cyl × 2 sides × 11 sectors)
    static constexpr int kDiskSectorBytes = 512;
    static constexpr int kDiskTrackSectors = 11;
    static constexpr int kDiskTrackBytes = kDiskTrackSectors * kDiskSectorBytes; // 5632
    static constexpr int kDiskNumTracks = 160;
    static constexpr int kDiskNumSectors = kDiskNumTracks * kDiskTrackSectors; // 1760
    static constexpr uint32_t kDiskTotalBytes = (uint32_t)kDiskNumSectors * kDiskSectorBytes;

    Mode m_mode = Mode::None;
    uint32_t m_curAddr = 0;            // current position for M/N/E/C/A modes
    std::optional<uint32_t> m_endAddr; // automatic-mode end for M/N/D
    uint32_t m_lastDisasmAddr = 0;     // D with no arg continues here
    bool m_hasLastDisasmAddr = false;
    int64_t m_asciiOffset = 0;             // NO
    int m_binBytesPerLine = 2;             // Y/YY/YYY bytes per line (YS sets this)
    uint32_t m_scrollEnd = 0xFFFFFFFF;     // MQ/NQ stop address
    std::optional<uint32_t> m_tempTraceBp; // TA temp breakpoint, removed on break

    uint32_t m_diskBufAddr = 0; // address of last auto-allocated disk buffer (T/S refs)

    // TS/T trainer search (survives across freezes until TX)
    struct TrainerState
    {
        bool active = false;
        std::vector<uint32_t> candidates; // narrowed lives-address working set
    };
    TrainerState m_trainer;

    // TDS/TDC deep trainer (changed-value search for energy-bar style counters)
    struct DeepTrainerState
    {
        bool active = false;
        bool firstPass = true; // candidates implicitly "all of RAM" until first TDC
        std::vector<uint32_t> candidates;
        struct Snap
        {
            uint32_t base = 0;
            std::vector<uint8_t> bytes;
        };
        std::vector<Snap> snapshot; // one per writable region
    };
    DeepTrainerState m_deep;
};

} // namespace adm::core
