#include "GdbServer.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <future>
#include <thread>
#include <vector>

#include "Logger.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#define ADM_CLOSESOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#define ADM_CLOSESOCKET ::close
#endif

namespace adm::core {

namespace {

// Packets that change machine state must run on the main thread, because that is the only
// thread allowed to touch the emulator. Everything else is a read and can be answered straight
// from the socket thread while the target is halted.
bool NeedsMainThread(const std::string& payload)
{
    if (payload.empty())
        return false;
    switch (payload[0]) {
        case 'G': // write all registers
        case 'P': // write one register
        case 'M': // write memory (hex)
        case 'X': // write memory (binary)
        case 'c': // continue                (Stage 3)
        case 's': // step                    (Stage 3)
        case 'C':
        case 'S':
        case 'Z': // insert breakpoint       (Stage 3)
        case 'z': // remove breakpoint       (Stage 3)
            return true;
        case '?':
            // Halts the machine if it is running, so it cannot be served from the socket thread.
            return true;
        case 'v':
            return payload.rfind("vCont", 0) == 0;
        case 'q':
            return payload.rfind("qRcmd", 0) == 0; // monitor commands can do anything
        default:
            return false;
    }
}

// True when the last socket error was "nothing to read yet" rather than a real failure.
bool WouldBlock()
{
#ifdef _WIN32
    const int err = WSAGetLastError();
    return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

uint8_t Checksum(const std::string& s)
{
    unsigned sum = 0;
    for (char c : s)
        sum += static_cast<unsigned char>(c);
    return static_cast<uint8_t>(sum & 0xFF);
}

int HexVal(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

} // namespace

// One packet waiting for the main thread.
struct PendingPacket
{
    std::string payload;
    std::promise<GdbPacketHandler::Response> result;
};

struct GdbServer::Impl
{
    // Both sockets are closed by Stop() on the main thread while the server thread is parked in
    // accept()/recv() on them, so both must be atomic and swapped out with exchange().
    std::atomic<socket_t> listenSock{kInvalidSocket};
    std::atomic<socket_t> clientSock{kInvalidSocket};

    GdbPacketHandler handler;

    // Set once before Start() and not mutated afterwards, so both threads may read it.
    GdbTargetOps ops;

    std::mutex queueMutex;
    std::deque<std::shared_ptr<PendingPacket>> queue;

    // Packets to push to the client that it did not ask for: stop replies and O output. The
    // socket thread drains this between reads, which is why the read has a timeout.
    std::mutex outMutex;
    std::deque<std::string> outQueue;

#ifdef _WIN32
    bool wsaStarted = false;
#endif
};

GdbServer::GdbServer() : m_impl(std::make_unique<Impl>()) {}

GdbServer::~GdbServer() { Stop(); }

void GdbServer::SetTargetOps(GdbTargetOps ops) { m_impl->ops = std::move(ops); }

void GdbServer::NotifyStopped(GdbPacketHandler::StopReason reason, uint32_t addr)
{
    if (!m_clientConnected.load())
        return;
    // Only answer a resume we actually issued. A stop that GDB did not ask for -- the user
    // hitting Pause in Peekaboo's toolbar while GDB believes the target is already stopped --
    // must not produce a second stop reply, or the session desynchronises.
    if (!m_targetResumed.exchange(false))
        return;
    std::lock_guard<std::mutex> lock(m_impl->outMutex);
    m_impl->outQueue.push_back(GdbPacketHandler::FormatStopReply(reason, addr));
}

void GdbServer::NotifyOutput(std::string_view text)
{
    if (!m_clientConnected.load() || text.empty())
        return;
    std::string packet = "O";
    packet += GdbPacketHandler::ToHex(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    std::lock_guard<std::mutex> lock(m_impl->outMutex);
    m_impl->outQueue.push_back(std::move(packet));
}

std::string GdbServer::LastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

bool GdbServer::Start(int port)
{
    if (m_running.load())
        return true;
    m_port = port;
    m_stopping.store(false);

#ifdef _WIN32
    if (!m_impl->wsaStarted) {
        WSADATA wsa{};
        // httplib may already have started Winsock; WSAStartup refcounts, so this is safe.
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::lock_guard<std::mutex> lock(m_errorMutex);
            m_lastError = "WSAStartup failed";
            return false;
        }
        m_impl->wsaStarted = true;
    }
#endif

    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = "could not create socket";
        return false;
    }
    int yes = 1;
#ifdef _WIN32
    // NOT SO_REUSEADDR on Windows: there it lets a second socket bind a port another socket is
    // already listening on, so a stale Peekaboo (or anything else) could silently steal the
    // debug port and GDB would attach to the wrong machine. SO_EXCLUSIVEADDRUSE makes the
    // second bind fail, which is the behaviour we actually want.
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes),
                 sizeof(yes));
#else
    // On POSIX SO_REUSEADDR only permits rebinding a port left in TIME_WAIT, which is what lets
    // a restart reclaim its own port immediately. It does not allow two live listeners.
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    // Loopback only. This port hands out full read/write access to emulated memory, so it must
    // never be reachable off-box.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ADM_CLOSESOCKET(s);
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = "port " + std::to_string(port) + " is already in use";
        return false;
    }
    if (::listen(s, 1) != 0) {
        ADM_CLOSESOCKET(s);
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = "listen failed on port " + std::to_string(port);
        return false;
    }

    m_impl->listenSock = s;
    m_running.store(true);
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError.clear();
    }
    m_thread = std::thread([this] { ServerLoop(); });
    Logger::Info("GdbServer: listening on 127.0.0.1:{}", port);
    return true;
}

