/* AdmXCopyBufferedBridge.cpp - see the header for what this is and why.
 *
 * Structure follows GreaseWeazleBridge.cpp + GreaseWeazleInterface::readRotation():
 * every hook is a short forward onto the link, and readData() is the same loop -
 * request, pump, feed the PLL, extract, callback - with the device's bounded read
 * in place of Cmd::ReadFlux.
 */

#include "AdmXCopyBufferedBridge.h"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

using namespace XCopy;

namespace {

const FloppyDiskBridge::BridgeDriver driverInfo = {
    "XCopy Standalone (buffered)", "https://github.com/AdamKeher/XCopyStandalone", "Adam Keher",
    "Adam Keher", CONFIG_OPTIONS_COMPORT | CONFIG_OPTIONS_AUTOCACHE | CONFIG_OPTIONS_SMARTSPEED};

/* The firmware that first answered XCL_CMD_READ_TRACK. */
const uint16_t kMinFirmware = 0x0718;

/* Writing needs 0x0733 (XCL_CMD_WRITE_TRACK and the bulk transfer), but the version
   is only the diagnostic: the gate is XCL_CAP_WRITE in the HELLO, because that is the
   device telling us rather than us inferring it. */
const uint16_t kMinWriteFirmware = 0x0733;

/* How long to wait for XCL_EV_WRITE_DONE. The write itself is one revolution behind
   an index wait of up to another, the transfer is tens of ms, and the device's own
   index bound is 500 ms - so this only has to be longer than all of that, not tight. */
const unsigned int kWriteDoneTimeoutMs = 2500;

/* Nominal cell, and the device's nominal cell in its own ticks. 48 ticks at BOTH
   densities (the device halves its prescaler for HD), so a record's measured
   span scaled by this is the DD-timescale flux the extractor expects - which is
   also the x2 the Greaseweazle driver applies to HD timings. */
const double kNominalCellNs = 2000.0;
const double kNominalCellTicks = 48.0;

/* Greaseweazle's assumption until this drive has been measured. */
const double kDefaultNsPerSequence = 8000.0;
const uint32_t kDefaultRevMs = 210;

/* The extractor's overlap search window for this driver, in sequences either side
   of the expected revolution point (libretro-uae-floppybridge-overlap.patch).
   Upstream searches +-5120 and will not declare a revolution until 1024 + 5120
   sequences have arrived past that point - ~30 ms of every read at the ~5 us per
   sequence a DD AmigaDOS track measures. That window is sized for real-flux
   jitter and an arbitrary anchor. This driver anchors on a sync word (onCells),
   so the join is known to within the pulse jitter (~70 sequences) plus whatever
   the platter's speed has drifted since the revolution time was learnt (~0.1%,
   ~40 sequences). 512 was shipped first with a 4x demonstrated margin (128 read
   the whole disk clean); 128 is the second round's figure, 2 ms cheaper, gated the
   same way. A miss is not fatal: the extractor falls back to index mode for that
   read. ADM_XCB_OVERLAP overrides it for a sweep. */
const uint32_t kDefaultOverlapWindow = 128;
/* And the marker the extractor matches: upstream's 1024 sequences (~8 ms) was
   sized for an arbitrary anchor. 48 cells into a sector header, 256 sequences
   (~600 cells, the whole info long and label) are unique to that sector, and the
   window above is a small fraction of a sector pitch, so nothing else can match.
   ADM_XCB_MATCHES overrides it. */
const uint32_t kDefaultOverlapMatches = 256;

double msBetween(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

} // namespace

AdmXCopyBufferedBridge::AdmXCopyBufferedBridge(FloppyBridge::BridgeMode bridgeMode,
                                               FloppyBridge::BridgeDensityMode bridgeDensity,
                                               bool enableAutoCache, bool useSmartSpeed,
                                               bool autoDetectComPort, char* comPort,
                                               bool allowWrites)
    : CommonBridgeTemplate(bridgeMode, bridgeDensity, enableAutoCache, useSmartSpeed),
      m_comPort(autoDetectComPort ? "" : (comPort ? comPort : "")), m_densityMode(bridgeDensity),
      m_allowWrites(allowWrites)
{
    m_isHDDisk = (bridgeDensity == FloppyBridge::BridgeDensityMode::bdmHDOnly);
    if (const char* path = getenv("ADM_XCB_LOG"))
        m_log = fopen(path, "a");
}

AdmXCopyBufferedBridge::~AdmXCopyBufferedBridge()
{
    if (m_log)
        fclose(m_log);
}

const FloppyDiskBridge::BridgeDriver* AdmXCopyBufferedBridge::staticBridgeInformation()
{
    return &driverInfo;
}

void AdmXCopyBufferedBridge::logLine(const char* fmt, ...)
{
    if (!m_log)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(m_log, fmt, ap);
    va_end(ap);
    fputc('\n', m_log);
    fflush(m_log);
}

/* ------------------------------------------------------------------------- */
/* Open / close                                                               */
/* ------------------------------------------------------------------------- */

bool AdmXCopyBufferedBridge::openInterface(std::string& errorMessage)
{
    m_link.setSink(this);
    const LinkResult r = m_link.open(m_comPort, false);
    if (r != LinkResult::Ok) {
        errorMessage = m_link.lastError().empty() ? toString(r) : m_link.lastError();
        m_ioError.store(true);
        return false;
    }

    const XclHello& h = m_link.hello();
    if (h.firmwareVersion < kMinFirmware) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "XCopy Standalone firmware v%u.%u is too old for the buffered driver - v%u.%u or "
                 "later is required (XCL_CMD_READ_TRACK).",
                 h.firmwareVersion >> 8, h.firmwareVersion & 0xFF, kMinFirmware >> 8,
                 kMinFirmware & 0xFF);
        errorMessage = buf;
        m_link.close();
        m_ioError.store(true);
        return false;
    }
    m_tickHz = h.tickHz ? h.tickHz : 24000000;

    /* Density: DD unless forced to HD. No auto-detect in this version. */
    m_link.config(XCL_CFG_DENSITY, m_isHDDisk ? 2 : 1);

    /* Head timings, the same knobs and clamps as driver 4 so an A/B is one
       environment variable. */
    unsigned long stepUs = 3000;
    if (const char* su = getenv("ADM_XC_STEP_US")) {
        const unsigned long v = strtoul(su, nullptr, 10);
        if (v >= 1000 && v <= 6000)
            stepUs = v;
    }
    /* 10 ms, not driver 4's 15: a buffered read discards its first 20 us, so a
       head still ringing costs little it can see. Swept on the standard disk at
       15/10/6 ms with the sync-word anchor: 160/160 tracks, 1.00 attempts, zero
       byte differences at each. 6 ms is NOT safe with the adaptive anchor (8e):
       the sync gate used to double as a data-quality check at the start of the
       read, and a marker taken blind 6 ms after the last step failed its match
       a lap later on one track in every pass (a 246 ms empty read). 10 ms with
       the probe: 0 in 480 reads. */
    unsigned long settleUs = 10000;
    if (const char* se = getenv("ADM_XC_SETTLE_US")) {
        const unsigned long v = strtoul(se, nullptr, 10);
        if (v >= 2000 && v <= 30000)
            settleUs = v;
    }
    /* The device's direction-line settle defaults to 20 ms and is paid on EVERY
       seek, whether or not the direction changed - measured as the difference
       between a 41 ms one-cylinder seek here and the Greaseweazle's ~31 ms, on the
       same per-track read. The Shugart setup time for the direction line is a
       microsecond; a millisecond is generous. Sweepable, like the others. */
    unsigned long dirUs = 1000;
    if (const char* du = getenv("ADM_XC_DIR_US")) {
        const unsigned long v = strtoul(du, nullptr, 10);
        if (v >= 100 && v <= 20000)
            dirUs = v;
    }
    m_overlapMatches = kDefaultOverlapMatches;
    if (const char* om = getenv("ADM_XCB_MATCHES")) {
        const unsigned long v = strtoul(om, nullptr, 10);
        if (v >= 64 && v <= OVERLAP_SEQUENCE_MATCHES)
            m_overlapMatches = (uint32_t)v;
    }
    m_overlapWindow = kDefaultOverlapWindow;
    /* ADM_XCB_PROBE=0 disables the adaptive anchor (every read waits for a sync). */
    m_probeEnable = true;
    if (const char* pe = getenv("ADM_XCB_PROBE"))
        m_probeEnable = strtoul(pe, nullptr, 10) != 0;
    if (const char* ov = getenv("ADM_XCB_OVERLAP")) {
        const unsigned long v = strtoul(ov, nullptr, 10);
        if (v >= 1 && v <= OVERLAP_SEQUENCE_MATCHES * (OVERLAP_EXTRA_BUFFER - 1))
            m_overlapWindow = (uint32_t)v;
    }
    m_link.config(XCL_CFG_STEP_INTERVAL_US, (uint32_t)stepUs);
    m_link.config(XCL_CFG_SEEK_SETTLE_US, (uint32_t)settleUs);
    m_link.config(XCL_CFG_DIR_SETTLE_US, (uint32_t)dirUs);
    m_link.config(XCL_CFG_WATCHDOG_MS, 5000);
    m_link.config(XCL_CFG_TX_FLUSH_US, 1000);
    /* Biggest records: the bulk path wants the fewest headers, and nothing here
       is paced at record granularity. */
    m_link.config(XCL_CFG_RECORD_CELLS, 1024);
    noteCommandSent();

    /* Everything answers with drive status bits; take the first one we can get. */
    refreshStatus(300);

    /* Find cylinder 0 before the guest asks for anything. */
    m_gotTrackChange = false;
    m_link.recalibrate();
    noteCommandSent();
    if (!waitFlag(m_gotTrackChange, 3000)) {
        errorMessage = "XCopy Standalone did not find track 0 (no TRACK_CHANGE after RECAL).";
        m_link.close();
        m_ioError.store(true);
        return false;
    }
    m_hwCylinder = 0;
    /* The TRACK_CHANGE that answered RECAL carries the side the device is actually
       on, and it is not always 0: a previous session that was killed rather than
       closed leaves the side line wherever it was. Overwriting m_hwUpperSide with
       false here (as this did until 2026-09-02) made setActiveSurface() skip the
       switch, and the first track of a run was side 1's data served as side 0 -
       0/11 on track 0 ten attempts running (docs/floppy-xcopy-buffered.md 8g).
       Trust the report and put the head where the template assumes it is. */
    const bool reportedUpper = m_hwUpperSide;
    if (m_hwUpperSide && !seekAndWait(0, false)) {
        errorMessage = "XCopy Standalone did not select side 0 after RECAL.";
        m_link.close();
        m_ioError.store(true);
        return false;
    }

    /* Writing needs three things to line up, and the guest is told the truth about
       all three at once: the slot was attached with writes allowed, the device says
       it can write, and the disk's own tab is open. refreshStatus() above has already
       given us the real /WPRO line. */
    m_deviceCanWrite = (h.capabilities & XCL_CAP_WRITE) != 0;
    if (!m_deviceCanWrite)
        m_allowWrites = false;
    setWriteProtectStatus(writeProtectedForGuest());

    logLine("open %s fw=0x%04x tickHz=%u hd=%d step=%lu settle=%lu dir=%lu overlap=%u "
            "matches=%u probe=%d recal_side=%d write=%d devwrite=%d wprot=%d",
            m_comPort.c_str(), h.firmwareVersion, m_tickHz, m_isHDDisk ? 1 : 0, stepUs, settleUs,
            dirUs, m_overlapWindow, m_overlapMatches, m_probeEnable ? 1 : 0, reportedUpper ? 1 : 0,
            m_allowWrites ? 1 : 0, m_deviceCanWrite ? 1 : 0, m_writeProtect ? 1 : 0);
    return true;
}

