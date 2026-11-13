/* XCopyLink - see XCopyLink.h for the contract and the two rules it enforces. */

#include "XCopyLink.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace XCopy {

const char* toString(LinkResult r)
{
    switch (r) {
    case LinkResult::Ok:
        return "ok";
    case LinkResult::PortNotFound:
        return "serial port not found";
    case LinkResult::PortInUse:
        return "serial port already in use";
    case LinkResult::PortError:
        return "serial port could not be configured";
    case LinkResult::NoResponse:
        return "no response to HELLO - is this an XCopy Standalone?";
    case LinkResult::BadVersion:
        return "device speaks a different XCopyLiveProtocol version";
    case LinkResult::NotXCopy:
        return "device is not an XCopy Standalone";
    case LinkResult::MissingFeature:
        return "device cannot seek mid-stream (XCL_CAP_INBAND_SEEK)";
    case LinkResult::WriteError:
        return "write to serial port failed";
    }
    return "unknown";
}

XCopyLink::XCopyLink()
{
    memset(&m_hello, 0, sizeof(m_hello));
    m_rx.resize(kRxBufferBytes);
}

XCopyLink::~XCopyLink()
{
    close();
}

/* ------------------------------------------------------------------------- */
/* Port enumeration                                                           */
/* ------------------------------------------------------------------------- */

void XCopyLink::enumPorts(std::vector<PortInfo>& out)
{
    out.clear();

    std::vector<SerialIO::SerialPortInformation> ports;
    SerialIO::enumSerialPorts(ports);

    out.reserve(ports.size());
    for (const SerialIO::SerialPortInformation& p : ports) {
        PortInfo info;
        quickw2a(p.portName, info.port);
        quickw2a(p.productName, info.product);
        info.vid = p.vid;
        info.pid = p.pid;
        info.looksLikeXCopy = (p.vid == kTeensyVid);
        out.push_back(info);
    }
}

/* ------------------------------------------------------------------------- */
/* Open / close                                                               */
/* ------------------------------------------------------------------------- */

std::string XCopyLink::handshakeSample() const
{
    if (m_hsLen == 0)
        return "nothing received";

    std::string out;
    for (uint32_t i = 0; i < m_hsLen; i++) {
        const uint8_t c = m_hsSample[i];
        if (c >= 32 && c < 127)
            out += (char)c;
        else if (c == '\r' || c == '\n')
            out += ' ';
        else
            out += '.';
    }
    return out;
}

