#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include "PeekabooEvents.h"

namespace adm {
namespace core {

struct PeekabooRunRequest
{
    // Which run this is. Readiness is only believable when it refers to the launch the caller
    // asked for: /api/run returns before the main thread has even seen the request, so without
    // this a debugger polling immediately after POSTing reads the PREVIOUS program's "loaded"
    // and attaches -- halting the machine in the middle of the reload it just asked for.
    uint64_t generation = 0;
    std::string program;
    bool focus = true;
    // Halt at the program's entry point instead of letting it run, so a debugger can attach and
    // set breakpoints before any user code executes.
    bool stopOnEntry = false;
};

// What the VS Code integration needs to know before it attaches a debugger: whether the program
// it asked for has actually been loaded by AmigaDOS yet. Published by the main thread each frame
// and read by the server thread, so it must stay a plain snapshot.
struct PeekabooRunState
{
    // A section map is loaded, i.e. Peekaboo knows which ELF the running program came from.
    bool sectionMap = false;
    // The program is in memory: its seglist was found and matches the section map. This is the
    // signal a debugger should wait for -- attaching earlier halts the machine mid-boot, and a
    // halted machine never finishes loading the program it is waiting for.
    bool programLoaded = false;
    int segments = 0;
    bool paused = false;
    bool running = false;
    // The run this state describes. Compared against the newest queued run before reporting
    // programLoaded, so a stale "yes" from the previous launch is never handed out.
    uint64_t generation = 0;
};

// Listens on localhost:7474 for requests from Peekaboo integrations. POST /api/load accepts
// remote releases from the Chrome extension; POST /api/run accepts a local Amiga executable
// from the VS Code integration. Thread-safe: events are queued from the server thread and
// dispatched on the main thread via the PopPending* methods called from Application::RunFrame().
class PeekabooHttpServer
{
public:
    static constexpr int kPort = 7474;

    explicit PeekabooHttpServer(int port = kPort);
    ~PeekabooHttpServer();

    PeekabooHttpServer(const PeekabooHttpServer&) = delete;
    PeekabooHttpServer& operator=(const PeekabooHttpServer&) = delete;

    void Start();
    void Stop();

    std::optional<PeekabooLoadEvent> PopPendingLoad();
    std::optional<PeekabooRunRequest> PopPendingRun();

    // Called from the main thread once per frame; read by the server thread in /api/status.
    void PublishRunState(const PeekabooRunState& state);

private:
    void ServerLoop();

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    std::thread m_thread;
    int m_port = kPort;
    std::mutex m_mutex;
    std::optional<PeekabooLoadEvent> m_pending;
    std::optional<PeekabooRunRequest> m_pendingRun;
    PeekabooRunState m_runState;
    uint64_t m_runGeneration = 0;
};

} // namespace core
} // namespace adm
