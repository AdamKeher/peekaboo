/* AdmXCopyBridge - live drive over an XCopy Standalone controller. Driver 4.
 *
 * A FloppyDiskBridge implemented directly (not through CommonBridgeTemplate), so
 * disk.c needs no change at all. Read-only: writing stays with the buffered
 * drivers, exactly as for driver 3.
 *
 * WHAT IS DIFFERENT FROM AdmStreamingBridge (driver 3)
 * ----------------------------------------------------
 * Driver 3 has to RECONSTRUCT rotational position, because a Greaseweazle read is
 * a bounded request that cannot be cancelled: the flux stream stops and restarts
 * for every head move, so absolute angle is never known. It decodes into a linear
 * scratch, correlates for where the flux repeats, cuts there (not at the index -
 * a revolution is not a whole number of cells, so cutting at the index leaves a
 * fractional cell of slip that corrupts the sector spanning it), and rotates the
 * result so the index lands at position 0.
 *
 * Here, angle is GIVEN. The stream never stops, every record carries a
 * free-running absolute cell index, and every index pulse arrives as an event
 * carrying the exact cell index it fired at. So:
 *
 *   angle = cellIndex - cellIndexOfLastIndexPulse
 *
 * and that is the whole of it. No correlation, no rotation, no join. Each lap is
 * simply the cells between two index events, and is published at exactly its own
 * measured length - which is how the 101,306-101,360 cell lap-to-lap variation
 * (docs/floppy.md 12f) is handled by construction rather than by clamping.
 *
 * Consequently NONE of these exist here, and should not be reintroduced:
 * m_trackLenLocked, anchor()/isAnchored(), kNominalDDTrackBits, m_seenIndex, the
 * overlap search, or endOfStream().
 *
 * WHAT IS KEPT FROM DRIVER 3, DELIBERATELY
 * ----------------------------------------
 * Three buffers and promote-at-the-index (docs/floppy.md 12n.5). Two revolutions
 * of the same track do not line up bit for bit, because the drive's speed varies,
 * so swapping the served buffer anywhere except the index puts a seam mid-sector.
 * The index is the one point on the platter where the end of a revolution really
 * is adjacent to the start of one.
 *
 * The guest's revolution boundary is detected by COVERAGE, not by the position
 * going backwards: UAE's DMA prediction jumps mfmPositionBits around ~775 times
 * per revolution, so "pos < previous pos" fires constantly. An early version of
 * driver 3's diagnostic was fooled by exactly this.
 *
 * And the three answers that must be about the GUEST, not the hardware
 * (docs/floppy.md 12g): isMotorRunning(), isAtCylinder0() and
 * getCurrentCylinderNumber(). disk.c's drive_running() is the first and gates the
 * entire disk path; Kickstart's track-0 recalibration never terminates without the
 * second.
 */

#ifndef ADMXCOPYBRIDGE_H
#define ADMXCOPYBRIDGE_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "XCopyLink.h"
#include "floppybridge_abstract.h"
#include "floppybridge_common.h"

/* Compile-time diagnostic, off by default, mirroring ADM_STREAM_DIAG on driver 3.
   It writes to C:/temp/admxcopy.log.

   The lesson from docs/floppy.md 12o is not optional: driver 3's diagnostic cost
   real work per emulated bit cell, which slowed the emulated machine against a
   platter turning at real speed and gave the driver slack it does not otherwise
   have. Boots went 7-of-9 to 15-of-15 with it on. NO TIMING OR BOOT FIGURE TAKEN
   WITH THIS ENABLED MEANS ANYTHING. */
#ifndef ADM_XCOPY_DIAG
#define ADM_XCOPY_DIAG 0
#endif

/* A bit the guest reads where we have nothing. 1, matching CommonBridgeTemplate:
   a run of ones is what a real drive gives you off an unsettled head, and it
   contains no sync word, so the guest simply keeps looking. */
#define ADM_XC_GARBAGE_BIT 1

/* Sizing. A DD revolution on real hardware is ~101,320 cells; 115,000 is driver
   3's upper clamp and leaves room for a slow drive or a long-track protection. */
static const unsigned int kXcMaxLapCells = 115000;
static const unsigned int kXcLapBytes = (kXcMaxLapCells + 7) / 8;
static const unsigned int kXcSpeedGroup = 1024; /* cells per speed slot */
static const unsigned int kXcSpeedSlots = (kXcMaxLapCells / kXcSpeedGroup) + 2;

/* Placeholder track length, used only between attach and the first index event.

   102,011 is the median the device measured over 149 laps on this drive (199.788
   ms, 300.32 RPM, 0.37% spread). Deliberately NOT 100,000: that figure is known
   wrong on real hardware, and a driver that starts from it looks correct for one
   revolution and then drifts. Replaced by a measured value within ~200 ms of the
   motor reaching speed, so this only has to be close. */
static const unsigned int kXcPlaceholderRevCells = 102011;

/* findGapOffset's "no gap found" answer. Distinct from 0, which means "the
   boundary is already at the gap's midpoint" and is a success. */
static const uint32_t kXcNoGap = 0xFFFFFFFFu;

/* One revolution of one surface, as delivered by the device. */
struct AdmXCopyLap {
    uint8_t cells[kXcLapBytes];
    uint16_t speed[kXcSpeedSlots]; /* per kXcSpeedGroup cells, 1000 = lap mean */
    uint32_t lengthCells;          /* MEASURED, index to index                  */
    int trackTag;                  /* cyl * 2 + side, or -1                     */

    void reset(int tag)
    {
        lengthCells = 0;
        trackTag = tag;
        /* speed MUST be cleared. closeLap() reads 0 as "no timing for this slot"
           and substitutes 1000; leaving the previous lap's raw tick counts in
           place would instead normalise them against THIS lap's mean, which is
           plausible-looking and wrong.

           cells is deliberately NOT cleared - it is 14 KB, and the covered range
           [m_backFrom, m_backTo) already stops anything unwritten being served. */
        memset(speed, 0, sizeof(speed));
    }

