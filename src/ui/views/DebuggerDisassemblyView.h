#pragma once

#include <imgui.h>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../core/AmigaEmulator.h"
#include "../../core/CSourceHighlighter.h"
#include "../../core/CpuProfiler.h"
#include "../../core/DebugLines.h"
#include "../../core/Disassembler.h"
#include "../../core/FunctionProfiler.h"
#include "../../core/LibraryCallAnnotator.h"
#include "../../core/SegListResolver.h"
#include "../../core/SourceFileCache.h"
#include "AddressActions.h"

namespace adm::core {
class MemoryAnnotations;
class DebugSymbols;
class UserPreferences;
} // namespace adm::core

namespace adm::ui {

class EmulatorView;
class BreakpointsView;

class DebuggerDisassemblyView
{
public:
    DebuggerDisassemblyView(EmulatorView* emulatorView, BreakpointsView* breakpointsView,
                            core::MemoryAnnotations* annotations);

    void Draw();
    void Clear();
    bool* GetOpenPtr() { return &m_open; }
    void Restore(bool open);
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }
    // The Profile hotspot table is a window of its own, but its data (the trace, the
    // profilers, the symbols) is all fed to this view, so it stays this class's to draw.
    bool* GetProfileOpenPtr() { return &m_profileOpen; }
    void RestoreProfile(bool open);

    // Feed current frame's CPU trace (called by DebuggerView each frame). The backing snapshot
    // remains owned by DebuggerView until the next call, so this view can consume it lazily.
    void SetTraceData(const adm::AmigaEmulator::CpuTraceEntry* entries, int count);

    // Supply the frame-start CCK so Profile/Trace cycle deltas are meaningful
    void SetFrameStartCycle(uint32_t cycle) { m_frameStartCycle = cycle; }

    // Supply source-level symbols (from the active document's sibling .elf) and the
    // running program's segment list (from SegListResolver) so Profile mode can
    // group per-PC cycles by function. Pass syms=nullptr to disable. Called each
    // frame by DebuggerView; cheap when unchanged.
    void SetFunctionSymbols(const adm::core::DebugSymbols* syms, const adm::core::DebugLines* lines,
                            std::vector<adm::core::AmigaSegment> segments);

    // Supply the preference store so the interleaved-source settings (toggle, density,
    // search roots) can be read at startup and written back when the user changes them.
    void SetUserPreferences(adm::core::UserPreferences* prefs);

    // Jump disassembly to the nearest trace entry at the given lRef (called by scrubber)
    void SetTracePositionLRef(uint32_t lRef);

    // Current navigated centre address / navigate to one — used by the .peek session format
    // to save and restore the disassembly view position.
    uint32_t GetNavAddress() const { return m_navAddr; }
    void GoToAddress(uint32_t addr) { NavigateToAddress(addr, true); }
    // Open a DWARF source path at a line in the configured editor. Public so other
    // debugger windows (the Call Stack) can reuse the same path-resolution logic.
    void OpenSource(const std::string& dwarfPath, uint32_t line)
    {
        OpenSourceInEditor(dwarfPath, line);
    }

    // Request an unconditional re-snap to the live PC on the next Live-mode draw, even
    // if the user had navigated away. Called when execution stops at a breakpoint / FB
    // bootblock trap so the listing shows where the CPU stopped.
    void RequestScrollToPc() { m_forceGoToPc = true; }

    // Set the current scrubber L-Ref so entries past it are shown dimmed
    void SetScrubberLRef(uint32_t lRef) { m_scrubberLRef = lRef; }

    // Transient highlight of a single trace row (by L-Ref), driven by hovering the matching
    // CPU instruction's DMA slot-run in the Frame Scrubber. UINT32_MAX = no highlight.
    // Does not change selection or scroll position.
    void SetTraceHoverLRef(uint32_t lRef) { m_traceHoverLRef = lRef; }

