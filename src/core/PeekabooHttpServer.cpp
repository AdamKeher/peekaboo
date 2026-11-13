#include "PeekabooHttpServer.h"

#include <httplib.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include "Logger.h"

namespace adm {
namespace core {

struct PeekabooHttpServer::Impl
{
    httplib::Server server;
};

PeekabooHttpServer::PeekabooHttpServer(int port) : m_impl(std::make_unique<Impl>()), m_port(port) {}

PeekabooHttpServer::~PeekabooHttpServer() { Stop(); }

// Origins of the sites the Peekaboo Chrome extension injects content scripts into
// (see chrome-extension/manifest.json). Keep in sync with that file.
static constexpr std::array<const char*, 3> kAllowedOrigins = {
    "https://demozoo.org",
    "https://www.pouet.net",
    "https://janeway.exotica.org.uk",
};

static void AddCorsHeaders(const httplib::Request& req, httplib::Response& res)
{
    // The extension's background service worker reaches us with host-permission privileges
    // and is not subject to CORS, so it keeps working regardless. The allow-origin echo below
    // only controls which pages may READ our responses; it does not stop a cross-origin POST
    // from executing, so each side-effecting endpoint enforces its own Origin check.
    std::string origin = req.get_header_value("Origin");
    if (!origin.empty() && std::find(kAllowedOrigins.begin(), kAllowedOrigins.end(), origin) !=
                               kAllowedOrigins.end()) {
        res.set_header("Access-Control-Allow-Origin", origin);
        res.set_header("Vary", "Origin");
    }
    res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

void PeekabooHttpServer::Start()
{
    auto& srv = m_impl->server;

    srv.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        PeekabooRunState state;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            state = m_runState;
            // Readiness from an older run is worse than no readiness at all: a debugger that
            // believes it would attach mid-reload and halt the machine before the program it
            // is waiting for can load. /api/run returns before the main thread has serviced the
            // request, so this window is the normal case, not a rare race.
            if (state.generation != m_runGeneration) {
                state.programLoaded = false;
                state.segments = 0;
            }
        }
        // programLoaded is what the VS Code integration waits for before attaching gdb: the
        // debugger's first act is to halt the machine, and halting it mid-boot means the program
        // it is waiting for can never finish loading.
        nlohmann::json body = {
            {"status", "ok"},
            {"application", "Peekaboo"},
            {"sectionMap", state.sectionMap},
            {"programLoaded", state.programLoaded},
            {"segments", state.segments},
            {"paused", state.paused},
            {"running", state.running},
        };
        res.set_content(body.dump(), "application/json");
    });

    srv.Options("/api/load", [](const httplib::Request& req, httplib::Response& res) {
        AddCorsHeaders(req, res);
        res.status = 204;
    });

    srv.Post("/api/load", [this](const httplib::Request& req, httplib::Response& res) {
        AddCorsHeaders(req, res);

        // CORS only blocks the response READ; a cross-origin "simple" POST (e.g. text/plain)
        // still executes server-side, so the load side effect must be gated here. Allowed:
        // the extension's background worker (chrome-extension:// origin), content scripts on
        // the known demoscene sites, and local tools that send no Origin at all.
        const std::string origin = req.get_header_value("Origin");
        if (!origin.empty() && origin.rfind("chrome-extension://", 0) != 0 &&
            std::find(kAllowedOrigins.begin(), kAllowedOrigins.end(), origin) ==
                kAllowedOrigins.end()) {
            res.status = 403;
            res.set_content(R"({"error":"origin not allowed"})", "application/json");
            return;
        }

        const std::string contentType = req.get_header_value("Content-Type");
        if (contentType.rfind("application/json", 0) != 0) {
            res.status = 415;
            res.set_content(R"({"error":"Content-Type must be application/json"})",
                            "application/json");
            return;
        }

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& e) {
            Logger::Error("PeekabooHttpServer: invalid JSON — {}", e.what());
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON"})", "application/json");
            return;
        }

