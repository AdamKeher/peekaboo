#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace adm {
namespace core {

/**
 * @brief Boot block identification result.
 */
struct BootBlockInfo
{
    std::string name;        // Name of the bootblock
    std::string classCode;   // Short class code (s, u, v, g, etc.)
    std::string className;   // Full class name (Standard, Utility, Virus, etc.)
    std::string notes;       // Additional notes
    std::string kickstart;   // Kickstart version requirement
    bool bootable = false;   // Whether the bootblock is bootable
    std::string crc32;       // CRC32 hash (if matched by CRC)
    std::string file;        // Relative path to the .bin in the library (if known)
    bool identified = false; // Whether the bootblock was identified
    bool isVirus = false;    // Convenience flag for virus class
};

/**
 * @brief Bootblock identification using XCopy brainfile database.
 *
 * Loads the brainfile.json and provides identification of Amiga bootblocks
 * via CRC32 matching or recognition pattern matching.
 */
class BrainFile
{
public:
    BrainFile();

    /**
     * @brief Load the brainfile database from path.
     * @param path Path to brainfile.json
     * @return true if loaded successfully
     */
    bool LoadDatabase(const std::string& path);

    /**
     * @brief Save the brainfile database to path.
     * @param path Path to brainfile.json (defaults to loaded path if empty)
     * @return true if saved successfully
     */
    bool SaveDatabase(const std::string& path = "");

    /**
     * @brief Add a new entry to the database.
     * @param info BootBlockInfo containing the details
     * @return true if added (false if CRC collision or invalid)
     */
    bool AddEntry(const BootBlockInfo& info);

    /**
     * @brief Check if database is loaded.
     */
    bool IsLoaded() const { return m_loaded; }

    /**
     * @brief Get the number of entries in the database.
     */
    size_t GetEntryCount() const { return m_entries.size(); }

    /**
     * @brief Identify a bootblock using both sectors (blocks 0 and 1).
     * @param sector0 First 512 bytes (block 0)
     * @param sector1 Second 512 bytes (block 1)
     * @return BootBlockInfo with identification results
     */
    BootBlockInfo IdentifyBootBlock(const std::vector<uint8_t>& sector0,
                                    const std::vector<uint8_t>& sector1) const;

    /**
     * @brief Look up an entry's metadata by its (brainfile) Name.
     * @param name The bootblock name (matches a library entry's file stem after sync).
     * @return BootBlockInfo populated from the entry; identified=false if not found.
     */
    BootBlockInfo FindByName(const std::string& name) const;

    /**
     * @brief Derive the Amiga machine type from a Kickstart requirement string.
     * @param kickstart e.g. "KS1.3", "KS2.04", "KS3.0".
     * @return "A500", "A600", "A1200", or "Any" when unknown/empty.
     */
    static std::string GetMachineType(const std::string& kickstart);

    /**
     * @brief Get the full class name for a class code.
     * @param classCode Short class code (s, u, v, etc.)
     * @return Full class name
     */
    static std::string GetClassName(const std::string& classCode);

    /**
     * @brief Get the ImGui color for a boot block class.
     * @param classCode Short class code
     * @return ImU32 color value
     */
    static uint32_t GetClassColor(const std::string& classCode);

private:
    struct BrainEntry
    {
        std::string name;
        std::string recog;
        std::string classCode;
        std::string notes;
        std::string kickstart;
        std::string crc32;
        std::string file;
        bool bootable = false;
    };

    /**
     * @brief Calculate CRC32 for boot block data (both sectors).
     */
    static std::string CalculateCRC32(const std::vector<uint8_t>& sector0,
                                      const std::vector<uint8_t>& sector1);

    /**
     * @brief Check if bootblock matches a recognition pattern.
     * @param recog Recognition pattern string (offset,value pairs)
     * @param sector0 First sector data
     * @param sector1 Second sector data
     * @return true if pattern matches
     */
    static bool MatchRecognitionPattern(const std::string& recog,
                                        const std::vector<uint8_t>& sector0,
                                        const std::vector<uint8_t>& sector1);

    std::vector<BrainEntry> m_entries;
    bool m_loaded = false;
    std::string m_loadedPath;
};

} // namespace core
} // namespace adm