    inline bool bit(unsigned int pos) const
    {
        return (cells[pos >> 3] >> (7 - (pos & 7))) & 1;
    }

    inline void setBit(unsigned int pos, bool v)
    {
        const uint8_t mask = (uint8_t)(1u << (7 - (pos & 7)));
        if (v)
            cells[pos >> 3] |= mask;
        else
            cells[pos >> 3] &= (uint8_t)~mask;
    }
};

/* Per-lap and per-promote instrumentation.
 *
 * NOT behind ADM_XCOPY_DIAG, deliberately. The §12o lesson is that a diagnostic
 * doing work per emulated BIT CELL slows the guest against a platter turning at
 * real speed and changes the thing being measured - driver 3 booted 15/15 with
 * its diagnostic on and 7/9 with it off. This one writes a handful of words per
 * revolution (about five a second) and never touches getMFMBit's hot path, so it
 * cannot move the timing it is there to explain. */
struct XcLapRecord {
    uint32_t nominal;   /* index-to-index length this lap                       */
    uint32_t chosen;    /* what the correlator actually cut at                  */
    uint32_t score;     /* matching cells, out of kJoinMatch                    */
    uint32_t from, to;  /* covered arc when the lap closed                      */
    uint32_t published; /* 1 if it became a servable revolution                 */
};

static const unsigned int kXcLogSize = 128;

class AdmXCopyBridge : public FloppyDiskBridge, private XCopy::XCopySink {
public:
    AdmXCopyBridge(FloppyBridge::BridgeDensityMode densityMode, bool autoDetectComPort,
                   const char* comPort, FloppyBridge::DriveSelection driveCable);
    virtual ~AdmXCopyBridge();

    static const FloppyDiskBridge::BridgeDriver* staticBridgeInformation();

    /* --- lifecycle ------------------------------------------------------- */
    virtual bool initialise() override;
    virtual void shutdown() override;
    virtual const BridgeDriver* getDriverInfo() override { return staticBridgeInformation(); }
    virtual const char* getLastErrorMessage() override { return m_lastError.c_str(); }
    virtual bool isStillWorking() override { return !m_ioError; }
    virtual unsigned char getBitSpeed() override { return 2; }
    virtual DriveTypeID getDriveTypeID() override { return DriveTypeID::dti35DD; }

    /* --- head ------------------------------------------------------------ */
    virtual bool resetDrive(int trackNumber) override;

    /* Answers for the GUEST, synchronously. Reporting the hardware's achieved
       position here left Kickstart stepping the head outward forever, because it
       re-checks faster than any physical seek completes. */
    virtual bool isAtCylinder0() override { return m_wantCylinder.load() == 0; }
    virtual unsigned char getMaxCylinder() override { return 82; }
    virtual void gotoCylinder(int cylinderNumber, bool side) override;
    virtual void handleNoClickStep(bool side) override;
    virtual unsigned char getCurrentCylinderNumber() override
    {
        return (unsigned char)m_wantCylinder.load();
    }
    virtual void setSurface(bool side) override;
    virtual bool getCurrentSide() override { return m_wantSide.load(); }

    /* --- motor and media -------------------------------------------------- */
    virtual bool isMotorRunning() override { return m_motorRunning.load(); }
    virtual void setMotorStatus(bool side, bool turnOn) override;
    virtual bool isReady() override;

    /* Never cleared by a pessimistic check: UAE will not spin the motor without
       media, and media detection needs the motor spinning. One wrong answer
       deadlocks the drive (docs/floppy.md 12g). */
    virtual bool isDiskInDrive() override { return m_diskInDrive.load(); }
    virtual bool hasDiskChanged() override { return !m_diskInDrive.load(); }

    /* --- reading ---------------------------------------------------------- */
    virtual bool isMFMPositionAtIndex(int mfmPositionBits) override;
    virtual bool isMFMDataAvailable() override;
    virtual bool getMFMBit(const int mfmPositionBits) override;
    virtual int getMFMSpeed(const int mfmPositionBits) override;
    virtual void mfmSwitchBuffer(bool side) override;
    virtual int maxMFMBitPosition() override
    {
        /* Live mode serves from the delay line, whose modulus is frozen per
           dwell - the guest's revolution must wrap at exactly that modulus or
           the wrap itself becomes a seam (docs/floppy-xcopy.md 8o). */
        if (m_liveServe) {
            const unsigned int dl = m_delayLen.load(std::memory_order_relaxed);
            if (dl)
                return (int)dl;
        }
        return (int)m_trackLenCells.load();
    }
    virtual int getMFMTrack(bool side, unsigned int track, bool resyncRotation,
                            const int bufferSizeInBytes, void* output) override;
    virtual bool setDirectMode(bool directModeEnable) override;

    /* --- writing: refused, as driver 3 does ------------------------------- */
    virtual bool writeMFMTrackToBuffer(bool side, unsigned int track, bool writeFromIndex,
                                       int sizeInBytes, void* mfmData) override;
    virtual void writeShortToBuffer(bool side, unsigned int track, unsigned short mfmData,
                                    int mfmPosition) override;
    virtual bool isWriteProtected() override { return true; }
    virtual unsigned int commitWriteBuffer(bool side, unsigned int track) override;
    virtual bool isWritePending() override { return false; }
    virtual bool isWriteComplete() override { return false; }
    virtual bool canTurboWrite() override { return false; }
    virtual bool isReadyToWrite() override { return false; }

