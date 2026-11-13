#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adm::core {

// One AmigaDOS sector recovered from a transfer's MFM payload.
struct DiskLogSectorInfo
{
    uint8_t track = 0;       // cyl*2+head, from the sector header
    uint8_t sector = 0;      // 0..10
    bool checksumOk = false; // header + data checksums both verified
    std::string sha1;        // hash of the decoded 512-byte payload
};

// One disk DMA transfer, mirroring a row of the "Disk I/O" window. Self-contained (no
// emulator/profiler types) so the AR console DSKLOG command and the MCP get_disk_log tool
// can consume it without pulling in AmigaEmulator/DiskProfiler.
struct DiskLogEntry
{
    uint32_t seq = 0;        // core ring sequence; RESTARTS AT 0 each session (see session)
    uint32_t session = 0;    // emulator session this transfer belongs to; see DiskLogSnapshot
    uint64_t ordinal = 0;    // strictly increasing across the whole log, sessions included
    uint32_t startCycle = 0; // absolute CCK at the DSKLEN write
    uint32_t endCycle = 0;   // absolute CCK at completion
    uint32_t lRef = 0;       // beam-relative offset (vpos*228 + hpos), 0 when unknown
    uint32_t frame = 0;      // host frame index the transfer completed in
    std::string direction;   // "read" | "write"
    std::string trigger;     // "cpu" | "copper"
    uint32_t pc = 0;         // instruction that armed the transfer
    uint8_t drive = 0;       // 0xFF when no drive was selected
    uint8_t cyl = 0, side = 0;
    uint32_t dskpt = 0, dskptEnd = 0;
    uint16_t dsklen = 0;          // raw DSKLEN register value
    uint16_t words = 0;           // DSKLEN & 0x3fff
    uint32_t bytes = 0;           // bytes actually captured
    bool turbo = false;           // transferred via the turbo (instant) path
    bool aborted = false;         // DSKLEN cleared before the transfer finished
    bool persistedToFile = false; // a matching track write reached the backing ADF
    std::string sha1;             // hash of the transferred bytes
    bool standardTrack = false;   // 11 checksum-valid sectors numbered 0..10
    int sectorCount = 0;
    int sectorsChecksumOk = 0;
    std::vector<DiskLogSectorInfo> sectors; // omitted when the caller opts out
};

// Filter applied when building a DiskLogSnapshot. Defaults return the most recent
// transfers in both directions, with decoded sector detail included.
struct DiskLogQuery
{
    int limit = 64;             // <= 0 returns everything matching
    int direction = 0;          // 0 any, 2 reads only, 3 writes only (DSKDMA_* values)
    bool includeSectors = true; // drop per-sector detail to keep responses small
    uint32_t sinceSeq = 0;      // only transfers with seq >= this, within `session`
    // Which session sinceSeq is measured in. 0 means "the current one", which is the
    // only reading that makes sense for incremental polling, since seq restarts at 0
    // whenever the machine is restarted.
    uint32_t sinceSession = 0;
    uint64_t sinceOrdinal = 0; // only transfers with ordinal > this (0 = all); session-proof
    bool newestFirst = true;   // order of `transfers`; the limit always keeps the newest
};

// Snapshot of the session's disk DMA activity, produced on demand by Application from the
// DiskProfiler and consumed by the AR console DSKLOG command + the MCP get_disk_log tool.
// Unlike BlitLogSnapshot this spans the whole session, not a single frame.
struct DiskLogSnapshot
{
    bool available = false; // emulator running, core exports present, profiler alive

    // --- scope: MATCHED THE FILTER ---
    int matched = 0; // transfers passing direction/since*, before `limit` was applied

    // --- scope: WHOLE LOG (every session held in the profiler, filter ignored) ---
    int totalTransfers = 0;
    int reads = 0;
    int writes = 0;
    uint64_t bytesRead = 0;
    uint64_t bytesWritten = 0;
    bool anyWrites = false; // the one-glance answer to "did it write back to the disk?"

    // The session the machine is in now. Entries with a lower `session` came from an
    // earlier run: their `frame` numbers are on a different clock and their `seq` values
    // repeat, which is why `ordinal` exists.
    uint32_t currentSession = 0;
    int sessionCount = 0; // distinct sessions represented in the log
    // Order of `transfers`, echoed so a caller never has to guess: "newestFirst" or
    // "oldestFirst". The limit always keeps the newest, whichever order is emitted.
    std::string order = "newestFirst";
    bool droppedEvents = false; // a seq gap: transfers were lost to ring overflow
    bool truncated = false;     // more transfers matched than were returned
    std::vector<DiskLogEntry> transfers;
};

} // namespace adm::core
