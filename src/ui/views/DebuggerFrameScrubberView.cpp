#include "DebuggerFrameScrubberView.h"

#include <fmt/format.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <utility>

#include "../../core/BlitScreenMapper.h"
#include "../../core/Constants.h"
#include "../../core/DebugInline.h"
#include "../../core/DebugLines.h"
#include "../../core/DebugSymbols.h"
#include "../../core/M68kCycles.h"
#include "../AddressLink.h"
#include "../DmaColors.h"
#include "../HyperlinkText.h"
#include "../IconsFontAwesome6.h"
#include "BlitOpPresets.h"
#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

static constexpr ImVec4 kColorCpu = {0.2f, 0.6f, 1.0f, 0.85f};
static constexpr ImVec4 kColorCopper = {1.0f, 0.6f, 0.1f, 0.85f};
static constexpr ImVec4 kColorLine = {0.5f, 1.0f, 0.3f, 0.85f};

DebuggerFrameScrubberView::DebuggerFrameScrubberView() {}

DebuggerFrameScrubberView::~DebuggerFrameScrubberView() {}

void DebuggerFrameScrubberView::SetScrubberLRef(uint32_t lRef)
{
    m_scrubberL = static_cast<float>(lRef);
    // Pan to keep the scrubber visible
    float visibleRange = kTotalSlots / m_zoom;
    if (m_scrubberL < m_scrollL || m_scrubberL > m_scrollL + visibleRange)
        m_scrollL = std::clamp(m_scrubberL - visibleRange * 0.4f, 0.0f, kTotalSlots - visibleRange);
}

void DebuggerFrameScrubberView::PinBlit(uint32_t lRef)
{
    m_pinnedBlitLRef = lRef;
    if (lRef == UINT32_MAX)
        return;

    // Freeze the panel next to the cursor, exactly as clicking the bar itself does — the
    // cursor is over the emulator picture in this path, which is where the user is looking.
    const ImVec2 mp = ImGui::GetIO().MousePos;
    m_pinnedWindowPos = {mp.x + 16.0f, mp.y + 16.0f};
    m_pinnedPosPending = true;

    // A blit picked off the screen is very often outside the visible L-range.
    SetScrubberLRef(lRef);
}

void DebuggerFrameScrubberView::SetBlitRecords(const std::vector<adm::core::BlitRecord>& records)
{
    m_blitRecords = records;
}

void DebuggerFrameScrubberView::SetCopperEvents(
    const std::vector<adm::AmigaEmulator::CopperExecEvent>& events)
{
    m_copperEvents = events;
}

void DebuggerFrameScrubberView::SetIdleEvents(
    const std::vector<adm::AmigaEmulator::IdleEvent>& events)
{
    m_idleEvents = events;
    if (!events.empty())
        m_idleRowActive = true;
}

std::vector<DebuggerFrameScrubberView::IdleSpan> DebuggerFrameScrubberView::BuildIdleSpans() const
{
    std::vector<IdleSpan> spans;
    if (m_idleEvents.empty())
        return spans;

    // Each marker's beam position (vpos/hpos) maps to a frame-relative lRef the same way copper,
    // blit and CPU rows do (lRef = vpos*CCKPerLine + hpos), so the Idle row lines up with them.
    // Events are already frame-windowed by the emulator, but a start/stop pair can straddle the
    // frame boundary (idle typically brackets the vblank wait), so a lone start runs to the frame
    // end and a lone stop runs from the frame start.
    constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;
    struct Marker
    {
        float lRef;
        bool start;
    };
    std::vector<Marker> marks;
    marks.reserve(m_idleEvents.size());
    for (const auto& e : m_idleEvents) {
        float lRef = static_cast<float>(static_cast<uint32_t>(e.vpos) * kCCKPerLine +
                                        static_cast<uint32_t>(e.hpos));
        lRef = std::clamp(lRef, 0.0f, kTotalSlots);
        marks.push_back({lRef, e.start != 0});
    }
    std::stable_sort(marks.begin(), marks.end(),
                     [](const Marker& a, const Marker& b) { return a.lRef < b.lRef; });

    bool inIdle = false;
    float spanStart = 0.0f;
    bool openStart = false;
    for (const auto& m : marks) {
        if (m.start) {
            if (!inIdle) {
                inIdle = true;
                spanStart = m.lRef;
                openStart = false;
            }
            // Nested/duplicate start markers are ignored — the outermost region wins.
        } else {
            if (inIdle) {
                spans.push_back({spanStart, m.lRef, openStart, false});
                inIdle = false;
            } else {
                // Lone stop: the idle began before the frame started (carried across the vblank).
                spans.push_back({0.0f, m.lRef, true, false});
            }
        }
    }
    if (inIdle) // Lone start: idle runs to the end of the frame.
        spans.push_back({spanStart, kTotalSlots, openStart, true});

    return spans;
}

void DebuggerFrameScrubberView::SetCpuTraceData(const adm::AmigaEmulator::CpuTraceEntry* entries,
                                                int count)
{
    m_cpuTraceEntries = entries;
    m_cpuTraceCount = std::max(0, count);
    m_cpuTraceDirty = true;
    m_hasCpuTrace = (count > 0);
    m_cpuTraceSeen = m_cpuTraceSeen || m_hasCpuTrace;
    m_bandsDirty = true;
}

void DebuggerFrameScrubberView::EnsureCpuTraceProcessed()
{
    if (!m_cpuTraceDirty)
        return;
    m_cpuTraceDirty = false;

    const auto* entries = m_cpuTraceEntries;
    const int count = m_cpuTraceCount;
    std::fill(std::begin(m_cpuDensity), std::end(m_cpuDensity), 0u);
    std::fill(std::begin(m_stallDensity), std::end(m_stallDensity), 0u);
    std::fill(std::begin(m_cyclesPerLine), std::end(m_cyclesPerLine), 0u);
    std::fill(std::begin(m_branchPerLine), std::end(m_branchPerLine), 0u);
    m_cpuSpans.clear();
    m_cpuSpans.reserve(static_cast<size_t>(std::max(0, count)));
    m_bandsDirty = true; // new trace -> function bands must be recomputed

    constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;
    for (int i = 0; i < count; ++i) {
        int line = entries[i].vpos;
        if (line >= 0 && line < kMaxScanlines) {
            if (m_cpuDensity[line] < 0xFFFF)
                ++m_cpuDensity[line];
        }

        // Build the per-instruction CCK span. endLRef = next instruction's lRef; the last
        // entry has no successor so give it a nominal 2-CCK (one bus access) width.
        uint32_t startLRef = static_cast<uint32_t>(entries[i].vpos) * kCCKPerLine + entries[i].hpos;
        uint32_t endLRef;
        bool haveNext = (i + 1 < count);
        if (haveNext) {
            uint32_t next =
                static_cast<uint32_t>(entries[i + 1].vpos) * kCCKPerLine + entries[i + 1].hpos;
            endLRef = (next > startLRef) ? next : startLRef + 2;
        } else {
            endLRef = startLRef + 2;
        }
        m_cpuSpans.push_back({startLRef, endLRef, entries[i].pc, entries[i].a[7], i});

        // Per-scanline stall aggregate for the stall heatmap: measured CPU cycles (CCK*2)
        // minus the textbook 68000 cost; clamped at 0. Computed straight from the trace's
        // raw words (mirrors the disassembly view's nominal-cycle decode).
        if (haveNext && line >= 0 && line < kMaxScanlines) {
            int measuredCpu = static_cast<int>(endLRef - startLRef) * 2;

            // Cycles-per-line heatmap: total measured CPU cycles attributed to this line.
            m_cyclesPerLine[line] += static_cast<uint32_t>(measuredCpu);

            uint32_t fallThrough = entries[i].pc + entries[i].raw_len;
            bool diverged = (entries[i + 1].pc != fallThrough);

            // Branch-churn heatmap: count instructions that did not fall through.
            if (diverged && m_branchPerLine[line] < 0xFFFF)
                ++m_branchPerLine[line];

            uint16_t words[5] = {0};
            const int nwords = std::min<int>(entries[i].raw_len / 2, 5);
            for (int wi = 0; wi < nwords; ++wi)
                words[wi] = static_cast<uint16_t>((entries[i].raw[wi * 2] << 8) |
                                                  entries[i].raw[wi * 2 + 1]);
            int nominal = core::EstimateM68000Cycles(words, nwords, diverged, entries[i].d);
            if (nominal >= 0) {
                int stall = measuredCpu - nominal;
                if (stall > 0)
                    m_stallDensity[line] += static_cast<uint32_t>(stall);
            }
        }
    }
}

void DebuggerFrameScrubberView::SetFunctionSymbols(const adm::core::DebugSymbols* syms,
                                                   const adm::core::DebugLines* lines,
                                                   const adm::core::DebugInline* inlines,
                                                   std::vector<adm::core::AmigaSegment> segments)
{
    m_debugSymbols = syms;
    m_debugLines = lines;
    m_debugInline = inlines;
    m_segments = std::move(segments);
    m_bandsDirty = true;
}

// Stable, well-distributed color per function NAME (golden-ratio hue hashing), so the
// same function is the same color in both flat and flame views.
static ImU32 FunctionBandColor(const std::string& name)
{
    uint32_t h = 2166136261u; // FNV-1a
    for (char ch : name)
        h = (h ^ static_cast<uint8_t>(ch)) * 16777619u;
    float hue = static_cast<float>(h & 0xFFFFu) / 65535.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.85f, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

// Flame-chart layout: max call depths shown and the pixel height of one depth row.
static constexpr int kMaxFlameDepths = 10;
static constexpr float kFlameDepthH = 11.0f;

int DebuggerFrameScrubberView::FlameShownDepths() const
{
    if (!m_showFuncBands || DisplayFrames().empty())
        return 0;
    return std::min(DisplayMaxDepth() + 1, kMaxFlameDepths);
}

void DebuggerFrameScrubberView::RebuildBands()
{
    m_bandsDirty = false;
    m_flatFrames.clear();
    m_callFrames.clear();
    m_callMaxDepth = 0;
    if (!m_debugSymbols || m_segments.empty() || m_cpuSpans.empty())
        return;

    const uint32_t base = m_segments.front().base;
    const uint32_t codeEnd = base + m_segments.front().size;
    const core::DebugSymbols* syms = m_debugSymbols;
    const bool haveInline = m_debugInline && m_debugInline->IsLoaded();

    // --- FLAT: consecutive same-function instructions merged into one depth-0 band. ---
    {
        bool haveCur = false;
        for (const CpuInstrSpan& s : m_cpuSpans) {
            const core::FunctionSymbol* fn =
                (s.pc >= base && s.pc < codeEnd) ? syms->ResolvePc(s.pc - base) : nullptr;
            if (!fn) {
                haveCur = false;
                continue;
            }
            if (haveCur && m_flatFrames.back().name == fn->name)
                m_flatFrames.back().endLRef = std::max(m_flatFrames.back().endLRef, s.endLRef);
            else {
                m_flatFrames.push_back({s.startLRef, s.endLRef, 0, fn->address, fn->name});
                haveCur = true;
            }
        }
    }

    // --- FLAME (full Bartman match): real call stack + inline expansion. ---
    //
    // Real-call frames come from the stack pointer + DWARF function identity, NOT
    // BSR/JSR opcodes: A7 gives exact depth (a call pushes a return address; an
    // interrupt pushes a whole exception frame; RTE/RTS restore A7), and DWARF tells
    // us which function each PC is in. Robust where BSR/JSR reconstruction staircased —
    // interrupts nest correctly because their SP drop then restore is just a deeper
    // frame that pops on return. On top we append the inlined functions at the current
    // PC (DebugInline), giving _start -> main -> ... -> voxel_c2p_kick ->
    // voxel_c2p_pass2 -> system_wait_blt. This same structure feeds the Call Stack
    // window (a vertical slice at the playhead).
    struct RealFrame
    {
        std::string name;
        uint32_t entrySp;
    };
    std::vector<RealFrame> callStack;
    std::vector<int> open; // per-depth currently-open frame index (or -1)

    auto emit = [&](int depth, const std::string& name, const CpuInstrSpan& s) {
        if (static_cast<int>(open.size()) <= depth)
            open.resize(depth + 1, -1);
        if (open[depth] >= 0 && m_callFrames[static_cast<size_t>(open[depth])].name == name) {
            m_callFrames[static_cast<size_t>(open[depth])].endLRef = s.endLRef;
        } else {
            open[depth] = static_cast<int>(m_callFrames.size());
            m_callFrames.push_back({s.startLRef, s.endLRef, depth, 0u, name});
            m_callMaxDepth = std::max(m_callMaxDepth, depth);
        }
    };

    for (const CpuInstrSpan& s : m_cpuSpans) {
        const bool inCode = (s.pc >= base && s.pc < codeEnd);

        std::vector<std::string> inl;
        std::string funcName;
        if (inCode && haveInline)
            inl = m_debugInline->ResolveInlineStack(s.pc - base);
        if (!inl.empty())
            funcName = inl[0];
        else if (inCode) {
            if (const core::FunctionSymbol* fn = syms->ResolvePc(s.pc - base))
                funcName = fn->name;
        }

        if (funcName.empty()) {
            callStack.clear();
            std::fill(open.begin(), open.end(), -1);
            continue;
        }

        // Update the real-call stack: pop returned frames (entry SP at/below current SP
        // and not the function we're now in), then push if this is a new activation.
        const uint32_t sp = s.sp;
        while (!callStack.empty() && callStack.back().entrySp <= sp &&
               callStack.back().name != funcName)
            callStack.pop_back();
        if (callStack.empty() || callStack.back().name != funcName)
            callStack.push_back({funcName, sp});

        int depth = 0;
        for (const RealFrame& rf : callStack)
            emit(depth++, rf.name, s);
        for (size_t k = 1; k < inl.size(); ++k) // inl[0] == enclosing func already emitted
            emit(depth++, inl[k], s);

        for (size_t d = static_cast<size_t>(depth); d < open.size(); ++d)
            open[d] = -1;
    }
}

std::vector<adm::core::CallFrame> DebuggerFrameScrubberView::GetCallStackAt(uint32_t lRef)
{
    EnsureBands();
    std::vector<adm::core::CallFrame> stack;
    for (const auto& f : m_callFrames)
        if (lRef >= f.startLRef && lRef < f.endLRef)
            stack.push_back(f);
    std::sort(stack.begin(), stack.end(),
              [](const core::CallFrame& a, const core::CallFrame& b) { return a.depth < b.depth; });
    return stack;
}

void DebuggerFrameScrubberView::DrawIdleRow(const TimelineCtx& c)
{
    if (c.idleRowH <= 0.0f)
        return;

    const float y0 = c.canvasPos.y + c.cpuRowH + c.dmaRowH + 1.0f;
    const float y1 = c.canvasPos.y + c.cpuRowH + c.dmaRowH + c.idleRowH - 1.0f;
    ImDrawList* dl = c.dl;
    const float clipL = c.canvasPos.x;
    const float clipR = c.canvasPos.x + c.canvasWidth;
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // Row background so the busy (non-idle) stretches read as filled work rather than empty canvas.
    dl->AddRectFilled({clipL, y0}, {clipR, y1}, IM_COL32(42, 42, 32, 255));

    const std::vector<IdleSpan> spans = BuildIdleSpans();
    const ImU32 idleCol = IM_COL32(90, 170, 90, 220);    // green = free / idle time
    const ImU32 idleEdge = IM_COL32(150, 220, 150, 255); // bright edge on a closed boundary
    int hovered = -1;
    for (size_t i = 0; i < spans.size(); ++i) {
        const IdleSpan& s = spans[i];
        float x0 = LRefToX(c, s.startLRef);
        float x1 = LRefToX(c, s.endLRef);
        if (x1 < clipL || x0 > clipR)
            continue;
        x0 = std::max(x0, clipL);
        x1 = std::min(x1, clipR);
        if (x1 < x0 + 1.0f)
            x1 = x0 + 1.0f;
        dl->AddRectFilled({x0, y0}, {x1, y1}, idleCol);
        // Mark the real (non-clamped) boundaries; a frame-edge-clamped side is left borderless.
        if (!s.openStart)
            dl->AddLine({x0, y0}, {x0, y1}, idleEdge, 1.0f);
        if (!s.openEnd)
            dl->AddLine({x1, y0}, {x1, y1}, idleEdge, 1.0f);
        if (c.hovered && mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y0 && mouse.y <= y1)
            hovered = static_cast<int>(i);
    }

    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f, {clipL + 2.0f, y0},
                IM_COL32(200, 220, 200, 180), "Idle");

    if (hovered >= 0) {
        const IdleSpan& s = spans[static_cast<size_t>(hovered)];
        const float cck = s.endLRef - s.startLRef;
        const float us = cck / 3.546895f; // CCK → microseconds (PAL color clock)
        const float pct = cck / kTotalSlots * 100.0f;
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Idle region (debug_start_idle / debug_stop_idle)");
        ImGui::TextDisabled("%.0f CCK  |  %.1f \xc2\xb5s  |  %.1f%% of frame", cck, us, pct);
        if (s.openStart || s.openEnd)
            ImGui::TextDisabled("(clamped to frame edge — spans the vblank boundary)");
        ImGui::EndTooltip();
    }
}

