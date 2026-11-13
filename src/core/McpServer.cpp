#include "McpServer.h"

#include <httplib.h>
#include <chrono>

#include "Logger.h"

namespace adm {
namespace core {

struct McpServer::Impl
{
    httplib::Server server;
};

McpServer::McpServer() : m_impl(std::make_unique<Impl>()) {}

McpServer::~McpServer() { Stop(); }

void McpServer::Start(int port, const std::string& token)
{
    if (m_running.load())
        return;

    m_port = port;
    m_token = token;
    auto& srv = m_impl->server;

    // Localhost only, but still gate on a shared token so other local processes can't drive the
    // emulator. Token comes via the X-Peekaboo-Token header or a ?token= query parameter.
    auto authorized = [this](const httplib::Request& req) {
        // Fail closed: an empty token means configuration went wrong, not "no auth required" —
        // this server grants full emulator/memory control.
        if (m_token.empty())
            return false;
        std::string tok = req.get_header_value("X-Peekaboo-Token");
        if (tok.empty())
            tok = req.get_param_value("token");
        return tok == m_token;
    };

    // A page in a browser - a local HTML file (Origin "null"), say - can use the API too, but
    // only with the token. The browser's preflight carries no token (it never carries custom
    // headers), so it is answered for any origin: all it grants is permission to ASK. The real
    // request then has to be authorized, and only an authorized response carries
    // Access-Control-Allow-Origin, so a page without the token cannot read anything - and
    // cannot do anything either, since an unauthorized request executes nothing.
    auto preflight = [](const httplib::Request& req, httplib::Response& res) {
        const std::string origin = req.get_header_value("Origin");
        if (!origin.empty()) {
            res.set_header("Access-Control-Allow-Origin", origin);
            res.set_header("Vary", "Origin");
        }
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, X-Peekaboo-Token");
        res.set_header("Access-Control-Max-Age", "600");
        // Chrome's Private Network Access: a request into localhost from a less private
        // context is preflighted with this, and refused without the answer
        if (req.get_header_value("Access-Control-Request-Private-Network") == "true")
            res.set_header("Access-Control-Allow-Private-Network", "true");
        res.status = 204;
    };
    srv.Options("/tools", preflight);
    srv.Options("/tool", preflight);
    auto allowOrigin = [](const httplib::Request& req, httplib::Response& res) {
        const std::string origin = req.get_header_value("Origin");
        if (!origin.empty()) {
            res.set_header("Access-Control-Allow-Origin", origin);
            res.set_header("Vary", "Origin");
        }
    };

    srv.Get("/tools",
            [this, authorized, allowOrigin](const httplib::Request& req, httplib::Response& res) {
                if (!authorized(req)) {
                    res.status = 401;
                    res.set_content(R"({"error":"unauthorized"})", "application/json");
                    return;
                }
                allowOrigin(req, res);
                nlohmann::json manifest = m_manifest ? m_manifest() : nlohmann::json::array();
                res.set_content(manifest.dump(), "application/json");
            });

    srv.Post("/tool", [this, authorized, allowOrigin](const httplib::Request& req,
                                                      httplib::Response& res) {
        if (!authorized(req)) {
            res.status = 401;
            res.set_content(R"({"error":"unauthorized"})", "application/json");
            return;
        }
        allowOrigin(req, res);

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                nlohmann::json{{"error", std::string("invalid JSON: ") + e.what()}}.dump(),
                "application/json");
            return;
        }

        auto request = std::make_shared<Request>();
        request->name = body.value("name", std::string{});
        request->args = body.contains("args") ? body["args"] : nlohmann::json::object();
        if (request->name.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"tool name required"})", "application/json");
            return;
        }

        std::future<nlohmann::json> fut = request->result.get_future();
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_pending.push_back(request);
        }

        // Block this httplib worker thread until the main thread drains + fulfils the request.
        if (fut.wait_for(std::chrono::seconds(kRequestTimeoutSeconds)) !=
            std::future_status::ready) {
            res.status = 504;
            res.set_content(R"j({"error":"tool timed out (main thread busy?)"})j",
                            "application/json");
            return;
        }

        try {
            nlohmann::json result = fut.get();
            // replace: a tool's strings can come from guest memory, which need not be UTF-8
            res.set_content(result.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                            "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(nlohmann::json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    m_running.store(true);
    m_thread = std::thread([this] { ServerLoop(); });
}

void McpServer::Stop()
{
    if (m_impl)
        m_impl->server.stop();
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false);

    // Fail any in-flight requests so blocked workers (already stopped) and future drains don't
    // leak.
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& req : m_pending) {
        try {
            req->result.set_value(nlohmann::json{{"error", "server stopped"}});
        } catch (...) {
            // The promise may already hold a value; second set_value throws — nothing to do.
        }
    }
    m_pending.clear();
}

void McpServer::ServerLoop()
{
    Logger::Info("McpServer: listening on http://127.0.0.1:{}", m_port);
    if (!m_impl->server.listen("127.0.0.1", m_port)) {
        Logger::Error("McpServer: failed to bind port {}", m_port);
        m_running.store(false);
    }
}

void McpServer::DrainPending(const ToolDispatch& dispatch)
{
    std::vector<std::shared_ptr<Request>> batch;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        batch.swap(m_pending);
    }
    std::vector<std::shared_ptr<Request>> deferred;
    for (auto& req : batch) {
        nlohmann::json result;
        try {
            result = dispatch(req->name, req->args);
        } catch (const std::exception& e) {
            result = nlohmann::json{{"error", e.what()}};
        } catch (...) {
            result = nlohmann::json{{"error", "unknown error"}};
        }
        // The tool is waiting on the emulator, which only advances once this function returns.
        // Leave the worker parked on its future and try again next frame.
        if (result.is_object() && result.value(kRetryKey, false)) {
            deferred.push_back(req);
            continue;
        }
        try {
            req->result.set_value(std::move(result));
        } catch (...) {
            // promise already satisfied (e.g. server stopped) — ignore
        }
    }
    if (!deferred.empty()) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_pending.insert(m_pending.begin(), deferred.begin(), deferred.end());
    }
}

} // namespace core
} // namespace adm
