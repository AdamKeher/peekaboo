#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ExocetBlock.h"

namespace adm::core {

// Reader for the memory map a Crass-engine production keeps in a diagnostic build
// (ENGINE_MEMMAP; the crass repo's engine/memmap.h): every
// allocation the disk's programs make, with its size, the program that made it and a name. The
// first program on the disk makes the table in chip RAM and publishes it in its ExoBlock (version
// 2, `memmap`); the programs it runs write the same table.
//
// The table is laid over exec's own view of memory - each MemHeader and its free chunks - so a
// region shows three kinds of bytes: free, allocated by a program the table knows (named), and
// in use by something the table cannot see (exec, trackdisk, the OS).
//
// Big-endian, m68k gcc layout. As with the block itself, every read is validated from scratch.

struct CrassMemMapEntry
{
    uint32_t addr = 0;
    uint32_t size = 0;
    uint16_t line = 0; // the source line that asked for it; 0 for a block entered by hand
    char owner = '?';  // the program: 'P' the player, 'S' the scroller, ...
    uint8_t flags = 0; // kCrassMemMap* flags
    std::string name;  // the given name, or the source file's (then `line` places it)

    bool Chip() const;
    bool Named() const;
    // "anim.bin", or "sneezeman.c:968" for a block with no name of its own
    std::string Label() const;
};

inline constexpr uint32_t kCrassMemMapMagic = 0x4D4D4150u; // "MMAP"
inline constexpr uint16_t kCrassMemMapVersion = 1;
inline constexpr uint32_t kCrassMemMapHeaderSize = 24;
inline constexpr uint32_t kCrassMemMapEntrySize = 32;
inline constexpr uint16_t kCrassMemMapMaxCapacity = 4096;
inline constexpr uint8_t kCrassMemMapChip = 0x01;
inline constexpr uint8_t kCrassMemMapNamed = 0x02;
inline constexpr uint8_t kCrassMemMapAbs = 0x04;
inline constexpr uint8_t kCrassMemMapNoted = 0x08;

struct CrassMemMapTable
{
    bool present = false;
    uint32_t address = 0;
    uint16_t version = 0;
    uint16_t capacity = 0;
    uint16_t count = 0;
    uint16_t peak = 0;    // the most entries there have ever been
    uint16_t dropped = 0; // allocations that found the table full, and are not in it
    uint16_t generation = 0;
    uint32_t changes = 0; // allocations and frees, ever
    // Read while the program was part way through changing it (the machine stopped there): the
    // entries are as they stood, and one of them may be half written.
    bool changing = false;
    std::vector<CrassMemMapEntry> entries; // by address
};

enum class CrassMemMapRead {
    Ok,
    Invalid,  // not a table this reads: `out` is empty
    Changing, // a change was being made (odd generation): `out` is the table as it stood, marked
              // `changing` - a machine stopped mid-change stays that way until it runs again
};

CrassMemMapRead ReadCrassMemMap(const ExocetRead& read, const std::vector<ExocetRam>& ram,
                                uint32_t addr, CrassMemMapTable& out);

// ---- exec's memory, with the table over it ----

struct CrassFreeChunk
{
    uint32_t addr = 0;
    uint32_t bytes = 0;
};

// One exec MemHeader as read: its span, attributes and free chunks.
struct CrassMemHeader
{
    std::string name;
    uint16_t attributes = 0; // MEMF_CHIP 2, MEMF_FAST 4
    uint32_t lower = 0;
    uint32_t upper = 0;
    uint32_t free = 0; // mh_Free
    std::vector<CrassFreeChunk> chunks;
};

struct CrassMemSegment
{
    enum class Kind { Free, Allocated, Untracked };
    uint32_t start = 0;
    uint32_t end = 0; // exclusive
    Kind kind = Kind::Untracked;
    int entry = -1; // into the table's entries, for Allocated
};

struct CrassMemRegion
{
    std::string name;
    bool chip = false;
    uint32_t lower = 0;
    uint32_t upper = 0;
    uint32_t free = 0;        // the free chunks' sum (mh_Free, as exec keeps it)
    uint32_t largestFree = 0; // the biggest single allocation that could succeed
    int freeChunks = 0;
    uint32_t allocated = 0; // bytes the table accounts for
    uint32_t untracked = 0; // bytes in use that no entry holds
    int overlaps = 0;       // entries that overlap a free chunk: a stale or wrong table
    std::vector<CrassMemSegment> segments; // in address order, covering [lower, upper)
};

// Each header's span as free, allocated (by table entry) and untracked segments.
std::vector<CrassMemRegion> LayOutCrassMemory(const std::vector<CrassMemHeader>& headers,
                                              const CrassMemMapTable& table);

// What the Exocet window and get_exocet_memory show: the table and exec's memory laid out.
struct ExocetMemoryState
{
    bool present = false; // a table was found and read
    CrassMemMapTable table;
    std::vector<CrassMemRegion> regions;
    // The programs that write it, as the demo's block names them (ExocetBlockState::owners)
    std::vector<ExocetOwner> owners;
    // A program's name for its owner letter, or the letter itself when the demo names none
    std::string OwnerName(char owner) const
    {
        for (const auto& o : owners)
            if (o.owner == owner)
                return o.name.empty() ? std::string(1, owner) : o.name;
        return std::string(1, owner);
    }
    const ExocetOwner* Owner(char owner) const
    {
        for (const auto& o : owners)
            if (o.owner == owner)
                return &o;
        return nullptr;
    }
};

} // namespace adm::core
