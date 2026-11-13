#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include <nlohmann/json.hpp>

#include "../../core/AmigaEmulator.h"
#include "../../core/AmigaScreenMap.h"
#include "../../core/BitplaneLayout.h"
#include "../../core/BlitProfiler.h"
#include "../../core/CallStackProfiler.h"
#include "../../core/CopperDisassembler.h"
#include "../../core/SegListResolver.h"
#include "../BlitDestPreview.h"
#include "../FramePreview.h"
#include "AddressActions.h"

namespace adm::core {
class DebugSymbols;
class DebugLines;
class DebugInline;
} // namespace adm::core

namespace adm::ui {

class DebuggerFrameScrubberView
{
public:
    DebuggerFrameScrubberView();
    ~DebuggerFrameScrubberView();

    void Open() { m_isOpen = true; }
    void Restore()
    {
        m_isOpen = true;
        m_noFocusOnAppearing = true;
    }
    void Close() { m_isOpen = false; }
    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenPtr() { return &m_isOpen; }
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }

    void Draw();
    void Clear();

    // Feed data each frame before Draw()
    void SetBlitRecords(const std::vector<adm::core::BlitRecord>& records);
    void SetCopperEvents(const std::vector<adm::AmigaEmulator::CopperExecEvent>& events);

    // Bartman idle-region markers (debug_start_idle/stop_idle). Drives the Idle row: paired
    // start/stop events are drawn as blocks so the effect's idle (vblank-wait) time is visible.
    void SetIdleEvents(const std::vector<adm::AmigaEmulator::IdleEvent>& events);
    void SetCpuTraceData(const adm::AmigaEmulator::CpuTraceEntry* entries, int count);
    void SetDmaRecordData(const adm::AmigaEmulator::DmaSlotRecord* records, int hposCount,
                          int vposCount);

    // Supply source-level symbols (+ optional DWARF line info) and the running program's
    // seglist so the timeline can paint a colored band per function across the frame and
    // show file:line in the hover tooltip (syms=nullptr disables).
    void SetFunctionSymbols(const adm::core::DebugSymbols* syms, const adm::core::DebugLines* lines,
                            const adm::core::DebugInline* inlines,
                            std::vector<adm::core::AmigaSegment> segments);

    // Frame-history timeline state, fed each frame. Drives the seek bar above the in-frame
    // timeline: that bar picks *which* frame, the timeline below picks where inside it.
    // `enabled` is the Frame History preference, `recording` whether it has actually armed.
    // The bar is drawn (disabled) whenever recording is switched on, and not drawn at all
    // when it is switched off - an empty transport is more use than a sentence explaining
    // why there isn't one, but only to someone who asked for a recorder.
    //
    // `firstSeekable` is the oldest frame that can actually be seeked to (0 = none): the
    // oldest frame held is only ever a base to re-run from, and it is not always the one
    // straight after it, because a frame the machine never completed leaves a hole in the
    // numbering. The bar must offer what the core will accept, not oldest + 1.
    void SetFrameTimeline(bool enabled, bool recording, uint64_t firstSeekable, uint64_t newest,
                          uint64_t position)
    {
        // A changed range can mean the timeline was trimmed or re-recorded, so a cached
        // preview may no longer belong to the frame it is filed under.
        m_preview.SetRange(firstSeekable, newest);
        m_histEnabled = enabled;
        m_histRecording = recording;
        m_histFirst = firstSeekable;
        m_histNewest = newest;
        if (!m_histSeeking)
            m_histPosition = position;
    }

    // Fired when the seek bar is released on a new frame. Seeking mid-drag would re-run a
    // frame for every pixel of mouse travel.
    std::function<void(uint64_t frame)> m_onSeekFrame;

    // The armed A-B loop and the transport state, fed each frame. Carries both back from
    // the core so a loop armed elsewhere (the console's LOOP command) shows up on the bar,
    // and the play/pause glyph matches what the timeline is actually doing.
    void SetFrameTimelineState(bool active, uint64_t a, uint64_t b, bool playing, int mode,
                               double speed, const std::string& stopReason)
    {
        m_loopActive = active;
        m_timelinePlaying = playing;
        m_timelineMode = mode;
        m_timelineSpeed = speed;
        m_timelineStopReason = stopReason;
        // A gripper drag owns the marks until it is released - the armed range is fed back
        // every frame, and taking it while dragging would snap the handle straight back.
        const bool draggingMark = m_barDrag == BarDrag::LoopA || m_barDrag == BarDrag::LoopB;
        if (active && !draggingMark) {
            m_loopA = a;
            m_loopB = b;
        }
    }

