#include "DebuggerView.h"
#include "../../core/DebugFrame.h"
#include "../../core/MemoryAnnotations.h"
#include "../../core/MemoryLocks.h"
#include "../../core/SegListResolver.h"
#include "../../core/StackUnwinder.h"
#include "../AddressLink.h"
#include "ARConsoleView.h"
#include "BreakpointsView.h"
#include "DebuggerAudioView.h"
#include "DebuggerBartmanView.h"
#include "DebuggerBitplanesView.h"
#include "DebuggerCIAView.h"
#include "DebuggerCPUView.h"
#include "DebuggerCallStackView.h"
#include "DebuggerCopperView.h"
#include "DebuggerCustomView.h"
#include "DebuggerDisassemblyView.h"
#include "DebuggerDisplayView.h"
#include "DebuggerFrameScrubberView.h"
#include "DebuggerMemoryView.h"
#include "DebuggerSpritesView.h"
#include "DebuggerTrainerView.h"
#include "EmulatorHostHooks.h"
#include "EmulatorView.h"
#include "MemoryAnnotationsView.h"

namespace adm::ui {

DebuggerView::DebuggerView(EmulatorView* emulatorView)
    : m_emulatorView(emulatorView), m_cpuView(std::make_unique<DebuggerCPUView>(emulatorView)),
      m_customView(std::make_unique<DebuggerCustomView>(emulatorView)),
      m_copperView(std::make_unique<DebuggerCopperView>(emulatorView)),
      m_memoryView(std::make_unique<DebuggerMemoryView>(emulatorView)),
      m_displayView(std::make_unique<DebuggerDisplayView>(emulatorView)),
      m_spritesView(std::make_unique<DebuggerSpritesView>(emulatorView)),
      m_bitplanesView(std::make_unique<DebuggerBitplanesView>(emulatorView)),
      m_bartmanView(std::make_unique<DebuggerBartmanView>(emulatorView)),
      m_ciaView(std::make_unique<DebuggerCIAView>(emulatorView)),
      m_audioView(std::make_unique<DebuggerAudioView>(emulatorView)),
      m_scrubberView(std::make_unique<DebuggerFrameScrubberView>()),
      m_callStackView(std::make_unique<DebuggerCallStackView>()),
      m_annotations(std::make_unique<core::MemoryAnnotations>()),
      m_locks(std::make_unique<core::MemoryLocks>()),
      m_breakpointsView(std::make_unique<BreakpointsView>(emulatorView)),
      m_consoleView(std::make_unique<ARConsoleView>(emulatorView, m_breakpointsView.get(),
                                                    m_annotations.get(), m_locks.get())),
      m_disasmView(std::make_unique<DebuggerDisassemblyView>(emulatorView, m_breakpointsView.get(),
                                                             m_annotations.get())),
      m_annotationsView(std::make_unique<MemoryAnnotationsView>(m_annotations.get()))
{
    // The copper view is constructed before the breakpoint store, so wire it up now to enable
    // the Copper-breakpoint gutter.
    m_copperView->SetBreakpointsView(m_breakpointsView.get());

    // Built here rather than in the initialiser list because it binds a member function: the
    // trainer window resolves anchors through the same MakeLockContext() everything else uses.
    m_trainerView = std::make_unique<DebuggerTrainerView>(m_locks.get(),
                                                          [this]() { return MakeLockContext(); });
    m_trainerView->SetProtectionSupport([this]() {
        using Support = DebuggerTrainerView::ProtectionSupport;
        auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
        // No core yet is not the same as a core without the hooks: SupportsProtections()
        // resolves its symbols at load time, so before that there is nothing to report.
        if (!emu || !emu->IsCoreLoaded())
            return Support::Unknown;
        return emu->SupportsProtections() ? Support::Supported : Support::Unsupported;
    });

    // Hand the same store to the register windows and the memory inspector. They share one
    // MemoryLocks object rather than synchronising copies, so a lock made in any of them is
    // immediately visible in the others, in the Trainer window and in the PR listing.
    LockUi lockUi{m_locks.get(), [this]() { return MakeLockContext(); }};
    m_customView->SetLocks(lockUi);
    m_ciaView->SetLocks(lockUi);
    m_memoryView->SetLocks(lockUi);

    // Wire frame scrubber → copper view, disassembly view, geometry overlay, and external callback
    m_scrubberView->m_onScrubberChanged = [this](uint32_t lRef) {
        m_copperView->SetScrubberLRef(lRef);
        m_disasmView->SetScrubberLRef(lRef);
        m_disasmView->SetTracePositionLRef(lRef);
        m_displayView->SetCrosshairLRef(lRef);
        if (m_onScrubberChanged)
            m_onScrubberChanged(lRef);
    };

    // Scrubber copper-tick click → sync all linked views, then open + scroll the Copper trace
    m_scrubberView->m_onJumpToCopper = [this](uint32_t lRef) {
        JumpToLRef(lRef);
        m_copperView->JumpToTraceLRef(lRef);
    };

    // Scrubber timeline "Break at this line" → arm a raster (beam) breakpoint.
    m_scrubberView->m_onBreakAtLine = [this](int vpos) {
        if (m_breakpointsView)
            m_breakpointsView->SetBeamBreak(vpos, -1);
    };

    // Scrubber blit hover/pin → forward to Application for the emulator destination overlay
    m_scrubberView->m_onActiveBlitChanged = [this](uint32_t lRef) {
        if (m_onActiveBlitChanged)
            m_onActiveBlitChanged(lRef);
    };

    // Scrubber blit-detail minterm click → forward to Application to open the Minterm window
    m_scrubberView->m_onOpenMinterm = [this](uint16_t bltcon0, uint16_t bltcon1) {
        if (m_onOpenMinterm)
            m_onOpenMinterm(bltcon0, bltcon1);
    };

    // Wire copper row click → sync scrubber + blitter + disasm
    m_copperView->m_onJumpToLRef = [this](uint32_t lRef) { JumpToLRef(lRef); };

    // Wire copper row hover → highlight corresponding copper tick in frame scrubber
    m_copperView->m_onHoverLRef = [this](uint32_t lRef) {
        m_scrubberView->SetHighlightedCopper(lRef);
        if (m_onRasterHighlight)
            m_onRasterHighlight(lRef);
    };

    // Scrubber DMA-row hover over a CPU instruction's slot-run → highlight the owning row in
    // the disassembly trace (UINT32_MAX clears it).
    m_scrubberView->m_onCpuSlotHovered = [this](uint32_t lRef) {
        m_disasmView->SetTraceHoverLRef(lRef);
    };

    // Scrubber DMA-slot tooltip → pull the instruction's mnemonic + cycle costs from the
    // disassembly view (which already computes them for the Trace table).
    m_scrubberView->m_queryInstrInfo =
        [this](int index) -> DebuggerFrameScrubberView::CpuInstrInfo {
        DebuggerFrameScrubberView::CpuInstrInfo info;
        info.valid = m_disasmView->GetTraceRowDisplay(index, info.text, info.nominalCycles,
                                                      info.deltaCycles, info.pc);
        return info;
    };

    // Copper MOVE register hyperlink → open Custom Registers, navigate to the register, and seed
    // the bit-field decoder with the copperlist instruction's value.
    m_copperView->m_onJumpToCustomReg = [this](uint32_t offset, uint16_t value) {
        m_customView->NavigateToRegister(offset, true, value);
    };

    // Wire geometry overlay click → sync all linked views (same as any other JumpToLRef source)
    m_displayView->m_onJumpToLRef = [this](uint32_t lRef) { JumpToLRef(lRef); };

    m_displayView->m_onShowSpriteBBoxesChanged = [this](bool show) {
        m_spritesView->SetAllBBoxes(show);
    };

    // Forwarded to Application, which owns the emulator and so the core-side map.
    m_displayView->m_onBlitOverlayChanged = [this](const BlitOverlaySettings& s) {
        if (m_onBlitOverlayChanged)
            m_onBlitOverlayChanged(s);
    };

    m_displayView->m_onCaptureDestDataChanged = [this](bool on) {
        if (m_onBlitCaptureDestDataChanged)
            m_onBlitCaptureDestDataChanged(on);
    };

    // Likewise for the raster overlay: Application arms the core's DMA recording.
    m_displayView->m_onShowCopperRasterChanged = [this](bool show) {
        if (m_onShowCopperRasterChanged)
            m_onShowCopperRasterChanged(show);
    };

    // Any console command could change breakpoint state — tell BreakpointsView to re-query.
    m_consoleView->m_onAnyCommandExecuted = [this]() { m_breakpointsView->MarkNeedsRefresh(); };

    // Console PROFILE / CALLSTACK commands read the live profiler snapshot (assembled from
    // the disassembly Profile view + the frame scrubber's call-stack reconstruction).
    m_consoleView->m_onGetProfile = [this](const core::ProfileQuery& q) {
        return GetProfileSnapshot(q);
    };

    // Console copper register hyperlink → open Custom Registers and navigate to the register
    m_consoleView->m_onJumpToCustomReg = [this](uint32_t offset) {
        m_customView->NavigateToRegister(offset);
    };

    // Wire disasm row click → sync scrubber + copper + blitter (but NOT disasm, it owns its click)
    m_disasmView->m_onJumpToLRef = [this](uint32_t lRef) {
        m_scrubberView->SetScrubberLRef(lRef);
        m_disasmView->SetScrubberLRef(lRef);
        m_copperView->SetScrubberLRef(lRef);
        if (m_onScrubberChanged)
            m_onScrubberChanged(lRef);
    };

    m_disasmView->m_onTraceEntrySelected = [this](bool active,
                                                  const adm::AmigaEmulator::CpuTraceEntry& entry) {
        m_cpuView->SetTraceEntry(active, entry);
    };

    BuildAddressContext();
}

void DebuggerView::BuildAddressContext()
{
    m_addrCtx.breakpoints = m_breakpointsView.get();
    m_addrCtx.annotations = m_annotations.get();

    m_addrCtx.jumpToMemory = [this](uint32_t addr, size_t len) {
        m_memoryView->HighlightMemoryResult(addr, len);
    };
    m_addrCtx.jumpToDisasm = [this](uint32_t addr) {
        *m_disasmView->GetOpenPtr() = true;
        m_disasmView->GoToAddress(addr);
    };
    m_addrCtx.showInMemoryMap = [this](uint32_t addr) {
        if (m_onShowInMemoryMap)
            m_onShowInMemoryMap(addr);
    };
    m_addrCtx.runConsoleCommand = [this](const std::string& cmd) {
        m_consoleView->RunCommandLine(cmd);
        m_breakpointsView->MarkNeedsRefresh();
    };
    m_addrCtx.readLong = [this](uint32_t addr, uint32_t& out) {
        return m_consoleView->ReadMemoryLong(addr, out);
    };
    m_addrCtx.isPaused = [this]() {
        return m_emulatorView->IsRunning() && m_emulatorView->IsPaused();
    };

    // Hand the shared context to every view that draws address links.
    m_cpuView->SetAddressContext(&m_addrCtx);
    m_customView->SetAddressContext(&m_addrCtx);
    m_copperView->SetAddressContext(&m_addrCtx);
    m_disasmView->SetAddressContext(&m_addrCtx);
    m_scrubberView->SetAddressContext(&m_addrCtx);
    m_consoleView->SetAddressContext(&m_addrCtx);
    m_annotationsView->SetAddressContext(&m_addrCtx);
    m_breakpointsView->SetAddressContext(&m_addrCtx);
    m_bitplanesView->SetAddressContext(&m_addrCtx);
    m_bartmanView->SetAddressContext(&m_addrCtx);
    m_audioView->SetAddressContext(&m_addrCtx);

    // Call Stack rows follow a frame in the Disassembly window, or open its source.
    m_callStackView->SetOnNavigateToAddress([this](uint32_t addr) {
        *m_disasmView->GetOpenPtr() = true;
        m_disasmView->GoToAddress(addr);
    });
    m_callStackView->SetOnOpenSource(
        [this](const std::string& file, uint32_t line) { m_disasmView->OpenSource(file, line); });
}

DebuggerView::~DebuggerView() = default;

void DebuggerView::SetUserPreferences(core::UserPreferences* preferences)
{
    m_bitplanesView->SetUserPreferences(preferences);
    m_audioView->SetUserPreferences(preferences);
    m_disasmView->SetUserPreferences(preferences);
}

void DebuggerView::Draw()
{
    // Trace-heavy windows have nothing meaningful to show while turbo is actively running the
    // machine flat out — the data changes every emulated frame faster than anyone could read
    // it. Suppressed only while turbo is actually engaged, not merely while a debugger window
    // happens to be open; resumes live the instant turbo ends or the machine pauses. Console,
    // Breakpoints, Trainer, Annotations and the Display panel aren't trace-driven and keep
    // updating normally.
    const bool turboActive = m_emulatorView && m_emulatorView->IsTurbo() &&
                             m_emulatorView->IsRunning() && !m_emulatorView->IsPaused();

    m_cpuView->SetTurboSuppressed(turboActive);
    m_customView->SetTurboSuppressed(turboActive);
    m_disasmView->SetTurboSuppressed(turboActive);
    m_copperView->SetTurboSuppressed(turboActive);
    m_memoryView->SetTurboSuppressed(turboActive);
    m_spritesView->SetTurboSuppressed(turboActive);
    m_bitplanesView->SetTurboSuppressed(turboActive);
    m_bartmanView->SetTurboSuppressed(turboActive);
    m_ciaView->SetTurboSuppressed(turboActive);
    m_audioView->SetTurboSuppressed(turboActive);
    m_scrubberView->SetTurboSuppressed(turboActive);
    m_callStackView->SetTurboSuppressed(turboActive);

    m_cpuView->Draw();
    m_customView->Draw();
    m_breakpointsView->Draw(); // must precede disasm so its cache is fresh before gutter queries
    m_disasmView->Draw();
    m_copperView->Draw();
    m_memoryView->Draw();
    m_displayView->Draw();
    m_spritesView->Draw();
    m_bitplanesView->Draw();
    m_bartmanView->Draw();
    m_ciaView->Draw();
    m_audioView->Draw();
    m_scrubberView->Draw();
    // Feed the Call Stack window both of its sources: the live DWARF unwind at the halted
    // PC, and the scrubber's reconstructed stack at the playhead. Skipped while turbo is
    // suppressing live views — ComputeLiveCallStack only produces anything once the machine
    // is paused anyway, so this is a free, correct skip rather than a behavior change.
    if (m_callStackView->IsOpen() && !turboActive) {
        UpdateLiveCallStack();
        m_scrubberView->EnsureBands();
        const uint32_t lRef = m_scrubberView->GetScrubberLRef();
        m_callStackView->SetStack(m_scrubberView->GetCallStackAt(lRef), lRef, TraceStackState());
    }
    m_callStackView->Draw();
    m_trainerView->Draw();
    m_consoleView->Draw();
    m_annotationsView->Draw();

    // Global label/comment editor opened from any address link's right-click menu.
    DrawAddressLinkModals();
}

void DebuggerView::Clear()
{
    m_disasmView->Clear();
    m_copperView->Clear();
    m_scrubberView->Clear();
    m_cpuView->Clear();
    m_memoryView->Clear();
    m_ciaView->Clear();
    m_spritesView->Clear();
    m_bitplanesView->Clear();
    m_bartmanView->Clear();
    m_audioView->Clear();
    m_callStackView->Clear();
    m_trainerView->Clear();
}

void DebuggerView::SetCopperScrubberLRef(uint32_t lRef) { m_copperView->SetScrubberLRef(lRef); }

void DebuggerView::SetHighlightedBlit(uint32_t lRef) { m_scrubberView->SetHighlightedBlit(lRef); }

void DebuggerView::PinBlit(uint32_t lRef) { m_scrubberView->PinBlit(lRef); }

void DebuggerView::SetHighlightedCopper(uint32_t lRef)
{
    m_scrubberView->SetHighlightedCopper(lRef);
}

void DebuggerView::JumpToLRef(uint32_t lRef)
{
    if (m_onRasterSelect)
        m_onRasterSelect(lRef);
    m_scrubberView->SetScrubberLRef(lRef);
    m_copperView->SetScrubberLRef(lRef);
    m_disasmView->SetScrubberLRef(lRef);
    m_disasmView->SetTracePositionLRef(lRef);
    m_displayView->SetCrosshairLRef(lRef);
    if (m_onScrubberChanged)
        m_onScrubberChanged(lRef);
}

void DebuggerView::SetScrubberData(const std::vector<adm::core::BlitRecord>& records,
                                   const std::vector<adm::AmigaEmulator::CopperExecEvent>& events,
                                   uint32_t frameStartCycle)
{
    m_scrubberView->SetBlitRecords(records);
    m_scrubberView->SetCopperEvents(events);
    m_scrubberView->SetFrameStartCycle(frameStartCycle);
}

void DebuggerView::SetScrubberIdleEvents(const std::vector<adm::AmigaEmulator::IdleEvent>& events)
{
    m_scrubberView->SetIdleEvents(events);
}

void DebuggerView::SetCpuTraceData(const adm::AmigaEmulator::CpuTraceEntry* entries, int count,
                                   uint32_t frameStartCycle)
{
    if (entries && count > 0)
        m_cpuTraceSnapshot.assign(entries, entries + count);
    else
        m_cpuTraceSnapshot.clear();

    const auto* snapshot = m_cpuTraceSnapshot.data();
    const int snapshotCount = static_cast<int>(m_cpuTraceSnapshot.size());
    m_scrubberView->SetCpuTraceData(snapshot, snapshotCount);
    m_disasmView->SetFrameStartCycle(frameStartCycle);
    m_disasmView->SetTraceData(snapshot, snapshotCount);
}

void DebuggerView::SetSourceDebugContext(adm::core::SourceDebugContext context)
{
    m_sourceDebug = std::move(context);
    // Any change to the program or its load address invalidates a cached unwind.
    m_liveStackValid = false;
    m_liveStackRun = UINT64_MAX;
    m_disasmView->SetFunctionSymbols(m_sourceDebug.symbols, m_sourceDebug.lines,
                                     m_sourceDebug.segments);
    m_scrubberView->SetFunctionSymbols(m_sourceDebug.symbols, m_sourceDebug.lines,
                                       m_sourceDebug.inlines, m_sourceDebug.segments);
}

// Why the trace-derived call stack has nothing to show. Reported as a state rather than a
// message so the view owns the wording, and so the AR console can reuse the distinction.
DebuggerCallStackView::TraceState DebuggerView::TraceStackState() const
{
    using TraceState = DebuggerCallStackView::TraceState;
    if (!m_scrubberView->HasDebugInfo())
        return TraceState::NoSymbols;
    if (!m_scrubberView->HasCpuTrace())
        return TraceState::NoCpuTrace;
    if (!m_scrubberView->HasSegments())
        return TraceState::NoSegments;
    return TraceState::Ok;
}

bool DebuggerView::ComputeLiveCallStack(core::UnwindResult& outResult, std::string& outRegion)
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    // Registers and stack memory are only meaningful while the machine is stopped: on a
    // running target they change under us between the read and the unwind.
    if (!emu || !m_emulatorView->IsRunning() || !m_emulatorView->IsPaused()) {
        return false;
    }