void DebuggerFrameScrubberView::DrawFunctionRow(const TimelineCtx& c)
{
    const int shownDepths = FlameShownDepths();
    if (c.funcRowH <= 0.0f || shownDepths <= 0)
        return;

    const auto& frames = DisplayFrames();
    const float rowTop = c.canvasPos.y + c.cpuRowH + c.dmaRowH + c.idleRowH;
    const float perH = kFlameDepthH;
    const float clipL = c.canvasPos.x;
    const float clipR = c.canvasPos.x + c.canvasWidth;
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // When the stack is deeper than the cap, show the DEEPEST rows (where the leaf work
    // is) rather than the outermost. depthOffset is the first visible depth; 0 when the
    // whole stack fits, so the root stays on top in the common case.
    const int depthOffset = std::max(0, (DisplayMaxDepth() + 1) - shownDepths);

    int hovered = -1;
    for (size_t i = 0; i < frames.size(); ++i) {
        const core::CallFrame& f = frames[i];
        const int row = f.depth - depthOffset;
        if (row < 0 || row >= shownDepths)
            continue; // outside the visible depth window
        float x0 = LRefToX(c, static_cast<float>(f.startLRef));
        float x1 = LRefToX(c, static_cast<float>(f.endLRef));
        if (x1 < clipL || x0 > clipR)
            continue;
        x0 = std::max(x0, clipL);
        x1 = std::min(x1, clipR);
        if (x1 < x0 + 1.0f)
            x1 = x0 + 1.0f; // keep sub-pixel frames visible

        const float y0 = rowTop + row * perH + 0.5f;
        const float y1 = rowTop + (row + 1) * perH - 0.5f;
        const ImU32 col = !f.name.empty() ? FunctionBandColor(f.name) : IM_COL32(95, 95, 95, 255);
        c.dl->AddRectFilled({x0, y0}, {x1, y1}, col);

        // Label every segment with the function name, clipped to the segment so a
        // narrow band shows as much of the name as fits without bleeding into its
        // neighbours. (Sub-pixel bands are skipped — nothing legible would show.)
        if (!f.name.empty() && x1 - x0 > 3.0f) {
            const ImVec2 ts = ImGui::CalcTextSize(f.name.c_str());
            const float ty = (y0 + y1) * 0.5f - ts.y * 0.5f;
            c.dl->PushClipRect({x0, y0}, {x1, y1}, true);
            c.dl->AddText({x0 + 2.0f, ty}, IM_COL32(15, 15, 15, 235), f.name.c_str());
            c.dl->PopClipRect();
        }

        if (c.hovered && mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y0 && mouse.y <= y1)
            hovered = static_cast<int>(i);
    }

    // Row label, in the same in-canvas style as the DMA and Idle rows — and, when the stack
    // is deeper than the cap, how many outer depths are off screen. Without this the flame
    // silently drops its outermost rows and reads as a complete stack.
    if (depthOffset > 0) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "Fn  " ICON_FA_TRIANGLE_EXCLAMATION " +%d outer", depthOffset);
        c.dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f, {clipL + 2.0f, rowTop},
                      IM_COL32(255, 191, 64, 220), lbl);
    } else {
        c.dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f, {clipL + 2.0f, rowTop},
                      IM_COL32(200, 200, 200, 180), "Fn");
    }

    if (hovered >= 0) {
        const core::CallFrame& f = frames[static_cast<size_t>(hovered)];
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(f.name.empty() ? "<unresolved>" : f.name.c_str());

        // Precise source line for the instruction directly under the cursor.
        if (m_debugLines && m_debugLines->IsLoaded() && !m_segments.empty() &&
            c.slotToPixel > 0.0f) {
            uint32_t lRef =
                static_cast<uint32_t>(m_scrollL + (mouse.x - c.canvasPos.x) / c.slotToPixel);
            int span = FindCpuSpan(lRef);
            const uint32_t base = m_segments.front().base;
            if (span >= 0 && m_cpuSpans[static_cast<size_t>(span)].pc >= base) {
                if (auto src = m_debugLines->ResolveLine(m_cpuSpans[static_cast<size_t>(span)].pc -
                                                         base)) {
                    size_t slash = src->file.find_last_of("/\\");
                    std::string sname =
                        (slash == std::string::npos) ? src->file : src->file.substr(slash + 1);
                    ImGui::TextDisabled("%s:%u", sname.c_str(), src->line);
                }
            }
        }

        ImGui::TextDisabled("depth %d  |  $%08X  |  %u CCK  |  L%07X-L%07X", f.depth, f.funcAddress,
                            f.endLRef - f.startLRef, f.startLRef, f.endLRef);
        if (depthOffset > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                               "showing depths %d-%d of %d — %d outer frame%s hidden", depthOffset,
                               depthOffset + shownDepths - 1, DisplayMaxDepth() + 1, depthOffset,
                               depthOffset == 1 ? "" : "s");
        ImGui::EndTooltip();
    }
}

uint32_t DebuggerFrameScrubberView::HeatLineValue(int mode, int line) const
{
    if (line < 0 || line >= kMaxScanlines)
        return 0;
    switch (mode) {
        case 0:
            return m_cpuDensity[line];
        case 1:
            return m_stallDensity[line];
        case 2:
            return m_cyclesPerLine[line];
        case 3:
            return m_branchPerLine[line];
        case 4:
            return m_dmaScanlines[line].total;
        case 5: { // CPU headroom = idle (free) slots on the line
            uint32_t total = m_dmaScanlines[line].total;
            return (total < 228u) ? (228u - total) : 0u;
        }
        case 6:
            return m_dmaScanlines[line].counts[6]; // Bitplane
        case 7:
            return m_dmaScanlines[line].counts[3]; // Copper
        case 8:
            return m_dmaScanlines[line].counts[5]; // Blitter
        case 9:
            return m_dmaScanlines[line].counts[7]; // Sprite
        default:
            return 0;
    }
}

int DebuggerFrameScrubberView::FindCpuSpan(uint32_t lRef) const
{
    // Spans are sorted ascending by startLRef. upper_bound finds the first span starting
    // strictly after lRef; the candidate owner is the one before it.
    auto it = std::upper_bound(m_cpuSpans.begin(), m_cpuSpans.end(), lRef,
                               [](uint32_t v, const CpuInstrSpan& s) { return v < s.startLRef; });
    if (it == m_cpuSpans.begin())
        return -1;
    --it;
    if (lRef >= it->startLRef && lRef < it->endLRef)
        return static_cast<int>(it - m_cpuSpans.begin());
    return -1;
}

void DebuggerFrameScrubberView::DrawCpuSlotTooltip(int spanIdx)
{
    if (spanIdx < 0 || spanIdx >= static_cast<int>(m_cpuSpans.size()))
        return;
    const CpuInstrSpan& s = m_cpuSpans[static_cast<size_t>(spanIdx)];
    constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;

    const uint32_t measuredCCK = (s.endLRef > s.startLRef) ? (s.endLRef - s.startLRef) : 0;
    const int measuredCpu = static_cast<int>(measuredCCK) * 2; // 1 CCK = 2 CPU cycles

    CpuInstrInfo info = m_queryInstrInfo ? m_queryInstrInfo(s.index) : CpuInstrInfo{};

    // Tally which DMA channels owned the bus during the instruction's slot-run.
    int counts[10] = {};
    int busTotal = 0;
    if (m_dmaRecords && m_dmaHposCount > 0) {
        for (uint32_t l = s.startLRef; l < s.endLRef; ++l) {
            int line = static_cast<int>(l / kCCKPerLine);
            int h = static_cast<int>(l % kCCKPerLine);
            if (line < 0 || line >= m_dmaVposCount || h >= m_dmaHposCount)
                continue;
            int t =
                std::clamp(static_cast<int>(m_dmaRecords[line * m_dmaHposCount + h].type), 0, 9);
            counts[t]++;
            if (t > 0)
                busTotal++;
        }
    }

    ImGui::BeginTooltip();

    // Header: index, frame ref, PC.
    ImGui::Text("#%d", s.index);
    ImGui::SameLine();
    ImGui::TextDisabled("L%07X", s.startLRef);
    if (info.valid && info.pc) {
        ImGui::SameLine();
        ImGui::TextDisabled("$%08X", info.pc);
    }

    // Instruction text.
    if (info.valid && !info.text.empty())
        ImGui::TextUnformatted(info.text.c_str());
    else
        ImGui::TextDisabled("(instruction unavailable — enable CPU Trace)");

    ImGui::TextDisabled("Line %u, Clock %u", static_cast<unsigned>(s.startLRef / kCCKPerLine),
                        static_cast<unsigned>(s.startLRef % kCCKPerLine));

    ImGui::Separator();

    // Cycle accounting. Measured CPU cycles = CCK*2; compare against the textbook 68k cost.
    if (info.valid && info.nominalCycles >= 0)
        ImGui::Text("Expected (68k): %d CPU cyc", info.nominalCycles);
    else
        ImGui::TextDisabled("Expected (68k): -");
    ImGui::Text("Measured: %u CCK  =  %d CPU cyc", measuredCCK, measuredCpu);

    if (info.valid && info.nominalCycles >= 0 && measuredCCK > 0) {
        int delta = measuredCpu - info.nominalCycles; // +ve = stalled beyond textbook
        ImVec4 col = (delta <= 0)   ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)  // at/under budget
                     : (delta <= 2) ? ImVec4(0.95f, 0.85f, 0.40f, 1.0f)  // minor
                                    : ImVec4(0.95f, 0.45f, 0.35f, 1.0f); // stalled
        if (delta <= 0) {
            ImGui::TextColored(col, "Delta: %d CPU cyc  (on budget)", delta);
        } else {
            ImGui::TextColored(col, "Delta: +%d CPU cyc  (bus stall)", delta);
            // (#1) Likely culprit: the non-CPU channel that grabbed the most slots here.
            int culprit = 0, culpritCount = 0;
            for (int t = 1; t < 10; ++t) {
                if (t == 2) // skip CPU itself
                    continue;
                if (counts[t] > culpritCount) {
                    culpritCount = counts[t];
                    culprit = t;
                }
            }
            if (culprit > 0) {
                ImVec4 cc = ImGui::ColorConvertU32ToFloat4(DmaTypeColor(culprit));
                ImGui::TextDisabled("Likely culprit:");
                ImGui::SameLine();
                ImGui::ColorButton("##cu", cc,
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                                   ImVec2(11, 11));
                ImGui::SameLine();
                ImGui::TextColored(cc, "%s (%d slots)", DmaTypeLabel(culprit), culpritCount);
            }
        }
    }

    ImGui::Separator();

    // (#2) Window occupancy: how the measured CCK split between CPU bus accesses, slots stolen
    // by other channels, and idle (no-DMA) cycles the CPU spent waiting.
    {
        int cpuSlots = counts[2];
        int stolen = busTotal - cpuSlots;
        int idle = std::max(0, static_cast<int>(measuredCCK) - busTotal);
        ImGui::TextDisabled(
            "Window %u CCK  \xc2\xb7  CPU %d  \xc2\xb7  stolen %d  \xc2\xb7  idle %d", measuredCCK,
            cpuSlots, stolen, idle);
    }

    // Per-channel key of the bus slots consumed across the window.
    ImGui::TextDisabled("Bus slots by channel:");
    if (busTotal == 0) {
        ImGui::TextDisabled("  (none recorded)");
    } else {
        for (int t = 1; t < 10; ++t) {
            if (counts[t] == 0)
                continue;
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(DmaTypeColor(t));
            ImGui::ColorButton("##k", c,
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                               ImVec2(11, 11));
            ImGui::SameLine();
            ImGui::Text("%s \xc3\x97 %d", DmaTypeLabel(t), counts[t]);
        }
    }

    ImGui::EndTooltip();
}