void AdmXCopyBufferedBridge::closeInterface()
{
    /* close() sends STREAM_STOP, MOTOR off and BYE so the next opener finds the
       device's console. */
    m_link.setSink(nullptr);
    m_link.close();
    m_motorOn = false;
    logLine("close");
}

/* ------------------------------------------------------------------------- */
/* Housekeeping                                                               */
/* ------------------------------------------------------------------------- */

void AdmXCopyBufferedBridge::poll()
{
    if (!m_link.isOpen())
        return;

    /* Whatever the device volunteered since the last wait - DISK_CHANGE,
       WRITE_PROTECT, a STATUS reply - lands here. Never blocks. */
    m_link.pump();

    const clock::time_point now = clock::now();

    /* The device's watchdog is literal: 5 s of host silence and it parks the
       motor and drops to its console. Any command resets it. */
    if (msBetween(m_lastCmdSent, now) > 1000.0) {
        m_link.ping();
        noteCommandSent();
    }

    /* Media and write-protect, without waiting for an edge that may never come
       (docs/floppy-xcopy.md 8f). The reply is consumed by the next pump. */
    if (msBetween(m_lastStatusAsked, now) > 500.0) {
        m_link.requestStatus();
        noteCommandSent();
        m_lastStatusAsked = now;
    }
}

