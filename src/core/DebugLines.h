#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace adm::core {

/// Result of resolving a link-time address to a source location.
struct SourceLine
{
    std::string file; // resolved path (or basename if the producer gave no directory)
    uint32_t line = 0;
};

/// One contiguous stretch of machine code a source line produced.
struct LineSite
{
    uint32_t lo = 0;   // link-time address, inclusive
    uint32_t hi = 0;   // link-time address, exclusive
    uint32_t line = 0; // the line this row actually carries (may differ from the one asked for)
    uint32_t fileId = 0;
    /// DWARF is_stmt: this address is a recommended breakpoint location. A line can also
    /// generate non-statement rows -- the middle of an expression, or code hoisted from
    /// elsewhere -- which are real code but poor places to stop.
    bool isStmt = false;
};

/// What ResolveLineToAddresses found. The distinction between these matters: telling a user
/// "no code" when the file simply was not parsed is a lie, and so is silently moving a
/// breakpoint to a different line.
enum class LineLookupStatus {
    Exact,          // the requested line generated code
    MovedForward,   // it did not, but a later line within the search window did
    NoCode,         // the file is covered and the line genuinely produced nothing
    FileUnknown,    // no such file in this ELF's line table
    FileNotCovered, // the file exists but its compilation unit was skipped (see DebugLines docs)
};

struct LineLookup
{
    LineLookupStatus status = LineLookupStatus::NoCode;
    uint32_t requestedLine = 0;
    uint32_t resolvedLine = 0;         // equals requestedLine unless status is MovedForward
    std::vector<LineSite> sites;       // ascending by lo; empty unless Exact or MovedForward
    std::vector<uint32_t> nearbyLines; // lines with code near the request, for a useful message
};

/**
 * @brief Reads the DWARF line-number program (.debug_line) from an ELF and maps
 *        link-time addresses to source file:line.
 *
 * Stage 5 of the source-level profiler: line-level granularity on top of the
 * function-level `DebugSymbols`. Parses DWARF version 5 specifically (what the
 * bartman m68k-amiga-elf toolchain emits) — the v5 line-program header differs
 * materially from v2-4 (form-encoded directory/file tables), so this targets v5
 * rather than being a general DWARF reader.
 *
 * Like DebugSymbols it is dependency-free and decoupled from the emulator: it
 * flattens every compilation unit's line program into one address-sorted table
 * and resolves by binary search.
 */
class DebugLines
{
public:
    bool LoadFromElf(const std::string& path);
    bool LoadFromElf(std::span<const uint8_t> data);
    void Clear();

    [[nodiscard]] bool IsLoaded() const { return !m_rows.empty(); }
    [[nodiscard]] size_t RowCount() const { return m_rows.size(); }

    /**
     * @brief Resolve a link-time address to its source file:line.
     * @param linkAddr Address after the segment load base has been subtracted.
     * @return The source location, or std::nullopt if the address is not covered
     *         by any line-program row (or falls in an end-of-sequence gap).
     */
    [[nodiscard]] std::optional<SourceLine> ResolveLine(uint32_t linkAddr) const;

    /**
     * @brief The reverse direction: every address range a source line generated.
     *
     * This is the half GDB gets wrong on -Ofast -flto builds -- not through any fault of its
     * own, but because the reverse mapping genuinely is ambiguous once a line has been inlined
     * into several callers or folded away entirely. Peekaboo does not place breakpoints with
     * this; GDB does. This exists to *audit* GDB's answer and explain it, which is why the
     * statuses distinguish "no code" from "file not covered".
     *
     * @param file          source path as the editor knows it; matched by trailing components
     * @param line          1-based source line
     * @param stmtOnly      prefer is_stmt sites, which are the recommended breakpoint locations
     * @param forwardSearch how many lines past the request to look when it produced nothing
     */
    [[nodiscard]] LineLookup ResolveLineToAddresses(std::string_view file, uint32_t line,
                                                    bool stmtOnly = true,
                                                    uint32_t forwardSearch = 25) const;

    /// Every line in the file that generated code, ascending. Empty if the file is unknown.
    [[nodiscard]] std::vector<uint32_t> LinesWithCode(std::string_view file) const;

    /**
     * @brief Resolve a DWARF file index against one compilation unit's file table.
     *
     * `DW_AT_call_file` on an inlined subroutine indexes the file table of its CU's line
     * program, reached through the CU's `DW_AT_stmt_list`. The flattened global table cannot
     * answer that, so the per-unit tables are kept alongside it.
     */
    [[nodiscard]] std::optional<std::string> ResolveUnitFile(uint64_t stmtListOffset,
                                                             uint64_t fileIndex) const;

    /// Source paths this line table covers.
    [[nodiscard]] const std::vector<std::string>& Files() const { return m_files; }

    /**
     * @brief Score how well a DWARF path matches a path the editor supplied.
     *
     * DWARF carries whatever the compiler saw ("../engine/foo.c", mixed separators); VS Code
     * sends an absolute Windows path. Comparing them naively is what produces the classic
     * "breakpoint never binds and nobody says why". Returns the number of matching trailing
     * path components, or -1 for no match, so an ambiguous match can be reported rather than
     * guessed at.
     */
    [[nodiscard]] static int PathScore(std::string_view dwarfPath, std::string_view queryPath);

private:
    // One row of the flattened line-number matrix.
    struct Row
    {
        uint32_t address; // link-time address the row starts at
        uint32_t line;    // 1-based source line (0 = unknown)
        uint32_t fileId;  // index into m_files
        bool endSequence; // true => address is one-past-end of a code range
        bool isStmt;      // DWARF is_stmt: a recommended breakpoint location
        // Which line-program sequence this row came from. A row's extent ends at the next row
        // *in the same sequence*: rows from different sequences interleave freely once every
        // compilation unit is flattened into one address-sorted table, so using "the next row"
        // without checking this produces ranges that span unrelated code.
        uint32_t seq;
    };

    // Parse a single compilation unit's line program starting at unitOff; returns
    // the offset of the next unit (or data.size() to stop). Appends to m_rows.
    size_t ParseUnit(std::span<const uint8_t> data, size_t unitOff,
                     std::span<const uint8_t> debugStr, std::span<const uint8_t> debugLineStr,
                     bool& ok);

    // Intern a resolved file path, returning its id in m_files.
    uint32_t InternFile(const std::string& path);

    // Find the end of the row's extent: the next row in the same sequence.
    [[nodiscard]] uint32_t RowEnd(size_t index) const;
    // Resolve a caller-supplied path to a fileId, or nullopt. Sets ambiguous when several
    // files match equally well.
    [[nodiscard]] std::optional<uint32_t> MatchFile(std::string_view path, bool* ambiguous) const;

    std::vector<Row> m_rows;                               // sorted ascending by address
    std::vector<std::string> m_files;                      // fileId -> resolved path
    std::unordered_map<std::string, uint32_t> m_fileIndex; // resolved path -> fileId
    // stmt_list offset -> that unit's local file index -> global fileId.
    std::map<size_t, std::vector<uint32_t>> m_unitFiles;
    uint32_t m_nextSeq = 0;
};

} // namespace adm::core
