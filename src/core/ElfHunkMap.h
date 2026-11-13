#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "SegListResolver.h"

namespace adm::core {

/// One allocated ELF section, paired with the AmigaDOS hunk it became.
struct MappedSection
{
    std::string name;         // ".text", ".MEMF_CHIP", ...
    uint32_t linkAddr = 0;    // ELF sh_addr (link time)
    uint32_t size = 0;        // ELF sh_size
    bool chip = false;        // the matching hunk asked for MEMF_CHIP
    bool bss = false;         // SHT_NOBITS / HUNK_BSS: occupies no file bytes
    int hunkIndex = -1;       // position in the AmigaDOS hunk table
    uint32_t runtimeBase = 0; // filled in by Relocate()
    bool relocated = false;
};

/**
 * @brief Pairs an ELF's allocated sections with the AmigaDOS hunks elf2hunk produced from them,
 *        then relocates them onto the addresses the program was actually loaded at.
 *
 * This is the piece that lets a real GDB see correct addresses. Bartman's toolchain emits **one
 * hunk per allocated ELF section**, and AmigaDOS allocates each hunk independently -- a typical
 * bartman build has six, two of which are forced into chip RAM far away from the rest. That is
 * why RSP's `qOffsets` (which carries exactly three values, Text/Data/Bss) structurally cannot
 * describe this layout, and why the mechanism is `add-symbol-file` with a `-s <section> <addr>`
 * pair per section.
 *
 * Pairing is positional -- section order matches hunk order -- but positional pairing is an
 * assumption, so Build() *checks* it: every pair must agree on size (to longword rounding) and on
 * chip-vs-any placement. A mismatch fails loudly rather than silently producing a map that would
 * put every symbol at the wrong address.
 */
class ElfHunkMap
{
public:
    /// Read both files from disk and pair them. Returns false with a reason in outError.
    bool Build(const std::string& elfPath, const std::string& exePath, std::string& outError);
    bool BuildFromBytes(std::span<const uint8_t> elf, std::span<const uint8_t> exe,
                        std::string& outError);

    /**
     * @brief Apply the runtime bases from a walked seglist.
     *
     * `segments` comes from SegListResolver::ResolveActiveProcess and must have one entry per
     * mapped section, in the same order.
     */
    bool Relocate(const std::vector<AmigaSegment>& segments, std::string& outError);

    void Clear();

    [[nodiscard]] bool IsLoaded() const { return !m_sections.empty(); }
    [[nodiscard]] bool IsRelocated() const { return m_relocated; }
    [[nodiscard]] const std::vector<MappedSection>& Sections() const { return m_sections; }

    /// `-s .text 0x21b48 -s .rodata 0x29000 ...` for GDB's add-symbol-file. Empty until relocated.
    [[nodiscard]] std::string AddSymbolFileArgs() const;

    /// A human-readable table, for `monitor sections` and the log.
    [[nodiscard]] std::string DescribeSections() const;

    /**
     * @brief `Text=..;Data=..;Bss=..` for RSP qOffsets, or empty when the layout cannot be
     *        expressed that way.
     *
     * Only a courtesy for someone driving gdb by hand; the adapter uses AddSymbolFileArgs(). It
     * returns empty whenever a chip-RAM hunk is present, because collapsing six independently
     * allocated hunks into three offsets would be a lie.
     */
    [[nodiscard]] std::string QOffsetsReply() const;

    /// Translate between link-time (what the DWARF says) and runtime addresses.
    [[nodiscard]] std::optional<uint32_t> ToRuntime(uint32_t linkAddr) const;
    [[nodiscard]] std::optional<uint32_t> ToLink(uint32_t runtimeAddr) const;

private:
    std::vector<MappedSection> m_sections;
    bool m_relocated = false;
};

} // namespace adm::core
