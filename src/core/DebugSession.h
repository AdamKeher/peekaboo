#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "GdbServer.h"
#include "TimeTravel.h"

namespace adm::ui {
class BreakpointsView;
class DebuggerView;
class EmulatorView;
} // namespace adm::ui

namespace adm::core {

class CompiledProgram;
class CpuTraceHistory;
class FrameTimeline;

/**
 * @brief State a GDB debug session carries that is not the wire protocol.
 *
 * Two features, both of which outlive any single RSP packet and neither of which GdbServer can
 * hold itself: the stop-at-entry handshake that halts a launched program before its first
 * instruction, and the reverse-execution position that gives "step back" something to step back
 * from.
 *
 * Deliberately not the GDB server, and deliberately not its wiring. SetupGdbServer stays on
 * Application because it binds ~18 ops hooks reaching most of the app; what moved here is the
 * state those hooks read and write.
 *
 * Extracted from Application.
 */
class DebugSession
{
public:
    /**
     * @brief Supply the collaborators the session drives.
     *
     * All are owned by Application and outlive this object. The two views may be null - the
     * moved code already tested for that, since Application holds them by unique_ptr and
     * releases them at shutdown. The other three are always present.
     */
    void Init(ui::EmulatorView* emulatorView, ui::DebuggerView* debuggerView, GdbServer* gdbServer,
              CpuTraceHistory* cpuTraceHistory, FrameTimeline* timeline,
              CompiledProgram* compiledProgram);

    // --- stop on entry ----------------------------------------------------------------

    /**
     * @brief Arm the stop-at-entry handshake for a launched program.
     *
     * @param programPath Host path of the program; its stem is what the log names.
     */
    void ArmStopOnEntry(const std::string& programPath);

    /// Advance the handshake by one frame. Cheap no-op while idle; call it every frame.
    void ServiceStopOnEntry();

    /// Handles the cli_Module watchpoint firing. Returns true to swallow the break and keep
    /// running, which is what the debug-break filter wants.
    bool ConsumeStopOnEntryWatch();

    void AbandonStopOnEntry(const std::string& why);

    // --- reverse execution ------------------------------------------------------------

    /// Binds TimeTravel to the live emulator and syncs the position to where it is now.
    bool PrepareTimeTravel(std::string& outError);

    /// Step back one recorded instruction from the current position.
    bool StepBack(std::string& outError);

    /// Run backwards until one of @p addresses is reached.
    bool ReverseContinue(const std::vector<uint32_t>& addresses, std::string& outError);

    // --- stop reporting ---------------------------------------------------------------

    /// Why the machine is halted, in the terms GDB's stop-reply packet understands.
    GdbPacketHandler::StopReason ClassifyGdbStop(uint32_t* outAddr = nullptr);

private:
    /**
     * @brief Phases of the stop-at-entry handshake.
     *
     * Polling for the program's entry point cannot work: whichever thing is polled for -- the
     * seglist, or cli_Module changing -- only becomes visible after LoadSeg returns, and the CLI
     * jumps to the code microseconds of emulated time later, far below the frame granularity
     * available to poll at. The machine has to be *halted* at the decisive moment instead.
     *
     * The decisive moment is the write of the seglist BPTR into the running CLI's cli_Module.
     * A memory watchpoint stops the machine on that write, which happens before the jump, so
     * the value can be read and a breakpoint placed on the entry point with time to spare.
     *
     * Everything this relies on -- ExecBase->ThisTask, pr_CLI, cli_Module -- is fixed by the
     * AmigaOS ABI and identical on 1.3, 2.0, 3.1 and beyond, and none of it touches the
     * chipset, so OCS/ECS/AGA make no difference. Breaking on dos.library/LoadSeg's jump-table
     * vector was tried first and measured never to fire: the CLI reaches the loader by an
     * internal path, so the public vector is never executed.
     */
    enum class StopOnEntryPhase {
        Idle,
        WaitForCli,      // no AmigaDOS CLI yet: Kickstart still booting
        WaitModuleWrite, // watchpoint armed on &cli_Module, waiting for the seglist BPTR
        WaitEntry,       // one-shot armed on the program's first instruction
    };

    ui::BreakpointsView* BreakpointsViewPtr();

    ui::EmulatorView* m_emulatorView = nullptr;
    ui::DebuggerView* m_debuggerView = nullptr;
    GdbServer* m_gdbServer = nullptr;
    CpuTraceHistory* m_cpuTraceHistory = nullptr;
    FrameTimeline* m_timeline = nullptr;
    // Relocating the hunk map at the entry stop is the compiled target's business, not ours.
    CompiledProgram* m_compiledProgram = nullptr;

    // Stop-on-entry: armed by a debug launch, cleared once the entry breakpoint is placed or
    // the attempt is abandoned. Frames are counted so a program that never produces a seglist
    // (no CLI, took over the machine) gives up instead of polling forever.
    StopOnEntryPhase m_stopOnEntryPhase = StopOnEntryPhase::Idle;
    int m_stopOnEntryFramesLeft = 0;
    uint32_t m_stopOnEntryModuleAddr = 0; // &cli_Module of the CLI that will run the program
    int m_stopOnEntryWatchSlot = -1;
    std::string m_stopOnEntryName; // basename we are waiting for, lowercased

    // Reverse execution for the GDB server. Position is tracked here because RSP has no notion
    // of one: gdb just says "go back one" and expects the target to know where it is.
    TimeTravel m_timeTravel;
    TimePosition m_timePosition;
    bool m_timeTravelReady = false;
    // One frame of recorded PCs, cached for the reverse-execution lookups.
    std::vector<uint32_t> m_timeTravelFramePcs;
    uint64_t m_timeTravelCacheFrame = UINT64_MAX;
};

} // namespace adm::core