LinkResult XCopyLink::open(const std::string& comPort, bool requireInbandSeek)
{
    close();
    m_lastError.clear();

    if (comPort.empty()) {
        /* Deliberately not an auto-scan. See kTeensyVid. */
        m_lastError = "no COM port specified";
        return LinkResult::PortNotFound;
    }

    std::wstring wide;
    quicka2w(comPort, wide);

    switch (m_port.openPort(wide)) {
    case SerialIO::Response::rInUse:
        m_lastError = comPort + " is in use";
        return LinkResult::PortInUse;
    case SerialIO::Response::rNotFound:
        m_lastError = comPort + " not found";
        return LinkResult::PortNotFound;
    case SerialIO::Response::rOK:
        break;
    default:
        m_lastError = comPort + " could not be opened";
        return LinkResult::PortError;
    }

    /* The Teensy is a CDC device and ignores the line settings entirely, but the
       port still has to be configured for the host driver to be happy. */
    SerialIO::Configuration config;
    config.baudRate = 9600;
    config.ctsFlowControl = false;
    if (m_port.configurePort(config) != SerialIO::Response::rOK) {
        close();
        m_lastError = comPort + " could not be configured";
        return LinkResult::PortError;
    }

    /* A generous OS-side receive buffer is what lets a late pump() catch up
       without the device ever having to drop a record. */
    m_port.setBufferSizes(256 * 1024, 8 * 1024);

    /* Short read timeouts: every read is preceded by getBytesWaiting(), so the
       data is already there and no wait should ever actually elapse. */
    m_port.setReadTimeouts(10, 1);
    m_port.setWriteTimeouts(500, 20);
    m_port.purgeBuffers();

    m_rxHead = m_rxTail = 0;
    m_gotHello = false;
    m_hsCapture = true;
    m_hsLen = 0;

    /* The device is normally at its ASCII console, but it may ALREADY be in
       binary mode because a previous host died without saying BYE - a debugger
       stop, a killed harness, a closed pipe. Both have to be handled, and the
       order matters:

       ASCII FIRST. A binary frame sent to the console lands in its line buffer
       and sits there until the next CR turns it into "Unknown command", which is
       harmless but leaves junk to step over. Whereas the entry command sent to a
       device already in binary mode is four bytes the frame parser discards. So
       the ASCII path is the safe one to try first, and the binary probe is the
       fallback rather than the opener. */
    const std::string flush = "\r";
    m_port.write(flush.c_str(), (unsigned int)flush.length());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    m_port.purgeBuffers();

    const std::string enter = XCL_ENTER_COMMAND "\r";
    if (m_port.write(enter.c_str(), (unsigned int)enter.length()) != enter.length()) {
        close();
        m_lastError = "could not write to " + comPort;
        return LinkResult::WriteError;
    }

    /* MEASURED: the console echoes the command inside 100 ms and the XCL_BANNER
       lands between 100 and 200 ms. An earlier version waited 50 ms, which put
       the first HELLO frame into a console that had not switched yet - it became
       another "Unknown command" and the open failed with "no HELLO" on a device
       that was working perfectly. 250 ms has real margin over the measurement. */
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    m_port.purgeBuffers();
    m_rxHead = m_rxTail = 0;

    /* MEASURED, and the budget is set by the empty-drive case rather than the
       normal one.

       The device prints the banner and only then spins the motor and probes the
       density. With no disk there is never an index pulse, so that probe runs to
       its timeouts - about 2 s, on top of a 600 ms spin-up - before the command
       loop that answers HELLO is even reached. An earlier version allowed 2.5 s
       total and gave up after 1 s of silence, which is comfortably inside that
       window: it reported "no HELLO" against a device that was working and simply
       had an empty drive.

       So: 4 s total, and the silence early-out is 2.5 s. A wrong-but-open port
       still costs only that, and AttachBridgeDrive refuses an empty port name
       outright, so this is not on the path of an ordinary mistake. */
    for (int attempt = 0; attempt < 8 && !m_gotHello; attempt++) {
        if (!sendFrame(XCL_CMD_HELLO, nullptr, 0)) {
            close();
            m_lastError = "could not write to " + comPort;
            return LinkResult::WriteError;
        }
        waitFor(500, &m_gotHello);

        /* Nothing at all after 2.5 s means nothing is listening, and this runs on
           the emulation thread - do not spend the rest of the budget proving it. */
        if (attempt >= 4 && m_hsLen == 0)
            break;
    }

    m_hsCapture = false;

    if (!m_gotHello) {
        const std::string sample = handshakeSample();
        close();
        m_lastError = "no HELLO from " + comPort + " - device sent: [" + sample + "]";
        return LinkResult::NoResponse;
    }

    if (m_hello.protocolVersion != XCL_PROTOCOL_VERSION) {
        char buf[128];
        snprintf(buf, sizeof(buf), "device protocol v%u, host expects v%u",
                 (unsigned)m_hello.protocolVersion, (unsigned)XCL_PROTOCOL_VERSION);
        close();
        m_lastError = buf;
        return LinkResult::BadVersion;
    }

    /* ident is NUL-padded, not guaranteed NUL-terminated. */
    char ident[sizeof(m_hello.ident) + 1];
    memcpy(ident, m_hello.ident, sizeof(m_hello.ident));
    ident[sizeof(m_hello.ident)] = 0;
    if (strstr(ident, "XCopy") == nullptr) {
        close();
        m_lastError = std::string("device identifies as '") + ident + "'";
        return LinkResult::NotXCopy;
    }

    if (m_hello.maxRecordBytes > kMaxRecordPayload) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "device wants records up to %u bytes, host caps at %u - rebuild the host",
                 (unsigned)m_hello.maxRecordBytes, (unsigned)kMaxRecordPayload);
        close();
        m_lastError = buf;
        return LinkResult::BadVersion;
    }

    /* The one capability that is not optional. A device without it would produce a
       driver that quietly behaves like the Greaseweazle one - slow in exactly the
       way this whole backend exists to avoid - so refuse it loudly instead. */
    if (requireInbandSeek && !(m_hello.capabilities & XCL_CAP_INBAND_SEEK)) {
        close();
        m_lastError = "device does not advertise XCL_CAP_INBAND_SEEK";
        return LinkResult::MissingFeature;
    }

    return LinkResult::Ok;
}

void XCopyLink::close()
{
    m_hsCapture = false;

    if (m_port.isPortOpen()) {
        /* Best effort: stop the stream and hand the console back, so the next
           thing to open this port finds a device in its normal state rather than
           mid-stream. Failures here are not interesting - we are closing anyway. */
        streamStop();
        motor(false);
        bye();
        m_port.closePort();
    }
    m_rxHead = m_rxTail = 0;
    m_gotHello = false;
    memset(&m_hello, 0, sizeof(m_hello));
}

