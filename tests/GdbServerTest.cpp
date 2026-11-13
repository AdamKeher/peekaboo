#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "core/GdbServer.h"
#include "core/GdbTargetOps.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#define ADM_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define INVALID_SOCKET (-1)
#define ADM_CLOSE      ::close
#endif

using adm::core::GdbRegs;
using adm::core::GdbServer;
using adm::core::GdbTargetOps;

namespace {

constexpr int kTestPort = 12346;

// A minimal RSP client: enough to drive the server the way real gdb does.
class RspClient
{
public:
    bool Connect(int port)
    {
        m_sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_sock == INVALID_SOCKET)
            return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (::connect(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    ~RspClient()
    {
        if (m_sock != INVALID_SOCKET)
            ADM_CLOSE(m_sock);
    }

    void SendRaw(const std::string& s) { ::send(m_sock, s.data(), static_cast<int>(s.size()), 0); }

    void SendPacket(const std::string& payload)
    {
        unsigned sum = 0;
        for (char c : payload)
            sum += static_cast<unsigned char>(c);
        char tail[8];
        std::snprintf(tail, sizeof(tail), "%02x", sum & 0xFF);
        SendRaw("$" + payload + "#" + std::string(tail, 2));
    }

    // Reads until a complete "$...#xx" is seen, skipping acks. Returns "" on timeout.
    std::string ReadPacket(int timeoutMs = 4000)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const size_t start = m_buf.find('$');
            if (start != std::string::npos) {
                const size_t hash = m_buf.find('#', start);
                if (hash != std::string::npos && m_buf.size() >= hash + 3) {
                    std::string payload = m_buf.substr(start + 1, hash - start - 1);
                    m_acks += static_cast<int>(
                        std::count(m_buf.begin(), m_buf.begin() + static_cast<long>(start), '+'));
                    m_buf.erase(0, hash + 3);
                    return payload;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return {};
            char chunk[1024];
            const int n = ::recv(m_sock, chunk, sizeof(chunk), 0);
            if (n > 0)
                m_buf.append(chunk, static_cast<size_t>(n));
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    std::string Exchange(const std::string& payload)
    {
        SendPacket(payload);
        return ReadPacket();
    }

    int AckCount() const { return m_acks; }

    // Close abortively (RST) rather than with a FIN. This is what a killed gdb looks like, and
    // it is the case that matters: a FIN makes recv() return 0, an RST makes it return -1 --
    // indistinguishable from a read timeout unless the error code is checked.
    void Abort()
    {
        if (m_sock == INVALID_SOCKET)
            return;
        linger lg{};
        lg.l_onoff = 1;
        lg.l_linger = 0; // discard, send RST
        ::setsockopt(m_sock, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
        ADM_CLOSE(m_sock);
        m_sock = INVALID_SOCKET;
    }

private:
    sock_t m_sock = INVALID_SOCKET;
    std::string m_buf;
    int m_acks = 0;
};

// Runs Pump() on a dedicated thread, standing in for Application::RunFrame().
class PumpDriver
{
public:
    explicit PumpDriver(GdbServer& server) : m_server(server)
    {
        m_thread = std::thread([this] {
            while (!m_stop.load()) {
                m_server.Pump(std::chrono::milliseconds(2));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    ~PumpDriver()
    {
        m_stop.store(true);
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    GdbServer& m_server;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

struct Fixture
{
    GdbRegs regs;
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x100, 0xAB);
    std::atomic<bool> halted{true};

    GdbTargetOps Ops()
    {
        GdbTargetOps ops;
        ops.isRunning = [] { return true; };
        ops.isHalted = [this] { return halted.load(); };
        ops.readRegs = [this](GdbRegs& out) {
            out = regs;
            return true;
        };
        ops.writeReg = [this](int idx, uint32_t v) {
            if (idx == 17)
                regs.pc = v;
            else if (idx >= 0 && idx < 8)
                regs.d[idx] = v;
            return true;
        };
        ops.readMem = [this](uint32_t addr, uint8_t* out, size_t len, std::vector<bool>* valid) {
            if (valid)
                valid->assign(len, false);
            for (size_t i = 0; i < len; ++i) {
                const uint32_t a = addr + static_cast<uint32_t>(i);
                if (a < mem.size()) {
                    out[i] = mem[a];
                    if (valid)
                        (*valid)[i] = true;
                }
            }
            return len;
        };
        ops.writeMem = [this](uint32_t addr, const uint8_t* data, size_t len, std::string& err) {
            if (addr + len > mem.size()) {
                err = "out of range";
                return false;
            }
            std::memcpy(mem.data() + addr, data, len);
            return true;
        };
        return ops;
    }
};

} // namespace

TEST(GdbServerTest, ServesAHandshakeAndReadsOverARealSocket)
{
    Fixture fx;
    fx.regs.pc = 0x00C0CB68;
    fx.regs.d[0] = 0x12345678;

    GdbServer server;
    server.SetTargetOps(fx.Ops());
    ASSERT_TRUE(server.Start(kTestPort)) << server.LastError();
    PumpDriver pump(server);

    RspClient client;
    ASSERT_TRUE(client.Connect(kTestPort));

    // Handshake.
    const std::string caps = client.Exchange("qSupported:swbreak+");
    EXPECT_NE(caps.find("PacketSize="), std::string::npos);
    EXPECT_EQ(client.Exchange("?"), "T05thread:1;");

    // Read-only packets are answered from the socket thread while halted.
    const std::string g = client.Exchange("g");
    ASSERT_EQ(g.size(), 360u);
    EXPECT_EQ(g.substr(0, 8), "12345678");
    EXPECT_EQ(g.substr(136, 8), "00c0cb68");

    // Memory read, and an unmapped read that must fail honestly rather than return filler.
    EXPECT_EQ(client.Exchange("m0,4"), "abababab");
    EXPECT_EQ(client.Exchange("mFFFF00,4"), "E01");

    EXPECT_TRUE(server.IsClientConnected());
    server.Stop();
}

TEST(GdbServerTest, MutatingPacketsAreServicedByThePump)
{
    Fixture fx;
    GdbServer server;
    server.SetTargetOps(fx.Ops());
    ASSERT_TRUE(server.Start(kTestPort + 1)) << server.LastError();
    PumpDriver pump(server);

    RspClient client;
    ASSERT_TRUE(client.Connect(kTestPort + 1));

    // These are routed through the main-thread queue; without a running Pump they would time out.
    EXPECT_EQ(client.Exchange("P11=00c01234"), "OK");
    EXPECT_EQ(fx.regs.pc, 0x00C01234u);
    EXPECT_EQ(client.Exchange("M10,4:deadbeef"), "OK");
    EXPECT_EQ(fx.mem[0x10], 0xDE);
    EXPECT_EQ(fx.mem[0x13], 0xEF);

    server.Stop();
}

TEST(GdbServerTest, BinaryWritePayloadMayContainRawCtrlC)
{
    // GDB's binary X packet escapes only # $ } * — a raw 0x03 inside the payload is legal
    // data. The server used to strip 0x03 from anywhere in the receive buffer, corrupting the
    // write and firing a spurious interrupt.
    Fixture fx;
    std::atomic<int> interrupts{0};
    GdbTargetOps ops = fx.Ops();
    ops.interrupt = [&] { interrupts++; };

    GdbServer server;
    server.SetTargetOps(ops);
    ASSERT_TRUE(server.Start(kTestPort + 8)) << server.LastError();
    PumpDriver pump(server);

    RspClient client;
    ASSERT_TRUE(client.Connect(kTestPort + 8));

    std::string payload = "X10,3:";
    payload += '\x01';
    payload += '\x03';
    payload += '\x02';
    EXPECT_EQ(client.Exchange(payload), "OK");
    EXPECT_EQ(fx.mem[0x10], 0x01);
    EXPECT_EQ(fx.mem[0x11], 0x03);
    EXPECT_EQ(fx.mem[0x12], 0x02);
    EXPECT_EQ(interrupts.load(), 0);

    // A bare 0x03 outside packet framing must still interrupt.
    client.SendRaw(std::string(1, '\x03'));
    for (int i = 0; i < 200 && interrupts.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(interrupts.load(), 1);

    server.Stop();
}

TEST(GdbServerTest, ReadsStillWorkWhileTheTargetIsRunning)
{
    // With the target running, reads cannot be answered on the socket thread; they must go
    // through the pump instead of racing the emulator or dead-locking.
    Fixture fx;
    fx.halted.store(false);
    fx.regs.d[1] = 0x0BADC0DE;

    GdbServer server;
    server.SetTargetOps(fx.Ops());
    ASSERT_TRUE(server.Start(kTestPort + 2)) << server.LastError();
    PumpDriver pump(server);

    RspClient client;
    ASSERT_TRUE(client.Connect(kTestPort + 2));
    const std::string g = client.Exchange("g");
    ASSERT_EQ(g.size(), 360u);
    EXPECT_EQ(g.substr(8, 8), "0badc0de");

    server.Stop();
}

TEST(GdbServerTest, BadChecksumIsNackedAndTheStreamStaysInSync)
{
    Fixture fx;
    GdbServer server;
    server.SetTargetOps(fx.Ops());
    ASSERT_TRUE(server.Start(kTestPort + 3)) << server.LastError();
    PumpDriver pump(server);

    RspClient client;
    ASSERT_TRUE(client.Connect(kTestPort + 3));

    // Deliberately wrong checksum: the server must NAK and not answer the packet...
    client.SendRaw("$g#00");
    // ...and a correctly framed packet afterwards must still be understood.
    EXPECT_EQ(client.Exchange("?"), "T05thread:1;");

    server.Stop();
}

TEST(GdbServerTest, NoAckModeStopsTheServerSendingAcks)
{
    Fixture fx;
    GdbServer server;
    server.SetTargetOps(fx.Ops());
    ASSERT_TRUE(server.Start(kTestPort + 4)) << server.LastError();
    PumpDriver pump(server);

    RspClient client;
    ASSERT_TRUE(client.Connect(kTestPort + 4));

    EXPECT_EQ(client.Exchange("QStartNoAckMode"), "OK");
    const int acksBefore = client.AckCount();
    client.Exchange("?");
    client.Exchange("?");
    // No further '+' should arrive once no-ack mode is negotiated.
    EXPECT_EQ(client.AckCount(), acksBefore);

    server.Stop();
}

TEST(GdbServerTest, DetachClosesTheConnectionButLeavesTheServerListening)
{
    Fixture fx;
    GdbServer server;
    server.SetTargetOps(fx.Ops());
    ASSERT_TRUE(server.Start(kTestPort + 5)) << server.LastError();
    PumpDriver pump(server);

    {
        RspClient client;
        ASSERT_TRUE(client.Connect(kTestPort + 5));
        EXPECT_EQ(client.Exchange("D"), "OK");
    }

    // A second session must be able to attach: closing one client is not closing the server.
    RspClient again;
    ASSERT_TRUE(again.Connect(kTestPort + 5));
    EXPECT_EQ(again.Exchange("?"), "T05thread:1;");

    server.Stop();
    EXPECT_FALSE(server.IsRunning());
}

TEST(GdbServerTest, AnAbruptlyDroppedClientDoesNotWedgeThePort)
{
    // Regression: a read timeout and a dead socket both make recv() return -1. Treating them
    // alike left the connection in CLOSE_WAIT, the accept loop never came back round, and the
    // port stayed unusable until Peekaboo was restarted. Found by a real gdb session dying
    // mid-run and every later attach failing with "nothing is listening".
    Fixture fx;
    GdbServer server;
    server.SetTargetOps(fx.Ops());
    ASSERT_TRUE(server.Start(kTestPort + 7)) << server.LastError();
    PumpDriver pump(server);

    {
        RspClient client;
        ASSERT_TRUE(client.Connect(kTestPort + 7));
        EXPECT_EQ(client.Exchange("?"), "T05thread:1;");
        client.Abort(); // RST, exactly like a gdb process that was killed
    }

    // The server must notice and be ready for the next session.
    for (int attempt = 0; attempt < 100 && server.IsClientConnected(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(server.IsClientConnected());

    RspClient second;
    ASSERT_TRUE(second.Connect(kTestPort + 7));
    EXPECT_EQ(second.Exchange("?"), "T05thread:1;");

    server.Stop();
}

TEST(GdbServerTest, StartFailsCleanlyWhenThePortIsTaken)
{
    Fixture fx;
    GdbServer first;
    first.SetTargetOps(fx.Ops());
    ASSERT_TRUE(first.Start(kTestPort + 6));

    GdbServer second;
    second.SetTargetOps(fx.Ops());
    EXPECT_FALSE(second.Start(kTestPort + 6));
    EXPECT_NE(second.LastError().find("already in use"), std::string::npos);

    first.Stop();
}