    /* --- accessors for scripts/ harnesses --------------------------------- */
    unsigned int debugPublishCount() const { return m_publishCount.load(); }
    unsigned int debugMeasuredRev() const { return m_measuredRevCells.load(); }
    unsigned int debugBackFrom() const { return m_backFrom.load(); }
    unsigned int debugBackTo() const { return m_backTo.load(); }
    unsigned int debugOverruns() const { return m_overruns.load(); }
    /* Torn reads tolerated - written with plain increments on the emulation
       thread; see the definitions. */
    unsigned int debugStallCount() const { return m_stallCount; }
    unsigned int debugStallSleeps() const { return m_stallSleeps; }
    unsigned int debugCellsLost() const { return m_cellsLost.load(); }
    unsigned int debugUnstableRecords() const { return m_unstableRecords.load(); }

    /* The last STATUS the device sent, so a harness can compare what the DEVICE
       reported against what the driver concluded from it. */
    struct DebugStatus {
        bool media;
        bool motor;
        bool diskChange;
        unsigned int cyl;
        unsigned int lastRevolutionCells;
    };
    DebugStatus debugLastStatus() const
    {
        DebugStatus d;
        const unsigned int s = m_lastStatusBits.load();
        d.media = (s & XCL_ST_MEDIA) != 0;
        d.motor = (s & XCL_ST_MOTOR) != 0;
        d.diskChange = (s & XCL_ST_DISKCHANGE) != 0;
        d.cyl = m_hwCylinder.load() < 0 ? 0u : (unsigned int)m_hwCylinder.load();
        d.lastRevolutionCells = m_measuredRevCells.load();
        return d;
    }

    /* The instrument. Ring buffers, so read them soon after the run that filled
       them; count() is the total ever written, not the number retained. */
    unsigned int debugLapCount() const { return m_lapLogCount.load(); }
    XcLapRecord debugLap(unsigned int i) const { return m_lapLog[i % kXcLogSize]; }
    unsigned int debugPromoteCount() const { return m_promoteLogCount.load(); }
    /* The lap boundary starts every track change in the WRONG place - it chains
       from the previous surface, whose gap sits at a different angle - and stays
       there until a published lap has been measured and the shift has worked
       through the anchor. Every read in that window loses whichever sector the
       boundary happens to land in. These three say when that window closes. */
    bool debugPhaseAligned() const { return m_phaseAlignedFlag.load(); }
    /* THE DEVICE'S CELL COUNTER IS A CLOCK. It advances at the platter's rate,
       about one cell every 2 us, so the difference between the last cell the host
       had processed when a seek was asked for and the cell the device stamped on
       XCL_EV_TRACK_CHANGE says how long the move took AT THE DEVICE. Comparing
       that against the host's own wall clock separates "the head really took this
       long" from "the host was reading the stream this far behind". Nothing else
       distinguishes those two, and they call for opposite fixes. */
    struct DebugSeekTrace {
        uint32_t reqCell, landCell;
        uint64_t reqMs, sentMs, landMs;
        /* The device's own measure of the head move, in capture ticks (24 per
           microsecond at DD). It rides on XCL_EV_TRACK_CHANGE, so the physical
           move can be separated from everything the firmware does around it. */
        uint32_t deviceTicks;
        /* When the device's ACK for the seek, and its SEEK_BEGIN event, reached
           the host. BOTH ARE EMITTED IMMEDIATELY by the firmware's command
           handler, while XCL_EV_TRACK_CHANGE is deliberately held back until the
           decoder has consumed the capture up to the sample the head landed on.
           So ack -> TRACK_CHANGE, minus the head move the device itself reports,
           is how far behind real time the device's decoder is running. Nothing
           host-side can be blamed for it and nothing else measures it. */
        uint64_t ackMs, beginMs;
        unsigned int seq;
    };
    DebugSeekTrace debugSeekTrace() const
    {
        DebugSeekTrace t;
        t.reqCell = m_seekReqCell.load();
        t.landCell = m_seekLandCell.load();
        t.reqMs = m_seekReqMs.load();
        t.sentMs = m_seekSentMs.load();
        t.landMs = m_seekLandMs.load();
        t.deviceTicks = m_seekDeviceTicks.load();
        t.ackMs = m_seekAckMs.load();
        t.beginMs = m_seekBeginMs.load();
        t.seq = m_seekTraceSeq.load();
        return t;
    }
    uint32_t debugLastCellIndex() const { return m_lastCellIndex.load(); }
    /* Round trip of the periodic STATUS request, in milliseconds: request
       written to reply parsed. Commands, replies and cell records share one
       stream, so a reply cannot overtake the data queued in front of it.
       ONLY MEANINGFUL BECAUSE THE REQUEST IS PAIRED WITH THE FIRST REPLY AFTER
       IT. An earlier version simply stamped every request and subtracted at every
       reply, and the device also states its status unprompted - so what it
       actually reported was the beat between two independent periodic events, a
       clean 15-30-45-61-77-15 sawtooth that reads exactly like a lag growing
       without bound. It is not. */
    unsigned int debugStatusRttMs() const { return m_statusRttMs.load(); }
    /* Bytes sitting unread in the OS receive buffer, sampled once per pass of the
       reader thread. This is where a host-side lag would be parked, and it tells
       a host that cannot keep up apart from a device that is buffering. */
    unsigned int debugRxWaiting() const { return m_rxWaiting.load(); }
    unsigned int debugPhaseShift() const { return m_phaseShift.load(); }
    /* What findGapOffset() actually saw the last time it ran: how many sector sync
       pairs, the median span it took for the sector pitch, and the gap it deduced
       from the longest span. Written once per track change. */
    unsigned int debugGapSyncs() const { return m_gapSyncs.load(); }
    unsigned int debugGapPitch() const { return m_gapPitch.load(); }
    unsigned int debugGapLen() const { return m_gapLen.load(); }
    unsigned int debugGapSyncAt() const { return m_gapSyncAt.load(); }
    unsigned int debugPhaseAlignSeq() const { return m_phaseAlignSeq.load(); }
    unsigned int debugPromotePos(unsigned int i) const { return m_promoteLog[i % kXcLogSize]; }
    /* Where the HARDWARE says the head is, from XCL_EV_TRACK_CHANGE - as opposed
       to getCurrentCylinderNumber(), which answers for the guest. The two
       disagreeing is the fault above. */
    int debugHwCylinder() const { return m_hwCylinder.load(); }
    bool debugHwSide() const { return m_hwSide.load(); }
    bool debugSeekInFlight() const { return m_seekInFlight.load(); }
    unsigned int debugGeneration() const { return m_generation.load(); }
    bool debugIoError() const { return m_ioError.load(); }
    const XCopy::XCopyLink::Stats& debugLinkStats() const { return m_link.stats(); }

private:
    int wantedTrackKey() const { return m_wantCylinder.load() * 2 + (m_wantSide.load() ? 1 : 0); }