void GdbServer::Stop()
{
    if (!m_running.load() && !m_thread.joinable())
        return;
    m_stopping.store(true);
    m_running.store(false);

    // Closing the sockets is what breaks the accept()/recv() the server thread is parked in.
    socket_t client = m_impl->clientSock.exchange(kInvalidSocket);
    if (client != kInvalidSocket)
        ADM_CLOSESOCKET(client);
    socket_t listener = m_impl->listenSock.exchange(kInvalidSocket);
    if (listener != kInvalidSocket)
        ADM_CLOSESOCKET(listener);
    if (m_thread.joinable())
        m_thread.join();

    // Fail anything still queued, so no socket thread is left waiting on a promise forever.
    std::lock_guard<std::mutex> lock(m_impl->queueMutex);
    for (auto& p : m_impl->queue) {
        GdbPacketHandler::Response r;
        r.payload = "E01";
        p->result.set_value(r);
    }
    m_impl->queue.clear();
    m_clientConnected.store(false);
}

void GdbServer::Pump(std::chrono::microseconds budget)
{
    if (!m_running.load())
        return;
    const GdbTargetOps& ops = m_impl->ops;

    // The budget only applies while the target is halted. If it is running we still drain the
    // queue -- otherwise a write issued mid-run would block the socket thread until timeout --
    // but we do not linger, because every microsecond here is stolen from the emulator.
    const bool halted = ops.isHalted && ops.isHalted();
    const auto deadline = std::chrono::steady_clock::now() + budget;
    int emptySpins = 0;

    for (;;) {
        std::shared_ptr<PendingPacket> item;
        {
            std::lock_guard<std::mutex> lock(m_impl->queueMutex);
            if (!m_impl->queue.empty()) {
                item = m_impl->queue.front();
                m_impl->queue.pop_front();
            }
        }

        if (item) {
            const GdbPacketHandler::Response resp = m_impl->handler.Handle(item->payload, ops);
            if (resp.resumesTarget)
                m_targetResumed.store(true);
            item->result.set_value(resp);
            emptySpins = 0;
            if (resp.resumesTarget)
                return; // the emulator must run before there is anything more to say
            // A packet that hands control back to the target ends the pump immediately: the
            // emulator must run before there is anything new to say.
            continue;
        }

        if (!halted)
            return;
        if (std::chrono::steady_clock::now() >= deadline)
            return;
        // GDB is synchronous, so a brief gap usually means "the next packet is in flight", not
        // "we are done". Spin a little before giving the frame back; a few tens of microseconds
        // here saves a whole 16 ms frame of latency per packet.
        if (++emptySpins > 64)
            return;
        std::this_thread::yield();
    }
}

