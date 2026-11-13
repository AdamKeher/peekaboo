#pragma once

#include <imgui.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../../core/KprintfBreakpoints.h"
#include "../../core/WatchpointTypes.h"
#include "AddressActions.h"

namespace adm::ui {

class EmulatorView;

class BreakpointsView
{
public:
    explicit BreakpointsView(EmulatorView* emulatorView);

    void Draw();
    void Restore(bool open);
    bool* GetOpenPtr() { return &m_open; }

    // Bring the window to the front on the next Draw(). Used when a breakpoint is armed from
    // another view (e.g. right-clicking a colour clock on the raster overlay) so the user can
    // see what was set in the window that owns it.
    void RequestFocus() { m_requestFocus = true; }
    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }

    // Signal that core breakpoint state may have changed (called by DebuggerView
    // whenever the console executes any command that could modify core state).
    void MarkNeedsRefresh();

    // Who asked for a breakpoint. GDB inserts and removes its breakpoints on every stop and
    // resume, so a row can appear that the user never set from Peekaboo; saying which side put
    // it there is the difference between "the debugger is working" and "something is adding
    // breakpoints behind my back".
    static constexpr const char* kOriginGdb = "GDB";
    // Record (or clear) the origin of the PC breakpoint at `address`. Kept beside the entry
    // list rather than inside it because RefreshFromCore rebuilds plain PC entries from UAE's
    // "fl" output, which cannot round-trip anything host-side.
    void SetBreakpointOrigin(uint32_t address, const std::string& origin);
    void ClearBreakpointOrigin(uint32_t address);
    [[nodiscard]] std::string GetBreakpointOrigin(uint32_t address) const;

    // Resolve a runtime address to source file:line, for the Source column. Supplied by
    // Application, which owns the DWARF line table and the runtime->link mapping. Resolving at
    // draw time rather than storing the answer keeps it correct across a reload, where the same
    // program comes back at a different base.
    using SourceResolver =
        std::function<bool(uint32_t address, std::string& outFile, uint32_t& outLine)>;
    void SetSourceResolver(SourceResolver resolver) { m_sourceResolver = std::move(resolver); }

    struct BreakpointEntry
    {
        std::string address; // normalised 8-digit uppercase hex, e.g. "00FC0000"
        // "PC", or a watchpoint access combination: "Read", "Write", "R+W", "Exec",
        // "R+X", "W+X", "R+W+X". Use TypeToAccess/AccessToType rather than comparing
        // against the strings directly.
        std::string type;
        bool enabled = true;
        int slot = -1; // UAE memwatch slot index (watchpoints only; -1 for PC / unassigned)
        // Watched region length in bytes. Ignored for PC breakpoints. UAE's memwatch
        // breaks when an access touches any byte in [address, address + size), so this
        // is a genuine range and not just an access width — a length of 0x300 watches
        // 768 bytes with one slot.
        int size = 2;
        // UAE memwatch access-source mask token: which agents trigger the watchpoint.
        // "CPU" = CPU code only (predictable; default). "COP"/"BLT"/"DMA"/"ALL" widen it
        // to Copper / Blitter / all DMA / everything. Ignored for PC breakpoints.
        // NB: registers the Copper or display touches every frame (e.g. COLOR00 at
        // $DFF180) fire constantly under anything but "CPU", and the break PC is then the
        // unrelated CPU instruction that happened to be running when the access occurred.
        std::string source = "CPU";
        // Optional value condition (watchpoints only): when valueEnabled, the watchpoint
        // only fires if the written value (masked to `size` bytes) equals `value`. Maps to
        // UAE memwatch's "V <value>" token. Lets you break on "DMACON write that sets X".
        bool valueEnabled = false;
        uint32_t value = 0;
        // Optional symbolic register name (e.g. "DMACON") chosen via the picker; purely for
        // display, the address is authoritative.
        std::string regName;
        // Log mode (watchpoints only): record every hit and keep running instead of
        // stopping. A read watchpoint over a wide range fires on essentially every access,
        // so breaking on each one is unusable; log mode is what makes "who touches this
        // region?" answerable in a single run. Hits land in the watchpoint log.
        bool logOnly = false;
        // Stop recording and disarm after this many hits. 0 = unlimited.
        uint32_t maxHits = 0;
        // Hits recorded in log mode (separate from m_hitCounts, which counts breaks).
        uint64_t logHits = 0;