    adm::AmigaEmulator::CpuState cpu{};
    if (!m_emulatorView->GetCpuState(cpu)) {
        return false;
    }

    core::ArMemoryAccess mem;
    {
        std::vector<core::ArMemoryAccess::Region> regions;
        for (const auto& r : m_emulatorView->GetFreshMemoryRegions()) {
            if (r.hostPtr)
                regions.push_back({r.hostPtr, r.amigaAddr, r.size, r.name, r.writable});
        }
        mem.SetRegions(std::move(regions));
    }

    core::UnwindInputs in;
    for (int i = 0; i < 8; ++i) {
        in.regs.reg[static_cast<size_t>(core::kDwarfRegD0) + i] = cpu.d[i];
        in.regs.reg[static_cast<size_t>(core::kDwarfRegA0) + i] = cpu.a[i];
    }
    in.regs.pc = cpu.pc;
    in.readLong = [&mem](uint32_t addr, uint32_t& out) {
        bool ok = false;
        out = mem.ReadLong(addr, &ok);
        return ok;
    };
    const core::SourceDebugContext* ctx = &m_sourceDebug;
    in.toLink = [ctx](uint32_t addr) { return ctx->ToLink(addr); };
    in.frames = m_sourceDebug.frames;
    in.symbols = m_sourceDebug.symbols;
    in.lines = m_sourceDebug.lines;
    in.inlines = m_sourceDebug.inlines;