    /* --- XCopySink, all called on the reader thread ----------------------- */
    void onCells(const XclRecordHeader& h, const uint8_t* packed, uint32_t bytes) override;
    void onFlux(const XclRecordHeader& h, const uint16_t* ticks, uint32_t count) override;
    void onEvent(const XclRecordHeader& h, const XclEventPayload& ev) override;
    void onStatus(const XclStatus& st) override;
    void onAck(const XclAck& ack, bool isNak) override;
    void onTest(const XclRecordHeader& h, const uint8_t* data, uint32_t bytes) override;

    void threadMain();
    /* Finish the lap at the chain-predicted boundary `cutCell`; `pulseCell` is
       where the triggering index pulse actually fired, used only when the chain
       has to be re-established. */
    void closeLapAt(uint32_t cutCell, uint32_t pulseCell);
    /* Finds the true revolution length near `nominal` by looking for where the
       captured cells repeat. See the comment on the definition. */
    uint32_t correlateLength(const AdmXCopyLap* lap, uint32_t nominal, uint32_t covered,
                             uint32_t* outScore) const;
    /* Where in the lap the inter-sector gap sits. The lap boundary is moved there
       so that no sector straddles it - see the comment on the definition. */
    uint32_t findGapOffset(const AdmXCopyLap* lap, uint32_t length) const;
    void startLap(uint32_t indexCell, int tag);
    /* Writes one contiguous run of cells from a record. Split out because a
       record can straddle the index and the two halves belong to different
       laps. */
    void writeCells(const XclRecordHeader& h, const uint8_t* packed, uint32_t cellStart,
                    uint32_t srcOffset, uint32_t count);
    void abandonLap(); /* head moved: drop the partial, keep what is published  */

    /* --- link ------------------------------------------------------------- */
    XCopy::XCopyLink m_link;
    std::string m_comPort;
    FloppyBridge::BridgeDensityMode m_densityMode;
    FloppyBridge::DriveSelection m_driveCable;
    std::string m_lastError;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ioError{false};

    /* --- what the guest asked for (emulation thread writes) --------------- */
    std::atomic<int> m_wantCylinder{0};
    std::atomic<bool> m_wantSide{false};
    std::atomic<bool> m_motorRunning{false}; /* the GUEST's view, set synchronously */
    std::atomic<bool> m_wantMotor{false};

    /* --- what the hardware has reached (reader thread writes) ------------- */
    std::atomic<int> m_hwCylinder{-1};
    std::atomic<bool> m_hwSide{false};
    std::atomic<bool> m_hwMotorOn{false};
    std::atomic<bool> m_motorReady{false};
    std::atomic<bool> m_diskInDrive{true};
    /* steady_clock milliseconds at the last index pulse. An index is positive
       proof of media and outranks the disk-change line, which cannot be trusted
       in the "present" direction - see onStatus(). */
    std::atomic<uint64_t> m_lastIndexMs{0};
    std::atomic<unsigned int> m_lastStatusBits{0};
    std::atomic<bool> m_streaming{false};
    std::atomic<unsigned int> m_generation{0};

    /* --- laps ------------------------------------------------------------- */
    AdmXCopyLap m_lap[3];

    /* Slot bookkeeping. The lock is held for a handful of instructions, roughly
       five times a second from each side - never during a bit read. */
    std::mutex m_slotLock;
    int m_frontIdx = -1; /* being served                                       */
    int m_readyIdx = -1; /* complete, waiting for the guest to cross the index  */
    int m_backIdx = 0;   /* being written                                      */

    /* Read without the lock by getMFMBit(). Buffers live for the object's
       lifetime, so a stale pointer is a freshness question, never a safety one. */
    std::atomic<AdmXCopyLap*> m_frontLap{nullptr};
    std::atomic<AdmXCopyLap*> m_backLap{nullptr};
    std::atomic<int> m_frontTag{-1};
    std::atomic<int> m_backTag{-1};
    /* Tag of the lap in the ready slot, -1 when none: the lock-free pre-check
       that lets getMFMBit promote a fresh publish immediately when the front
       is stale, without touching the mutex per bit. */
    std::atomic<int> m_readyTag{-1};

    /* The arc of the back lap that actually holds data, as [from, to) angles.
       A range rather than a single watermark because a lap that begins after a
       seek starts wherever the head landed, not at the index - and those cells
       are perfectly good, they simply do not begin at angle 0. */
    std::atomic<unsigned int> m_backFrom{0};
    std::atomic<unsigned int> m_backTo{0};

