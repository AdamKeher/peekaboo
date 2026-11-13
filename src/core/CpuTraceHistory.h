#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "AmigaEmulator.h"
#include "M68kEffectiveAddress.h"

namespace adm::core {

// One recorded instruction, with the session-wide bookkeeping the per-frame core
// buffer does not carry.
struct CpuTraceRecord
{
    uint64_t seq = 0;   // monotonic across the session; stable identity for a row
    uint64_t frame = 0; // emulated frame it executed in
    uint32_t cycle = 0; // absolute CCK
    uint32_t lRef = 0;  // cycle - frame start, i.e. position within the frame
    uint16_t vpos = 0;
    uint16_t hpos = 0;
    uint32_t pc = 0;
    uint8_t raw[10] = {};
    uint8_t rawLen = 0;
    uint16_t sr = 0;
    uint32_t d[8] = {};
    uint32_t a[8] = {};

    M68kRegisters Registers() const;
};

enum class TraceQueryMode {
    Filter,     // predicate match over the recorded instructions
    Accesses,   // instructions whose effective address falls in a range
    LastWriter, // the most recent instruction to write a range
};

struct CpuTraceQuery
{
    TraceQueryMode mode = TraceQueryMode::Filter;

    // Filter predicates. Unset fields do not constrain the match.
    bool havePcRange = false;
    uint32_t pcLo = 0;
    uint32_t pcHi = 0; // exclusive
    bool haveFrame = false;
    uint64_t frame = 0;

    // Accesses / LastWriter target range.
    uint32_t addr = 0;
    uint32_t len = 1;
    bool readsOnly = false;
    bool writesOnly = false;

    // Only consider records with seq < beforeSeq (0 = no bound). Lets a caller
    // walk provenance backwards one writer at a time.
    uint64_t beforeSeq = 0;

    // Register predicate, e.g. reg="d0". Empty means no constraint.
    std::string reg;
    uint32_t regValue = 0;

    // Paging for plain reads: start at the record with this seq (0 = oldest kept).
    uint64_t startSeq = 0;
    int limit = 64; // <= 0 means all
    bool newestFirst = false;
};

// A matched instruction plus, for access queries, the access that matched.
struct CpuTraceMatch
{
    CpuTraceRecord record;
    bool haveAccess = false;
    M68kAccess access;
};

struct CpuTraceSnapshot
{
    bool available = false;
    bool enabled = false; // history recording turned on
    std::string hint;     // set when unavailable, explaining what to do
    uint64_t totalRecords = 0;
    uint64_t oldestSeq = 0;
    uint64_t newestSeq = 0;
    int frames = 0;
    bool dropped = false; // the ring wrapped, so older instructions are gone
    uint32_t totalMatches = 0;
    bool truncated = false;
    std::vector<CpuTraceMatch> matches;
};

/**
 * @brief A rolling, searchable history of executed instructions.
 *
 * The core's trace buffer is per-frame: it is refilled every retro_run, so a
 * question like "which instruction last wrote this?" can only be asked about the
 * frame you happen to be sitting in. This keeps a bounded ring of recent frames
 * so provenance survives across a boot.
 *
 * Recording is opt-in because it is not cheap — each record is ~90 bytes and a
 * traced A500 frame is 14-17k instructions, so the default cap is a few tens of
 * frames rather than a whole session.
 */
class CpuTraceHistory
{
public:
    static constexpr size_t kDefaultMaxRecords = 500000; // ~44 MB, ~30 frames

    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled; }

    // Cap in records. Shrinking drops the oldest immediately.
    void SetCapacity(size_t maxRecords);
    size_t Capacity() const { return m_capacity; }

    // Append one frame's worth of core trace entries. No-op while disabled or
    // when this frame has already been appended.
    void AppendFrame(const AmigaEmulator::CpuTraceEntry* entries, int count, uint64_t frame,
                     uint32_t frameStartCycle);

    // Drop everything (reset, state restore, new document).
    void Clear();

    size_t Size() const { return m_records.size(); }
    bool Dropped() const { return m_dropped; }
    uint64_t OldestSeq() const { return m_records.empty() ? 0 : m_records.front().seq; }
    uint64_t NewestSeq() const { return m_records.empty() ? 0 : m_records.back().seq; }
    int FrameCount() const;

    CpuTraceSnapshot Query(const CpuTraceQuery& q) const;

    // Parses "d0".."d7"/"a0".."a7"/"pc"/"sr" into a value getter. Returns false
    // for an unknown name.
    static bool ReadRegister(const CpuTraceRecord& rec, const std::string& name, uint32_t& out);

private:
    void Trim();

    std::deque<CpuTraceRecord> m_records;
    size_t m_capacity = kDefaultMaxRecords;
    bool m_enabled = false;
    bool m_dropped = false;
    uint64_t m_nextSeq = 1;
    uint64_t m_lastFrame = 0;
    bool m_haveLastFrame = false;
};

} // namespace adm::core