void AdmXCopyBufferedBridge::applyStatusBits(uint8_t bits)
{
    m_statusBits = bits;
    m_writeProtect = (bits & XCL_ST_WRPROT) != 0;

    /* The disk-change line latches "removed" until the head steps, so a set bit
       is proof of media and a clear one is only a hint. A recent index pulse
       outranks the line either way: only a turning disk makes one. */
    const bool recentIndex = msBetween(m_lastIndexSeen, clock::now()) < 1000.0;
    if (bits & XCL_ST_MEDIA)
        m_diskPresent = true;
    else if (!recentIndex)
        m_diskPresent = false;
}

bool AdmXCopyBufferedBridge::refreshStatus(unsigned int timeoutMs)
{
    m_gotStatus = false;
    if (!m_link.requestStatus()) {
        m_ioError.store(true);
        return false;
    }
    noteCommandSent();
    m_lastStatusAsked = clock::now();
    return waitFlag(m_gotStatus, timeoutMs);
}

bool AdmXCopyBufferedBridge::waitFlag(volatile bool& flag, unsigned int timeoutMs)
{
    const clock::time_point start = clock::now();
    while (!flag) {
        if (msBetween(start, clock::now()) > (double)timeoutMs)
            return false;
        if (!m_link.isOpen())
            return false;
        m_link.pumpWait();
    }
    return true;
}

bool AdmXCopyBufferedBridge::getDiskChangeStatus(const bool forceCheck)
{
    if (!forceCheck)
        return m_diskPresent;

    /* A read that saw an index pulse in the last second already answered. */
    if (msBetween(m_lastIndexSeen, clock::now()) < 1000.0) {
        updateLastManualCheckTime();
        return true;
    }

    if (!refreshStatus())
        return m_diskPresent;
    if (m_statusBits & XCL_ST_MEDIA) {
        updateLastManualCheckTime();
        return true;
    }

    /* The line says "no disk", which it also says about a disk inserted since
       the head last moved. Step and ask again, exactly as the Greaseweazle
       driver does: silently at cylinder 0, one cylinder out and back elsewhere. */
    if (m_hwCylinder == 0) {
        performNoClickSeek();
    } else {
        const unsigned int here = (unsigned int)m_hwCylinder;
        seekAndWait(here > 40 ? here - 1 : here + 1, m_hwUpperSide);
        seekAndWait(here, m_hwUpperSide);
    }
    updateLastManualCheckTime();

    if (!refreshStatus())
        return m_diskPresent;
    return (m_statusBits & XCL_ST_MEDIA) != 0;
}

bool AdmXCopyBufferedBridge::attemptToDetectDiskChange()
{
    /* Only reached if supportsDiskChange() were false; kept honest anyway: a
       short index-bounded read on an empty drive comes back "no index". */
    Bound b;
    b.maxIndex = 2;
    b.lingerMs = 0;
    b.maxTicks = (uint32_t)((uint64_t)m_tickHz * 450 / 1000);
    const ReadResponse r = runCapture(nullptr, b, nullptr, 0, nullptr, nullptr, "detect");
    return r != ReadResponse::rrNoDiskInDrive;
}

