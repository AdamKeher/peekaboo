#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace adm::core {

// Where the Amiga's serial port goes (core 1.89.0, retrodep/adm_hostio.c). The core takes it as
// one string: "TCP://host:port" listens for a client, "TCP://host:port/connect" dials out and
// keeps redialling, anything else is a host serial device ("COM3", "/dev/ttyUSB0").
struct SerialPortTarget
{
    enum class Mode { Off, TcpListen, TcpConnect, Device };

    Mode mode = Mode::Off;
    std::string host;   // TCP modes
    int port = 1234;    // TCP modes
    std::string device; // Device mode

    // The string adm_serial_set_port takes; empty for Off.
    std::string ToCoreName() const;
    // A TCP server reachable from other machines: a listen address that is not loopback
    // (an empty host listens on 127.0.0.1)
    bool ListensBeyondThisComputer() const;
    // Accepts the core's spelling plus the console's: OFF/NONE, TCP://..., TCP:..., a device.
    // Empty when the text is not a target (a TCP spec without a valid port).
    static std::optional<SerialPortTarget> Parse(const std::string& text);

    bool operator==(const SerialPortTarget&) const = default;
};

const char* SerialPortModeName(SerialPortTarget::Mode mode); // "off", "tcp-listen", ...

// What the core reports (adm_hostio_get_status). The core struct is copied into this one field
// for field; see HostPorts.cpp for the wire layout.
struct HostPortsStatus
{
    SerialPortTarget::Mode serialMode = SerialPortTarget::Mode::Off;
    bool serialOpen = false;
    bool serialConnected = false;
    uint32_t serialTx = 0;
    uint32_t serialRx = 0;
    uint32_t serialDropped = 0;
    int serialBaud = 0;
    std::string serialName;
    std::string serialPeer;
    std::string serialError;
    bool parallelOpen = false;
    uint32_t parallelBytes = 0;
    std::string parallelPath;
    std::string parallelError;
    // MIDI (core 1.90.0). Out: -2 none, -1 the system mapper, 0.. a device. In: -1 none.
    bool hasMidi = false; // the core reported the MIDI fields
    int midiOutDevice = -2;
    int midiInDevice = -1;
    bool midiActive = false; // the Amiga has the UART at MIDI speed and a device is open
    uint32_t midiOutMessages = 0;
    uint32_t midiInBytes = 0;
    std::string midiError;
};

// The core's struct adm_hostio_status, byte for byte.
inline constexpr int kHostPortsStatusWireSize = 4 * 7 + 256 + 64 + 160 + 4 * 2 + 512 + 160;
// The same struct from core 1.90.0 on, with the MIDI fields appended.
inline constexpr int kHostPortsStatusWireSizeMidi = kHostPortsStatusWireSize + 4 * 5 + 160;
std::optional<HostPortsStatus> ParseHostPortsStatus(const uint8_t* data, int size);

// One line for the console, the status bar tooltip and Preferences: "listening on 0.0.0.0:1234,
// no client", "connected to 127.0.0.1:50123 - 12 out, 3 in".
std::string DescribeSerialStatus(const HostPortsStatus& status);

// Serial devices the host has now: COM ports on Windows, /dev/tty* serial devices elsewhere.
std::vector<std::string> EnumerateHostSerialDevices();

// Host MIDI devices, by the number adm_midi_set_devices takes. Windows only; empty elsewhere.
struct MidiDevice
{
    int id = 0;
    std::string name;
};
// Cached: winmm's device-caps calls cost enough that asking once per frame (the Ports tab's two
// combo previews, get_status) showed. The first call scans; `rescan` asks the system again -
// the Ports tab's Rescan button and the console's MIDI listing do.
std::vector<MidiDevice> EnumerateMidiOutputs(bool rescan = false);
std::vector<MidiDevice> EnumerateMidiInputs(bool rescan = false);
// "none", "system mapper", or the device's name (or "device N" when it has gone away). From
// the cache.
std::string MidiDeviceName(int id, bool output);

} // namespace adm::core