    core::UnwindResult result = core::StackUnwinder::Unwind(in);

    // Name the region the walk stopped in, so "no CFI beyond $00FC1A2E" can say
    // "(Kickstart ROM)" rather than leaving the user to look the address up.
    std::string region;
    if (const auto* r = mem.FindRegion(result.stopPc)) {
        region = r->name;
    }
    outResult = std::move(result);
    outRegion = std::move(region);
    return true;
}

void DebuggerView::UpdateLiveCallStack()
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    adm::AmigaEmulator::CpuState cpu{};
    const bool halted = emu && m_emulatorView->IsRunning() && m_emulatorView->IsPaused() &&
                        m_emulatorView->GetCpuState(cpu);
    if (!halted) {
        m_callStackView->ClearLiveStack();
        m_liveStackValid = false;
        return;
    }

    // The unwind is only a handful of memory reads, but ImGui redraws many times between
    // emulated frames and there is no reason to repeat it. GetRunCount ticks on single
    // steps and breakpoint stops as well as whole frames.
    const uint64_t run = emu->GetRunCount();
    if (m_liveStackValid && run == m_liveStackRun && cpu.pc == m_liveStackPc) {
        return;
    }

    core::UnwindResult result;
    std::string region;
    if (!ComputeLiveCallStack(result, region)) {
        m_callStackView->ClearLiveStack();
        m_liveStackValid = false;
        return;
    }
    m_liveStackRun = run;
    m_liveStackPc = cpu.pc;
    m_liveStackValid = true;
    m_callStackView->SetLiveStack(std::move(result), std::move(region));
}