void GdbServer::ServerLoop()
{
    while (m_running.load() && !m_stopping.load()) {
        sockaddr_in peer{};
#ifdef _WIN32
        int peerLen = sizeof(peer);
#else
        socklen_t peerLen = sizeof(peer);
#endif
        const socket_t listener = m_impl->listenSock.load();
        if (listener == kInvalidSocket)
            break; // Stop() already swapped it out
        socket_t client = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (client == kInvalidSocket) {
            if (m_stopping.load())
                break;
            // A persistent accept failure (EMFILE, listener closed under us) must not become
            // a hot spin. Interrupted calls retry immediately; anything else backs off.
#ifdef _WIN32
            const int err = WSAGetLastError();
            if (err == WSAEINTR)
                continue;
#else
            if (errno == EINTR)
                continue;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        int yes = 1;
        // GDB is strictly request/response, so Nagle would add a round-trip delay per packet.
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes),
                     sizeof(yes));
        // A read timeout is what lets this loop also flush asynchronous output: without it the
        // thread would sit in recv() and a stop reply would not reach GDB until the next packet.
#ifdef _WIN32
        DWORD timeoutMs = 50;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs),
                     sizeof(timeoutMs));
#else
        timeval tv{};
        tv.tv_usec = 50 * 1000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
                     sizeof(tv));
