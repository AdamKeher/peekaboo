#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adm::core {

/**
 * @brief A single function symbol taken from an ELF symbol table.
 *
 * Addresses are link-time addresses (the value stored in the ELF, i.e. before
 * the AmigaDOS loader relocates the hunk). To resolve a runtime program counter
 * the caller must first subtract the segment load base.
 */
struct FunctionSymbol
{
    uint32_t address = 0; // st_value: link-time start address
    uint32_t size = 0;    // st_size: byte length (0 if the compiler omitted it)
    std::string name;     // demangled-as-stored name from .strtab
};

/**
 * @brief Reads function symbols from an ELF file's .symtab section.
 *
 * This is Stage 1 of the source-level profiler: it answers "which function is
 * link-time address X in?". It deliberately parses only the ELF section headers
 * and the symbol/string tables — no DWARF — so it is small, dependency-free and
 * unit-testable in isolation. Line-level resolution (.debug_line) is a later
 * stage that layers on top of the same load flow.
 *
 * Endianness is honoured from the ELF header (e_ident[EI_DATA]); Amiga m68k
 * ELFs produced by the bartman toolchain are big-endian, but x86 host ELFs are
 * little-endian, and both parse correctly.
 */
class DebugSymbols
{
public:
    /**
     * @brief Load and parse an ELF file from disk.
     * @param path Filesystem path to the .elf.
     * @return true if the file is a valid ELF32 with a usable .symtab.
     *
     * On success the previous contents are replaced; on failure the object is
     * cleared and IsLoaded() returns false.
     */
    bool LoadFromElf(const std::string& path);

    /**
     * @brief Parse an ELF image already held in memory.
     * @param data The full ELF file bytes.
     */
    bool LoadFromElf(std::span<const uint8_t> data);

    /// Clear all loaded symbols.
    void Clear();

    [[nodiscard]] bool IsLoaded() const { return !m_functions.empty(); }
    [[nodiscard]] size_t FunctionCount() const { return m_functions.size(); }

    /// Functions sorted ascending by link-time address.
    [[nodiscard]] const std::vector<FunctionSymbol>& Functions() const { return m_functions; }

    /// Path this was loaded from (empty for in-memory loads).
    [[nodiscard]] const std::string& SourcePath() const { return m_sourcePath; }

    /**
     * @brief Resolve a link-time address to the enclosing function.
     * @param linkAddr Address after the segment load base has been subtracted.
     * @return The containing function, or nullptr if the address falls in a gap
     *         (before the first function, past a sized function's end, or after
     *         the last function).
     *
     * Functions with a known size are treated as [address, address+size).
     * Functions whose size the compiler left as 0 are treated as extending up to
     * the start of the next function (a standard best-effort heuristic).
     */
    [[nodiscard]] const FunctionSymbol* ResolvePc(uint32_t linkAddr) const;

    /**
     * @brief Find a sibling .elf for a loaded executable.
     * @param executablePath Path to the loaded file (e.g. ".../out/a.exe").
     * @return The path of a matching .elf if one exists, else std::nullopt.
     *
     * Prefers an .elf with the same base name in the same directory
     * (a.exe -> a.elf); if none, falls back to a lone .elf in that directory.
     * Pure filesystem lookup — does not parse the file.
     */
    [[nodiscard]] static std::optional<std::string>
    FindSiblingElf(const std::string& executablePath);

    /**
     * @brief Convenience: locate a sibling .elf for an executable and load it.
     * @return true if a sibling .elf was found and parsed successfully.
     */
    bool LoadForExecutable(const std::string& executablePath);

private:
    std::vector<FunctionSymbol> m_functions; // sorted ascending by address
    std::string m_sourcePath;
};

} // namespace adm::core
