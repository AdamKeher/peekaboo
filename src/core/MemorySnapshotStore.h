#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace adm::core {

// One address range the caller asked to capture or exclude.
struct MemoryRange
{
    uint32_t addr = 0;
    uint32_t len = 0;
};

// A live memory region as handed in by the emulator at capture time.
struct LiveMemoryRegion
{
    uint32_t base = 0;
    uint32_t size = 0;
    const uint8_t* data = nullptr;
    std::string name;
    bool writable = false;
};

// One captured slice of the address space.
struct SnapshotRegion
{
    uint32_t base = 0;
    std::string name;
    std::vector<uint8_t> bytes;

    uint32_t End() const { return base + static_cast<uint32_t>(bytes.size()); }
};

struct MemorySnapshotInfo
{
    std::string name;
    uint64_t frame = 0;
    std::string created; // "YYYY-MM-DD HH:MM:SS" local time
    uint64_t bytes = 0;
    int regionCount = 0;
    std::vector<MemoryRange> regions; // base/size of each captured slice
};

// A contiguous span of addresses that satisfied the diff predicate.
struct MemoryDiffRun
{
    uint32_t addr = 0;
    uint32_t len = 0;
    std::vector<uint8_t> before; // bytes from snapshot A
    std::vector<uint8_t> after;  // bytes from snapshot B
};

struct MemoryDiffOptions
{
    std::vector<MemoryRange> exclude;
    // "changed" (default): bytes that differ between A and B.
    // "same": bytes that are identical between A and B.
    bool wantSame = false;
    // Optional third snapshot C. A run additionally requires B == C over its
    // whole span, which is what isolates "changed between A and B but stable
    // afterwards" — the shape of a selection-dependent byte.
    std::string stableWith;
    uint32_t minRun = 1;
    uint32_t maxRun = 0; // 0 = no upper bound
    int limit = 256;     // <= 0 means all
    bool includeBytes = true;
};

struct MemoryDiffResult
{
    bool ok = false;
    std::string error;
    uint32_t totalRuns = 0;     // before the limit
    uint64_t bytesMatching = 0; // total bytes across all qualifying runs
    bool truncated = false;
    std::vector<MemoryDiffRun> runs;
};

/**
 * @brief Named raw memory snapshots plus a temporal diff over them.
 *
 * Deliberately distinct from the two things that already exist:
 *   - EmulatorStateStore holds whole machine states (for restoring a run);
 *   - the AR console's deep trainer is value-oriented and is wiped by TX.
 *
 * This is a raw byte record held server-side and keyed by name, so it survives
 * a load_state and can be compared across one. Snapshots are in-memory only —
 * they describe one investigation, not a durable artefact.
 */
class MemorySnapshotStore
{
public:
    // Capture `ranges` out of `live`. With no ranges, captures every writable
    // region in full. Returns false with outError when nothing could be
    // captured or when the name is already taken and replace is false.
    bool Capture(const std::string& name, const std::vector<MemoryRange>& ranges,
                 const std::vector<LiveMemoryRegion>& live, uint64_t frame, bool replace,
                 std::string& outError);

    bool Remove(const std::string& name);
    void Clear() { m_snapshots.clear(); }

    bool Has(const std::string& name) const { return Find(name) != nullptr; }
    std::optional<MemorySnapshotInfo> Info(const std::string& name) const;
    // Newest first.
    std::vector<MemorySnapshotInfo> List() const;
    size_t Count() const { return m_snapshots.size(); }

    // Compare two snapshots. Refuses (ok=false) when the captured layouts differ,
    // because misaligned regions would produce meaningless runs — the same guard
    // the deep trainer applies before it diffs.
    MemoryDiffResult Diff(const std::string& a, const std::string& b,
                          const MemoryDiffOptions& opts) const;

    // Name rule shared with the tool layer: printable, no path separators.
    static bool IsValidName(const std::string& name);

private:
    struct Entry
    {
        MemorySnapshotInfo info;
        std::vector<SnapshotRegion> regions;
    };

    const Entry* Find(const std::string& name) const;

    // Newest last.
    std::vector<Entry> m_snapshots;
};

} // namespace adm::core
