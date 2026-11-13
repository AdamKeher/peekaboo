#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "SegListResolver.h" // AmigaSegment

namespace adm::core {

class DebugFrame;
class DebugInline;
class DebugLines;
class DebugSymbols;
class ElfHunkMap;

/**
 * @brief Everything needed to talk about the running program in source terms, gathered
 *        in one place so it can be handed to the debugger views as a single value.
 *
 * The readers are non-owning pointers into whichever debug info is active (the
 * compile-and-run target's, or the active document's sibling .elf); `segments` and
 * `hunkMap` describe where that program actually landed in memory this run.
 *
 * The address translation lives here because every consumer needs it and getting it
 * wrong is silent: DWARF talks in link-time addresses, the emulator in runtime ones.
 */
struct SourceDebugContext
{
    const DebugSymbols* symbols = nullptr;
    const DebugLines* lines = nullptr;
    const DebugInline* inlines = nullptr;
    const DebugFrame* frames = nullptr;

    /// Exact per-section mapping, when the ELF could be paired with its hunks and the
    /// seglist walked. Preferred over `segments`: a bartman build has several hunks
    /// allocated independently, two of them far away in chip RAM.
    const ElfHunkMap* hunkMap = nullptr;

    /// Fallback from walking the AmigaDOS seglist. segments[0] is the code hunk.
    std::vector<AmigaSegment> segments;

    [[nodiscard]] bool HasSymbols() const { return symbols != nullptr; }
    [[nodiscard]] bool CanUnwind() const;

    /// Runtime address -> link-time address, or nullopt when the address is not part of
    /// the loaded program (ROM, a library, unallocated space).
    [[nodiscard]] std::optional<uint32_t> ToLink(uint32_t runtimeAddr) const;

    /// The reverse. Uses the hunk map when available, else the code hunk's base.
    [[nodiscard]] std::optional<uint32_t> ToRuntime(uint32_t linkAddr) const;
};

} // namespace adm::core
