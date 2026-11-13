#pragma once

#include <imgui.h>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include "../../core/AmigaEmulator.h"
#include "../../core/AmigaScreenMap.h"
#include "../../core/ArCommandInterpreter.h"
#include "../../core/BitplaneLayout.h"
#include "../../core/BlitLogSnapshot.h"
#include "../../core/BlitProfiler.h"
#include "../../core/MemoryLocks.h"
#include "../../core/MemorySearcher.h"
#include "../../core/PaulaStateSnapshot.h"
#include "../../core/ProfileSnapshot.h"
#include "../../core/RasterFrameMap.h"
#include "../../core/SegListResolver.h"
#include "../../core/SourceDebugContext.h"
#include "../../core/StackUnwinder.h"
#include "../BlitOverlaySettings.h"
#include "../RasterOverlaySettings.h"
#include "AddressActions.h"
#include "DebuggerCallStackView.h"

namespace adm::core {
class ExocetSession;
class MemoryAnnotations;
class DebugSymbols;
class DebugLines;
class DebugInline;
class UserPreferences;
struct BlitRecord;
} // namespace adm::core

namespace adm::ui {

class EmulatorView;
class DebuggerCPUView;
class DebuggerCustomView;
class DebuggerDisassemblyView;
class DebuggerCopperView;
class DebuggerFrameScrubberView;
class DebuggerMemoryView;
class DebuggerDisplayView;
class DebuggerSpritesView;
class DebuggerBitplanesView;
class DebuggerBartmanView;
class DebuggerCIAView;
class DebuggerAudioView;
class DebuggerCallStackView;
class DebuggerTrainerView;
class ARConsoleView;
class BreakpointsView;
class MemoryAnnotationsView;

class DebuggerView
{
public:
    explicit DebuggerView(EmulatorView* emulatorView);
    ~DebuggerView();

    void Draw();
    void Clear();
    void SetUserPreferences(core::UserPreferences* preferences);

    // Per-window open state accessors
    bool* GetCpuOpenPtr();
    bool* GetCustomOpenPtr();
    bool* GetDisassemblyOpenPtr();
    bool* GetProfileOpenPtr();
    bool* GetCopperOpenPtr();
    bool* GetMemoryOpenPtr();
    bool* GetDisplayOpenPtr();
    bool* GetSpritesOpenPtr();
    bool* GetBitplanesOpenPtr();
    bool* GetBartmanOpenPtr();
    bool* GetCIAOpenPtr();
    bool* GetAudioOpenPtr();
    bool* GetScrubberOpenPtr();
    bool* GetCallStackOpenPtr();
    // True when any debugger window is open. Gates the frame-history recorder: capturing a
    // save state every frame is only worth paying for while something is looking at it.
    bool IsAnyWindowOpen();
    // Frame-scrubber view settings persisted across sessions (unit selector + instruction-#
    // overlay toggle).
    int GetScrubberTickUnit() const;
    void SetScrubberTickUnit(int unit);
    bool GetScrubberShowInstrNumbers() const;
    void SetScrubberShowInstrNumbers(bool show);
    int GetScrubberHeatMap() const;
    void SetScrubberHeatMap(int mode);
    // Sprites view layout persisted across sessions (Scanlines panel width + detail-panel height).
    float GetSpritesScanlinesPanelWidth() const;
    void SetSpritesScanlinesPanelWidth(float w);
    float GetSpritesDetailPanelHeight() const;
    void SetSpritesDetailPanelHeight(float h);
    // Memory window bytes-per-line (hex columns) persisted across sessions.
    int GetMemoryBytesPerLine() const;
    void SetMemoryBytesPerLine(int bpl);
    bool* GetConsoleOpenPtr();
    void ToggleConsole();
    bool* GetBreakpointsOpenPtr();
    bool* GetAnnotationsOpenPtr();

    // Sub-view accessors — used by the .peek session format to save/restore breakpoints and
    // the disassembly/memory navigation addresses.
    BreakpointsView* GetBreakpointsView() { return m_breakpointsView.get(); }
    DebuggerDisassemblyView* GetDisassemblyView() { return m_disasmView.get(); }
    DebuggerMemoryView* GetMemoryView() { return m_memoryView.get(); }