    /* THE PREVIOUS REVOLUTION, SNAPSHOTTED AT EVERY CUT - published or not.
     *
     * Why it exists: between a head move and the first promoted lap, the guest is
     * served the in-progress back lap, which only covers [m_backFrom, m_backTo).
     * The guest and the platter advance at the same rate, so a guest whose read
     * angle happens to sit AHEAD of the writer's watermark stays ahead, and reads
     * garbage for entire revolutions - measured under the emulator as DMA reads
     * with ZERO decodable sectors while the head sat still on a good track, and
     * as a scatter of failing sectors that moved every lap (the arc between the
     * cut and the guest's angle). Whether a post-seek read succeeded was a
     * lottery on the guest's phase.
     *
     * Yet the cells for those angles exist - they were captured one revolution
     * earlier and thrown away because the lap they were in was never published.
     * The anchor CHAINS (lap N+1 starts where lap N was cut), so angle x in this
     * lap and angle x in the previous one are the same platter angle, and the
     * previous lap's cells are valid data for every angle the writer has not
     * reached yet.
     *
     * A uniformly offset track image is still self-consistent MFM - AmigaDOS
     * finds sectors by sync word, not by angle from the index - so the only
     * place this can corrupt anything is the SEAM where serving switches between
     * back and prev, which costs at most the sector spanning it. That is the
     * trade: one possibly-bad sector at the seam instead of a phase-dependent
     * garbage arc.
     *
     * It is a COPY (~13 KB, ~4 us, once per revolution on the reader thread),
     * not a pointer into the rotation, because every buffer in m_lap[] gets
     * recycled as a back buffer and a served pointer must never be written. */
    AdmXCopyLap m_prevSnap;
    std::atomic<unsigned int> m_prevFrom{0};
    std::atomic<unsigned int> m_prevTo{0}; /* 0 = no snapshot; set LAST on write */
    std::atomic<unsigned int> m_prevShift{0}; /* current-lap pos + this = snap angle */
    std::atomic<unsigned int> m_prevLen{0};   /* snap's own revolution, for the wrap */
    std::atomic<int> m_prevTag{-1};

    std::atomic<unsigned int> m_trackLenCells{kXcPlaceholderRevCells};
    std::atomic<unsigned int> m_measuredRevCells{0};

    /* --- stream position (reader thread only) ----------------------------- */
    uint32_t m_lapStartCell = 0;
    bool m_haveLapStart = false;
    uint32_t m_expectedCell = 0;
    bool m_haveExpected = false;
    /* Cells before this belong to the surface we just left. */
    uint32_t m_skipBeforeCell = 0;
    bool m_haveSkip = false;
    /* Set when something has made the covered arc non-contiguous - a counter gap,
       an XCL_EV_OVERRUN, an XCL_RF_DISCONTINUITY record, or a run of records the
       device flagged as unusable. The next good record restarts the arc at its
       own angle rather than extending the old one over the hole. */
    bool m_restartCoverage = false;

    /* True from the moment a seek is sent until XCL_EV_TRACK_CHANGE reports the
       head has landed. The cell counter runs across that span (XCL_CAP_SEEK_PHASE)
       but no record covers it, so the contiguity check has to be disarmed or every
       head move is reported as tens of thousands of lost cells. Measured: ~305,000
       across six seeks, on a link with zero resyncs, zero CRC errors and zero
       device overruns, and with a stationary head delivering every cell. */
    std::atomic<bool> m_seekInFlight{false};

    /* A SEEK the device answered with XCL_RESULT_BUSY, which it does if one is
       already stepping. Without this the refusal was silently dropped: the driver
       had already recorded the new position as sent, so it never re-issued, and
       the head stayed where it was while every answer to the guest claimed
       otherwise. Sector headers then came off the wrong cylinder - data that
       decodes perfectly and belongs somewhere else, which is how a disk boots its
       bootblock and then reports "Not a DOS disk in unit 0" when AmigaDOS reads
       the root block at cylinder 40. */
    std::atomic<bool> m_seekRefused{false};

    /* An index pulse that has arrived but cannot be acted on yet.

       The event turns up the moment the pulse does, but the cells for the END of
       that lap have not been delivered - records are 1024 cells and do not align
       with a ~102,000-cell revolution, so the record straddling the index arrives
       afterwards. Closing the lap on the event therefore measured the covered arc
       short of the lap length on nearly every revolution, nothing was ever
       "complete", and NOTHING WAS EVER PUBLISHED: measured 0 publishes in 3000 ms
       on a stationary head. The lap is now closed by the first record that starts
       at or after the pulse, and a straddling record is split across the two.

       THE PULSE ONLY TRIGGERS THE CUT; IT DOES NOT PLACE IT. The cut goes at the
       chain's own predicted boundary (m_lapStartCell + m_lastLapLength). An
       earlier version placed the cut at (pulse + a carried offset), and the two
       drift apart at every track change - the cut then fired thousands of cells
       before the chain's boundary, coverage never reached `length`, no lap was
       ever complete again, and nothing published or re-aligned for the rest of
       the dwell. Measured: arc=[0,100584) against len=102047, lap after lap. */
    uint32_t m_pendingIndexCell = 0;
    bool m_havePendingIndex = false;

    /* The platter's own revolution, measured pulse to pulse. Immune to where the
       lap boundary sits, which is what makes it the one safe feedback signal for
       the chained length - the lap's own span measures the CHAIN, not the
       platter, once the cut is chain-placed. Survives a seek: the index is a
       mark on the platter and XCL_CAP_SEEK_PHASE keeps the counter running. */
    uint32_t m_lastIndexEventCell = 0;
    bool m_haveLastIndexEventCell = false;
    uint32_t m_indexPeriodCells = 0;

    /* Per-lap fault tallies, reader thread only, for the lap log: how many times
       coverage restarted inside this lap (each restart is a hole, and every hole
       edge is a seam a sector can die on), and how many records arrived flagged
       unusable. */
    uint32_t m_lapHoles = 0;
    uint32_t m_lapFlagged = 0;

    /* The length of the last lap, and the reason position 0 means one fixed angle
       rather than "wherever the index sensor fired this time round".

       Each lap now begins where the previous one was CUT, not at the next index
       pulse, so consecutive laps are phase-continuous by construction. The index
       is used to trigger the cut and to establish the phase once; after that the
       anchor chains. See the comment above kJoinMargin. */
    uint32_t m_lastLapLength = 0;