        // ---- Smart PC breakpoint extensions (PC type only) ----
        // A breakpoint carrying a register condition or an ignore-count is driven through UAE's
        // explicit "fo <idx> ..." bpnode command (+ chaining) instead of the "f <addr>" toggle.
        // One-shot is host-side only (the breakpoint is removed after it first fires). Any of
        // these flags marks the entry as carrying host-side metadata, so it is carried across an
        // "fl" refresh rather than reconstructed from the core (which can't round-trip them).
        bool condEnabled = false;       // register-compare condition attached
        std::string condReg = "D0";     // UAE register token: D0..D7, A0..A7, PC, SR, USP, ...
        std::string condOper = "==";    // UAE operator token: == != < > <= >=
        uint32_t condValue = 0;         // compared value (hex)
        uint32_t condMask = 0xFFFFFFFF; // AND-mask before compare: (reg & mask) <oper> value
        int ignoreCount = 0;            // skip this many hits before breaking (UAE N=)
        bool oneShot = false;           // remove the breakpoint after it first fires (host-side)
        int bpSlot = -1;   // UAE bpnode index for the PC head node (explicit-node only)
        int condSlot = -1; // UAE bpnode index for the chained condition node

        // True when this PC breakpoint carries host-side metadata (condition / ignore-count /
        // one-shot) and must be carried across refreshes rather than reparsed from "fl".
        bool hasHostMeta() const
        {
            return type == "PC" && (condEnabled || ignoreCount > 0 || oneShot);
        }
        // True when this breakpoint needs UAE's explicit "fo" bpnode command (a register
        // condition and/or an ignore-count). One-shot alone stays on the "f <addr>" toggle.
        bool needsExplicitNode() const { return type == "PC" && (condEnabled || ignoreCount > 0); }
    };

    // Split a watchpoint type string into its read/write/execute flags, and back again.
    // "PC" yields all-false. AccessToType returns "Write" when nothing is set, since a
    // watchpoint with no access would never fire.
    static void TypeToAccess(const std::string& type, bool& read, bool& write, bool& exec);
    static std::string AccessToType(bool read, bool write, bool exec);

    // Transient details of the most recent hit on a watchpoint, keyed by address. Populated
    // when the emulator breaks; surfaced in the table tooltip / hit column.
    struct HitInfo
    {
        uint32_t accessAddr = 0; // exact address the access touched
        uint32_t value = 0;      // value written (0 for reads)
        int rwi = 0;             // bit0=read, bit1=write, bit2=instr fetch
        int size = 0;            // access width in bytes
        std::string source;      // resolved access source (CPU/Copper/Blitter/...) if known
        uint32_t pc = 0;         // instruction that performed the access
        // True when this was a plain PC breakpoint rather than a watchpoint: there is no
        // access to describe, only the address the CPU stopped at.
        bool pcBreak = false;
        uint64_t hitCount = 0; // value of the entry's hit counter when this hit was recorded
    };

    const std::vector<BreakpointEntry>& GetEntries() const { return m_entries; }

    // Serialize current breakpoints/watchpoints (address/type/enabled) for the .peek session
    // format. Slots are not persisted — they are reallocated on import.
    nlohmann::json ExportEntries() const;
    // Replace all entries from JSON and re-inject the enabled ones into the core. Must be
    // called with the emulator running so re-injection takes effect.
    void ImportEntries(const nlohmann::json& j);

