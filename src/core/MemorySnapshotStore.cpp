#include "MemorySnapshotStore.h"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace adm::core {

namespace {

std::string NowString()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// Clamped [lo, hi) intersection of a requested range with a live region.
bool Intersect(uint32_t aLo, uint32_t aHi, uint32_t bLo, uint32_t bHi, uint32_t& outLo,
               uint32_t& outHi)
{
    outLo = aLo > bLo ? aLo : bLo;
    outHi = aHi < bHi ? aHi : bHi;
    return outLo < outHi;
}

bool InAnyRange(uint32_t addr, const std::vector<MemoryRange>& ranges)
{
    for (const auto& r : ranges)
        if (r.len != 0 && addr >= r.addr && addr - r.addr < r.len)
            return true;
    return false;
}

} // namespace

bool MemorySnapshotStore::IsValidName(const std::string& name)
{
    if (name.empty() || name.size() > 64)
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
    });
}

const MemorySnapshotStore::Entry* MemorySnapshotStore::Find(const std::string& name) const
{
    auto it = std::find_if(m_snapshots.begin(), m_snapshots.end(),
                           [&](const Entry& e) { return e.info.name == name; });
    return it == m_snapshots.end() ? nullptr : &*it;
}

bool MemorySnapshotStore::Capture(const std::string& name, const std::vector<MemoryRange>& ranges,
                                  const std::vector<LiveMemoryRegion>& live, uint64_t frame,
                                  bool replace, std::string& outError)
{
    if (!IsValidName(name)) {
        outError = "invalid snapshot name (letters, digits, - _ . only, max 64 chars)";
        return false;
    }
    if (Find(name) && !replace) {
        outError =
            "a snapshot named '" + name + "' already exists (pass replace:true to overwrite)";
        return false;
    }

    Entry entry;
    entry.info.name = name;
    entry.info.frame = frame;
    entry.info.created = NowString();

    auto capture = [&](const LiveMemoryRegion& reg, uint32_t lo, uint32_t hi) {
        SnapshotRegion s;
        s.base = lo;
        s.name = reg.name;
        s.bytes.assign(reg.data + (lo - reg.base), reg.data + (hi - reg.base));
        entry.info.bytes += s.bytes.size();
        entry.info.regions.push_back({s.base, static_cast<uint32_t>(s.bytes.size())});
        entry.regions.push_back(std::move(s));
    };

    for (const auto& reg : live) {
        if (!reg.data || reg.size == 0)
            continue;
        if (ranges.empty()) {
            // The default capture is "everything the program could have written",
            // which is what makes a blind before/after diff useful.
            if (!reg.writable)
                continue;
            capture(reg, reg.base, reg.base + reg.size);
            continue;
        }
        for (const auto& want : ranges) {
            if (want.len == 0)
                continue;
            uint32_t lo = 0, hi = 0;
            if (Intersect(want.addr, want.addr + want.len, reg.base, reg.base + reg.size, lo, hi))
                capture(reg, lo, hi);
        }
    }

    if (entry.regions.empty()) {
        outError = ranges.empty() ? "no writable memory regions to capture"
                                  : "none of the requested ranges are mapped";
        return false;
    }

    std::sort(entry.regions.begin(), entry.regions.end(),
              [](const SnapshotRegion& a, const SnapshotRegion& b) { return a.base < b.base; });
    std::sort(entry.info.regions.begin(), entry.info.regions.end(),
              [](const MemoryRange& a, const MemoryRange& b) { return a.addr < b.addr; });
    entry.info.regionCount = static_cast<int>(entry.regions.size());

    Remove(name);
    m_snapshots.push_back(std::move(entry));
    return true;
}

bool MemorySnapshotStore::Remove(const std::string& name)
{
    auto it = std::find_if(m_snapshots.begin(), m_snapshots.end(),
                           [&](const Entry& e) { return e.info.name == name; });
    if (it == m_snapshots.end())
        return false;
    m_snapshots.erase(it);
    return true;
}

