#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "AddressDescriber.h"
#include "AudioLogSnapshot.h"
#include "BlitLogSnapshot.h"
#include "CpuExtState.h"
#include "CpuTraceHistory.h"
#include "CrassMemMap.h"
#include "CrassProfile.h"
#include "DiskLogSnapshot.h"
#include "DosBreakpoints.h"
#include "ExecIntrospect.h"
#include "ExocetBlock.h"
#include "ExocetQuery.h"
#include "FrameHistory.h"
#include "KprintfBreakpoints.h"
#include "LibraryCallTracer.h"
#include "MemorySnapshotStore.h"
#include "PaulaStateSnapshot.h"
#include "ProfileSnapshot.h"
#include "RegLogSnapshot.h"
#include "ResidentScanner.h"
#include "UiLayoutSnapshot.h"
#include "WatchpointTypes.h"

namespace adm {
class AmigaEmulator;
}

namespace adm::core {

// get_extended_cpu_registers' result for one CPU state. group: "control" (stack + system),
// "mmu", "fpu" or "all". Only registers the CPU has carry values; the rest are listed in
// `unsupported` with the CPUs that do have them.
nlohmann::json BuildExtendedCpuRegistersJson(const CpuExtendedState& ext, const std::string& group);

class AdfHandler;
class DiskSearcher;
class MemoryAnnotations;
class EmulatorStateStore;
class ExocetPack;
class ExocetSession;

// What the reverse-engineering tools need to know about the loaded debug symbols.
struct DebugSymbolsInfo
{
    bool loaded = false;
    std::string path;
    size_t functionCount = 0;
};

// The mounted disk document the ADF filesystem tools read. All pointers may be
// null, which the tools report as "no disk is open".
struct DiskDocumentAccess
{
    AdfHandler* volume = nullptr;
    DiskSearcher* searcher = nullptr;
    std::string path;       // host path of the image
    std::string volumeName; // Amiga volume label
};

// Callbacks the MCP tools use to reach the emulator, breakpoint store, console and documents.
// All hooks are optional; a missing hook makes the relevant tool report it is unavailable. Every
// hook is invoked on the main thread (the dispatcher runs inside McpServer::DrainPending).
// Mirrors the ArHost pattern used by ArCommandInterpreter.
struct McpHost
{
    std::function<adm::AmigaEmulator*()> emulator;
    MemoryAnnotations* annotations = nullptr;
    // Named machine-state snapshots (save_state / load_state / list_states).
    // null -> those tools report unavailable.
    EmulatorStateStore* states = nullptr;
    // Path of the document currently mounted, recorded with a snapshot so a restore onto a
    // different disk can be refused instead of silently producing a broken machine.
    std::function<std::string()> activeDocumentPath;
    // Re-read breakpoint/watchpoint state from the core after something (a state restore)
    // may have changed it underneath the host. null -> skipped.
    std::function<void()> refreshBreakpoints;

    // Execution state / control (wired to EmulatorView).
    std::function<bool()> isRunning;
    std::function<bool()> isPaused;
    std::function<void()> pause;
    std::function<void()> resume;
    std::function<void()> stepInto;
    std::function<bool()> stepOver;
    // Step one full emulated frame (emulator must be paused). null -> unavailable.
    std::function<void()> stepFrame;
    // Total emulated frames since reset. null -> unavailable.
    std::function<uint64_t()> getFrameCount;
    // Run until the emulated frame counter reaches targetFrame, then auto-pause. turbo
    // enables warp mode for the run. Returns false if not running or the target is
    // at/behind the current frame. null -> unavailable. (Wired by WireEmulatorHostHooks.)
    std::function<bool(uint64_t targetFrame, bool turbo)> runUntilFrame;