    // Arm the A-B loop over [a,b]; (0,0) disarms. Replaying a range keeps the machine
    // paused, so this is the core's call to make, not the view's.
    std::function<void(uint64_t a, uint64_t b)> m_onSetFrameLoop;

    // Transport over the recorded timeline. Every one of these acts on the timeline, never
    // on the live machine - the core decides what that means.
    std::function<void(bool playing)> m_onTimelinePlay;
    std::function<void(int delta)> m_onTimelineStep; // +1 / -1 frames
    std::function<void(int mode)> m_onTimelineMode;  // 0 fwd, 1 rev, 2 ping-pong
    std::function<void(double speed)> m_onTimelineSpeed;
    // Throw the whole recorded timeline away and start filling it again.
    std::function<void()> m_onClearTimeline;

    // Arm or disarm the recorder itself. Recording is off until it is asked for - a save
    // state every frame is not something to charge a session that never rewinds - so the
    // transport carries the one control that turns it on.
    std::function<void(bool recording)> m_onSetRecording;

    // Screen thumbnail for a frame, used to preview where a scrub is about to land. The
    // store keeps one every N frames, so it answers with the nearest shot at or before the
    // frame asked for and reports which frame that actually is. Unset = no previews.
    FramePreview::Source m_onGetFrameThumbnail;

    // Set the frame-start CCK anchor so absolute_cycle fields can be normalized.
    // Call each frame with AmigaEmulator::GetFrameStartCycle() before Draw().
    void SetFrameStartCycle(uint32_t cycle) { m_frameStartCycle = cycle; }

    // Mark the timeline as a partial frame stopped at a mid-frame breakpoint: draws a break
    // marker at breakLRef (vpos*CCKPerLine + hpos) and dims the not-yet-executed region.
    void SetBreakMarker(bool partial, uint32_t breakLRef)
    {
        m_partialFrame = partial;
        m_breakLRef = breakLRef;
    }

    // Fired when the scrubber handle is moved
    std::function<void(uint32_t lRef)> m_onScrubberChanged;

    // Set scrubber position from an external view click (no callback fired, pans to show it)
    void SetScrubberLRef(uint32_t lRef);

    // Tick unit (0=%, 1=µs, 2=Cycles, 3=Rasterlines) and the instruction-# overlay toggle.
    // Persisted across sessions via UserPreferences.
    int GetTickUnit() const { return m_tickUnit; }
    void SetTickUnit(int unit) { m_tickUnit = unit; }
    bool GetShowInstrNumbers() const { return m_showInstrNumbers; }
    void SetShowInstrNumbers(bool show) { m_showInstrNumbers = show; }
    int GetTopRowMode() const { return m_topRowMode; }
    void SetTopRowMode(int mode) { m_topRowMode = mode; }

    // Highlight a specific blit bar (by lRef) from an external hover — UINT32_MAX = clear
    void SetHighlightedBlit(uint32_t lRef) { m_highlightedBlitLRef = lRef; }

    // Pin a blit's detail panel from outside — same state a click on the bar sets, used when
    // the blit was picked off the emulator picture instead. Pans it into view.
    // UINT32_MAX unpins.
    void PinBlit(uint32_t lRef);

    // Highlight a specific copper tick (by lRef) from an external hover — UINT32_MAX = clear
    void SetHighlightedCopper(uint32_t lRef) { m_highlightedCopperLRef = lRef; }

    // Fired when the DMA row is hovered over a traced CPU instruction's slot-run; passes the
    // owning instruction's start lRef (UINT32_MAX when over none). Drives the disasm row
    // highlight. Fires only on change.
    std::function<void(uint32_t lRef)> m_onCpuSlotHovered;

    // Display info for a traced instruction, resolved on demand for the DMA-slot tooltip. The
    // disassembly view owns the computed mnemonic + cycle costs, so the scrubber queries it
    // rather than re-disassembling.
    struct CpuInstrInfo
    {
        bool valid = false;
        std::string text;         // disassembled mnemonic
        int nominalCycles = -1;   // textbook 68000 CPU cycles (-1 = not modeled)
        uint32_t deltaCycles = 0; // measured color-clocks elapsed (0 = unknown / last entry)
        uint32_t pc = 0;
    };
    // Resolve the trace instruction at the given index (a span's "#"). Set by DebuggerView.
    std::function<CpuInstrInfo(int index)> m_queryInstrInfo;

