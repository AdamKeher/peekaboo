#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "AmigaEmulator.h"
#include "AmigaMfmDecoder.h"

namespace adm::core {

// Enriched record computed from a raw DiskDmaEvent: the captured payload plus
// everything derived from it (hash, decoded AmigaDOS sectors).
struct DiskRecord
{
    AmigaEmulator::DiskDmaEvent raw{}; // raw hardware capture

    uint32_t frame = 0; // host frame index the transfer completed in
    uint32_t lRef = 0;  // beam-relative offset, 0 when the frame start is unknown

    // The core rewinds raw.seq to 0 whenever the machine restarts, and `frame` is
    // measured from that restart too, so neither orders the log on its own. These do.
    uint32_t session = 0; // which run this transfer came from (0 = the first)
    uint64_t ordinal = 0; // strictly increasing over the profiler's whole lifetime

    // Payload as it crossed the bus. Retained in full only for the most recent
    // kPayloadRetainCount transfers; older records keep the hash + sector summary.
    std::vector<uint8_t> payload;
    bool payloadDropped = false;

    std::string sha1; // hash of the transferred bytes

    bool standardTrack = false;
    int sectorsChecksumOk = 0;
    std::vector<AmigaMfmDecoder::Sector> sectors;
    std::vector<std::string> sectorHashes; // parallel to `sectors`

    // Set once a matching track write reaches the backing ADF (DiskWriteEvent).
    bool persistedToFile = false;
};

// Session-scoped log of every disk DMA transfer. Unlike BlitProfiler (which rebuilds
// a per-frame vector), this accumulates across the whole session, because the question
// it answers is "what has this program done to the disk so far?".
class DiskProfiler
{
public:
    static constexpr size_t kMaxRecords = 4096;
    // Full payloads are memory-heavy (~13KB per track), so only the most recent
    // transfers keep theirs; the hash and decoded sectors are retained for all.
    static constexpr size_t kPayloadRetainCount = 64;

    // Copies `len` bytes of a transfer's payload out of the core's arena into `out`,
    // returning the number of bytes read. Injected rather than reached for directly so
    // the profiler stays free of UI/emulator-session dependencies and remains testable.
    using PayloadReader = std::function<int(uint32_t off, uint32_t len, uint8_t* out)>;

    // Call after each EmulatorView::Draw() (which calls retro_run() internally), passing
    // the freshly harvested core ring. Events already seen are skipped via their seq.
    void Update(const AmigaEmulator::DiskDmaEvent* events, int count,
                const PayloadReader& readPayload, uint32_t frameIndex, uint32_t frameStartCycle);
    void Clear();

    // The session index new records are stamped with. Bumped whenever the core's seq
    // ring is seen to rewind, which is exactly when the emulated machine restarted.
    uint32_t CurrentSession() const { return m_session; }
    // Number of distinct sessions present in the retained records.
    int SessionCount() const;

    // Flags the most recent not-yet-flagged write transfer for this drive+track as
    // having reached the backing ADF, driven by the core's DiskWriteEvent stream.
    // A DMA write means the program wrote to the emulated drive; this means it stuck.
    void MarkPersisted(uint8_t drive, int track);

    const std::deque<DiskRecord>& GetRecords() const { return m_records; }

    // Incremented each time new transfers arrive, so views can cache derived state.
    uint32_t GetVersion() const { return m_version; }

    // True when a seq gap was seen: the core ring overflowed between polls.
    bool DroppedEvents() const { return m_droppedEvents; }

    struct Stats
    {
        int totalTransfers = 0;
        int reads = 0;
        int writes = 0;
        uint64_t bytesRead = 0;
        uint64_t bytesWritten = 0;
    };
    Stats ComputeStats() const;

    static std::string HashBytes(const uint8_t* data, size_t len);

    // Builds the derived half of a record (hash, decoded sectors) from raw payload
    // bytes. Exposed for tests, which synthesise payloads with AmigaMfmDecoder.
    static DiskRecord Enrich(const AmigaEmulator::DiskDmaEvent& ev, const uint8_t* payload,
                             size_t payloadLen, uint32_t frameIndex, uint32_t frameStartCycle);

private:
    void TrimPayloads();

    std::deque<DiskRecord> m_records;

    // Events carry a monotonic seq; the core ring is not drained per frame, so we
    // dedup on this rather than clearing (which would also rewind the payload arena).
    uint32_t m_nextSeq = 0;
    bool m_haveSeq = false;
    bool m_droppedEvents = false;
    uint32_t m_session = 0;     // stamped onto each new record; see CurrentSession()
    uint64_t m_nextOrdinal = 0; // stamped onto each new record; never rewinds
    uint32_t m_version = 0;
};

} // namespace adm::core