    // Shared annotation store + AR console — used by the MCP server to read/write labels and
    // comments and to run console commands on Claude's behalf.
    core::MemoryAnnotations* GetAnnotations() { return m_annotations.get(); }
    core::MemoryLocks* GetLocks() { return m_locks.get(); }
    DebuggerTrainerView* GetTrainerView() { return m_trainerView.get(); }
    bool* GetTrainerOpenPtr();
    /// Refreshes the lock memory map and resolves the running process's segments.
    /// Everything that adds, enables or rescans a lock goes through this.
    core::LockContext MakeLockContext();
    ARConsoleView* GetConsoleView() { return m_consoleView.get(); }

    // Assemble a profiling snapshot for the MCP get_profile tool + the AR console PROFILE /
    // CALLSTACK commands. Pulls per-instruction / per-function data from the disassembly
    // Profile view and the flat/flame/call-stack reconstruction from the frame scrubber.
    // Safe to call with those windows closed. Requires CPU trace to be enabled to have data.
    core::ProfileSnapshot GetProfileSnapshot(const core::ProfileQuery& query);

    // The shared address-link context, so Application-owned views (e.g. the Memory Map) can give
    // their address columns the same hyperlink + right-click menu.
    const AddressActionContext* GetAddressContext() const { return &m_addrCtx; }

    // Restore windows from saved prefs (with no-focus-on-appearing)
    void RestoreWindows(bool cpu, bool custom, bool disasm, bool copper, bool memory, bool display,
                        bool sprites = false, bool bitplanes = false, bool cia = false,
                        bool scrubber = false, bool console = false, bool breakpoints = false,
                        bool annotations = false, bool callstack = false, bool bartman = false,
                        bool audio = false, bool profile = false, bool trainer = false);

    // Called by Application to forward scrubber position to the copper view
    void SetCopperScrubberLRef(uint32_t lRef);

    // Highlight a blit bar in the frame scrubber from a BlitterView hover (UINT32_MAX = clear)
    void SetHighlightedBlit(uint32_t lRef);
    void SetHighlightedCopper(uint32_t lRef);

    // Pin a blit's detail panel in the frame scrubber (UINT32_MAX = unpin). Used by the blit
    // provenance overlay pick, which resolves a screen pixel back to the blit that wrote it.
    void PinBlit(uint32_t lRef);

    // Sync all traces + scrubber to this L-Ref (called by Application when BlitterView row clicked)
    void JumpToLRef(uint32_t lRef);

    // Feed data to the frame scrubber each frame (called by Application)
    void SetScrubberData(const std::vector<adm::core::BlitRecord>& records,
                         const std::vector<adm::AmigaEmulator::CopperExecEvent>& events,
                         uint32_t frameStartCycle);

    // Feed Bartman idle-region markers to the frame scrubber's Idle row (called by Application).
    void SetScrubberIdleEvents(const std::vector<adm::AmigaEmulator::IdleEvent>& events);

    // Feed CPU trace data each frame (called by Application when tracing is enabled)
    void SetCpuTraceData(const adm::AmigaEmulator::CpuTraceEntry* entries, int count,
                         uint32_t frameStartCycle);

    // Feed the source-level debug info (symbols, line table, inline scopes, call-frame
    // info) plus where the program is loaded. Pushed each frame; a context with no
    // symbols disables the source-level features. Drives the disassembly Profile mode's
    // function grouping and file:line, the scrubber's function bands, and the Call Stack
    // window's live unwind.
    void SetSourceDebugContext(adm::core::SourceDebugContext context);

