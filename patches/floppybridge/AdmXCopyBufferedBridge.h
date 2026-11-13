/* AdmXCopyBufferedBridge.h - XCopy Standalone as a BUFFERED FloppyBridge driver.
 *
 * Driver index 5, "XCopy Standalone (buffered)". The same board and the same
 * binary link as driver 4 (AdmXCopyBridge, the phase-locked live stream), but
 * shaped like the Greaseweazle driver: a CommonBridgeTemplate subclass that asks
 * the device for a BOUNDED track read, feeds what comes back into FloppyBridge's
 * PLL and RotationExtractor, and lets the template serve the guest from its
 * revolution cache with the ordinary bridge modes (Fast / Compatible / Turbo /
 * Stalling), head cheating and auto-caching.
 *
 * Everything downstream of pll.submitFlux() is upstream's code, untouched. What
 * this class owns is the device side of readData(): choosing the bound, pumping
 * the link, and turning the device's already-decoded MFM cells back into the flux
 * timings the PLL expects.
 *
 * Where it beats the Greaseweazle, per uncached track (docs/floppy-xcopy-buffered.md):
 *
 *   - the capture bound is the MEASURED revolution plus an overlap sized from the
 *     measured sequence rate, not a flat 210 ms + 49 ms;
 *   - a read ENDS when the extractor has what it needs (XCL_CMD_STREAM_STOP,
 *     sub-millisecond), where a Greaseweazle read runs to its bound regardless;
 *   - a guest seek during a background read (head cheating) aborts the read in
 *     under a millisecond instead of draining up to a quarter of a second of flux;
 *   - the device decodes; the host receives 63 KB/s of cells, not 383 KB/s of
 *     flux, and the ACK/first-record path is ~2 ms because the firmware forces
 *     short USB packets (XCL_CFG_TX_FLUSH_US).
 *
 * Writing is supported and is OFF BY DEFAULT. A real disk is destructive and
 * irreversible to write, so the slot has to be attached with allowWrites (flags bit
 * 64 in the serialised config, "Allow writes to physical disk" in the Floppy Drives
 * window) before the guest is told the drive is writable at all. With it off the
 * guest sees a write-protected drive whatever the disk's own tab says. DD only - the
 * device's write buffer shares its scratch block with the live transmit ring and an
 * HD track does not fit. See docs/floppy-write-support.md.
 *
 * The write path is short for one reason worth stating: FloppyBridge hands writeData()
 * MFM bit cells MSB first, and that is EXACTLY what the device's writer clocks out.
 * The Greaseweazle driver spends 90 lines turning those cells into flux timings with
 * precompensation because a Greaseweazle writes flux; here the device's fixed-interval
 * timer is the encoder, so the cells go out as cells and nothing converts anything.
 *
 * DD only unless the bridge density mode forces HD; no density auto-detect.
 *
 * Requires firmware 0x0718 or later (XCL_CMD_READ_TRACK, XCL_EV_READ_DONE,
 * XCL_CMD_NOCLICK), and 0x0733 or later for writing (XCL_CMD_WRITE_TRACK, the bulk
 * transfer, XCL_EV_WRITE_DONE) - which is advertised as XCL_CAP_WRITE rather than
 * assumed from the version. Older firmware is refused at open with a message that says
 * so; firmware without XCL_CAP_WRITE opens read-only.
 */

#ifndef ADM_XCOPY_BUFFERED_BRIDGE_H
#define ADM_XCOPY_BUFFERED_BRIDGE_H

#include "CommonBridgeTemplate.h"
#include "XCopyLink.h"
#include "XCopyLiveProtocol.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

class AdmXCopyBufferedBridge : public CommonBridgeTemplate, private XCopy::XCopySink {
public:
    AdmXCopyBufferedBridge(FloppyBridge::BridgeMode bridgeMode,
                           FloppyBridge::BridgeDensityMode bridgeDensity, bool enableAutoCache,
                           bool useSmartSpeed, bool autoDetectComPort, char* comPort,
                           bool allowWrites);
    virtual ~AdmXCopyBufferedBridge();

