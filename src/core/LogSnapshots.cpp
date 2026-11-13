#include "LogSnapshots.h"

#include <fmt/format.h>
#include <filesystem>
#include <string>
#include <vector>

#include "../ui/views/BlitOpPresets.h"
#include "../ui/views/EmulatorView.h"
#include "../ui/views/RegisterOpsView.h"
#include "AmigaEmulator.h"
#include "BlitProfiler.h"
#include "DiskProfiler.h"
#include "RegWriteProfiler.h"

namespace adm::core {

void LogSnapshots::Init(ui::EmulatorView* emulatorView, BlitProfiler* blitProfiler,
                        RegWriteProfiler* regWriteProfiler, DiskProfiler* diskProfiler,
                        ui::RegisterOpsView* registerOpsView)
{
    m_emulatorView = emulatorView;
    m_blitProfiler = blitProfiler;
    m_regWriteProfiler = regWriteProfiler;
    m_diskProfiler = diskProfiler;
    m_registerOpsView = registerOpsView;
}

core::AudioLogSnapshot LogSnapshots::MakeAudioLogSnapshot() const
{
    core::AudioLogSnapshot snap;
    if (!m_emulatorView)
        return snap; // available stays false

    auto* emu = m_emulatorView->GetEmulator();
    if (!emu)
        return snap;

    snap.available = true;
    const auto s = emu->GetAudioStats();
    snap.initialized = s.initialized;
    snap.muted = s.muted;
    snap.resampling = s.resampling;
    snap.backend = s.backend;
    snap.coreSampleRate = s.coreSampleRate;
    snap.deviceSampleRate = s.deviceSampleRate;
    snap.targetFps = s.targetFps;
    snap.framesProduced = s.framesProduced;
    snap.framesDroppedOverrun = s.framesDroppedOverrun;
    snap.framesConsumed = s.framesConsumed;
    snap.framesHeld = s.framesHeld;
    snap.framesDroppedResampler = s.framesDroppedResampler;
    snap.deviceCallbacks = s.deviceCallbacks;
    snap.underrunCallbacks = s.underrunCallbacks;
    snap.ringSize = s.ringSize;
    snap.ringLevel = s.ringLevel;
    snap.ringLevelMin = s.ringLevelMin;
    snap.ringLevelMax = s.ringLevelMax;
    snap.ringTarget = s.ringTarget;
    snap.ringLevelMean = s.ringLevelMean;
    snap.framesTrimmed = s.framesTrimmed;
    snap.trimEvents = s.trimEvents;
    snap.ringLevelMinPlaying = s.ringLevelMinPlaying;
    snap.pacePpm = s.pacePpm;
    snap.produceGapEdgesMs = AmigaEmulator::kProduceGapEdgesMs;
    snap.produceGapHist = s.produceGapHist;
    snap.devicePeriodFrames = s.devicePeriodFrames;
    snap.deviceBufferFrames = s.deviceBufferFrames;
    snap.latencyMs = s.latencyMs;
    snap.lastBlockFrames = s.lastBlockFrames;
    snap.minBlockFrames = s.minBlockFrames;
    snap.maxBlockFrames = s.maxBlockFrames;
    snap.blockCount = s.blockCount;
    snap.meanBlockFrames = s.meanBlockFrames;
    snap.timeDiscardEvents = s.timeDiscardEvents;
    snap.timeDiscardedFrames = s.timeDiscardedFrames;
    snap.timeDiscardElapsedClamp = s.timeDiscardElapsedClamp;
    snap.timeDiscardAccumulatorClamp = s.timeDiscardAccumulatorClamp;
    snap.timeDiscardFrameStopCap = s.timeDiscardFrameStopCap;
    snap.produceGapMeanMs = s.produceGapMeanMs;
    snap.produceGapMaxMs = s.produceGapMaxMs;
    snap.produceStalls = s.produceStalls;
    snap.updateMeanMs = s.updateMeanMs;
    snap.updateMaxMs = s.updateMaxMs;
    snap.updateSlow = s.updateSlow;
    snap.retroRunCalls = s.retroRunCalls;
    snap.frameBoundaries = s.frameBoundaries;
    snap.frameCckMean = s.frameCckMean;
    snap.frameCckMin = s.frameCckMin;
    snap.frameCckMax = s.frameCckMax;

    auto fillTap = [emu](AmigaEmulator::AudioTap tap) {
        const auto t = emu->GetAudioRecordingStats(tap);
        core::AudioTapStatus status;
        status.recording = t.recording;
        status.path = t.path;
        status.sampleRate = t.sampleRate;
        status.channels = t.channels;
        status.framesWritten = t.framesWritten;
        status.framesDroppedStaging = t.framesDroppedStaging;
        status.seconds = t.seconds;
        return status;
    };
    snap.coreTap = fillTap(AmigaEmulator::AudioTap::Core);
    snap.deviceTap = fillTap(AmigaEmulator::AudioTap::Device);
    return snap;
}

core::AudioRecordResult LogSnapshots::ApplyAudioRecordRequest(const core::AudioRecordRequest& req)
{
    core::AudioRecordResult result;

    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu) {
        result.message = "emulator unavailable";
        return result;
    }