std::optional<MemorySnapshotInfo> MemorySnapshotStore::Info(const std::string& name) const
{
    const Entry* e = Find(name);
    if (!e)
        return std::nullopt;
    return e->info;
}

std::vector<MemorySnapshotInfo> MemorySnapshotStore::List() const
{
    std::vector<MemorySnapshotInfo> out;
    out.reserve(m_snapshots.size());
    for (auto it = m_snapshots.rbegin(); it != m_snapshots.rend(); ++it)
        out.push_back(it->info);
    return out;
}

MemoryDiffResult MemorySnapshotStore::Diff(const std::string& aName, const std::string& bName,
                                           const MemoryDiffOptions& opts) const
{
    MemoryDiffResult res;
    const Entry* a = Find(aName);
    const Entry* b = Find(bName);
    if (!a) {
        res.error = "no snapshot named '" + aName + "'";
        return res;
    }
    if (!b) {
        res.error = "no snapshot named '" + bName + "'";
        return res;
    }

    const Entry* c = nullptr;
    if (!opts.stableWith.empty()) {
        c = Find(opts.stableWith);
        if (!c) {
            res.error = "no snapshot named '" + opts.stableWith + "'";
            return res;
        }
    }

    // Comparing snapshots taken over different layouts would silently line up
    // unrelated bytes, so refuse instead — the same guard the deep trainer uses.
    auto sameLayout = [](const Entry& x, const Entry& y) {
        if (x.regions.size() != y.regions.size())
            return false;
        for (size_t i = 0; i < x.regions.size(); ++i)
            if (x.regions[i].base != y.regions[i].base ||
                x.regions[i].bytes.size() != y.regions[i].bytes.size())
                return false;
        return true;
    };
    if (!sameLayout(*a, *b) || (c && !sameLayout(*b, *c))) {
        res.error = "snapshots cover different memory layouts — recapture them together";
        return res;
    }

    const uint32_t minRun = opts.minRun == 0 ? 1 : opts.minRun;
    const size_t limit = opts.limit <= 0 ? SIZE_MAX : static_cast<size_t>(opts.limit);

    auto emit = [&](const SnapshotRegion& ra, const SnapshotRegion& rb, uint32_t startIdx,
                    uint32_t endIdx) {
        uint32_t len = endIdx - startIdx;
        if (len < minRun || (opts.maxRun != 0 && len > opts.maxRun))
            return;
        ++res.totalRuns;
        res.bytesMatching += len;
        if (res.runs.size() >= limit) {
            res.truncated = true;
            return;
        }
        MemoryDiffRun run;
        run.addr = ra.base + startIdx;
        run.len = len;
        if (opts.includeBytes) {
            run.before.assign(ra.bytes.begin() + startIdx, ra.bytes.begin() + endIdx);
            run.after.assign(rb.bytes.begin() + startIdx, rb.bytes.begin() + endIdx);
        }
        res.runs.push_back(std::move(run));
    };

    for (size_t ri = 0; ri < a->regions.size(); ++ri) {
        const SnapshotRegion& ra = a->regions[ri];
        const SnapshotRegion& rb = b->regions[ri];
        const SnapshotRegion* rc = c ? &c->regions[ri] : nullptr;
        const uint32_t n = static_cast<uint32_t>(ra.bytes.size());

        uint32_t runStart = 0;
        bool inRun = false;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t addr = ra.base + i;
            bool qualifies =
                opts.wantSame ? (ra.bytes[i] == rb.bytes[i]) : (ra.bytes[i] != rb.bytes[i]);
            if (qualifies && rc)
                qualifies = rb.bytes[i] == rc->bytes[i];
            if (qualifies && !opts.exclude.empty() && InAnyRange(addr, opts.exclude))
                qualifies = false;

            if (qualifies && !inRun) {
                runStart = i;
                inRun = true;
            } else if (!qualifies && inRun) {
                emit(ra, rb, runStart, i);
                inRun = false;
            }
        }
        if (inRun)
            emit(ra, rb, runStart, n);
    }

    res.ok = true;
    return res;
}

} // namespace adm::core
