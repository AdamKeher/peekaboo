#include "DebugSession.h"

#include <fmt/format.h>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "../ui/views/BreakpointsView.h"
#include "../ui/views/DebuggerView.h"
#include "../ui/views/EmulatorHostHooks.h"
#include "../ui/views/EmulatorView.h"
#include "AmigaEmulator.h"
#include "ArMemoryAccess.h"
#include "CompiledProgram.h"
#include "CpuTraceHistory.h"
#include "FrameTimeline.h"
#include "Logger.h"

namespace adm::core {

namespace {
// How many frames to watch for a launched program's seglist before giving up. A CLI launch
// populates it within a frame or two; a program that took over the machine never will.
// Long enough to cover a Kickstart boot plus the CLI loading the program: measured at roughly
// ten seconds of wall clock on an A500 profile, so 120 frames (~2s) was far too short.
constexpr int kStopOnEntryFrameBudget = 1500;
// Instruction budget for the entry breakpoint. Generous: the CLI still has to return and jump
// to the code, and the one-shot check is a compare per instruction, not a debugger entry.
constexpr uint32_t kStopOnEntryInstructionBudget = 20'000'000;
std::string LowerCopy(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

} // namespace

void DebugSession::Init(ui::EmulatorView* emulatorView, ui::DebuggerView* debuggerView,
                        GdbServer* gdbServer, CpuTraceHistory* cpuTraceHistory,
                        FrameTimeline* timeline, CompiledProgram* compiledProgram)
{
    m_emulatorView = emulatorView;
    m_debuggerView = debuggerView;
    m_gdbServer = gdbServer;
    m_cpuTraceHistory = cpuTraceHistory;
    m_timeline = timeline;
    m_compiledProgram = compiledProgram;
}

ui::BreakpointsView* DebugSession::BreakpointsViewPtr()
{
    return m_debuggerView ? m_debuggerView->GetBreakpointsView() : nullptr;
}

void DebugSession::ArmStopOnEntry(const std::string& programPath)
{
    // Wait for Kickstart to finish booting, then break inside dos.library/LoadSeg. Halting
    // the machine at each step is what makes this reliable; see StopOnEntryPhase.
    m_stopOnEntryPhase = StopOnEntryPhase::WaitForCli;
    m_stopOnEntryFramesLeft = kStopOnEntryFrameBudget;
    m_stopOnEntryModuleAddr = 0;
    m_stopOnEntryWatchSlot = -1;
    m_stopOnEntryName = LowerCopy(std::filesystem::path(programPath).stem().string());
    Logger::Info("Debugger: stop-on-entry armed for \"{}\"", m_stopOnEntryName);
}

void DebugSession::AbandonStopOnEntry(const std::string& why)
{
    if (m_stopOnEntryPhase == StopOnEntryPhase::Idle)
        return;
    m_stopOnEntryPhase = StopOnEntryPhase::Idle;
    if (auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr)
        emu->CancelOneShotBreakpoint();
    if (m_stopOnEntryWatchSlot >= 0) {
        if (auto* bps = BreakpointsViewPtr())
            bps->RemoveWatchpointSlot(m_stopOnEntryWatchSlot);
        m_stopOnEntryWatchSlot = -1;
    }
    Logger::Warn("Debugger: stop-on-entry abandoned - {}", why);
}

bool DebugSession::ConsumeStopOnEntryWatch()
{
    if (m_stopOnEntryPhase != StopOnEntryPhase::WaitModuleWrite)
        return false;
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu)
        return false;

    AmigaEmulator::MemwatchHit hit{};
    if (!emu->GetLastMemwatchHit(hit) || hit.addr < m_stopOnEntryModuleAddr ||
        hit.addr >= m_stopOnEntryModuleAddr + 4)
        return false; // some other watchpoint

    // The value written is the seglist BPTR -- but do not take it from the watchpoint. A 68020
    // splits the longword store, so the hit reports only part of it (measured: $000032D9 where
    // the real value was $003032D9, entry $0000CB68 instead of $00C0CB68). Read the longword
    // back instead: the break is delivered at end-of-frame, by which point the storing
    // instruction has completed and memory holds the whole value. hit.val is kept only as a
    // fallback for a core that cannot serve the read.
    core::ArMemoryAccess mem;
    ui::BuildEmulatorMemoryAccess(mem, emu);
    bool readOk = false;
    const uint32_t fromMemory = mem.ReadLong(m_stopOnEntryModuleAddr, &readOk);
    const uint32_t bptr = readOk ? fromMemory : hit.val;
    if (m_stopOnEntryWatchSlot >= 0) {
        if (auto* bps = BreakpointsViewPtr())
            bps->RemoveWatchpointSlot(m_stopOnEntryWatchSlot);
        m_stopOnEntryWatchSlot = -1;
    }
    if (bptr == 0) {
        // The CLI clears cli_Module when a command finishes; that is not our load.
        AbandonStopOnEntry("cli_Module was cleared rather than set (the command exited)");
        return true;
    }