core::ProfileSnapshot DebuggerView::GetProfileSnapshot(const core::ProfileQuery& query)
{
    core::ProfileSnapshot snap;
    snap.available = m_disasmView->HasProfileData();
    snap.haveSymbols = m_disasmView->HasProfileSymbols();
    snap.totalCycles = m_disasmView->GetProfileTotalCycles();
    m_disasmView->GetProfileCycleSplit(snap.resolvedCycles, snap.unresolvedCycles);
    snap.playheadLRef = m_scrubberView->GetScrubberLRef();

    auto cap = [&](std::vector<adm::core::CallFrame>& v) {
        if (query.limit > 0 && static_cast<int>(v.size()) > query.limit)
            v.resize(static_cast<size_t>(query.limit));
    };

    switch (query.view) {
        case core::ProfileView::Instructions:
            snap.instructions = m_disasmView->GetProfileInstructions(query.limit);
            break;
        case core::ProfileView::Functions:
            snap.functions = m_disasmView->GetProfileFunctions(query.limit);
            break;
        case core::ProfileView::Flat:
            snap.frames = m_scrubberView->GetFlatFrames();
            cap(snap.frames);
            break;
        case core::ProfileView::Flame:
            snap.frames = m_scrubberView->GetFlameFrames();
            cap(snap.frames);
            break;
        case core::ProfileView::CallStack: {
            // Both sources: the live DWARF unwind (available whenever the machine is
            // halted, trace or no trace) and the scrubber's reconstruction at the playhead.
            core::UnwindResult live;
            std::string region;
            if (ComputeLiveCallStack(live, region)) {
                snap.liveAvailable = true;
                snap.liveStop = core::DescribeUnwindStop(live, region);
                snap.liveStack = std::move(live.frames);
                if (query.limit > 0 && static_cast<int>(snap.liveStack.size()) > query.limit)
                    snap.liveStack.resize(static_cast<size_t>(query.limit));
            }
            snap.callStack = m_scrubberView->GetCallStackAt(snap.playheadLRef);
            break;
        }
    }
    return snap;
}