    // Returns {exists, enabled} for a PC breakpoint at the given address in one pass.
    std::pair<bool, bool> GetBreakpointStatus(uint32_t address) const;
    bool IsBreakpointAt(uint32_t address) const;
    bool IsBreakpointEnabledAt(uint32_t address) const;
    void ToggleBreakpointAt(uint32_t address);
    void RemoveBreakpointAt(uint32_t address);

    // Copper breakpoints (the Copper-window gutter). A Copper breakpoint is a Read watchpoint
    // with Source=Copper on the instruction's chip-RAM address: it fires when the Copper
    // fetches that instruction. Returns {exists, enabled}; Toggle adds/removes one at addr.
    std::pair<bool, bool> GetCopperBreakpointStatus(uint32_t address) const;
    void ToggleCopperBreakpointAt(uint32_t address);

    // AR console API: add/remove/clear entries by address and type.
    // AddBreakpointAt returns false if an entry of that type already exists.
    // size (1/2/4 bytes) and source (UAE mask token, e.g. "CPU"/"ALL") apply to
    // watchpoints only; both ignored for "PC".
    bool AddBreakpointAt(uint32_t address, const std::string& type, int size = 2,
                         const std::string& source = "CPU");
    // AR console API: add a smart PC breakpoint with an optional register condition, ignore-count
    // and/or one-shot flag. condReg/condOper are UAE tokens (e.g. "D0", "=="); pass condEnabled
    // false to omit the condition. Returns false if a PC entry already exists at the address.
    bool AddSmartBreakpointAt(uint32_t address, bool condEnabled, const std::string& condReg,
                              const std::string& condOper, uint32_t condValue, uint32_t condMask,
                              int ignoreCount, bool oneShot);
    bool RemoveEntryAt(uint32_t address, const std::string& type);
    void ClearAllByType(const std::string& type); // "" clears everything

    // ---- Full watchpoint API (MCP add_watchpoint / list_watchpoints) ----
    // Add a watchpoint with an arbitrary range, access set (read/write/execute), access
    // source, optional value condition and log mode. Returns the allocated memwatch slot,
    // or -1 with outError set (duplicate, no free slot, emulator not running).
    int AddWatchpoint(const core::WatchpointSpec& spec, std::string& outError);
    // Remove the watchpoint occupying a memwatch slot. False if no entry uses that slot.
    bool RemoveWatchpointSlot(int slot);
    // Every watchpoint entry (not PC breakpoints), with its configuration and hit count.
    std::vector<core::WatchpointInfo> ListWatchpoints() const;
    // Recorded log-mode hits, oldest first. slot < 0 means every slot; limit <= 0 means
    // all of them. With clear, the returned hits are removed from the buffer.
    std::vector<core::WatchpointHit> GetWatchpointLog(int slot, int limit, bool clear);
    void ClearWatchpointLog() { m_watchLog.clear(); }

    // Called from EmulatorView when a debug break fires, BEFORE it pauses. If the break
    // was caused by a log-mode watchpoint, the hit is recorded and true is returned,
    // meaning "keep running". Anything else returns false and the normal pause happens.
    bool ConsumeLogModeBreak();

    // Break-on-CPU-exception (EB): the armed mask is a bitmask of 68k vector numbers (0..63).
    // SetExceptionMask arms the core (if running) and stores the mask for the UI + session.
    void SetExceptionMask(uint64_t mask);
    uint64_t GetExceptionMask() const { return m_exceptionMask; }

    // Break-on-beam-position (BL): vpos < 0 means disarmed. Arms the core (if running) and stores
    // the position for the UI + session.
    void SetBeamBreak(int vpos, int hpos = -1);
    int GetBeamVpos() const { return m_beamVpos; }
    int GetBeamHpos() const { return m_beamHpos; }