    const uint32_t entry = (bptr << 2) + 4;
    // A BPTR that did not survive the read produces an address in no RAM at all, and arming a
    // breakpoint there would just time out later with a misleading complaint about the program.
    bool entryMapped = false;
    for (const auto& region : emu->GetMemoryRegions()) {
        if (entry >= region.amigaAddr && entry < region.amigaAddr + region.size) {
            entryMapped = true;
            break;
        }
    }
    if (!entryMapped) {
        AbandonStopOnEntry(fmt::format("the seglist pointer did not resolve to mapped memory "
                                       "(cli_Module=${:08X}, entry would be ${:08X})",
                                       bptr, entry));
        return true;
    }
    if (!emu->ArmOneShotBreakpoint(entry, kStopOnEntryInstructionBudget)) {
        AbandonStopOnEntry("the entry breakpoint could not be armed");
        return true;
    }
    m_stopOnEntryPhase = StopOnEntryPhase::WaitEntry;
    Logger::Info("Debugger: stop-on-entry saw the seglist, will halt at ${:08X}", entry);
    return true; // swallow: this stop was ours, and the machine should keep running to the entry
}

void DebugSession::ServiceStopOnEntry()
{
    if (m_stopOnEntryPhase == StopOnEntryPhase::Idle)
        return;
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu || !emu->HasOneShotBreakpoint()) {
        AbandonStopOnEntry("this core has no one-shot breakpoint support");
        return;
    }
    if (--m_stopOnEntryFramesLeft <= 0) {
        // Distinguish "the program did something we did not expect" from "this core cannot do
        // this on this CPU model". The one-shot check lives in the CPU interpreter loop, and
        // only some loops carry it (see PATCHES.md); on an unhooked loop it stays Armed with
        // its instruction counter stuck at zero, which is a reliable signature. Blaming the
        // program for that would send someone hunting a bug that is not there.
        uint32_t executed = 0;
        const bool armed = m_stopOnEntryPhase == StopOnEntryPhase::WaitEntry &&
                           emu->GetOneShotBreakpointStatus(nullptr, &executed) ==
                               AmigaEmulator::OneShotBreakpointStatus::Armed;
        if (armed && executed == 0) {
            AbandonStopOnEntry("the one-shot breakpoint never advanced, so this CPU model's "
                               "interpreter loop has no ADM hook. Breakpoints and watchpoints "
                               "still work; stop-on-entry and the CPU trace do not. See "
                               "PATCHES.md, \"CPU-loop hooks and CPU-model coverage\".");
            return;
        }
        AbandonStopOnEntry("the program never reached its entry point in time (did it take over "
                           "the machine, or bypass the CLI?)");
        return;
    }

    if (m_stopOnEntryPhase == StopOnEntryPhase::WaitForCli) {
        // Walk to the running CLI. Every offset here is AmigaOS ABI and version-independent;
        // SegListResolver walks the same chain.
        core::ArMemoryAccess mem;
        ui::BuildEmulatorMemoryAccess(mem, emu);
        bool ok = false;
        const uint32_t execBase = mem.ReadLong(4, &ok);
        if (!ok || execBase == 0)
            return;
        const uint32_t task = mem.ReadLong(execBase + 276, &ok); // ExecBase->ThisTask
        if (!ok || task == 0)
            return;
        if (mem.ReadByte(task + 8, &ok) != 13 || !ok)
            return;                                             // NT_PROCESS: not a DOS process yet
        const uint32_t cliBptr = mem.ReadLong(task + 172, &ok); // pr_CLI
        if (!ok || cliBptr == 0)
            return;

        const uint32_t moduleAddr = (cliBptr << 2) + 60; // &cli_Module
        core::WatchpointSpec spec;
        spec.addr = moduleAddr;
        spec.len = 4;
        spec.write = true;
        spec.read = false;
        spec.source = "CPU";
        std::string err;
        auto* bps = BreakpointsViewPtr();
        const int slot = bps ? bps->AddWatchpoint(spec, err) : -1;
        if (slot < 0) {
            AbandonStopOnEntry("no free watchpoint slot for the cli_Module trap: " + err);
            return;
        }
        m_stopOnEntryModuleAddr = moduleAddr;
        m_stopOnEntryWatchSlot = slot;
        m_stopOnEntryPhase = StopOnEntryPhase::WaitModuleWrite;
        Logger::Info("Debugger: stop-on-entry watching cli_Module at ${:08X}", moduleAddr);
        return;
    }