bool AdmXCopyBufferedBridge::checkWriteProtectStatus(const bool forceCheck)
{
    if (forceCheck)
        refreshStatus();
    /* The real /WPRO line, unless the slot was attached read-only - in which case the
       answer is "protected" and does not change when the tab does, which is the whole
       point of the gate. */
    return writeProtectedForGuest();
}

const FloppyDiskBridge::DriveTypeID AdmXCopyBufferedBridge::_getDriveTypeID()
{
    return m_isHDDisk ? DriveTypeID::dti35HD : DriveTypeID::dti35DD;
}

void AdmXCopyBufferedBridge::checkDiskType()
{
    /* No auto-detect in this version: DD unless the density mode forces HD. */
    m_isHDDisk = (m_densityMode == FloppyBridge::BridgeDensityMode::bdmHDOnly);
}

void AdmXCopyBufferedBridge::forceDiskDensity(bool forceHD)
{
    if (m_isHDDisk == forceHD)
        return;
    m_isHDDisk = forceHD;
    /* Refused by the device mid-stream; only ever called between reads. */
    m_link.config(XCL_CFG_DENSITY, forceHD ? 2 : 1);
    noteCommandSent();
}

/* ------------------------------------------------------------------------- */
/* Motor and head                                                             */
/* ------------------------------------------------------------------------- */

bool AdmXCopyBufferedBridge::setMotorStatus(const bool switchedOn)
{
    if (m_motorOn == switchedOn)
        return true;
    m_motorOn = switchedOn;
    /* The device blocks ~600 ms inside its own motorOn() before answering; the
       template counts getDriveSpinupTime() in parallel. Not waited on here. */
    if (!m_link.motor(switchedOn)) {
        m_ioError.store(true);
        return false;
    }
    noteCommandSent();
    logLine("motor %d", switchedOn ? 1 : 0);
    return true;
}

bool AdmXCopyBufferedBridge::seekAndWait(unsigned int cylinder, bool upperSide)
{
    if (cylinder >= MAX_CYLINDER_BRIDGE)
        return false;

    for (int attempt = 0; attempt < 4; attempt++) {
        m_gotTrackChange = false;
        m_gotAck = false;
        if (!m_link.seek(cylinder, upperSide)) {
            m_ioError.store(true);
            return false;
        }
        noteCommandSent();

        const clock::time_point start = clock::now();
        for (;;) {
            if (m_gotTrackChange)
                break;
            /* A BUSY refusal means the head did not move (docs 8b): the
               previous move's TRACK_CHANGE is still to come. Wait it out, then
               ask again. */
            if (m_gotAck && m_ackCommand == XCL_CMD_SEEK && m_ackResult == XCL_RESULT_BUSY) {
                m_gotAck = false;
                waitFlag(m_gotTrackChange, 1500);
                m_gotTrackChange = false;
                break;
            }
            if (msBetween(start, clock::now()) > 1500.0) {
                logLine("seek %u/%d: no TRACK_CHANGE", cylinder, upperSide ? 1 : 0);
                m_ioError.store(true);
                return false;
            }
            m_link.pumpWait();
        }
        if (m_gotTrackChange && m_hwCylinder == (int)cylinder && m_hwUpperSide == upperSide)
            return true;
    }
    return false;
}

bool AdmXCopyBufferedBridge::setCurrentCylinder(const unsigned int cylinder)
{
    if (m_hwCylinder == (int)cylinder)
        return true;
    const clock::time_point t = clock::now();
    const bool ok = seekAndWait(cylinder, m_hwUpperSide);
    logLine("seek cyl %u side %d: %s %.1f ms", cylinder, m_hwUpperSide ? 1 : 0, ok ? "ok" : "FAIL",
            msBetween(t, clock::now()));
    /* Every TRACK_CHANGE carries the drive's status bits, so a seek is also the
       disk check the template expects one to be. */
    if (ok)
        updateLastManualCheckTime();
    return ok;
}

bool AdmXCopyBufferedBridge::setActiveSurface(const DiskSurface activeSurface)
{
    const bool upper = (activeSurface == DiskSurface::dsUpper);
    if (m_hwUpperSide == upper && m_hwCylinder >= 0)
        return true;
    const unsigned int cyl = m_hwCylinder < 0 ? 0 : (unsigned int)m_hwCylinder;
    return seekAndWait(cyl, upper);
}

bool AdmXCopyBufferedBridge::performNoClickSeek()
{
    m_gotAck = false;
    if (!m_link.noClick()) {
        m_ioError.store(true);
        return false;
    }
    noteCommandSent();
    const clock::time_point start = clock::now();
    while (!(m_gotAck && m_ackCommand == XCL_CMD_NOCLICK)) {
        if (msBetween(start, clock::now()) > 200.0)
            return false;
        m_link.pumpWait();
    }
    if (m_ackResult != XCL_RESULT_OK)
        return false;
    /* The step re-armed the disk-change line; read it back. */
    refreshStatus();
    updateLastManualCheckTime();
    return true;
}

/* ------------------------------------------------------------------------- */
/* Reading                                                                    */
/* ------------------------------------------------------------------------- */