    // Break-on-disk-DMA (BDSK). Like the beam and exception breaks this is core-side
    // state rather than an address-keyed entry, since it is not expressible as a UAE
    // "f"/"w" command. access < 0 disarms; 0 any, 1 read, 2 write. cyl/side/sector < 0
    // mean "any"; addrHi == 0 means "any address".
    struct DiskBreak
    {
        int access = -1; // -1 disarmed
        int cyl = -1, side = -1, sector = -1;
        uint32_t addrLo = 0, addrHi = 0;
        bool Armed() const { return access >= 0; }
    };
    void SetDiskBreak(const DiskBreak& db);
    const DiskBreak& GetDiskBreak() const { return m_diskBreak; }

    // Break-on-KPrintF-text. The rule store lives in AmigaEmulator (which assembles the serial
    // output into lines and does the matching); this window, the AR console and the MCP tools all
    // drive that one store. Returns null when there is no emulator instance to reach.
    core::KprintfBreakpoints* GetKprintfBreakpoints() const;

    // Call when the emulator is reset (toolbar button or console "reset"). Unlike a stop→start,
    // Reset() only reloads the game (retro_unload_game/retro_load_game); the core dylib stays
    // loaded, so its breakpoint nodes survive — but the CPU re-init clears SPCFLAG_BRK/trace_mode
    // so they stop firing. Re-arm the debug trace WITHOUT re-sending "f <addr>" (a toggle that
    // would remove the surviving breakpoints).
    void NotifyEmulatorReset();

private:
    const AddressActionContext* m_addrCtx = nullptr;

    // Key for the locally-cached disabled set (not in core; address + type)
    using DisabledKey = std::pair<std::string, std::string>;

    void RefreshFromCore();
    bool ExecuteAdd(const std::string& addr, const std::string& type, int size = 2,
                    const std::string& source = "CPU", bool valueEnabled = false,
                    uint32_t value = 0, const std::string& regName = {});
    // Allocate slots for a fully-built entry, push it to the core (if running) and cache
    // it. False if no memwatch/bpnode slot is free or the core rejected the command.
    bool InsertEntry(BreakpointEntry entry);
    void ExecuteRemove(int index);
    void ExecuteToggle(int index);
    bool RunCoreCommand(const std::string& uaeCmd);
    std::string NormalizeAddress(const std::string& token) const;
    // PC breakpoints use UAE's "f <addr>" (a toggle). Watchpoints use UAE's memwatch
    // "w <slot> <addr> <size> <flags>" to add and "w <slot>" to remove — these are NOT
    // toggles, so add and remove need distinct command builders.
    std::string BuildAddCommand(const BreakpointEntry& e) const;
    std::string BuildRemoveCommand(const BreakpointEntry& e) const;
    // Lowest memwatch slot (0..MEMWATCH_TOTAL-1) not used by an existing watchpoint entry.
    int AllocateWatchpointSlot() const;
    // Highest free bpnode index (BREAKPOINT_TOTAL-1 .. 0) not used by an existing explicit-node
    // breakpoint, excluding any indices in `exclude`. Top-down so explicit nodes don't collide
    // with the plain "f <addr>" toggle path (which auto-allocates bottom-up in the core).
    int AllocateBreakpointSlot(const std::set<int>& exclude = {}) const;
    // Run the core command(s) to add/remove an entry. Smart PC breakpoints (condition and/or
    // ignore-count) need two bpnode commands; plain PC and watchpoints need one. Returns true on
    // success (or when the emulator is not running, treated as deferred success).
    bool IssueAdd(const BreakpointEntry& e);
    bool IssueRemove(const BreakpointEntry& e);

    void ReinjectionToCore();
    void DrawBreakpointList();          // toolbar + the breakpoint/watchpoint table
    void DrawAddPopup();                // "Add Breakpoint" modal
    void DrawLastHitStatus();           // bottom status row: which breakpoint fired + its source
    void DrawExceptionAndBeamSection(); // CPU-exception checkboxes + raster (beam) break controls
    void DrawKprintfSection();          // break-on-KPrintF-text rules

    EmulatorView* m_emulatorView = nullptr;