    // Emulator session start/stop and end-of-frame (mirror the AR console START/STOP/EF).
    std::function<void()> startEmulator; // run the active document (Run/F5). null -> unavailable.
    std::function<void()> stopEmulator;  // stop the running emulator session. null -> unavailable.
    std::function<void()> endFrame;      // complete the current mid-execution frame (paused only).

    // Breakpoints / watchpoints (wired to BreakpointsView so they stay in sync with the window).
    std::function<bool(uint32_t addr)> addBreakpoint;
    // Conditional / smart PC breakpoint (shared signature with ArHost::addSmartBreakpoint).
    std::function<bool(uint32_t addr, bool condEnabled, const std::string& reg,
                       const std::string& oper, uint32_t value, uint32_t mask, int ignoreCount,
                       bool oneShot)>
        addSmartBreakpoint;
    std::function<bool(uint32_t addr)> removeBreakpoint;
    std::function<std::vector<std::pair<uint32_t, bool>>()> listBreakpoints; // addr, enabled
    std::function<bool(uint32_t addr)> addWatchpoint;
    std::function<bool(uint32_t addr)> removeWatchpoint;
    std::function<std::vector<uint32_t>()> listWatchpoints;
    // Full watchpoint API: arbitrary range, read/write/execute access, access source,
    // value condition and log mode. addWatchpointFull returns the allocated memwatch slot
    // or -1 with outError set. null -> the tools fall back to reporting unavailable.
    std::function<int(const WatchpointSpec& spec, std::string& outError)> addWatchpointFull;
    std::function<bool(int slot)> removeWatchpointSlot;
    std::function<std::vector<WatchpointInfo>()> listWatchpointsFull;
    // Recorded log-mode hits. slot < 0 means every slot, limit <= 0 means all of them.
    std::function<std::vector<WatchpointHit>(int slot, int limit, bool clear)> getWatchpointLog;

    // Break-on-KPrintF-text (shared signatures with ArHost — same store behind both).
    // addKprintfBreakpoint returns the new rule id, or 0 for a duplicate / uncompilable regex.
    std::function<int(const std::string& text, KprintfMatch match, bool caseSensitive,
                      bool oneShot)>
        addKprintfBreakpoint;
    std::function<bool(int id)> removeKprintfBreakpoint;
    std::function<void()> clearKprintfBreakpoints;
    std::function<std::vector<KprintfBreakpoint>()> listKprintfBreakpoints;

    // Console passthrough: runs an AR5/UAE command, echoes it into the AR console view and
    // returns the captured text.
    std::function<std::string(const std::string& cmd)> execConsole;

    // Source-level profiler snapshot (get_profile tool). null -> tool reports unavailable.
    std::function<ProfileSnapshot(const ProfileQuery&)> getProfile;

    // Current frame's blitter operations (get_blitter_log tool). limit <= 0 returns all.
    // null -> tool reports unavailable.
    std::function<BlitLogSnapshot(int limit)> getBlitLog;

    // The running Amiga program's own raster-line profiler, when it is a Crass-engine build
    // with PROFILE compiled in (get_crass_profile tool). Nothing to do with getProfile above,
    // which is the host-side source-level profiler over the CPU trace.
    // null -> tool reports unavailable.
    std::function<CrassProfileStats()> getCrassProfile;

    // The running program's Exocet block (get_exocet tool) and the active document's sync pack.
    // null -> tool reports unavailable.
    std::function<ExocetSnapshot()> getExocet;
    // The disk's memory map over exec's memory (get_exocet_memory). null -> unavailable.
    std::function<ExocetMemoryState()> getExocetMemory;
    // The Exocet window's session: editing, the push, proofing, the issues (set_exocet_key,
    // delete_exocet_key, push_exocet, get_exocet_proof, get_exocet_issues). null -> unavailable.
    std::function<ExocetSession*()> exocetSession;