    static const BridgeDriver* staticBridgeInformation();

protected:
    /* CommonBridgeTemplate hooks. All run on the template's worker thread except
       abortDiskReading() and isStillWorking(), which only touch atomics. */
    virtual const unsigned int getDriveSpinupTime() override { return 650; }
    virtual void checkDiskType() override;
    virtual void forceDiskDensity(bool forceHD) override;
    virtual void poll() override;
    virtual bool isStillWorking() override { return !m_ioError.load(); }
    virtual void abortDiskReading() override { m_abort.store(true); }
    virtual bool supportsDiskChange() override { return true; }
    virtual bool getDiskChangeStatus(const bool forceCheck) override;
    virtual void closeInterface() override;
    virtual bool openInterface(std::string& errorMessage) override;
    virtual bool checkWriteProtectStatus(const bool forceCheck) override;
    virtual const BridgeDriver* _getDriverInfo() override { return staticBridgeInformation(); }
    virtual const DriveTypeID _getDriveTypeID() override;
    virtual bool setActiveSurface(const DiskSurface activeSurface) override;
    virtual bool setMotorStatus(const bool switchedOn) override;
    virtual bool setCurrentCylinder(const unsigned int cylinder) override;
    virtual bool performNoClickSeek() override;
    virtual ReadResponse
    readData(PLL::BridgePLL& pll, const unsigned int maxBufferSize,
             RotationExtractor::MFMSample* buffer, RotationExtractor::IndexSequenceMarker& indexMarker,
             std::function<bool(RotationExtractor::MFMSample* mfmData, const unsigned int dataLengthInBits)>
                 onRotation) override;
    virtual ReadResponse readLinearData(PLL::BridgePLL& pll) override;
    virtual bool writeData(const unsigned char* rawMFMData, const unsigned int numBits,
                           const bool writeFromIndex, const bool suggestUsingPrecompensation) override;
    virtual bool attemptToDetectDiskChange() override;
    virtual bool canTurboWrite() override { return false; }

private:
    /* XCopySink - the link's record callbacks, all on the worker thread. */
    virtual void onCells(const XclRecordHeader& h, const uint8_t* packed, uint32_t bytes) override;
    virtual void onFlux(const XclRecordHeader& h, const uint16_t* ticks, uint32_t count) override;
    virtual void onEvent(const XclRecordHeader& h, const XclEventPayload& ev) override;
    virtual void onStatus(const XclStatus& st) override;
    virtual void onAck(const XclAck& ack, bool isNak) override;
    virtual void onTest(const XclRecordHeader& h, const uint8_t* data, uint32_t bytes) override {}

    using clock = std::chrono::steady_clock;

    /* One bounded capture, start to READ_DONE. The bound arithmetic is in
       chooseBound(); the pump loop is runCapture(). */
    struct Bound {
        uint8_t maxIndex = 0;
        uint16_t lingerMs = 0;
        uint32_t maxTicks = 0;
    };
    Bound chooseBound(PLL::BridgePLL& pll) const;
    void probeSequence(uint32_t zeros);
    /* pll may be null (a presence probe: nothing is decoded). */
    ReadResponse runCapture(PLL::BridgePLL* pll, const Bound& b, RotationExtractor::MFMSample* buffer,
                            unsigned int maxBufferSize, RotationExtractor::IndexSequenceMarker* indexMarker,
                            std::function<bool(RotationExtractor::MFMSample*, const unsigned int)>* onRotation,
                            const char* what);

    /* Waits, pumping the link, until flag is true or timeoutMs passes. */
    bool waitFlag(volatile bool& flag, unsigned int timeoutMs);
    /* Sends SEEK and waits for TRACK_CHANGE, re-issuing a BUSY-refused seek. */
    bool seekAndWait(unsigned int cylinder, bool upperSide);
    /* STATUS round trip; false if the device did not answer. */
    bool refreshStatus(unsigned int timeoutMs = 100);
    void noteCommandSent() { m_lastCmdSent = clock::now(); }
    void applyStatusBits(uint8_t bits);
    void logLine(const char* fmt, ...);

    XCopy::XCopyLink m_link;
    std::string m_comPort;
    std::string m_lastError;
    FloppyBridge::BridgeDensityMode m_densityMode;
    bool m_isHDDisk = false;

    std::atomic<bool> m_ioError{false};
    std::atomic<bool> m_abort{false};

    /* THE WRITE GATE. What the slot was attached with, and what the device says it
       can do. Both have to be true before the guest is shown a writable drive;
       m_writeProtect (the real /WPRO line) is the third. */
    bool m_allowWrites = false;
    bool m_deviceCanWrite = false;
    /* Set from XCL_EV_WRITE_DONE. Result is an XCL_RESULT_*. */
    volatile bool m_gotWriteDone = false;
    uint8_t m_writeResult = 0;
    uint32_t m_writeElapsedMs = 0;
    /* Written so the guest is told at once rather than after the next read. */
    bool writeProtectedForGuest() const { return !m_allowWrites || m_writeProtect; }

    /* Where the device says the head is (from TRACK_CHANGE), or -1 until the
       first recalibrate answers. Kept apart from what was asked for. */
    int m_hwCylinder = -1;
    bool m_hwUpperSide = false;
    bool m_motorOn = false;

