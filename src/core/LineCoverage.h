#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "CpuTraceHistory.h"
#include "DebugLines.h"

namespace adm::core {

class DebugInline;
class ElfHunkMap;

/// One stretch of code a source line produced, and what the CPU actually did with it.
struct CoveredSite
{
    uint32_t lo = 0;        // link-time
    uint32_t hi = 0;        // link-time, exclusive
    uint32_t runtimeLo = 0; // 0 when the program is not relocated
    uint32_t runtimeHi = 0;
    bool isStmt = false;
    uint64_t hits = 0; // instructions executed inside this stretch

    // Where this stretch came from, when the line was inlined. Outermost first, so the last
    // entry is the function the source line is written in. Empty when not inlined.
    std::vector<std::string> inlineStack;

    // The first and last time it ran, and the register file at the first hit. On optimised
    // code this is usually the answer wanted: not "did it run" but "with what".
    bool haveSample = false;
    uint64_t firstSeq = 0;
    uint64_t lastSeq = 0;
    uint64_t firstFrame = 0;
    uint32_t firstLRef = 0;
    uint32_t d[8] = {};
    uint32_t a[8] = {};
    uint16_t sr = 0;
};

struct LineCoverageResult
{
    bool available = false;
    std::string hint; // why it is unavailable, or a caveat worth reading

    LineLookupStatus status = LineLookupStatus::NoCode;
    uint32_t requestedLine = 0;
    uint32_t resolvedLine = 0;
    bool relocated = false;

    uint64_t totalHits = 0;
    std::vector<CoveredSite> sites; // every stretch, executed or not
    size_t executedSites = 0;
};

/**
 * @brief Answers "did this source line run, where, and with what?" from the recorded CPU trace.
 *
 * This is the counterpart to a breakpoint, and on a `-Ofast -flto -fwhole-program` build it is
 * frequently the only thing that can answer at all: most lines generate no code of their own to
 * break on, and those that do are often scattered across several inlined or unrolled copies, so a
 * single breakpoint address covers only part of the line. Asking the trace afterwards sidesteps
 * both problems -- there is nothing to place, and every copy is visible.
 *
 * It reports sites with zero hits as well as executed ones. "This line ran, but only two of its
 * four copies did" is usually the interesting answer, and it is invisible if unexecuted stretches
 * are dropped.
 */
class LineCoverage
{
public:
    struct Inputs
    {
        const DebugLines* lines = nullptr;
        const DebugInline* inlines = nullptr; // optional: names the inline chain
        const ElfHunkMap* hunkMap = nullptr;  // optional: adds runtime addresses
        const CpuTraceHistory* history = nullptr;
    };

    /**
     * @param file    source path, matched by trailing components
     * @param line    1-based source line
     * @param frame   only count hits in this emulated frame; 0 means every recorded frame
     */
    static LineCoverageResult Query(const Inputs& in, const std::string& file, uint32_t line,
                                    uint64_t frame = 0);
};

} // namespace adm::core
