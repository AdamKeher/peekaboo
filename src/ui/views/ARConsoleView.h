#pragma once

#include <imgui.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "../../core/ArCommandInterpreter.h"
#include "../../core/ArMemoryAccess.h"
#include "../../core/Disassembler.h"
#include "ARConsoleTerminal.h"
#include "AddressActions.h"

namespace adm::core {
class ExocetSession;
class MemoryAnnotations;
class MemoryLocks;
} // namespace adm::core

namespace adm::ui {

class EmulatorView;
class BreakpointsView;

// Action Replay style debugger/monitor console: a type-over terminal backed by
// ArCommandInterpreter, wired to the emulator and the Breakpoints window.
class ARConsoleView
{
public:
    ARConsoleView(EmulatorView* emulatorView, BreakpointsView* breakpointsView,
                  core::MemoryAnnotations* annotations, core::MemoryLocks* locks);

    void Draw();
    void Clear();
    void Restore(bool open);
    void ToggleOpen();
    bool* GetOpenPtr() { return &m_open; }

    // Appends a line of plain output (used by DebuggerView facade callers).
    void AddOutputLine(const std::string& line);

    // Prints the AR-style break banner (registers + disasm at PC).
    void OnDebugBreak();

    // Runs an AR5/UAE command exactly as if typed: echoes it into the terminal and emits its
    // output, while also capturing the emitted line text and returning it. Used by the MCP
    // exec_console tool so Claude gets the text and the user watches it run. Main thread only.
    std::string RunCommandCapture(const std::string& cmd);

    // Runs an AR5/UAE command exactly as if typed (echoes + emits output). Used by the shared
    // address-actions menu for console-routed commands (memory dump, run-to, set-pc).
    void RunCommandLine(const std::string& cmd) { ExecuteCommandLine(cmd); }

    // Reads the 32-bit long at addr from the emulated address space (refreshes region mappings
    // first). Used by the shared address menu's "Follow pointer". Returns false if unmapped.
    bool ReadMemoryLong(uint32_t addr, uint32_t& out);

    // Custom-register name/description for a $DFF000 byte offset, from the tables this view
    // already loads out of assets/custom_registers.json. Empty when the offset is not named.
    // Shared with the MCP describe_address tool so both say the same thing.
    std::string CustomRegisterName(uint32_t offset) const;
    std::string CustomRegisterDescription(uint32_t offset) const;

    // Fired after any command that may have changed core state.
    std::function<void()> m_onAnyCommandExecuted;

    // Shared address-link context driving the console's address hyperlinks + right-click menu.
    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }

    // Fired when a copper MOVE register name is clicked (open Custom Registers window).
    std::function<void(uint32_t offset)> m_onJumpToCustomReg;

    // Fired by the LOAD command to open an ADF/EXE/DMS document.
    // The bool is the force flag: replace the open document even if it is dirty.
    std::function<bool(const std::string&, bool)> m_onLoadDocument;

    // Called by LIST to retrieve the recent-files list.
    std::function<std::vector<std::string>()> m_onGetRecentFiles;

    // Called by STATE. Backed by the same snapshot store as the save_state/load_state MCP
    // tools, so a snapshot taken from either front end is visible to the other. Save and
    // load return an error string, empty on success. All null -> STATE reports unavailable.
    std::function<std::string(const std::string& name, bool persist, std::string& outWarning)>
        m_onSaveState;
    std::function<std::string(const std::string& name)> m_onLoadState;
    std::function<bool(const std::string& name)> m_onDeleteState;
    std::function<std::vector<core::ArHost::StateEntry>()> m_onListStates;

    // Called by HISTORY / REWIND / GOTO / LOOP / DIFF. The frame-history timeline lives on
    // the application, not the emulator, so these cannot go through WireEmulatorHostHooks.
    // All null -> the commands report the timeline as unavailable.
    std::function<core::ArHost::FrameHistoryInfo()> m_onFrameHistoryInfo;
    std::function<std::string(uint64_t frame)> m_onFrameHistorySeek;
    std::function<std::string(uint64_t count)> m_onFrameHistoryRewind;
    std::function<std::string(uint64_t a, uint64_t b)> m_onFrameHistoryLoop;
    std::function<void()> m_onFrameHistoryClear;
    std::function<void(bool playing)> m_onFrameHistoryPlay;
    std::function<std::string(int delta)> m_onFrameHistoryStep;
    std::function<void(int mode)> m_onFrameHistoryPlayMode;
    std::function<void(double speed)> m_onFrameHistoryPlaySpeed;
    std::function<std::string(uint64_t a, uint64_t b, std::vector<core::FrameDiffRun>& out,
                              size_t maxRuns, bool& truncated)>
        m_onFrameHistoryDiff;

    // Called by DOCS (no arg) to list open documents; paths returned in display order.
    std::function<std::vector<std::string>()> m_onGetOpenDocuments;
    std::function<std::vector<core::ArHost::DocumentInfo>()> m_onGetOpenDocumentInfo;
    // Called by DOCS N to boot the 0-based document index in the emulator.
    std::function<void(int idx)> m_onRunDocumentByIndex;
    // Called by CLOSE dN to close the 0-based document index.
    std::function<void(int idx)> m_onCloseDocumentByIndex;