#endif
        m_impl->clientSock.store(client);
        m_clientConnected.store(true);
        m_targetResumed.store(false);
        m_impl->handler.Reset();
        Logger::Info("GdbServer: client connected");

        std::string buf;
        std::vector<char> chunk(4096);
        bool closing = false;

        auto sendRaw = [&](const std::string& data) {
            size_t sent = 0;
            while (sent < data.size()) {
                const int n =
                    ::send(client, data.data() + sent, static_cast<int>(data.size() - sent), 0);
                if (n <= 0)
                    return false;
                sent += static_cast<size_t>(n);
            }
            return true;
        };

        auto sendPacket = [&](const std::string& payload) {
            char tail[4];
            std::snprintf(tail, sizeof(tail), "%02x", Checksum(payload));
            return sendRaw("$" + payload + "#" + std::string(tail, 2));
        };

        while (m_running.load() && !m_stopping.load() && !closing) {
            // Push anything the target has to say before waiting on the client again.
            for (;;) {
                std::string pending;
                {
                    std::lock_guard<std::mutex> lock(m_impl->outMutex);
                    if (m_impl->outQueue.empty())
                        break;
                    pending = std::move(m_impl->outQueue.front());
                    m_impl->outQueue.pop_front();
                }
                if (!sendPacket(pending)) {
                    closing = true;
                    break;
                }
            }
            if (closing)
                break;

            const int n = ::recv(client, chunk.data(), static_cast<int>(chunk.size()), 0);
            if (n == 0)
                break; // orderly shutdown by the client
            if (n < 0) {
                // A read timeout and a dead socket both return -1, and treating them the same
                // spins forever on a client that has gone away: the socket sits in CLOSE_WAIT,
                // the accept loop never comes back round, and the port is unusable until
                // Peekaboo restarts. Only a timeout is allowed to continue.
                if (!WouldBlock())
                    break;
                continue; // timeout: loop round to flush output and re-check the flags
            }
            buf.append(chunk.data(), static_cast<size_t>(n));

            for (;;) {
                if (buf.empty())
                    break;

                // Ctrl-C arrives as a bare 0x03 OUTSIDE packet framing. Only honour one that
                // appears before the first '$': GDB's binary X packet escapes only # $ } *, so
                // a raw 0x03 inside a payload is legal data — stripping it from anywhere in the
                // buffer corrupted binary writes and mis-framed the parse.
                const size_t firstDollar = buf.find('$');
                const size_t etx = buf.find('\x03');
                if (etx != std::string::npos &&
                    (firstDollar == std::string::npos || etx < firstDollar)) {
                    buf.erase(etx, 1);
                    // GDB's Ctrl-C. The interrupt hook is safe to call from here: it only sets
                    // the paused flag, which EmulatorView owns and the main thread acts on.
                    if (m_impl->ops.interrupt)
                        m_impl->ops.interrupt();
                    if (m_targetResumed.exchange(false)) {
                        std::lock_guard<std::mutex> lock(m_impl->outMutex);
                        m_impl->outQueue.push_back(GdbPacketHandler::FormatStopReply(
                            GdbPacketHandler::StopReason::Interrupt));
                    }
                    continue;
                }
                if (buf[0] == '+' || buf[0] == '-') {
                    buf.erase(0, 1);
                    continue;
                }
                const size_t start = buf.find('$');
                if (start == std::string::npos) {
                    buf.clear();
                    break;
                }
                const size_t hash = buf.find('#', start);
                if (hash == std::string::npos || buf.size() < hash + 3)
                    break; // wait for the rest, including both checksum digits

                const std::string payload = buf.substr(start + 1, hash - start - 1);
                const int hi = HexVal(buf[hash + 1]);
                const int lo = HexVal(buf[hash + 2]);
                buf.erase(0, hash + 3);

                const bool checksumOk =
                    hi >= 0 && lo >= 0 && static_cast<uint8_t>((hi << 4) | lo) == Checksum(payload);
                if (!m_impl->handler.NoAckMode()) {
                    if (!sendRaw(checksumOk ? "+" : "-")) {
                        closing = true;
                        break;
                    }
                }
                if (!checksumOk)
                    continue; // GDB will resend after the '-'

                GdbPacketHandler::Response resp;
                if (NeedsMainThread(payload)) {
                    auto pending = std::make_shared<PendingPacket>();
                    pending->payload = payload;
                    auto fut = pending->result.get_future();
                    {
                        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
                        m_impl->queue.push_back(pending);
                    }
                    if (fut.wait_for(std::chrono::seconds(kRequestTimeoutSeconds)) !=
                        std::future_status::ready) {
                        resp.payload = "E01";
                        Logger::Warn("GdbServer: timed out waiting for main thread on '{}'",
                                     payload.substr(0, 32));
                    } else {
                        resp = fut.get();
                    }
                } else if (m_impl->ops.isHalted && m_impl->ops.isHalted()) {
                    // Read-only and the target is stopped, so nothing is mutating the banks:
                    // answer straight from this thread. This is what keeps a multi-frame unwind
                    // down to a few milliseconds instead of one packet per 16 ms host frame.
                    // Reading isHalted() here races the main thread benignly -- the worst case
                    // is that we queue a packet we could have answered inline.
                    resp = m_impl->handler.Handle(payload, m_impl->ops);
                } else {
                    // Target is running: even a read has to be ordered against the emulator, so
                    // hand it to the main thread like any mutation.
                    auto pending = std::make_shared<PendingPacket>();
                    pending->payload = payload;
                    auto fut = pending->result.get_future();
                    {
                        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
                        m_impl->queue.push_back(pending);
                    }
                    if (fut.wait_for(std::chrono::seconds(kRequestTimeoutSeconds)) !=
                        std::future_status::ready)
                        resp.payload = "E01";
                    else
                        resp = fut.get();
                }

                if (resp.resumesTarget)
                    m_targetResumed.store(true);
                if (resp.hasReply && !sendPacket(resp.payload)) {
                    closing = true;
                    break;
                }
                if (resp.closeConnection) {
                    closing = true;
                    break;
                }
            }
        }

        socket_t toClose = m_impl->clientSock.exchange(kInvalidSocket);
        if (toClose != kInvalidSocket)
            ADM_CLOSESOCKET(toClose);
        m_clientConnected.store(false);
        m_targetResumed.store(false);
        {
            std::lock_guard<std::mutex> lock(m_impl->outMutex);
            m_impl->outQueue.clear();
        }
        m_impl->handler.Reset();
        Logger::Info("GdbServer: client disconnected");
    }
}

} // namespace adm::core
