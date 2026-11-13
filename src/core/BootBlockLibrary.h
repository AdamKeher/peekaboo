#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace adm::core {

/**
 * @brief A single saved boot block in the library.
 */
struct BootBlockEntry
{
    std::string name; // Display name (file stem, without extension)
    std::string path; // Absolute path to the .bin file
};

/**
 * @brief Persistent collection of custom boot blocks.
 *
 * Entries are stored in a `bootblocks/` folder as raw 1024-byte dumps of the two
 * boot block sectors (block 0 followed by block 1), named `<name>.bin`. The folder
 * is resolved like the other asset folders (e.g. `tosec/`): relative to the
 * working directory, falling back to the parent directory. The brainfile
 * database (`brainfile.json`) lives in this same folder.
 */
class BootBlockLibrary
{
public:
    /**
     * @brief Locate the bootblocks/ folder and enumerate entries.
     * @return true if the folder was found (an empty/absent folder still returns true).
     */
    bool Load();

    /**
     * @brief Get the current list of entries (sorted by name).
     */
    const std::vector<BootBlockEntry>& Entries() const { return m_entries; }

    /**
     * @brief The resolved bootblocks/ directory (valid after Load()).
     */
    const std::filesystem::path& Directory() const { return m_dir; }

    /**
     * @brief Save a boot block to the library as <name>.bin (1024 bytes).
     * @param name Display name (will be sanitized for the filesystem).
     * @param sector0 First 512 bytes (block 0).
     * @param sector1 Second 512 bytes (block 1).
     * @return true on success.
     */
    bool Save(const std::string& name, const std::vector<uint8_t>& sector0,
              const std::vector<uint8_t>& sector1);

    /**
     * @brief Rename an existing entry.
     * @return true on success.
     */
    bool Rename(const std::string& oldName, const std::string& newName);

    /**
     * @brief Delete an entry.
     * @return true on success.
     */
    bool Delete(const std::string& name);

    /**
     * @brief Read the 1024 bytes of an entry.
     * @return The data, or empty if not found / unreadable.
     */
    std::vector<uint8_t> Read(const std::string& name) const;

    /**
     * @brief Sanitize a display name into a safe file stem.
     */
    static std::string SanitizeName(const std::string& name);

private:
    std::filesystem::path EnsureDir();
    void Refresh();

    std::filesystem::path m_dir;
    std::vector<BootBlockEntry> m_entries;
};

} // namespace adm::core