    // Feed DMA record data each frame (called by Application when DMA debug is enabled)
    void SetScrubberDmaData(const adm::AmigaEmulator::DmaSlotRecord* records, int hposCount,
                            int vposCount);
    void SetScrubberBreakMarker(bool partial, uint32_t breakLRef);
    // Frame-history seek bar above the in-frame timeline: recorded range + playhead.
    void SetScrubberFrameTimeline(bool enabled, bool recording, uint64_t oldest, uint64_t newest,
                                  uint64_t position);
    void SetOnScrubberSeekFrame(std::function<void(uint64_t frame)> cb);
    // A-B loop over the seek bar: the armed range in, the user's arm/disarm out.
    void SetScrubberFrameLoop(bool active, uint64_t a, uint64_t b, bool playing, int mode,
                              double speed, const std::string& stopReason);
    void SetOnScrubberSetFrameLoop(std::function<void(uint64_t a, uint64_t b)> cb);
    // Transport over the recorded timeline (play/pause, step, direction, speed).
    void SetOnScrubberClearTimeline(std::function<void()> cb);
    // The transport's record arm: turns the recorder itself on and off.
    void SetOnScrubberSetRecording(std::function<void(bool recording)> cb);
    void SetOnScrubberTimelineTransport(std::function<void(bool playing)> play,
                                        std::function<void(int delta)> step,
                                        std::function<void(int mode)> mode,
                                        std::function<void(double speed)> speed);
    // Supplies the scrub preview with a screen thumbnail for a frame (nearest recorded
    // shot at or before it; `gotFrame` reports which frame it is really from).
    void SetOnScrubberFrameThumbnail(std::function<bool(uint64_t frame, std::vector<uint8_t>& rgb,
                                                        int& w, int& h, uint64_t& gotFrame)>
                                         cb);

    // Feed the bitplane layout + screen map so the pinned blit panel can show whether the
    // blit maps to a visible emulator overlay.
    void SetScrubberOverlayContext(const core::BitplaneLayout& layout,
                                   const core::AmigaScreenMap& map);

    // Fired by the shared address menu's "Memory Map" item — Application opens the Memory Map
    // window (which it owns) focused near the address.
    std::function<void(uint32_t addr)> m_onShowInMemoryMap;

    // Fired when scrubber moves — Application wires this to BlitterView::SetHighlightLRef
    std::function<void(uint32_t lRef)> m_onScrubberChanged;

    // Fired when the blit the scrubber wants to overlay changes (hovered, else pinned;
    // UINT32_MAX = none). Application resolves it to a BlitRecord and draws the destination
    // overlay on the emulator screen.
    std::function<void(uint32_t lRef)> m_onActiveBlitChanged;

    // Fired when the minterm in the pinned blit-detail panel is clicked — Application opens
    // the Blitter Minterm window populated from this blit's BLTCON0/BLTCON1.
    std::function<void(uint16_t bltcon0, uint16_t bltcon1)> m_onOpenMinterm;

    // Snapshot of the decoded bitplane geometry, for the blit-destination overlay. Non-const:
    // refreshes the geometry decode on demand so it's valid even with the Bitplanes window shut.
    core::BitplaneLayout GetBitplaneLayout();

    // Forward the frame's active blit (nullptr = none) to the Bitplanes view for highlighting.
    void SetActiveBlit(const core::BlitRecord* rec);

    // Draw the sprites bounding-box overlay onto the emulator screen
    void DrawSpritesOverlay(ImDrawList* dl, ImVec2 screenMin, ImVec2 screenMax);

    // Draw the DIW/DDF geometry and frame counter overlays onto the emulator screen
    void DrawOverlays(ImDrawList* dl, ImVec2 screenMin, ImVec2 screenMax);
    bool IsGeometryOverlayVisible() const;

    // --- Blit overlay -----------------------------------------------------------------------
    // Settings live on the display window; Application owns the overlay itself because it owns
    // the emulator the provenance spans come from. Same split as the raster overlay below.
    const BlitOverlaySettings& GetBlitOverlaySettings() const;
    void SetBlitOverlaySettings(const BlitOverlaySettings& settings);
    bool IsBlitOverlayVisible() const;
    void SetBlitOverlayStats(int spans, int pixelBlits, int boxBlits, int unplacedBlits);

    // Destination-data capture mirrors, pushed in by Application::SyncBlitDestCapture.
    void SetBlitCaptureDestData(bool on);
    void SetBlitCaptureDestDataSupported(bool supported);