    // ---- Frame history (rewind) ----
    // Same timeline the HISTORY / REWIND / GOTO / LOOP / DIFF console commands drive.
    // null -> the tools report the timeline as unavailable.
    std::function<FrameHistoryStats()> getFrameHistory;
    std::function<uint64_t()> getFrameHistoryPosition;
    std::function<std::string(uint64_t frame)> frameHistorySeek;
    std::function<std::string(uint64_t count)> frameHistoryRewind;
    std::function<std::string(uint64_t a, uint64_t b)> frameHistoryLoop;
    // Transport over the recorded timeline: play/pause, one-frame steps, direction, speed,
    // and the state of all four for get_frame_history.
    std::function<void()> frameHistoryClear;
    std::function<void(bool playing)> frameHistoryPlay;
    std::function<std::string(int delta)> frameHistoryStep;
    std::function<void(int mode)> frameHistoryPlayMode;
    std::function<void(double speed)> frameHistoryPlaySpeed;
    struct FrameHistoryPlayback
    {
        bool playing = false;
        int mode = 0; // 0 forward, 1 reverse, 2 ping-pong
        double speed = 1.0;
        bool looping = false;
        uint64_t loopStart = 0;
        uint64_t loopEnd = 0;
    };
    std::function<FrameHistoryPlayback()> frameHistoryPlayback;
    std::function<std::string(uint64_t a, uint64_t b, std::vector<FrameDiffRun>& out,
                              size_t maxRuns, bool& truncated)>
        frameHistoryDiff;
    std::function<RegLogSnapshot(int limit)> getRegLog;

    // Session-wide disk DMA traffic (get_disk_log tool). null -> tool reports unavailable.
    std::function<DiskLogSnapshot(const DiskLogQuery&)> getDiskLog;

    // Audio-path health (get_audio_stats tool). null -> tool reports unavailable.
    std::function<AudioLogSnapshot()> getAudioStats;
    // get_paula_state — the chipset side of the audio path (the four voices).
    std::function<PaulaStateSnapshot()> getPaulaState;

    // Audio debug capture taps (start_audio_record / stop_audio_record).
    // null -> tools report unavailable.
    std::function<AudioRecordResult(const AudioRecordRequest&)> audioRecord;

    // Disk DMA breakpoint (add_disk_breakpoint / remove_disk_breakpoint).
    // access: -1 disarm, 0 any, 1 read, 2 write. cyl/side/sector < 0 mean "any";
    // addrHi == 0 means "any address". Returns false when unsupported by the core.
    std::function<bool(int access, int cyl, int side, int sector, uint32_t addrLo, uint32_t addrHi)>
        armDiskBreak;

    // Documents. force replaces a dirty active document instead of refusing.
    // reloadIfOpen: re-read the file even when this path is already open. MCP always
    // passes true - an edit/build/test loop must never be served the previous build.
    std::function<bool(const std::string& path, bool force, bool reloadIfOpen)> loadDocument;
    std::function<std::vector<std::string>()> listDocuments;
    std::function<void(int idx)> runDocument;
    // Close the open document at 0-based index. force closes even with unsaved changes;
    // returns false if it has unsaved changes and force is false. null -> unavailable.
    std::function<bool(int idx, bool force)> closeDocument;

    // --- Reverse-engineering tools (see McpReTools.h) ------------------------
    // Each of these may be null/absent, which makes the matching tool report
    // that it is unavailable rather than failing.