    if (req.stop) {
        const bool wasCore = emu->IsAudioRecording(AmigaEmulator::AudioTap::Core);
        const bool wasDevice = emu->IsAudioRecording(AmigaEmulator::AudioTap::Device);
        if (!wasCore && !wasDevice) {
            result.message = "no audio recording in progress";
            return result;
        }
        // Capture the paths before stopping — Stop() is what finalises the WAV headers.
        const auto coreStats = emu->GetAudioRecordingStats(AmigaEmulator::AudioTap::Core);
        const auto deviceStats = emu->GetAudioRecordingStats(AmigaEmulator::AudioTap::Device);
        emu->StopAllAudioRecording();

        std::string msg = "Audio recording stopped.";
        if (wasCore) {
            msg += fmt::format(" Core tap: {:.2f}s -> {}", coreStats.seconds, coreStats.path);
        }
        if (wasDevice) {
            msg += fmt::format(" Device tap: {:.2f}s -> {}", deviceStats.seconds, deviceStats.path);
        }
        result.ok = true;
        result.message = msg;
        return result;
    }

    if (!req.tapCore && !req.tapDevice) {
        result.message = "no tap selected";
        return result;
    }
    if (req.path.empty()) {
        result.message = "no output path";
        return result;
    }

    // A two-tap capture writes a matched pair, distinguished by a ".device" infix so one command
    // produces two files that can be compared directly.
    std::filesystem::path base(req.path);
    std::string devicePath = req.path;
    if (req.tapCore && req.tapDevice) {
        std::filesystem::path d = base;
        d.replace_extension();
        devicePath = d.string() + ".device" + base.extension().string();
    }

    std::string error;
    std::string started;

    if (req.tapCore) {
        if (!emu->StartAudioRecording(AmigaEmulator::AudioTap::Core, req.path, req.seconds,
                                      error)) {
            result.message = "core tap: " + error;
            return result;
        }
        started += fmt::format(" core -> {}", req.path);
    }
    if (req.tapDevice) {
        if (!emu->StartAudioRecording(AmigaEmulator::AudioTap::Device, devicePath, req.seconds,
                                      error)) {
            // Leave nothing half-started: a lone core tap would be mistaken for a matched pair.
            if (req.tapCore)
                emu->StopAudioRecording(AmigaEmulator::AudioTap::Core);
            result.message = "device tap: " + error;
            return result;
        }
        started += fmt::format(" device -> {}", devicePath);
    }

    // Counters are session-scoped, so a fresh capture starts from a clean slate.
    emu->ResetAudioStats();

    result.ok = true;
    result.message = fmt::format(
        "Audio recording started:{}{}", started,
        req.seconds > 0.0 ? fmt::format(" (stops after {:.1f}s)", req.seconds) : std::string());
    return result;
}