void DebuggerView::SetScrubberDmaData(const adm::AmigaEmulator::DmaSlotRecord* records,
                                      int hposCount, int vposCount)
{
    m_scrubberView->SetDmaRecordData(records, hposCount, vposCount);
}

void DebuggerView::SetScrubberBreakMarker(bool partial, uint32_t breakLRef)
{
    m_scrubberView->SetBreakMarker(partial, breakLRef);
}

void DebuggerView::SetScrubberOverlayContext(const core::BitplaneLayout& layout,
                                             const core::AmigaScreenMap& map)
{
    m_scrubberView->SetOverlayContext(layout, map);
}

bool* DebuggerView::GetCpuOpenPtr() { return m_cpuView->GetOpenPtr(); }
bool* DebuggerView::GetCustomOpenPtr() { return m_customView->GetOpenPtr(); }
bool* DebuggerView::GetDisassemblyOpenPtr() { return m_disasmView->GetOpenPtr(); }
bool* DebuggerView::GetProfileOpenPtr() { return m_disasmView->GetProfileOpenPtr(); }
bool* DebuggerView::GetCopperOpenPtr() { return m_copperView->GetOpenPtr(); }
bool* DebuggerView::GetMemoryOpenPtr() { return m_memoryView->GetOpenPtr(); }
bool* DebuggerView::GetDisplayOpenPtr() { return m_displayView->GetOpenPtr(); }
bool* DebuggerView::GetSpritesOpenPtr() { return m_spritesView->GetOpenPtr(); }
bool* DebuggerView::GetBitplanesOpenPtr() { return m_bitplanesView->GetOpenPtr(); }
bool* DebuggerView::GetBartmanOpenPtr() { return m_bartmanView->GetOpenPtr(); }
bool* DebuggerView::GetCIAOpenPtr() { return m_ciaView->GetOpenPtr(); }
bool* DebuggerView::GetAudioOpenPtr() { return m_audioView->GetOpenPtr(); }
bool* DebuggerView::GetScrubberOpenPtr() { return m_scrubberView->GetOpenPtr(); }
bool* DebuggerView::GetCallStackOpenPtr() { return m_callStackView->GetOpenPtr(); }
bool* DebuggerView::GetTrainerOpenPtr() { return m_trainerView->GetOpenPtr(); }

core::LockContext DebuggerView::MakeLockContext()
{
    core::LockContext ctx;
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu || !m_emulatorView->IsRunning())
        return ctx; // ctx.mem stays null, which every caller treats as "not now"

    BuildEmulatorMemoryAccess(m_lockMem, emu);
    ctx.mem = &m_lockMem;
    // Resolved fresh every time rather than cached: a program that has not loaded yet has
    // no seglist, and one that just did must not be resolved against the previous
    // program's bases.
    auto readLong = [this](uint32_t a, bool& ok) { return m_lockMem.ReadLong(a, &ok); };
    auto readByte = [this](uint32_t a, bool& ok) { return m_lockMem.ReadByte(a, &ok); };
    ctx.segments = core::SegListResolver::ResolveActiveProcess(readLong, readByte);
    return ctx;
}

bool DebuggerView::IsAnyWindowOpen()
{
    // The sub-views expose their flag as a pointer rather than a predicate, so this reads
    // through the same accessors the menu checkboxes bind to.
    bool* const flags[] = {
        GetCpuOpenPtr(),         GetCustomOpenPtr(),      GetDisassemblyOpenPtr(),
        GetProfileOpenPtr(),     GetCopperOpenPtr(),      GetMemoryOpenPtr(),
        GetDisplayOpenPtr(),     GetSpritesOpenPtr(),     GetBitplanesOpenPtr(),
        GetBartmanOpenPtr(),     GetCIAOpenPtr(),         GetAudioOpenPtr(),
        GetScrubberOpenPtr(),    GetCallStackOpenPtr(),   GetConsoleOpenPtr(),
        GetBreakpointsOpenPtr(), GetAnnotationsOpenPtr(), GetTrainerOpenPtr()};
    for (bool* f : flags) {
        if (f && *f)
            return true;
    }
    return false;
}

int DebuggerView::GetScrubberTickUnit() const { return m_scrubberView->GetTickUnit(); }
void DebuggerView::SetScrubberTickUnit(int unit) { m_scrubberView->SetTickUnit(unit); }
bool DebuggerView::GetScrubberShowInstrNumbers() const
{
    return m_scrubberView->GetShowInstrNumbers();
}
void DebuggerView::SetScrubberShowInstrNumbers(bool show)
{
    m_scrubberView->SetShowInstrNumbers(show);
}
int DebuggerView::GetScrubberHeatMap() const { return m_scrubberView->GetTopRowMode(); }
void DebuggerView::SetScrubberHeatMap(int mode) { m_scrubberView->SetTopRowMode(mode); }
float DebuggerView::GetSpritesScanlinesPanelWidth() const
{
    return m_spritesView->GetScanlinesPanelWidth();
}
void DebuggerView::SetSpritesScanlinesPanelWidth(float w)
{
    m_spritesView->SetScanlinesPanelWidth(w);
}
float DebuggerView::GetSpritesDetailPanelHeight() const
{
    return m_spritesView->GetDetailPanelHeight();
}
void DebuggerView::SetSpritesDetailPanelHeight(float h) { m_spritesView->SetDetailPanelHeight(h); }
int DebuggerView::GetMemoryBytesPerLine() const { return m_memoryView->GetBytesPerLine(); }
void DebuggerView::SetMemoryBytesPerLine(int bpl) { m_memoryView->SetBytesPerLine(bpl); }
bool* DebuggerView::GetConsoleOpenPtr() { return m_consoleView->GetOpenPtr(); }
bool* DebuggerView::GetBreakpointsOpenPtr() { return m_breakpointsView->GetOpenPtr(); }
bool* DebuggerView::GetAnnotationsOpenPtr() { return m_annotationsView->GetOpenPtr(); }

