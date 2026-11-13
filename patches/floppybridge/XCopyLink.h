/* XCopyLink - the host end of the XCopy Standalone live-drive link.
 *
 * Transport only. It opens the port, shakes hands, sends commands, and turns the
 * inbound byte stream into records. It knows nothing about UAE, tracks, laps or
 * rotational angle - AdmXCopyBridge does all of that on top.
 *
 * Two rules this class exists to enforce, both learned the hard way on the
 * Greaseweazle path (docs/floppy.md 12i.1, 12o):
 *
 *   1. BULK READS ONLY. Upstream's GreaseWeazleInterface dropped to one byte per
 *      read once an abort was requested, so cancelling a ~259 ms flux stream
 *      drained it a byte at a time over USB while a queued seek waited. Every
 *      read here is bounded by getBytesWaiting() and takes everything at once.
 *
 *   2. RESYNC IS SILENT AND CHEAP. The device never stops to handshake, so the
 *      host must be able to pick the stream back up from any byte offset. Records
 *      are found by their sync pair, validated by CRC, and positioned by their own
 *      absolute index field - never by counting from a previous record.
 *
 * Threading: one thread calls pump() and the send* methods. Nothing here is
 * thread-safe by itself; AdmXCopyBridge owns an instance from its reader thread.
 */

#ifndef XCOPYLINK_H
#define XCOPYLINK_H

#include <cstdint>
#include <string>
#include <vector>

#include "SerialIO.h"
#include "XCopyLiveProtocol.h"

namespace XCopy {

/* Largest payload we will believe. A record claiming more than this is taken as a
   false sync rather than a real header, and we resync from the next byte. The
   device also reports its real figure in XclHello::maxRecordBytes, which is
   checked against this at handshake time. */
static const uint32_t kMaxRecordPayload = 8192;

/* Inbound staging buffer. Flux mode is ~400 KB/s, so even a 60 Hz pump only needs
   ~7 KB per call; this is slack, not a design parameter. */
static const uint32_t kRxBufferBytes = 64 * 1024;

/* Teensy 3.x USB vendor ID, used only to LABEL a port in the UI. Never to pick one
   automatically: AttachBridgeDrive refuses an empty COM port because an auto-scan
   costs 819 ms on the emulation thread (docs/floppy.md, live-attach notes). */
static const unsigned int kTeensyVid = 0x16C0;

enum class LinkResult {
    Ok,
    PortNotFound,
    PortInUse,
    PortError,
    NoResponse,     /* opened, but nothing answered XCL_CMD_HELLO             */
    BadVersion,     /* answered, but speaks a protocol version we do not      */
    NotXCopy,       /* answered, but is not an XCopy Standalone               */
    MissingFeature, /* answered, but cannot seek mid-stream - see below       */
    WriteError
};

const char* toString(LinkResult r);

/* What a caller gets told about the stream. Every callback runs on the thread that
   called pump(). None of them may block: in the bridge, that thread is the only
   thing draining USB, and a stall there corrupts the angle mapping. */
class XCopySink {
public:
    virtual ~XCopySink() {}

    /* Packed MFM cells, MSB first. h.index is the absolute cell index of the first
       cell; h.count is how many; h.ticks is how long they took, which is what
       becomes getMFMSpeed(). */
    virtual void onCells(const XclRecordHeader& h, const uint8_t* packed, uint32_t bytes) = 0;

    /* Raw flux, uint16 tick deltas. h.index is an absolute TICK counter here, not
       a cell counter. */
    virtual void onFlux(const XclRecordHeader& h, const uint16_t* ticks, uint32_t count) = 0;

    virtual void onEvent(const XclRecordHeader& h, const XclEventPayload& ev) = 0;
    virtual void onStatus(const XclStatus& st) = 0;
    virtual void onAck(const XclAck& ack, bool isNak) = 0;
    virtual void onTest(const XclRecordHeader& h, const uint8_t* data, uint32_t bytes) = 0;
};

class XCopyLink {
public:
    struct Stats {
        uint64_t bytesRead = 0;
        uint64_t bytesSent = 0;  /* bulk track data only, not command frames     */
        uint64_t records = 0;
        uint64_t resyncs = 0;    /* times we lost and re-found the sync pair    */
        uint64_t crcErrors = 0;  /* records that framed but failed CRC          */
        uint64_t droppedBytes = 0;
    };

    /* One entry per serial port on the machine. */
    struct PortInfo {
        std::string port;
        unsigned int vid = 0;
        unsigned int pid = 0;
        std::string product;
        bool looksLikeXCopy = false; /* VID matches a Teensy - a hint, not proof */
    };

    XCopyLink();
    ~XCopyLink();

    XCopyLink(const XCopyLink&) = delete;
    XCopyLink& operator=(const XCopyLink&) = delete;

    /* Opens the port, walks the device out of its ASCII console into binary mode,
       and completes the XCL_CMD_HELLO handshake. On success hello() is populated.

       requireInbandSeek defaults true: a device that cannot seek mid-stream is
       useless to this driver - that single capability is the entire reason this
       backend exists - so it is rejected at open time rather than producing a
       driver that silently behaves like the Greaseweazle one. */
    LinkResult open(const std::string& comPort, bool requireInbandSeek = true);
    void close();
    bool isOpen() const { return m_port.isPortOpen(); }

    const XclHello& hello() const { return m_hello; }
    const std::string& lastError() const { return m_lastError; }

    void setSink(XCopySink* sink) { m_sink = sink; }