    bool m_open = false;
    bool m_requestFocus = false;
    bool m_noFocus = false;
    bool m_needsRefresh = false;
    bool m_wasRunning = false; // used to detect emulator restart for breakpoint re-injection

    // m_entries is rebuilt from "fl" output on each refresh.
    // Enabled entries come from the core; disabled entries come from m_disabledEntries.
    std::vector<BreakpointEntry> m_entries;
    std::set<DisabledKey> m_disabledEntries; // removed from core but cached here
    int m_selectedRow = -1;

    // Add popup state
    bool m_showAddPopup = false;
    char m_addAddress[32] = {};
    int m_addTypeIndex = 0; // 0=PC, 1=Read, 2=Write, 3=R+W, 4=Exec
    // 0=byte(1), 1=word(2), 2=long(4), 3=custom range (m_addLength); watchpoints only
    int m_addSizeIndex = 1;
    char m_addLength[16] = {}; // custom range length, hex
    bool m_addLogOnly = false; // record hits and keep running instead of breaking
    char m_addMaxHits[12] = {};
    int m_addSourceIndex = 0; // 0=CPU, 1=Copper, 2=Blitter, 3=DMA, 4=All; watchpoints only
    // Symbolic register picker (watchpoints only)
    bool m_addUseRegister = false; // pick a named register instead of typing an address
    int m_addRegGroup = 0;         // index into WatchRegisterGroups()
    int m_addRegIndex = 0;         // index into the selected group's regs
    // Value condition (watchpoints only)
    bool m_addValueEnabled = false;
    char m_addValue[16] = {};
    // Smart PC breakpoint controls (Add popup; PC type only)
    bool m_addCondEnabled = false;
    int m_addCondRegIndex = 0;  // index into the condition register list
    int m_addCondOperIndex = 0; // index into the condition operator list
    char m_addCondValue[16] = {};
    char m_addCondMask[16] = {};
    char m_addIgnoreCount[12] = {};
    bool m_addOneShot = false;

    // Break-on-exception / break-on-beam state (mirrors the core; persisted in the session).
    uint64_t m_exceptionMask = 0;
    int m_beamVpos = -1;
    int m_beamHpos = -1;
    DiskBreak m_diskBreak;

    // KPrintF section: add-form state (the rules themselves live in AmigaEmulator).
    char m_kprintfText[128] = {};
    int m_kprintfMatchIndex = 0; // 0=Contains, 1=Exact, 2=Prefix, 3=Regex
    bool m_kprintfCaseSensitive = false;
    bool m_kprintfOneShot = false;
    bool m_kprintfAddFailed = false; // last Add was rejected (duplicate / bad regex)

    // Hit tracking
    // Origin per PC breakpoint address ("GDB"); absent means the user set it here. Survives
    // RefreshFromCore, which rebuilds the entry list from UAE and knows nothing host-side.
    std::map<std::string, std::string> m_origins;
    SourceResolver m_sourceResolver;
    std::map<std::string, uint64_t> m_hitCounts; // key: Normalized address (8-digit hex)
    std::map<std::string, HitInfo> m_lastHit;    // key: Normalized address (8-digit hex)
    std::string m_lastHitAddr;                   // key of the most recent hit (for the status row)
    std::string m_lastPausePc;

    // Minimum height of the breakpoint table, in rows. Small enough to leave room for the
    // sections below in a short window, large enough that the list is still a list.
    static constexpr float kMinTableRows = 4.0f;

    // Log-mode watchpoint hits. Bounded so a wide read watchpoint left running cannot
    // grow without limit; the oldest entries are dropped first.
    static constexpr size_t kMaxWatchLog = 20000;
    std::deque<core::WatchpointHit> m_watchLog;
    // Index of an entry in m_entries by memwatch slot, or -1.
    int FindEntryBySlot(int slot) const;

    // Resolve a UAE memwatch access mask (if the core exposes it) to a short source name.
    static std::string SourceFromAccessMask(uint32_t accessMask);
};

} // namespace adm::ui