void DebuggerView::ToggleConsole() { m_consoleView->ToggleOpen(); }

void DebuggerView::RestoreWindows(bool cpu, bool custom, bool disasm, bool copper, bool memory,
                                  bool display, bool sprites, bool bitplanes, bool cia,
                                  bool scrubber, bool console, bool breakpoints, bool annotations,
                                  bool callstack, bool bartman, bool audio, bool profile,
                                  bool trainer)
{
    m_cpuView->Restore(cpu);
    m_customView->Restore(custom);
    m_disasmView->Restore(disasm);
    m_disasmView->RestoreProfile(profile);
    m_copperView->Restore(copper);
    m_memoryView->Restore(memory);
    m_displayView->Restore(display);
    m_spritesView->Restore(sprites);
    m_bitplanesView->Restore(bitplanes);
    m_ciaView->Restore(cia);
    if (scrubber)
        m_scrubberView->Restore();
    m_consoleView->Restore(console);
    m_breakpointsView->Restore(breakpoints);
    m_annotationsView->Restore(annotations);
    m_callStackView->Restore(callstack);
    m_trainerView->Restore(trainer);
    m_bartmanView->Restore(bartman);
    m_audioView->Restore(audio);
}

void DebuggerView::DrawSpritesOverlay(ImDrawList* dl, ImVec2 screenMin, ImVec2 screenMax)
{
    m_spritesView->DrawOverlay(dl, screenMin, screenMax);
}

void DebuggerView::DrawOverlays(ImDrawList* dl, ImVec2 screenMin, ImVec2 screenMax)
{
    m_displayView->DrawOverlays(dl, screenMin, screenMax);
}

bool DebuggerView::IsGeometryOverlayVisible() const
{
    return m_displayView->IsGeometryOverlayVisible();
}

const BlitOverlaySettings& DebuggerView::GetBlitOverlaySettings() const
{
    return m_displayView->GetBlitSettings();
}

void DebuggerView::SetBlitOverlaySettings(const BlitOverlaySettings& settings)
{
    m_displayView->SetBlitSettings(settings);
}

bool DebuggerView::IsBlitOverlayVisible() const { return m_displayView->IsBlitOverlayVisible(); }

void DebuggerView::SetBlitOverlayStats(int spans, int pixelBlits, int boxBlits, int unplacedBlits)
{
    m_displayView->SetBlitOverlayStats(spans, pixelBlits, boxBlits, unplacedBlits);
}

void DebuggerView::SetBlitCaptureDestData(bool on) { m_displayView->SetCaptureDestData(on); }

void DebuggerView::SetBlitCaptureDestDataSupported(bool supported)
{
    m_displayView->SetCaptureDestDataSupported(supported);
}

const RasterSettings& DebuggerView::GetRasterSettings() const
{
    return m_displayView->GetRasterSettings();
}

void DebuggerView::SetRasterSettings(const RasterSettings& settings)
{
    m_displayView->SetRasterSettings(settings);
}

bool DebuggerView::IsRasterOverlayVisible() const
{
    return m_displayView->IsRasterOverlayVisible();
}

void DebuggerView::SetRasterStats(int copperCells, int dmaCells)
{
    m_displayView->SetRasterStats(copperCells, dmaCells);
}

void DebuggerView::SetRasterGeometry(const adm::core::RasterFrameMap& map)
{
    m_displayView->SetRasterGeometry(map);
}

std::map<std::string, bool> DebuggerView::GetDisplaySectionStates() const
{
    return m_displayView->GetSectionStates();
}

void DebuggerView::SetDisplaySectionStates(const std::map<std::string, bool>& states)
{
    m_displayView->SetSectionStates(states);
}

uint32_t DebuggerView::GetScrubberLRef() const { return m_scrubberView->GetScrubberLRef(); }

void DebuggerView::JumpToCopperTrace(uint32_t lRef) { m_copperView->JumpToTraceLRef(lRef); }

void DebuggerView::NavigateToCustomRegister(uint32_t offset, uint16_t value)
{
    m_customView->NavigateToRegister(offset, true, value);
}

void DebuggerView::NavigateToDisassembly(uint32_t pc)
{
    *m_disasmView->GetOpenPtr() = true;
    m_disasmView->GoToAddress(pc);
}

void DebuggerView::SetBeamBreakAt(int vpos, int hpos)
{
    if (m_breakpointsView)
        m_breakpointsView->SetBeamBreak(vpos, hpos);
}

void DebuggerView::GetBeamBreak(int& outVpos, int& outHpos) const
{
    outVpos = m_breakpointsView ? m_breakpointsView->GetBeamVpos() : -1;
    outHpos = m_breakpointsView ? m_breakpointsView->GetBeamHpos() : -1;
}

void DebuggerView::RevealBreakpointsWindow()
{
    if (!m_breakpointsView)
        return;
    *m_breakpointsView->GetOpenPtr() = true;
    m_breakpointsView->RequestFocus();
}

bool DebuggerView::DrawCopperEventTooltip(int index, uint32_t lRef)
{
    return m_scrubberView->DrawCopperEventTooltip(index, lRef);
}

void DebuggerView::SetSpritesJumpMemory(std::function<void(uint32_t)> cb)
{
    m_spritesView->SetOnJumpToMemory(std::move(cb));
}

