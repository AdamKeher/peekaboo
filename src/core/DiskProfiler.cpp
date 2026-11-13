#include "DiskProfiler.h"

#include <fmt/format.h>
#include <TinySHA1.hpp>

namespace adm::core {

std::string DiskProfiler::HashBytes(const uint8_t* data, size_t len)
{
    if (!data || len == 0)
        return {};
    sha1::SHA1 hasher;
    hasher.processBytes(data, len);
    sha1::SHA1::digest8_t digest;
    hasher.getDigestBytes(digest);
    // Runs per disk-DMA event with payload — a plain nibble table, not 20 fmt::format calls.
    static const char* hex = "0123456789abcdef";
    std::string result;
    result.reserve(40);
    for (int i = 0; i < 20; ++i) {
        result += hex[digest[i] >> 4];
        result += hex[digest[i] & 0x0F];
    }
    return result;
}

void DiskProfiler::Clear()
{
    m_records.clear();
    m_nextSeq = 0;
    m_haveSeq = false;
    m_droppedEvents = false;
    m_session = 0;
    m_nextOrdinal = 0;
    ++m_version;
}

int DiskProfiler::SessionCount() const
{
    if (m_records.empty())
        return 0;
    int count = 1;
    uint32_t prev = m_records.front().session;
    for (const auto& r : m_records) {
        if (r.session != prev) {
            ++count;
            prev = r.session;
        }
    }
    return count;
}

void DiskProfiler::MarkPersisted(uint8_t drive, int track)
{
    for (auto it = m_records.rbegin(); it != m_records.rend(); ++it) {
        if (it->raw.direction != 3 || it->persistedToFile)
            continue;
        if (it->raw.drive != drive)
            continue;
        if (it->raw.cyl * 2 + it->raw.side != track)
            continue;
        it->persistedToFile = true;
        ++m_version;
        return;
    }
}

DiskRecord DiskProfiler::Enrich(const AmigaEmulator::DiskDmaEvent& ev, const uint8_t* payload,
                                size_t payloadLen, uint32_t frameIndex, uint32_t frameStartCycle)
{
    DiskRecord rec;
    rec.raw = ev;
    rec.frame = frameIndex;
    // Beam-relative position, derived the same way BlitProfiler derives lRef.
    if (frameStartCycle != 0 && ev.startCycle >= frameStartCycle)
        rec.lRef = ev.startCycle - frameStartCycle;

    if (!payload || payloadLen == 0)
        return rec;

    rec.payload.assign(payload, payload + payloadLen);
    rec.sha1 = HashBytes(rec.payload.data(), rec.payload.size());

    // Recover the AmigaDOS sectors so a row can name real track/sector numbers
    // instead of just a chip-RAM address and a length.
    const AmigaMfmDecoder::Track track =
        AmigaMfmDecoder::DecodeTrack(rec.payload.data(), rec.payload.size());
    rec.standardTrack = track.isStandard;
    rec.sectors = track.sectors;
    rec.sectorHashes.reserve(track.sectors.size());
    for (const auto& s : track.sectors) {
        if (s.checksumOk)
            ++rec.sectorsChecksumOk;
        rec.sectorHashes.push_back(HashBytes(s.data.data(), s.data.size()));
    }
    return rec;
}

void DiskProfiler::TrimPayloads()
{
    if (m_records.size() <= kPayloadRetainCount)
        return;
    const size_t cutoff = m_records.size() - kPayloadRetainCount;
    for (size_t i = 0; i < cutoff; ++i) {
        DiskRecord& r = m_records[i];
        if (!r.payload.empty()) {
            r.payload.clear();
            r.payload.shrink_to_fit();
            r.payloadDropped = true;
        }
    }
}

void DiskProfiler::Update(const AmigaEmulator::DiskDmaEvent* events, int count,
                          const PayloadReader& readPayload, uint32_t frameIndex,
                          uint32_t frameStartCycle)
{
    if (!events || count <= 0)
        return;

    std::vector<uint8_t> buf;
    bool ingested = false;

    // The core rewinds seq to 0 on a ring clear (start / disk change / reset). Detect it
    // from the *newest* event being older than everything we have already consumed —
    // testing individual events for seq == 0 would misfire on every poll, because the
    // ring is never drained and so keeps re-presenting seq 0.
    if (m_haveSeq && static_cast<uint64_t>(events[count - 1].seq) + 1 < m_nextSeq) {
        m_nextSeq = 0;
        m_droppedEvents = false;
        // The machine restarted. Records already held keep their old session index, so
        // a reader can tell "before the restart" from "after" - seq and frame both
        // rewind here and would otherwise interleave into nonsense.
        ++m_session;
    }

    for (int i = 0; i < count; ++i) {
        const AmigaEmulator::DiskDmaEvent& e = events[i];

        if (m_haveSeq && e.seq < m_nextSeq)
            continue; // already ingested
        if (m_haveSeq && e.seq > m_nextSeq)
            m_droppedEvents = true; // core ring overflowed between polls

        buf.clear();
        if (e.payloadLen > 0 && readPayload) {
            buf.resize(e.payloadLen);
            const int got = readPayload(e.payloadOff, e.payloadLen, buf.data());
            buf.resize(got > 0 ? static_cast<size_t>(got) : 0);
        }

        DiskRecord rec =
            Enrich(e, buf.empty() ? nullptr : buf.data(), buf.size(), frameIndex, frameStartCycle);
        rec.session = m_session;
        rec.ordinal = m_nextOrdinal++;
        m_records.push_back(std::move(rec));
        while (m_records.size() > kMaxRecords)
            m_records.pop_front();

        m_nextSeq = e.seq + 1;
        m_haveSeq = true;
        ingested = true;
    }

    if (ingested) {
        TrimPayloads();
        ++m_version;
    }
}

DiskProfiler::Stats DiskProfiler::ComputeStats() const
{
    Stats s;
    for (const auto& r : m_records) {
        ++s.totalTransfers;
        const uint32_t bytes = r.raw.payloadLen;
        if (r.raw.direction == 3) {
            ++s.writes;
            s.bytesWritten += bytes;
        } else {
            ++s.reads;
            s.bytesRead += bytes;
        }
    }
    return s;
}

} // namespace adm::core
