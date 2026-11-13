#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace adm::core {

/**
 * @brief Reconstructs the inlined-function stack at any link-time address by
 *        parsing DWARF `.debug_info` (subprograms + inlined subroutines).
 *
 * This is what makes a flame chart match Bartman's tool on an LTO build: with
 * heavy inlining there are no call instructions for the inlined functions, so a
 * BSR/JSR call-stack reconstruction can't see them. DWARF records the nesting
 * directly (DW_TAG_inlined_subroutine), and this reader turns it into a per-PC
 * stack: e.g. voxel_c2p_kick -> voxel_c2p_pass2 -> system_wait_blt.
 *
 * Targets the DWARF 5 subset the bartman m68k-amiga-elf toolchain emits: direct
 * forms (DW_FORM_strp/addr/ref_addr), contiguous ranges (low_pc + high_pc) and
 * DW_AT_ranges into `.debug_rnglists`. Indexed forms (strx/addrx) are absent for
 * this toolchain. Dependency-free and decoupled from the emulator.
 */
class DebugInline
{
public:
    bool LoadFromElf(const std::string& path);
    bool LoadFromElf(std::span<const uint8_t> data);
    void Clear();

    [[nodiscard]] bool IsLoaded() const { return !m_byDepth.empty(); }
    [[nodiscard]] size_t ScopeCount() const;

    /**
     * @brief The function + inlined-function stack containing `linkAddr`.
     * @return Names from OUTERMOST (the real function, depth 0) to innermost
     *         inlined function. Empty if the address is not covered.
     */
    [[nodiscard]] std::vector<std::string> ResolveInlineStack(uint32_t linkAddr) const;

    /**
     * @brief The same stack, with each level's call site.
     *
     * A level's `callFile`/`callLine` say where in its *caller* the inlining happened, which
     * is what a backtrace should show for the caller's frame. Without it every frame in an
     * inlined chain reports the innermost body's line, so the same file:line appears several
     * times and never says where the call was made. `stmtList` identifies the compilation
     * unit's line program, since DW_AT_call_file indexes that unit's file table -- resolve it
     * with DebugLines::ResolveUnitFile.
     */
    struct InlineLevel
    {
        std::string name;
        uint64_t stmtList = 0; // CU's DW_AT_stmt_list, for resolving callFile
        uint64_t callFile = 0; // index into that unit's file table
        uint32_t callLine = 0;
        bool haveCallSite = false;
    };
    [[nodiscard]] std::vector<InlineLevel> ResolveInlineLevels(uint32_t linkAddr) const;

private:
    // A [lo, hi) PC interval at a fixed inline depth, naming its function.
    struct Interval
    {
        uint32_t lo;
        uint32_t hi;
        uint32_t nameId;
        uint64_t stmtList;
        uint64_t callFile;
        uint32_t callLine;
        bool haveCallSite;
    };

    // Intervals grouped by inline depth (0 = enclosing function). Within a depth
    // they are sorted by lo and non-overlapping, so lookup is a binary search.
    std::vector<std::vector<Interval>> m_byDepth;
    std::vector<std::string> m_names; // nameId -> function name
};

} // namespace adm::core