void DebuggerView::SetBitplanesJumpMemory(std::function<void(uint32_t)> cb)
{
    m_bitplanesView->SetOnJumpToMemory(std::move(cb));
}

void DebuggerView::SetBartmanJumpMemory(std::function<void(uint32_t)> cb)
{
    m_bartmanView->SetOnJumpToMemory(std::move(cb));
}

void DebuggerView::SetBartmanSupportChanged(std::function<void(bool)> cb)
{
    m_bartmanView->SetOnSupportChanged(std::move(cb));
}

core::BitplaneLayout DebuggerView::GetBitplaneLayout()
{
    return m_bitplanesView->GetBitplaneLayout();
}

void DebuggerView::SetActiveBlit(const core::BlitRecord* rec)
{
    m_bitplanesView->SetActiveBlit(rec);
}

void DebuggerView::SetMemorySearcher(core::MemorySearcher* searcher)
{
    m_memoryView->SetMemorySearcher(searcher);
}

void DebuggerView::ShowFindDialog() { m_memoryView->ShowFindDialog(); }

void DebuggerView::FindNext() { m_memoryView->FindNext(); }

void DebuggerView::FindPrevious() { m_memoryView->FindPrevious(); }

void DebuggerView::HighlightMemoryResult(uint32_t amigaAddr, size_t length)
{
    m_memoryView->HighlightMemoryResult(amigaAddr, length);
}

void DebuggerView::SetOnMemoryTabActive(std::function<void()> callback)
{
    m_memoryView->SetOnMemoryTabActive(std::move(callback));
}

void DebuggerView::SetOnLoadDocument(std::function<bool(const std::string&, bool)> cb)
{
    if (m_consoleView)
        m_consoleView->m_onLoadDocument = std::move(cb);
}

void DebuggerView::SetOnGetRecentFiles(std::function<std::vector<std::string>()> cb)
{
    if (m_consoleView)
        m_consoleView->m_onGetRecentFiles = std::move(cb);
}

void DebuggerView::SetStateSnapshotHooks(
    std::function<std::string(const std::string&, bool, std::string&)> save,
    std::function<std::string(const std::string&)> load,
    std::function<bool(const std::string&)> remove,
    std::function<std::vector<core::ArHost::StateEntry>()> list)
{
    if (!m_consoleView)
        return;
    m_consoleView->m_onSaveState = std::move(save);
    m_consoleView->m_onLoadState = std::move(load);
    m_consoleView->m_onDeleteState = std::move(remove);
    m_consoleView->m_onListStates = std::move(list);
}

void DebuggerView::SetScrubberFrameTimeline(bool enabled, bool recording, uint64_t oldest,
                                            uint64_t newest, uint64_t position)
{
    m_scrubberView->SetFrameTimeline(enabled, recording, oldest, newest, position);
}

void DebuggerView::SetOnScrubberSeekFrame(std::function<void(uint64_t)> cb)
{
    m_scrubberView->m_onSeekFrame = std::move(cb);
}

void DebuggerView::SetScrubberFrameLoop(bool active, uint64_t a, uint64_t b, bool playing, int mode,
                                        double speed, const std::string& stopReason)
{
    m_scrubberView->SetFrameTimelineState(active, a, b, playing, mode, speed, stopReason);
}

void DebuggerView::SetOnScrubberClearTimeline(std::function<void()> cb)
{
    m_scrubberView->m_onClearTimeline = std::move(cb);
}

void DebuggerView::SetOnScrubberSetRecording(std::function<void(bool)> cb)
{
    m_scrubberView->m_onSetRecording = std::move(cb);
}

void DebuggerView::SetOnScrubberTimelineTransport(std::function<void(bool)> play,
                                                  std::function<void(int)> step,
                                                  std::function<void(int)> mode,
                                                  std::function<void(double)> speed)
{
    m_scrubberView->m_onTimelinePlay = std::move(play);
    m_scrubberView->m_onTimelineStep = std::move(step);
    m_scrubberView->m_onTimelineMode = std::move(mode);
    m_scrubberView->m_onTimelineSpeed = std::move(speed);
}

void DebuggerView::SetOnScrubberSetFrameLoop(std::function<void(uint64_t, uint64_t)> cb)
{
    m_scrubberView->m_onSetFrameLoop = std::move(cb);
}

void DebuggerView::SetOnScrubberFrameThumbnail(
    std::function<bool(uint64_t, std::vector<uint8_t>&, int&, int&, uint64_t&)> cb)
{
    m_scrubberView->m_onGetFrameThumbnail = std::move(cb);
}

void DebuggerView::SetOnFrameHistoryClear(std::function<void()> cb)
{
    if (m_consoleView)
        m_consoleView->m_onFrameHistoryClear = std::move(cb);
}

void DebuggerView::SetFrameHistoryTransportHooks(std::function<void(bool)> play,
                                                 std::function<std::string(int)> step,
                                                 std::function<void(int)> mode,
                                                 std::function<void(double)> speed)
{
    if (!m_consoleView)
        return;
    m_consoleView->m_onFrameHistoryPlay = std::move(play);
    m_consoleView->m_onFrameHistoryStep = std::move(step);
    m_consoleView->m_onFrameHistoryPlayMode = std::move(mode);
    m_consoleView->m_onFrameHistoryPlaySpeed = std::move(speed);
}

void DebuggerView::SetFrameHistoryHooks(
    std::function<core::ArHost::FrameHistoryInfo()> info, std::function<std::string(uint64_t)> seek,
    std::function<std::string(uint64_t)> rewind,
    std::function<std::string(uint64_t, uint64_t)> loop,
    std::function<std::string(uint64_t, uint64_t, std::vector<core::FrameDiffRun>&, size_t, bool&)>
        diff)
{
    if (!m_consoleView)
        return;
    m_consoleView->m_onFrameHistoryInfo = std::move(info);
    m_consoleView->m_onFrameHistorySeek = std::move(seek);
    m_consoleView->m_onFrameHistoryRewind = std::move(rewind);
    m_consoleView->m_onFrameHistoryLoop = std::move(loop);
    m_consoleView->m_onFrameHistoryDiff = std::move(diff);
}