    /* Drains everything the OS has buffered and dispatches whole records to the
       sink. Returns bytes consumed. Never blocks: it reads exactly what
       getBytesWaiting() reports and no more, so a caller with nothing to do should
       sleep rather than spin. */
    uint32_t pump();

    /* pump(), but if the OS has nothing buffered, first BLOCKS until one byte
       arrives or the port's read timeout (~11 ms) passes. Wakes on data rather
       than on a timer: a 1 ms sleep on Windows is really 1-16 ms depending on the
       process's timer resolution, and measured on the bounded-read path that
       granularity was the whole of a ~15 ms command-to-first-record latency and
       a 16 ms STREAM_STOP-to-READ_DONE tail. For a caller that is waiting on the
       device and has nothing else to do - the buffered driver, on its own worker
       thread - this is the right wait. The streaming driver keeps pump()+sleep,
       because its thread must also service its own state between records. */
    uint32_t pumpWait();

    /* Bytes the OS has buffered that pump() has not taken yet. Zero in the steady
       state; a persistent backlog here means the host is reading the stream
       behind the device, and everything served to the guest is that stale. */
    uint32_t bytesWaiting();

    /* Commands. All are valid mid-stream; seek() in particular is the whole point.
       Each returns false only if the bytes could not be written to the port. */
    bool motor(bool on);
    bool seek(unsigned int cylinder, bool side);
    bool recalibrate();
    bool streamStart(XclStreamMode mode, uint8_t flags = 0);
    bool streamStop();

    /* A bounded capture (XCL_CMD_READ_TRACK, firmware 0x0718+). The stream ends by
       itself with XCL_EV_READ_DONE; streamStop() ends it early. See the protocol
       header for the exact meaning of the three bounds. */
    bool readTrack(XclStreamMode mode, uint8_t maxIndex, uint16_t lingerMs, uint32_t maxTicks);
    /* One outward step at track 0 to re-arm /DSKCHG (XCL_CMD_NOCLICK, 0x0718+). */
    bool noClick();

    /* A track write (XCL_CMD_WRITE_TRACK, firmware 0x0733+), in two halves,
       because the device's ACK sits between them and this class deliberately does
       not own a wait loop - the caller's does.

         1. writeTrackRequest() asks. The device answers XCL_REC_ACK/OK to mean
            "send it" and XCL_REC_NAK with a reason not to; NOTHING may be sent
            until that answer has arrived.
         2. sendTrackData() sends the preamble and the cells.
         3. The caller waits for XCL_EV_WRITE_DONE, about a revolution later.

       `cells` is MFM bit cells, MSB first - exactly what FloppyBridge hands to
       writeData(), with no conversion, because the device clocks the cells out at
       a fixed interval and is itself the encoder. `bytes` must not exceed
       XclHello::ringBytes. crc16() below is what the request has to carry. */
    bool writeTrackRequest(uint32_t bytes, uint8_t flags, uint16_t crc);
    bool sendTrackData(const uint8_t* cells, uint32_t bytes);
    /* CRC-16/CCITT-FALSE, the one the device will check the transfer against. */
    static uint16_t crc16(const uint8_t* data, uint32_t bytes) { return xclCrc16(data, bytes); }
    bool requestStatus();
    bool config(XclConfigKey key, uint32_t value);
    /* paced = platter-paced (~63 KB/s, the MFM stream's shape) rather than full
       rate; delivery-latency measurements need paced, throughput needs full. */
    bool selfTest(bool on, bool paced = false);
    bool ping();
    bool bye();

    const Stats& stats() const { return m_stats; }
    void resetStats() { m_stats = Stats(); }

    static void enumPorts(std::vector<PortInfo>& out);

private:
    bool sendFrame(uint8_t command, const uint8_t* payload, uint8_t length);

    /* Parses as many whole records as the staging buffer holds. Everything that is
       not a valid record is skipped one byte at a time so a false sync inside a
       payload cannot swallow the real record that follows it. */
    void parseBuffer();
    void dispatch(const XclRecordHeader& h, const uint8_t* payload, uint32_t payloadBytes);

    /* Pumps until predicate is satisfied or the deadline passes. Used only by the
       handshake - the streaming path never waits for anything. */
    bool waitFor(unsigned int timeoutMs, const bool* flag);

    SerialIO m_port;
    XCopySink* m_sink = nullptr;
    XclHello m_hello;
    std::string m_lastError;
    Stats m_stats;

    std::vector<uint8_t> m_rx;
    uint32_t m_rxHead = 0; /* first unconsumed byte  */
    uint32_t m_rxTail = 0; /* first unfilled byte    */

    /* Set when a HELLO record arrives. dispatch() intercepts that type before it
       consults m_sink, so the handshake completes with no sink attached. */
    bool m_gotHello = false;

    /* A sample of whatever arrives during the handshake, so a failure can say what
       the device actually sent instead of only that it was not a HELLO. The
       difference between "silence" and "an ASCII error message" is the difference
       between a dead port and a console that did not switch. */
    bool m_hsCapture = false;
    uint32_t m_hsLen = 0;
    uint8_t m_hsSample[64];
    std::string handshakeSample() const;

    /* Flux payloads are uint16 on the wire but land at an arbitrary offset in the
       staging buffer, so they are copied through here rather than cast in place.
       A member, not a local: this class is single-threaded by contract and the
       buffer should be allocated once, not once per record. */
    std::vector<uint16_t> m_fluxScratch;
};

} /* namespace XCopy */

#endif /* XCOPYLINK_H */
