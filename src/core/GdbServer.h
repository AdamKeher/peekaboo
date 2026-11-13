#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "GdbPacketHandler.h"
#include "GdbTargetOps.h"

namespace adm::core {

/**
 * @brief A GDB remote-serial-protocol server, so a real m68k-amiga-elf-gdb can debug the
 *        emulated program while Peekaboo keeps its own debugger windows live.
 *
 * Host-side on purpose. Everything the protocol needs is already resolved in the app --
 * registers, the memory banks, the seglist-derived load bases, the debug-break callback -- and
 * relocation in particular *cannot* be done inside the PuAE core, which has no ELF reader and no
 * seglist walker. Keeping it here also means no core rebuild per iteration.
 *
 * Threading. The emulator is strictly main-thread, and vsync pins the main loop to display
 * refresh (~6-16 ms per turn). A naive "one packet per frame" handoff would make a single GDB
 * stop -- which issues dozens to hundreds of memory reads while unwinding -- take seconds. So
 * packets are split:
 *
 *  - **Read-only** packets (g, p, m, q*, ?, H) are served directly on the socket thread. While
 *    the target is halted the emulator is not running (EmulatorView::StepEmulator early-returns
 *    when paused), so nothing is mutating the banks and reading them is safe and fast.
 *  - **State-mutating** packets (G, P, M, X, qRcmd, and the run-control packets that arrive in
 *    Stage 3) are queued and executed by Pump() on the main thread, exactly like McpServer.
 *
 * Ownership. The server is an *observer* of the emulator's paused state, never its owner. There
 * is one m_isPaused and one ArmDebugTrace() re-arm path, both owned by EmulatorView. If the user
 * pauses from Peekaboo's own toolbar, GDB is told via NotifyStopped(); the server never fights
 * the UI for control.
 */
class GdbServer
{
public:
    /// Default port. Deliberately NOT 2345: Bartman's adapter and his savestate editor both
    /// hardcode that, and connecting to the wrong emulator is a confusing failure.
    static constexpr int kDefaultPort = 2346;

    /// How long the socket thread will wait for the main thread to service a queued packet.
    static constexpr int kRequestTimeoutSeconds = 10;

    GdbServer();
    ~GdbServer();

    GdbServer(const GdbServer&) = delete;
    GdbServer& operator=(const GdbServer&) = delete;

    /**
     * @brief Supply the hooks the protocol runs against. Call before Start().
     *
     * Read-only hooks may be invoked from the socket thread (only while the target is halted);
     * state-mutating hooks are only ever invoked from Pump() on the main thread.
     */
    void SetTargetOps(GdbTargetOps ops);

    /// Begin listening on 127.0.0.1:port. Safe to call when already running (no-op).
    bool Start(int port = kDefaultPort);
    void Stop();

    [[nodiscard]] bool IsRunning() const { return m_running.load(); }
    [[nodiscard]] bool IsClientConnected() const { return m_clientConnected.load(); }
    [[nodiscard]] int Port() const { return m_port; }
    /// Last error from Start(), for surfacing in the UI.
    [[nodiscard]] std::string LastError() const;

    /**
     * @brief Service queued packets on the main thread. Call once per frame from
     *        Application::RunFrame().
     *
     * Returns immediately when no client is connected and nothing is queued. When the target is
     * halted it will keep servicing packets until the queue goes quiet or `budget` is spent, so
     * a burst of reads completes within a single host frame instead of one packet per frame.
     * While the target is running the budget is ignored and the queue is drained without
     * lingering, so the emulator is not stalled.
     */
    void Pump(std::chrono::microseconds budget = std::chrono::milliseconds(6));

    /**
     * @brief Tell an attached GDB that the target stopped.
     *
     * Call from the debug-break callback. GDB is waiting for an unsolicited stop reply after
     * every resume, so this is what ends that wait; without it a `continue` hangs forever. Safe
     * to call when nothing is attached (it does nothing) and when the stop came from Peekaboo's
     * own UI rather than from GDB -- that is exactly the case the two-masters rule exists for.
     */
    void NotifyStopped(GdbPacketHandler::StopReason reason, uint32_t addr = 0);

    /// Forward target output (KPrintF/serial) to GDB as an O packet, for the Debug Console.
    void NotifyOutput(std::string_view text);

    /// True between a resume and the stop reply that answers it.
    [[nodiscard]] bool IsTargetResumed() const { return m_targetResumed.load(); }

private:
    void ServerLoop();
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_clientConnected{false};
    std::atomic<bool> m_stopping{false};
    // Set when a c/s/vCont reply handed control back, cleared by the stop reply that answers it.
    // Guards against emitting a stop reply GDB never asked for, which desynchronises the session.
    std::atomic<bool> m_targetResumed{false};
    int m_port = kDefaultPort;

    mutable std::mutex m_errorMutex;
    std::string m_lastError;
};

} // namespace adm::core