    if (m_stopOnEntryPhase != StopOnEntryPhase::WaitEntry)
        return; // WaitModuleWrite is driven by the watchpoint, not by polling

    uint32_t stopPc = 0;
    const auto status = emu->GetOneShotBreakpointStatus(&stopPc, nullptr);
    if (status == AmigaEmulator::OneShotBreakpointStatus::BudgetExpired) {
        AbandonStopOnEntry("the instruction budget ran out before the entry point was reached");
        return;
    }
    if (status != AmigaEmulator::OneShotBreakpointStatus::Hit)
        return;

    // Halted before the program's first instruction executes. Pause for real so it does not run
    // on while a debugger attaches, and relocate now that the seglist exists.
    m_stopOnEntryPhase = StopOnEntryPhase::Idle;
    if (m_emulatorView && !m_emulatorView->IsPaused())
        m_emulatorView->TogglePause();
    std::string err;
    if (m_compiledProgram && m_compiledProgram->RelocateHunkMap(err))
        Logger::Info("Debugger: stopped at entry ${:08X}, {} sections relocated", stopPc,
                     m_compiledProgram->HunkMap().Sections().size());
    else
        Logger::Info("Debugger: stopped at entry ${:08X} (relocation unavailable: {})", stopPc,
                     err);
    if (m_debuggerView)
        m_debuggerView->NotifyDebugBreak();
    m_gdbServer->NotifyStopped(core::GdbPacketHandler::StopReason::Trap);
}

