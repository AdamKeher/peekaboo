#include "core/HostPorts.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

using adm::core::DescribeSerialStatus;
using adm::core::HostPortsStatus;
using adm::core::kHostPortsStatusWireSize;
using adm::core::ParseHostPortsStatus;
using adm::core::SerialPortTarget;
using Mode = SerialPortTarget::Mode;

// APP-08: a listen with no address is this computer's alone
TEST(HostPorts, AListenWithNoHostIsLoopback)
{
    SerialPortTarget t;
    t.mode = Mode::TcpListen;
    t.port = 4321;
    EXPECT_EQ(t.ToCoreName(), "TCP://127.0.0.1:4321");
    EXPECT_FALSE(t.ListensBeyondThisComputer());
    for (const char* local : {"127.0.0.1", "127.1.2.3", "localhost", "::1"}) {
        t.host = local;
        EXPECT_FALSE(t.ListensBeyondThisComputer()) << local;
    }
    for (const char* wide : {"0.0.0.0", "192.168.1.5", "::"}) {
        t.host = wide;
        EXPECT_TRUE(t.ListensBeyondThisComputer()) << wide;
    }
}

TEST(HostPorts, ParsesWinUaeAndConsoleSpellings)
{
    auto listen = SerialPortTarget::Parse("TCP://0.0.0.0:1234");
    ASSERT_TRUE(listen);
    EXPECT_EQ(listen->mode, Mode::TcpListen);
    EXPECT_EQ(listen->host, "0.0.0.0");
    EXPECT_EQ(listen->port, 1234);

    auto connect = SerialPortTarget::Parse("tcp:192.168.1.20:2000/connect");
    ASSERT_TRUE(connect);
    EXPECT_EQ(connect->mode, Mode::TcpConnect);
    EXPECT_EQ(connect->host, "192.168.1.20");
    EXPECT_EQ(connect->port, 2000);

    auto ipv6 = SerialPortTarget::Parse("TCP://[::1]:1234");
    ASSERT_TRUE(ipv6);
    EXPECT_EQ(ipv6->host, "::1");

    auto device = SerialPortTarget::Parse("COM3");
    ASSERT_TRUE(device);
    EXPECT_EQ(device->mode, Mode::Device);
    EXPECT_EQ(device->device, "COM3");

    for (const char* off : {"", "OFF", "none"}) {
        auto parsed = SerialPortTarget::Parse(off);
        ASSERT_TRUE(parsed) << off;
        EXPECT_EQ(parsed->mode, Mode::Off) << off;
    }

    EXPECT_FALSE(SerialPortTarget::Parse("TCP://host:99999"));
    EXPECT_FALSE(SerialPortTarget::Parse("TCP://host:12ab"));
    EXPECT_FALSE(SerialPortTarget::Parse("TCP://host:1234/sideways"));
}

TEST(HostPorts, CoreNamesRoundTrip)
{
    for (const char* name :
         {"TCP://0.0.0.0:1234", "TCP://127.0.0.1:4000/connect", "COM12", "/dev/ttyUSB0"}) {
        const auto target = SerialPortTarget::Parse(name);
        ASSERT_TRUE(target) << name;
        EXPECT_EQ(target->ToCoreName(), name);
    }
    EXPECT_EQ(SerialPortTarget{}.ToCoreName(), "");
}

TEST(HostPorts, ReadsTheCoreStatusLayout)
{
    std::vector<uint8_t> wire(kHostPortsStatusWireSize, 0);
    const auto put = [&](int offset, uint32_t value) {
        std::memcpy(wire.data() + offset, &value, sizeof(value));
    };
    put(0, 1); // listen
    put(4, 1);
    put(8, 1);
    put(12, 12);
    put(16, 3);
    put(24, 9600);
    std::strcpy(reinterpret_cast<char*>(wire.data() + 28), "TCP://0.0.0.0:1234");
    std::strcpy(reinterpret_cast<char*>(wire.data() + 284), "127.0.0.1:50123");
    put(508, 1);
    put(512, 42);
    std::strcpy(reinterpret_cast<char*>(wire.data() + 516), "C:/out/printer.prn");

    const auto status = ParseHostPortsStatus(wire.data(), static_cast<int>(wire.size()));
    ASSERT_TRUE(status);
    EXPECT_EQ(status->serialMode, Mode::TcpListen);
    EXPECT_TRUE(status->serialConnected);
    EXPECT_EQ(status->serialTx, 12u);
    EXPECT_EQ(status->serialBaud, 9600);
    EXPECT_EQ(status->serialPeer, "127.0.0.1:50123");
    EXPECT_EQ(status->parallelBytes, 42u);
    EXPECT_EQ(status->parallelPath, "C:/out/printer.prn");
    EXPECT_EQ(DescribeSerialStatus(*status), "client 127.0.0.1:50123 connected - 12 out, 3 in");

    EXPECT_FALSE(ParseHostPortsStatus(wire.data(), kHostPortsStatusWireSize - 1));
    EXPECT_FALSE(status->hasMidi) << "a 1.89.0 core reports no MIDI fields";
}

TEST(HostPorts, ReadsTheMidiFieldsACore190Appends)
{
    std::vector<uint8_t> wire(adm::core::kHostPortsStatusWireSizeMidi, 0);
    const auto put = [&](int offset, int32_t value) {
        std::memcpy(wire.data() + offset, &value, sizeof(value));
    };
    put(1188, -1); // the mapper
    put(1192, 2);
    put(1196, 1);
    put(1200, 30);
    put(1204, 9);
    std::strcpy(reinterpret_cast<char*>(wire.data() + 1208), "cannot open MIDI in 2 (error 2)");
    const auto status = ParseHostPortsStatus(wire.data(), static_cast<int>(wire.size()));
    ASSERT_TRUE(status);
    EXPECT_TRUE(status->hasMidi);
    EXPECT_EQ(status->midiOutDevice, -1);
    EXPECT_EQ(status->midiInDevice, 2);
    EXPECT_TRUE(status->midiActive);
    EXPECT_EQ(status->midiOutMessages, 30u);
    EXPECT_EQ(status->midiInBytes, 9u);
    EXPECT_EQ(status->midiError, "cannot open MIDI in 2 (error 2)");
    EXPECT_EQ(adm::core::MidiDeviceName(-2, true), "none");
    EXPECT_EQ(adm::core::MidiDeviceName(-1, true), "system mapper");
    EXPECT_EQ(adm::core::MidiDeviceName(-1, false), "none");
}

TEST(HostPorts, DescribesAListenerWithNoClientAndAFailure)
{
    HostPortsStatus status;
    status.serialMode = Mode::TcpListen;
    status.serialOpen = true;
    status.serialName = "TCP://0.0.0.0:1234";
    EXPECT_EQ(DescribeSerialStatus(status), "listening on 0.0.0.0:1234, no client - 0 out, 0 in");

    HostPortsStatus failed;
    failed.serialError = "cannot open COM99 (error 2)";
    EXPECT_EQ(DescribeSerialStatus(failed), "failed: cannot open COM99 (error 2)");
}

} // namespace