void DebuggerFrameScrubberView::SetDmaRecordData(const adm::AmigaEmulator::DmaSlotRecord* records,
                                                 int hposCount, int vposCount)
{
    std::memset(m_dmaScanlines, 0, sizeof(m_dmaScanlines));
    m_hasDmaData = false;
    m_dmaRecords = nullptr;
    m_dmaHposCount = 0;
    m_dmaVposCount = 0;

    if (!records || hposCount <= 0 || vposCount <= 0)
        return;

    m_dmaRecords = records;
    m_dmaHposCount = hposCount;
    m_dmaVposCount = vposCount;

    int lines = std::min(vposCount, kMaxDmaScanlines);
    for (int line = 0; line < lines; ++line) {
        auto& summary = m_dmaScanlines[line];
        for (int h = 0; h < hposCount; ++h) {
            const auto& rec = records[line * hposCount + h];
            int t = std::clamp(static_cast<int>(rec.type), 0, 9);
            summary.counts[t]++;
            if (t > 0)
                summary.total++;
        }
    }
    m_hasDmaData = true;
    m_dmaSeen = true;
}

void DebuggerFrameScrubberView::LoadCopperRegsDef()
{
    if (m_copperRegsDefLoaded)
        return;
    m_copperRegsDefLoaded = true;
    try {
        std::ifstream f("assets/custom_registers.json");
        if (f.is_open())
            m_copperRegsDef = nlohmann::json::parse(f);
        else
            m_copperRegsDef = nlohmann::json::object();
    } catch (...) {
        m_copperRegsDef = nlohmann::json::object();
    }
}

bool DebuggerFrameScrubberView::DrawCopperEventTooltip(int index, uint32_t lRef)
{
    if (index < 0 || index >= static_cast<int>(m_copperEvents.size()))
        return false;
    DrawCopperTickTooltip(m_copperEvents[index], lRef);
    return true;
}

void DebuggerFrameScrubberView::DrawCopperTickTooltip(const adm::AmigaEmulator::CopperExecEvent& ev,
                                                      uint32_t lRef)
{
    LoadCopperRegsDef();
    adm::core::CopperInstruction inst =
        adm::core::CopperDisassembler::Disassemble(ev.word1, ev.word2, m_copperRegsDef);

    ImGui::BeginTooltip();
    const char* listStr = (ev.list == 1) ? "COP1" : (ev.list == 2) ? "COP2" : "Copper";
    ImGui::Text("%s  \xc2\xb7  L%07X", listStr, lRef);
    ImGui::Text("Line %u, Clock %u", static_cast<unsigned>(ev.vpos),
                static_cast<unsigned>(ev.hpos));
    ImGui::Text("Address: $%08X", ev.addr);
    ImGui::Separator();
    ImGui::Text("%s %s", inst.name.c_str(), inst.formattedWords.c_str());
    if (!inst.details.empty())
        ImGui::TextDisabled("%s", inst.details.c_str());
    if (inst.isColor) {
        ImGui::SameLine();
        ImGui::ColorButton("##copcol", ImVec4(inst.r, inst.g, inst.b, 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                           ImVec2(14, 14));
    }
    ImGui::Separator();
    ImGui::TextDisabled("Click to open in Copper trace");
    ImGui::EndTooltip();
}

void DebuggerFrameScrubberView::EmitActiveBlit(uint32_t lRef)
{
    if (lRef == m_lastActiveBlitFired)
        return;
    m_lastActiveBlitFired = lRef;
    if (m_onActiveBlitChanged)
        m_onActiveBlitChanged(lRef);
}

void DebuggerFrameScrubberView::EmitCpuSlotHover(uint32_t lRef)
{
    if (lRef == m_lastCpuSlotHovered)
        return;
    m_lastCpuSlotHovered = lRef;
    if (m_onCpuSlotHovered)
        m_onCpuSlotHovered(lRef);
}

void DebuggerFrameScrubberView::Draw()
{
    // A drag that ended while the row was not on screen has to be settled wherever the
    // window went. m_histSeeking is what stops the bar following the core's position, so a
    // latch left set freezes the playhead for the rest of the session.
    FlushPendingSeek();

    if (!m_isOpen) {
        EmitActiveBlit(UINT32_MAX); // window closed → no overlay target
        EmitCpuSlotHover(UINT32_MAX);
        return;
    }

    // Ages out blit-destination thumbnails; must run before any tooltip draws one.
    m_blitDestPreview.BeginFrame();

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }

    ImGui::SetNextWindowSize(ImVec2(900, 90), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_FILM " Frame Scrubber###Frame Scrubber##debugger", &m_isOpen,
                      flags)) {
        EmitActiveBlit(UINT32_MAX); // collapsed → no overlay target
        EmitCpuSlotHover(UINT32_MAX);
        ImGui::End();
        return;
    }

    // The record button lives in the seek bar and must stay live regardless of turbo — it's
    // the user's own recording control, not trace data. Only the timeline itself (blit/copper/
    // CPU-trace bands, all harvested data) is meaningless while turbo is actually running.
    DrawFrameSeekBar();
    if (m_turboSuppressed) {
        DrawTurboSuppressedPlaceholder();
    } else {
        EnsureCpuTraceProcessed();
        DrawTimeline();
    }

    ImGui::End();

    // Pinned blit-detail window is a separate top-level window (drawn outside the scrubber).
    DrawPinnedBlitWindow();
}

// Settle a seek whose drag ended somewhere the bar could not see it. The seek fires on
// release, and the release is only noticed while the row is being drawn - so a window
// closed, collapsed or squeezed to nothing mid-drag would leave m_histSeeking latched, and
// with it the playhead frozen at wherever the drag had got to.
void DebuggerFrameScrubberView::FlushPendingSeek()
{
    if (!m_histSeeking || ImGui::IsMouseDown(ImGuiMouseButton_Left))
        return;
    m_histSeeking = false;
    m_barDrag = BarDrag::None;
    if (m_onSeekFrame)
        m_onSeekFrame(m_histPosition);
}

// ENGINE9000 puts a seek bar under the emulator window; here it sits directly above the
// in-frame timeline, which reads better because the two are the same gesture at different
// scales - this one picks the frame, the one below picks the position within it.
// Arm or disarm the recorder. Recording is off until it is asked for, so this is the one
// control on the row that stays live when everything beside it is dead - and the only place
// outside the Emulator menu that turns the whole mechanism on.
void DebuggerFrameScrubberView::DrawRecordButton()
{
    const ImVec2 btn(ImGui::GetFrameHeight() * 1.1f, 0.0f);
    // Lit while frames are actually going in; a duller red for armed-but-not-recording,
    // which is what waiting for the emulator to run looks like.
    if (m_histEnabled) {
        ImGui::PushStyleColor(ImGuiCol_Button, m_histRecording ? IM_COL32(170, 45, 45, 255)
                                                               : IM_COL32(105, 55, 55, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(205, 65, 65, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(230, 80, 80, 255));
    }
    ImGui::BeginDisabled(!m_onSetRecording);
    if (ImGui::Button(ICON_FA_CIRCLE_DOT "##FrameRecord", btn) && m_onSetRecording)
        m_onSetRecording(!m_histEnabled);
    ImGui::EndDisabled();
    if (m_histEnabled)
        ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!m_histEnabled)
            ImGui::SetTooltip("Record frames\nKeeps a save state per frame so you can step "
                              "back, replay and loop.\nOff until you ask for it - it costs "
                              "memory and time every frame.");
        else if (m_histRecording)
            ImGui::SetTooltip("Recording - click to stop\nStopping throws the recorded "
                              "frames away.");
        else
            ImGui::SetTooltip("Armed - recording starts once the emulator is running\nClick "
                              "to switch it off");
    }
}

void DebuggerFrameScrubberView::DrawFrameSeekBar()
{
    // The record arm first, and outside the disabled block below: with recording off by
    // default the row is dead until someone presses this, so hiding the row - or greying
    // this with it - would leave nothing to press.
    DrawRecordButton();
    ImGui::SameLine();
    if (!m_histEnabled)
        FlushPendingSeek();

    // The oldest recorded frame is only ever a base to replay from, never a destination,
    // so the seekable span starts one after it. Until there are two of them the whole row
    // is drawn disabled - the shape of the thing you are waiting for, in place, rather than
    // a sentence where it will appear.
    const bool haveRange = m_histEnabled && m_histFirst != 0 && m_histNewest >= m_histFirst;
    ImGui::BeginDisabled(!haveRange);

    ImGui::AlignTextToFramePadding();
    // Playback that stopped itself says so here. It used to just stop, which is the single
    // thing that makes a transport feel haunted rather than merely broken.
    if (!m_timelineStopReason.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SetItemTooltip("Playback stopped: %s", m_timelineStopReason.c_str());
    } else {
        ImGui::TextUnformatted(ICON_FA_ROTATE_LEFT);
    }
    ImGui::SameLine();

    const uint64_t first = haveRange ? m_histFirst : 0;
    uint64_t value = m_histPosition < first ? first : m_histPosition;
    if (!haveRange)
        value = 0;

    // Frame number as its own label rather than the bar's overlay text: a half-height bar
    // has no room to print inside it. Given a fixed slot so the buttons beside it do not
    // shift every time the count gains a digit - they are click targets, not decoration.
    const float numX = ImGui::GetCursorPosX();
    if (haveRange)
        ImGui::Text("%llu", static_cast<unsigned long long>(value));
    else
        ImGui::TextUnformatted("-");
    ImGui::SameLine(numX + ImGui::CalcTextSize("00000000").x + ImGui::GetStyle().ItemSpacing.x);

    DrawFrameLoopControls(first);
    ImGui::SameLine();

    // Half-height track. Hand-drawn rather than a SliderInt because the bar also has to
    // carry the loop range and its two marks, which a slider has no way to show.
    const float rowH = ImGui::GetFrameHeight();
    const float trackH = std::max(6.0f, rowH * 0.5f);
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail <= 1.0f) {
        FlushPendingSeek();
        ImGui::NewLine();
        ImGui::EndDisabled();
        ImGui::Separator();
        return;
    }
    // Centred against the buttons beside it, so the row still reads as one line.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - trackH) * 0.5f);
    ImGui::InvisibleButton("##FrameSeek", ImVec2(avail, trackH));

    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    const float span = hi.x - lo.x;
    const double frameSpan = static_cast<double>(m_histNewest > first ? m_histNewest - first : 1);

    const auto frameToX = [&](uint64_t f) {
        const double off = static_cast<double>(f) - static_cast<double>(first);
        const double t = std::clamp(off, 0.0, frameSpan) / frameSpan;
        return lo.x + static_cast<float>(t) * span;
    };
    const auto xToFrame = [&](float x) {
        const float t = span > 1.0f ? std::clamp((x - lo.x) / span, 0.0f, 1.0f) : 0.0f;
        return first + static_cast<uint64_t>(static_cast<double>(t) * frameSpan + 0.5);
    };

    // A press has to decide up front what it is moving: a loop gripper if it landed on one,
    // the playhead otherwise. The whole bar is one item, so the mode is latched on the way
    // down and held until the button comes up.
    const float grabPx = std::max(7.0f, trackH);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto nearMark = [&](uint64_t mark, float x) {
        return mark != 0 && std::fabs(x - frameToX(mark)) <= grabPx * 0.5f + 1.0f;
    };

    if (ImGui::IsItemActivated()) {
        // B first: with both marks on the same frame, the end you mean to pull away is the
        // one you just put down.
        if (nearMark(m_loopB, mouse.x))
            m_barDrag = BarDrag::LoopB;
        else if (nearMark(m_loopA, mouse.x))
            m_barDrag = BarDrag::LoopA;
        else
            m_barDrag = BarDrag::Seek;
    }

    const bool active = ImGui::IsItemActive();
    const bool draggingMark =
        active && (m_barDrag == BarDrag::LoopA || m_barDrag == BarDrag::LoopB);
    if (draggingMark) {
        // Marks keep their order while dragging, so the band can never turn inside out.
        const uint64_t at = xToFrame(mouse.x);
        if (m_barDrag == BarDrag::LoopA)
            m_loopA = m_loopB ? std::min(at, m_loopB) : at;
        else
            m_loopB = m_loopA ? std::max(at, m_loopA) : at;
    } else if (active) {
        // Seek on release, not per pixel of travel: each seek restores a state and re-runs
        // a frame, which is far too much work to do on every mouse move.
        m_histSeeking = true;
        value = xToFrame(mouse.x);
        m_histPosition = value;
    } else if (m_histSeeking) {
        m_histSeeking = false;
        m_histPosition = value;
        if (m_onSeekFrame)
            m_onSeekFrame(m_histPosition);
    }

    if (!active && m_barDrag != BarDrag::None) {
        // Moving a gripper on a running loop re-arms it over the new range, which restarts
        // the replay at A.
        const bool movedMark = m_barDrag == BarDrag::LoopA || m_barDrag == BarDrag::LoopB;
        if (movedMark && m_loopActive && m_onSetFrameLoop && m_loopA && m_loopB)
            m_onSetFrameLoop(std::min(m_loopA, m_loopB), std::max(m_loopA, m_loopB));
        m_barDrag = BarDrag::None;
    }

    const bool hovered = ImGui::IsItemHovered();
    const bool idle = m_barDrag == BarDrag::None;
    const bool hotA =
        m_barDrag == BarDrag::LoopA || (idle && hovered && nearMark(m_loopA, mouse.x));
    const bool hotB =
        m_barDrag == BarDrag::LoopB || (idle && hovered && nearMark(m_loopB, mouse.x));
    if (hotA || hotB)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = trackH * 0.5f;
    dl->AddRectFilled(lo, hi, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    dl->AddRectFilled(lo, ImVec2(frameToX(value), hi.y), ImGui::GetColorU32(ImGuiCol_SliderGrab),
                      rounding);

    // Loop range: a shaded band between the two marks. A half-set range still draws its one
    // gripper, so marking a loop out shows progress before it can be armed.
    const ImU32 loopCol = m_loopActive ? IM_COL32(90, 220, 120, 90) : IM_COL32(220, 190, 80, 70);
    const ImU32 markCol = m_loopActive ? IM_COL32(120, 255, 150, 255) : IM_COL32(255, 215, 90, 255);
    const ImU32 hotCol = IM_COL32(255, 255, 255, 255);
    if (m_loopA && m_loopB) {
        const float ax = frameToX(std::min(m_loopA, m_loopB));
        const float bx = frameToX(std::max(m_loopA, m_loopB));
        dl->AddRectFilled(ImVec2(ax, lo.y), ImVec2(std::max(bx, ax + 1.0f), hi.y), loopCol,
                          rounding);
    }

    // Playhead under the grippers: a mark sitting on the playhead is the one you want to
    // grab, so it has to be the thing you can see.
    const float px = frameToX(value);
    dl->AddRectFilled(ImVec2(px - 1.5f, lo.y - 2.0f), ImVec2(px + 1.5f, hi.y + 2.0f),
                      ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
    dl->AddRect(lo, hi, ImGui::GetColorU32(ImGuiCol_Border), rounding);

    // Grippers: wide enough to grab, and drawn proud of the track top and bottom so they
    // read as handles rather than as marks painted on it.
    const float gw = grabPx;
    const float gy = 3.0f;
    const std::pair<uint64_t, bool> marks[2] = {{m_loopA, hotA}, {m_loopB, hotB}};
    for (const auto& m : marks) {
        if (!m.first)
            continue;
        const float mx = frameToX(m.first);
        const ImVec2 g0(mx - gw * 0.5f, lo.y - gy);
        const ImVec2 g1(mx + gw * 0.5f, hi.y + gy);
        dl->AddRectFilled(g0, g1, m.second ? hotCol : markCol, 2.0f);
        dl->AddRect(g0, g1, IM_COL32(20, 20, 20, 200), 2.0f);
        // Grip line down the middle, the usual "this one moves" cue.
        dl->AddLine(ImVec2(mx, g0.y + 3.0f), ImVec2(mx, g1.y - 3.0f), IM_COL32(20, 20, 20, 160));
    }

    if (active || hovered) {
        // While dragging, the thing being moved is what to preview. While merely hovering,
        // map the cursor onto the bar so the frame under the pointer previews before
        // committing to a seek.
        uint64_t previewFrame = value;
        const char* what = nullptr;
        if (m_barDrag == BarDrag::LoopA && active) {
            previewFrame = m_loopA;
            what = "Loop start (A)";
        } else if (m_barDrag == BarDrag::LoopB && active) {
            previewFrame = m_loopB;
            what = "Loop end (B)";
        } else if (!active) {
            previewFrame = xToFrame(mouse.x);
            if (hotA)
                what = "Loop start (A) - drag to move";
            else if (hotB)
                what = "Loop end (B) - drag to move";
            if (hotA)
                previewFrame = m_loopA;
            else if (hotB)
                previewFrame = m_loopB;
        }

        ImGui::BeginTooltip();
        const bool shown = m_preview.Draw(previewFrame, m_onGetFrameThumbnail, 256.0f);
        if (what)
            ImGui::TextUnformatted(what);
        ImGui::Text(
            "Frame %llu   (recorded %llu-%llu)", static_cast<unsigned long long>(previewFrame),
            static_cast<unsigned long long>(first), static_cast<unsigned long long>(m_histNewest));
        if (!shown) {
            ImGui::TextDisabled("No preview - turn on Emulator > Frame History > "
                                "Scrub previews");
        }
        ImGui::EndTooltip();
    }

    // Say what the empty bar is waiting for, since the disabled controls cannot.
    if (!haveRange && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!m_histEnabled)
            ImGui::SetTooltip("Not recording - press the record button to start keeping "
                              "frames");
        else if (m_histRecording)
            ImGui::SetTooltip("Recording - nothing to seek to yet");
        else
            ImGui::SetTooltip("Recording arms once the emulator is running and a debugger "
                              "window is open");
    }

    ImGui::EndDisabled();
    ImGui::Separator();
}