    // Shared address-link context for the pinned blit-detail address hyperlinks.
    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }

    // Fired when the minterm value in the pinned blit-detail window is clicked — opens the
    // Blitter Minterm window populated from that blit's BLTCON0/BLTCON1.
    std::function<void(uint16_t bltcon0, uint16_t bltcon1)> m_onOpenMinterm;

    // Fired when a copper instruction tick is clicked — jumps to that instruction in the
    // Copper trace window (and syncs the other linked views).
    std::function<void(uint32_t lRef)> m_onJumpToCopper;

    // Fired from the timeline right-click menu "Break at this line" — arms a raster (beam)
    // breakpoint at the given scanline (vpos).
    std::function<void(int vpos)> m_onBreakAtLine;
    int m_ctxMenuLine = 0; // scanline captured when the timeline context menu was opened

    // Fired when the blit to overlay on the emulator changes: the hovered blit, or the
    // pinned blit when nothing is hovered (UINT32_MAX = none). Fires only on change.
    std::function<void(uint32_t lRef)> m_onActiveBlitChanged;

    // Current bitplane layout + screen map, so the pinned detail panel can show whether the
    // blit maps to a visible emulator overlay (and which buffer). Fed each frame.
    void SetOverlayContext(const core::BitplaneLayout& layout, const core::AmigaScreenMap& map)
    {
        m_overlayLayout = layout;
        m_overlayMap = map;
    }

private:
    const AddressActionContext* m_addrCtx = nullptr;
    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;
    bool m_turboSuppressed = false;

    // Current data
    std::vector<adm::core::BlitRecord> m_blitRecords;
    std::vector<adm::AmigaEmulator::CopperExecEvent> m_copperEvents;

    // Bartman idle-region markers (raw start/stop events; paired into spans at draw time using
    // the current frame-start anchor so a start/stop that straddles the frame edge is handled).
    std::vector<adm::AmigaEmulator::IdleEvent> m_idleEvents;
    // Once Bartman idle markers have appeared, retain the row for the rest of the debugger
    // session so frames without markers do not change the scrubber's height.
    bool m_idleRowActive = false;
    // A paired idle span in frame-relative CCK. openStart/openEnd mark spans clamped to the frame
    // edge because their matching marker fell outside the displayed frame window.
    struct IdleSpan
    {
        float startLRef;
        float endLRef;
        bool openStart;
        bool openEnd;
    };
    // Pairs m_idleEvents into spans (normalized by m_frameStartCycle). Recomputed on demand.
    std::vector<IdleSpan> BuildIdleSpans() const;

    // CPU trace density: per-scanline instruction counts (0..312 for PAL)
    static constexpr int kMaxScanlines = 320;
    uint16_t m_cpuDensity[kMaxScanlines] = {};
    bool m_hasCpuTrace = false;
    // Latched like m_idleRowActive: once a CPU trace / DMA records have been seen, their rows
    // and controls keep their space for the rest of the session. The per-frame flags come and
    // go (a frame with no trace, a DMA fetch that failed), and sizing the timeline and the
    // status row from them made the whole scrubber jump from frame to frame.
    bool m_cpuTraceSeen = false;
    bool m_dmaSeen = false;
    const adm::AmigaEmulator::CpuTraceEntry* m_cpuTraceEntries = nullptr;
    int m_cpuTraceCount = 0;
    bool m_cpuTraceDirty = false;

    // Per-scanline aggregates for the alternate top-row heatmaps (built from the CPU trace).
    // DrawTimeline recomputes the per-mode normalization max each frame (DMA modes need it too),
    // so no cached maxima are kept here.
    uint32_t m_stallDensity[kMaxScanlines] = {};  // measured CPU cycles over textbook
    uint32_t m_cyclesPerLine[kMaxScanlines] = {}; // measured CPU cycles executed
    uint16_t m_branchPerLine[kMaxScanlines] = {}; // taken branches/jumps/calls/returns

    // Top-row heatmap selector. Trace-based: 0 CPU density, 1 bus stalls, 2 cycles/line,
    // 3 branch churn. DMA-based: 4 DMA pressure, 5 CPU headroom, 6 Bitplane, 7 Copper,
    // 8 Blitter, 9 Sprite.
    int m_topRowMode = 0;

    // Returns the per-scanline value + running max for the selected heatmap mode. Pulls from
    // the trace aggregates or the per-scanline DMA summary depending on the mode.
    uint32_t HeatLineValue(int mode, int line) const;

    // Per-instruction CCK span, derived from the CPU trace. Instruction i owns the half-open
    // slot range [startLRef, endLRef) (endLRef = next instruction's lRef). Used to tie DMA
    // slots back to the instruction that issued them. Sorted ascending by startLRef.
    struct CpuInstrSpan
    {
        uint32_t startLRef;
        uint32_t endLRef;
        uint32_t pc; // instruction address (for source-level + inline resolution)
        uint32_t sp; // stack pointer (A7) — drives the real-call stack depth
        int index;   // sequential trace index (the disassembly's "#")
    };
    std::vector<CpuInstrSpan> m_cpuSpans;

    // Returns the index into m_cpuSpans whose [start,end) contains lRef, or -1 if none.
    int FindCpuSpan(uint32_t lRef) const;

    // Source-level function bands on the timeline. Two modes:
    //   Flat  (0): the leaf function at each point — consecutive same-function spans
    //              merged into one depth-0 band (fine-grained, robust).
    //   Flame (1): the DWARF inline stack at each PC — inlined callees stacked below
    //              their caller (matches Bartman's view; stable, from debug info).
    // Flat leaf view (all depth 0) and the full call-stack view (real calls + inline
    // expansion), both built each rebuild; the toggle picks which to render, the call-
    // stack window always reads m_callFrames.
    std::vector<adm::core::CallFrame> m_flatFrames;
    std::vector<adm::core::CallFrame> m_callFrames;
    int m_callMaxDepth = 0;
    int m_bandMode = 0; // 0 = flat, 1 = flame
    const adm::core::DebugSymbols* m_debugSymbols = nullptr;
    const adm::core::DebugLines* m_debugLines = nullptr;
    const adm::core::DebugInline* m_debugInline = nullptr;
    std::vector<adm::core::AmigaSegment> m_segments;
    bool m_bandsDirty = false;
    bool m_showFuncBands = true;
    void EnsureCpuTraceProcessed();
    void RebuildBands();
    // The frames + max depth for the currently-selected display mode.
    const std::vector<adm::core::CallFrame>& DisplayFrames() const
    {
        return m_bandMode == 1 ? m_callFrames : m_flatFrames;
    }
    int DisplayMaxDepth() const { return m_bandMode == 1 ? m_callMaxDepth : 0; }
    // Number of depth rows to draw (0 = hidden), capped for layout sanity.
    int FlameShownDepths() const;