    // Accessors onto emulated memory, in the shape ExecIntrospect/SegListResolver
    // expect. Used by describe_address, the resident scanner and the library
    // tracer.
    std::function<exec::Readers()> execReaders;
    // The live memory map, for describe_address and the stolen-memory check.
    std::function<std::vector<AddressRegion>()> memoryRegions;
    // The same regions with their host pointers, for snapshot_memory.
    std::function<std::vector<LiveMemoryRegion>()> liveMemoryRegions;
    // Resolves a link-time address to a function via the loaded debug symbols.
    std::function<bool(uint32_t linkAddr, std::string& outName, uint32_t& outBase,
                       uint32_t& outSize)>
        resolveSymbol;
    std::function<DebugSymbolsInfo()> debugSymbolsInfo;
    /**
     * @brief Where a source line's code actually lives, and whether it has any.
     *
     * The reverse of resolveSymbol. Reported as JSON rather than a struct because the
     * interesting part is the *status* -- "no code", "moved forward", "file not covered" --
     * which is what lets a caller explain a breakpoint that will not bind instead of leaving
     * the user guessing. Null when no debug line table is loaded.
     */
    std::function<nlohmann::json(const std::string& file, uint32_t line, bool stmtOnly)>
        resolveSourceLine;
    /**
     * @brief Whether a source line actually executed, in which of its copies, and with what.
     *
     * The retrospective counterpart to a breakpoint. On an optimised build most lines cannot be
     * broken on at all, so this is frequently the only way to answer "did this run". frame 0
     * means every recorded frame.
     */
    std::function<nlohmann::json(const std::string& file, uint32_t line, uint64_t frame)>
        lineCoverage;
    // Names/descriptions for a $DFF000 byte offset.
    std::function<std::string(uint32_t offset)> customRegName;
    std::function<std::string(uint32_t offset)> customRegDesc;
    // The mounted ADF the disk filesystem tools read.
    std::function<DiskDocumentAccess()> activeDiskDocument;

    // --- Peekaboo's own UI (see McpUiTools.h) --------------------------------
    // These describe and drive the HOST application - its windows, docks, panels and workspaces -
    // never the emulated machine. Windows are addressed by their View-menu name ("Sprites", "CIA")
    // or by their ImGui id; both resolve. As everywhere else in this struct, a null hook makes the
    // matching tool report that it is unavailable.

    // Windows, panels and the dock trees, all in one read.
    std::function<UiLayoutSnapshot()> getUiLayout;
    // Write through a window's open flag. Fails for an unknown name or a draw-list overlay.
    std::function<bool(const std::string& window, bool open, std::string& err)> setWindowOpen;
    // Raise a window, selecting the tab of every panel that contains it.
    std::function<bool(const std::string& window, std::string& err)> focusWindow;
    std::function<bool(const UiDockRequest& req, std::string& err)> dockWindow;

    // Application screenshots are deferred: the request is parked and serviced at end-of-frame,
    // because the MCP dispatcher runs before anything has been drawn. requestAppCapture returns
    // false when a capture is already in flight; pollAppCapture returns nullopt until it lands.
    std::function<bool(const UiCaptureRequest& req, std::string& err)> requestAppCapture;
    std::function<std::optional<UiCaptureResult>()> pollAppCapture;
    // Resolve a window name to the capture rect covering it.
    std::function<bool(const std::string& window, UiCaptureRequest& out, std::string& err)>
        windowRect;

    std::function<std::vector<std::string>()> listWorkspaces;
    std::function<bool(const std::string& name, std::string& err)> loadWorkspace;
    std::function<bool(const std::string& name, std::string& err)> saveWorkspace;
    // Screen profile ("hires"/"lores"): the live one, switching it, and whether a workspace has a
    // layout of its own in the live profile (false: loading it scales the other profile's).
    std::function<std::string()> getScreenProfile;
    std::function<bool(const std::string& profile, std::string& err)> setScreenProfile;
    std::function<bool(const std::string& name)> workspaceHasOwnLayout;

    // Peekaboo's own log (the Log window), not anything the guest printed.
    std::function<UiLogSnapshot(int limit, const std::string& level)> getUiLog;
    // The live layout in imgui.ini format. Peekaboo keeps no imgui.ini on disk.
    std::function<std::string()> getImguiIni;

    // Host-side input injection. uiInputBusy reports whether queued events are still playing, so a
    // follow-up screenshot can wait for the click to actually land.
    std::function<bool(const UiInputEvent& ev, std::string& err)> injectUiInput;
    std::function<bool()> uiInputBusy;