/* ------------------------------------------------------------------------- */
/* Commands                                                                   */
/* ------------------------------------------------------------------------- */

bool XCopyLink::sendFrame(uint8_t command, const uint8_t* payload, uint8_t length)
{
    if (!m_port.isPortOpen())
        return false;
    if (length > XCL_CMD_MAX_PAYLOAD)
        return false;

    uint8_t frame[XCL_CMD_MAX_FRAME];
    uint32_t n = 0;
    frame[n++] = XCL_CMD_SYNC0;
    frame[n++] = XCL_CMD_SYNC1;
    frame[n++] = command;
    frame[n++] = length;
    for (uint8_t i = 0; i < length; i++)
        frame[n++] = payload[i];
    /* crc8 covers cmd, len and payload - everything after the sync pair. */
    frame[n] = xclCrc8(&frame[2], (uint32_t)(2 + length));
    n++;

    return m_port.write(frame, n) == n;
}

bool XCopyLink::motor(bool on)
{
    const uint8_t p = on ? 1 : 0;
    return sendFrame(XCL_CMD_MOTOR, &p, 1);
}

bool XCopyLink::seek(unsigned int cylinder, bool side)
{
    const uint8_t p[2] = {(uint8_t)cylinder, (uint8_t)(side ? 1 : 0)};
    return sendFrame(XCL_CMD_SEEK, p, 2);
}

bool XCopyLink::recalibrate()
{
    return sendFrame(XCL_CMD_RECAL, nullptr, 0);
}

bool XCopyLink::streamStart(XclStreamMode mode, uint8_t flags)
{
    const uint8_t p[2] = {(uint8_t)mode, flags};
    return sendFrame(XCL_CMD_STREAM_START, p, 2);
}

bool XCopyLink::streamStop()
{
    return sendFrame(XCL_CMD_STREAM_STOP, nullptr, 0);
}

bool XCopyLink::readTrack(XclStreamMode mode, uint8_t maxIndex, uint16_t lingerMs,
                          uint32_t maxTicks)
{
    const uint8_t p[8] = {(uint8_t)mode,
                          maxIndex,
                          (uint8_t)(lingerMs & 0xFF),
                          (uint8_t)((lingerMs >> 8) & 0xFF),
                          (uint8_t)(maxTicks & 0xFF),
                          (uint8_t)((maxTicks >> 8) & 0xFF),
                          (uint8_t)((maxTicks >> 16) & 0xFF),
                          (uint8_t)((maxTicks >> 24) & 0xFF)};
    return sendFrame(XCL_CMD_READ_TRACK, p, 8);
}

bool XCopyLink::noClick()
{
    return sendFrame(XCL_CMD_NOCLICK, nullptr, 0);
}

bool XCopyLink::writeTrackRequest(uint32_t bytes, uint8_t flags, uint16_t crc)
{
    if (bytes == 0 || bytes > 0xFFFFu)
        return false;
    const uint8_t p[8] = {(uint8_t)(bytes & 0xFF),
                          (uint8_t)((bytes >> 8) & 0xFF),
                          flags,
                          (uint8_t)(crc & 0xFF),
                          (uint8_t)((crc >> 8) & 0xFF),
                          0,
                          0,
                          0};
    return sendFrame(XCL_CMD_WRITE_TRACK, p, 8);
}

bool XCopyLink::sendTrackData(const uint8_t* cells, uint32_t bytes)
{
    if (!m_port.isPortOpen() || cells == nullptr || bytes == 0)
        return false;

    const uint8_t preamble[XCL_BULK_PREAMBLE_BYTES] = {
        XCL_BULK_SYNC0, XCL_BULK_SYNC1, (uint8_t)(bytes & 0xFF), (uint8_t)((bytes >> 8) & 0xFF)};
    if (m_port.write(preamble, (unsigned int)sizeof(preamble)) != (unsigned int)sizeof(preamble))
        return false;

    /* Chunked rather than one 13 KB write, for two reasons. The write timeout is
       500 + 20 ms a byte, so a single call to a device that has stopped listening
       would block for four minutes; and the device is still emitting records the
       host has to keep taking, which is what the pump() below is for - a full OS
       receive buffer is how this deadlocks. */
    static const uint32_t kChunk = 2048;
    uint32_t sent = 0;
    while (sent < bytes) {
        const uint32_t n = (bytes - sent < kChunk) ? (bytes - sent) : kChunk;
        if (m_port.write(cells + sent, (unsigned int)n) != n)
            return false;
        sent += n;
        pump();
    }
    m_stats.bytesSent += bytes + sizeof(preamble);
    return true;
}

