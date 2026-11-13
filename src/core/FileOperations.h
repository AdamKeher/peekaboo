#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "AdfWrapper.h"

// Forward declaration
struct AdfVolume;

namespace adm {
namespace core {

/**
 * @brief Handles file-level operations for Amiga disk images.
 *
 * Provides file tree scanning, block retrieval, and content reading.
 */
class FileOperations
{
public:
    /// Lightweight description of a single directory entry, produced by EnumerateDir.
    struct DirEntry
    {
        uint32_t sector = 0; ///< Header sector of the entry.
        std::string name;    ///< Entry name.
        int32_t secType = 0; ///< ADF secondary type (ADF_ST_FILE / ADF_ST_DIR / ...).
        bool isDir = false;  ///< Convenience flag (secType == ADF_ST_DIR).
    };

    /**
     * @brief Cycle-safe enumeration of a single directory's immediate entries.
     *
     * Reads the directory block directly and walks the hash table / nextSameHash chains with
     * cycle detection, mirroring ScanDirectoryRecursive. Use this instead of adflib's
     * adfGetDirEnt, which hangs on disks with cyclic hash chains.
     *
     * @param vol The mounted volume.
     * @param dirSector The directory's header sector (resolve 0 to the root before calling).
     * @return The directory's immediate children (not recursive).
     */
    static std::vector<DirEntry> EnumerateDir(struct AdfVolume* vol, uint32_t dirSector);

    /**
     * @brief Builds a FileNode from a raw 512-byte entry/header block buffer.
     * @param buf The 512-byte block (big-endian on-disk layout).
     * @param blockNum The sector this block was read from.
     * @param parentSector The sector of the directory that contains this entry.
     * @return A populated FileNode (name, size, access, comment, dates, isDir).
     */
    static FileNode ParseEntryBlock(const uint8_t* buf, uint32_t blockNum, long parentSector);

    /**
     * @brief Loads the entire file structure recursively.
     * @param vol The mounted volume.
     * @return Root node representing the volume root.
     */
    static FileNode GetFileTree(struct AdfVolume* vol);

    /**
     * @brief Retrieves all sectors allocated to a file.
     * @param vol The mounted volume.
     * @param headerBlock The file's header sector number.
     * @return List of sector indices (header, data, and extensions).
     */
    static std::vector<uint32_t> GetFileBlocks(struct AdfVolume* vol, uint32_t headerBlock);

    /**
     * @brief Reads the complete content of a file.
     * @param vol The mounted volume.
     * @param headerBlock The file's header sector number.
     * @param readBlockFn Function to read individual blocks.
     * @return The file content as a string.
     */
    static std::string ReadFileContent(struct AdfVolume* vol, uint32_t headerBlock,
                                       std::function<std::vector<uint8_t>(uint32_t)> readBlockFn);

private:
    static void ScanDirectoryRecursive(struct AdfVolume* vol, long sector,
                                       std::vector<FileNode>& outChildren,
                                       std::set<long>& visitedSectors);
};

} // namespace core
} // namespace adm