public:
    // Rebuild the call-stack/flame frames if the trace changed (call before querying
    // GetCallStackAt when the scrubber panel itself may not have been drawn this frame).
    void EnsureBands()
    {
        EnsureCpuTraceProcessed();
        if (m_bandsDirty)
            RebuildBands();
    }
    // The call stack (real calls + inlined functions, outermost first) active at a
    // timeline position, for the Call Stack window. Empty if nothing covers `lRef`.
    std::vector<adm::core::CallFrame> GetCallStackAt(uint32_t lRef);
    // Flat leaf-function bands over the whole traced frame (Flat mode). Rebuilds if stale.
    const std::vector<adm::core::CallFrame>& GetFlatFrames()
    {
        EnsureBands();
        return m_flatFrames;
    }
    // Call-stack flame bands over the whole traced frame (Flame mode). Rebuilds if stale.
    const std::vector<adm::core::CallFrame>& GetFlameFrames()
    {
        EnsureBands();
        return m_callFrames;
    }
    // Current scrubber playhead position (color-clock ref).
    uint32_t GetScrubberLRef() const { return static_cast<uint32_t>(m_scrubberL); }

    // Renders the copper tooltip for the event at `index` in the frame's trace. Returns false
    // when the index is out of range. Exposed so the raster overlay reuses this exact tooltip
    // rather than growing a second one that drifts out of sync with it.
    bool DrawCopperEventTooltip(int index, uint32_t lRef);
    // Whether source-level symbols are loaded (drives the Call Stack window's hint).
    bool HasDebugInfo() const { return m_debugSymbols != nullptr; }
    // Whether a CPU trace was captured for the current frame. Distinguishes "tracing is
    // off" from "traced, but nothing under the playhead" in the Call Stack window.
    bool HasCpuTrace() const { return m_hasCpuTrace; }
    // Whether the running program's load address is known (seglist walked).
    bool HasSegments() const { return !m_segments.empty(); }