bool XCopyLink::requestStatus()
{
    return sendFrame(XCL_CMD_STATUS, nullptr, 0);
}

bool XCopyLink::config(XclConfigKey key, uint32_t value)
{
    const uint8_t p[5] = {(uint8_t)key, (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF),
                          (uint8_t)((value >> 16) & 0xFF), (uint8_t)((value >> 24) & 0xFF)};
    return sendFrame(XCL_CMD_CONFIG, p, 5);
}

bool XCopyLink::selfTest(bool on, bool paced)
{
    const uint8_t p[2] = {(uint8_t)(on ? 1 : 0), (uint8_t)(paced ? 1 : 0)};
    return sendFrame(XCL_CMD_SELFTEST, p, 2);
}

bool XCopyLink::ping()
{
    return sendFrame(XCL_CMD_PING, nullptr, 0);
}

bool XCopyLink::bye()
{
    return sendFrame(XCL_CMD_BYE, nullptr, 0);
}

/* ------------------------------------------------------------------------- */
/* Receive                                                                    */
/* ------------------------------------------------------------------------- */

uint32_t XCopyLink::bytesWaiting()
{
    return m_port.isPortOpen() ? m_port.getBytesWaiting() : 0;
}

uint32_t XCopyLink::pumpWait()
{
    if (!m_port.isPortOpen())
        return 0;

    if (m_port.getBytesWaiting() == 0) {
        /* Reclaim first so the byte lands at the tail of live data. */
        if (m_rxHead > 0) {
            const uint32_t live = m_rxTail - m_rxHead;
            if (live > 0)
                memmove(m_rx.data(), m_rx.data() + m_rxHead, live);
            m_rxHead = 0;
            m_rxTail = live;
        }
        if (m_rxTail >= (uint32_t)m_rx.size())
            return pump();

        /* One byte, blocking up to the port's read timeout. This is the wait; the
           bulk read in pump() below takes whatever arrived with it. Not the
           byte-at-a-time drain rule 1 forbids: that was reading a whole stream one
           byte per call, this is one call per idle period. */
        const uint32_t got = m_port.read(m_rx.data() + m_rxTail, 1);
        if (got == 0)
            return 0;
        if (m_hsCapture && m_hsLen < (uint32_t)sizeof(m_hsSample))
            m_hsSample[m_hsLen++] = m_rx[m_rxTail];
        m_rxTail += got;
        m_stats.bytesRead += got;
        return got + pump();
    }

    return pump();
}

uint32_t XCopyLink::pump()
{
    if (!m_port.isPortOpen())
        return 0;

    /* Reclaim the consumed prefix before asking for more. Doing this first means a
       single pump can always take everything the OS has, which is rule 1. */
    if (m_rxHead > 0) {
        const uint32_t live = m_rxTail - m_rxHead;
        if (live > 0)
            memmove(m_rx.data(), m_rx.data() + m_rxHead, live);
        m_rxHead = 0;
        m_rxTail = live;
    }

    uint32_t total = 0;
    for (;;) {
        const uint32_t space = (uint32_t)m_rx.size() - m_rxTail;
        if (space == 0)
            break;

        uint32_t waiting = m_port.getBytesWaiting();
        if (waiting == 0)
            break;
        if (waiting > space)
            waiting = space;

        const uint32_t got = m_port.read(m_rx.data() + m_rxTail, waiting);
        if (got == 0)
            break;

        if (m_hsCapture) {
            uint32_t n = got;
            if (n > (uint32_t)sizeof(m_hsSample) - m_hsLen)
                n = (uint32_t)sizeof(m_hsSample) - m_hsLen;
            memcpy(m_hsSample + m_hsLen, m_rx.data() + m_rxTail, n);
            m_hsLen += n;
        }

        m_rxTail += got;
        total += got;
        m_stats.bytesRead += got;

        /* Parse as we go: a big backlog would otherwise sit unparsed while we keep
           filling, and the buffer would hit its ceiling for no reason. */
        parseBuffer();

        if (m_rxHead > 0) {
            const uint32_t live = m_rxTail - m_rxHead;
            if (live > 0)
                memmove(m_rx.data(), m_rx.data() + m_rxHead, live);
            m_rxHead = 0;
            m_rxTail = live;
        }
    }

    parseBuffer();
    return total;
}

