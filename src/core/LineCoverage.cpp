#include "LineCoverage.h"

#include "DebugInline.h"
#include "ElfHunkMap.h"

namespace adm::core {

LineCoverageResult LineCoverage::Query(const Inputs& in, const std::string& file, uint32_t line,
                                       uint64_t frame)
{
    LineCoverageResult out;
    out.requestedLine = line;
    out.resolvedLine = line;

    if (!in.lines || !in.lines->IsLoaded()) {
        out.hint = "no debug line table is loaded; build with -g and launch through Peekaboo's "
                   "VS Code target";
        return out;
    }

    // stmtOnly=false: coverage wants every address the line touched, not only the locations the
    // compiler recommends stopping at. A non-statement stretch still executed.
    const LineLookup lookup = in.lines->ResolveLineToAddresses(file, line, /*stmtOnly=*/false);
    out.status = lookup.status;
    out.resolvedLine = lookup.resolvedLine;
    if (lookup.sites.empty()) {
        out.available = true; // the question was answered: this line produced no code
        return out;
    }

    out.relocated = in.hunkMap && in.hunkMap->IsRelocated();
    for (const LineSite& site : lookup.sites) {
        CoveredSite cs;
        cs.lo = site.lo;
        cs.hi = site.hi;
        cs.isStmt = site.isStmt;
        if (out.relocated) {
            if (auto rl = in.hunkMap->ToRuntime(site.lo))
                cs.runtimeLo = *rl;
            // hi is exclusive, so relocate the last byte and step past it.
            if (auto rh = in.hunkMap->ToRuntime(site.hi - 1))
                cs.runtimeHi = *rh + 1;
        }
        if (in.inlines) {
            auto chain = in.inlines->ResolveInlineStack(site.lo);
            if (chain.size() > 1)
                cs.inlineStack = std::move(chain);
        }
        out.sites.push_back(std::move(cs));
    }

    if (!in.history) {
        out.available = true;
        out.hint = "addresses only: no CPU trace history, so hit counts are unavailable";
        return out;
    }

    // Hit counting needs runtime addresses: the trace records the PC the CPU actually executed.
    // Without relocation the link-time ranges would match nothing, and reporting zero hits would
    // read as "this line never ran" when the truth is "we could not tell".
    if (!out.relocated) {
        out.available = true;
        out.hint = "addresses only: the program is not relocated, so trace PCs cannot be matched "
                   "to these link-time ranges";
        return out;
    }

    bool sawTrace = false;
    for (CoveredSite& site : out.sites) {
        if (site.runtimeHi <= site.runtimeLo)
            continue;

        CpuTraceQuery q;
        q.mode = TraceQueryMode::Filter;
        q.havePcRange = true;
        q.pcLo = site.runtimeLo;
        q.pcHi = site.runtimeHi;
        q.haveFrame = frame != 0;
        q.frame = frame;
        q.limit = 0; // count everything; totalMatches is the number wanted
        q.newestFirst = false;

        const CpuTraceSnapshot snap = in.history->Query(q);
        if (!snap.available) {
            out.hint = snap.hint.empty()
                           ? "CPU trace history is unavailable; enable it to get hit counts"
                           : snap.hint;
            continue;
        }
        sawTrace = true;
        site.hits = snap.totalMatches;
        out.totalHits += site.hits;
        if (site.hits > 0)
            ++out.executedSites;

        // Keep the first hit's registers: on optimised code "with what values" is usually the
        // real question, and the first execution is the one a user is normally reasoning about.
        if (!snap.matches.empty()) {
            const CpuTraceRecord& first = snap.matches.front().record;
            site.haveSample = true;
            site.firstSeq = first.seq;
            site.firstFrame = first.frame;
            site.firstLRef = first.lRef;
            site.lastSeq = snap.matches.back().record.seq;
            site.sr = first.sr;
            for (int i = 0; i < 8; ++i) {
                site.d[i] = first.d[i];
                site.a[i] = first.a[i];
            }
        }
    }

    out.available = true;
    if (sawTrace && out.totalHits == 0 && out.hint.empty()) {
        // Distinguish "definitely did not run" from "the ring no longer goes back that far".
        out.hint = "no hits in the recorded window; the trace may not reach back to when this "
                   "line last ran";
    }
    return out;
}

} // namespace adm::core