    // Fired when any blit overlay setting changes. Application uses it to arm or release the
    // core-side provenance map, since it owns the emulator.
    std::function<void(const BlitOverlaySettings&)> m_onBlitOverlayChanged;

    // Fired when destination capture is toggled from the same panel.
    std::function<void(bool)> m_onBlitCaptureDestDataChanged;

    // --- Copper / DMA raster overlay --------------------------------------------------------
    // Settings live on the display window; Application owns the overlay itself because it owns
    // the emulator the DMA record comes from. Same split as blit provenance above.
    const RasterSettings& GetRasterSettings() const;
    void SetRasterSettings(const RasterSettings& settings);
    bool IsRasterOverlayVisible() const;
    void SetRasterStats(int copperCells, int dmaCells);
    void SetRasterGeometry(const adm::core::RasterFrameMap& map);

    // Collapsible-section state of the Display window, for workspace save/restore.
    std::map<std::string, bool> GetDisplaySectionStates() const;
    void SetDisplaySectionStates(const std::map<std::string, bool>& states);

    // Fired when the user toggles the raster overlay. Application uses it to arm the core's DMA
    // recording and release the overlay texture when it is switched off.
    std::function<void(bool)> m_onShowCopperRasterChanged;

    // Current frame-scrubber playhead, so the raster can draw the beam marker where the
    // scrubber is.
    uint32_t GetScrubberLRef() const;

    // Open the Copper window on the instruction at this L-Ref (Trace mode, scrolled + selected).
    void JumpToCopperTrace(uint32_t lRef);

    // Open the Custom Registers window at `offset`, seeding its bit-field decoder with `value`
    // (the value a log row actually wrote, rather than the register's current contents).
    void NavigateToCustomRegister(uint32_t offset, uint16_t value);

    // Point the Disassembly window at `pc` — used when a log row's source is the CPU, to show
    // the instruction that performed the write.
    void NavigateToDisassembly(uint32_t pc);

    // Arm a beam breakpoint at an exact colour clock. hpos < 0 breaks anywhere on the line.
    void SetBeamBreakAt(int vpos, int hpos);

    // Current one-shot beam breakpoint, so a view can show what is armed. vpos < 0 = none.
    void GetBeamBreak(int& outVpos, int& outHpos) const;

    // Open and focus the Breakpoints window, so a breakpoint armed from elsewhere is visible in
    // the window that owns it rather than being set invisibly.
    void RevealBreakpointsWindow();

    // Render the Copper trace's own tooltip for the copper event at `index`. Returns false when
    // the index is out of range, so callers can fall back to their own tooltip. Shared so the
    // raster and the scrubber cannot describe the same instruction differently.
    bool DrawCopperEventTooltip(int index, uint32_t lRef);

    // Highlight a colour clock on the raster from an external hover -- UINT32_MAX clears it.
    std::function<void(uint32_t)> m_onRasterHighlight;

    // Select a colour clock on the raster. Fired from JumpToLRef, so the raster follows every
    // other window that resolves to a beam position.
    std::function<void(uint32_t)> m_onRasterSelect;

    // Forward a "jump to memory address" callback into the sprites sub-view
    void SetSpritesJumpMemory(std::function<void(uint32_t)> cb);

    // Forward a "jump to memory address" callback into the bitplanes sub-view
    void SetBitplanesJumpMemory(std::function<void(uint32_t)> cb);

    // Forward a "jump to memory address" callback into the Bartman bitmaps sub-view
    void SetBartmanJumpMemory(std::function<void(uint32_t)> cb);

    // Forward the "Bartman debug support toggled" callback (host persists the opt-in)
    void SetBartmanSupportChanged(std::function<void(bool)> cb);

    // Memory search
    void SetMemorySearcher(core::MemorySearcher* searcher);
    void ShowFindDialog();
    void FindNext();
    void FindPrevious();
    void HighlightMemoryResult(uint32_t amigaAddr, size_t length);

    void SetOnMemoryTabActive(std::function<void()> callback);