    // Long-lived stores owned by Application.
    MemorySnapshotStore* memorySnapshots = nullptr;
    CpuTraceHistory* cpuTraceHistory = nullptr;
    LibraryCallTracer* libraryTracer = nullptr;
    DosBreakpoints* dosBreakpoints = nullptr;

    // Fired after any tool that may have mutated state, so dependent views refresh.
    std::function<void()> onStateChanged;

    std::string appVersion;
};

// Dispatches MCP tool calls against the live emulator + annotation store and records an activity
// log of everything Claude does. Built once by Application; Handle()/Manifest() are the entry
// points called from McpServer.
class McpToolHandler
{
public:
    explicit McpToolHandler(McpHost host);

    // Executes a tool by name. Always returns a JSON object (an {"error": ...} object on failure).
    // Runs on the main thread.
    nlohmann::json Handle(const std::string& name, const nlohmann::json& args);

    // The tool manifest (name/description/inputSchema). Pure + thread-safe — may be called from
    // the server thread. Built once in the constructor.
    nlohmann::json Manifest() const { return m_manifest; }

    // Exocet's tools and the crAss engine's: left out of the manifest and refused by name unless
    // private features are on (core/PrivateFeatures.h)
    static bool IsPrivateTool(const std::string& name);

    // One logged tool invocation, newest last. Read on the main thread by McpActivityView.
    struct ActivityEntry
    {
        std::string time; // "HH:MM:SS"
        std::string tool;
        std::string args;   // compact JSON summary
        std::string result; // compact JSON / "ok" / error text
        double durationMs = 0.0;
        bool ok = true;
    };

    const std::deque<ActivityEntry>& Activity() const { return m_activity; }
    void ClearActivity() { m_activity.clear(); }

private:
    nlohmann::json Dispatch(const std::string& name, const nlohmann::json& args);

    // One dispatcher per tool category — the former ~1,900-line Dispatch if/else chain, split
    // along its section comments. Each returns nullopt for tool names it does not own.
    std::optional<nlohmann::json>
    DispatchStatusTools(const std::string& name, const nlohmann::json& args, AmigaEmulator* emu);
    std::optional<nlohmann::json> DispatchAnnotationTools(const std::string& name,
                                                          const nlohmann::json& args,
                                                          AmigaEmulator* emu);
    std::optional<nlohmann::json>
    DispatchWriteTools(const std::string& name, const nlohmann::json& args, AmigaEmulator* emu);
    std::optional<nlohmann::json>
    DispatchExecutionTools(const std::string& name, const nlohmann::json& args, AmigaEmulator* emu);
    std::optional<nlohmann::json>
    DispatchDocumentTools(const std::string& name, const nlohmann::json& args, AmigaEmulator* emu);
    std::optional<nlohmann::json>
    DispatchConsoleTools(const std::string& name, const nlohmann::json& args, AmigaEmulator* emu);
    std::optional<nlohmann::json>
    DispatchProfilerTools(const std::string& name, const nlohmann::json& args, AmigaEmulator* emu);

    void BuildManifest();
    void Record(const std::string& tool, const nlohmann::json& args, const nlohmann::json& result,
                double ms);

    McpHost m_host;
    nlohmann::json m_manifest;
    nlohmann::json m_customRegsDef;

    // In-flight run_until_frame(wait: true). The request is re-dispatched once per frame (see
    // McpServer::kRetryKey) until the run ends or the budget expires, so the state of the wait
    // has to outlive a single Dispatch() call.
    struct PendingFrameWait
    {
        bool active = false;
        uint64_t target = 0;
        std::chrono::steady_clock::time_point deadline;
    };
    PendingFrameWait m_frameWait;

    std::deque<ActivityEntry> m_activity;
    static constexpr size_t kMaxActivity = 500;
};

} // namespace adm::core
