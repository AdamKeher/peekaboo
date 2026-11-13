#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adm {
namespace core {

/**
 * @brief Types of Amiga hunks.
 */
enum class HunkType {
    Unknown = 0,
    Header,
    Code,
    Data,
    BSS,
    Reloc32,
    Reloc16,
    Reloc8,
    External,
    Symbol,
    Debug,
    End,
    Overlay,
    Break,
    Unit,
    Name,
    Library,
    Index,
    RelReloc32,
    AbsReloc16
};

/**
 * @brief Information about a single hunk.
 */
struct HunkInfo
{
    HunkType type = HunkType::Unknown;
    std::string typeName;
    uint32_t memoryFlags = 0; // MEMF_CHIP, MEMF_FAST, etc.
    uint32_t size = 0;        // Size in bytes
    uint32_t allocSize = 0;   // Allocated size
    uint32_t dataOffset = 0;  // Offset in file
    int index = 0;            // Hunk index
    std::vector<std::string> symbols;
    std::vector<uint32_t> relocations;
};

/**
 * @brief Type of Amiga executable.
 */
enum class ExecutableType {
    Unknown,
    Executable, // Standard executable (HUNK_HEADER)
    Object,     // Object file (HUNK_UNIT)
    Library     // Library file (HUNK_LIB)
};

/**
 * @brief Resident module type (from RomTag).
 * These are the NT_* node types from exec/nodes.h
 */
enum class ResidentType {
    None = 0,       // No RomTag found
    Task = 1,       // NT_TASK
    Interrupt = 2,  // NT_INTERRUPT
    Device = 3,     // NT_DEVICE
    MsgPort = 4,    // NT_MSGPORT
    Handler = 5,    // NT_HANDLER (filesystem handler)
    SoftInt = 6,    // NT_SOFTINT
    Resource = 8,   // NT_RESOURCE
    Library = 9,    // NT_LIBRARY
    Memory = 10,    // NT_MEMORY
    SoftLink = 11,  // NT_SOFTLINK
    Semaphore = 15, // NT_SEMAPHORE
    Unknown = 255   // RomTag found but unknown type
};

/**
 * @brief Result of parsing an Amiga executable.
 */
struct HunkParseResult
{
    bool success = false;
    std::string errorMessage;
    ExecutableType executableType = ExecutableType::Unknown;
    std::string executableTypeName;
    uint32_t numHunks = 0;
    uint32_t firstLoadHunk = 0;
    uint32_t lastLoadHunk = 0;
    std::vector<HunkInfo> hunks;

    // Statistics
    uint32_t totalCodeSize = 0;
    uint32_t totalDataSize = 0;
    uint32_t totalBSSSize = 0;
    uint32_t totalRelocations = 0;
    uint32_t totalSymbols = 0;
    bool hasChipMem = false;
    bool hasFastMem = false;
};

/**
 * @brief Parser for Amiga Hunk executable format.
 *
 * Wraps the AmigaHunkParser library to provide C++ interface
 * for inspecting Amiga executables, libraries, and object files.
 */
class HunkParser
{
public:
    /**
     * @brief Parse an Amiga executable from raw data.
     * @param data The executable data.
     * @return Parse result with hunk information.
     */
    static HunkParseResult Parse(const std::vector<uint8_t>& data);

    /**
     * @brief Check if data appears to be an Amiga executable.
     * @param data The data to check (at least 4 bytes needed).
     * @return true if it looks like an Amiga executable.
     */
    static bool IsAmigaExecutable(const std::vector<uint8_t>& data);

    /**
     * @brief Get the executable type from header.
     * @param data The data to check.
     * @return The executable type.
     */
    static ExecutableType GetExecutableType(const std::vector<uint8_t>& data);

    /**
     * @brief Get a human-readable name for a hunk type.
     * @param type The hunk type.
     * @return Human-readable name.
     */
    static std::string GetHunkTypeName(HunkType type);

    /**
     * @brief Get memory flags description.
     * @param flags Memory flags value.
     * @return Human-readable description.
     */
    static std::string GetMemoryFlagsString(uint32_t flags);

    /**
     * @brief Detect the resident module type by scanning for RomTag.
     * @param data The executable data.
     * @return The resident type (Library, Device, Handler, etc.) or None if no RomTag found.
     */
    static ResidentType DetectResidentType(const std::vector<uint8_t>& data);

    /**
     * @brief Get a human-readable name for a resident type.
     * @param type The resident type.
     * @return Human-readable name.
     */
    static std::string GetResidentTypeName(ResidentType type);

private:
    static HunkType MapHunkType(uint32_t rawType);
};

} // namespace core
} // namespace adm