AdmXCopyBufferedBridge::Bound AdmXCopyBufferedBridge::chooseBound(PLL::BridgePLL& pll) const
{
    /* Revolution: the device's last index-to-index period when it has one,
       otherwise the Greaseweazle's 210 ms. */
    uint32_t revMs = kDefaultRevMs;
    if (m_revTicks) {
        const uint32_t ms = (uint32_t)(((uint64_t)m_revTicks * 1000) / m_tickHz);
        if (ms >= 150 && ms <= 260)
            revMs = ms;
    }

    /* Overlap: what the extractor needs past the revolution to match the join,
       at the sequence rate this drive/disk actually produces, plus a margin for
       the record that straddles the cut. The Greaseweazle assumes 8 us per
       sequence, which is ~49 ms; a DD AmigaDOS track measures nearer 35 ms. */
    /* The margin is generous on purpose: the read normally ends early, the
       moment the extractor has produced its revolution (runCapture's STREAM_STOP),
       so this is a ceiling and not the cost. Measured with a 5 ms margin: the
       first read of a cylinder whose sequences ran longer than the previous
       cylinder's ended a few ms short of what the extractor needed and produced
       nothing, and the whole 230 ms was wasted. */
    const double overlapMs =
        ((m_overlapMatches + m_overlapWindow) * m_nsPerSequence) / 1.0e6 + 45.0;

    Bound b;
    MFMExtractionTarget* ex = pll.rotationExtractor();
    const bool indexMode = ex && (ex->isInIndexMode() || !ex->hasLearntRotationSpeed());
    if (indexMode) {
        /* One pulse, then a revolution plus the overlap. The cap is where an
           empty drive gives up: two revolutions to find a pulse that is not
           coming, then the linger. */
        b.maxIndex = 1;
        b.lingerMs = (uint16_t)(revMs + (uint32_t)overlapMs);
        b.maxTicks = (uint32_t)((uint64_t)m_tickHz * (2 * revMs + (uint32_t)overlapMs + 20) / 1000);
    } else {
        b.maxIndex = 0;
        b.lingerMs = 0;
        b.maxTicks = (uint32_t)((uint64_t)m_tickHz * (revMs + (uint32_t)overlapMs) / 1000);
    }
    return b;
}

CommonBridgeTemplate::ReadResponse AdmXCopyBufferedBridge::runCapture(
    PLL::BridgePLL* pll, const Bound& b, RotationExtractor::MFMSample* buffer,
    unsigned int maxBufferSize, RotationExtractor::IndexSequenceMarker* indexMarker,
    std::function<bool(RotationExtractor::MFMSample*, const unsigned int)>* onRotation,
    const char* what)
{
    if (!m_link.isOpen())
        return ReadResponse::rrError;

    /* Anything the device volunteered while we were idle. */
    m_link.pump();

    m_pll = pll;
    m_capturing = true;
    m_zeros = 0;
    m_haveIndexCell = false;
    m_feedOpen = false;
    m_indexArmed = false;
    m_armCells = 0;
    m_shift = 0;
    m_anchorCountdown = 0;
    m_capIndexMarker = indexMarker;
    m_probeCount = 0;
    m_probeVerdict = 0;
    /* Probe only a timed read that will search for its own overlap: in index mode
       the join is at the index anchor and the capture start is not a marker. */
    {
        MFMExtractionTarget* ex = pll ? pll->rotationExtractor() : nullptr;
        const bool indexMode = ex && (ex->isInIndexMode() || !ex->hasLearntRotationSpeed());
        m_probe = (m_probeEnable && pll && indexMarker && buffer && !indexMode);
    }
    m_capCells = 0;
    m_capTransitions = 0;
    m_capIndexEvents = 0;
    m_capTicks = 0;
    m_capRotations = 0;
    m_capRecords = 0;
    m_capBadSpans = 0;
    m_capFirstTicks = 0;
    m_capFirstCount = 0;
    m_capFirstMs = m_capExtractMs = m_capStopMs = -1;
    m_gotReadDone = false;
    m_readDoneReason = XCL_RD_COMPLETE;
    m_readDonePulses = 0;
    m_abort.store(false);
    m_capStart = clock::now();

    if (!m_link.readTrack(XCL_MODE_MFM, b.maxIndex, b.lingerMs, b.maxTicks)) {
        m_capturing = false;
        m_pll = nullptr;
        m_ioError.store(true);
        return ReadResponse::rrError;
    }
    noteCommandSent();

    bool stopped = false;
    bool wantMore = true;
    clock::time_point lastData = clock::now();

    while (!m_gotReadDone) {
        const uint32_t got = m_link.pumpWait();
        const clock::time_point now = clock::now();
        if (got)
            lastData = now;
        else if (msBetween(lastData, now) > 1500.0) {
            /* Not even READ_DONE within a second and a half of silence: the
               device has gone, or dropped to its console on the watchdog. */
            logLine("%s: silence, giving up", what);
            m_ioError.store(true);
            break;
        }

        /* The extractor decides when a revolution is in hand; the callback
           decides whether to keep going. Same as GreaseWeazleInterface, except
           that "stop" here actually stops the device. */
        if (!stopped && pll && buffer && wantMore && pll->canExtract()) {
            unsigned int bits = 0;
            if (pll->extractRotation(buffer, bits, maxBufferSize)) {
                m_capRotations++;
                if (m_capExtractMs < 0)
                    m_capExtractMs = msBetween(m_capStart, clock::now());
                if (onRotation && !(*onRotation)(buffer, bits))
                    wantMore = false;
                if (indexMarker)
                    pll->getIndexSequence(*indexMarker);
            }
        }

        if (!stopped && (m_abort.load() || !wantMore)) {
            stopped = true;
            m_capStopMs = msBetween(m_capStart, clock::now());
            m_link.streamStop();
            noteCommandSent();
        }
    }

    m_capturing = false;
    m_pll = nullptr;
    const double totalMs = msBetween(m_capStart, clock::now());

    /* Learn the drive for the next bound. */
    if (m_capTransitions > 200 && m_capCells > 2000) {
        const double ns = ((double)m_capCells * kNominalCellNs) / (double)m_capTransitions;
        if (ns > 3000.0 && ns < 12000.0)
            m_nsPerSequence = ns;
    }

    ReadResponse r = ReadResponse::rrOK;
    if (!m_gotReadDone)
        r = ReadResponse::rrError;
    else if (m_readDoneReason == XCL_RD_NO_INDEX)
        r = ReadResponse::rrNoDiskInDrive;
    else if (b.maxIndex == 0 && m_capIndexEvents == 0 && totalMs > 230.0)
        r = ReadResponse::rrNoDiskInDrive; /* a whole revolution with no pulse */

    if (r == ReadResponse::rrNoDiskInDrive)
        m_diskPresent = false;
    else if (m_capIndexEvents > 0)
        m_diskPresent = true;

    logLine("%s cyl %d/%d bound idx=%u linger=%u cap=%ums: %s rot=%u cells=%u trans=%u idx=%u "
            "first=%.1f extract=%.1f stop=%.1f done=%.1f ms ns/seq=%.0f rev=%ums rec0=%u/%u "
            "badspans=%u probe=%u%s",
            what, m_hwCylinder, m_hwUpperSide ? 1 : 0, b.maxIndex, b.lingerMs,
            (unsigned)(((uint64_t)b.maxTicks * 1000) / m_tickHz),
            r == ReadResponse::rrOK ? "ok" : (r == ReadResponse::rrNoDiskInDrive ? "NODISK" : "ERR"),
            m_capRotations, m_capCells, m_capTransitions, m_capIndexEvents, m_capFirstMs,
            m_capExtractMs, m_capStopMs, totalMs, m_nsPerSequence,
            (unsigned)(m_revTicks ? ((uint64_t)m_revTicks * 1000) / m_tickHz : 0),
            m_capFirstTicks, m_capFirstCount, m_capBadSpans, m_probeVerdict,
            m_abort.load() ? " ABORTED" : "");
    return r;
}