bool DebugSession::PrepareTimeTravel(std::string& outError)
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu) {
        outError = "no emulator";
        return false;
    }
    if (!emu->HasDebugCommand()) {
        outError = "this core cannot step instructions";
        return false;
    }
    // Reverse debugging drives the machine itself, restoring states and replaying
    // instructions. Two things steering the same machine is one too many, so the timeline
    // transport lets go before it starts.
    m_timeline->ReleaseMachine();

    if (!m_timeTravelReady) {
        core::TimeTravel::Deps deps;
        deps.restoreFrame = [this](uint64_t frame) { return m_timeline->SeekTo(frame).empty(); };
        deps.recordedRange = [this](uint64_t& oldest, uint64_t& newest) {
            return m_timeline->History().Range(oldest, newest);
        };
        deps.replayInstructions = [emu](uint32_t n) {
            // UAE's `t <n>` runs n instructions inside one retro_run(), which is what keeps a
            // replay affordable; stepping one at a time would cost an emulator entry each.
            std::string out;
            if (!emu->ExecuteDebugCommand(fmt::format("t {}", n), out))
                return false;
            emu->RunForStep();
            return true;
        };
        deps.currentPc = [emu]() -> uint32_t {
            AmigaEmulator::CpuState cpu{};
            return emu->GetCpuState(cpu) ? cpu.pc : 0u;
        };
        // One frame's worth of PCs, cached. A query per lookup would run a full trace scan for
        // every instruction stepped, which is slow enough to wedge the emulator rather than
        // merely feel sluggish.
        auto framePcs = [this](uint64_t frame) -> const std::vector<uint32_t>& {
            static std::vector<uint32_t> empty;
            if (m_timeTravelCacheFrame == frame && !m_timeTravelFramePcs.empty())
                return m_timeTravelFramePcs;
            core::CpuTraceQuery q;
            q.mode = core::TraceQueryMode::Filter;
            q.haveFrame = true;
            q.frame = frame;
            q.limit = 0;
            const core::CpuTraceSnapshot snap = m_cpuTraceHistory->Query(q);
            if (!snap.available)
                return empty;
            m_timeTravelFramePcs.clear();
            m_timeTravelFramePcs.reserve(snap.matches.size());
            for (const auto& m : snap.matches)
                m_timeTravelFramePcs.push_back(m.record.pc);
            m_timeTravelCacheFrame = frame;
            return m_timeTravelFramePcs;
        };
        deps.recordedPc = [framePcs](uint64_t frame, uint32_t index, uint32_t& outPc) {
            const auto& pcs = framePcs(frame);
            if (index >= pcs.size())
                return false;
            outPc = pcs[index];
            return true;
        };
        deps.frameInstructionCount = [framePcs](uint64_t frame, uint32_t& outCount) {
            const auto& pcs = framePcs(frame);
            if (pcs.empty())
                return false;
            outCount = static_cast<uint32_t>(pcs.size());
            return true;
        };
        deps.saveState = [emu] { return emu->SaveStateForRestore(); };
        deps.loadState = [emu](const std::vector<uint8_t>& blob) { return emu->LoadState(blob); };
        m_timeTravel.SetDeps(std::move(deps));
        m_timeTravelReady = true;
    }

    const auto reach = m_timeTravel.Reachable();
    if (!reach.available) {
        outError = reach.hint;
        return false;
    }
    // Anchor to where the machine actually is. This has to be derived, not assumed: RSP has no
    // notion of a position, so "step back" is only meaningful once we know which recorded
    // instruction the CPU is currently sitting on. Guessing index 0 while the machine is
    // mid-frame makes every subsequent step back replay to the wrong place -- observed
    // reverse-stepi landing on the instruction *after* the current one.
    if (m_timePosition.frame == 0) {
        AmigaEmulator::CpuState cpu{};
        if (!emu->GetCpuState(cpu)) {
            outError = "cannot read the CPU state to anchor reverse execution";
            return false;
        }
        const uint64_t frame = emu->GetTotalFrameCount();

        core::CpuTraceQuery q;
        q.mode = core::TraceQueryMode::Filter;
        q.haveFrame = true;
        q.frame = frame;
        q.limit = 0;
        const core::CpuTraceSnapshot snap = m_cpuTraceHistory->Query(q);
        if (!snap.available || snap.matches.empty()) {
            outError = "the CPU trace has no record of the current frame, so there is no "
                       "position to step back from (enable the CPU trace and let a frame run)";
            return false;
        }
        // The last recorded instruction whose PC matches where the CPU is now. Searching from
        // the end matters: a PC can recur many times in one frame, and the current one is the
        // most recent.
        size_t index = snap.matches.size();
        for (size_t i = snap.matches.size(); i-- > 0;) {
            if (snap.matches[i].record.pc == cpu.pc) {
                index = i;
                break;
            }
        }
        if (index >= snap.matches.size()) {
            outError = fmt::format("the CPU is at ${:08X}, which does not appear in the recorded "
                                   "trace for frame {}; reverse execution cannot be anchored",
                                   cpu.pc, frame);
            return false;
        }
        m_timePosition.frame = frame;
        m_timePosition.index = static_cast<uint32_t>(index);
        m_timePosition.pc = cpu.pc;
    }
    return true;
}

bool DebugSession::StepBack(std::string& outError)
{
    return m_timeTravel.StepBack(m_timePosition, outError);
}

bool DebugSession::ReverseContinue(const std::vector<uint32_t>& addresses, std::string& outError)
{
    return m_timeTravel.ReverseContinue(addresses, m_timePosition, outError);
}

// Work out why the machine stopped, for GDB's T packet. Uses only the non-destructive
// accessors: IsDebugBreakPending() is read-and-clear and has already been consumed by
// EmulatorView::HandlePostStepStop by the time this runs.
GdbPacketHandler::StopReason DebugSession::ClassifyGdbStop(uint32_t* outAddr)
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (emu) {
        AmigaEmulator::MemwatchHit hit{};
        if (emu->GetLastMemwatchHit(hit)) {
            if (outAddr)
                *outAddr = hit.addr;
            const bool read = (hit.rwi & 1) != 0;
            const bool write = (hit.rwi & 2) != 0;
            if (read && write)
                return GdbPacketHandler::StopReason::AccessWatch;
            if (read)
                return GdbPacketHandler::StopReason::ReadWatch;
            if (write)
                return GdbPacketHandler::StopReason::WriteWatch;
        }
    }
    // Anything else -- a PC breakpoint, a step, a manual pause -- is a plain trap. Reporting
    // swbreak would tell GDB the PC sits exactly on a breakpoint it placed, which is only
    // reliably true for a bpnode hit, so the conservative answer is the correct one here.
    return GdbPacketHandler::StopReason::Trap;
}

} // namespace adm::core
