#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace adm {
namespace core {

// Local MCP transport for Claude Desktop integration. Serves a tiny JSON-over-HTTP API on
// 127.0.0.1 (the Python stdio bridge in mcp/ speaks MCP and forwards calls here):
//   GET  /tools  -> tool manifest (name/description/inputSchema), the single source of truth
//   POST /tool   -> { "name": "...", "args": {...} } executes a tool and returns its JSON result
//
// The emulator only runs on the main thread, so tool calls cannot touch it from the httplib
// worker threads. Each POST is queued as a request and the worker thread blocks on a future;
// Application::RunFrame() calls DrainPending() on the main thread to execute the tool and fulfil
// the promise. This generalizes PeekabooHttpServer's fire-and-forget queue into a round-trip.
class McpServer
{
public:
    using ToolDispatch =
        std::function<nlohmann::json(const std::string& name, const nlohmann::json& args)>;
    using ManifestFn = std::function<nlohmann::json()>;

    // A tool that has to wait for the emulator cannot block inside DrainPending: the main
    // thread it would block is the one that runs the emulator. Instead it returns a result
    // carrying this key set to true, which re-queues the request for the next frame while the
    // HTTP worker stays parked on its future. The tool is responsible for its own deadline —
    // it must eventually return something without the key, well inside kRequestTimeoutSeconds.
    static constexpr const char* kRetryKey = "__mcp_retry";

    McpServer();
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Manifest provider is invoked on the server thread, so it must be pure / thread-safe
    // (it reads a const tool table; it must not touch the emulator). Set before Start().
    void SetManifestProvider(ManifestFn fn) { m_manifest = std::move(fn); }

    // How long an HTTP worker waits for the main thread to fulfil its request. Bounds the
    // budget any kRetryKey-deferred tool may spend waiting.
    static constexpr int kRequestTimeoutSeconds = 10;

    void Start(int port, const std::string& token);
    void Stop();
    bool IsRunning() const { return m_running.load(); }
    int Port() const { return m_port; }

    // Called every frame on the main thread. Executes queued tool requests via dispatch (which
    // runs on the calling thread) and fulfils their promises.
    void DrainPending(const ToolDispatch& dispatch);

private:
    void ServerLoop();

    struct Request
    {
        std::string name;
        nlohmann::json args;
        std::promise<nlohmann::json> result;
    };

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    ManifestFn m_manifest;
    std::string m_token;
    int m_port = 0;
    std::atomic<bool> m_running{false};

    std::thread m_thread;
    std::mutex m_mutex;
    std::vector<std::shared_ptr<Request>> m_pending;
};

} // namespace core
} // namespace adm