CommonBridgeTemplate::ReadResponse AdmXCopyBufferedBridge::readData(
    PLL::BridgePLL& pll, const unsigned int maxBufferSize, RotationExtractor::MFMSample* buffer,
    RotationExtractor::IndexSequenceMarker& indexMarker,
    std::function<bool(RotationExtractor::MFMSample* mfmData, const unsigned int dataLengthInBits)>
        onRotation)
{
    pll.prepareExtractor(m_isHDDisk, indexMarker);
    /* The overlap patch: a marker and a search window sized for an anchor on unique
       data, not for an arbitrary one. No-ops on an unpatched extractor. */
    if (MFMExtractionTarget* ex = pll.rotationExtractor()) {
        ex->setOverlapMatches(m_overlapMatches);
        ex->setOverlapSearchWindow(m_overlapWindow);
    }
    const Bound b = chooseBound(pll);
    return runCapture(&pll, b, buffer, maxBufferSize, &indexMarker, &onRotation, "read");
}

CommonBridgeTemplate::ReadResponse AdmXCopyBufferedBridge::readLinearData(PLL::BridgePLL& pll)
{
    /* The template has pointed the PLL at its LinearExtractor; it only wants a
       stretch of bits, the Greaseweazle gives it 220 ms. */
    Bound b;
    b.maxIndex = 0;
    b.lingerMs = 0;
    b.maxTicks = (uint32_t)((uint64_t)m_tickHz * 220 / 1000);
    return runCapture(&pll, b, nullptr, 0, nullptr, nullptr, "linear");
}