        std::string fileUrl = body.value("fileUrl", std::string{});
        if (fileUrl.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"fileUrl required"})", "application/json");
            return;
        }

        PeekabooLoadEvent ev;
        ev.fileUrl = fileUrl;
        ev.releaseName = body.value("releaseName", std::string{});
        ev.groupName = body.value("groupName", std::string{});
        ev.fileType = body.value("fileType", std::string{"diskImage"});

        if (body.contains("demozooId") && body["demozooId"].is_number_integer()) {
            ev.releaseId = body["demozooId"].get<int>();
            ev.source = "demozoo";
        } else if (body.contains("pouetId") && body["pouetId"].is_number_integer()) {
            ev.releaseId = body["pouetId"].get<int>();
            ev.source = "pouet";
        } else if (body.contains("janewayId") && body["janewayId"].is_number_integer()) {
            ev.releaseId = body["janewayId"].get<int>();
            ev.source = "janeway";
        }
        // Derive filename from the last path component of the URL
        auto slash = ev.fileUrl.rfind('/');
        ev.filename = (slash != std::string::npos) ? ev.fileUrl.substr(slash + 1) : ev.fileUrl;
        // Strip query string from filename if present
        auto q = ev.filename.find('?');
        if (q != std::string::npos)
            ev.filename = ev.filename.substr(0, q);

        Logger::Info("PeekabooHttpServer: load request — {} ({})", ev.releaseName, ev.fileUrl);

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_pending = std::move(ev);
        }

        res.status = 200;
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    srv.Post("/api/run", [this](const httplib::Request& req, httplib::Response& res) {
        // This endpoint grants access to a host file path. Browser pages never need it, so reject
        // every request carrying an Origin header instead of relying on CORS (which only protects
        // response reads and can still allow a simple cross-origin POST to reach the server).
        if (!req.get_header_value("Origin").empty()) {
            res.status = 403;
            res.set_content(R"({"error":"browser origins are not allowed"})", "application/json");
            return;
        }

        const std::string contentType = req.get_header_value("Content-Type");
        if (contentType.rfind("application/json", 0) != 0) {
            res.status = 415;
            res.set_content(R"({"error":"Content-Type must be application/json"})",
                            "application/json");
            return;
        }

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& e) {
            Logger::Error("PeekabooHttpServer: invalid /api/run JSON — {}", e.what());
            res.status = 400;
            res.set_content(R"({"error":"invalid JSON"})", "application/json");
            return;
        }

        if (!body.contains("program") || !body["program"].is_string()) {
            res.status = 400;
            res.set_content(R"({"error":"program required"})", "application/json");
            return;
        }
        if (body.contains("stopOnEntry") && !body["stopOnEntry"].is_boolean()) {
            res.status = 400;
            res.set_content(R"({"error":"stopOnEntry must be a boolean"})", "application/json");
            return;
        }
        if (body.contains("focus") && !body["focus"].is_boolean()) {
            res.status = 400;
            res.set_content(R"({"error":"focus must be a boolean"})", "application/json");
            return;
        }

        PeekabooRunRequest run;
        run.program = body["program"].get<std::string>();
        run.focus = body.value("focus", true);
        run.stopOnEntry = body.value("stopOnEntry", false);

        std::error_code ec;
        const std::filesystem::path programPath(run.program);
        if (run.program.empty() || !programPath.is_absolute()) {
            res.status = 400;
            res.set_content(R"({"error":"program must be an absolute path"})", "application/json");
            return;
        }
        if (!std::filesystem::is_regular_file(programPath, ec) || ec) {
            res.status = 404;
            res.set_content(R"({"error":"program does not exist"})", "application/json");
            return;
        }

        Logger::Info("PeekabooHttpServer: local run request — {}", run.program);
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            run.generation = ++m_runGeneration;
            m_pendingRun = std::move(run);
        }

        res.status = 202;
        res.set_content(R"({"status":"accepted"})", "application/json");
    });

    m_thread = std::thread([this] { ServerLoop(); });
}

void PeekabooHttpServer::Stop()
{
    if (m_impl)
        m_impl->server.stop();
    if (m_thread.joinable())
        m_thread.join();
}

void PeekabooHttpServer::ServerLoop()
{
    Logger::Info("PeekabooHttpServer: listening on localhost:{}", m_port);
    if (!m_impl->server.listen("127.0.0.1", m_port))
        Logger::Error("PeekabooHttpServer: failed to bind port {}", m_port);
}

std::optional<PeekabooLoadEvent> PeekabooHttpServer::PopPendingLoad()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return std::exchange(m_pending, std::nullopt);
}

std::optional<PeekabooRunRequest> PeekabooHttpServer::PopPendingRun()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return std::exchange(m_pendingRun, std::nullopt);
}

void PeekabooHttpServer::PublishRunState(const PeekabooRunState& state)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_runState = state;
}

} // namespace core
} // namespace adm
