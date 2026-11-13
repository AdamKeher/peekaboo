/* AdmXCopyBridge - see AdmXCopyBridge.h for the design and what it does NOT do. */

#include "AdmXCopyBridge.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

using clk = std::chrono::steady_clock;

static double msSince(const clk::time_point& t)
{
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

static uint64_t nowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               clk::now().time_since_epoch())
        .count();
}

/* Per-region read speed, from the device's own per-record timing.

   OFF by default is not an option here - a constant 1000 is what driver 3 returns,
   and it is why a long-track or variable-speed protection cannot be told apart
   from a normal one. But get the normalisation wrong and it is worse than
   useless, so see the long comment in closeLap(). Flip this to false to fall
   straight back to driver 3's behaviour. */
static const bool kUseMeasuredSpeed = true;

/* How long after the motor command before the platter is at speed. The device
   uses 600 ms for its own console paths; the guest is told the motor is running
   immediately regardless (see setMotorStatus). */
static const double kSpinUpMs = 600.0;

/* How long to wait for XCL_EV_TRACK_CHANGE before assuming a seek was lost. The
   longest real move is 83 cylinders at ~3.2 ms each plus settle - measured 163.6
   ms for forty - so this is several times the worst case and only ever fires when
   something has genuinely gone wrong. */
static const double kSeekTimeoutMs = 1500.0;

/* THE INDEX DOES NOT FIRE AT THE SAME BIT CELL EVERY REVOLUTION.
 *
 * This driver was built on the premise that an exact cell index for the index
 * pulse makes the revolution boundary free - no correlation search needed, unlike
 * driver 3 (docs/floppy.md 12n.7). That premise is WRONG, and measurably so.
 *
 * Measured on cylinder 40: sectors sit 8704 bits apart, sector 9 begins at bit
 * 101,823, and the lap length reported index-to-index moves between 101,955,
 * 102,011 and 102,114 on consecutive revolutions. Sector 9's data therefore
 * straddles the buffer wrap, and the ~270-cell spread in where the index fires
 * becomes slip at exactly that join - so sector 9 decodes on some laps and not
 * others. Cylinder 0 read 20/20 only because its index happens to land in the
 * inter-sector gap, where slip does no harm.
 *
 * Knowing the index to the cell removed the FRACTIONAL-cell error driver 3 fought.
 * It did not, and cannot, remove the jitter of a physical sensor.
 *
 * So do what driver 3 ended up doing, for the same reason: cut where the data
 * repeats, not where the index fires. The index still triggers the cut - UAE
 * derives its index pulse from position 0 - but the LENGTH comes from
 * correlation. Capturing a margin past the index is what makes the search
 * possible, and unlike driver 3 it is nearly free here, because the stream is
 * continuous and those cells have already arrived.
 *
 * CORRECTING THE LENGTH IS NOT ENOUGH ON ITS OWN, and this cost a round trip to
 * learn. A lap anchored at its own index pulse starts at a slightly different
 * ANGLE from the lap before it, by exactly the sensor jitter. So when the guest
 * crosses position 0 and the driver promotes a fresh lap underneath it, a sector
 * straddling that boundary gets ~135 cells of slip spliced into the middle of
 * it - guaranteed corruption, every promote. Cylinder 0 survived only because
 * its index happens to land in the inter-sector gap; cylinder 40 has sector 9
 * across the boundary and read 9 laps in 20.
 *
 * So the anchor CHAINS: lap N+1 begins where lap N was cut, at
 * m_lapStartCell + length. Position 0 then means one fixed angle for as long as
 * the drive keeps turning, consecutive laps line up bit for bit, and the index
 * is needed only to establish the phase once and to trigger each cut. That is
 * what "the index is only needed to say which angle position 0 is" was always
 * supposed to mean. */
/* The search window has to be wider than the index jitter, or the correlator
   cannot reach the true offset and confidently returns the best WRONG one.
   Measured index-to-index lengths on this drive span 101,955-102,144 - about 270
   cells - so a +/-192 window was too small, and chaining the anchor on top of a
   wrong offset took cylinder 40 from 9 laps in 20 to 0. Margin must exceed
   search + match, or correlateLength has nothing to compare against and falls
   back to the index. */
/* These three are EMPIRICAL, and the sensitivity is sharp enough to be worth
   recording. Measured on cylinder 40 - the track whose sector 9 straddles the
   buffer wrap - as laps in 20 delivering all 11 sectors:

     margin  match  search   result
       512    256     192    9/20   window narrower than the jitter
      1024    256     512   18/20   <- this
      2048    512    1024   13/20   too wide: more chance of a spurious tie
      1536    512     512    0/20   match too long to survive bit jitter

   Search must exceed the index jitter (~270 cells here) or the correlator cannot
   reach the true offset and confidently returns the best wrong one. Match must
   stay short enough that two readings of the same stretch still agree on ~90% of
   cells. Margin must exceed search + match or there is nothing to compare. */
static const uint32_t kJoinMargin = 1024; /* cells captured past the index      */
static const uint32_t kJoinMatch = 256;   /* cells compared when correlating    */
static const uint32_t kJoinSearch = 512;  /* +/- cells searched around expected */

/* ------------------------------------------------------------------------- */

AdmXCopyBridge::AdmXCopyBridge(FloppyBridge::BridgeDensityMode densityMode,
                               bool autoDetectComPort, const char* comPort,
                               FloppyBridge::DriveSelection driveCable)
    : m_comPort(comPort ? comPort : ""), m_densityMode(densityMode), m_driveCable(driveCable)
{
    /* autoDetectComPort is accepted for signature compatibility with the other
       drivers and deliberately ignored: an auto-scan costs 819 ms inside the
       emulation thread, and AttachBridgeDrive already refuses an empty port. */
    (void)autoDetectComPort;

    for (int i = 0; i < 3; i++)
        m_lap[i].reset(-1);
    m_backLap.store(&m_lap[0]);
    m_backTag.store(-1);
}

AdmXCopyBridge::~AdmXCopyBridge()
{
    shutdown();
}

const FloppyDiskBridge::BridgeDriver* AdmXCopyBridge::staticBridgeInformation()
{
    static const FloppyDiskBridge::BridgeDriver driver = {
        "XCopy Standalone (streaming)", "https://github.com/AdamKeher/XCopyStandalone",
        "Adam Keher", "Adam Keher",
        /* ConfigOption_ComPort, spelled numerically to avoid depending on
           floppybridge_lib.h - the same choice AdmStreamingBridge makes.

           Deliberately nothing else. No AutoCache and no SmartSpeed, because this
           driver has neither and should not offer settings it silently ignores.
           No AutoDetectComport (see the constructor). No DriveABCable: the XCopy
           board drives one drive off its own cable and the select line is fixed
           in its pin map, so there is nothing to choose. */
        0x02};
    return &driver;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

bool AdmXCopyBridge::initialise()
{
    shutdown();
    m_lastError.clear();
    m_ioError = false;

    /* Instrumentation for runs INSIDE the emulator - see the header. Append
       mode: an attach happens on every emulator session and truncating lost the
       exact windows that needed reading. */
    if (const char* logPath = getenv("ADM_XCOPY_LOG")) {
        m_log = fopen(logPath, "a");
        liveLog("---- open port=%s ----\n", m_comPort.c_str());
    }

    /* Live serving is the default; ADM_XC_LIVE=0 selects the photo model
       (ADM_XC_LIVE=1 still forces live, for scripted A/Bs) - see m_liveServe. */
    if (const char* live = getenv("ADM_XC_LIVE"))
        m_liveServe = live[0] == '1';
    m_lockOffset = 0;
    m_lockGen = 0xFFFFFFFFu;

    /* The serving lead, in cells (500/ms at DD). Firmware v717.26's pacing
       flush cut delivery jitter to the record cadence (docs/floppy-xcopy.md
       8n), so the lead is now a sweepable bench parameter rather than a
       constant chosen to ride above 30 ms bursts. */
    /* The dynamic stall (see the header). On by default; ADM_XC_STALL=0
       restores the pure static-lead behaviour of core 1.67.0 for A/B - and
       with it the wide lead below, because a small lead without the stall is
       not marginal but fatal (measured 0/3 boots at 4096). The margin's
       floor is the writer's record granularity plus delivery jitter (~1 ms
       since firmware v717.26); 512 over 256-cell records was the swept
       optimum, 256 already degrades (docs/floppy-xcopy.md 8r). */
    m_stallEnable = true;
    if (const char* st = getenv("ADM_XC_STALL"))
        m_stallEnable = st[0] != '0';
    m_stallMargin = 512;
    if (const char* sm = getenv("ADM_XC_STALL_MARGIN")) {
        const unsigned long v = strtoul(sm, nullptr, 10);
        if (v >= 256 && v <= 16384)
            m_stallMargin = (unsigned int)v;
    }
    m_stallGiveUpGen = 0xFFFFFFFFu;
    m_stallCount = 0;
    m_stallSleeps = 0;

    m_phaseLead = m_stallEnable ? kXcPhaseLeadStalled : kXcPhaseLead;
    if (const char* lead = getenv("ADM_XC_LIVE_LEAD")) {
        const unsigned long v = strtoul(lead, nullptr, 10);
        if (v >= 512 && v <= 65536)
            m_phaseLead = (unsigned int)v;
    }

    m_link.setSink(this);
    const XCopy::LinkResult r = m_link.open(m_comPort);
    if (r != XCopy::LinkResult::Ok) {
        m_lastError = m_link.lastError().empty() ? XCopy::toString(r) : m_link.lastError();
        m_ioError = true;
        return false;
    }

    /* Density. bdmAuto leaves the device to work it out from the media, which is
       what its own console paths already do. */
    if (m_densityMode == FloppyBridge::BridgeDensityMode::bdmDDOnly)
        m_link.config(XCL_CFG_DENSITY, 1);
    else if (m_densityMode == FloppyBridge::BridgeDensityMode::bdmHDOnly)
        m_link.config(XCL_CFG_DENSITY, 2);
    else
        m_link.config(XCL_CFG_DENSITY, 0);

    /* The device's own defaults were chosen for a UI-driven copier and are
       generous. These are the ones a live drive can afford; a seek is on the
       critical path here in a way it never was for the console. Both are
       sweepable from the environment (docs/floppy-xcopy-subframe-pacing.md,
       avenue 3): the drive's datasheet floor may be below these, and with the
       stall shrinking the lead they become the dominant landing terms. */
    unsigned long stepUs = 3000;
    if (const char* su = getenv("ADM_XC_STEP_US")) {
        const unsigned long v = strtoul(su, nullptr, 10);
        if (v >= 1000 && v <= 6000)
            stepUs = v;
    }
    unsigned long settleUs = 15000;
    if (const char* se = getenv("ADM_XC_SETTLE_US")) {
        const unsigned long v = strtoul(se, nullptr, 10);
        if (v >= 2000 && v <= 30000)
            settleUs = v;
    }
    /* The device's direction-line settle defaults to 20 ms and is paid on EVERY
       seek, direction changed or not: the whole of the 41 vs 31 ms one-cylinder
       seek against the Greaseweazle (docs/floppy-xcopy-buffered.md 7). Driver 5
       runs it at 1 ms. Here the default STAYS at the device's 20 ms: measured
       2026-09-02 (docs/floppy-xcopy.md 8t), 1 ms vs 20 ms on the standard disk
       was 37-39 s / 1.50-1.58 against 38-39 s / 1.43-1.56 - no difference the
       boot benchmark can see - and the landing sweep of 8r was done at 20 ms.
       The knob is an override for a future sweep (same clamp as driver 5). */
    unsigned long dirUs = 20000;
    if (const char* du = getenv("ADM_XC_DIR_US")) {
        const unsigned long v = strtoul(du, nullptr, 10);
        if (v >= 100 && v <= 20000)
            dirUs = v;
    }
    m_link.config(XCL_CFG_STEP_INTERVAL_US, (uint32_t)stepUs);
    m_link.config(XCL_CFG_SEEK_SETTLE_US, (uint32_t)settleUs);
    m_link.config(XCL_CFG_DIR_SETTLE_US, (uint32_t)dirUs);

    /* If the host stops pumping for this long the device parks itself, so a
       crashed emulator cannot leave a drive spinning against a platter. */
    m_link.config(XCL_CFG_WATCHDOG_MS, 5000);

    /* Delivery pacing (firmware v717.26+). The device defaults to 1000 us on
       its own; sending it explicitly states the intent and gives an A/B lever
       under the emulator (0 = the old batched delivery). Old firmware NAKs
       the key as UNSUPPORTED and keeps its behaviour - harmless. */
    unsigned long flushUs = 1000;
    if (const char* f = getenv("ADM_XC_TX_FLUSH_US"))
        flushUs = strtoul(f, nullptr, 10);
    if (flushUs <= 100000)
        m_link.config(XCL_CFG_TX_FLUSH_US, (uint32_t)flushUs);

    /* Watermark granularity: cells per MFM record, device default 1024 (~2 ms
       at DD). The stalled 2 ms lead paces the guest at record granularity, so
       the shipping default is now 256 (~0.5 ms) - part of the swept optimum
       (docs/floppy-xcopy.md 8r). The cost is framing overhead (14 bytes per
       record) against a link measured at 1 MB/s: ~4x the records, still under
       a tenth of the bandwidth. With the stall disabled the device default is
       left alone, exactly as core 1.67.0 behaved. */
    unsigned long recCells = m_stallEnable ? 256 : 0;
    if (const char* rc = getenv("ADM_XC_RECORD_CELLS")) {
        const unsigned long v = strtoul(rc, nullptr, 10);
        if (v >= 64 && v <= 1024 && (v & 7) == 0)
            recCells = v;
    }
    if (recCells)
        m_link.config(XCL_CFG_RECORD_CELLS, (uint32_t)recCells);

    /* Establish a known head position before the guest asks for anything. */
    m_link.recalibrate();
    m_wantCylinder = 0;
    m_wantSide = false;
    m_hwCylinder = -1;
    memset(m_alignCache, 0, sizeof(m_alignCache));

    m_running = true;
    m_thread = std::thread(&AdmXCopyBridge::threadMain, this);
    return true;
}

void AdmXCopyBridge::shutdown()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();

    m_link.setSink(nullptr);
    m_link.close();

    m_streaming = false;
    m_motorReady = false;
    m_hwMotorOn = false;
    m_hwCylinder = -1;

    if (m_log) {
        fclose(m_log);
        m_log = nullptr;
    }
}