// A-B loop: mark two recorded frames and replay that range on repeat, which is how you get
// to watch a one-frame glitch as many times as it takes. The marks live in the view; arming
// hands the range to the core, which pauses live running and drives the replay.
void DebuggerFrameScrubberView::DrawFrameLoopControls(uint64_t first)
{
    const uint64_t position = m_histPosition < first ? first : m_histPosition;
    const ImVec2 btn(ImGui::GetFrameHeight() * 1.1f, 0.0f);

    // Moving a mark while the loop runs would change the range under the replay, so the
    // marks are frozen until it is disarmed.
    ImGui::BeginDisabled(m_loopActive);
    if (ImGui::Button(m_loopA ? "A*" : "A", btn)) {
        m_loopA = position;
        if (m_loopB && m_loopB < m_loopA)
            std::swap(m_loopA, m_loopB);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        if (m_loopA)
            ImGui::SetTooltip("Loop start: frame %llu\nClick to move it to frame %llu",
                              static_cast<unsigned long long>(m_loopA),
                              static_cast<unsigned long long>(position));
        else
            ImGui::SetTooltip("Set the loop start to frame %llu",
                              static_cast<unsigned long long>(position));
    }

    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(m_loopActive);
    if (ImGui::Button(m_loopB ? "B*" : "B", btn)) {
        m_loopB = position;
        if (m_loopA && m_loopB < m_loopA)
            std::swap(m_loopA, m_loopB);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        if (m_loopB)
            ImGui::SetTooltip("Loop end: frame %llu\nClick to move it to frame %llu",
                              static_cast<unsigned long long>(m_loopB),
                              static_cast<unsigned long long>(position));
        else
            ImGui::SetTooltip("Set the loop end to frame %llu",
                              static_cast<unsigned long long>(position));
    }

    // A one-frame range is legal - the same frame replayed over and over - so the only
    // requirement is that both ends are marked and still inside the recorded window. Marks
    // outlive the frames they point at once eviction catches up with them, and the core
    // would refuse that range anyway; better to say so on the button than in the log.
    const bool marked = m_loopA != 0 && m_loopB != 0;
    const bool inRange =
        marked && std::min(m_loopA, m_loopB) >= first && std::max(m_loopA, m_loopB) <= m_histNewest;
    const bool armable = marked && inRange;
    ImGui::SameLine(0.0f, 2.0f);
    if (m_loopActive)
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 140, 70, 255));
    ImGui::BeginDisabled(!armable || !m_onSetFrameLoop);
    if (ImGui::Button(ICON_FA_REPEAT, btn)) {
        if (m_loopActive)
            m_onSetFrameLoop(0, 0);
        else
            m_onSetFrameLoop(std::min(m_loopA, m_loopB), std::max(m_loopA, m_loopB));
    }
    ImGui::EndDisabled();
    if (m_loopActive)
        ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        if (!marked)
            ImGui::SetTooltip("Mark A and B first, then loop that range");
        else if (!inRange)
            ImGui::SetTooltip("Frames %llu-%llu are no longer recorded (the timeline now "
                              "holds %llu-%llu) - mark the range again",
                              static_cast<unsigned long long>(std::min(m_loopA, m_loopB)),
                              static_cast<unsigned long long>(std::max(m_loopA, m_loopB)),
                              static_cast<unsigned long long>(first),
                              static_cast<unsigned long long>(m_histNewest));
        else if (m_loopActive)
            ImGui::SetTooltip("Looping frames %llu-%llu - click to stop",
                              static_cast<unsigned long long>(std::min(m_loopA, m_loopB)),
                              static_cast<unsigned long long>(std::max(m_loopA, m_loopB)));
        else
            ImGui::SetTooltip("Replay frames %llu-%llu on repeat.\nThe machine stays paused "
                              "while the range runs.",
                              static_cast<unsigned long long>(std::min(m_loopA, m_loopB)),
                              static_cast<unsigned long long>(std::max(m_loopA, m_loopB)));
    }

    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(!m_loopA && !m_loopB);
    if (ImGui::Button(ICON_FA_XMARK, btn)) {
        if (m_loopActive && m_onSetFrameLoop)
            m_onSetFrameLoop(0, 0);
        m_loopA = 0;
        m_loopB = 0;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Clear the loop marks");

    // Transport. These drive the recorded timeline, never the live machine: play replays
    // recorded frames, and an armed A-B only decides where that playback wraps.
    ImGui::SameLine(0.0f, 6.0f);
    {
        // Hand-drawn divider: SeparatorEx's vertical form lives in imgui_internal.h, and one
        // line is not worth pulling that in.
        const ImVec2 sep = ImGui::GetCursorScreenPos();
        const float sepH = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(sep.x, sep.y + 2.0f),
                                            ImVec2(sep.x, sep.y + sepH - 2.0f),
                                            ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(1.0f, sepH));
    }
    ImGui::SameLine(0.0f, 6.0f);

    if (ImGui::Button(ICON_FA_BACKWARD_STEP "##TimelineStepBack", btn)) {
        if (m_onTimelineStep)
            m_onTimelineStep(-1);
    }
    ImGui::SetItemTooltip("Step back one recorded frame\nStops playback");

    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::Button(m_timelinePlaying ? ICON_FA_PAUSE "##TimelinePlay"
                                        : ICON_FA_PLAY "##TimelinePlay",
                      btn)) {
        if (m_onTimelinePlay)
            m_onTimelinePlay(!m_timelinePlaying);
    }
    if (ImGui::IsItemHovered()) {
        if (m_timelinePlaying)
            ImGui::SetTooltip("Pause playback and hold this frame");
        else if (m_loopActive)
            ImGui::SetTooltip("Replay the A-B range from here");
        else
            ImGui::SetTooltip("Replay recorded frames from here.\nMark A and B to bound it.");
    }

    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::Button(ICON_FA_FORWARD_STEP "##TimelineStepFwd", btn)) {
        if (m_onTimelineStep)
            m_onTimelineStep(1);
    }
    ImGui::SetItemTooltip("Step forward one recorded frame\nStops playback");

    // Direction cycles forward -> reverse -> ping-pong. Ping-pong is the one that finds the
    // frame a defect appears on: it crosses the same boundary in both directions.
    ImGui::SameLine(0.0f, 2.0f);
    const char* dirIcon = m_timelineMode == 1   ? ICON_FA_BACKWARD "##TimelineDir"
                          : m_timelineMode == 2 ? ICON_FA_RIGHT_LEFT "##TimelineDir"
                                                : ICON_FA_FORWARD "##TimelineDir";
    if (ImGui::Button(dirIcon, btn)) {
        if (m_onTimelineMode)
            m_onTimelineMode((m_timelineMode + 1) % 3);
    }
    if (ImGui::IsItemHovered()) {
        if (m_timelineMode == 1)
            ImGui::SetTooltip("Reverse - every frame back costs a state restore, so this is "
                              "the expensive direction\nClick for ping-pong");
        else if (m_timelineMode == 2)
            ImGui::SetTooltip("Ping-pong - plays the range forwards, then backwards\nClick "
                              "for forward");
        else
            ImGui::SetTooltip("Forward\nClick for reverse");
    }

    // Fixed-width label so cycling the speed cannot shift the bar beside it.
    ImGui::SameLine(0.0f, 2.0f);
    const ImVec2 speedBtn(ImGui::CalcTextSize("1/8x").x + ImGui::GetStyle().FramePadding.x * 2.0f,
                          0.0f);
    if (ImGui::Button(SpeedLabel(m_timelineSpeed), speedBtn)) {
        if (m_onTimelineSpeed)
            m_onTimelineSpeed(NextSpeed(m_timelineSpeed));
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && m_onTimelineSpeed)
        m_onTimelineSpeed(1.0);
    ImGui::SetItemTooltip("Playback speed, as a share of the machine's own frame rate\n"
                          "Click to cycle, right-click for 1x");

    // Clear the recording itself, as opposed to the X above which only drops the A-B marks.
    ImGui::SameLine(0.0f, 6.0f);
    if (ImGui::Button(ICON_FA_TRASH "##ClearTimeline", btn)) {
        if (m_onClearTimeline)
            m_onClearTimeline();
        m_loopA = 0;
        m_loopB = 0;
    }
    ImGui::SetItemTooltip("Clear the recorded timeline\nRecording carries on from the next "
                          "frame");
}

// The speed ladder. 3/4x is left out deliberately - it is not distinguishable from 1x by
// eye, and the rungs that matter are the slow ones.
static constexpr double kTimelineSpeeds[] = {0.125, 0.25, 0.5, 1.0, 2.0};

const char* DebuggerFrameScrubberView::SpeedLabel(double speed)
{
    if (speed < 0.2)
        return "1/8x";
    if (speed < 0.375)
        return "1/4x";
    if (speed < 0.75)
        return "1/2x";
    if (speed < 1.5)
        return "1x";
    return "2x";
}

double DebuggerFrameScrubberView::NextSpeed(double speed)
{
    for (size_t i = 0; i < IM_ARRAYSIZE(kTimelineSpeeds); ++i) {
        if (speed < kTimelineSpeeds[i] * 1.001)
            return kTimelineSpeeds[(i + 1) % IM_ARRAYSIZE(kTimelineSpeeds)];
    }
    return kTimelineSpeeds[0];
}

void DebuggerFrameScrubberView::Clear()
{
    m_blitRecords.clear();
    m_copperEvents.clear();
    m_idleEvents.clear();
    m_idleRowActive = false;
    m_cpuTraceSeen = false;
    m_dmaSeen = false;
    std::fill(std::begin(m_cpuDensity), std::end(m_cpuDensity), 0u);
    std::fill(std::begin(m_stallDensity), std::end(m_stallDensity), 0u);
    std::fill(std::begin(m_cyclesPerLine), std::end(m_cyclesPerLine), 0u);
    std::fill(std::begin(m_branchPerLine), std::end(m_branchPerLine), 0u);
    m_hasCpuTrace = false;
    m_cpuTraceEntries = nullptr;
    m_cpuTraceCount = 0;
    m_cpuTraceDirty = false;
    m_cpuSpans.clear();
    m_flatFrames.clear();
    m_callFrames.clear();
    m_callMaxDepth = 0;
    m_bandsDirty = false;
    std::memset(m_dmaScanlines, 0, sizeof(m_dmaScanlines));
    m_hasDmaData = false;
    m_dmaRecords = nullptr;
    m_dmaHposCount = 0;
    m_scrubberL = 0;
    m_pinnedBlitLRef = UINT32_MAX;
}