void DebuggerView::SetOnGetOpenDocuments(std::function<std::vector<std::string>()> cb)
{
    if (m_consoleView)
        m_consoleView->m_onGetOpenDocuments = std::move(cb);
}

void DebuggerView::SetOnGetOpenDocumentInfo(
    std::function<std::vector<core::ArHost::DocumentInfo>()> cb)
{
    if (m_consoleView)
        m_consoleView->m_onGetOpenDocumentInfo = std::move(cb);
}

void DebuggerView::SetHostMountCallbacks(
    std::function<std::array<core::HostDirectoryMount, 2>()> configured,
    std::function<std::array<core::HostDirectoryMount, 2>()> effective,
    std::function<bool(const core::HostDirectoryMount&, std::string&)> set,
    std::function<bool(std::string&)> apply, std::function<bool(core::HostMountDevice)> open,
    std::function<bool(core::HostMountDevice)> refresh)
{
    if (!m_consoleView)
        return;
    m_consoleView->m_onGetConfiguredHostMounts = std::move(configured);
    m_consoleView->m_onGetEffectiveHostMounts = std::move(effective);
    m_consoleView->m_onSetHostMount = std::move(set);
    m_consoleView->m_onApplyHostMounts = std::move(apply);
    m_consoleView->m_onOpenHostMount = std::move(open);
    m_consoleView->m_onRefreshHostMount = std::move(refresh);
}

void DebuggerView::SetHardDriveControllerRomsCallback(
    std::function<void(const core::HardDriveControllerRoms&)> set)
{
    if (!m_consoleView)
        return;
    m_consoleView->m_onSetHardDriveControllerRoms = std::move(set);
}

void DebuggerView::SetOnRunDocumentByIndex(std::function<void(int idx)> cb)
{
    if (m_consoleView)
        m_consoleView->m_onRunDocumentByIndex = std::move(cb);
}

void DebuggerView::SetOnCloseDocumentByIndex(std::function<void(int idx)> cb)
{
    if (m_consoleView)
        m_consoleView->m_onCloseDocumentByIndex = std::move(cb);
}

void DebuggerView::SetOnStartEmulator(std::function<void()> cb)
{
    if (m_consoleView)
        m_consoleView->m_onStartEmulator = std::move(cb);
}

void DebuggerView::SetOnStopEmulator(std::function<void()> cb)
{
    if (m_consoleView)
        m_consoleView->m_onStopEmulator = std::move(cb);
}

void DebuggerView::SetOnGetBlitLog(std::function<core::BlitLogSnapshot(int limit)> cb)
{
    if (m_consoleView)
        m_consoleView->m_onGetBlitLog = std::move(cb);
}

void DebuggerView::SetOnExocetSession(std::function<core::ExocetSession*()> cb)
{
    if (m_consoleView)
        m_consoleView->m_onExocetSession = std::move(cb);
}

void DebuggerView::SetOnLineCoverage(
    std::function<nlohmann::json(const std::string&, uint32_t, uint64_t)> cb)
{
    if (m_consoleView)
        m_consoleView->m_onLineCoverage = std::move(cb);
}

void DebuggerView::SetOnTraceHistory(std::function<bool()> isOn, std::function<void(bool)> setOn)
{
    if (!m_consoleView)
        return;
    m_consoleView->m_onIsTraceHistory = std::move(isOn);
    m_consoleView->m_onSetTraceHistory = std::move(setOn);
}

void DebuggerView::SetOnGetRegLog(std::function<core::RegLogSnapshot(int limit)> cb)
{
    if (m_consoleView)
        m_consoleView->m_onGetRegLog = std::move(cb);
}

void DebuggerView::SetOnGetDiskLog(
    std::function<core::DiskLogSnapshot(const core::DiskLogQuery&)> cb)
{
    if (m_consoleView)
        m_consoleView->m_onGetDiskLog = std::move(cb);
}

void DebuggerView::SetOnGetAudioStats(std::function<core::AudioLogSnapshot()> cb)
{
    if (m_audioView)
        m_audioView->SetOnGetAudioStats(cb);
    if (m_consoleView)
        m_consoleView->m_onGetAudioStats = std::move(cb);
}

void DebuggerView::WirePaulaState()
{
    // The console reads the same builder the Audio window uses, so PAULA and the window can never
    // disagree about the mask or the filter.
    if (m_consoleView)
        m_consoleView->m_onGetPaulaState = [this] { return BuildPaulaSnapshot(); };
}

core::PaulaStateSnapshot DebuggerView::BuildPaulaSnapshot() const
{
    if (!m_audioView)
        return {};
    return m_audioView->BuildSnapshot();
}

void DebuggerView::SetOnAudioRecord(
    std::function<core::AudioRecordResult(const core::AudioRecordRequest&)> cb)
{
    if (m_consoleView)
        m_consoleView->m_onAudioRecord = std::move(cb);
}

void DebuggerView::SetOnScenario(
    std::function<core::ScenarioResult(const core::ScenarioRequest&)> cb)
{
    if (m_consoleView)
        m_consoleView->m_onScenario = std::move(cb);
}

void DebuggerView::AddConsoleOutputLine(const std::string& line)
{
    if (m_consoleView) {
        m_consoleView->AddOutputLine(line);
    }
}

void DebuggerView::MarkMemoryRegionsDirty() { m_memoryView->MarkRegionsDirty(); }

void DebuggerView::NotifyDebugBreak()
{
    if (m_consoleView)
        m_consoleView->OnDebugBreak();
    // Snap the disassembly listing to the stopped PC so it shows where execution broke,
    // even if the user had navigated the listing elsewhere.
    if (m_disasmView)
        m_disasmView->RequestScrollToPc();
}

} // namespace adm::ui