    // Look up the Trace table's display fields for a row by index (the scrubber's span "#").
    // Returns false if out of range. Reuses the already-computed mnemonic + cycle costs so the
    // scrubber's DMA-slot tooltip doesn't re-disassemble.
    bool GetTraceRowDisplay(int index, std::string& mnemo, int& nominalCycles,
                            uint32_t& deltaCycles, uint32_t& pc);

    // --- Profiler data for external consumers (MCP get_profile tool + AR console PROFILE
    // command). Safe to call with the window closed: both profiler layers are rebuilt on demand
    // from the latest shared trace snapshot. ---
    bool HasProfileSymbols() const { return m_debugSymbols != nullptr; }
    bool HasProfileData();
    // Total traced CCKs this frame (sum over every executed instruction).
    uint32_t GetProfileTotalCycles();
    // Resolved (named-function) vs unresolved (ROM/library/gap) cycle split. Rebuilds the
    // rollup if the trace changed. Both 0 when no symbols are loaded.
    void GetProfileCycleSplit(uint32_t& resolved, uint32_t& unresolved);
    // Top-N per-PC instruction stats (mnemonics filled), sorted by totalCycles desc.
    // maxRows <= 0 returns all rows.
    std::vector<adm::core::InstructionStats> GetProfileInstructions(int maxRows);
    // Top-N per-function stats, sorted by totalCycles desc. maxRows <= 0 returns all.
    std::vector<adm::core::FunctionStats> GetProfileFunctions(int maxRows);

    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }
    // Fired when selected trace entry changes (active=false when returning to Live mode)
    std::function<void(bool active, const adm::AmigaEmulator::CpuTraceEntry&)>
        m_onTraceEntrySelected;
    // Fired when user clicks a trace row — propagates L-Ref for cross-view sync
    std::function<void(uint32_t lRef)> m_onJumpToLRef;