    std::function<std::array<core::HostDirectoryMount, 2>()> m_onGetConfiguredHostMounts;
    std::function<std::array<core::HostDirectoryMount, 2>()> m_onGetEffectiveHostMounts;
    std::function<bool(const core::HostDirectoryMount&, std::string&)> m_onSetHostMount;
    std::function<bool(std::string&)> m_onApplyHostMounts;
    std::function<bool(core::HostMountDevice)> m_onOpenHostMount;
    std::function<bool(core::HostMountDevice)> m_onRefreshHostMount;
    std::function<void(const core::HardDriveControllerRoms&)> m_onSetHardDriveControllerRoms;

    // Called by G when the emulator is not running (same as F5/Run button).
    std::function<void()> m_onStartEmulator;
    // Called by STOP to halt the running emulator.
    std::function<void()> m_onStopEmulator;

    // Called by PROFILE / CALLSTACK to read the live profiler snapshot (set by DebuggerView,
    // which owns the disassembly + frame-scrubber views the data comes from).
    std::function<core::ProfileSnapshot(const core::ProfileQuery&)> m_onGetProfile;

    // Called by BLIT to read the current frame's blitter operations (set by Application, which
    // owns the BlitProfiler). limit <= 0 returns all blits.
    std::function<core::BlitLogSnapshot(int limit)> m_onGetBlitLog;
    // EXO: the Exocet window's session (owned by Application)
    std::function<core::ExocetSession*()> m_onExocetSession;
    // COV: coverage lives in Application (it needs the debug info, hunk map and trace history
    // together), so the console reaches it through here.
    std::function<nlohmann::json(const std::string&, uint32_t, uint64_t)> m_onLineCoverage;
    std::function<bool()> m_onIsTraceHistory;
    std::function<void(bool)> m_onSetTraceHistory;
    std::function<core::RegLogSnapshot(int limit)> m_onGetRegLog;
    std::function<core::DiskLogSnapshot(const core::DiskLogQuery&)> m_onGetDiskLog;

    // Called by AUDIO / AUDREC to read audio-path health and drive the debug capture taps
    // (set by Application, which owns the emulator).
    std::function<core::AudioLogSnapshot()> m_onGetAudioStats;
    std::function<core::PaulaStateSnapshot()> m_onGetPaulaState;
    std::function<core::AudioRecordResult(const core::AudioRecordRequest&)> m_onAudioRecord;
    std::function<core::ScenarioResult(const core::ScenarioRequest&)> m_onScenario;

private:
    void BuildInterpreter();
    void RefreshMemoryRegions();
    void LoadCustomRegisterNames();
    void HandleReturn(int row, const std::string& line);
    void PrintBanner();
    // Address actions context menu (Cmd/Alt+click or right-click on an address).
    void DrawAddressContextMenu();
    // Echoes cmd at the bottom of the buffer and runs it like a typed command.
    void ExecuteCommandLine(const std::string& cmd);

    EmulatorView* m_emulatorView = nullptr;
    BreakpointsView* m_breakpointsView = nullptr;
    core::MemoryAnnotations* m_annotations = nullptr;
    core::MemoryLocks* m_locks = nullptr;
    const AddressActionContext* m_addrCtx = nullptr;

    bool m_open = false;
    bool m_noFocus = false;
    bool m_reclaimFocus = false;

    ARConsoleTerminal m_terminal;
    core::ArMemoryAccess m_mem;
    core::Disassembler m_disassembler;
    std::unique_ptr<core::ArCommandInterpreter> m_interp;

    nlohmann::json m_customRegsDef; // assets/custom_registers.json (for copper disasm)
    std::unordered_map<uint32_t, std::string> m_customRegNames;
    std::unordered_map<uint32_t, std::string> m_customRegDesc;    // offset -> full_name (CHIPREGS)
    std::unordered_map<uint32_t, std::string> m_customRegDocName; // offset -> doc_name (DCHIP)
    nlohmann::json m_customRegDocs; // assets/custom_registers_docs.json (DCHIP bit tables)
    // Lazily-parsed bit-field tables, keyed by $DFF000 byte offset.
    std::unordered_map<uint32_t, std::vector<core::ArHost::RegBitField>> m_customRegBitsCache;
    const std::vector<core::ArHost::RegBitField>& GetRegBits(uint32_t offset);

    size_t m_emitCount = 0; // lines emitted by the interpreter (cursor placement)

    // When non-null, the interpreter's emitLine sink also appends each emitted line's plain text
    // here (used by RunCommandCapture to return console output to the MCP server).
    std::string* m_captureSink = nullptr;

    // Address actions context menu state
    uint32_t m_ctxAddr = 0;     // address the actions menu targets
    bool m_openAddrCtx = false; // request to open ##ar_addr_ctx this frame
};

} // namespace adm::ui