    /* Cleared on every track change, set once the lap boundary has been moved
       into the inter-sector gap for the surface now under the head. */
    bool m_phaseAligned = false;

    /* Complete laps on which findGapOffset found NO gap. The first laps after a
       seek can be degraded enough to lose sync words, and one bad measurement
       used to latch "aligned" with the boundary mid-sector for the rest of the
       dwell. Retry on later, cleaner laps; only after kXcAlignGiveUp misses is
       the track treated as genuinely gapless (custom format) and published as
       it stands. Reset on every track change.

       A complete lap with FEWER THAN EIGHT sync pairs is not a degraded
       AmigaDOS track - the transient no-gap laps measured in docs/floppy-xcopy.md
       8k all saw 12-13 syncs (a spurious sync splitting the gap span) - it is a
       custom format, and it is decided immediately rather than four laps later.
       A copy-protected loader retries two or three times, not ten; four wasted
       revolutions on every visit to a protection track is the difference
       between loading and "track read error". */
    unsigned int m_phaseAlignTries = 0;
    static const unsigned int kXcAlignGiveUp = 4;

    /* The surface under the head has no inter-sector gap (custom format), or
       alignment has been abandoned for this dwell after an empty-lap re-anchor.
       Published laps are then cut by CORRELATION (correlateLength) rather than
       by the damped index period: on a gapless track the wrap sits mid-data,
       and tens of cells of slip there corrupt whatever spans it on every single
       lap - no retry count recovers that. Correlation is unambiguous exactly
       where slip is harmful (real data) and ambiguous only where slip is
       harmless (a periodic gap), so the two schemes are complementary.
       Cleared on every track change. */
    bool m_trackGapless = false;

    /* Set when a closed lap covered NOTHING while the chain was established.
       Measured under the emulator (docs/floppy-xcopy.md): a forward-shifted
       anchor plus ~60 ms of reader lag locks the anchor permanently ahead of
       the stream - every lap closes with arc=[0,0), nothing publishes, and the
       guest starves for the rest of the dwell (7 seconds observed, ending in
       the loader's "track read error"). The close re-anchors from the pulse;
       this flag then routes the NEXT complete lap straight to gapless
       publishing instead of re-applying the shift that caused it, so the
       recovery cannot oscillate. */
    bool m_alignAbandoned = false;

    /* Run findGapOffset once on the first lap published after a cache-applied
       alignment, to confirm the cached angle still holds (and refresh it). */
    bool m_alignVerify = false;

    /* Per-track alignment memory, reader thread only. The gap's angular
       position is a property of how the track was FORMATTED, so it is stable
       for the life of the disk; re-measuring it on every visit cost one
       revolution per visit, which is most of the difference between this
       driver's 3 attempts per track visit and the buffered Greaseweazle's 1-2.
       The angle is stored relative to the physical index pulse (jitter ~135
       cells against a ~6300-cell gap, so the reference is comfortably stable)
       and refreshed on every publish so spindle drift cannot stale it.
       Invalidated whole on any media change. */
    struct XcTrackAlignEntry {
        uint8_t state;          /* 0 unknown, 1 = gapPulseAngle valid          */
        uint32_t gapPulseAngle; /* boundary's target angle after the pulse     */
    };
    XcTrackAlignEntry m_alignCache[168] = {};

    /* Readable mirrors, for the harnesses. Written on the reader
       thread once per track change, never per cell. */
    std::atomic<bool> m_phaseAlignedFlag{false};
    std::atomic<unsigned int> m_phaseShift{0};
    std::atomic<unsigned int> m_phaseAlignSeq{0};
    mutable std::atomic<unsigned int> m_gapSyncs{0};
    mutable std::atomic<unsigned int> m_gapPitch{0};
    mutable std::atomic<unsigned int> m_gapLen{0};
    mutable std::atomic<unsigned int> m_gapSyncAt{0};

    /* Seek trace - see debugSeekTrace(). Written once per seek, never per cell. */
    std::atomic<uint32_t> m_lastCellIndex{0};
    std::atomic<uint32_t> m_seekReqCell{0};
    std::atomic<uint32_t> m_seekLandCell{0};
    std::atomic<uint64_t> m_seekReqMs{0};
    std::atomic<uint64_t> m_seekSentMs{0};
    std::atomic<uint32_t> m_seekDeviceTicks{0};
    std::atomic<uint64_t> m_seekAckMs{0};
    std::atomic<uint64_t> m_seekBeginMs{0};
    std::atomic<uint64_t> m_seekLandMs{0};
    std::atomic<unsigned int> m_seekTraceSeq{0};
    std::atomic<uint64_t> m_statusReqMs{0};
    std::atomic<unsigned int> m_statusRttMs{0};
    std::atomic<unsigned int> m_rxWaiting{0};

    XcLapRecord m_lapLog[kXcLogSize] = {};
    std::atomic<unsigned int> m_lapLogCount{0};
    unsigned int m_promoteLog[kXcLogSize] = {};
    std::atomic<unsigned int> m_promoteLogCount{0};
    int m_streamTag = -1;
    /* Totals for the lap in progress, used to normalise getMFMSpeed against the
       lap's own mean cell time. See the long comment in closeLap() for why it is
       normalised to the lap and not to the nominal 2 us. */
    uint64_t m_lapTicks = 0;
    uint64_t m_lapCells = 0;

    /* --- guest-side revolution tracking (emulation thread only) ----------- */
    unsigned int m_guestMaxPos = 0;