    /* Latest drive status bits (XCL_ST_*), from any event/status record. */
    uint8_t m_statusBits = 0;
    bool m_diskPresent = false;
    bool m_writeProtect = false;
    clock::time_point m_lastIndexSeen{};
    clock::time_point m_lastStatusAsked{};
    clock::time_point m_lastCmdSent{};

    /* Handshake flags for the synchronous waits. */
    volatile bool m_gotTrackChange = false;
    volatile bool m_gotStatus = false;
    volatile bool m_gotAck = false;
    volatile bool m_gotReadDone = false;
    uint8_t m_ackCommand = 0;
    uint8_t m_ackResult = 0;
    uint8_t m_readDoneReason = 0;
    uint32_t m_readDonePulses = 0;

    /* The capture in progress. */
    PLL::BridgePLL* m_pll = nullptr;
    bool m_capturing = false;
    uint32_t m_zeros = 0;           /* zero cells since the last transition          */
    bool m_haveIndexCell = false;   /* an INDEX event is waiting for its cell        */
    uint32_t m_indexCell = 0;
    /* THE SEAM. A one-revolution buffer joins two readings of the same angle at
       its wrap, and the extractor aligns that join by matching 1024 sequences
       taken at its anchor - the capture start in timed mode, the index pulse in
       index mode. Inside a run of zero-filled sectors (all mfm01) every offset
       matches, the join slips by the pulse jitter, and the sector spanning the
       wrap fails its checksum on every read (docs 13b; measured here as sector 8
       of track 1, ~40% of reads). Both anchors are therefore moved onto the first
       0x4489 sync word: the sync and the header behind it are unique, so the
       match is exact and the wrap is seamless. Costs at most one sector pitch of
       extra capture (~17 ms, covered by the early stop), and moves the emulated
       index to that sync - which AmigaDOS does not care about (driver 4 moved
       its boundary into the gap for the same reason and the protected disk loads). */
    bool m_feedOpen = false;        /* timed mode: PLL fed only from the first sync  */
    bool m_indexArmed = false;      /* pulse seen: flag the index at the next sync   */
    uint32_t m_armCells = 0;        /* cells since the pulse, for the no-sync cap    */
    uint32_t m_shift = 0;           /* last 32 cells, for the sync detector          */
    /* Cells still to go from the last sync word to the anchor point. The anchor
       is placed INSIDE the header that follows the sync pair, not on the sync
       itself: the extractor rotates its output to start at the anchor, and an
       anchor on the sync's last transition split that sync across the wrap and
       lost the sector on every read. */
    uint32_t m_anchorCountdown = 0;
    uint32_t m_capCells = 0;        /* cells received this capture                   */
    uint32_t m_capTransitions = 0;  /* transitions (ones) this capture               */
    uint32_t m_capIndexEvents = 0;
    uint64_t m_capTicks = 0;
    uint32_t m_capRecords = 0;
    uint32_t m_capBadSpans = 0;     /* records whose span was rejected as implausible */
    uint32_t m_capFirstTicks = 0;   /* the first record's claimed span and size       */
    uint32_t m_capFirstCount = 0;

    /* What the device has taught us about this drive, for the bound. */
    uint32_t m_revTicks = 0;        /* last index-to-index period, device ticks      */
    uint32_t m_tickHz = 24000000;
    double m_nsPerSequence = 8000.0;/* measured mean transition spacing, ns          */
    uint32_t m_overlapWindow = 128; /* extractor overlap search window, sequences    */
    uint32_t m_overlapMatches = 256;/* extractor overlap marker length, sequences    */

    /* Adaptive anchor (docs/floppy-xcopy-buffered.md 8e). A timed read feeds the
       PLL from its first transition and records the first marker+window sequence
       classes; if that stretch matches itself at any shift within the window the
       join would be ambiguous, and the read re-anchors on the next sync word the
       way every read used to. */
    bool m_probeEnable = true;      /* ADM_XCB_PROBE                                 */
    bool m_probe = false;           /* probing the capture start                     */
    uint32_t m_probeCount = 0;
    uint8_t m_probeVerdict = 0;     /* 0 not probed, 1 unique, 2 ambiguous (re-anchored) */
    uint8_t m_probeSeq[OVERLAP_SEQUENCE_MATCHES * OVERLAP_EXTRA_BUFFER];
    RotationExtractor::IndexSequenceMarker* m_capIndexMarker = nullptr;

    /* Per-read timing for the log. */
    clock::time_point m_capStart{};
    double m_capFirstMs = -1, m_capExtractMs = -1, m_capStopMs = -1;
    unsigned int m_capRotations = 0;

    FILE* m_log = nullptr;
};

#endif /* ADM_XCOPY_BUFFERED_BRIDGE_H */