private:
    enum class Mode { Live, Trace };

    EmulatorView* m_emulatorView;
    BreakpointsView* m_breakpointsView;
    const AddressActionContext* m_addrCtx = nullptr;
    adm::core::MemoryAnnotations* m_annotations = nullptr;
    adm::core::Disassembler m_disassembler;
    bool m_open = false;
    bool m_noFocus = false;
    bool m_turboSuppressed = false;
    bool m_profileOpen = false;
    bool m_profileNoFocus = false;
    Mode m_mode = Mode::Live;

    // Live mode state
    struct DisasmInstruction
    {
        uint32_t address;
        uint32_t nextAddress;
        std::string text;
        std::string hex;
        bool sectionEnd = false; // flow-terminating instruction -> draw separator below
        // PC-relative branch info (BRA/BSR/Bcc/DBcc). targetIdx is the cache index of
        // the destination instruction, or -1 if it lands outside the cache window.
        bool isBranch = false;
        bool branchConditional = false;
        uint32_t branchTarget = 0;
        int targetIdx = -1;
        int branchLane = 0; // lane assignment so nested arrows don't overlap

        // Interleaved source (filled by RefreshSourceMapping, not MakeInstruction). The
        // gap-fill rule is sequential over the whole cache — each entry's answer depends
        // on the line the previous entry emitted — so it is resolved once per rebuild
        // rather than recomputed per frame.
        int srcFileIdx = -1;       // index into m_sourcePaths; -1 = no line info here
        uint32_t srcLine = 0;      // 1-based DWARF line this instruction belongs to
        uint32_t srcEmitFirst = 0; // inclusive run of source lines to draw above this row
        uint32_t srcEmitLast = 0;  // 0 = emit nothing (a previous entry already showed it)
        bool srcFileHeader = false;
        bool srcTruncated = false; // run hit the gap cap; show a "... N lines" note
    };
    std::vector<DisasmInstruction> m_disasmCache;
    int m_maxBranchLane = -1; // highest lane used in current cache (-1 = no branches)
    // User-selected highlight line (dark-blue row + emphasised branch arrows). Stored
    // by address so it survives cache rebuilds; UINT32_MAX = nothing highlighted.
    uint32_t m_highlightedAddr = 0xFFFFFFFF;
    uint32_t m_disasmCachedPc = 0xFFFFFFFF;
    int m_disasmPcIdx = -1;
    bool m_disasmScrollToPc = false; // force scroll to PC (rebuild / Go-to-PC button)
    bool m_scrollPcIfHidden = false; // lazy scroll: only if the PC row drifted out of view
    bool m_scrollPcCenter = false;   // center the PC (Go-to-PC) instead of direction edge
    bool m_scrollPcForward = true;   // last PC move direction (forward = scroll toward bottom)
    uint32_t m_prevPc = 0xFFFFFFFF;  // PC last frame, to detect movement + direction

    // Quick "add label" popup, opened by the + button drawn in a section separator.
    uint32_t m_labelTargetAddr = 0; // address the popup labels (start of next section)
    bool m_openLabelPopup = false;  // request to open the popup this frame
    char m_labelInputBuf[256] = {}; // label edit buffer

    // Address-link navigation. Clicking a branch/jump target (or the context menu's
    // "Follow") scrolls the listing to that address in-place. While an override is
    // active the live-PC re-snap is suppressed so the view stays where the user
    // navigated, until they step the CPU or press "Go to PC".
    bool m_navOverride = false;      // showing a navigated address, not the live PC
    bool m_forceGoToPc = false;      // a break asked the listing to re-snap to the live PC
    uint32_t m_navAddr = 0;          // current navigated centre address
    std::vector<uint32_t> m_navBack; // back history (addresses)
    std::vector<uint32_t> m_navFwd;  // forward history (addresses)
    int m_scrollToIdx = -1;          // cache row to scroll to next frame (-1 = none)
    bool m_doScrollToIdx = false;
    // An in-listing follow requested from a link click is deferred to after the row
    // loop: NavigateToAddress may rebuild m_disasmCache, which must not happen while
    // the clipper is iterating it.
    bool m_pendingNav = false;
    uint32_t m_pendingNavAddr = 0;

    // Lazy listing growth. The fixed rebuild window (RebuildDisasmCache) only decodes
    // a few hundred instructions around the PC; while the emulator is paused, scrolling
    // to the top or bottom edge splices more decoded instructions onto that end so the
    // user can browse arbitrarily far without re-centring on the PC. Growth is
    // unbounded (paused only) and is discarded — snapped back to the compact window —
    // the moment a sustained Play/continue resumes execution. Stepping keeps the growth.
    static constexpr int kExtendChunk = 64; // instructions decoded per edge-reach
    float m_lastScrollY = -1.0f;            // table scroll-Y last frame (detects scrolling)
    int m_pendingScrollAddRows = 0;         // rows prepended; scroll comp to apply next frame
    bool m_prevFreeze = false;              // freeze state last frame (detects resume)
    char m_gotoAddrBuf[16] = "";            // hex address entry for manual disassembly jumps

    // Right-click context menu state (mirrors ARConsoleView's address menu).
    bool m_openDisasmCtx = false;    // request to open the menu this frame
    uint32_t m_ctxLineAddr = 0;      // instruction address the menu targets
    bool m_ctxHasOperand = false;    // a right-clicked address operand is present
    uint32_t m_ctxOperandAddr = 0;   // that operand's address
    bool m_openAnnotEdit = false;    // request to open the label/comment editor
    bool m_annotEditIsLabel = false; // editor edits the label (vs the comment)
    char m_annotBuf[256] = {};       // label/comment edit buffer
    bool m_openBlockEdit = false;    // request to open the comment-block editor
    char m_blockBuf[2048] = {};      // multi-line comment-block edit buffer

    // Trace mode state
    std::span<const adm::AmigaEmulator::CpuTraceEntry> m_traceData;
    int m_selectedTraceIdx = 0;
    bool m_traceScrollToSelected = false;
    uint32_t m_scrubberLRef = UINT32_MAX;   // entries with lRef > this are dimmed
    uint32_t m_traceHoverLRef = UINT32_MAX; // transient row highlight from scrubber DMA hover
    uint32_t m_frameStartCycle = 0;
    char m_traceSearchAddr[16] = "";  // Search address string (hex)
    uint32_t m_traceSearchValue = 0;  // Parsed search address
    bool m_traceSearchActive = false; // True when a valid hex address is entered

    // Pre-formatted trace row strings (rebuilt in SetTraceData)
    struct FormattedTraceRow
    {
        std::string lRef;   // "L%07X"
        std::string label;  // "$%08X##tr%d" — selectable label
        std::string hexStr; // disassembled hex bytes
        std::string mnemo;  // disassembled mnemonic
        uint32_t lRefValue = 0;
        uint32_t deltaCycles = 0; // measured elapsed color-clocks to the next instruction
        int nominalCycles = -1;   // textbook 68000 CPU cycles (-1 = not modeled)
        // Control-flow gutter (computed in RebuildFormattedTrace, drawn when paused):
        int depth = 0;         // call-nesting depth this instruction ran at
        uint8_t cfKind = 0;    // CF_SEQ / CF_BRANCH / CF_CALL / CF_RETURN
        bool diverged = false; // execution did NOT fall through to pc+len (taken branch/jump)
        // Interleaved source. Trace is execution order, not address order, so a "gap"
        // between consecutive entries is meaningless — one line per change, never a fill.
        int srcFileIdx = -1;
        uint32_t srcLine = 0; // 1-based line to show above this row; 0 = none
        bool srcFileHeader = false;
    };
    std::vector<FormattedTraceRow> m_formattedTrace;
    int m_traceMaxDepth = 0; // deepest call-nesting in the trace; sizes the gutter column
    // m_formattedTrace is expensive to build (a disassemble + several string allocations per
    // entry, ~14k-17k entries/frame). It's rebuilt lazily: SetTraceData only marks it dirty, and
    // EnsureFormattedTrace() rebuilds on demand the first time a consumer actually reads it (Trace
    // mode draw or the scrubber's tooltip query). If the user is in Live/Profile mode and not
    // hovering the scrubber, the rebuild is skipped entirely.
    bool m_formattedTraceDirty = false;

    // Profile mode
    adm::core::CpuProfiler m_profiler;
    bool m_profilerDirty = false;
    // Profile table sorted copy with mnemonics filled in
    std::vector<adm::core::InstructionStats> m_profileRows;
    uint32_t m_profilerVersion = UINT32_MAX; // rebuild trigger
    // Sort state preserved across data refreshes (default: address ascending)
    int m_profileSortCol = 0;
    ImGuiSortDirection m_profileSortDir = ImGuiSortDirection_Ascending;

    // Profile mode: source-level "group by function" (Stage 4 of the profiler).
    // Fed each frame by SetFunctionSymbols; drawn instead of the per-PC table when
    // m_groupByFunction is on and symbols are available.
    adm::core::FunctionProfiler m_funcProfiler;
    const adm::core::DebugSymbols* m_debugSymbols = nullptr;
    const adm::core::DebugLines* m_debugLines = nullptr; // DWARF line info (optional)
    std::vector<adm::core::AmigaSegment> m_segments;     // running program's seglist
    bool m_groupByFunction = false;
    std::vector<adm::core::FunctionStats> m_funcRows; // sorted copy for the table
    uint32_t m_funcProfilerVersion = UINT32_MAX;
    bool m_funcRowsDirty = true;
    int m_funcSortCol = 2; // default: Total cy
    ImGuiSortDirection m_funcSortDir = ImGuiSortDirection_Descending;
    // Manual load-base override for when the seglist can't be auto-resolved (e.g. a
    // demo that took over the machine without an AmigaDOS process).
    bool m_useManualBase = false;
    char m_manualBaseBuf[16] = "";
    void RebuildFunctionRows();
    // Rebuild the per-function rollup only if the trace changed since it was last built.
    void EnsureFunctionRollup();
    void ApplyFunctionSort();
    void DrawFunctionTable();

    // --- Interleaved C/C++ source ---------------------------------------------------
    // Off by default: source rows are expensive in vertical space, and the feature needs
    // both DWARF line info and the source tree present on this machine. m_sourceDetail
    // defaults to the tightest mode for the same reason.
    enum class SourceDetail : int { StatementOnly = 0, CodeOnly = 1, Full = 2 };
    static constexpr int kMaxSourceGapLines = 24;        // default cap, overridden by prefs
    static constexpr int kMaxSourceRowsPerListing = 600; // hard ceiling per rebuild

    adm::core::UserPreferences* m_userPrefs = nullptr;
    adm::core::SourceFileCache m_sourceCache;
    bool m_showSource = false;
    SourceDetail m_sourceDetail = SourceDetail::StatementOnly;
    int m_sourceMaxGap = kMaxSourceGapLines;
    bool m_sourceFileHeaders = true;
    bool m_sourceLineNumbers = true;
    bool m_sourceMapDirty = true;
    bool m_sourceBudgetHit = false; // kMaxSourceRowsPerListing stopped the mapping early
    // DWARF paths referenced by the current cache, interned so DisasmInstruction stays small.
    std::vector<std::string> m_sourcePaths;
    // Per-file lexer carry-state, so tokenizing one line out of the middle of a file is
    // O(line) rather than O(file). Keyed by DWARF path, versioned against SourceFile.
    struct SourceLexCache
    {
        uint32_t fileVersion = 0;
        std::vector<adm::core::CLexState> states; // states[n] enters 0-based line n
        std::vector<bool> commentOrBlank;         // per 0-based line, for CodeOnly
    };
    std::unordered_map<std::string, SourceLexCache> m_sourceLex;
    // Change detection for the per-frame SetFunctionSymbols call: without this the
    // mapping pass (and RebuildFormattedTrace) would re-run 60 times a second.
    uint64_t m_segSignature = 0;
    // "Source paths..." popup state.
    bool m_openSourcePaths = false;
    std::vector<std::string> m_srcRootsEdit;
    char m_srcRootInput[512] = "";
    // Source-row right-click menu (separate from ##disasm_ctx, which is address-centric).
    bool m_openSrcCtx = false;
    std::string m_srcCtxPath;
    uint32_t m_srcCtxLine = 0;
    uint32_t m_srcCtxAddr = 0;
    adm::core::LineLookup m_srcCtxLookup;

    int InternSourcePath(const std::string& path);
    // Recompute every entry's srcEmit* run. Called from the cache rebuild/extend paths
    // and whenever m_sourceMapDirty is set — never from the row loop.
    void RefreshSourceMapping();
    const SourceLexCache& LexStatesFor(const adm::core::SourceFile& file);
    // Rows DrawSourceRows will emit for one entry; used to keep the scroll compensation
    // in ExtendCacheUp honest.
    int SourceRowsFor(const DisasmInstruction& inst);
    int TotalSourceRows();
    void DrawSourceRows(const DisasmInstruction& inst, float rowH);
    void DrawSourceLineRow(uint32_t anchorAddr, const std::string& dwarfPath,
                           const adm::core::SourceFile& file, uint32_t line, float rowH, int selCol,
                           int textCol, bool rail);
    void OpenSourceInEditor(const std::string& dwarfPath, uint32_t line);
    void DrawSourceNoteRow(uint32_t anchorAddr, const char* idSuffix, adm::core::ArColor color,
                           const std::string& text, float rowH, int selCol, int textCol, bool rail);
    // Shared body of both: column indices differ between Live (2/4) and Trace (4/8),
    // the same reason DrawCommentBlock already takes an addrCol.
    void DrawSourceRunAt(uint32_t anchorAddr, int fileIdx, uint32_t firstLine, uint32_t lastLine,
                         bool header, bool truncated, float rowH, int selCol, int textCol,
                         bool rail);
    // A vertical stripe down the left edge of the Instruction column marking the row's
    // language (C source vs disassembly), so interleaved listings separate at a glance.
    // Call with the Instruction cell current and empty: it advances the cursor past the
    // rail, so whatever is submitted next lands clear of it.
    void DrawLanguageRail(float rowH, bool isSource);
    void DrawTraceSourceRows(uint32_t addr, const FormattedTraceRow& row, float rowH);
    void DrawSourceToolbar();
    void DrawSourcePathsPopup();
    void DrawSourceContextMenu();
    void ApplySourcePrefs();
    void PersistSourceRoots();

    // Instruction docs tooltip data
    bool m_instructionDocsLoaded = false;
    std::unordered_map<std::string, std::string> m_instructionDocs;
    bool m_showDocs = false;
    float m_docsSplitRatio = 0.62f;
    std::string m_selectedInstructionDocKey;
    std::string m_selectedInstructionName;

    // Amiga library-call annotation (e.g. "  ; exec/DoIO()"). Optional because the
    // strategy (c) lookup reads emulated memory per visible call line; toggle it
    // off if it ever becomes a per-frame cost concern.
    bool m_showLibCalls = true;
    adm::core::LibraryCallAnnotator m_libAnnotator;
    bool m_libAnnotatorLoaded = false;
    // Builds the library-call suffix for a line. state/regions enable strategy (c)
    // (resolve target library from live a6 + memory); pass null for exec-default.
    std::string LibraryCallSuffix(const std::string& mnemonic,
                                  const adm::AmigaEmulator::CpuState* state,
                                  const std::vector<adm::AmigaEmulator::MemoryRegion>* regions);

    void DrawContent();
    void DrawDocPanel();
    // Scroll the listing to addr (rebuilding the cache around it if needed) and mark
    // the view as navigated. recordHistory=false is used by the back/forward buttons.
    void NavigateToAddress(uint32_t addr, bool recordHistory = true);
    void DrawDisasmContextMenu();
    // Renders a multi-line "; ..." comment block (owned by addr) as its own table
    // rows, spanning from the Address column to the window's right edge. Each row is
    // interactive: double-click edits the block, right-click opens the context menu.
    // Call before the label row.
    void DrawCommentBlock(uint32_t addr, const std::string& block, int addrCol = 2);
    void RebuildDisasmCache(uint32_t pc);
    // Splice more decoded instructions onto an edge of m_disasmCache when the user
    // scrolls to it. Up-growth probes for an aligned lead-in (m68k is variable-length)
    // and records a scroll compensation so the view stays put; down-growth just appends.
    void ExtendCacheUp(int chunk);
    void ExtendCacheDown(int chunk);
    void AnalyzeBranches(); // populate branch targets + lane assignment for m_disasmCache

    // Builds a cache entry from a decoded line (shared by the rebuild + both extend
    // paths so the field population stays in one place). Consumes the line's strings.
    static DisasmInstruction MakeInstruction(uint32_t addr, core::Disassembler::Line& line);
    // True if disassembling forward from `start` lands exactly on `target` within
    // `maxIters` instructions. m68k is variable-length, so callers probe for an
    // aligned lead-in rather than assuming a fixed step-back.
    bool DecodeAlignsTo(uint32_t start, uint32_t target, int maxIters) const;
    void RebuildFormattedTrace();
    // Rebuild m_formattedTrace only if it's stale (m_formattedTraceDirty). Called on demand by
    // every consumer of the formatted rows so the rebuild happens at most once per trace frame,
    // and not at all when nothing reads them.
    void EnsureFormattedTrace();
    void EnsureProfiler();
    void RebuildProfileRows();
    void ApplyProfileSort();
    void LoadInstructionDocs();
    std::string InstructionDocKeyFromDisasm(const std::string& disasmText) const;
    void SelectInstructionDocFromDisasm(const std::string& disasmText);
    void DrawLiveMode();
    void DrawTraceMode();
    // The Profile window: its own Begin/End, drawn from Draw() alongside (and
    // independently of) the Disassembly window. DrawProfileMode is its body.
    void DrawProfileWindow();
    void DrawProfileMode();
    void FireTraceSelected(bool active);
};

} // namespace adm::ui