    // Wire the LOAD command to open a document in the application
    void SetOnLoadDocument(std::function<bool(const std::string&, bool)> cb);
    // Wire the LIST command to retrieve recent files
    void SetOnGetRecentFiles(std::function<std::vector<std::string>()> cb);
    // Wire the STATE command to the application's machine-snapshot store (the same one the
    // save_state/load_state MCP tools use). Save and load return an error string, empty on
    // success; save also reports a warning when it could only be kept in memory.
    void SetStateSnapshotHooks(
        std::function<std::string(const std::string& name, bool persist, std::string& outWarning)>
            save,
        std::function<std::string(const std::string& name)> load,
        std::function<bool(const std::string& name)> remove,
        std::function<std::vector<core::ArHost::StateEntry>()> list);
    // Wire HISTORY / REWIND / GOTO / LOOP / DIFF to the application's frame-history
    // timeline. Seek, rewind, loop and diff each return an error string, empty on success.
    void SetOnFrameHistoryClear(std::function<void()> cb);
    // Console-side transport hooks (the LOOP PLAY/PAUSE/STEP/SPEED/DIR sub-commands).
    void SetFrameHistoryTransportHooks(std::function<void(bool playing)> play,
                                       std::function<std::string(int delta)> step,
                                       std::function<void(int mode)> mode,
                                       std::function<void(double speed)> speed);
    void SetFrameHistoryHooks(
        std::function<core::ArHost::FrameHistoryInfo()> info,
        std::function<std::string(uint64_t frame)> seek,
        std::function<std::string(uint64_t count)> rewind,
        std::function<std::string(uint64_t a, uint64_t b)> loop,
        std::function<std::string(uint64_t a, uint64_t b, std::vector<core::FrameDiffRun>& out,
                                  size_t maxRuns, bool& truncated)>
            diff);
    // Wire the DOCS command to list open documents and boot one in the emulator
    void SetOnGetOpenDocuments(std::function<std::vector<std::string>()> cb);
    void SetOnGetOpenDocumentInfo(std::function<std::vector<core::ArHost::DocumentInfo>()> cb);
    void
    SetHostMountCallbacks(std::function<std::array<core::HostDirectoryMount, 2>()> configured,
                          std::function<std::array<core::HostDirectoryMount, 2>()> effective,
                          std::function<bool(const core::HostDirectoryMount&, std::string&)> set,
                          std::function<bool(std::string&)> apply,
                          std::function<bool(core::HostMountDevice)> open,
                          std::function<bool(core::HostMountDevice)> refresh);
    void SetHardDriveControllerRomsCallback(
        std::function<void(const core::HardDriveControllerRoms&)> set);
    void SetOnRunDocumentByIndex(std::function<void(int idx)> cb);
    void SetOnCloseDocumentByIndex(std::function<void(int idx)> cb);
    // Wire G (when not running) to start the emulator, and STOP to stop it
    void SetOnStartEmulator(std::function<void()> cb);
    void SetOnStopEmulator(std::function<void()> cb);
    // Wire the BLIT command to read the current frame's blitter log (owned by Application's
    // BlitProfiler). limit <= 0 returns all blits.
    void SetOnGetBlitLog(std::function<core::BlitLogSnapshot(int limit)> cb);
    // EXO command: the Exocet window's session
    void SetOnExocetSession(std::function<core::ExocetSession*()> cb);
    // COV command: line coverage, plus the trace-history switch it depends on.
    void
    SetOnLineCoverage(std::function<nlohmann::json(const std::string&, uint32_t, uint64_t)> cb);
    void SetOnTraceHistory(std::function<bool()> isOn, std::function<void(bool)> setOn);
    void SetOnGetRegLog(std::function<core::RegLogSnapshot(int limit)> cb);
    void SetOnGetDiskLog(std::function<core::DiskLogSnapshot(const core::DiskLogQuery&)> cb);

    // Wire the AUDIO / AUDREC commands to the emulator's audio path and debug capture taps.
    void SetOnGetAudioStats(std::function<core::AudioLogSnapshot()> cb);
    void
    SetOnAudioRecord(std::function<core::AudioRecordResult(const core::AudioRecordRequest&)> cb);

    // Wire the SCEN command to scenario recording and playback.
    void SetOnScenario(std::function<core::ScenarioResult(const core::ScenarioRequest&)> cb);