float DebuggerFrameScrubberView::LRefToX(const TimelineCtx& c, float l) const
{
    return c.canvasPos.x + (l - m_scrollL) * c.slotToPixel;
}

int DebuggerFrameScrubberView::HitTestBlit(const TimelineCtx& c, ImVec2 m) const
{
    // Hit-test a mouse position against the visible blit bars; returns the record index under
    // the cursor or -1.
    if (m.y < c.blitY0 || m.y >= c.blitY1)
        return -1;
    for (int i = 0; i < static_cast<int>(m_blitRecords.size()); ++i) {
        const auto& rec = m_blitRecords[i];
        float startL = static_cast<float>(rec.lRef);
        float endL = startL + static_cast<float>(rec.dmaCycles);
        float visStart = std::max(startL, m_scrollL);
        float visEnd = std::min(endL, m_scrollL + c.visibleRange);
        if (visEnd <= visStart)
            continue;
        float x0 = LRefToX(c, visStart);
        float x1 = LRefToX(c, visEnd);
        if (x1 - x0 < 1.0f)
            x1 = x0 + 1.0f;
        if (m.x >= x0 && m.x < x1)
            return i;
    }
    return -1;
}

int DebuggerFrameScrubberView::HitTestCopper(const TimelineCtx& c, ImVec2 m) const
{
    // Hit-test a mouse position against the visible copper ticks; returns the nearest event
    // index within a pixel tolerance (ticks are 1px lines, so an exact hit is unrealistic), or
    // -1 if none is close enough.
    if (m.y < c.copperY0 || m.y >= c.copperY1)
        return -1;
    constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;
    int best = -1;
    float bestDist = 4.0f; // px tolerance around the tick line
    for (int i = 0; i < static_cast<int>(m_copperEvents.size()); ++i) {
        const auto& ev = m_copperEvents[i];
        float lRef = static_cast<float>(ev.vpos) * kCCKPerLine + static_cast<float>(ev.hpos);
        if (lRef < m_scrollL || lRef > m_scrollL + c.visibleRange)
            continue;
        float x = LRefToX(c, lRef);
        float d = std::abs(m.x - x);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

void DebuggerFrameScrubberView::HandleTimelineInput(TimelineCtx& c)
{
    constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;

    // Cap the zoom where a single color clock reaches kMaxSlotPx wide — past that the CCK is
    // already big enough to show its 1-CPU-cycle subdivision, and zooming further (toward one
    // color clock filling the whole canvas) just makes the timeline harder to read. The cap is
    // width-dependent: slotToPixel = canvasWidth * zoom / kTotalSlots.
    static constexpr float kMaxSlotPx = 60.0f;
    float maxZoom = std::max(1.0f, kMaxSlotPx * kTotalSlots / c.canvasWidth);
    m_zoom = std::clamp(m_zoom, 1.0f, maxZoom);

    c.visibleRange = kTotalSlots / m_zoom;
    c.slotToPixel = c.canvasWidth / c.visibleRange;

    float scrubPixelX = LRefToX(c, m_scrubberL);
    float clampedScrubPixelX =
        std::clamp(scrubPixelX, c.canvasPos.x, c.canvasPos.x + c.canvasWidth);

    // Lock drag mode on initial click
    if (c.activated) {
        float mx = ImGui::GetIO().MousePos.x;
        int clickedBlit = HitTestBlit(c, ImGui::GetIO().MousePos);
        int clickedCopper = HitTestCopper(c, ImGui::GetIO().MousePos);
        if (clickedCopper >= 0) {
            // Click on a copper tick hyperlinks to that instruction in the Copper trace.
            const auto& ev = m_copperEvents[clickedCopper];
            uint32_t lRef =
                static_cast<uint32_t>(ev.vpos) * kCCKPerLine + static_cast<uint32_t>(ev.hpos);
            if (m_onJumpToCopper)
                m_onJumpToCopper(lRef);
            m_scrubberDragging = false;
            m_panning = false;
        } else if (clickedBlit >= 0) {
            // Click on a blit bar toggles a persistent (pinned) detail panel for that blit.
            uint32_t lref = m_blitRecords[clickedBlit].lRef;
            if (m_pinnedBlitLRef == lref) {
                m_pinnedBlitLRef = UINT32_MAX; // re-clicking the pinned blit unpins it
            } else {
                m_pinnedBlitLRef = lref;
                // Freeze the panel where the tooltip currently sits (mouse + tooltip offset).
                ImVec2 mp = ImGui::GetIO().MousePos;
                m_pinnedWindowPos = {mp.x + 16.0f, mp.y + 16.0f};
                m_pinnedPosPending = true;
            }
            m_scrubberDragging = false;
            m_panning = false;
        } else if (std::abs(mx - clampedScrubPixelX) < 10.0f) {
            m_scrubberDragging = true;
            m_panning = false;
            m_scrubberDragStartX = mx;

            // If the scrubber is off-screen, snap it to the edge where it was grabbed
            if (scrubPixelX != clampedScrubPixelX) {
                m_scrubberL = m_scrollL + (clampedScrubPixelX - c.canvasPos.x) / c.slotToPixel;
                if (m_onScrubberChanged)
                    m_onScrubberChanged(static_cast<uint32_t>(m_scrubberL));
            }
            m_scrubberDragStartL = m_scrubberL;
        } else {
            m_panning = true;
            m_scrubberDragging = false;
            m_panStartX = mx;
            m_panStartScroll = m_scrollL;
        }
    }
    if (!c.active) {
        m_scrubberDragging = false;
        m_panning = false;
    }

    // Zoom (mouse wheel)
    if (c.hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            float mx = ImGui::GetIO().MousePos.x - c.canvasPos.x;
            float frac = mx / c.canvasWidth;
            float oldRange = kTotalSlots / m_zoom;
            float mouseL = m_scrollL + frac * oldRange;

            m_zoom *= (wheel > 0.0f ? 1.15f : 1.0f / 1.15f);
            m_zoom = std::clamp(m_zoom, 1.0f, maxZoom); // maxZoom caps at kMaxSlotPx per CCK

            float newRange = kTotalSlots / m_zoom;
            m_scrollL = mouseL - frac * newRange;
        }
    }

    // Scrubber drag. In a partial (mid-frame breakpoint) frame, the scrubber can't pass the
    // break: there is no executed data beyond it.
    const float maxScrubL = m_partialFrame ? static_cast<float>(m_breakLRef) : kTotalSlots;
    if (c.active && m_scrubberDragging) {
        float dx = ImGui::GetIO().MousePos.x - m_scrubberDragStartX;
        float dL = dx / c.canvasWidth * c.visibleRange;
        float newL = std::clamp(m_scrubberDragStartL + dL, 0.0f, maxScrubL);
        if (newL != m_scrubberL) {
            m_scrubberL = newL;
            if (m_onScrubberChanged)
                m_onScrubberChanged(static_cast<uint32_t>(m_scrubberL));
        }
        scrubPixelX = LRefToX(c, m_scrubberL);
    }

    // Pan drag
    if (c.active && m_panning && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float dx = ImGui::GetIO().MousePos.x - m_panStartX;
        m_scrollL = m_panStartScroll - dx / c.canvasWidth * c.visibleRange;
    }

    // Right-click on the timeline → context menu to arm a raster (beam) break at that scanline.
    if (m_onBreakAtLine && c.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        float mx = ImGui::GetIO().MousePos.x;
        float lRef = m_scrollL + (mx - c.canvasPos.x) / c.slotToPixel;
        m_ctxMenuLine = static_cast<int>(lRef / static_cast<float>(kCCKPerLine));
        ImGui::OpenPopup("##scrub_ctx");
    }
    if (ImGui::BeginPopup("##scrub_ctx")) {
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "Break at scanline %d", m_ctxMenuLine);
        if (ImGui::MenuItem(lbl) && m_onBreakAtLine)
            m_onBreakAtLine(m_ctxMenuLine);
        ImGui::EndPopup();
    }

    // Clamp scroll
    c.visibleRange = kTotalSlots / m_zoom;
    c.slotToPixel = c.canvasWidth / c.visibleRange;
    m_scrollL = std::clamp(m_scrollL, 0.0f, std::max(0.0f, kTotalSlots - c.visibleRange));
    // Keep the scrubber within the executed window (covers external SetScrubberLRef too).
    if (m_partialFrame && m_scrubberL > maxScrubL)
        m_scrubberL = maxScrubL;
    c.scrubPixelX = LRefToX(c, m_scrubberL);
}

void DebuggerFrameScrubberView::DrawHeatRow(const TimelineCtx& c)
{
    // Top-strip heatmap, selectable via the dropdown. Trace-based modes (0-3) need the CPU
    // trace; DMA-based modes (4-9) need the per-scanline DMA summary. Each mode maps a
    // per-scanline value to an intensity-graded color (see HeatLineValue / heatColor).
    if (!(m_hasCpuTrace || m_hasDmaData))
        return;

    constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;
    auto lToX = [&](float l) { return LRefToX(c, l); };
    ImDrawList* dl = c.dl;

    const int mode = m_topRowMode;
    // Per-mode short label, descriptive unit (for the hover tooltip), and label color.
    struct HeatMeta
    {
        const char* label;
        const char* unit;
        ImU32 labelCol;
    };
    auto meta = [](int m) -> HeatMeta {
        switch (m) {
            case 1:
                return {"STALL", "stall CPU cyc", IM_COL32(255, 150, 90, 200)};
            case 2:
                return {"CYC", "CPU cyc", IM_COL32(210, 130, 230, 200)};
            case 3:
                return {"BRANCH", "taken branches", IM_COL32(90, 220, 200, 200)};
            case 4:
                return {"DMA", "DMA slots", IM_COL32(220, 220, 220, 200)};
            case 5:
                return {"IDLE", "free slots", IM_COL32(110, 220, 110, 200)};
            case 6:
                return {"BPL", "bitplane slots", DmaTypeColor(6)};
            case 7:
                return {"COP", "copper slots", DmaTypeColor(3)};
            case 8:
                return {"BLT", "blitter slots", DmaTypeColor(5)};
            case 9:
                return {"SPR", "sprite slots", DmaTypeColor(7)};
            default:
                return {"CPU", "instructions", IM_COL32(100, 180, 255, 180)};
        }
    };
    // Intensity ramp per mode. Per-channel modes ramp toward that channel's DMA color.
    auto heatColor = [](int m, float t) -> ImU32 {
        auto chan = [&](int type) {
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(DmaTypeColor(type));
            return IM_COL32(static_cast<uint8_t>(c.x * 255 * t),
                            static_cast<uint8_t>(c.y * 255 * t),
                            static_cast<uint8_t>(c.z * 255 * t), 210);
        };
        switch (m) {
            case 1: // stalls: dark → orange → red
                return IM_COL32((uint8_t)(70 + t * 185), (uint8_t)(t * 110), (uint8_t)(t * 20),
                                210);
            case 2: // cycles/line: magenta
                return IM_COL32((uint8_t)(t * 220), (uint8_t)(t * 40), (uint8_t)(t * 200), 205);
            case 3: // branch churn: teal
                return IM_COL32((uint8_t)(t * 40), (uint8_t)(t * 200), (uint8_t)(t * 180), 205);
            case 4: // DMA pressure: neutral grey → white
                return IM_COL32((uint8_t)(t * 230), (uint8_t)(t * 230), (uint8_t)(t * 235), 205);
            case 5: // headroom: green
                return IM_COL32((uint8_t)(t * 40), (uint8_t)(60 + t * 180), (uint8_t)(t * 40), 205);
            case 6:
                return chan(6);
            case 7:
                return chan(3);
            case 8:
                return chan(5);
            case 9:
                return chan(7);
            default: // CPU density: dark blue → cyan → white
                return IM_COL32((uint8_t)(t * 100), (uint8_t)(t * 200), (uint8_t)(80 + t * 175),
                                200);
        }
    };

    // Running max for normalization.
    uint32_t maxVal = 0;
    for (int line = 0; line < kMaxScanlines; ++line)
        maxVal = std::max(maxVal, HeatLineValue(mode, line));

    const float cpuY0 = c.canvasPos.y + 1.0f;
    const float cpuY1 = c.canvasPos.y + c.cpuRowH - 1.0f;
    for (int line = 0; maxVal > 0 && line < kMaxScanlines; ++line) {
        uint32_t val = HeatLineValue(mode, line);
        if (val == 0)
            continue;
        float lRef = static_cast<float>(line) * kCCKPerLine;
        float lRefEnd = lRef + static_cast<float>(kCCKPerLine);
        float visStart = std::max(lRef, m_scrollL);
        float visEnd = std::min(lRefEnd, m_scrollL + c.visibleRange);
        if (visEnd <= visStart)
            continue;
        float x0 = lToX(visStart);
        float x1 = lToX(visEnd);
        if (x1 - x0 < 1.0f)
            x1 = x0 + 1.0f;
        float intensity = static_cast<float>(val) / static_cast<float>(maxVal);
        dl->AddRectFilled({x0, cpuY0}, {x1, cpuY1}, heatColor(mode, intensity));
    }

    HeatMeta hm = meta(mode);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f, {c.canvasPos.x + 2.0f, cpuY0},
                hm.labelCol, hm.label);

    // Click in the top row to snap scrubber to that scanline; tooltip shows the value.
    if (c.hovered && ImGui::GetIO().MousePos.y >= cpuY0 && ImGui::GetIO().MousePos.y <= cpuY1) {
        float mx = ImGui::GetIO().MousePos.x - c.canvasPos.x;
        float lClickRef = m_scrollL + mx / c.canvasWidth * c.visibleRange;
        int sl = static_cast<int>(lClickRef / kCCKPerLine);
        ImGui::SetTooltip("Scanline %d  \xc2\xb7  %u %s", sl, HeatLineValue(mode, sl), hm.unit);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_scrubberL = std::clamp(lClickRef, 0.0f, kTotalSlots);
            if (m_onScrubberChanged)
                m_onScrubberChanged(static_cast<uint32_t>(m_scrubberL));
        }
    }
}

