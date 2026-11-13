#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ExecIntrospect.h"

namespace adm::core {

// A memory region as the describer sees it (mirrors ArMemoryAccess::Region and
// AmigaEmulator::MemoryRegion without depending on either).
struct AddressRegion
{
    uint32_t base = 0;
    uint32_t size = 0;
    std::string name;
    bool writable = false;
};

// One thing an address turns out to be. An address is usually several at once —
// a byte can be inside chip RAM, inside a task's stack and inside a free chunk —
// so a description is a list, ordered from the most concrete answer outwards.
struct AddressLayer
{
    std::string kind;    // "region" | "customRegister" | "cia" | "vector" | "segment" |
                         // "function" | "execBase" | "memoryHeader" | "allocation" |
                         // "taskStack" | "execNode" | "libraryVector" | "annotation"
    std::string name;    // "COLOR00", "Level 3 Int", "dos.library", ...
    std::string detail;  // free-text elaboration; may be empty
    uint32_t base = 0;   // start of the containing thing (0 when not applicable)
    uint32_t size = 0;   // its length (0 when unknown)
    uint32_t offset = 0; // addr - base
};

struct AddressDescription
{
    uint32_t addr = 0;
    bool mapped = false;
    std::vector<AddressLayer> layers;
};

/**
 * @brief Explains what a raw Amiga address actually is.
 *
 * Every other tool here produces addresses; this is what turns them back into
 * meaning without a manual hunt through the memory map, TASKS and AVAIL. It is
 * read-only and side-effect free, and reads through injected accessors so it can
 * be unit-tested against a synthetic image.
 */
class AddressDescriber
{
public:
    struct Context
    {
        exec::Readers readers;
        std::vector<AddressRegion> regions;

        // Custom-chip register name/description for a byte offset from $DFF000.
        // Either may be null.
        std::function<std::string(uint32_t offset)> customRegName;
        std::function<std::string(uint32_t offset)> customRegDesc;

        // Resolve a link-time address to a function. Returns false when unknown.
        std::function<bool(uint32_t linkAddr, std::string& outName, uint32_t& outBase,
                           uint32_t& outSize)>
            resolveSymbol;

        // Annotation text at an exact address; either may be null / return empty.
        std::function<std::string(uint32_t addr)> annotationLabel;
        std::function<std::string(uint32_t addr)> annotationComment;

        // Set false to skip every AmigaOS walk (no OS booted, or a bootblock that
        // has already torn Exec down).
        bool inspectOs = true;
    };

    static AddressDescription Describe(uint32_t addr, const Context& ctx);

    // Exposed for testing and for reuse by the library tracer.
    // Decodes a CIA register address into chip ('A'/'B') and register name.
    // Returns false when addr is not a CIA register.
    static bool DescribeCia(uint32_t addr, char& outChip, std::string& outReg, uint32_t& outIndex);

    // 68k vector name for vector number `num`, including TRAPs and the Amiga's
    // autovector range. Never empty.
    static std::string VectorName(int num);
};

} // namespace adm::core