void XCopyLink::parseBuffer()
{
    for (;;) {
        uint32_t avail = m_rxTail - m_rxHead;
        if (avail < 2)
            return;

        const uint8_t* p = m_rx.data() + m_rxHead;

        if (p[0] != XCL_REC_SYNC0 || p[1] != XCL_REC_SYNC1) {
            /* Not a record boundary. Skip exactly one byte: skipping more could
               step over a real sync pair that begins inside what we discarded. */
            m_rxHead++;
            m_stats.droppedBytes++;
            continue;
        }

        if (avail < sizeof(XclRecordHeader))
            return; /* header not here yet */

        XclRecordHeader h;
        memcpy(&h, p, sizeof(h));

        /* A sync pair with a type we do not know is a false sync inside somebody
           payload, not a record. Two bytes of MFM data land on 0x9E 0xC5 roughly
           once every 64 KB, so this happens for real and must be cheap. */
        if (h.type < XCL_REC_MFM || h.type > XCL_REC_TEST) {
            m_rxHead++;
            m_stats.droppedBytes++;
            m_stats.resyncs++;
            continue;
        }

        const uint32_t payloadBytes = xclPayloadBytes(&h);
        const uint32_t total = (uint32_t)sizeof(XclRecordHeader) + payloadBytes + 2u;

        if (payloadBytes > kMaxRecordPayload) {
            m_rxHead++;
            m_stats.droppedBytes++;
            m_stats.resyncs++;
            continue;
        }

        if (avail < total)
            return; /* whole record not here yet */

        uint16_t wireCrc;
        memcpy(&wireCrc, p + sizeof(XclRecordHeader) + payloadBytes, sizeof(wireCrc));
        const uint16_t calcCrc = xclCrc16(p, (uint32_t)sizeof(XclRecordHeader) + payloadBytes);

        if (wireCrc != calcCrc) {
            /* Framed like a record but is not one. Resync from the next byte
               rather than trusting its length field. */
            m_rxHead++;
            m_stats.droppedBytes++;
            m_stats.crcErrors++;
            m_stats.resyncs++;
            continue;
        }

        dispatch(h, p + sizeof(XclRecordHeader), payloadBytes);
        m_stats.records++;
        m_rxHead += total;
    }
}

void XCopyLink::dispatch(const XclRecordHeader& h, const uint8_t* payload, uint32_t payloadBytes)
{
    /* HELLO is intercepted here rather than pushed at the sink, so the handshake
       works before the caller has attached one. */
    if (h.type == XCL_REC_HELLO && payloadBytes == sizeof(XclHello)) {
        memcpy(&m_hello, payload, sizeof(m_hello));
        m_gotHello = true;
        return;
    }

    if (!m_sink)
        return;

    switch (h.type) {
    case XCL_REC_MFM:
        m_sink->onCells(h, payload, payloadBytes);
        break;

    case XCL_REC_FLUX: {
        /* Cannot be cast in place: the payload lands at an arbitrary offset, so an
           ARM host would fault on the unaligned uint16 read and x86 would merely
           be slow. Copy through the aligned member scratch. */
        const uint32_t count = payloadBytes / 2u;
        if (m_fluxScratch.size() < count)
            m_fluxScratch.resize(count);
        memcpy(m_fluxScratch.data(), payload, count * 2u);
        m_sink->onFlux(h, m_fluxScratch.data(), count);
        break;
    }

    case XCL_REC_EVENT:
        if (payloadBytes == sizeof(XclEventPayload)) {
            XclEventPayload ev;
            memcpy(&ev, payload, sizeof(ev));
            m_sink->onEvent(h, ev);
        }
        break;

    case XCL_REC_STATUS:
        if (payloadBytes == sizeof(XclStatus)) {
            XclStatus st;
            memcpy(&st, payload, sizeof(st));
            m_sink->onStatus(st);
        }
        break;

    case XCL_REC_ACK:
    case XCL_REC_NAK:
        if (payloadBytes == sizeof(XclAck)) {
            XclAck ack;
            memcpy(&ack, payload, sizeof(ack));
            m_sink->onAck(ack, h.type == XCL_REC_NAK);
        }
        break;

    case XCL_REC_TEST:
        m_sink->onTest(h, payload, payloadBytes);
        break;

    default:
        break;
    }
}

bool XCopyLink::waitFor(unsigned int timeoutMs, const bool* flag)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pump() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (*flag)
            return true;
    }
    return *flag;
}

} /* namespace XCopy */