    /* THE GUEST IS PHASE-LOCKED TO THE PLATTER - the piece that makes this a
       live drive rather than a photo of one.

       UAE's mfmPositionBits and the real platter both advance at real time,
       but the OFFSET between them was arbitrary: whatever phase the guest
       happened to be at. So at landing, roughly half the guest's angles
       pointed at cells not yet captured this pass, and the landing
       revolution was a guaranteed failed attempt - the one structural term
       a real Amiga does not have, because on real hardware the guest's read
       position IS the head position.

       We own the pos -> angle mapping. At the first serving moment on a new
       surface the offset is chosen so the guest reads kXcPhaseLead cells
       BEHIND the live capture watermark, and since both sides advance at
       real time it stays there: the guest permanently reads the bits that
       just came off the head, through a ~16 ms pipeline that is invisible
       because it is pure phase. A constant rotation of the track image is
       meaningless to AmigaDOS (sectors are found by sync word), and every
       source - back, prev, front - is indexed in the same chain coordinates,
       so one offset serves them all consistently.

       The offset is per-dwell CONSTANT: relocking mid-read would jump the
       served stream and corrupt whatever sector was under it. It is
       re-derived only when the previous lock cannot serve (a new surface
       with no matching photo), or when accumulated clock drift has moved
       the guest more than kXcPhaseDriftLimit cells off its lead - many
       seconds of dwell - where one small jump is cheaper than the drift
       growing without bound. Emulation thread only. */
    /* LIVE SERVING IS THE DEFAULT (docs/floppy-xcopy.md 8q). Set the
       environment variable ADM_XC_LIVE=0 to fall back to the photo model.

       Flipped on the measurements of 2026-09-02, core 1.66.0, delay line at
       the 48 ms lead: standard AmigaDOS disk 3/3 boots, 33-35 s power-on to
       XCopy (two trials beat the buffered Greaseweazle's 35 s), mean 2.06
       attempts/visit vs the photo path's 2.37 and 37-39 s the same day;
       protected disk 1.83 attempts/visit vs the photo model's 3 reads/track
       flat. The photo path stays intact behind the variable - every live
       change is gated on m_liveServe - and remains the fallback for hosts
       whose frame burst outruns the 48 ms lead (kXcPhaseLead's floor is the
       emulation thread's sprint, measured on one machine; ADM_XC_LIVE_LEAD
       raises it without a rebuild). */
    bool m_liveServe = true;
    unsigned int m_lockOffset = 0;
    unsigned int m_lockGen = 0xFFFFFFFFu;

    /* THE CONTINUOUS DELAY LINE - live mode's serving source, replacing the
       lap buffers there entirely (docs/floppy-xcopy.md 8o).

       The device's cell counter never resets, so no anchor and no laps are
       needed: every arriving cell is written at (cellIndex mod m_delayLen) and
       the buffer, read in angle order, is a continuous bit stream with exactly
       ONE discontinuity - at the write cursor, where fresh bits meet bits one
       revolution older. The guest trails the cursor by m_phaseLead cells, so
       it never reads the discontinuity: no cut, no join, no seam sector. The
       lap machinery keeps running (it feeds the measured revolution length,
       m_liveSpeed and the photo path's alignment cache) but serves nothing
       while live.

       Validity is one comparison. XCL_EV_TRACK_CHANGE carries the exact cell
       index where the new surface begins (m_delayValidFrom); the bit at angle
       a was last written at index W - ((Wangle - a) mod len), and it is
       servable iff that is >= validFrom. Angles the head has not reached since
       landing fail the test and serve garbage - which is exactly what a real
       drive's head gives you at angles it has not passed over yet, and
       trackdisk's WORDSYNC waits it out in milliseconds, not revolutions.

       m_delayLen is FROZEN per dwell (set at TRACK_CHANGE): the modulus and
       the guest's revolution wrap must agree exactly, and re-mapping a
       populated buffer under a mid-dwell length update would shift every
       angle. The ±60-cell/rev spindle wobble against a frozen modulus only
       drifts the guest-to-cursor gap, tiny against the lead.

       m_delayBits is written by the reader thread and read by the emulation
       thread with no lock: the release store of m_delayW orders the bit
       writes, and validity guarantees the guest only reads cells written at
       least a lead ago. */
    uint8_t m_delayBits[kXcLapBytes] = {0};
    std::atomic<uint32_t> m_delayW{0};         /* next cell index to write     */
    std::atomic<uint32_t> m_delayValidFrom{0}; /* first cell of this surface   */
    std::atomic<unsigned int> m_delayLen{0};   /* modulus, frozen per dwell    */

    void writeDelayLine(uint32_t cellIndex, const uint8_t* packed, uint32_t count);
    bool delayLineBit(int mfmPositionBits);
    /* The lead must exceed the WORST-CASE USB delivery jitter, not the mean.
       At 8192 (16 ms) the published watermark's arrival bursts crossed the
       guest's angle and serving flip-flopped between the current and previous
       revolution at record granularity - measured under the emulator as
       multi-sector checksum failures on every live-served attempt. 24576
       (~48 ms) rides above the bursts. The cost is pure phase, not latency:
       the guest reads bits 48 ms after they left the platter, which no guest
       can observe - and thanks to trackdisk's WORDSYNC the first read after a
       seek still begins at the first clean sync it is served.

       The trade: the lead is also a span of nothing directly after a landing
       (those angles were never captured on the new surface), so it is set
       just above the jitter the TIME_CRITICAL reader thread still shows, not
       comfortably above. Measured under the emulator: 8192 flip-flopped
       (broken), 49152 was safe but its ~96 ms garbage span pushed the first
       real post-landing read past its window - one whole extra attempt per
       visit. */
    /* Swept on the protected disk with the delay line (core 1.66.0): 8192
       (16 ms) is broken, 16384 (32 ms) intermittently dirty, 24576 (48 ms)
       clean. The floor is NOT USB delivery (2 ms since fw v717.26) and NOT
       UAE's DMA predict (one scanline): it is the emulation thread's frame
       burst - a 20 ms frame emulated in a few ms of wall time sprints the
       guest's read position up to two frames ahead of real time, and the
       lead must cover the sprint or the guest overtakes the write cursor
       mid-revolution. Two frames plus margin = 48 ms. */
    static const unsigned int kXcPhaseLead = 24576;
    /* With the dynamic stall active the lead stops being a coverage number
       (see below) and only sets where the guest lands after a lock, so the
       stalled default is the swept optimum: 1024 cells (~2 ms). Measured on
       the standard disk, core 1.68.0 (docs/floppy-xcopy.md 8r): 1024 -> 1.37
       attempts/visit {1:90, 2:42, 3:4}; 512 was worse (1.48, a 4-bucket
       appears - delivery jitter starts to bite), 2048 -> 1.51, 4096 -> 1.66.
       kXcPhaseLead stays the no-stall default because without the stall a
       small lead is not marginal, it is fatal: ADM_XC_STALL=0 with a 4096
       lead measured 0/3 boots, mean 6.38 attempts. */
    static const unsigned int kXcPhaseLeadStalled = 1024;
    /* The live value: the stall-appropriate default unless ADM_XC_LIVE_LEAD
       overrides it. Written once in initialise(), read on the emulation
       thread. */
    unsigned int m_phaseLead = kXcPhaseLead;

