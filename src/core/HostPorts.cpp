#include "HostPorts.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
// mmsystem.h needs windows.h first; the blank line stops include sorting swapping them.

#include <mmsystem.h>
#endif

namespace adm::core {
namespace {

std::string Upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// A fixed char field from the wire struct, which the core always terminates.
std::string Field(const uint8_t* data, int offset, int length)
{
    const auto* begin = reinterpret_cast<const char*>(data + offset);
    return std::string(begin, strnlen(begin, static_cast<size_t>(length)));
}

uint32_t U32(const uint8_t* data, int offset)
{
    uint32_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

} // namespace

std::string SerialPortTarget::ToCoreName() const
{
    switch (mode) {
        case Mode::Off:
            return {};
        case Mode::TcpListen: // this computer only, unless a wider address is asked for
            return fmt::format("TCP://{}:{}", host.empty() ? "127.0.0.1" : host, port);
        case Mode::TcpConnect:
            return fmt::format("TCP://{}:{}/connect", host.empty() ? "127.0.0.1" : host, port);
        case Mode::Device:
            return device;
    }
    return {};
}

bool SerialPortTarget::ListensBeyondThisComputer() const
{
    if (mode != Mode::TcpListen || host.empty())
        return false;
    std::string h = host;
    for (char& c : h)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h != "localhost" && h != "::1" && h.rfind("127.", 0) != 0;
}

std::optional<SerialPortTarget> SerialPortTarget::Parse(const std::string& input)
{
    const std::string text = Trim(input);
    SerialPortTarget target;
    const std::string upper = Upper(text);
    if (text.empty() || upper == "OFF" || upper == "NONE")
        return target;

    std::string spec;
    if (upper.rfind("TCP://", 0) == 0)
        spec = text.substr(6);
    else if (upper.rfind("TCP:", 0) == 0)
        spec = text.substr(4);
    else {
        target.mode = Mode::Device;
        target.device = text;
        return target;
    }

    target.mode = Mode::TcpListen;
    if (const auto slash = spec.find('/'); slash != std::string::npos) {
        const std::string suffix = Upper(spec.substr(slash + 1));
        if (suffix == "CONNECT")
            target.mode = Mode::TcpConnect;
        else if (suffix != "WAIT")
            return std::nullopt;
        spec.resize(slash);
    }
    const auto colon = spec.rfind(':');
    if (colon == std::string::npos) {
        target.host = spec;
        return target; // the core's default port
    }
    target.host = spec.substr(0, colon);
    const std::string portText = spec.substr(colon + 1);
    if (portText.empty() || portText.size() > 5 ||
        !std::all_of(portText.begin(), portText.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; }))
        return std::nullopt;
    target.port = std::stoi(portText);
    if (target.port < 1 || target.port > 65535)
        return std::nullopt;
    if (target.host.size() >= 2 && target.host.front() == '[' && target.host.back() == ']')
        target.host = target.host.substr(1, target.host.size() - 2);
    return target;
}

const char* SerialPortModeName(SerialPortTarget::Mode mode)
{
    switch (mode) {
        case SerialPortTarget::Mode::Off:
            return "off";
        case SerialPortTarget::Mode::TcpListen:
            return "tcp-listen";
        case SerialPortTarget::Mode::TcpConnect:
            return "tcp-connect";
        case SerialPortTarget::Mode::Device:
            return "device";
    }
    return "off";
}

std::optional<HostPortsStatus> ParseHostPortsStatus(const uint8_t* data, int size)
{
    if (!data || size < kHostPortsStatusWireSize)
        return std::nullopt;
    HostPortsStatus status;
    const uint32_t mode = U32(data, 0);
    status.serialMode =
        mode <= 3 ? static_cast<SerialPortTarget::Mode>(mode) : SerialPortTarget::Mode::Off;
    status.serialOpen = U32(data, 4) != 0;
    status.serialConnected = U32(data, 8) != 0;
    status.serialTx = U32(data, 12);
    status.serialRx = U32(data, 16);
    status.serialDropped = U32(data, 20);
    status.serialBaud = static_cast<int>(U32(data, 24));
    status.serialName = Field(data, 28, 256);
    status.serialPeer = Field(data, 284, 64);
    status.serialError = Field(data, 348, 160);
    status.parallelOpen = U32(data, 508) != 0;
    status.parallelBytes = U32(data, 512);
    status.parallelPath = Field(data, 516, 512);
    status.parallelError = Field(data, 1028, 160);
    if (size >= kHostPortsStatusWireSizeMidi) {
        status.hasMidi = true;
        status.midiOutDevice = static_cast<int32_t>(U32(data, 1188));
        status.midiInDevice = static_cast<int32_t>(U32(data, 1192));
        status.midiActive = U32(data, 1196) != 0;
        status.midiOutMessages = U32(data, 1200);
        status.midiInBytes = U32(data, 1204);
        status.midiError = Field(data, 1208, 160);
    }
    return status;
}

std::string DescribeSerialStatus(const HostPortsStatus& status)
{
    // "TCP://0.0.0.0:1234/connect" -> "0.0.0.0:1234"
    std::string address = status.serialName;
    if (const auto target = SerialPortTarget::Parse(status.serialName);
        target && target->mode != SerialPortTarget::Mode::Device)
        address = fmt::format("{}:{}", target->host, target->port);
    const std::string traffic = fmt::format(
        "{} out, {} in{}", status.serialTx, status.serialRx,
        status.serialDropped ? fmt::format(", {} dropped", status.serialDropped) : std::string());
    if (!status.serialError.empty() && !status.serialOpen)
        return "failed: " + status.serialError;
    switch (status.serialMode) {
        case SerialPortTarget::Mode::Off:
            return "off";
        case SerialPortTarget::Mode::TcpListen:
            return status.serialConnected
                       ? fmt::format("client {} connected - {}", status.serialPeer, traffic)
                       : fmt::format("listening on {}, no client - {}", address, traffic);
        case SerialPortTarget::Mode::TcpConnect:
            return status.serialConnected
                       ? fmt::format("connected to {} - {}", status.serialPeer, traffic)
                       : fmt::format("connecting to {} - {}", address,
                                     status.serialError.empty() ? traffic : status.serialError);
        case SerialPortTarget::Mode::Device:
            return fmt::format("{} open at {} baud - {}", status.serialName, status.serialBaud,
                               traffic);
    }
    return "off";
}

std::vector<std::string> EnumerateHostSerialDevices()
{
    std::vector<std::string> devices;
#ifdef _WIN32
    char target[512];
    for (int index = 1; index <= 255; ++index) {
        const std::string name = "COM" + std::to_string(index);
        if (QueryDosDeviceA(name.c_str(), target, sizeof(target)) != 0)
            devices.push_back(name);
    }
#else
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        for (const char* prefix : {"ttyS", "ttyUSB", "ttyACM", "tty.", "cu."}) {
            if (name.rfind(prefix, 0) == 0) {
                devices.push_back(entry.path().string());
                break;
            }
        }
    }
    std::sort(devices.begin(), devices.end());
#endif
    return devices;
}