void DebuggerFrameScrubberView::DrawDmaRow(const TimelineCtx& c)
{
    // DMA channel timeline row (below CPU row, above axis labels)
    if (!m_hasDmaData)
        return;

    constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;
    auto lToX = [&](float l) { return LRefToX(c, l); };
    ImDrawList* dl = c.dl;

    const float dmaY0 = c.canvasPos.y + c.cpuRowH + 1.0f;
    const float dmaY1 = c.canvasPos.y + c.cpuRowH + c.dmaRowH - 1.0f;

    // At high zoom, render per-slot; otherwise per-scanline dominant color
    bool perSlot = (c.slotToPixel > 2.0f) && m_dmaRecords && m_dmaHposCount > 0;

    if (perSlot) {
        // Per-slot rendering.
        // The DMA buffer is NR_DMA_REC_HPOS=288 wide, but hardware hpos only goes to ~226
        // (228 CCK per line). Slots at h=228..287 are buffer padding (always None) and must
        // not be drawn — they would appear at lRef positions overlapping the next scanline.
        // Skipping None slots also avoids painting idle-color over the dark background.
        int hLimit = std::min(m_dmaHposCount, static_cast<int>(kCCKPerLine));
        // Per-slot mode means zoomed IN, i.e. almost every scanline is off-screen — bound the
        // outer loop to the visible lines instead of walking all ~73k slots and rejecting
        // them one by one inside the inner loop.
        const int lineFirst =
            std::clamp(static_cast<int>((m_scrollL - 1.0f) / kCCKPerLine), 0, kMaxDmaScanlines);
        const int lineLast =
            std::clamp(static_cast<int>((m_scrollL + c.visibleRange + 1.0f) / kCCKPerLine) + 1, 0,
                       kMaxDmaScanlines);
        for (int line = lineFirst; line < lineLast; ++line) {
            for (int h = 0; h < hLimit; ++h) {
                const auto& rec = m_dmaRecords[line * m_dmaHposCount + h];
                if (rec.type == 0) // kDmaRecordNone — skip idle, let background show
                    continue;
                float lRef = static_cast<float>(line) * kCCKPerLine + static_cast<float>(h);
                if (lRef < m_scrollL - 1.0f || lRef > m_scrollL + c.visibleRange + 1.0f)
                    continue;
                float x0 = lToX(lRef);
                float x1 = x0 + c.slotToPixel;
                if (x1 < c.canvasPos.x || x0 > c.canvasPos.x + c.canvasWidth)
                    continue;
                ImU32 col = DmaTypeColor(static_cast<int>(rec.type));
                dl->AddRectFilled({x0, dmaY0}, {x1, dmaY1}, col);
            }
        }
    } else {
        // Per-scanline: use dominant DMA type
        for (int line = 0; line < kMaxDmaScanlines; ++line) {
            const auto& summary = m_dmaScanlines[line];
            if (summary.total == 0)
                continue;
            float lRef = static_cast<float>(line) * kCCKPerLine;
            float lRefEnd = lRef + static_cast<float>(kCCKPerLine);
            float visStart = std::max(lRef, m_scrollL);
            float visEnd = std::min(lRefEnd, m_scrollL + c.visibleRange);
            if (visEnd <= visStart)
                continue;
            float x0 = lToX(visStart);
            float x1 = lToX(visEnd);
            if (x1 - x0 < 1.0f)
                x1 = x0 + 1.0f;

            // Find dominant type (highest count, skip type 0 = idle)
            int bestType = 0;
            uint16_t bestCount = 0;
            for (int t = 1; t < 10; ++t) {
                if (summary.counts[t] > bestCount) {
                    bestCount = summary.counts[t];
                    bestType = t;
                }
            }
            dl->AddRectFilled({x0, dmaY0}, {x1, dmaY1}, DmaTypeColor(bestType));
        }
    }

    // --- Per-instruction overlay: tie DMA slots back to the issuing CPU instruction ---
    // Each traced instruction owns the half-open slot range [startLRef, endLRef). When the
    // band is wide enough we stamp its "#" index over the run; the instruction under the
    // scrub marker (and under the mouse) gets an outline. Hovering also feeds the disasm
    // window so it can highlight the owning row.
    if (!m_cpuSpans.empty()) {
        const float labelFont = ImGui::GetFontSize() * 0.75f;
        const float lo = m_scrollL;
        const float hi = m_scrollL + c.visibleRange;

        // Which span sits under the scrub marker, and which under the mouse (DMA row only).
        int scrubSpan = FindCpuSpan(static_cast<uint32_t>(m_scrubberL));
        int hoverSpan = -1;
        if (c.hovered) {
            float my = ImGui::GetIO().MousePos.y;
            if (my >= dmaY0 && my <= dmaY1) {
                float mxRel = ImGui::GetIO().MousePos.x - c.canvasPos.x;
                float lRefAtMouse = m_scrollL + mxRel / c.slotToPixel;
                hoverSpan = FindCpuSpan(static_cast<uint32_t>(lRefAtMouse));
            }
        }

        // "#" labels for every visible span wide enough to read (only a handful at the
        // zoom levels where this fires). Binary-search the first span ending after `lo`.
        auto first =
            std::lower_bound(m_cpuSpans.begin(), m_cpuSpans.end(), static_cast<uint32_t>(lo),
                             [](const CpuInstrSpan& s, uint32_t v) { return s.endLRef <= v; });
        for (auto it = first; m_showInstrNumbers && it != m_cpuSpans.end() && it->startLRef < hi;
             ++it) {
            float bx0 = lToX(static_cast<float>(it->startLRef));
            float bx1 = lToX(static_cast<float>(it->endLRef));
            if (bx1 - bx0 >= 22.0f) {
                char buf[16];
                snprintf(buf, sizeof(buf), "#%d", it->index);
                ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(labelFont, FLT_MAX, 0.0f, buf);
                float tx = bx0 + 2.0f;
                float ty = dmaY0 + (dmaY1 - dmaY0 - labelFont) * 0.5f;
                // Dark pill so the number reads over the colored slots.
                dl->AddRectFilled({tx - 1.0f, ty}, {tx + ts.x + 1.0f, ty + labelFont},
                                  IM_COL32(0, 0, 0, 150), 2.0f);
                dl->AddText(ImGui::GetFont(), labelFont, {tx, ty}, IM_COL32(235, 235, 235, 230),
                            buf);
            }
        }

        // Outline the scrub-marker instruction (subtle) and the hovered one (bright).
        auto outline = [&](int spanIdx, ImU32 col, float thick) {
            if (spanIdx < 0)
                return;
            const CpuInstrSpan& s = m_cpuSpans[static_cast<size_t>(spanIdx)];
            float bx0 = lToX(static_cast<float>(s.startLRef));
            float bx1 = lToX(static_cast<float>(s.endLRef));
            bx0 = std::max(bx0, c.canvasPos.x);
            bx1 = std::min(bx1, c.canvasPos.x + c.canvasWidth);
            if (bx1 > bx0)
                dl->AddRect({bx0, dmaY0 - 1.0f}, {bx1, dmaY1 + 1.0f}, col, 2.0f, 0, thick);
        };
        outline(scrubSpan, IM_COL32(255, 230, 80, 150), 1.0f);
        outline(hoverSpan, IM_COL32(255, 255, 255, 230), 1.5f);

        // Feed the disasm window: highlight the hovered instruction's row (dedup'd).
        EmitCpuSlotHover((hoverSpan >= 0) ? m_cpuSpans[static_cast<size_t>(hoverSpan)].startLRef
                                          : UINT32_MAX);

        // Detail tooltip for the hovered instruction's slot-run.
        if (hoverSpan >= 0)
            DrawCpuSlotTooltip(hoverSpan);
    }

    // Label
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f, {c.canvasPos.x + 2.0f, dmaY0},
                IM_COL32(200, 200, 200, 180), "DMA");
}

void DebuggerFrameScrubberView::DrawAxis(const TimelineCtx& c)
{
    // Axis labels and tick marks — auto-resolution based on zoom level
    static constexpr float kCCKToMicroseconds = 1.0f / 3.546895f; // ~0.28194 µs per CCK
    static constexpr float kCCKToCycles = 2.0f;                   // M68K CPU cycles per CCK
    static constexpr float kCCKToRasterlines = 1.0f / 228.0f;     // rasterlines per CCK

    auto lToX = [&](float l) { return LRefToX(c, l); };
    ImDrawList* dl = c.dl;

    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
    float fontSize = ImGui::GetFontSize() * 0.8f;
    {
        // Round x up to the nearest "nice" value using the 1-2-5 sequence
        auto niceStep = [](float x) -> float {
            if (x <= 0.0f)
                return 1.0f;
            float exp = std::floor(std::log10(x));
            float f = x / std::pow(10.0f, exp);
            float nice = (f < 1.5f) ? 1.0f : (f < 3.5f) ? 2.0f : (f < 7.5f) ? 5.0f : 10.0f;
            return nice * std::pow(10.0f, exp);
        };

        // Target: ~8 ticks across the visible range; convert to the display unit, snap, convert
        // back
        float targetCCK = c.visibleRange / 8.0f;
        float tickIntervalCCK;
        switch (m_tickUnit) {
            case 1: // µs — snap in µs space
                tickIntervalCCK = niceStep(targetCCK * kCCKToMicroseconds) / kCCKToMicroseconds;
                break;
            case 2: // CPU cycles — snap in cycle space (minimum 1 cycle)
                tickIntervalCCK = niceStep(std::max(1.0f, targetCCK * kCCKToCycles)) / kCCKToCycles;
                break;
            case 3: // Rasterlines — snap in line space (minimum 1 line)
                tickIntervalCCK =
                    niceStep(std::max(1.0f, targetCCK * kCCKToRasterlines)) / kCCKToRasterlines;
                break;
            default: // % of frame — snap in percent space
                tickIntervalCCK =
                    niceStep((targetCCK / kTotalSlots) * 100.0f) / 100.0f * kTotalSlots;
                break;
        }

        float firstN = (tickIntervalCCK > 0.0f) ? std::floor(m_scrollL / tickIntervalCCK) : 0.0f;

        float lastLabelRight = -9999.0f;
        for (float n = firstN; n * tickIntervalCCK <= m_scrollL + c.visibleRange; n += 1.0f) {
            float tickL = n * tickIntervalCCK;
            if (tickL < m_scrollL)
                continue;

            float x = lToX(tickL);
            dl->AddLine({x, c.canvasPos.y}, {x, c.canvasPos.y + c.canvasH},
                        IM_COL32(55, 55, 55, 255));

            std::string label;
            switch (m_tickUnit) {
                case 1:
                    label = fmt::format("{:.4g}\xc2\xb5s", tickL * kCCKToMicroseconds);
                    break;
                case 2:
                    label = fmt::format("{:.0f}", tickL * kCCKToCycles);
                    break;
                case 3:
                    label = fmt::format("{:.0f}", tickL * kCCKToRasterlines);
                    break;
                default:
                    label = fmt::format("{:.4g}%", (tickL / kTotalSlots) * 100.0f);
                    break;
            }

            ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            float labelW = ts.x * 0.8f;
            float labelX = x - labelW * 0.5f;
            if (labelX >= lastLabelRight + 4.0f) {
                dl->AddText(ImGui::GetFont(), fontSize, {labelX, c.canvasPos.y + c.canvasH + 2.0f},
                            IM_COL32(130, 130, 130, 255), label.c_str());
                lastLabelRight = labelX + labelW;
            }
        }

        // In Cycles mode, once a color clock is wide enough to subdivide, draw a faint
        // half-height divider down the middle of each CCK block. One color clock = 2 CPU
        // cycles, so the midline marks the boundary between this slot's two CPU cycles.
        if (m_tickUnit == 2 && c.slotToPixel >= 16.0f) {
            const float yTop = c.canvasPos.y + c.canvasH * 0.5f;
            const float yBot = c.canvasPos.y + c.canvasH;
            float firstC = std::floor(m_scrollL);
            for (float cc = firstC; cc <= m_scrollL + c.visibleRange; cc += 1.0f) {
                float mid = cc + 0.5f; // CCK midpoint = the 1-CPU-cycle mark
                if (mid < m_scrollL)
                    continue;
                float x = lToX(mid);
                dl->AddLine({x, yTop}, {x, yBot}, IM_COL32(80, 80, 80, 150));
            }
        }
    }
    ImGui::PopFont();
}