    // Chipset-side audio state for the PAULA console command and the get_paula_state MCP tool.
    // Reads the custom registers directly, so it answers whether or not the Audio window is open.
    core::PaulaStateSnapshot BuildPaulaSnapshot() const;
    // Points the console's PAULA command at BuildPaulaSnapshot(). Called once at startup.
    void WirePaulaState();

    // Console output
    void AddConsoleOutputLine(const std::string& line);

    // Prints the AR break banner in the console (registers + disasm at PC)
    void NotifyDebugBreak();

    void MarkMemoryRegionsDirty();

private:
    EmulatorView* m_emulatorView;

    std::unique_ptr<DebuggerCPUView> m_cpuView;
    std::unique_ptr<DebuggerCustomView> m_customView;
    std::unique_ptr<DebuggerCopperView> m_copperView;
    std::unique_ptr<DebuggerMemoryView> m_memoryView;
    std::unique_ptr<DebuggerDisplayView> m_displayView;
    std::unique_ptr<DebuggerSpritesView> m_spritesView;
    std::unique_ptr<DebuggerBitplanesView> m_bitplanesView;
    std::unique_ptr<DebuggerBartmanView> m_bartmanView;
    std::unique_ptr<DebuggerCIAView> m_ciaView;
    std::unique_ptr<DebuggerAudioView> m_audioView;
    std::unique_ptr<DebuggerFrameScrubberView> m_scrubberView;
    std::unique_ptr<DebuggerCallStackView> m_callStackView;
    std::unique_ptr<DebuggerTrainerView> m_trainerView;

    // Source-level debug info for the running program, refreshed by Application.
    adm::core::SourceDebugContext m_sourceDebug;
    // Live-unwind cache. The unwind is cheap, but recomputing it on every ImGui redraw
    // would re-read the stack 60 times a second for no reason; it can only change when
    // the machine has run (GetRunCount ticks on steps and breakpoint stops too).
    uint64_t m_liveStackRun = UINT64_MAX;
    uint32_t m_liveStackPc = 0;
    bool m_liveStackValid = false;

    // Unwind the halted CPU's stack. False when the machine is not stopped, in which case
    // nothing can be read. Shared by the Call Stack window and the profile snapshot.
    bool ComputeLiveCallStack(adm::core::UnwindResult& outResult, std::string& outRegion);
    // Unwind (cached) and push the result to the Call Stack window.
    void UpdateLiveCallStack();
    // Classify why the trace-derived call stack is empty, for a hint that names the cause.
    DebuggerCallStackView::TraceState TraceStackState() const;
    // Note: m_annotations must precede m_consoleView / m_disasmView / m_annotationsView
    // (construction order — they all receive a pointer to it). m_breakpointsView must
    // precede m_consoleView (the console delegates breakpoint commands to it).
    std::unique_ptr<core::MemoryAnnotations> m_annotations;
    // Shared with the console (PR), the Trainer window, the register windows and the
    // memory inspector. One store is what makes a lock set in any of them appear in all
    // the others without synchronisation code.
    std::unique_ptr<core::MemoryLocks> m_locks;
    // Memory access used only for resolving lock anchors and applying patches. Separate
    // from the console's so a rescan cannot disturb whatever the console is mid-way
    // through, and refreshed on demand rather than every frame.
    core::ArMemoryAccess m_lockMem;
    std::unique_ptr<BreakpointsView> m_breakpointsView;
    std::unique_ptr<ARConsoleView> m_consoleView;
    std::unique_ptr<DebuggerDisassemblyView> m_disasmView;
    std::unique_ptr<MemoryAnnotationsView> m_annotationsView;

    // A single owned copy of the current trace is shared by lazy consumers. This keeps the
    // emulator's next frame from invalidating their source without duplicating the large buffer.
    std::vector<adm::AmigaEmulator::CpuTraceEntry> m_cpuTraceSnapshot;

    // One shared context driving every address hyperlink + right-click menu. Built in the
    // constructor and handed (by pointer) to each sub-view that draws address links.
    AddressActionContext m_addrCtx;
    void BuildAddressContext();
};

} // namespace adm::ui