namespace {

std::vector<MidiDevice> ScanMidiOutputs()
{
    std::vector<MidiDevice> devices;
#ifdef _WIN32
    const UINT count = midiOutGetNumDevs();
    for (UINT index = 0; index < count; ++index) {
        MIDIOUTCAPSA caps{};
        if (midiOutGetDevCapsA(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            devices.push_back({static_cast<int>(index), caps.szPname});
    }
#endif
    return devices;
}

std::vector<MidiDevice> ScanMidiInputs()
{
    std::vector<MidiDevice> devices;
#ifdef _WIN32
    const UINT count = midiInGetNumDevs();
    for (UINT index = 0; index < count; ++index) {
        MIDIINCAPSA caps{};
        if (midiInGetDevCapsA(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            devices.push_back({static_cast<int>(index), caps.szPname});
    }
#endif
    return devices;
}

// One scan serves both lists. The mutex is for get_status, which may ask from the MCP thread
// while the Preferences dialog draws.
struct MidiDeviceCache
{
    std::mutex mutex;
    bool scanned = false;
    std::vector<MidiDevice> outputs;
    std::vector<MidiDevice> inputs;
};

MidiDeviceCache& MidiCache()
{
    static MidiDeviceCache cache;
    return cache;
}

void RefreshMidiCacheLocked(MidiDeviceCache& cache, bool rescan)
{
    if (!rescan && cache.scanned)
        return;
    cache.outputs = ScanMidiOutputs();
    cache.inputs = ScanMidiInputs();
    cache.scanned = true;
}

} // namespace

std::vector<MidiDevice> EnumerateMidiOutputs(bool rescan)
{
    auto& cache = MidiCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    RefreshMidiCacheLocked(cache, rescan);
    return cache.outputs;
}

std::vector<MidiDevice> EnumerateMidiInputs(bool rescan)
{
    auto& cache = MidiCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    RefreshMidiCacheLocked(cache, rescan);
    return cache.inputs;
}

std::string MidiDeviceName(int id, bool output)
{
    if (output ? id < -1 : id < 0)
        return "none";
    if (output && id == -1)
        return "system mapper";
    for (const auto& device : output ? EnumerateMidiOutputs() : EnumerateMidiInputs()) {
        if (device.id == id)
            return device.name;
    }
    return "device " + std::to_string(id);
}

} // namespace adm::core