core::BlitLogSnapshot LogSnapshots::MakeBlitLogSnapshot(int limit) const
{
    core::BlitLogSnapshot snap;
    if (!m_blitProfiler || !m_emulatorView || !m_emulatorView->IsRunning())
        return snap; // available stays false

    snap.available = true;
    const auto stats = m_blitProfiler->ComputeStats();
    snap.totalBlits = stats.totalBlits;
    snap.cpuBlits = stats.cpuBlits;
    snap.copperBlits = stats.copperBlits;
    snap.totalDmaCycles = stats.totalDmaCycles;
    snap.busPercent = stats.busPercent;

    const auto& records = m_blitProfiler->GetCurrentFrame();
    size_t count = records.size();
    if (limit > 0 && static_cast<size_t>(limit) < count) {
        count = static_cast<size_t>(limit);
        snap.truncated = true;
    }

    snap.blits.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& rec = records[i];
        const auto& raw = rec.raw;
        core::BlitLogEntry e;
        e.startCycle = rec.startCycle;
        e.lRef = rec.lRef;
        e.startLine = rec.startLine;
        e.startClock = rec.startClock;
        e.fromCopper = raw.fromCopper;
        e.lineMode = rec.lineMode;
        e.widthPx = static_cast<uint16_t>(rec.wordsPerRow * 16u);
        e.rows = rec.rows;
        e.wordsPerRow = rec.wordsPerRow;
        e.aptr = raw.bltapt;
        e.bptr = raw.bltbpt;
        e.cptr = raw.bltcpt;
        e.dptr = raw.bltdpt;
        e.amod = static_cast<int16_t>(raw.bltamod);
        e.bmod = static_cast<int16_t>(raw.bltbmod);
        e.cmod = static_cast<int16_t>(raw.bltcmod);
        e.dmod = static_cast<int16_t>(raw.bltdmod);
        e.useA = rec.useA;
        e.useB = rec.useB;
        e.useC = rec.useC;
        e.useD = rec.useD;
        e.minterm = rec.minterm;
        e.bltcon0 = raw.bltcon0;
        e.bltcon1 = raw.bltcon1;
        e.dmaCycles = rec.dmaCycles;

        // Friendly op label, matching the Blitter Operations window's column.
        if (rec.lineMode) {
            e.op = "line";
        } else {
            e.op = "-";
            for (const auto& c : ui::kBlitOpPresets) {
                if (raw.bltcon0 == c.bltcon0 && raw.bltcon1 == c.bltcon1) {
                    e.op = c.name;
                    break;
                }
            }
        }
        snap.blits.push_back(std::move(e));
    }
    return snap;
}

core::RegLogSnapshot LogSnapshots::MakeRegLogSnapshot(int limit) const
{
    core::RegLogSnapshot snap;
    if (!m_regWriteProfiler || !m_emulatorView || !m_emulatorView->IsRunning())
        return snap; // available stays false

    snap.available = true;
    // Distinguish "no writes this frame" from "this core cannot report them at all", so a
    // caller seeing an empty log knows which it is.
    if (auto* emu = m_emulatorView->GetEmulator())
        snap.coreSupported = emu->HasRegWriteLog();

    const auto stats = m_regWriteProfiler->ComputeStats();
    snap.totalWrites = stats.totalWrites;
    snap.cpuWrites = stats.cpuWrites;
    snap.copperWrites = stats.copperWrites;
    snap.changedWrites = stats.changedWrites;
    snap.distinctRegisters = stats.distinctRegisters;

    const auto& records = m_regWriteProfiler->GetCurrentFrame();
    size_t count = records.size();
    if (limit > 0 && static_cast<size_t>(limit) < count) {
        count = static_cast<size_t>(limit);
        snap.truncated = true;
    }

    snap.writes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& rec = records[i];
        const auto& raw = rec.raw;
        core::RegLogEntry e;
        e.lRef = rec.lRef;
        e.line = raw.vpos;
        e.clock = raw.hpos;
        e.reg = raw.reg;
        e.value = raw.value;
        e.prevValue = rec.prevValue;
        e.changed = rec.changed;
        e.firstWrite = rec.firstWrite;
        e.fromCopper = raw.fromCopper;
        e.pc = raw.pc;
        // Names/descriptions come from the same JSON the window reads, via the view that
        // already owns the parsed copy and its per-offset cache.
        if (m_registerOpsView) {
            e.name = m_registerOpsView->RegisterName(raw.reg);
            e.description = m_registerOpsView->RegisterDescription(raw.reg);
        }
        snap.writes.push_back(std::move(e));
    }
    return snap;
}