/* Never on the per-bit path. Callers are per-revolution (about five a second)
   or per-seek, from either thread; fprintf serialises internally and a rare
   interleaved line costs nothing. */
void AdmXCopyBridge::liveLog(const char* fmt, ...)
{
    if (!m_log)
        return;
    fprintf(m_log, "%llu ", (unsigned long long)nowMs());
    va_list args;
    va_start(args, fmt);
    vfprintf(m_log, fmt, args);
    va_end(args);
    fflush(m_log);
}

/* ------------------------------------------------------------------------- */
/* Reader thread                                                              */
/* ------------------------------------------------------------------------- */

void AdmXCopyBridge::threadMain()
{
#ifdef _WIN32
    /* The emulation thread burns a full core; on a loaded host the reader was
       being scheduled late and the USB stream arrived in bursts - which is the
       delivery jitter the phase lock's lead has to ride above. The platter
       does not wait for the scheduler. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    /* The loop below sleeps 1 ms when the port is dry - and a 1 ms sleep is up
       to 15.6 ms unless somebody raises the system timer resolution. That lump
       is indistinguishable from USB delivery batching at the watermark: the
       firmware's v717.26 pacing flush (docs/floppy-xcopy.md 8n) cut delivery to
       the OS buffer to ~2 ms, and the live-path attempt figures did not move at
       all until this was raised to match. Loaded dynamically so the core gains
       no link dependency; process-scoped on Win10 2004+, restored on exit. */
    typedef unsigned int(WINAPI * TimePeriodFn)(unsigned int);
    TimePeriodFn timeBeginPeriodFn = nullptr;
    TimePeriodFn timeEndPeriodFn = nullptr;
    if (HMODULE winmm = LoadLibraryA("winmm.dll")) {
        timeBeginPeriodFn = (TimePeriodFn)GetProcAddress(winmm, "timeBeginPeriod");
        timeEndPeriodFn = (TimePeriodFn)GetProcAddress(winmm, "timeEndPeriod");
    }
    if (timeBeginPeriodFn)
        timeBeginPeriodFn(1);
#endif
    /* What we last ASKED the device for, as opposed to where it has reported
       arriving (m_hwCylinder, set from XCL_EV_TRACK_CHANGE). Keeping the two
       apart is what stops a seek being re-sent every millisecond while the head
       is still moving. */
    int sentCylinder = -1;
    bool sentSide = false;
    bool sentMotor = false;
    clk::time_point motorOnAt = clk::now();
    clk::time_point lastPing = clk::now();
    clk::time_point lastStatus = clk::now();
    clk::time_point seekSentAt = clk::now();

    /* initialise() issued a RECAL, which moves the head and makes the device busy.
       Treat it as a seek already in flight so the first real one waits for it
       instead of being refused. */
    m_seekInFlight = true;

    while (m_running.load()) {
        if (!m_link.isOpen()) {
            m_ioError = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        /* --- motor ------------------------------------------------------- */
        const bool wantMotor = m_wantMotor.load();
        if (wantMotor != sentMotor) {
            m_link.motor(wantMotor);
            sentMotor = wantMotor;
            m_hwMotorOn = wantMotor;
            m_motorReady = false;
            if (wantMotor)
                motorOnAt = clk::now();
        }
        if (wantMotor && !m_motorReady.load() && msSince(motorOnAt) > kSpinUpMs)
            m_motorReady = true;

        /* --- the stream: started once, then left alone -------------------- */
        if (wantMotor && !m_streaming.load()) {
            m_link.streamStart(XCL_MODE_MFM);
            m_streaming = true;
        } else if (!wantMotor && m_streaming.load()) {
            m_link.streamStop();
            m_streaming = false;
            abandonLap();
        }

        /* --- the seek, in band -------------------------------------------
           This is the whole reason the driver exists. No stop, no drain, no
           restart, no re-handshake: the command goes into the running stream and
           the device answers with XCL_EV_TRACK_CHANGE at the exact cell index
           where the new surface begins. */
        const int wantCyl = m_wantCylinder.load();
        const bool wantSide = m_wantSide.load();

        /* A refusal means the head did NOT move. Forget what we thought we had
           sent so the request is made again. */
        if (m_seekRefused.exchange(false)) {
            sentCylinder = -1;
            m_seekInFlight = false;
        }

        /* Never stack seeks. The device answers XCL_RESULT_BUSY if one is already
           stepping, and Kickstart bounces the head far faster than a 40-cylinder
           move completes, so issuing on every change loses most of them. Wait for
           XCL_EV_TRACK_CHANGE - with a timeout, because a device that never
           answers must not wedge the head forever. */
        if (m_seekInFlight.load() && msSince(seekSentAt) > kSeekTimeoutMs) {
            m_seekInFlight = false;
            sentCylinder = -1;
        }

        if ((wantCyl != sentCylinder || wantSide != sentSide) && !m_seekInFlight.load()) {
            if (m_log)
                liveLog("seek cyl=%d side=%d\n", wantCyl, wantSide ? 1 : 0);
            m_link.seek((unsigned int)wantCyl, wantSide);
            m_seekSentMs.store(nowMs());
            sentCylinder = wantCyl;
            sentSide = wantSide;
            m_seekInFlight = true;
            seekSentAt = clk::now();
        }

        /* --- keepalive ---------------------------------------------------- */
        if (msSince(lastPing) > 1000.0) {
            m_link.ping();
            lastPing = clk::now();
        }

        /* --- media presence ------------------------------------------------
           XCL_EV_DISK_CHANGE is emitted on a CHANGE, and the device latches the
           starting state when the session opens. Attach with an empty drive and
           there is no edge, so no event ever arrives and an optimistic default
           stands forever - which is exactly what reported a disk in an empty
           drive. Ask instead of waiting to be told. */
        if (msSince(lastStatus) > 500.0) {
            /* Only stamp when nothing is outstanding, so the figure pairs one
               request with the first reply after it. See debugStatusRttMs(). */
            uint64_t expect = 0;
            m_statusReqMs.compare_exchange_strong(expect, nowMs());
            m_link.requestStatus();
            lastStatus = clk::now();
        }

        if (m_link.pump() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        m_rxWaiting.store(m_link.bytesWaiting());
    }

    if (m_link.isOpen()) {
        m_link.streamStop();
        m_link.motor(false);
    }

#ifdef _WIN32
    if (timeEndPeriodFn)
        timeEndPeriodFn(1);
#endif
}

/* ------------------------------------------------------------------------- */
/* Lap assembly - all on the reader thread                                    */
/* ------------------------------------------------------------------------- */

uint32_t AdmXCopyBridge::correlateLength(const AdmXCopyLap* lap, uint32_t nominal,
                                        uint32_t covered, uint32_t* outScore) const
{
    if (outScore)
        *outScore = 0;
    /* Look for the offset L at which the start of the lap begins to repeat. The
       platter is a circle, so cells[0..N) and cells[L..L+N) are the same physical
       stretch one revolution apart, and the L that matches best is the true
       revolution length regardless of when the index sensor fired. */
    if (nominal <= kJoinSearch || covered < nominal + kJoinSearch + kJoinMatch)
        return nominal; /* not enough margin captured - fall back to the index */

    uint32_t bestLen = nominal;
    uint32_t bestScore = 0;

    for (uint32_t off = nominal - kJoinSearch; off <= nominal + kJoinSearch; off++) {
        uint32_t score = 0;
        for (uint32_t i = 0; i < kJoinMatch; i++)
            if (lap->bit(i) == lap->bit(off + i))
                score++;

        /* Strictly greater, and the search runs outwards from the nominal point,
           so a tie resolves to the offset nearest the index rather than to
           whichever the loop happened to reach first. The inter-sector gap is
           perfectly periodic 0xAAAA, so ties are common and picking the wrong one
           puts the join hundreds of cells out while still looking flawless -
           driver 3 hit exactly this (docs/floppy.md 12n.7). */
        const bool better = score > bestScore ||
                            (score == bestScore &&
                             (off > nominal ? off - nominal : nominal - off) <
                                 (bestLen > nominal ? bestLen - nominal : nominal - bestLen));
        if (better) {
            bestScore = score;
            bestLen = off;
        }
    }

    /* A weak best match means the correlation found nothing real - an unformatted
       or badly damaged track. Trust the index rather than a guess. */
    if (outScore)
        *outScore = bestScore;

    if (bestScore * 100u < kJoinMatch * 90u)
        return nominal;

    return bestLen;
}

uint32_t AdmXCopyBridge::findGapOffset(const AdmXCopyLap* lap, uint32_t length) const
{
    /* A ONE-REVOLUTION BUFFER CANNOT SERVE A SECTOR THAT CROSSES ITS OWN WRAP.
     *
     * When the guest runs off the end of the buffer it wraps to position 0, and
     * those two stretches are the same physical angles read a revolution APART -
     * two separate readings of the same flux. Most cells agree; a few do not,
     * because that is what reading flux is like. Inside a sector's data one
     * disagreeing cell fails the checksum, so a sector spanning position 0 fails
     * essentially every time.
     *
     * Measured with the sector data actually checked (docs/floppy-xcopy.md 8g):
     * cylinder 40 lost sector 10 on 20 laps of 20, every failure in the last tenth
     * of the revolution, while cylinder 0 read 19 of 20. The difference is not
     * quality - the device reads both with zero errors - it is where the boundary
     * happens to fall.
     *
     * So put the join in the inter-sector gap deliberately. AmigaDOS finds sectors
     * by sync word, not by angle from the index, so moving the boundary costs
     * nothing.
     *
     * THE ARITHMETIC THIS GETS WRONG IF YOU RUSH IT, AND DID:
     *
     * 1. The gap is the SPAN BETWEEN TWO SYNCS MINUS ONE SECTOR, not the span.
     *    Sectors sit 8704 cells apart on a DD track (544 bytes, two MFM cells per
     *    bit) and the gap is only ~6300, so the longest sync-to-sync distance is
     *    ~15000 and its midpoint lands squarely inside the preceding sector's
     *    data - the very fault this is here to avoid. The pitch is measured here
     *    as the median span rather than assumed, so a non-standard format is
     *    handled by the same code.
     *
     * 2. The scan has to be CIRCULAR. The long span is the one that contains the
     *    join, so once the boundary has been moved anywhere near the gap the two
     *    syncs either side of it sit at opposite ends of the buffer and a linear
     *    scan cannot see the pair at all. A linear version reported "no gap" on
     *    every track of a perfectly ordinary disk.
     *
     * 3. Count the 32-bit PAIR, not single 0x4489 words. Every sector carries two
     *    back to back, so counting singles puts a 16-cell span in the list and
     *    poisons the median.
     *
     * A track with no gap to find - a custom or copy-protected format - returns
     * kXcNoGap and the boundary is left where the chain put it, because guessing
     * would be worse. kXcNoGap is distinct from 0 - "the boundary is already at
     * the gap's midpoint" is a real answer and latching it as "no gap" left a
     * track published with its boundary mid-sector forever, which reads as one
     * deterministically bad sector on every lap (measured: cylinder 74 sector 10,
     * six laps straight, 10 of 11 every time, until AmigaDOS gave up).
     */
    if (length < 4096)
        return kXcNoGap;

    /* 11 on a DD track; the cap is slack for a track carrying spurious syncs. */
    static const unsigned int kMaxSyncs = 64;
    uint32_t sync[kMaxSyncs];
    unsigned int n = 0;

    uint32_t sr = 0;
    for (uint32_t i = 0; i < length && n < kMaxSyncs; i++) {
        sr = (sr << 1) | (lap->bit(i) ? 1u : 0u);
        if (sr == 0x44894489u)
            sync[n++] = i;
    }
    m_gapSyncs.store(n);

    /* Eight, not four. A custom-format track can carry a handful of 0x4489
       pairs without being AmigaDOS at all - measured on a copy-protected disk:
       cylinder 1 showed exactly 4 syncs at a 25,314-cell pitch, this function
       claimed a 2,492-cell "gap" from them, and the 28,786-cell shift it
       prescribed was a guess that moved the boundary to an arbitrary angle.
       The transient degraded looks on real AmigaDOS tracks (8k) all saw 12-13
       syncs - a spurious sync SPLITTING the gap - never fewer than the real
       eleven. Below eight this is not a gapped track and the answer is kXcNoGap,
       which the caller treats as "custom format, publish as it stands". */
    if (n < 8)
        return kXcNoGap;

    /* Spans between consecutive syncs, the last one wrapping the circle. */
    uint32_t span[kMaxSyncs];
    for (unsigned int k = 0; k < n; k++)
        span[k] = (k + 1 < n) ? (sync[k + 1] - sync[k]) : (length - sync[k] + sync[0]);

    /* Median span = the sector pitch. Insertion sort on a copy; n is 11. */
    uint32_t sorted[kMaxSyncs];
    memcpy(sorted, span, n * sizeof(uint32_t));
    for (unsigned int a = 1; a < n; a++) {
        const uint32_t v = sorted[a];
        unsigned int b = a;
        while (b > 0 && sorted[b - 1] > v) {
            sorted[b] = sorted[b - 1];
            b--;
        }
        sorted[b] = v;
    }
    const uint32_t pitch = sorted[n / 2];

    unsigned int best = 0;
    for (unsigned int k = 1; k < n; k++)
        if (span[k] > span[best])
            best = k;

    m_gapPitch.store(pitch);
    m_gapSyncAt.store(sync[best]);
    if (span[best] <= pitch)
        return kXcNoGap;
    const uint32_t gapLen = span[best] - pitch;
    m_gapLen.store(gapLen);

    /* An AmigaDOS DD track has ~6300 cells of gap. Well under that is either a
       format with no slack to cut into or a misread, and both are better left
       alone than cut at a guess. */
    if (gapLen < 2000)
        return kXcNoGap;

    /* The gap sits at the END of the long span - the sector's own data occupies
       the pitch immediately after its sync - so aim past one sector and then half
       way into what is left. */
    return (sync[best] + pitch + gapLen / 2u) % length;
}

void AdmXCopyBridge::startLap(uint32_t indexCell, int tag)
{
    AdmXCopyLap* back = &m_lap[m_backIdx];
    back->reset(tag);
    m_lapStartCell = indexCell;
    m_haveLapStart = true;
    m_backFrom = 0;
    m_backTo = 0;
    m_backTag.store(tag);
    m_backLap.store(back);
    m_lapTicks = 0;
    m_lapCells = 0;
    m_restartCoverage = false;
    m_lapHoles = 0;
    m_lapFlagged = 0;
}

void AdmXCopyBridge::abandonLap()
{
    /* Only the partial revolution in flight is dropped. What has already been
       published stays: the guest may still be reading it, and if the head has not
       actually left this track it is still correct data.

       Driver 3 learned this the hard way (docs/floppy.md 12n.6): Kickstart
       verifies head position by bouncing cylinder 1 -> 0 faster than a stream
       completes, and wiping everything on each bounce made booting a 1-in-3 coin
       toss. Tags are what make this safe - getMFMBit will not serve a lap whose
       tag does not match where the guest thinks the head is. */
    m_backFrom = 0;
    m_backTo = 0;
    m_haveExpected = false;

    /* The snapshot's angle mapping rests on the anchor chain, and every caller
       of this - a head move, a stream stop, a media change - is a point where
       that chain is about to break or has. The published front lap survives on
       its tag; the snapshot does not get the same latitude, because unlike the
       front it is indexed relative to the CURRENT lap's anchor. */
    m_prevTo.store(0);
    m_prevTag.store(-1);
}

void AdmXCopyBridge::closeLapAt(uint32_t cutCell, uint32_t pulseCell)
{
    AdmXCopyLap* back = &m_lap[m_backIdx];

    /* Where this lap is cut. Hoisted out of the block below because the carry at
       the end of this function needs it: the cells past the cut belong to the
       NEXT lap. */
    uint32_t length = 0;
    uint32_t lapScore = 0, lapNominal = 0, lapFrom = 0, lapTo = 0;
    bool lapPublished = false;

    /* How far the lap boundary has to move to land in the inter-sector gap of the
       surface now under the head. Non-zero only on the first complete lap after a
       track change, and the reason that lap is not published - see below. */
    uint32_t phaseShift = 0;

    /* Where the new lap will be anchored. Normally the cut point, so the phase
       chains; the index only when the chain has to be re-established. */
    const uint32_t oldStart = m_lapStartCell;
    bool reanchorOnIndex = !m_haveLapStart;

    if (m_haveLapStart) {
        const unsigned int from = m_backFrom.load();
        const unsigned int to = m_backTo.load();
        /* The span this lap actually ran, anchor to cut. With the cut placed by
           the chain this mostly restates the chain; the platter's own say is
           `period` below. */
        const uint32_t nominal = cutCell - oldStart;
        /* The platter's revolution, pulse to pulse - the one signal that stays
           honest whatever the boundary does. */
        const uint32_t period = m_indexPeriodCells;
        const bool periodValid = period > 90000 && period < kXcMaxLapCells;

        const uint32_t expected =
            (m_lastLapLength > 90000 && m_lastLapLength < kXcMaxLapCells) ? m_lastLapLength
                                                                         : nominal;
        uint32_t score = 0;
        bool cutByCorrelation = false;
        /* Live mode: the delay line ignores lap cuts entirely, so the only
           things a close feeds are the damped length and m_liveSpeed - and
           correlateLength is a multi-millisecond bit search ON THE READER
           THREAD. Every stall here parks the write cursor while the guest
           keeps consuming, which is the burst the serving lead has to ride
           above: gating the search off is what lets the lead come down. */
        if (!m_liveServe && expected == m_lastLapLength && periodValid && m_trackGapless &&
            from == 0) {
            /* A GAPLESS TRACK'S WRAP SITS MID-DATA, AND SLIP THERE IS FATAL.
               The damped index period keeps the chain centred on the platter's
               MEAN revolution, but each individual lap differs from that mean
               by tens of cells of real spindle wobble - and on a gapless
               (custom / copy-protected) track that slip lands inside somebody's
               data, so whatever spans the wrap fails on EVERY lap and no retry
               count recovers it. Correlate this lap's own repeat point instead:
               zero slip at the wrap, a self-consistent ring. Where correlation
               is ambiguous (a periodic stretch) slip is harmless, and where
               slip is harmful (real data) correlation is unambiguous - the two
               failure modes cannot coincide. */
            const uint32_t c = correlateLength(back, expected, to, &score);
            if (score * 100u >= kJoinMatch * 90u) {
                length = c;
                cutByCorrelation = true;
            }
        }
        if (!cutByCorrelation && expected == m_lastLapLength && periodValid) {
            /* THE CORRELATOR IS BLIND ONCE THE BOUNDARY SITS IN THE GAP.
               The inter-sector gap is pure 0xAAAA, so with the lap cut mid-gap
               every offset in the window matches perfectly, the tie-break snaps
               to `expected`, and the chosen length never moves again - measured:
               len pinned at 102,001 lap after lap while the platter walked away
               40 cells a lap, until the drift guard fired.

               A precise length is not needed - what is needed is that the error
               does not ACCUMULATE. So take a damped step towards the measured
               index period each lap: the chain stays centred on the true
               revolution with the sensor's ~135-cell jitter averaged down, and
               there is nothing to tie-break wrong because nothing is compared. */
            const int32_t err = (int32_t)(period - expected);
            int32_t step = err / 4;
            if (step > 64)
                step = 64;
            if (step < -64)
                step = -64;
            length = expected + step;
        } else if (!cutByCorrelation) {
            length = (!m_liveServe && from == 0) ? correlateLength(back, expected, to, &score)
                                                 : expected;
        }

        lapScore = score;
        lapNominal = periodValid ? period : nominal;
        lapFrom = from;
        lapTo = to;

        /* The platter's true mean cell time this lap, as a getMFMSpeed value
           (1000 = the nominal 2 us cell). This is what slaves the guest's bit
           clock to the platter while serving live - see m_liveSpeed. */
        if (kUseMeasuredSpeed && m_lapCells > 4096) {
            const uint32_t tickHz = m_link.hello().tickHz;
            if (tickHz > 0) {
                /* ticks per cell * 1000 / ticks per nominal cell */
                const uint64_t s =
                    (m_lapTicks * 1000u * 500000u) / (m_lapCells * (uint64_t)tickHz);
                if (s >= 900 && s <= 1100)
                    m_liveSpeed.store((unsigned int)s);
            }
        }

        /* A LAP THAT COVERED NOTHING MEANS THE ANCHOR NO LONGER MAPS TO THE
           STREAM. Measured under the emulator on a copy-protected disk: a
           forward anchor shift plus ~60 ms of reader lag put the anchor
           permanently ahead of the stream - every cell of every lap was dropped
           as a negative angle, every lap closed with arc=[0,0), and because the
           dead-reckoned chain advances by ~one period per close, the state was
           a fixed point: NOTHING published for seven seconds while the guest's
           loader retried into a stale snapshot and finally reported a track
           read error. One empty lap on a spinning platter with a healthy link
           is proof enough; re-anchor from the pulse now, and abandon alignment
           for the rest of this dwell (m_alignAbandoned) so the recovery cannot
           re-apply the shift that caused it and oscillate. The track is then
           published as it stands, gapless-style, which is degraded by at most
           the wrap seam - against "nothing at all", not a close call. */
        /* from == to == 0 exactly: a lap that closed with startLap's zeroed arc
           untouched. A close straddling a head move also has an empty arc, but
           at the LANDING angle (TRACK_CHANGE sets both ends there, nonzero),
           and it must not trip this - the anchor there is fine. */
        const bool emptyLap = from == 0 && to == 0 && expected == m_lastLapLength &&
                              !m_seekInFlight.load() && !m_haveSkip;
        if (emptyLap) {
            m_alignAbandoned = true;
            m_trackGapless = true;
            if (m_log)
                liveLog("empty lap tag=%d -> reanchor, alignment abandoned\n", back->trackTag);
        }

        /* If the chain has wandered a long way from the platter's own revolution
           - a speed change, a media change, a long stall, a missed pulse - stop
           chaining and re-acquire from the pulse. */
        const uint32_t ref = periodValid ? period : nominal;
        const uint32_t drift = (ref > length) ? ref - length : length - ref;
        if (drift > 3000 || emptyLap) {
            /* Cut at the pulse itself: the chain's prediction is the thing that
               is broken. The lap's contents are still self-consistent up to
               that span. */
            length = pulseCell - oldStart;
            reanchorOnIndex = true;
            m_lastLapLength = 0;
            /* Re-anchoring moves the boundary back to wherever the pulse fires,
               which is mid-sector for most tracks - so the gap has to be
               re-measured, on a CLEAN lap. The alignment and publish blocks
               below are gated on !reanchorOnIndex for the same reason: this
               lap's length is raw and jittery, and 1.58.0 measured what
               aligning on it does - findGapOffset on a mis-sized lap returned a
               103,347-cell "shift" on a 104,785-cell "revolution". */
            m_phaseAligned = false;
            m_phaseAlignedFlag.store(false);
        }

        /* The measured revolution. It is not 100,000 and it is not constant -
           this drive runs ~1.3% slow and varies 101,306-101,360 lap to lap
           (docs/floppy.md 12f). Publishing each lap at its OWN length is what
           makes that a non-issue instead of one corrupt sector per read. */
        if (length > 90000 && length < kXcMaxLapCells) {
            m_measuredRevCells = length;

            /* Adopt the measured length immediately while nothing is being
               served. maxMFMBitPosition() starts on a placeholder, and leaving
               the guest on it until the first promote would mean its first
               revolution is the wrong length - the exact fault that put sector 0
               bad on 100% of reads in docs/floppy.md 12n.4. Once a lap IS being
               served the length only changes at a promote, where the guest is
               crossing the index anyway. */
            if (m_frontLap.load() == nullptr)
                m_trackLenCells = length;

            /* Complete only if the lap covered the whole revolution. A lap that
               began mid-revolution - the first one after a seek - is served live
               through the back-lap path but never published, because promoting a
               partial buffer would hand the guest a hole. */
            const bool complete = (from == 0) && (to >= length) && (back->trackTag >= 0);

            /* THE FIRST COMPLETE LAP AFTER A TRACK CHANGE IS NOT SERVABLE, AND
               PUBLISHING IT COSTS THE GUEST A SECTOR EVERY TIME.

               The anchor chains, so after a head move the boundary is still at
               the angle of the PREVIOUS surface's inter-sector gap - which on the
               new track is an arbitrary angle, and lands inside a sector eight or
               nine times out of eleven. The sector spanning the boundary then
               fails its data checksum on every read, because the two sides of the
               wrap are separate readings of the same flux taken a revolution
               apart (the finding in docs/floppy.md 13b).

               Measured before this: every read within about two revolutions of a
               seek returned all 11 sector HEADERS and exactly 10 sectors whose
               data checksum verified. A whole-disk sweep that reads three times
               per track - which is all a loader does - scored 7 of 60, while a
               soak that keeps reading the same track long enough to get past the
               window scored 17 of 20. The difference was entirely this.

               So measure the gap on this lap, shift the anchor, and do NOT serve
               it. The next lap is cut in the gap and is the first one published.
               It costs one revolution after a head move, on top of a seek that
               already costs far more, and it is the difference between correct
               and nearly correct.

               A track with no gap to find - a custom or copy-protected format -
               returns 0 here and is published as it stands, because there is
               nothing better to move the boundary to. */
            /* The boundary's angle after the index pulse, for the cache. Both
               sides of the subtraction ride the same free-running counter, and
               the anchor sits within a revolution of the pulse that triggered
               this close, so the signed cast is safe. */
            const bool cacheable =
                periodValid && back->trackTag >= 0 && (unsigned)back->trackTag < 168;
            const uint32_t boundaryPulseAngle =
                periodValid
                    ? (uint32_t)((((int64_t)(int32_t)(oldStart + length - pulseCell)) %
                                      (int64_t)period +
                                  (int64_t)period) %
                                 (int64_t)period)
                    : 0;

            /* Whether the measure lap itself may be published - set when the
               wrap it was cut with already sits inside the gap. */
            bool publishWithShift = false;

            /* Alignment exists to park the photo model's wrap in the gap; the
               delay line has no wrap to park, and findGapOffset is another
               whole-lap bit scan on the reader thread - same stall argument as
               the correlator above. */
            if (!m_liveServe && complete && (!m_phaseAligned || m_alignVerify) &&
                !reanchorOnIndex) {
                if (m_alignAbandoned) {
                    /* Post-recovery dwell: publish as it stands, gapless-style.
                       Correlation makes the wrap self-consistent; re-running
                       the alignment that broke the chain is what must not
                       happen. */
                    m_phaseAligned = true;
                    m_trackGapless = true;
                    m_alignVerify = false;
                } else {
                    const bool verifying = m_phaseAligned; /* cache said aligned */
                    const uint32_t gap = findGapOffset(back, length);
                    if (gap != kXcNoGap) {
                        /* Is the boundary ALREADY inside the gap? Then this lap
                           is servable as it stands: its wrap sits where slip is
                           harmless. Nudge the anchor to the measured midpoint
                           anyway - it keeps the boundary centred against drift
                           and keeps the cache honest - but do not throw the lap
                           away for it. Measured on the DOSCOPY pass: the shift
                           needed by consecutive same-side tracks was 70-130
                           cells against a ~6,300-cell gap, so the "measure lap"
                           this used to discard was almost always a wasted
                           revolution - a third of the whole per-visit cost. */
                        const uint32_t g = m_gapLen.load();
                        const uint32_t inGap = (g > 2000) ? (g / 2 > 800 ? g / 2 - 800 : 0) : 0;
                        publishWithShift = (gap <= inGap) || (gap + inGap >= length);
                        /* Apply the SHORTER way round. gap and gap-length are
                           the same angle; a boundary 150 cells past the gap mid
                           (measured: side-0 tracks needed 101,854 on a 102,000
                           lap, i.e. -150) must move BACK 150, not forward a
                           whole revolution - the forward jump swallows a lap in
                           catch-up, and under reader lag it is the empty-lap
                           spiral. A backward move costs nothing: the cells for
                           the reopened angles are already in hand and the carry
                           below provides them. */
                        phaseShift = (gap > length / 2) ? gap - length /* wraps: backward */
                                                        : gap;
                        m_phaseAligned = true;
                        m_alignVerify = false;
                        if (cacheable) {
                            const uint32_t mid = (uint32_t)(
                                (((int64_t)(int32_t)(oldStart + gap - pulseCell)) %
                                     (int64_t)period +
                                 (int64_t)period) %
                                (int64_t)period);
                            m_alignCache[back->trackTag].gapPulseAngle = mid;
                            m_alignCache[back->trackTag].state = 1;
                        }
                    } else if (verifying) {
                        /* One degraded look does not outvote the cache. */
                        m_alignVerify = false;
                    } else if (m_gapSyncs.load() < 8 ||
                               ++m_phaseAlignTries >= kXcAlignGiveUp) {
                        /* No gap to find. Under eight syncs on a COMPLETE lap
                           this is a custom format, known now - not four laps
                           from now, which a protection loader's retry budget
                           does not cover. Publish as it stands, cut by
                           correlation. */
                        m_phaseAligned = true;
                        m_trackGapless = true;
                    }
                }
                if (m_phaseAligned) {
                    m_phaseAlignedFlag.store(true);
                    m_phaseShift.store(phaseShift);
                    m_phaseAlignSeq.fetch_add(1);
                }
                if (m_log)
                    liveLog("align tag=%d shift=%d pubw=%d gapless=%d tries=%u syncs=%u "
                            "pitch=%u gapLen=%u\n",
                            back->trackTag, (int32_t)phaseShift, publishWithShift ? 1 : 0,
                            m_trackGapless ? 1 : 0, m_phaseAlignTries, m_gapSyncs.load(),
                            m_gapPitch.load(), m_gapLen.load());
                /* The shift is applied by moving the chain's anchor when the
                   next lap starts - see startLap below. Nothing pulse-relative
                   is carried: the cut is chain-placed, so moving the anchor IS
                   moving every future boundary. */
            } else if (!complete && !m_phaseAligned && !reanchorOnIndex &&
                       !m_alignAbandoned && cacheable &&
                       m_alignCache[back->trackTag].state == 1) {
                /* CACHED ALIGNMENT, APPLIED AT THE FIRST CLOSE AFTER LANDING.
                   The gap's angle is a property of how the track was formatted,
                   stable for the life of the disk; re-measuring it on every
                   visit cost a revolution per visit. Anchor the boundary at the
                   cached pulse-relative angle now, so the first COMPLETE lap is
                   already cut in the gap and publishes - one revolution earlier
                   than measure-then-align. The next complete lap still runs
                   findGapOffset once (m_alignVerify) to confirm and refresh. */
                const uint32_t want = m_alignCache[back->trackTag].gapPulseAngle % period;
                const uint32_t s = (want >= boundaryPulseAngle)
                                       ? want - boundaryPulseAngle
                                       : want + period - boundaryPulseAngle;
                /* Shorter way round, as in the measured-alignment path. */
                phaseShift = (s > period / 2) ? s - period /* wraps: backward */ : s;
                m_phaseAligned = true;
                m_alignVerify = true;
                m_phaseAlignedFlag.store(true);
                m_phaseShift.store(phaseShift);
                m_phaseAlignSeq.fetch_add(1);
                if (m_log)
                    liveLog("align tag=%d shift=%d FROM CACHE (want=%u cur=%u)\n",
                            back->trackTag, (int32_t)phaseShift, want, boundaryPulseAngle);
            }

            /* Publish only once the boundary question is settled - a lap
               published before alignment has its wrap mid-sector, and that
               sector fails on every single read of the track. */
            if (complete && m_phaseAligned && (phaseShift == 0 || publishWithShift) &&
                !reanchorOnIndex) {
                back->lengthCells = length;

                if (kUseMeasuredSpeed && m_lapCells > 0) {
                    /* Normalise each slot against THIS lap's own mean cell time,
                       not against the nominal 2 us.

                       The drive's global 1.3% slowness is already expressed in
                       maxMFMBitPosition(): the guest reads 101,320 cells per
                       revolution at its own nominal bit rate, which is 202.6 ms -
                       the real figure. Returning 1013 here as well would apply
                       the same correction twice and the guest would run a
                       revolution slower than the platter. What getMFMSpeed is
                       actually for is the variation WITHIN a track, which is what
                       a long-track or variable-density protection looks like. */
                    const uint32_t meanQ10 =
                        (uint32_t)(((uint64_t)m_lapTicks << 10) / m_lapCells);
                    for (unsigned int s = 0; s < kXcSpeedSlots; s++) {
                        const uint32_t raw = back->speed[s];
                        if (raw == 0 || meanQ10 == 0) {
                            back->speed[s] = 1000;
                            continue;
                        }
                        uint32_t v = (uint32_t)(((uint64_t)raw * 1000u) / meanQ10);
                        if (v < 900)
                            v = 900;
                        if (v > 1100)
                            v = 1100;
                        back->speed[s] = (uint16_t)v;
                    }
                } else {
                    for (unsigned int s = 0; s < kXcSpeedSlots; s++)
                        back->speed[s] = 1000;
                }

                /* Hand it over as "ready". The guest picks it up when it next
                   crosses the index, never before - see getMFMBit. */
                int retired = -1;
                {
                    std::lock_guard<std::mutex> lock(m_slotLock);
                    const int previousReady = m_readyIdx;
                    m_readyIdx = m_backIdx;
                    m_readyTag.store(back->trackTag);

                    /* Find a buffer that is neither being served nor waiting. */
                    if (previousReady >= 0 && previousReady != m_frontIdx) {
                        m_backIdx = previousReady;
                    } else {
                        for (int i = 0; i < 3; i++) {
                            if (i != m_frontIdx && i != m_readyIdx) {
                                m_backIdx = i;
                                break;
                            }
                        }
                    }
                    retired = previousReady;
                }
                (void)retired;
                m_publishCount++;
                lapPublished = true;

                /* Keep the cached gap angle tracking the (slowly drifting)
                   boundary, so a revisit lands on where the boundary actually
                   is, not where it was when first measured. */
                if (cacheable && !m_trackGapless && phaseShift == 0) {
                    m_alignCache[back->trackTag].gapPulseAngle = boundaryPulseAngle;
                    m_alignCache[back->trackTag].state = 1;
                }
#if ADM_XCOPY_DIAG
                diagLog("publish tag=%d len=%u rev=%u\n", back->trackTag, length,
                        m_measuredRevCells.load());
#endif
            }
        }
    }

    /* Carry the cells captured past the cut into the new lap.

       They were written into the lap just closed, at angles >= length, but they
       are also the FIRST cells of the next revolution and nothing else will
       deliver them again - the split loop hands each run of cells to exactly one
       lap. Without this the new lap starts with a hole and can never be
       published.

       The carry starts at length + phaseShift: the new anchor moved by the
       shift, so the new lap's angle 0 is the old lap's angle length+shift. For
       a shift larger than what was captured past the cut there is nothing to
       carry and the new lap fills in once the stream crosses the new anchor. */
    const AdmXCopyLap* old = back;
    const uint32_t carryFrom = length + phaseShift;
    const uint32_t carryTo = m_haveLapStart ? m_backTo.load() : 0;

    if (length > 90000 && length < kXcMaxLapCells)
        m_lastLapLength = length;

    /* One record per revolution. Cheap enough to leave on permanently. */
    {
        const unsigned int n = m_lapLogCount.load();
        XcLapRecord& rec = m_lapLog[n % kXcLogSize];
        rec.nominal = lapNominal;
        rec.chosen = length;
        rec.score = lapScore;
        rec.from = lapFrom;
        rec.to = lapTo;
        rec.published = lapPublished ? 1u : 0u;
        m_lapLogCount.store(n + 1);
    }

    if (m_log)
        liveLog("lap tag=%d nom=%u len=%u score=%u arc=[%u,%u) pub=%d shift=%u aligned=%d "
                "holes=%u flagged=%u served f=%u b=%u p=%u g=%u\n",
                back->trackTag, lapNominal, length, lapScore, lapFrom, lapTo,
                lapPublished ? 1 : 0, phaseShift, m_phaseAligned ? 1 : 0, m_lapHoles,
                m_lapFlagged, m_servedFront, m_servedBack, m_servedPrev, m_servedGarbage);

    /* Snapshot the lap just closed so getMFMBit can serve last revolution's
       cells for any angle the new lap has not reached yet - see m_prevSnap.
       Taken for EVERY cut, published or not: the whole point is covering the
       window in which nothing is published. Must happen before startLap, while
       the closed lap's cells are still untouched (an unpublished lap keeps the
       same buffer and is overwritten as the new lap's cells arrive). */
    {
        const unsigned int sFrom = lapFrom;
        const unsigned int sTo =
            (length > 0 && length < lapTo) ? length : lapTo; /* cap at the cut */
        if (reanchorOnIndex) {
            /* The chain is being re-established from the index, so "angle x last
               lap" no longer means the same platter angle as "angle x next lap"
               and the snapshot would be served at the wrong rotation. Drop it. */
            m_prevTo.store(0);
        } else if (m_haveLapStart && back->trackTag >= 0 && sTo > sFrom &&
                   sTo <= kXcMaxLapCells) {
            m_prevTo.store(0); /* invalidate while the copy is in flight */
            const unsigned int b0 = sFrom >> 3;
            const unsigned int b1 = (sTo + 7) >> 3;
            memcpy(m_prevSnap.cells + b0, back->cells + b0, b1 - b0);
            m_prevTag.store(back->trackTag);
            m_prevFrom.store(sFrom);
            /* Normalised to [0, revolution): a backward shift is stored as its
               forward equivalent, so the serving path's single-subtract wrap
               still lands inside the snapshot. */
            {
                uint32_t ps = phaseShift;
                if (length > 90000 && length < kXcMaxLapCells) {
                    int64_t v = (int32_t)phaseShift;
                    v %= (int64_t)length;
                    if (v < 0)
                        v += length;
                    ps = (uint32_t)v;
                }
                m_prevShift.store(ps);
            }
            m_prevLen.store((sFrom == 0 && length > 90000 && length < kXcMaxLapCells) ? length
                                                                                      : 0);
            m_prevTo.store(sTo); /* last: readers gate on this */
        }
    }

    startLap(reanchorOnIndex ? pulseCell : oldStart + length + phaseShift, m_streamTag);

    if (!reanchorOnIndex && length > 0) {
        /* Never carry cells the old lap did not actually cover - the buffers
           are not cleared on reuse, so an uncovered range holds bits from an
           OLDER revolution or a different track entirely. If the covered part
           starts above the new anchor, the new lap begins with an honest hole
           (backFrom > 0) instead of a silent one. */
        uint32_t srcFrom = carryFrom;
        if (srcFrom < lapFrom)
            srcFrom = lapFrom;
        if (carryTo > srcFrom && (carryTo - srcFrom) < kXcMaxLapCells) {
            /* An unpublished lap keeps the SAME buffer, so this can be a move
               within one buffer: the source sits a revolution above the
               destination, so the ranges never overlap - checked, not
               assumed. */
            AdmXCopyLap* fresh = m_backLap.load(std::memory_order_relaxed);
            const uint32_t dstFrom = srcFrom - carryFrom;
            const uint32_t carry = carryTo - srcFrom;
            if (fresh && srcFrom >= dstFrom + carry && dstFrom + carry < kXcMaxLapCells) {
                for (uint32_t i = 0; i < carry; i++)
                    fresh->setBit(dstFrom + i, old->bit(srcFrom + i));
                if (dstFrom > 0)
                    m_backFrom = dstFrom;
                m_backTo = dstFrom + carry;
            }
        }
    }

    /* After a re-anchor the stream may already be past the new (pulse) anchor -
       the empty-lap recovery fires precisely when the reader is lagging - so the
       first cells written land at a positive angle. Restart the covered arc at
       that angle rather than letting backFrom claim [0, angle) it never saw:
       the lap after this one is the first publishable one, built from the carry
       above, and it must not inherit a silent hole. */
    if (reanchorOnIndex)
        m_restartCoverage = true;
}

/* ------------------------------------------------------------------------- */
/* XCopySink                                                                  */
/* ------------------------------------------------------------------------- */

void AdmXCopyBridge::onCells(const XclRecordHeader& h, const uint8_t* packed, uint32_t bytes)
{
    (void)bytes;

    /* Cells captured before the head arrived belong to the surface we just left.
       The device marks the boundary exactly, so this is a comparison, not a
       guess. Signed difference: the counter is free-running and wraps. */
    if (m_haveSkip && (int32_t)(h.index - m_skipBeforeCell) < 0) {
        /* Off the surface we just left. Disarm the contiguity check rather than
           advancing it: the span across a head move is not lost data, it is data
           that was never worth sending, and counting it made a link with zero
           resyncs, zero CRC errors and zero device overruns report ~300,000
           "lost" cells across six seeks. */
        m_haveExpected = false;
        return;
    }
    m_haveSkip = false;

    /* Contiguity accounting happens even for cells we are about to throw away,
       so the counter stays the sole authority on where we are. */
    if (m_haveExpected && !m_seekInFlight.load() && h.index != m_expectedCell) {
        const uint32_t missing = h.index - m_expectedCell;
        if (missing != 0 && missing < kXcMaxLapCells) {
            m_cellsLost += missing;
            m_restartCoverage = true;
        }
    }
    m_expectedCell = h.index + h.count;
    m_haveExpected = true;
    m_lastCellIndex.store(h.index + h.count);

    /* The two "not to be trusted" flags mean very different things, and treating
       them alike was a bug worth spelling out.

       XCL_RF_TRACK_UNSTABLE is the head in flight. Those cells are not a poor
       reading of this track, they are not this track at all, and a spurious
       0x4489 in them would have the guest decode a sector out of noise. Drop
       them; the guest gets ADM_XC_GARBAGE_BIT, a run of ones with no sync word,
       which is what a real drive gives you off an unsettled head.

       XCL_RF_PLL_UNLOCKED is a wobble in an otherwise real read of the right
       track. Dropping those is STRICTLY WORSE than keeping them, because the
       covered arc is contiguous: a hole anywhere in the lap moves m_backFrom off
       zero, the lap can never be "complete", nothing is ever promoted, and the
       guest is served a partial back-lap indefinitely.

       Measured, reading the same track 20 times over: cylinder 0 gave 20/20 laps
       with all 11 sectors, cylinder 40 gave 2-4/20 with the rest partial (worst
       3 of 11) - the track number correct every time, so not a head-position
       fault, just holes. Cylinder 40 is a shorter track at the same data rate, so
       the PLL wobbles more there. That is the outer half of a disk becoming
       unreadable, and the root block lives at cylinder 40: the bootblock loads
       off cylinder 0 and then AmigaDOS reports "Not a DOS disk in unit 0".

       What a real drive does is hand Paula the bits it got. A wobble costs one
       sector its checksum and the guest re-reads it. It does not punch a hole in
       the track. So keep the cells, count them, and let the checksum decide. */
    if (h.flags & XCL_RF_TRACK_UNSTABLE) {
        m_restartCoverage = true;
        m_unstableRecords++;
        m_lapFlagged++;
        return;
    }
    if (h.flags & XCL_RF_PLL_UNLOCKED) {
        m_unstableRecords++;
        m_lapFlagged++;
    }

    /* The device's own marker for "data before this was dropped". It has already
       advanced the cell counter across the hole by the time that actually
       elapsed, so the angular mapping on this side of it is correct - trust this
       record's index rather than anything accumulated. */
    if (h.flags & XCL_RF_DISCONTINUITY)
        m_restartCoverage = true;

    /* Live mode's serving source. Fed before the lap walk so serving does not
       depend on any lap bookkeeping; the laps keep running below for length
       damping, m_liveSpeed and the photo machinery. */
    if (m_liveServe)
        writeDelayLine(h.index, packed, h.count);

    /* A record can straddle the revolution boundary, and its two halves belong
       to different laps. Walk it in runs, closing the pending lap when we reach
       the cut point. */
    uint32_t cellStart = h.index;
    uint32_t srcOffset = 0;
    uint32_t remaining = h.count;

    while (remaining > 0) {
        /* THE CUT GOES WHERE THE CHAIN PREDICTS IT, NOT WHERE THE PULSE FIRED.
           The pulse jitters ~135 cells and, worse, any offset between pulse and
           boundary changes at every track change - placing the cut relative to
           the pulse made the cut and the chain disagree by thousands of cells
           after a seek, coverage never reached `length`, and nothing was ever
           complete again. The pulse's job is only to say "a revolution ended
           near here". Close only once kJoinMargin cells PAST the cut have been
           captured - the correlator needs them, and they are carried into the
           next lap. */
        uint32_t cutCell = m_pendingIndexCell;
        if (m_haveLapStart && m_lastLapLength > 90000 && m_lastLapLength < kXcMaxLapCells) {
            const uint32_t predicted = m_lapStartCell + m_lastLapLength;
            const int32_t lead = (int32_t)(predicted - m_pendingIndexCell);
            /* Sanity: the chain's boundary should sit within one revolution
               after the pulse. Anything else means the chain is broken - fall
               back to cutting at the pulse, which re-anchors. */
            if (lead >= 0 && lead < (int32_t)kXcMaxLapCells)
                cutCell = predicted;
        }

        if (m_havePendingIndex && (int32_t)(cellStart - (cutCell + kJoinMargin)) >= 0) {
            closeLapAt(cutCell, m_pendingIndexCell);
            m_havePendingIndex = false;
        }

        uint32_t chunk = remaining;
        if (m_havePendingIndex) {
            const uint32_t untilClose = (cutCell + kJoinMargin) - cellStart;
            if (untilClose > 0 && untilClose < chunk)
                chunk = untilClose;
        }

        writeCells(h, packed, cellStart, srcOffset, chunk);
        cellStart += chunk;
        srcOffset += chunk;
        remaining -= chunk;
    }
}

void AdmXCopyBridge::writeCells(const XclRecordHeader& h, const uint8_t* packed,
                                uint32_t cellStart, uint32_t srcOffset, uint32_t count)
{
    if (!m_haveLapStart || count == 0)
        return; /* no anchor yet - we do not know what angle these cells are at */

    uint32_t angle = cellStart - m_lapStartCell;
    if (angle >= kXcMaxLapCells) {
        /* Two very different reasons land here, and only one is a discard.
           A NEGATIVE angle (huge as unsigned) is a record from before the
           anchor - normal while the stream catches up to a forward-moved
           anchor. If the record ends before the anchor, drop it. But a record
           STRADDLING the anchor carries the lap's very first cells, and
           dropping it whole left a permanent [0, ~1000) hole: the lap could
           never be complete, so nothing published for the whole dwell -
           measured as sweep tracks that never read cleanly. Split it at the
           anchor instead. */
        const int32_t neg = (int32_t)angle;
        if (neg < 0 && (int64_t)neg + (int64_t)count > 0) {
            const uint32_t skip = (uint32_t)(-neg);
            srcOffset += skip;
            count -= skip;
            angle = 0;
        } else {
            return; /* wholly before the anchor, or longer than any revolution */
        }
    }
    if (angle + count > kXcMaxLapCells)
        count = kXcMaxLapCells - angle;

    AdmXCopyLap* back = m_backLap.load(std::memory_order_relaxed);
    if (!back)
        return;

    /* The covered arc is a single CONTIGUOUS range, so a hole cannot be papered
       over by extending the end of it - that would hand the guest whatever the
       previous lap left in those bytes as though it were this one's. Restart the
       range here instead. closeLapAt requires from == 0, so the lap is no longer
       publishable, but everything after the hole is still served live. */
    if (m_restartCoverage) {
        m_backFrom = angle;
        m_backTo = angle;
        m_restartCoverage = false;
        m_lapHoles++;
    }

    /* The covered arc is contiguous BY CONSTRUCTION: records arrive in order
       and angles are monotone within a lap, so a write starting above the
       current watermark means cells in between were dropped - an anchor that
       moved (a phase shift, a re-anchor) or the kXcMaxLapCells cap on an
       overlong lap. Whatever the cause, [watermark, angle) was never written;
       claiming it would serve stale bits, possibly from another track. */
    {
        const unsigned int curTo = m_backTo.load();
        if (angle > curTo) {
            m_backFrom = angle;
            m_backTo = angle;
            m_lapHoles++;
        }
    }

    /* Unpack MSB first, matching the wire format. */
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t b = srcOffset + i;
        const bool bit = (packed[b >> 3] >> (7 - (b & 7))) & 1;
        back->setBit(angle + i, bit);
    }

    /* Per-slot timing, stored raw for now; closeLapAt normalises it. The rate is
       per cell, so a split record apportions naturally. */
    if (kUseMeasuredSpeed && h.count > 0 && h.ticks > 0) {
        const uint32_t perCellQ10 = ((uint32_t)h.ticks << 10) / h.count;
        const unsigned int first = angle / kXcSpeedGroup;
        const unsigned int last = (angle + count - 1) / kXcSpeedGroup;
        for (unsigned int s = first; s <= last && s < kXcSpeedSlots; s++)
            back->speed[s] = (uint16_t)(perCellQ10 > 0xFFFF ? 0xFFFF : perCellQ10);
        m_lapTicks += (uint64_t)h.ticks * count / h.count;
        m_lapCells += count;
    }

    const unsigned int end = angle + count;
    if (end > m_backTo.load())
        m_backTo = end;
}

/* Reader thread. The whole write path of the delay line: unpack MSB-first and
   lay the cells down at their absolute index mod the frozen modulus. No anchor,
   no arc, no holes to track - a skipped span (overrun, unstable records) simply
   leaves older bits in place, and the validity comparison in delayLineBit()
   decides whether they may be served. */
void AdmXCopyBridge::writeDelayLine(uint32_t cellIndex, const uint8_t* packed, uint32_t count)
{
    const unsigned int len = m_delayLen.load(std::memory_order_relaxed);
    if (!len)
        return;

    unsigned int a = cellIndex % len;
    for (uint32_t i = 0; i < count; i++) {
        const bool bit = (packed[i >> 3] >> (7 - (i & 7))) & 1;
        const unsigned int byte = a >> 3;
        const uint8_t mask = (uint8_t)(0x80u >> (a & 7));
        if (bit)
            m_delayBits[byte] |= mask;
        else
            m_delayBits[byte] &= (uint8_t)~mask;
        if (++a == len)
            a = 0;
    }

    /* Release: the guest's acquire of W is what orders the bit stores above
       before anything it reads behind the cursor. */
    m_delayW.store(cellIndex + count, std::memory_order_release);
}

void AdmXCopyBridge::onFlux(const XclRecordHeader& h, const uint16_t* ticks, uint32_t count)
{
    /* Flux mode exists for imaging and protection analysis, and as a fallback to
       the host PLL. The live path runs in XCL_MODE_MFM and never asks for it, so
       nothing here consumes it yet. */
    (void)h;
    (void)ticks;
    (void)count;
}

void AdmXCopyBridge::onEvent(const XclRecordHeader& h, const XclEventPayload& ev)
{
    (void)h;

    switch (ev.event) {
    case XCL_EV_INDEX:
        /* Seeing index pulses is proof of media, and the only proof available
           without stopping to probe. Never the other way round: this flag is not
           cleared here, because UAE will not spin the motor without media and a
           pessimistic answer deadlocks the drive (docs/floppy.md 12g). */
        m_lastIndexMs = nowMs();
        m_diskInDrive = true;
        /* The platter's revolution, measured pulse to pulse. Range-checked at
           the point of use, so a counter reset (stream restart) or a missed
           pulse produces one out-of-range value that is simply not believed. */
        if (m_haveLastIndexEventCell)
            m_indexPeriodCells = ev.cellIndex - m_lastIndexEventCell;
        m_lastIndexEventCell = ev.cellIndex;
        m_haveLastIndexEventCell = true;
        /* Deferred on purpose - see m_pendingIndexCell. The cells at the end of
           this lap have not arrived yet. */
        m_pendingIndexCell = ev.cellIndex;
        m_havePendingIndex = true;
        break;

    case XCL_EV_TRACK_CHANGE: {
        if (m_log)
            liveLog("track_change cyl=%u side=%u cell=%u devTicks=%u\n", (unsigned)ev.cyl,
                    (unsigned)ev.side, ev.cellIndex, ev.arg2);
        m_hwCylinder = (int)ev.cyl;
        m_hwSide = ev.side != 0;
        m_seekLandCell.store(ev.cellIndex);
        m_seekDeviceTicks.store(ev.arg2);
        m_seekLandMs.store(nowMs());
        m_seekTraceSeq.fetch_add(1);
        m_streamTag = (int)ev.cyl * 2 + (ev.side ? 1 : 0);
        m_skipBeforeCell = ev.cellIndex;
        m_haveSkip = true;
        m_seekInFlight = false;
        m_phaseAligned = false; /* different surface, different gap position */
        m_phaseAlignTries = 0;
        m_trackGapless = false;
        m_alignAbandoned = false;
        m_alignVerify = false;
        m_phaseAlignedFlag.store(false);
        m_generation++;

        /* Delay line: everything in the buffer is the old surface from here.
           The modulus is frozen NOW, for the whole dwell - the best measured
           revolution if one exists, else whatever the guest is already
           wrapping at - so the mapping cannot shift under a populated
           buffer. */
        if (m_liveServe) {
            unsigned int dl = m_measuredRevCells.load();
            if (dl <= 90000 || dl >= kXcMaxLapCells)
                dl = m_trackLenCells.load();
            m_delayLen.store(dl);
            m_delayValidFrom.store(ev.cellIndex);
        }

        abandonLap();

        /* Does the anchor survive the head move?

           The index is a mark on the PLATTER, not on a track, so it keeps firing
           while the head moves. If the device's cell counter also keeps advancing
           at the platter's rate through the seek - which is what
           XCL_CAP_SEEK_PHASE promises - then the angle of every cell after the
           move is still (cellIndex - m_lapStartCell), and we can serve data off
           the new surface the instant it arrives. That is the difference between
           ~20 ms and ~200 ms to first data, and it is the single biggest number
           in this driver.

           Without that guarantee the anchor is worthless after a move and we have
           to wait for the next index, paying up to a full revolution on every
           seek - which is most of what makes driver 3 slow. */
        AdmXCopyLap* back = &m_lap[m_backIdx];
        back->reset(m_streamTag);
        m_backTag.store(m_streamTag);
        m_backLap.store(back);
        m_lapTicks = 0;
        m_lapCells = 0;

        if (m_link.hello().capabilities & XCL_CAP_SEEK_PHASE) {
            /* Anchor retained. The new lap starts mid-revolution, so it is served
               live but will not be published - only the lap after it can be
               complete.

               A LONG seek leaves the anchor several revolutions behind the
               landing point, and dropping it cost the whole point of
               XCL_CAP_SEEK_PHASE: nothing was served until the next pulse
               re-anchored, measured under the emulator as visits opening with
               two or three zero-sector attempts. Roll the chain forward by
               whole laps instead - dead reckoning at ~±40 cells per lap of
               real spindle wobble, absorbed by the damped period tracking and
               tiny against the ~6,300-cell gap the alignment cache aims at. */
            uint32_t angle = ev.cellIndex - m_lapStartCell;
            if (angle >= kXcMaxLapCells && angle < 0x80000000u && m_lastLapLength > 90000 &&
                m_lastLapLength < kXcMaxLapCells) {
                const uint32_t laps = angle / m_lastLapLength;
                m_lapStartCell += laps * m_lastLapLength;
                angle -= laps * m_lastLapLength;
            }
            if (angle < kXcMaxLapCells) {
                m_backFrom = angle;
                m_backTo = angle;
            } else {
                m_haveLapStart = false;
            }
        } else {
            m_haveLapStart = false;
            m_backFrom = 0;
            m_backTo = 0;
        }
        break;
    }

    case XCL_EV_SEEK_BEGIN:
        m_seekBeginMs.store(nowMs());
        /* The head is about to move, so the cell counter is about to advance
           across a span no record will cover. Disarm the contiguity check now -
           XCL_EV_TRACK_CHANGE arrives on the far side of that span, which is too
           late to stop the gap being counted. */
        m_haveExpected = false;
        m_restartCoverage = true;
        break;

    case XCL_EV_OVERRUN:
        m_overruns++;
        /* Do NOT reset the anchor or the expected counter. The device advances
           the cell counter across the hole by the time that actually elapsed, so
           the angular mapping on the far side is still correct and the lap start
           is still valid - only the covered arc has to restart, which the next
           record does when it arrives flagged XCL_RF_DISCONTINUITY.

           Calling abandonLap() here was a bug: it set the covered range back to
           [0, 0), and the next record then extended it to [0, angle+count) -
           claiming coverage of the hole it had just been told about. */
        m_restartCoverage = true;
        break;

    case XCL_EV_DISK_CHANGE:
        /* Either direction is a different disk (or the same disk reinserted at
           an unknown state): the per-track gap angles belong to the old one. */
        memset(m_alignCache, 0, sizeof(m_alignCache));
        /* And so does everything in the delay line. */
        if (m_liveServe)
            m_delayValidFrom.store(m_lastCellIndex.load());
        if (ev.arg == 0) {
            /* The one place media presence is allowed to go false: the device
               said so explicitly. */
            m_diskInDrive = false;
            m_generation++;
            abandonLap();
            {
                std::lock_guard<std::mutex> lock(m_slotLock);
                m_readyIdx = -1;
                m_readyTag.store(-1);
            }
        } else {
            m_diskInDrive = true;
        }
        break;

    case XCL_EV_ERROR:
        m_lastError = "device reported error " + std::to_string((unsigned)ev.arg);
        break;

    default:
        break;
    }
}

void AdmXCopyBridge::onStatus(const XclStatus& st)
{
    {
        const uint64_t req = m_statusReqMs.exchange(0);
        if (req)
            m_statusRttMs.store((unsigned int)(nowMs() - req));
    }
    m_hwCylinder = (int)st.cyl;
    m_hwSide = st.side != 0;

    /* Media presence. Two sources, and they are NOT equally trustworthy.

       XCL_ST_MEDIA is the device's read of the drive's disk-change line, and on a
       PC drive that line latches when a disk is removed and is only cleared by a
       step pulse. So a freshly inserted disk reads as "no media" until something
       moves the head: believing it in the "present" direction would report an
       empty drive with a disk sitting in it, and it would fight the index events
       every 500 ms.

       An index pulse is unambiguous - only a turning disk produces one - and the
       live session holds the motor on for its whole duration, so pulses arrive
       about five times a second whenever media is there. So: a recent index wins
       outright, and the disk-change line is consulted only in its absence, where
       it is reliable.

       This still satisfies docs/floppy.md 12g. That rule forbids clearing
       presence on a PESSIMISTIC check, because UAE will not spin the motor
       without media and the drive then deadlocks. Here the motor is already
       running, absence is a positive finding rather than a guess, and STATUS is
       polled forever - so a disk inserted later is picked up within a revolution
       and the drive recovers on its own. */
    m_lastStatusBits = (unsigned int)st.status;

    const uint64_t sinceIndex = nowMs() - m_lastIndexMs.load();
    if (sinceIndex < 1000)
        m_diskInDrive = true;
    else
        m_diskInDrive = (st.status & XCL_ST_MEDIA) != 0;
    if (st.lastRevolutionCells > 90000 && st.lastRevolutionCells < kXcMaxLapCells)
        m_measuredRevCells = st.lastRevolutionCells;
}

void AdmXCopyBridge::onAck(const XclAck& ack, bool isNak)
{
    /* Stamped before the early-out: an accepted seek is exactly the case that
       never reaches the code below, and its timing is the measurement. */
    if (ack.command == XCL_CMD_SEEK)
        m_seekAckMs.store(nowMs());

    if (!isNak || ack.result == XCL_RESULT_UNSUPPORTED)
        return;

    /* A refused SEEK is not a message, it is a head that did not move. Re-issue
       it rather than reporting it - the guest is entitled to assume the position
       it asked for is the position it gets. */
    if (ack.command == XCL_CMD_SEEK && ack.result == XCL_RESULT_BUSY) {
        m_seekRefused = true;
        return;
    }

    m_lastError = "device refused command " + std::to_string((unsigned)ack.command) + " (" +
                  std::to_string((unsigned)ack.result) + ")";
}

void AdmXCopyBridge::onTest(const XclRecordHeader& h, const uint8_t* data, uint32_t bytes)
{
    (void)h;
    (void)data;
    (void)bytes;
}

/* ------------------------------------------------------------------------- */
/* Drive control - emulation thread                                           */
/* ------------------------------------------------------------------------- */

bool AdmXCopyBridge::resetDrive(int trackNumber)
{
    /* Head position only. The motor is left alone: the guest drives it through
       setMotorStatus(), and forcing it off here left a reset-to-boot with the
       drive stopped and no media reported. */
    m_wantCylinder = trackNumber;
    m_generation++;
    return true;
}

void AdmXCopyBridge::gotoCylinder(int cylinderNumber, bool side)
{
    if (cylinderNumber < 0)
        cylinderNumber = 0;
    if (cylinderNumber >= getMaxCylinder())
        cylinderNumber = getMaxCylinder() - 1;

    if (cylinderNumber != m_wantCylinder.load() || side != m_wantSide.load()) {
        /* Where the host had got to in the stream when the move was asked for.
           See debugSeekTrace(). */
        m_seekReqCell.store(m_lastCellIndex.load());
        m_seekReqMs.store(nowMs());
    }
    m_wantCylinder.store(cylinderNumber);
    m_wantSide.store(side);
    /* Deliberately no invalidation. The tag check in getMFMBit is what keeps the
       guest from being served the wrong surface, and the reader thread will issue
       the seek on its next pass. */
}

void AdmXCopyBridge::setSurface(bool side)
{
    m_wantSide.store(side);
}

void AdmXCopyBridge::handleNoClickStep(bool side)
{
    /* A probe for disk presence that must not move the head audibly. The device
       has no equivalent, and answering it by actually stepping would be worse
       than ignoring it: the guest only uses the result to decide whether to show
       a click. */
    (void)side;
}

void AdmXCopyBridge::setMotorStatus(bool side, bool turnOn)
{
    setSurface(side);

    /* Report the motor as running the instant the guest asks, exactly as
       CommonBridgeTemplate does. disk.c's drive_running() is isMotorRunning() and
       gates the entire disk path; reporting the hardware's real state here left
       drive_running() false through spin-up and the guest never got past the
       bootblock. */
    if (m_motorRunning.exchange(turnOn) != turnOn) {
        m_wantMotor = turnOn;
        if (!turnOn)
            m_motorReady = false;
    }
}

bool AdmXCopyBridge::isReady()
{
    return m_motorReady.load() && m_diskInDrive.load() && !m_ioError.load();
}

/* ------------------------------------------------------------------------- */
/* Reading - emulation thread, once per emulated bit cell                     */
/* ------------------------------------------------------------------------- */

bool AdmXCopyBridge::isMFMPositionAtIndex(int mfmPositionBits)
{
    const unsigned int len = m_trackLenCells.load();
    if (!len || mfmPositionBits < 0)
        return false;
    /* Position 0 really is the index here: laps are cut at the index event, not
       at an inferred join. */
    return ((unsigned int)mfmPositionBits % len) == 0;
}

bool AdmXCopyBridge::isMFMDataAvailable()
{
    const int want = wantedTrackKey();
    if (m_frontLap.load() && m_frontTag.load() == want)
        return true;
    if (m_prevTag.load() == want && m_prevTo.load() > m_prevFrom.load())
        return true;
    return m_backTag.load() == want && m_backTo.load() > m_backFrom.load();
}

bool AdmXCopyBridge::getMFMBit(const int mfmPositionBits)
{
    if (m_directMode || mfmPositionBits < 0)
        return false;

    /* Live mode: the continuous delay line is the ONLY source - no photos, no
       promotes, no back/prev mixes, and none of their seams. See the header. */
    if (m_liveServe)
        return delayLineBit(mfmPositionBits);

    const unsigned int len = m_trackLenCells.load();
    if (!len)
        return ADM_XC_GARBAGE_BIT;
    const unsigned int pos = ((unsigned int)mfmPositionBits) % len;
    const int want = wantedTrackKey();

    /* m_lockOffset is only ever non-zero in live mode, which returned above,
       so the photo path's chain angle is the position itself. */
    const unsigned int a = (pos + m_lockOffset) % len;

    /* Take a newly completed lap only as the guest crosses the index.

       Anywhere else is a seam: two revolutions of the same track do not line up
       bit for bit, because the drive's speed varies. At the index they do - it is
       the one point where the end of a revolution really is adjacent to the start
       of one.

       EXCEPT when nothing consistent is being served. After a head move the
       front is absent or belongs to another track, and the guest reads the
       back/prev mix - which always carries one seam sector where the source
       switches. The first published lap then sat in `ready` until the guest
       crossed position 0, and the guest crosses it milliseconds BEFORE the
       publish lands, every time: one whole revolution of 10-of-11 reads, per
       visit. Measured under the emulator as the dominant visit shape
       (partial -> 11/10 -> 11/11), the one revolution separating this driver's
       3.6 attempts per visit from the buffered Greaseweazle's 1.35. Switching
       mid-revolution costs at most the sector spanning the switch - no worse
       than the seam already being paid - so when the front is stale, take the
       ready lap the moment it exists.

       The wrap MUST be detected by coverage. UAE's DMA prediction reads ahead and
       jumps the position around roughly 775 times per revolution, so
       "pos < previous pos" fires constantly and is not a revolution boundary. */
    if (pos > m_guestMaxPos)
        m_guestMaxPos = pos;
    /* Lock-free pre-check: the mutex is touched only when a promote can
       actually happen, never per bit in the steady state. */
    const bool frontStale =
        (!m_frontLap.load(std::memory_order_relaxed) ||
         m_frontTag.load(std::memory_order_relaxed) != want) &&
        m_readyTag.load(std::memory_order_relaxed) == want;
    /* Promotes are gated on the CHAIN angle: a = 0 is where the lap boundary
       sits, which alignment parked in the inter-sector gap, so a swap there
       cannot land mid-sector whatever the guest's own (offset) position is.
       The stale-front case skips the coverage requirement - a fresh publish
       should be adopted at the first safe crossing, not a revolution later -
       and gets a wider window because live serving is already carrying the
       guest and one crossing must not be missed for a marginal count. */
    const bool promoteWindow =
        frontStale ? (!m_liveServe || a < 2048 || a + 2048 >= len)
                   : (a < 512 && m_guestMaxPos * 100u >= len * 90u);
    if (promoteWindow) {
        std::lock_guard<std::mutex> lock(m_slotLock);
        if (m_readyIdx >= 0 && m_lap[m_readyIdx].trackTag == want) {
            m_guestMaxPos = 0;
            /* WHERE the swap happened, in chain angle. If this is not near 0,
               the swap did not land in the gap. */
            const unsigned int pn = m_promoteLogCount.load();
            m_promoteLog[pn % kXcLogSize] = a;
            m_promoteLogCount.store(pn + 1);
            const int retiring = m_frontIdx;
            m_frontIdx = m_readyIdx;
            m_readyIdx = -1;
            m_readyTag.store(-1);
            AdmXCopyLap* f = &m_lap[m_frontIdx];
            m_frontLap.store(f);
            m_frontTag.store(f->trackTag);
            if (f->lengthCells > 90000 && f->lengthCells < kXcMaxLapCells)
                m_trackLenCells = f->lengthCells;
            if (m_log)
                liveLog("promote pos=%u tag=%d len=%u\n", pos, f->trackTag, f->lengthCells);
            /* The retired buffer becomes available again; the reader thread picks
               it up the next time it needs one. */
            (void)retiring;
        }
    }

    if (!m_diskInDrive.load() || !m_motorReady.load())
        return ADM_XC_GARBAGE_BIT;

    /* All sources are indexed by the CHAIN angle `a`, so front, back and prev
       agree on what any position means and switching between them never
       remaps the stream. */

    /* 1. The steady state: a complete lap off the surface the guest is on. */
    AdmXCopyLap* f = m_frontLap.load(std::memory_order_relaxed);
    if (f && m_frontTag.load(std::memory_order_relaxed) == want && a < f->lengthCells) {
        m_servedFront++;
        return f->bit(a);
    }

    /* 2. The lap still being written - with the phase lock, the guest's angle
          sits kXcPhaseLead cells behind the watermark, so this is the normal
          serving path from the moment the head lands until a photo exists:
          the guest reads the bits that just came off the platter, exactly as
          a real drive's read line would deliver them. */
    if (m_backTag.load(std::memory_order_relaxed) == want) {
        AdmXCopyLap* b = m_backLap.load(std::memory_order_relaxed);
        const unsigned int from = m_backFrom.load(std::memory_order_relaxed);
        const unsigned int to = m_backTo.load(std::memory_order_relaxed);
        if (b && a >= from && a < to) {
            m_servedBack++;
            return b->bit(a);
        }
    }

    /* 3. The previous revolution, for angles the writer has not reached this
          lap - drift, UAE's DMA read-ahead, or the tail of the lap the lock's
          lead reaches into. Same platter angle - the anchor chains - one
          revolution stale, which for a stationary head is the same data. */
    if (m_prevTag.load(std::memory_order_relaxed) == want) {
        const unsigned int pTo = m_prevTo.load(std::memory_order_relaxed);
        unsigned int p = a + m_prevShift.load(std::memory_order_relaxed);
        /* Past the snapshot's end: the same platter angle sits one revolution
           earlier in the snapshot's own timeline. Only meaningful when the
           snapshot is a complete revolution (prevLen != 0). */
        const unsigned int pLen = m_prevLen.load(std::memory_order_relaxed);
        if (pLen && p >= pLen)
            p -= pLen;
        if (p >= m_prevFrom.load(std::memory_order_relaxed) && p < pTo) {
            m_servedPrev++;
            return m_prevSnap.bit(p);
        }
    }

    /* 4. Nothing here yet. A run of ones is what a real drive gives you off an
          unsettled head, and it contains no sync word, so the guest keeps
          looking rather than decoding rubbish. */
    m_servedGarbage++;
    return ADM_XC_GARBAGE_BIT;
}

/* Emulation thread. The whole read path of the delay line.

   Stateless per bit: position -> angle -> cells-behind-cursor -> validity ->
   bit. UAE's DMA prediction jumps the position around ~775 times a revolution
   and none of it matters here, because nothing is inferred from the previous
   call. The one per-dwell state is the lock: taken when a generation first has
   m_phaseLead cells of fresh surface, placing the guest that far behind the
   write cursor, where both advance at the platter's rate (getMFMSpeed slaves
   the guest's bit clock) and the gap only wobbles, never grows. */
bool AdmXCopyBridge::delayLineBit(const int mfmPositionBits)
{
    const unsigned int len = m_delayLen.load(std::memory_order_relaxed);
    if (!len || !m_diskInDrive.load() || !m_motorReady.load()) {
        m_servedGarbage++;
        return ADM_XC_GARBAGE_BIT;
    }

    const unsigned int pos = ((unsigned int)mfmPositionBits) % len;
    uint32_t W = m_delayW.load(std::memory_order_acquire);
    const uint32_t validFrom = m_delayValidFrom.load(std::memory_order_relaxed);

    const unsigned int gen = m_generation.load(std::memory_order_relaxed);
    if (gen != m_lockGen) {
        /* Lock only once a full lead of the NEW surface exists; until then the
           cursor may still be parked on old-surface indices and an offset from
           it would trail something meaningless. Unlocked reads fall through to
           the validity test, which fails them all - garbage, exactly what an
           unsettled head serves. */
        if ((int32_t)(W - (validFrom + m_phaseLead)) >= 0) {
            const unsigned int target = (uint32_t)(W - m_phaseLead) % len;
            m_lockOffset = (target + len - pos) % len;
            m_lockGen = gen;
            if (m_log)
                liveLog("dlock gen=%u off=%u W=%u valid=%u pos=%u\n", gen, m_lockOffset, W,
                        validFrom, pos);
        }
    }

    const unsigned int a = (pos + m_lockOffset) % len;

    /* How many cells behind the write cursor this angle sits; the bit here was
       written at absolute index W - d. In the steady state d is about the
       lead; ahead-of-cursor reads (UAE's prediction overshooting, drift after
       a pause) wrap to large d and resolve to the PREVIOUS revolution of the
       same track - real data, one revolution stale, which for a stationary
       head is what a real platter would have under those angles too. */
    unsigned int wa = W % len;
    unsigned int d = (wa >= a) ? (wa - a) : (wa + len - a);

    /* THE DYNAMIC STALL (see the header). The emulation thread's frame burst
       is about to run the guest over the write cursor; wait for the platter
       instead of serving it the previous revolution's bits. Only when locked
       (an unlocked guest reads garbage by design and WORDSYNC waits it out),
       never during a seek (the writer is silent across one by design and the
       guest is on a surface it is about to leave), and bounded: a timeout
       gives up for the rest of the generation, so a dead device costs one
       timeout and then behaves exactly like core 1.67.0. */
    if (m_stallEnable && gen == m_lockGen && d < m_stallMargin && gen != m_stallGiveUpGen &&
        !m_seekInFlight.load(std::memory_order_relaxed)) {
        m_stallCount++;
        unsigned int slept = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            m_stallSleeps++;
            if (++slept >= kXcStallTimeoutMs) {
                m_stallGiveUpGen = gen;
                break;
            }
            W = m_delayW.load(std::memory_order_acquire);
            wa = W % len;
            d = (wa >= a) ? (wa - a) : (wa + len - a);
            /* Resume above margin + hysteresis so the next ~2 ms of guest
               progress cannot immediately re-enter the stall; d >= half the
               revolution means the read is behind the cursor by design (a
               prediction overshoot wrapped), not approaching it. */
            if (d >= m_stallMargin + kXcStallHysteresis || d >= len / 2)
                break;
        }
    }

    if ((int32_t)((W - d) - validFrom) < 0) {
        m_servedGarbage++;
        return ADM_XC_GARBAGE_BIT; /* not captured on this surface yet */
    }

    m_servedFront++;
    return (m_delayBits[a >> 3] >> (7 - (a & 7))) & 1;
}