    /* THE DYNAMIC STALL - sub-frame pacing of the guest's disk consumption
       (docs/floppy-xcopy-subframe-pacing.md, avenue 1).

       The 48 ms lead exists only to cover the emulation thread's frame burst:
       a 20 ms frame emulated in a few ms of wall time sprints the guest's read
       angle toward the write cursor, and a static lead has to be wider than
       the worst sprint. The stall replaces coverage with waiting: when the
       locked guest's distance behind the cursor drops below m_stallMargin,
       getMFMBit sleeps the emulation thread until the platter has delivered
       enough cells to clear the margin (plus hysteresis, so the wait is paid
       once per ~2 ms of guest progress, not per bit). The guest can then never
       overtake the cursor, and the lead stops being a coverage number - it
       only sets where the guest lands after a lock, so it can drop to ~8 ms
       and take most of the post-landing dead time with it.

       The check is one compare on values delayLineBit already loads, so the
       hot path pays nothing when the guest is not sprinting. The wait is
       bounded (kXcStallTimeoutMs) and gives up for the remainder of the
       generation, so a dead device degrades to exactly today's behaviour
       instead of hanging the emulator; it is skipped while a seek is in
       flight, because the writer is silent across a seek by design and the
       guest is reading a stale surface it will abandon at TRACK_CHANGE.

       The frame limiter sleeps at vsync to pin emulated time to real time;
       the stall moves part of that sleep into the disk read. Net wall-clock
       speed is unchanged - the guest was going to wait either way, but now it
       waits BEFORE reading bits that do not exist yet instead of after
       reading garbage. */
    bool m_stallEnable = true;              /* ADM_XC_STALL=0 disables        */
    unsigned int m_stallMargin = 512;       /* cells; ADM_XC_STALL_MARGIN     */
    static const unsigned int kXcStallHysteresis = 1024; /* resume above this */
    static const unsigned int kXcStallTimeoutMs = 50;
    unsigned int m_stallGiveUpGen = 0xFFFFFFFFu; /* gen whose stall timed out */
    /* Plain increments, emulation thread only, same reasoning as
       m_servedFront: roughly right is right enough for instrumentation. */
    uint32_t m_stallCount = 0;  /* stalls entered                             */
    uint32_t m_stallSleeps = 0; /* 1 ms sleeps taken; ~total stalled ms       */

    /* THE GUEST'S BIT CLOCK IS SLAVED TO THE PLATTER WHILE SERVING LIVE.
       getMFMSpeed(1000) means a 2.000 us cell, which makes the guest's
       revolution len * 2 us ~= 204 ms - but this platter turns in ~199.8 ms.
       At speed 1000 the phase lock decays by that 2.1% - ~2,100 cells every
       revolution - and the guest is dragged backwards across lap boundaries
       and serving seams, measured under the emulator as multi-sector checksum
       failures on every pre-photo attempt. A real Amiga cannot have this
       problem because Paula recovers its clock from the flux itself; this is
       the same thing, one number per revolution: the lap's measured mean cell
       time, expressed as a speed. The front photo keeps its own per-slot
       normalisation (image semantics, unchanged). */
    std::atomic<unsigned int> m_liveSpeed{1000};

    /* --- counters --------------------------------------------------------- */
    std::atomic<unsigned int> m_publishCount{0};
    std::atomic<unsigned int> m_overruns{0};
    std::atomic<unsigned int> m_cellsLost{0};
    /* Records dropped because the device flagged them unusable. Expect a burst on
       every seek; a steady trickle on a stationary head means the PLL is losing
       lock and the disk or drive is the suspect, not this driver. */
    std::atomic<unsigned int> m_unstableRecords{0};

    bool m_directMode = false;

    /* Which serving path answered getMFMBit, counted with PLAIN increments -
       one cycle each, emulation thread only, torn reads tolerated - because a
       relaxed fetch_add at 500k calls a second is real work on the hot path and
       the numbers only have to be roughly right. Read (racily, fine) by the
       lap log below. */
    uint32_t m_servedFront = 0;
    uint32_t m_servedBack = 0;
    uint32_t m_servedPrev = 0;
    uint32_t m_servedGarbage = 0;

    /* Runtime instrumentation, OFF unless the ADM_XCOPY_LOG environment variable
       names a file. One fprintf per lap close / track change / promote - about
       five a second - and NOTHING on the per-bit path, so unlike ADM_XCOPY_DIAG
       it cannot slow the guest against the platter and its figures can be
       believed. This is what makes a stall diagnosable inside the real emulator
       rather than only in the harnesses. */
    FILE* m_log = nullptr;
    void liveLog(const char* fmt, ...);

#if ADM_XCOPY_DIAG
    void diagLog(const char* fmt, ...);
#endif
};

#endif /* ADMXCOPYBRIDGE_H */