bool AdmXCopyBufferedBridge::writeData(const unsigned char* rawMFMData, const unsigned int numBits,
                                      const bool writeFromIndex, const bool)
{
    /* suggestUsingPrecompensation is accepted and ignored. The device clocks cells at
       a fixed interval and cannot shift one transition, which is what the device's own
       ADF-to-disk path has always done on real media. docs/floppy-write-support.md 8a. */
    if (!m_allowWrites || !m_deviceCanWrite) {
        logLine("write refused: allow=%d devwrite=%d", m_allowWrites ? 1 : 0,
                m_deviceCanWrite ? 1 : 0);
        return false;
    }
    if (rawMFMData == nullptr || numBits == 0 || !m_link.isOpen()) {
        m_ioError.store(!m_link.isOpen());
        return false;
    }
    if (m_writeProtect) {
        /* Say so once, so the guest stops asking - the same thing the Greaseweazle
           driver does with a drWriteProtected. */
        setWriteProtectStatus(true);
        logLine("write refused: disk is write-protected");
        return false;
    }

    const uint32_t bytes = (numBits + 7u) / 8u;
    /* The device's bound is its scratch block minus the transmit ring, which the HELLO
       reports as ringBytes; one byte of headroom because the device clocks eight cells
       past the last byte sent. Refusing here rather than being NAKed saves the round
       trip and says which number was wrong. */
    const uint32_t capacity = m_link.hello().ringBytes;
    if (capacity != 0 && bytes + 1 > capacity) {
        logLine("write refused: %u bytes over the device's %u", bytes, capacity);
        return false;
    }

    const clock::time_point t0 = clock::now();
    const uint16_t crc = XCopyLink::crc16(rawMFMData, bytes);
    const uint8_t flags = (uint8_t)(writeFromIndex ? XCL_WF_FROM_INDEX : 0);

    m_gotAck = false;
    m_ackCommand = 0;
    if (!m_link.writeTrackRequest(bytes, flags, crc)) {
        m_ioError.store(true);
        return false;
    }
    /* Every other command here is fire-and-forget; this one cannot be. Nothing may go
       on the wire until the device has said it is listening for a bulk transfer, or
       13 KB of cells arrive at a command parser. */
    if (!waitFlag(m_gotAck, 500) || m_ackCommand != XCL_CMD_WRITE_TRACK) {
        logLine("write %u bytes: no ACK", bytes);
        return false;
    }
    if (m_ackResult != XCL_RESULT_OK) {
        if (m_ackResult == XCL_RESULT_WRITE_PROTECTED) {
            m_writeProtect = true;
            setWriteProtectStatus(true);
        }
        logLine("write %u bytes: refused, result=%u", bytes, m_ackResult);
        return false;
    }
    const double ackMs = msBetween(t0, clock::now());

    m_gotWriteDone = false;
    m_writeResult = 0;
    m_writeElapsedMs = 0;
    if (!m_link.sendTrackData(rawMFMData, bytes)) {
        m_ioError.store(true);
        logLine("write %u bytes: transfer failed", bytes);
        return false;
    }
    const double sendMs = msBetween(t0, clock::now()) - ackMs;

    if (!waitFlag(m_gotWriteDone, kWriteDoneTimeoutMs)) {
        logLine("write %u bytes: no WRITE_DONE in %ums (ack=%.1f send=%.1f)", bytes,
                kWriteDoneTimeoutMs, ackMs, sendMs);
        return false;
    }

    const bool ok = (m_writeResult == XCL_RESULT_OK);
    if (m_writeResult == XCL_RESULT_WRITE_PROTECTED) {
        m_writeProtect = true;
        setWriteProtectStatus(true);
    }
    logLine("write %u bytes idx=%d ack=%.1f send=%.1f done=%.1f dev=%ums result=%u %s", bytes,
            writeFromIndex ? 1 : 0, ackMs, sendMs, msBetween(t0, clock::now()), m_writeElapsedMs,
            m_writeResult, ok ? "ok" : "FAILED");
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Link callbacks                                                             */
/* ------------------------------------------------------------------------- */

void AdmXCopyBufferedBridge::onCells(const XclRecordHeader& h, const uint8_t* packed, uint32_t bytes)
{
    if (!m_capturing)
        return;
    if (m_capFirstMs < 0)
        m_capFirstMs = msBetween(m_capStart, clock::now());
    m_capCells += h.count;
    m_capTicks += h.ticks;
    if (!m_pll)
        return; /* a presence probe: counting is all that is wanted */

    /* Real cell width for this record, on the DD timescale the extractor works
       in, from the device's measured span. A record with no tick count (should
       not happen) is taken as nominal. */
    double cellNs = kNominalCellNs;
    if (h.ticks && h.count)
        cellNs = kNominalCellNs * ((double)h.ticks / (kNominalCellTicks * (double)h.count));
    /* A record claiming a cell more than a fifth off nominal is not a slow
       sector, it is a bad span (a 16-bit tick field that wrapped, or a record
       whose start the device's PLL clock did not see): nominal is the honest
       answer, and the log shows what was claimed. */
    if (cellNs < kNominalCellNs * 0.8 || cellNs > kNominalCellNs * 1.2) {
        m_capBadSpans++;
        cellNs = kNominalCellNs;
    }
    if (m_capRecords == 0) {
        m_capFirstTicks = h.ticks;
        m_capFirstCount = h.count;
    }
    m_capRecords++;

    /* Which cell, if any, in this record carries the index pulse. */
    uint32_t indexAt = 0xFFFFFFFFu;
    if (m_haveIndexCell) {
        const int32_t rel = (int32_t)(m_indexCell - h.index);
        if (rel < 0) {
            /* Already passed (the event arrived after its record): arm from
               here rather than lose it. */
            m_indexArmed = true;
            m_armCells = 0;
            m_haveIndexCell = false;
        } else if ((uint32_t)rel < h.count) {
            indexAt = (uint32_t)rel;
            m_haveIndexCell = false;
        }
    }

    /* No sync within this many cells of an anchor and it is placed anyway: a
       custom format's 25k-cell pitch fits, an unformatted surface does not wait
       forever. Consistent from read to read either way. */
    const uint32_t kSyncWaitCells = 32768;
    /* Cells from the end of a sync word to the anchor: past a second 0x4489
       (16 cells) and into the header proper. */
    const uint32_t kAnchorAfterSync = 48;

    uint32_t cell = 0;
    for (uint32_t i = 0; i < bytes && cell < h.count; i++) {
        uint8_t v = packed[i];
        for (int bit = 7; bit >= 0 && cell < h.count; bit--, cell++) {
            if (cell == indexAt) {
                m_indexArmed = true;
                m_armCells = 0;
            }
            if (m_indexArmed)
                m_armCells++;
            const uint32_t b = (v >> bit) & 1u;
            m_shift = (m_shift << 1) | b;
            if ((m_shift & 0xFFFFu) == 0x4489u)
                m_anchorCountdown = kAnchorAfterSync;
            else if (m_anchorCountdown > 1)
                m_anchorCountdown--;
            if (b) {
                /* An anchor point: the first transition once the countdown from a
                   sync has run out, or the cap. */
                const bool anchorHere = (m_anchorCountdown == 1);
                if (anchorHere)
                    m_anchorCountdown = 0;
                /* Adaptive anchor: a probing read opens the feed at its very first
                   transition rather than waiting up to a sector pitch for a sync
                   word. Whether that anchor is usable is decided once the marker
                   and window have been fed (below). */
                if (m_probe && !m_feedOpen)
                    m_feedOpen = true;
                if (!m_feedOpen && (anchorHere || m_capCells > kSyncWaitCells))
                    m_feedOpen = true;
                bool isIndex = false;
                if (m_indexArmed && (anchorHere || m_armCells > kSyncWaitCells)) {
                    isIndex = true;
                    m_indexArmed = false;
                    m_feedOpen = true; /* an index is an anchor too */
                }
                if (m_feedOpen) {
                    const uint32_t ns = (uint32_t)((double)(m_zeros + 1) * cellNs + 0.5);
                    m_pll->submitFlux(ns, isIndex);
                    if (m_probe)
                        probeSequence(m_zeros);
                }
                m_zeros = 0;
                m_capTransitions++;
            } else {
                m_zeros++;
            }
        }
    }
}

/* One fed transition during the probe: record its sequence class, and once the
   marker plus the search window are in, decide. The extractor searches outward
   from the expected revolution point and takes the FIRST perfect match, so a
   marker that matches itself at any shift within the window (a run of zeros,
   any fill with a short period) would put the join at the wrong offset and
   fail the sector spanning the wrap - the section 5 seam. That is exactly the
   self-match test here. An ambiguous start costs what every read used to cost:
   the extractor is reset and the feed reopens at the next sync word. */
void AdmXCopyBufferedBridge::probeSequence(uint32_t zeros)
{
    const uint32_t need = m_overlapMatches + m_overlapWindow;
    if (m_probeCount < need)
        m_probeSeq[m_probeCount++] = (uint8_t)(zeros > 3 ? 3 : zeros);
    if (m_probeCount < need)
        return;
    m_probe = false;
    bool ambiguous = false;
    for (uint32_t s = 1; s <= m_overlapWindow && !ambiguous; s++) {
        bool same = true;
        for (uint32_t i = 0; i < m_overlapMatches; i++) {
            if (m_probeSeq[i] != m_probeSeq[i + s]) {
                same = false;
                break;
            }
        }
        if (same)
            ambiguous = true;
    }
    if (!ambiguous) {
        m_probeVerdict = 1;
        return;
    }
    m_probeVerdict = 2;
    /* Back to the sync-word anchor: forget what was fed, close the feed, and let
       the countdown from the next 0x4489 reopen it. An index pulse that fell in
       the probed stretch is not lost: the capture spans more than a revolution,
       so the next one lands inside the buffer and is flagged at its sync. */
    m_pll->reset();
    m_pll->prepareExtractor(m_isHDDisk, *m_capIndexMarker);
    if (MFMExtractionTarget* ex = m_pll->rotationExtractor()) {
        ex->setOverlapMatches(m_overlapMatches);
        ex->setOverlapSearchWindow(m_overlapWindow);
    }
    m_feedOpen = false;
    m_anchorCountdown = 0;
}

void AdmXCopyBufferedBridge::onFlux(const XclRecordHeader&, const uint16_t*, uint32_t)
{
    /* Never requested. */
}

void AdmXCopyBufferedBridge::onEvent(const XclRecordHeader&, const XclEventPayload& ev)
{
    applyStatusBits(ev.status);

    switch (ev.event) {
    case XCL_EV_INDEX:
        m_lastIndexSeen = clock::now();
        m_diskPresent = true;
        if (ev.arg >= m_tickHz / 8 && ev.arg <= m_tickHz / 3) /* 125..333 ms */
            m_revTicks = ev.arg;
        if (m_capturing) {
            m_capIndexEvents++;
            /* The record holding this cell has not arrived yet (docs 8a). */
            m_haveIndexCell = true;
            m_indexCell = ev.cellIndex;
        }
        break;

    case XCL_EV_TRACK_CHANGE:
        m_hwCylinder = (int)(ev.arg >> 8);
        m_hwUpperSide = (ev.arg & 0xFF) != 0;
        m_gotTrackChange = true;
        break;

    case XCL_EV_READ_DONE:
        m_readDoneReason = (uint8_t)ev.arg;
        m_readDonePulses = ev.arg2;
        m_gotReadDone = true;
        break;

    case XCL_EV_WRITE_DONE:
        m_writeResult = (uint8_t)ev.arg;
        m_writeElapsedMs = ev.arg2;
        m_gotWriteDone = true;
        break;

    case XCL_EV_DISK_CHANGE:
        m_diskPresent = ev.arg != 0;
        break;

    case XCL_EV_WRITE_PROTECT:
        m_writeProtect = ev.arg != 0;
        break;

    default:
        break;
    }
}

void AdmXCopyBufferedBridge::onStatus(const XclStatus& st)
{
    applyStatusBits(st.status);
    if (st.lastIndexPeriodTicks >= m_tickHz / 8 && st.lastIndexPeriodTicks <= m_tickHz / 3)
        m_revTicks = st.lastIndexPeriodTicks;
    m_gotStatus = true;
}

void AdmXCopyBufferedBridge::onAck(const XclAck& ack, bool)
{
    m_ackCommand = ack.command;
    m_ackResult = ack.result;
    m_gotAck = true;
}