void DebuggerFrameScrubberView::DrawBlitBars(TimelineCtx& c)
{
    // Blit bars (below the CPU + DMA rows, upper portion of remaining canvas).
    auto lToX = [&](float l) { return LRefToX(c, l); };
    ImDrawList* dl = c.dl;

    int hoveredBlit = -1;
    int highlightedBlit = -1;
    int pinnedBlit = -1;
    ImVec2 mousePos = ImGui::GetIO().MousePos;
    for (int i = 0; i < static_cast<int>(m_blitRecords.size()); ++i) {
        const auto& rec = m_blitRecords[i];
        float startL = static_cast<float>(rec.lRef);
        float endL = startL + static_cast<float>(rec.dmaCycles);

        float visStart = std::max(startL, m_scrollL);
        float visEnd = std::min(endL, m_scrollL + c.visibleRange);
        if (visEnd <= visStart) {
            // Off-screen, but still resolve the pinned index so its window stays open.
            if (m_pinnedBlitLRef != UINT32_MAX && rec.lRef == m_pinnedBlitLRef)
                pinnedBlit = i;
            continue;
        }

        float x0 = lToX(visStart);
        float x1 = lToX(visEnd);
        if (x1 - x0 < 1.0f)
            x1 = x0 + 1.0f;

        ImVec4 col4 = rec.lineMode ? kColorLine : (rec.raw.fromCopper ? kColorCopper : kColorCpu);
        ImU32 col = ImGui::ColorConvertFloat4ToU32(col4);
        dl->AddRectFilled({x0, c.blitY0}, {x1, c.blitY1}, col);

        if (mousePos.x >= x0 && mousePos.x < x1 && mousePos.y >= c.blitY0 && mousePos.y < c.blitY1)
            hoveredBlit = i;
        if (m_highlightedBlitLRef != UINT32_MAX && rec.lRef == m_highlightedBlitLRef)
            highlightedBlit = i;
        if (m_pinnedBlitLRef != UINT32_MAX && rec.lRef == m_pinnedBlitLRef) {
            pinnedBlit = i;
            // Solid outline marks the pinned blit (distinct from the hover highlight below).
            dl->AddRect({x0, c.blitY0}, {x1, c.blitY1}, IM_COL32(255, 230, 80, 255), 0.0f, 0, 2.0f);
        }
    }

    // Report the overlay target: the blit hovered here, else the one hovered in the Blitter
    // window, else the pinned blit (none if none of these). Routing the Blitter-window hover
    // through this single channel means hovering a Blitter row lights up the emulator overlay
    // exactly as hovering the corresponding scrubber bar does.
    EmitActiveBlit((c.hovered && hoveredBlit >= 0)         ? m_blitRecords[hoveredBlit].lRef
                   : (m_highlightedBlitLRef != UINT32_MAX) ? m_highlightedBlitLRef
                                                           : m_pinnedBlitLRef);

    // Draw highlight outline for the blit hovered in the Blitter window
    if (highlightedBlit >= 0) {
        const auto& rec = m_blitRecords[highlightedBlit];
        float startL = static_cast<float>(rec.lRef);
        float endL = startL + static_cast<float>(rec.dmaCycles);
        float visStart = std::max(startL, m_scrollL);
        float visEnd = std::min(endL, m_scrollL + c.visibleRange);
        if (visEnd > visStart) {
            float x0 = lToX(visStart);
            float x1 = lToX(visEnd);
            if (x1 - x0 < 1.0f)
                x1 = x0 + 1.0f;
            dl->AddRect({x0, c.blitY0}, {x1, c.blitY1}, IM_COL32(255, 255, 255, 220), 0.0f, 0,
                        1.5f);
        }
    }

    c.hoveredBlit = hoveredBlit;
    c.pinnedBlit = pinnedBlit;
}

void DebuggerFrameScrubberView::DrawCopperTicks(const TimelineCtx& c)
{
    // Copper ticks (lower half of canvas) — use vpos/hpos (same lRef space as Trace mode).
    constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;
    auto lToX = [&](float l) { return LRefToX(c, l); };
    ImDrawList* dl = c.dl;
    ImVec2 mousePos = ImGui::GetIO().MousePos;

    int hoveredCopper = (c.hovered && !c.active) ? HitTestCopper(c, mousePos) : -1;
    for (int i = 0; i < static_cast<int>(m_copperEvents.size()); ++i) {
        const auto& ev = m_copperEvents[i];
        float lRef = static_cast<float>(ev.vpos) * kCCKPerLine + static_cast<float>(ev.hpos);
        if (lRef < m_scrollL || lRef > m_scrollL + c.visibleRange)
            continue;
        float x = lToX(lRef);
        if (i == hoveredCopper)
            dl->AddLine({x, c.copperY0}, {x, c.copperY1}, IM_COL32(220, 170, 255, 255), 2.0f);
        else
            dl->AddLine({x, c.copperY0}, {x, c.copperY1}, IM_COL32(180, 100, 255, 160), 1.0f);
    }

    // Copper tick tooltip on hover (and hand cursor to signal it is clickable).
    if (hoveredCopper >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const auto& ev = m_copperEvents[hoveredCopper];
        uint32_t lRef =
            static_cast<uint32_t>(ev.vpos) * kCCKPerLine + static_cast<uint32_t>(ev.hpos);
        DrawCopperTickTooltip(ev, lRef);
    }

    // Draw highlight for the copper instruction hovered in the Copper window
    if (m_highlightedCopperLRef != UINT32_MAX) {
        float lRef = static_cast<float>(m_highlightedCopperLRef);
        if (lRef >= m_scrollL && lRef <= m_scrollL + c.visibleRange) {
            float x = lToX(lRef);
            dl->AddLine({x, c.copperY0}, {x, c.copperY1}, IM_COL32(255, 255, 255, 230), 2.0f);
            const float th = 4.0f;
            dl->AddTriangleFilled({x - th, c.copperY1}, {x + th, c.copperY1},
                                  {x, c.copperY1 - th * 1.4f}, IM_COL32(255, 220, 80, 230));
        }
    }
}

void DebuggerFrameScrubberView::DrawTimeline()
{
    TimelineCtx c;

    // If we have CPU trace data add a row above the blit/copper rows
    // Top strip hosts the selectable heatmap; it needs either the CPU trace (density/stalls/
    // cycles/branches) or the DMA records (pressure/headroom/per-channel) to have content.
    if (m_bandsDirty)
        RebuildBands();
    c.cpuRowH = (m_cpuTraceSeen || m_dmaSeen) ? 12.0f : 0.0f;
    c.dmaRowH = m_dmaSeen ? 12.0f : 0.0f;
    c.idleRowH = m_idleRowActive ? 12.0f : 0.0f;
    c.funcRowH = static_cast<float>(FlameShownDepths()) * kFlameDepthH;
    c.canvasH = 44.0f + c.cpuRowH + c.dmaRowH + c.idleRowH + c.funcRowH;
    const float labelH = ImGui::GetTextLineHeightWithSpacing();
    c.totalH = c.canvasH + labelH;

    c.canvasPos = ImGui::GetCursorScreenPos();
    c.canvasWidth = ImGui::GetContentRegionAvail().x;
    if (c.canvasWidth <= 0.0f)
        return;

    ImGui::InvisibleButton("##scrubcanvas", ImVec2(c.canvasWidth, c.totalH));
    c.hovered = ImGui::IsItemHovered();
    c.active = ImGui::IsItemActive();
    c.activated = ImGui::IsItemActivated();

    // Band geometry, shared by the hit-tests and the render passes. Anchor the blit and copper
    // sub-areas below the CPU+DMA rows so the split stays consistent however many rows exist.
    const float rowsH = c.cpuRowH + c.dmaRowH + c.idleRowH + c.funcRowH;
    c.blitY0 = c.canvasPos.y + rowsH + 3.0f;
    c.blitY1 = c.canvasPos.y + rowsH + (c.canvasH - rowsH) * 0.55f;
    c.copperY0 = c.canvasPos.y + rowsH + (c.canvasH - rowsH) * 0.6f;
    c.copperY1 = c.canvasPos.y + c.canvasH - 3.0f;

    // Input first (zoom/pan/drag), then render passes in z-order.
    HandleTimelineInput(c);

    c.dl = ImGui::GetWindowDrawList();
    ImVec2 canvasEnd = {c.canvasPos.x + c.canvasWidth, c.canvasPos.y + c.canvasH};

    // Background
    c.dl->AddRectFilled(c.canvasPos, canvasEnd, IM_COL32(25, 25, 25, 255));
    c.dl->AddRect(c.canvasPos, canvasEnd, IM_COL32(70, 70, 70, 255));

    DrawHeatRow(c);
    DrawDmaRow(c);
    DrawIdleRow(c);
    DrawFunctionRow(c);
    DrawAxis(c);
    DrawBlitBars(c);
    DrawCopperTicks(c);

    // Blit tooltip on hover (suppressed for the blit that already has a pinned window open).
    bool hoverIsPinned = (c.hoveredBlit >= 0 && c.hoveredBlit == c.pinnedBlit);
    if (c.hoveredBlit >= 0 && c.hovered && !hoverIsPinned) {
        ImGui::SetNextWindowSize({825.0f, 0.0f});
        ImGui::BeginTooltip();
        DrawBlitDetailContents(m_blitRecords[c.hoveredBlit], c.hoveredBlit, /*interactive=*/false,
                               nullptr);
        ImGui::EndTooltip();
    }

    // Partial-frame (mid-frame breakpoint) overlay: dim the region the beam hasn't reached
    // yet this frame and mark where execution stopped. The event rows are already clipped to
    // the break by the core, so this just makes the boundary explicit.
    if (m_partialFrame) {
        float bx = std::clamp(LRefToX(c, static_cast<float>(m_breakLRef)), c.canvasPos.x,
                              c.canvasPos.x + c.canvasWidth);
        const float vx0 = bx;
        const float vx1 = c.canvasPos.x + c.canvasWidth;
        const float vy0 = c.canvasPos.y;
        const float vy1 = c.canvasPos.y + c.canvasH;
        if (vx1 > vx0 + 0.5f) {
            // The "void" region — beam hasn't reached it this frame. Darken it, then overlay a
            // diagonal hatch so it reads as intentionally-empty rather than just dim/black data.
            c.dl->AddRectFilled({vx0, vy0}, {vx1, vy1}, IM_COL32(0, 0, 0, 150));
            c.dl->PushClipRect({vx0, vy0}, {vx1, vy1}, true);
            const ImU32 hatch = IM_COL32(120, 120, 120, 60);
            const float step = 9.0f;
            const float height = vy1 - vy0;
            for (float x = vx0 - height; x < vx1; x += step)
                c.dl->AddLine({x, vy1}, {x + height, vy0}, hatch, 1.0f);
            c.dl->PopClipRect();
            // "VOID" tag centered in the region if there's room.
            const char* tag = "VOID";
            ImVec2 ts = ImGui::CalcTextSize(tag);
            if (vx1 - vx0 > ts.x + 8.0f)
                c.dl->AddText({(vx0 + vx1) * 0.5f - ts.x * 0.5f, (vy0 + vy1) * 0.5f - ts.y * 0.5f},
                              IM_COL32(200, 120, 120, 200), tag);
        }
        ImU32 bc = IM_COL32(255, 80, 80, 235);
        c.dl->AddLine({bx, c.canvasPos.y}, {bx, c.canvasPos.y + c.canvasH}, bc, 2.0f);
        const float bth = 6.0f;
        c.dl->AddTriangleFilled({bx - bth, c.canvasPos.y + c.canvasH - bth * 1.6f},
                                {bx + bth, c.canvasPos.y + c.canvasH - bth * 1.6f},
                                {bx, c.canvasPos.y + c.canvasH}, bc);
    }

    // Scrubber line + handle
    {
        float sx = std::clamp(c.scrubPixelX, c.canvasPos.x, c.canvasPos.x + c.canvasWidth);
        ImU32 sc = IM_COL32(255, 230, 80, 220);
        c.dl->AddLine({sx, c.canvasPos.y}, {sx, c.canvasPos.y + c.canvasH}, sc, 1.5f);
        const float th = 6.0f;
        c.dl->AddTriangleFilled({sx - th, c.canvasPos.y}, {sx + th, c.canvasPos.y},
                                {sx, c.canvasPos.y + th * 1.6f}, sc);
    }

    // Bottom row: position label + hints (left), unit selector (right-aligned)
    static const char* kUnitLabels[] = {"% of frame", "\xc2\xb5s", "Cycles", "Rasterlines"};
    static constexpr float kComboW = 180.0f;

    // Every item on this row is always drawn, in a slot of fixed width, and greyed out when it
    // has nothing to act on - never hidden. Readings that came and went with the frame (Idle,
    // the Heat combo, Show instruction #) or changed width with their value used to shove
    // everything to their right about from one frame to the next. The one reading whose text
    // really does vary, PARTIAL, goes last, where it has nothing to its right to move.
    const float em = ImGui::GetFontSize();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    float slotX = 0.0f;
    const auto nextSlot = [&](float width) {
        ImGui::SameLine(slotX + width + gap);
        slotX += width + gap;
    };

    ImGui::SetCursorScreenPos({c.canvasPos.x, c.canvasPos.y + c.totalH + 2.0f});
    slotX = ImGui::GetCursorPosX();
    ImGui::TextDisabled("L%07X", static_cast<uint32_t>(m_scrubberL));
    nextSlot(ImGui::CalcTextSize("L0000000").x);

    {
        float idleCck = 0.0f;
        for (const auto& s : BuildIdleSpans())
            idleCck += s.endLRef - s.startLRef;
        if (m_idleRowActive)
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "Idle: %5.1f%%",
                               idleCck / kTotalSlots * 100.0f);
        else
            ImGui::TextDisabled("Idle:    --");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Total Bartman idle time this frame (debug_start_idle/stop_idle)%s",
                              m_idleRowActive ? "" : "\nNo idle markers seen yet.");
        nextSlot(ImGui::CalcTextSize("Idle: 100.0%").x);
    }

    {
        const float heatComboW = 9.5f * em;
        ImGui::BeginDisabled(!(m_cpuTraceSeen || m_dmaSeen));
        ImGui::TextDisabled("Heat:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(heatComboW);
        static const char* kHeatLabels[] = {
            "CPU density",  "Bus stalls",   "Cycles / line", "Branch churn", "DMA pressure",
            "CPU headroom", "Bitplane DMA", "Copper DMA",    "Blitter DMA",  "Sprite DMA"};
        ImGui::Combo("##heatmap", &m_topRowMode, kHeatLabels, IM_ARRAYSIZE(kHeatLabels));
        ImGui::EndDisabled();
        nextSlot(ImGui::CalcTextSize("Heat:").x + ImGui::GetStyle().ItemSpacing.x + heatComboW);
    }

    {
        ImGui::BeginDisabled(!(m_cpuTraceSeen && m_dmaSeen));
        ImGui::Checkbox("Show instruction #", &m_showInstrNumbers);
        ImGui::EndDisabled();
        nextSlot(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                 ImGui::CalcTextSize("Show instruction #").x);
    }

    {
        const float bandComboW = 5.0f * em;
        ImGui::BeginDisabled(!m_debugSymbols);
        ImGui::Checkbox("Fn bands", &m_showFuncBands);
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_showFuncBands);
        ImGui::SetNextItemWidth(bandComboW);
        static const char* kBandModes[] = {"Flat", "Flame"};
        if (ImGui::Combo("##bandmode", &m_bandMode, kBandModes, IM_ARRAYSIZE(kBandModes)))
            m_bandsDirty = true;
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }

    if (m_partialFrame) {
        ImGui::SameLine();
        constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "PARTIAL — broke at line %u",
                           m_breakLRef / kCCKPerLine);
    }

    ImGui::SetCursorScreenPos(
        {c.canvasPos.x + c.canvasWidth - kComboW, c.canvasPos.y + c.totalH + 2.0f});
    ImGui::SetNextItemWidth(kComboW);
    ImGui::Combo("##tickunit", &m_tickUnit, kUnitLabels, IM_ARRAYSIZE(kUnitLabels));
}