int AdmXCopyBridge::getMFMSpeed(const int mfmPositionBits)
{
    if (!kUseMeasuredSpeed || mfmPositionBits < 0)
        return 1000;

    /* The delay line has no per-slot speeds and needs none: the platter's
       measured mean cell rate is what slaves the guest to the spindle, and it
       is the same number at every angle. */
    if (m_liveServe)
        return (int)m_liveSpeed.load(std::memory_order_relaxed);

    /* Only the front lap has normalised slots - see closeLap(). The in-progress
       lap still holds raw device ticks, which are meaningless here. */
    AdmXCopyLap* f = m_frontLap.load(std::memory_order_relaxed);
    if (!f || m_frontTag.load(std::memory_order_relaxed) != wantedTrackKey())
        return 1000;

    const unsigned int len = m_trackLenCells.load();
    if (!len)
        return 1000;
    /* Same chain-angle mapping as getMFMBit, same lock. */
    const unsigned int pos = ((unsigned int)mfmPositionBits) % len;
    const unsigned int a = (pos + m_lockOffset) % len;
    const unsigned int slot = a / kXcSpeedGroup;
    if (slot >= kXcSpeedSlots)
        return 1000;

    const uint16_t v = f->speed[slot];
    return v ? (int)v : 1000;
}

void AdmXCopyBridge::mfmSwitchBuffer(bool side)
{
    /* Nothing to switch here - the promote happens in getMFMBit, at the index,
       where it is safe. UAE calls this at its own revolution boundary and the
       surface may have changed with it. */
    setSurface(side);
}