private:
    // Hover tooltip for an instruction's DMA slot-run: instruction text, expected vs measured
    // cycles (color-coded delta), and a per-channel key of the bus slots it consumed.
    void DrawCpuSlotTooltip(int spanIdx);

    // Dedup for m_onCpuSlotHovered.
    uint32_t m_lastCpuSlotHovered = UINT32_MAX;
    void EmitCpuSlotHover(uint32_t lRef);

    // Frame-history seek bar state.
    bool m_histEnabled = false;   // Frame History -> Record frames
    bool m_histRecording = false; // ...and it has armed (a debugger window is open)
    uint64_t m_histFirst = 0;     // oldest seekable frame, 0 when there is none
    uint64_t m_histNewest = 0;
    uint64_t m_histPosition = 0;
    bool m_histSeeking = false; // a drag is in progress; ignore fed-in positions until it ends

    // A-B loop marks. 0 = unset; both must be set before the loop can be armed. Held here
    // rather than in the core so a range can be marked out and adjusted before it runs.
    uint64_t m_loopA = 0;
    uint64_t m_loopB = 0;
    bool m_loopActive = false; // the core is replaying the range right now
    bool m_timelinePlaying = false;
    int m_timelineMode = 0; // 0 forward, 1 reverse, 2 ping-pong
    double m_timelineSpeed = 1.0;
    // Why playback stopped on its own, straight from the core. Shown on the bar: a replay
    // that halts for a reason nobody is told reads as the tool being broken.
    std::string m_timelineStopReason;

    // What the press that is currently on the seek bar is moving. One item owns the whole
    // bar, so the mode is decided when the button goes down and held until it comes up -
    // overlapping widgets would fight over the same click.
    enum class BarDrag { None, Seek, LoopA, LoopB };
    BarDrag m_barDrag = BarDrag::None;

    // Scrub preview. The texture is re-uploaded only when the previewed frame changes, so
    // holding the mouse still over the bar costs nothing.
    FramePreview m_preview;
    void DrawFrameSeekBar();
    // The record arm at the head of the row. Always live, even when everything beside it
    // is dead - it is what makes the rest of the row work.
    void DrawRecordButton();
    // Settle a seek whose drag ended while the bar was not being drawn.
    void FlushPendingSeek();
    // The A/B/loop buttons and the transport that sit to the left of the seek bar.
    void DrawFrameLoopControls(uint64_t first);
    // Speed ladder: the label for a rate, and the rung after it (wrapping round).
    static const char* SpeedLabel(double speed);
    static double NextSpeed(double speed);

    // Scrubber state
    float m_scrubberL = 0.0f;
    bool m_scrubberDragging = false;
    float m_scrubberDragStartX = 0.0f;
    float m_scrubberDragStartL = 0.0f;

    // Frame-start cycle anchor for normalizing absolute_cycle event fields
    uint32_t m_frameStartCycle = 0;

    // Partial-frame (mid-frame breakpoint) marker state
    bool m_partialFrame = false;
    uint32_t m_breakLRef = 0;

    // External blit highlight from BlitterView hover (UINT32_MAX = none)
    uint32_t m_highlightedBlitLRef = UINT32_MAX;

    // External copper highlight from CopperView hover (UINT32_MAX = none)
    uint32_t m_highlightedCopperLRef = UINT32_MAX;

    // Overlay context for the pinned detail panel's availability band (fed each frame).
    core::BitplaneLayout m_overlayLayout;
    core::AmigaScreenMap m_overlayMap;

    // Renders "what this blit wrote" inside the blit hover tooltip and the pinned panel.
    // Empty unless destination capture is armed in the Blitter Operations window.
    BlitDestPreview m_blitDestPreview;

    // Pinned blit detail window: identified by lRef (UINT32_MAX = none pinned).
    // Set by clicking a blit bar; the hover tooltip freezes in place as a draggable,
    // chrome-less panel with clickable source/destination memory links.
    uint32_t m_pinnedBlitLRef = UINT32_MAX;
    // Screen position captured at click time so the panel freezes where the tooltip was.
    ImVec2 m_pinnedWindowPos = {0.0f, 0.0f};
    bool m_pinnedPosPending = false;

    // Zoom / pan
    float m_zoom = 1.0f;
    float m_scrollL = 0.0f;
    bool m_panning = false;
    float m_panStartX = 0.0f;
    float m_panStartScroll = 0.0f;

    static constexpr float kTotalSlots = 71364.0f;

    // Per-scanline DMA summary for the DMA row
    struct DmaScanlineSummary
    {
        uint16_t counts[10] = {}; // indexed by DMARECORD_* type (0..9)
        uint16_t total = 0;       // non-idle slots in this scanline
    };
    static constexpr int kMaxDmaScanlines = 320;
    DmaScanlineSummary m_dmaScanlines[kMaxDmaScanlines] = {};
    bool m_hasDmaData = false;
    int m_dmaHposCount = 0;
    int m_dmaVposCount = 0;
    const adm::AmigaEmulator::DmaSlotRecord* m_dmaRecords =
        nullptr; // for high-zoom per-slot rendering

    // 0=%, 1=µs, 2=Cycles, 3=Rasterlines
    int m_tickUnit = 0;

    // Whether to stamp the "#N" trace index over each instruction's DMA slot-run when zoomed.
    bool m_showInstrNumbers = true;

    // Shared per-frame geometry + interaction state threaded through the DrawTimeline stages.
    struct TimelineCtx
    {
        ImVec2 canvasPos{};
        float canvasWidth = 0.0f;
        float cpuRowH = 0.0f;      // top heatmap strip height (0 when no data)
        float dmaRowH = 0.0f;      // DMA channel row height (0 when no DMA data)
        float idleRowH = 0.0f;     // Bartman idle-region row height (0 when no idle markers)
        float funcRowH = 0.0f;     // source-level function band row (0 when no symbols/bands)
        float canvasH = 0.0f;      // total canvas height (rows + bars/ticks band)
        float totalH = 0.0f;       // canvasH + the bottom label line
        float visibleRange = 0.0f; // CCK span currently visible
        float slotToPixel = 0.0f;  // pixels per CCK
        float scrubPixelX = 0.0f;  // final on-canvas X of the scrubber marker
        bool hovered = false;
        bool active = false;
        bool activated = false;
        // Band y-extents, shared by hit-testing and rendering so they can't drift apart.
        float blitY0 = 0.0f, blitY1 = 0.0f;
        float copperY0 = 0.0f, copperY1 = 0.0f;
        ImDrawList* dl = nullptr;
        // Produced by DrawBlitBars, consumed by the blit hover tooltip in DrawTimeline.
        int hoveredBlit = -1;
        int pinnedBlit = -1;
    };

    // Orchestrates the stages below over a freshly-built TimelineCtx.
    void DrawTimeline();

    // Maps a frame ref (lRef, color clocks) to its canvas X using the ctx pan/zoom state.
    float LRefToX(const TimelineCtx& c, float l) const;
    // Hit-tests the visible blit bars / copper ticks at mouse position m; -1 if none.
    int HitTestBlit(const TimelineCtx& c, ImVec2 m) const;
    int HitTestCopper(const TimelineCtx& c, ImVec2 m) const;

    // DrawTimeline split into stages (zoom/pan/drag input, then the render passes in z-order).
    void HandleTimelineInput(TimelineCtx& c);
    void DrawHeatRow(const TimelineCtx& c);
    void DrawDmaRow(const TimelineCtx& c);
    void DrawIdleRow(const TimelineCtx& c);
    void DrawFunctionRow(const TimelineCtx& c);
    void DrawAxis(const TimelineCtx& c);
    void DrawBlitBars(TimelineCtx& c);
    void DrawCopperTicks(const TimelineCtx& c);

    // Renders the per-blit detail breakdown (timing, flags, channels, minterm, presets).
    // Shared by the hover tooltip (interactive=false) and the pinned window (interactive=true,
    // where source/destination addresses become clickable memory hyperlinks).
    // When interactive, *addrHovered is set true if the mouse is over an address link.
    void DrawBlitDetailContents(const adm::core::BlitRecord& rec, int index, bool interactive,
                                bool* addrHovered);

    // Draws the pinned blit-detail window if a blit is pinned.
    void DrawPinnedBlitWindow();

    // Fires m_onActiveBlitChanged when the overlay target changes (deduplicated).
    void EmitActiveBlit(uint32_t lRef);

    // Last lRef pushed through m_onActiveBlitChanged (UINT32_MAX = none).
    uint32_t m_lastActiveBlitFired = UINT32_MAX;

    // Custom-register definitions, lazily loaded so copper tick tooltips can disassemble
    // MOVE/WAIT/SKIP instructions (same JSON the Copper window uses).
    nlohmann::json m_copperRegsDef;
    bool m_copperRegsDefLoaded = false;
    void LoadCopperRegsDef();

    // Hover tooltip for a copper instruction tick (interactive=false path of detail rendering).
    void DrawCopperTickTooltip(const adm::AmigaEmulator::CopperExecEvent& ev, uint32_t lRef);
};

} // namespace adm::ui
