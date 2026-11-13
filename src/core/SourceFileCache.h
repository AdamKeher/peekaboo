#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace adm::core {

/// Why a DWARF path could not be turned into readable source text.
enum class SourceFileStatus : uint8_t {
    Ok,         ///< located and read
    NotFound,   ///< no probe resolved to an existing regular file
    ReadFailed, ///< resolved, but open/read failed (permissions, vanished mid-session)
    TooLarge,   ///< above the byte limit; refused rather than stalling the UI thread
    Binary,     ///< a NUL appears in the first 4 KiB — this is not source text
    Pseudo,     ///< a DWARF pseudo-name like <artificial> or <built-in>; no file exists
};

/// One source file, split into lines, plus the story of how it was found.
struct SourceFile
{
    SourceFileStatus status = SourceFileStatus::NotFound;
    std::string dwarfPath;          ///< the cache key: exactly what DWARF said
    std::string resolvedPath;       ///< local path actually opened (empty unless Ok)
    std::vector<std::string> lines; ///< 1-based: line N is lines[N-1]; tabs expanded, no CR
    uint64_t sizeBytes = 0;
    /// Bumped every time this entry is (re-)populated, so a caller can cache derived data
    /// (tokenized spans, lexer states) against it without comparing strings.
    uint32_t version = 0;

    [[nodiscard]] bool Ok() const { return status == SourceFileStatus::Ok; }
    [[nodiscard]] uint32_t LineCount() const { return static_cast<uint32_t>(lines.size()); }
    /// 1-based. Empty view for line 0 or past end of file — never throws, never asserts.
    [[nodiscard]] std::string_view Line(uint32_t oneBased) const;
};

/**
 * @brief Finds the C/C++ files a DWARF line table names, and hands back their text.
 *
 * DWARF records whatever path the compiler saw. On a cross-compiled Amiga build that is
 * routinely a path that does not exist on this machine — a MinGW/MSYS "/c/dev/..." form,
 * a relative "../engine/foo.c" anchored at a build directory that has since moved, or an
 * absolute path out of someone else's checkout. Rather than guess, this probes a fixed,
 * documented ladder of candidates and can report which rung answered, so a file that
 * fails to resolve can be explained instead of silently disappearing.
 *
 * Lazily populated and negatively cached: a path that resolves to nothing is probed once,
 * not once per frame. Nothing here watches the filesystem — staleness is the caller's
 * call (see Forget / RetryFailed / Clear).
 */
class SourceFileCache
{
public:
    static constexpr uint64_t kDefaultMaxFileBytes = 4ull * 1024 * 1024;
    static constexpr int kMaxSuffixComponents = 6; ///< longest trailing run we try to match
    static constexpr int kMaxWalkUpLevels = 6;     ///< how far above the .elf dir we look
    static constexpr int kMaxLineLength = 512;     ///< longer lines are elided with "..."

    // --- configuration (each setter is a no-op when the value is unchanged) ---

    /// Path of the .elf the DWARF came from. Its directory anchors probes 2 and 3.
    /// Changing it clears the cache.
    void SetElfPath(std::string elfPath);
    [[nodiscard]] const std::string& ElfPath() const { return m_elfPath; }

    /// User-configured search roots, tried last, in the order given. Changing them
    /// re-probes previously failed entries (see RetryFailed).
    void SetSearchRoots(std::vector<std::string> roots);
    [[nodiscard]] const std::vector<std::string>& SearchRoots() const { return m_searchRoots; }

    /// Tab stop used when expanding '\t' at read time. Default 4.
    void SetTabWidth(int width);
    [[nodiscard]] int TabWidth() const { return m_tabWidth; }

    /// Refuse files above this size. Default kDefaultMaxFileBytes.
    void SetMaxFileBytes(uint64_t bytes);

    // --- lookup ---

    /// True for a DWARF pseudo-filename — <artificial>, <built-in> and friends. The
    /// compiler emits these for code it generated itself, so there is nothing on disk to
    /// find; probing for them would fail slowly and then report a missing file that was
    /// never supposed to exist.
    [[nodiscard]] static bool IsPseudoPath(std::string_view dwarfPath);

    /// Resolve + read, cached both ways. The reference stays valid until the next
    /// Clear/Forget/RetryFailed. Check .Ok() before using .lines.
    const SourceFile& Get(const std::string& dwarfPath);

    /// Convenience: 1-based line text, or nullopt when the file or the line is unavailable.
    std::optional<std::string_view> GetLine(const std::string& dwarfPath, uint32_t line);

    /// Path resolution only — no read, no caching. For the diagnostics table and tests.
    [[nodiscard]] std::optional<std::string> ResolvePath(const std::string& dwarfPath) const;

    /// The candidates ResolvePath would try, in order. Powers the "why did this not
    /// resolve?" tooltip.
    [[nodiscard]] std::vector<std::string> ProbeCandidates(const std::string& dwarfPath) const;

    // --- invalidation ---

    void Clear();
    void Forget(const std::string& dwarfPath); ///< drop one entry ("Reload source file")
    /// Re-probe entries that previously failed, keeping successful reads. Successes are
    /// deliberately sticky: a newly added root should not silently re-point a file the
    /// user is already reading.
    void RetryFailed();

    // --- diagnostics ---

    struct Stats
    {
        size_t entries = 0;  ///< distinct DWARF paths probed
        size_t resolved = 0; ///< of those, how many read successfully
        uint64_t bytes = 0;  ///< total source text held
    };
    [[nodiscard]] Stats GetStats() const;

private:
    std::string m_elfPath;
    std::string m_elfDir; // cached parent directory of m_elfPath
    std::vector<std::string> m_searchRoots;
    int m_tabWidth = 4;
    uint64_t m_maxFileBytes = kDefaultMaxFileBytes;
    uint32_t m_nextVersion = 1;
    std::unordered_map<std::string, SourceFile> m_files;

    SourceFile Load(const std::string& dwarfPath) const;
    [[nodiscard]] std::vector<std::string> SplitText(std::string_view raw) const;
};

} // namespace adm::core