core::DiskLogSnapshot LogSnapshots::MakeDiskLogSnapshot(const core::DiskLogQuery& q) const
{
    core::DiskLogSnapshot snap;
    if (!m_diskProfiler || !m_emulatorView || !m_emulatorView->IsRunning())
        return snap; // available stays false
    if (!m_emulatorView->HasDiskDmaLog())
        return snap; // core predates the disk DMA exports

    snap.available = true;
    snap.droppedEvents = m_diskProfiler->DroppedEvents();
    snap.currentSession = m_diskProfiler->CurrentSession();
    snap.sessionCount = m_diskProfiler->SessionCount();

    // WHOLE-LOG totals, deliberately independent of the caller's filter, so the
    // "did anything get written back?" answer never depends on the limit. `matched`
    // below is the filter-scoped count; the two used to be conflated under one name.
    const auto stats = m_diskProfiler->ComputeStats();
    snap.totalTransfers = stats.totalTransfers;
    snap.reads = stats.reads;
    snap.writes = stats.writes;
    snap.bytesRead = stats.bytesRead;
    snap.bytesWritten = stats.bytesWritten;
    snap.anyWrites = stats.writes > 0;

    const auto& records = m_diskProfiler->GetRecords();

    // sinceSeq is only meaningful inside one session, because the core rewinds seq to 0
    // on every restart. Default it to the current session so incremental polling cannot
    // silently re-match transfers from a previous run that happen to share seq numbers.
    const uint32_t sinceSession = q.sinceSession != 0 ? q.sinceSession : snap.currentSession;

    // Walk newest-first so a limit keeps the most RECENT transfers.
    std::vector<const core::DiskRecord*> matched;
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const core::DiskRecord& r = *it;
        if (q.direction != 0 && r.raw.direction != q.direction)
            continue;
        if (q.sinceOrdinal != 0 && r.ordinal <= q.sinceOrdinal)
            continue;
        if (q.sinceSeq != 0 && (r.session != sinceSession || r.raw.seq < q.sinceSeq))
            continue;
        ++snap.matched;
        if (q.limit > 0 && matched.size() >= static_cast<size_t>(q.limit)) {
            snap.truncated = true;
            continue;
        }
        matched.push_back(&r);
    }

    // `matched` is newest-first as built. Emit it that way by default - which is what
    // the tool description has always claimed - and reverse only when asked.
    snap.order = q.newestFirst ? "newestFirst" : "oldestFirst";
    snap.transfers.reserve(matched.size());
    for (size_t mi = 0; mi < matched.size(); ++mi) {
        const core::DiskRecord& r = *matched[q.newestFirst ? mi : matched.size() - 1 - mi];
        const auto& raw = r.raw;
        core::DiskLogEntry e;
        e.seq = raw.seq;
        e.session = r.session;
        e.ordinal = r.ordinal;
        e.startCycle = raw.startCycle;
        e.endCycle = raw.endCycle;
        e.lRef = r.lRef;
        e.frame = r.frame;
        e.direction = (raw.direction == 3) ? "write" : "read";
        e.trigger = raw.fromCopper ? "copper" : "cpu";
        e.pc = raw.pc;
        e.drive = raw.drive;
        e.cyl = raw.cyl;
        e.side = raw.side;
        e.dskpt = raw.dskptStart;
        e.dskptEnd = raw.dskptEnd;
        e.dsklen = raw.dsklen;
        e.words = raw.words;
        e.bytes = raw.payloadLen;
        e.turbo = raw.turbo != 0;
        e.aborted = raw.aborted != 0;
        e.persistedToFile = r.persistedToFile;
        e.sha1 = r.sha1;
        e.standardTrack = r.standardTrack;
        e.sectorCount = static_cast<int>(r.sectors.size());
        e.sectorsChecksumOk = r.sectorsChecksumOk;
        if (q.includeSectors) {
            e.sectors.reserve(r.sectors.size());
            for (size_t i = 0; i < r.sectors.size(); ++i) {
                core::DiskLogSectorInfo s;
                s.track = r.sectors[i].trackNumber;
                s.sector = r.sectors[i].sectorNumber;
                s.checksumOk = r.sectors[i].checksumOk;
                if (i < r.sectorHashes.size())
                    s.sha1 = r.sectorHashes[i];
                e.sectors.push_back(std::move(s));
            }
        }
        snap.transfers.push_back(std::move(e));
    }
    return snap;
}

} // namespace adm::core
