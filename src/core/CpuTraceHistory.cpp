#include "CpuTraceHistory.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace adm::core {

namespace {

bool InRange(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v < hi; }

} // namespace

M68kRegisters CpuTraceRecord::Registers() const
{
    M68kRegisters r;
    std::memcpy(r.d, d, sizeof(r.d));
    std::memcpy(r.a, a, sizeof(r.a));
    return r;
}

void CpuTraceHistory::SetEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    // Turning recording on starts a fresh history so a new run is not spliced
    // onto an old one. Turning it off keeps what was recorded, so the answers
    // are still there to query.
    if (enabled)
        Clear();
    m_enabled = enabled;
}

void CpuTraceHistory::SetCapacity(size_t maxRecords)
{
    m_capacity = maxRecords == 0 ? 1 : maxRecords;
    Trim();
}

void CpuTraceHistory::Clear()
{
    m_records.clear();
    m_dropped = false;
    m_nextSeq = 1;
    m_haveLastFrame = false;
    m_lastFrame = 0;
}

void CpuTraceHistory::Trim()
{
    while (m_records.size() > m_capacity) {
        m_records.pop_front();
        m_dropped = true;
    }
}

void CpuTraceHistory::AppendFrame(const AmigaEmulator::CpuTraceEntry* entries, int count,
                                  uint64_t frame, uint32_t frameStartCycle)
{
    if (!m_enabled || !entries || count <= 0)
        return;
    // The harvested per-frame view is re-read every UI tick; only take it once.
    if (m_haveLastFrame && frame == m_lastFrame)
        return;
    m_haveLastFrame = true;
    m_lastFrame = frame;

    for (int i = 0; i < count; ++i) {
        const auto& e = entries[i];
        CpuTraceRecord rec;
        rec.seq = m_nextSeq++;
        rec.frame = frame;
        rec.cycle = e.absolute_cycle;
        rec.lRef = e.absolute_cycle >= frameStartCycle
                       ? e.absolute_cycle - frameStartCycle
                       : static_cast<uint32_t>(e.vpos) * 228u + e.hpos;
        rec.vpos = e.vpos;
        rec.hpos = e.hpos;
        rec.pc = e.pc;
        rec.rawLen =
            e.raw_len > sizeof(rec.raw) ? static_cast<uint8_t>(sizeof(rec.raw)) : e.raw_len;
        std::memcpy(rec.raw, e.raw, rec.rawLen);
        rec.sr = e.sr;
        std::memcpy(rec.d, e.d, sizeof(rec.d));
        std::memcpy(rec.a, e.a, sizeof(rec.a));
        m_records.push_back(rec);
    }
    Trim();
}

int CpuTraceHistory::FrameCount() const
{
    if (m_records.empty())
        return 0;
    int frames = 1;
    uint64_t prev = m_records.front().frame;
    for (const auto& r : m_records) {
        if (r.frame != prev) {
            ++frames;
            prev = r.frame;
        }
    }
    return frames;
}

bool CpuTraceHistory::ReadRegister(const CpuTraceRecord& rec, const std::string& name,
                                   uint32_t& out)
{
    if (name.size() == 2) {
        const char kind = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
        const char digit = name[1];
        if (digit >= '0' && digit <= '7') {
            const int idx = digit - '0';
            if (kind == 'd') {
                out = rec.d[idx];
                return true;
            }
            if (kind == 'a') {
                out = rec.a[idx];
                return true;
            }
        }
        if (kind == 'p' && std::tolower(static_cast<unsigned char>(digit)) == 'c') {
            out = rec.pc;
            return true;
        }
        if (kind == 's' && std::tolower(static_cast<unsigned char>(digit)) == 'r') {
            out = rec.sr;
            return true;
        }
    }
    if (name == "sp" || name == "SP") {
        out = rec.a[7];
        return true;
    }
    return false;
}

CpuTraceSnapshot CpuTraceHistory::Query(const CpuTraceQuery& q) const
{
    CpuTraceSnapshot snap;
    snap.enabled = m_enabled;
    if (m_records.empty()) {
        snap.hint = m_enabled
                        ? "History is on but empty - enable the CPU trace (set_cpu_trace) and run "
                          "at least one frame."
                        : "CPU trace history is off - turn it on with configure_cpu_trace_history, "
                          "enable set_cpu_trace, then run a frame.";
        return snap;
    }

    snap.available = true;
    snap.totalRecords = m_records.size();
    snap.oldestSeq = m_records.front().seq;
    snap.newestSeq = m_records.back().seq;
    snap.frames = FrameCount();
    snap.dropped = m_dropped;

    const uint32_t lo = q.addr;
    const uint32_t hi = q.addr + (q.len == 0 ? 1 : q.len);
    const size_t limit = q.limit <= 0 ? SIZE_MAX : static_cast<size_t>(q.limit);

    // Shared predicates. `outAccess` is only filled for the access-based modes.
    auto matches = [&](const CpuTraceRecord& rec, M68kAccess& outAccess, bool& haveAccess) {
        haveAccess = false;
        if (q.beforeSeq != 0 && rec.seq >= q.beforeSeq)
            return false;
        if (q.haveFrame && rec.frame != q.frame)
            return false;
        if (q.havePcRange && !InRange(rec.pc, q.pcLo, q.pcHi))
            return false;
        if (!q.reg.empty()) {
            uint32_t v = 0;
            if (!ReadRegister(rec, q.reg, v) || v != q.regValue)
                return false;
        }
        if (q.mode == TraceQueryMode::Filter)
            return true;

        const M68kRegisters regs = rec.Registers();
        for (const auto& acc : M68kEffectiveAddress::Decode(rec.raw, rec.rawLen, rec.pc, regs)) {
            if (!acc.Touches(lo, hi))
                continue;
            if (q.writesOnly && !acc.write)
                continue;
            if (q.readsOnly && !acc.read)
                continue;
            if (q.mode == TraceQueryMode::LastWriter && !acc.write)
                continue;
            outAccess = acc;
            haveAccess = true;
            return true;
        }
        return false;
    };

    if (q.mode == TraceQueryMode::LastWriter) {
        // Walk backwards and stop at the first hit - that is the answer, and
        // scanning the whole ring for it would be wasted work.
        for (auto it = m_records.rbegin(); it != m_records.rend(); ++it) {
            M68kAccess acc;
            bool have = false;
            if (!matches(*it, acc, have))
                continue;
            snap.totalMatches = 1;
            snap.matches.push_back({*it, have, acc});
            return snap;
        }
        return snap;
    }

    auto consider = [&](const CpuTraceRecord& rec) {
        if (rec.seq < q.startSeq)
            return;
        M68kAccess acc;
        bool have = false;
        if (!matches(rec, acc, have))
            return;
        ++snap.totalMatches;
        if (snap.matches.size() >= limit) {
            snap.truncated = true;
            return;
        }
        snap.matches.push_back({rec, have, acc});
    };

    if (q.newestFirst)
        for (auto it = m_records.rbegin(); it != m_records.rend(); ++it)
            consider(*it);
    else
        for (const auto& rec : m_records)
            consider(rec);

    return snap;
}

} // namespace adm::core