void DebuggerFrameScrubberView::DrawBlitDetailContents(const adm::core::BlitRecord& rec, int index,
                                                       bool interactive, bool* addrHovered)
{
    const auto& raw = rec.raw;

    static constexpr float kLabelCol = 210.0f;

    // Left-edge availability band: indent the content so text clears the band plus a gap.
    // The band itself is drawn at the end, once the content height is known.
    constexpr float kBandW = 8.0f, kBandGap = 8.0f;
    const ImVec2 bandWinPos = ImGui::GetWindowPos();
    float bandIndent = kBandW + kBandGap - ImGui::GetStyle().WindowPadding.x;
    if (bandIndent < 0.0f)
        bandIndent = 0.0f;
    ImGui::Indent(bandIndent);

    // Header: timing
    ImGui::Text("Blit #%d", index + 1);
    ImGui::Text("Trigger: %s%s", raw.fromCopper ? "Copper" : "CPU", rec.lineMode ? "  [LINE]" : "");
    ImGui::Text("Start: Line %u, Clock %u  L%07X", rec.startLine, rec.startClock, rec.startCycle);
    ImGui::Text("End:   Line %u, Clock %u  L%07X", rec.endLine, rec.endClock, rec.endCycle);
    ImGui::Text("Duration: %u DMA cycles", rec.dmaCycles);

    // What the blit actually wrote, when destination capture is armed in the Blitter
    // Operations window. Capped small enough that the tooltip stays a tooltip.
    if (BlitDestPreview::HasData(rec)) {
        ImGui::Separator();
        m_blitDestPreview.Detail(rec, 420.0f, 300.0f);
    }

    ImGui::Separator();

    // Draw text with a filled box background when active, or dimmed when inactive
    auto DrawFlag = [](const char* label, bool active) {
        if (active) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 sz = ImGui::CalcTextSize(label);
            constexpr float px = 3.0f, py = 1.5f;
            dl->AddRectFilled({pos.x - px, pos.y - py}, {pos.x + sz.x + px, pos.y + sz.y + py},
                              IM_COL32(55, 90, 195, 220), 2.0f);
            ImGui::TextUnformatted(label);
        } else {
            ImGui::TextDisabled("%s", label);
        }
    };

    // Renders a 24-bit address. In the pinned window it is a clickable memory hyperlink;
    // in the hover tooltip it is plain text (tooltips can't receive clicks).
    auto AddrLink = [&](uint32_t addr) {
        if (!interactive || !m_addrCtx || !m_addrCtx->valid()) {
            ImGui::Text("$%06X", addr);
            return;
        }
        char suffix[24];
        snprintf(suffix, sizeof(suffix), "blit_%X", addr);
        DrawAddressLink(suffix, addr & 0x00FFFFFFu, "$%06X", *m_addrCtx);
        if (addrHovered && ImGui::IsItemHovered())
            *addrHovered = true;
    };

    // Size
    ImGui::Text("Size");
    ImGui::SameLine(kLabelCol);
    ImGui::Text("%ux%upx", static_cast<unsigned>(rec.wordsPerRow) * 16u,
                static_cast<unsigned>(rec.rows));

    // Blitter Control flags (from BLTCON0 and BLTCON1)
    struct FlagDef
    {
        const char* name;
        bool active;
    };
    const FlagDef flags[] = {
        {"USEA", rec.useA},
        {"USEB", rec.useB},
        {"USEC", rec.useC},
        {"USED", rec.useD},
        {"LINE", (raw.bltcon1 & 0x01) != 0},
        {"DOFF", (raw.bltcon1 & 0x20) != 0},
        {"EFE", (raw.bltcon1 & 0x08) != 0},
        {"IFE", (raw.bltcon1 & 0x10) != 0},
        {"FCI", (raw.bltcon1 & 0x04) != 0},
        {"DESC", (raw.bltcon1 & 0x02) != 0},
    };
    ImGui::Text("Blitter Control");
    ImGui::SameLine(kLabelCol);
    for (int fi = 0; fi < static_cast<int>(std::size(flags)); ++fi) {
        if (fi > 0)
            ImGui::SameLine(0, 8);
        DrawFlag(flags[fi].name, flags[fi].active);
    }

    // Sources and Destination
    const uint8_t ashif = static_cast<uint8_t>((raw.bltcon0 >> 12) & 0xF);
    const uint8_t bshif = static_cast<uint8_t>((raw.bltcon1 >> 12) & 0xF);

    // Boolean channel-relevance tests on the 8-bit minterm truth table.
    // A channel is irrelevant if swapping it never changes D for any combination
    // of the other two channels.
    const uint8_t m = rec.minterm;
    const bool minterDependsOnA = ((m >> 4) & 0xF) != (m & 0x0F);
    const bool minterDependsOnB = (m & 0x33) != ((m >> 2) & 0x33);
    const bool minterDependsOnC = (m & 0x55) != ((m >> 1) & 0x55);

    // Formats a uint16_t as grouped binary: "1010 1111 0000 1100"
    auto FormatBinary16 = [](uint16_t v, char* buf, int bufsz) {
        snprintf(buf, bufsz, "%c%c%c%c %c%c%c%c %c%c%c%c %c%c%c%c", (v >> 15 & 1) ? '1' : '0',
                 (v >> 14 & 1) ? '1' : '0', (v >> 13 & 1) ? '1' : '0', (v >> 12 & 1) ? '1' : '0',
                 (v >> 11 & 1) ? '1' : '0', (v >> 10 & 1) ? '1' : '0', (v >> 9 & 1) ? '1' : '0',
                 (v >> 8 & 1) ? '1' : '0', (v >> 7 & 1) ? '1' : '0', (v >> 6 & 1) ? '1' : '0',
                 (v >> 5 & 1) ? '1' : '0', (v >> 4 & 1) ? '1' : '0', (v >> 3 & 1) ? '1' : '0',
                 (v >> 2 & 1) ? '1' : '0', (v >> 1 & 1) ? '1' : '0', (v >> 0 & 1) ? '1' : '0');
    };
    char binbuf[24];

    ImGui::Text("Source A");
    ImGui::SameLine(kLabelCol);
    if (!rec.useA && minterDependsOnA) {
        FormatBinary16(raw.bltadat, binbuf, sizeof(binbuf));
        ImGui::Text("BLTADAT $%04X  %s", raw.bltadat, binbuf);
    } else if (!rec.useA)
        ImGui::TextDisabled("(not used)");
    else {
        AddrLink(raw.bltapt);
        ImGui::SameLine(0, 0);
        ImGui::Text("  Shift %u  Mod %d", ashif, static_cast<int16_t>(raw.bltamod));
        ImGui::Text("Masks");
        ImGui::SameLine(kLabelCol);
        FormatBinary16(raw.bltafwm, binbuf, sizeof(binbuf));
        char binbuf2[24];
        FormatBinary16(raw.bltalwm, binbuf2, sizeof(binbuf2));
        ImGui::Text("FWM %%%s  LWM %%%s", binbuf, binbuf2);
    }

    ImGui::Text("Source B");
    ImGui::SameLine(kLabelCol);
    if (!rec.useB && minterDependsOnB) {
        FormatBinary16(raw.bltbdat, binbuf, sizeof(binbuf));
        ImGui::Text("BLTBDAT $%04X  %s", raw.bltbdat, binbuf);
    } else if (!rec.useB)
        ImGui::TextDisabled("(not used)");
    else {
        AddrLink(raw.bltbpt);
        ImGui::SameLine(0, 0);
        ImGui::Text("  Shift %u  Mod %d", bshif, static_cast<int16_t>(raw.bltbmod));
    }

    ImGui::Text("Source C");
    ImGui::SameLine(kLabelCol);
    if (!rec.useC && minterDependsOnC) {
        FormatBinary16(raw.bltcdat, binbuf, sizeof(binbuf));
        ImGui::Text("BLTCDAT $%04X  %s", raw.bltcdat, binbuf);
    } else if (!rec.useC)
        ImGui::TextDisabled("(not used)");
    else {
        AddrLink(raw.bltcpt);
        ImGui::SameLine(0, 0);
        ImGui::Text("  Mod %d", static_cast<int16_t>(raw.bltcmod));
    }

    ImGui::Text("Destination");
    ImGui::SameLine(kLabelCol);
    if (!rec.useD)
        ImGui::TextDisabled("(not used)");
    else {
        AddrLink(raw.bltdpt);
        ImGui::SameLine(0, 0);
        ImGui::Text("  Mod %d", static_cast<int16_t>(raw.bltdmod));
    }

    // Minterm
    static constexpr const char* kTerms[8] = {"ABC", "ABc", "AbC", "Abc",
                                              "aBC", "aBc", "abC", "abc"};
    ImGui::Text("Minterm");
    ImGui::SameLine(kLabelCol);
    if (m_onOpenMinterm) {
        ImGui::TextColored(kHyperlinkColor, "$%02X ", rec.minterm);
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImVec2 mn = ImGui::GetItemRectMin();
            ImVec2 mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine({mn.x, mx.y - 1.0f}, {mx.x, mx.y - 1.0f},
                                                ImGui::ColorConvertFloat4ToU32(kHyperlinkColor));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                m_onOpenMinterm(raw.bltcon0, raw.bltcon1);
        }
    } else {
        ImGui::Text("$%02X ", rec.minterm);
    }
    for (int i = 0; i < 8; ++i) {
        ImGui::SameLine(0, 8);
        DrawFlag(kTerms[i], ((rec.minterm >> (7 - i)) & 1) != 0);
    }

    // Common blit presets
    ImGui::Text("Common");
    ImGui::SameLine(kLabelCol);
    for (int ci = 0; ci < static_cast<int>(std::size(kBlitOpPresets)); ++ci) {
        if (ci > 0)
            ImGui::SameLine(0, 8);
        bool match = (raw.bltcon0 == kBlitOpPresets[ci].bltcon0) &&
                     (raw.bltcon1 == kBlitOpPresets[ci].bltcon1);
        DrawFlag(kBlitOpPresets[ci].name, match);
    }

    ImGui::Unindent(bandIndent);

    // Draw the band denoting emulator-overlay availability for this blit:
    // green = on-screen (front) buffer, orange = off-screen (back) buffer, grey = no overlay.
    core::BlitOverlayBox box = core::MapBlitDestToScreen(rec, m_overlayLayout, m_overlayMap);
    ImU32 bandCol;
    if (!box.valid)
        bandCol = IM_COL32(140, 140, 140, 230);
    else if (box.kind == core::BlitBufferKind::Front)
        bandCol = IM_COL32(90, 255, 120, 255);
    else
        bandCol = IM_COL32(255, 190, 60, 255);

    // Foreground draw list so the strip flush against the left edge isn't trimmed by the
    // window's padded content clip rect. Span from the window top to just past the last item.
    float bandBottom = ImGui::GetCursorScreenPos().y + ImGui::GetStyle().WindowPadding.y;
    ImGui::GetForegroundDrawList(ImGui::GetWindowViewport())
        ->AddRectFilled({bandWinPos.x, bandWinPos.y}, {bandWinPos.x + kBandW, bandBottom}, bandCol);
}

void DebuggerFrameScrubberView::DrawPinnedBlitWindow()
{
    if (m_pinnedBlitLRef == UINT32_MAX)
        return;

    // Resolve the pinned lRef to the current frame's record; if it no longer exists
    // (e.g. the frame changed), drop the pin.
    int idx = -1;
    for (int i = 0; i < static_cast<int>(m_blitRecords.size()); ++i) {
        if (m_blitRecords[i].lRef == m_pinnedBlitLRef) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        m_pinnedBlitLRef = UINT32_MAX;
        return;
    }

    // Freeze the panel where it was clicked on the first frame; afterwards it is free to be
    // dragged by the user (body-drag, since there is no title bar).
    if (m_pinnedPosPending) {
        ImGui::SetNextWindowPos(m_pinnedWindowPos, ImGuiCond_Always);
        m_pinnedPosPending = false;
    }

    // Chrome-less, auto-sized panel — no title bar, collapse, resize grip or saved settings.
    // It remains draggable from its body (ConfigWindowsMoveFromTitleBarOnly defaults to off).
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    // DrawBlitDetailContents draws the left-edge availability band itself.
    if (ImGui::Begin("##scrubberpin_blitdetail", nullptr, flags)) {
        DrawBlitDetailContents(m_blitRecords[idx], idx, /*interactive=*/true, nullptr);

        // Close button in the top-right corner. Drawn with the window draw list and a manual
        // hit-test (rather than an ImGui widget) so it overlays the existing content without
        // reflowing the layout, growing the auto-sized window, or adding any window chrome.
        const ImVec2 winPos = ImGui::GetWindowPos();
        const ImVec2 winSize = ImGui::GetWindowSize();
        const float sz = ImGui::GetFontSize();
        const float pad = ImGui::GetStyle().WindowPadding.x;
        const ImVec2 bmin = {winPos.x + winSize.x - sz - pad, winPos.y + pad};
        const ImVec2 bmax = {bmin.x + sz, bmin.y + sz};
        const bool hovered =
            ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(bmin, bmax, false);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hovered)
            dl->AddRectFilled(bmin, bmax, IM_COL32(200, 60, 60, 255), 2.0f);
        const ImU32 xcol = IM_COL32(235, 235, 235, hovered ? 255 : 170);
        const float t = sz * 0.30f;
        dl->AddLine({bmin.x + t, bmin.y + t}, {bmax.x - t, bmax.y - t}, xcol, 1.5f);
        dl->AddLine({bmax.x - t, bmin.y + t}, {bmin.x + t, bmax.y - t}, xcol, 1.5f);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_pinnedBlitLRef = UINT32_MAX;
    }
    ImGui::End();
}

} // namespace adm::ui