int AdmXCopyBridge::getMFMTrack(bool side, unsigned int track, bool resyncRotation,
                                const int bufferSizeInBytes, void* output)
{
    /* The imaging path. Serve the current complete lap if it happens to be the
       track asked for; this driver never seeks speculatively. */
    (void)resyncRotation;
    if (!output || bufferSizeInBytes <= 0)
        return 0;

    const int wantTag = (int)track * 2 + (side ? 1 : 0);
    AdmXCopyLap* f = m_frontLap.load();
    if (!f || m_frontTag.load() != wantTag || f->lengthCells == 0)
        return 0;

    unsigned int bytes = (f->lengthCells + 7) / 8;
    if (bytes > (unsigned int)bufferSizeInBytes)
        bytes = (unsigned int)bufferSizeInBytes;
    memcpy(output, f->cells, bytes);
    return (int)(bytes * 8);
}

bool AdmXCopyBridge::setDirectMode(bool directModeEnable)
{
    m_directMode = directModeEnable;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Writing - refused                                                          */
/* ------------------------------------------------------------------------- */

bool AdmXCopyBridge::writeMFMTrackToBuffer(bool side, unsigned int track, bool writeFromIndex,
                                           int sizeInBytes, void* mfmData)
{
    (void)side;
    (void)track;
    (void)writeFromIndex;
    (void)sizeInBytes;
    (void)mfmData;
    return false;
}

void AdmXCopyBridge::writeShortToBuffer(bool side, unsigned int track, unsigned short mfmData,
                                        int mfmPosition)
{
    (void)side;
    (void)track;
    (void)mfmData;
    (void)mfmPosition;
}

unsigned int AdmXCopyBridge::commitWriteBuffer(bool side, unsigned int track)
{
    (void)side;
    (void)track;
    return 0;
}

/* ------------------------------------------------------------------------- */

#if ADM_XCOPY_DIAG
void AdmXCopyBridge::diagLog(const char* fmt, ...)
{
    static FILE* f = fopen("C:/temp/admxcopy.log", "w");
    if (!f)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fflush(f);
}
#endif
