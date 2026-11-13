#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "HostDirectoryDocument.h"

// Forward declaration from adflib (C-struct)
struct AdfVolume;
struct AdfDevice;

// windows.h (via glad) #defines CreateDirectory/MoveFile/DeleteFile to their A/W variants;
// undef here so the method names below are preserved.
#ifdef _WIN32
#undef CreateDirectory
#undef MoveFile
#undef DeleteFile
#endif

namespace adm {
namespace core {
class MemoryDevice;
class GreaseweazelReader;
} // namespace core
} // namespace adm

namespace adm {
namespace core {

struct DateTime
{
    int year, month, day;
    int hour, min, sec;
};

struct FileNode
{
    DocumentEntryId entryId;
    std::string name;
    int32_t size;
    bool isDir;
    bool isDeleted = false;
    DateTime modified;
    std::string comment;
    int32_t access;
    int32_t startSector;
    int32_t parentSector;
    std::string dataType;      // Identified file format (e.g., "IFF ILBM Picture")
    std::string dataTypeGroup; // Format category (e.g., "Image", "Audio", "Executable")
    std::vector<FileNode> children;
};

struct DiskInfo
{
    std::string name;
    std::string type; // e.g. "OFS", "FFS"
    uint32_t sizeBytes;
    uint32_t usedBytes;
    uint32_t freeBytes;
    float usedPercentage;
    bool isReadOnly;
    uint32_t cylinders;
    uint32_t heads;
    uint32_t sectors;
    std::string runtimeStatus;
    std::string warning;
    uint64_t hostReads = 0;
    uint64_t hostWrites = 0;
    int hostActivity = 0; // 0 idle, 1 read, 2 write
};

struct RootBlockInfo
{
    std::string volumeName;
    uint32_t blockSize;
    uint32_t rootPtr;
    uint32_t diskType; // e.g. DOS0, DOS1...
    uint32_t firstData;
    uint32_t lastData;
    DateTime creationDate;
    DateTime modificationDate;
    bool hasRoot;

    // Extended Info
    uint32_t checkSum;
    int32_t bmFlag;
    uint32_t bmExt;
    uint32_t dirCachePtr;
    std::vector<int32_t> hashTable;
    std::vector<int32_t> bmPages;
};

struct BlockDetails
{
    uint32_t blockNum = 0;
    std::string typeLabel;
    int32_t primaryType = 0;
    int32_t secondaryType = 0;
    std::string secondaryTypeLabel;

    uint32_t checksum = 0;
    bool isChecksumValid = false;
    bool hasChecksum = false; // False for NDOS/raw blocks that have no checksum structure

    // Common/Specific fields
    std::string name;
    std::string comment;
    uint32_t byteSize = 0;
    uint32_t firstData = 0;
    uint32_t parent = 0;
    uint32_t nextBlock = 0;
    uint32_t extension = 0;

    // File specific
    uint8_t access = 0;
    std::vector<uint32_t> dataBlocks;
    int32_t highSeq = 0;

    // Data specific
    int32_t seqNum = 0;
    uint32_t dataSize = 0;

    // Root specific
    bool isBitmapValid = false;
    std::vector<uint32_t> bitmapBlocks;
    std::string dateStr;

    // Boot specific
    std::string dosType;
    uint32_t rootPtr = 0;

    // Helpers
    bool isDataBlock = false;
    bool isBootBlock = false;
    bool isBitmapBlock = false;
    bool isDeletedFile = false; // FileHeader block that is marked free in bitmap
    uint32_t fileHeader = 0;    // The head of the file this block belongs to
};

/**
 * @brief RAII Wrapper around adflib's AdfVolume and AdfDevice.
 */
class AdfHandler
{
public:
    AdfHandler();
    ~AdfHandler();

    AdfHandler(const AdfHandler&) = delete;
    AdfHandler& operator=(const AdfHandler&) = delete;

    AdfHandler(AdfHandler&& other) noexcept;
    AdfHandler& operator=(AdfHandler&& other) noexcept;

    /**
     * @brief Opens an ADF file.
     *
     * Tries to mount the volume (filesytem). If mounting fails (e.g. NDOS disk),
     * the device is still kept open for raw access.
     *
     * @param path Absolute path to the .adf file
     * @return true if device was opened successfully
     */
    bool Mount(const std::string& path);

    /**
     * @brief Mounts an ADF from a memory buffer.
     * @param data The file content.
     * @return true if mounted successfully.
     */
    bool Mount(std::vector<uint8_t> data);

    /**
     * @brief Mounts in lazy-loading mode backed by a GreaseweazelReader.
     * @param reader Pointer to the reader (must outlive this handler).
     * @return true if mounted successfully.
     */
    bool MountLazy(GreaseweazelReader* reader);

    void Unmount();

    [[nodiscard]] bool IsMounted() const;

    /**
     * @brief Creates a blank formatted ADF file (FFS or OFS).
     * @param path File path to create.
     * @param label Volume name.
     * @param useOFS If true, formats as OFS (standard), otherwise FFS.
     * @return true on success.
     */
    static bool CreateBlank(const std::string& path, const std::string& label, bool useOFS = false);

    /**
     * @brief Formats the currently mounted device.
     * @param label Volume name.
     * @param useOFS If true, formats as OFS, otherwise FFS.
     * @return true on success.
     */
    bool Format(const std::string& label, bool useOFS = false);

    /**
     * @brief Sets the volume name (label) of the mounted volume.
     * @param name New volume name (truncated to 30 characters).
     * @return true on success.
     */
    bool SetVolumeName(const std::string& name);

    /**
     * @return true if the disk has a valid DOS filesystem.
     */
    [[nodiscard]] bool HasFileSystem() const;

    /**
     * @brief One partition of a hard-disk image, as adflib found it in the RDB (or the single
     *        volume of a hardfile without one).
     */
    struct PartitionInfo
    {
        int index = 0;
        std::string name;        // the RDB drive name, e.g. "DH0"
        uint32_t firstBlock = 0; // from the start of the image
        uint32_t lastBlock = 0;
        std::string filesystem; // "DOS\3" style id + type, e.g. "DOS3"
        bool dosFilesystem = false;
    };

    /**
     * @return true for a hard-disk image (RDB or plain hardfile); false for a floppy.
     */
    [[nodiscard]] bool IsPartitionedDevice() const;

    /**
     * @return The partitions of a hard-disk image; empty for a floppy.
     */
    [[nodiscard]] std::vector<PartitionInfo> GetPartitions() const;

    /**
     * @return The partition the volume views currently show, or -1 for the whole image raw.
     */
    [[nodiscard]] int GetSelectedPartition() const { return m_partitionIndex; }

    /**
     * @brief Mounts partition `index` as the browsed volume, or -1 for the whole image as raw
     *        blocks. Every block number the handler takes or returns is then relative to that
     *        partition, so the block, bitmap and file views need no change.
     * @return true when the partition mounted with a filesystem (or -1 was asked for). A
     *         partition adflib cannot mount leaves the whole image in raw mode.
     */
    bool SelectPartition(int index);

    /**
     * @return The image block the handler's block 0 corresponds to.
     */
    [[nodiscard]] uint32_t GetPartitionOffset() const;

    /**
     * @brief Loads the entire file structure recursively.
     * @return Root node representing the volume root.
     */
    FileNode GetFileTree() const;

    /**
     * @brief Renames a file or directory.
     * @param parentSector The sector of the parent directory.
     * @param oldName The current name of the file/directory.
     * @param newName The new name.
     * @return true on success.
     */
    bool RenameFile(uint32_t parentSector, uint32_t fileSector, const std::string& oldName,
                    const std::string& newName);

    /**
     * @brief Moves a file or directory to a different directory.
     * @param srcParentSector The sector of the source parent directory.
     * @param fileName The name of the file/directory to move.
     * @param destParentSector The sector of the destination directory.
     * @param newName Optional new name (if empty, keeps the original name).
     * @return true on success.
     */
    bool MoveFile(uint32_t srcParentSector, const std::string& fileName, uint32_t destParentSector,
                  const std::string& newName = "");

    /**
     * @brief Imports a file from the host filesystem into the ADF.
     * @param parentSector The sector of the destination directory (0 for root).
     * @param hostPath The absolute path to the file on the host.
     * @return true on success.
     */
    bool ImportFile(uint32_t parentSector, const std::string& hostPath);

    /**
     * @brief Imports a file from a memory buffer into the ADF.
     * @param parentSector The sector of the destination directory (0 for root).
     * @param fileName The name for the file in the ADF.
     * @param data The file content.
     * @return true on success.
     */
    bool ImportFile(uint32_t parentSector, const std::string& fileName,
                    const std::vector<uint8_t>& data);

    /**
     * @brief Creates a new directory in the ADF.
     * @param parentSector The sector of the parent directory (0 for root).
     * @param name The name of the new directory.
     * @brief Create a new directory in ADF.
     */
    bool CreateDirectory(uint32_t parentSector, const std::string& name);

    /**
     * @brief Delete a file or directory from ADF.
     */
    bool DeleteFile(uint32_t parentSector, const std::string& name);

    /**
     * @brief Finds a file or directory in a specific directory.
     * @param parentSector The sector of the parent directory (0 for root).
     * @param name The name of the file/directory to find.
     * @return The sector number of the header block, or 0 if not found.
     */
    [[nodiscard]] uint32_t FindFile(uint32_t parentSector, const std::string& name) const;

    /**
     * @brief Sets the permission flags for a file or directory.
     * @param sector The sector of the file/directory header block.
     * @param access The new access flags value.
     * @return true on success.
     */
    bool SetPermissions(uint32_t sector, int32_t access);

    /**
     * @brief Checks if a block is marked as free in the bitmap.
     * @param blockNum The block number to check.
     * @return true if the block is free.
     */
    [[nodiscard]] bool IsBlockFree(uint32_t blockNum) const;

    /**
     * @brief Restores a deleted file from its header block.
     * @param headerBlock The file header block of the deleted file.
     * @return true on success, false if restore failed.
     */
    bool RestoreDeletedFile(uint32_t headerBlock);

    /**
     * @brief Securely scrubs a file by deleting it and overwriting its blocks with zeros.
     * @param headerBlock The file's header sector number.
     * @return true on success.
     */
    bool ScrubFile(uint32_t headerBlock);

    /**
     * @brief Writes zeros to a specific block.
     * @param blockNum The block number to clear.
     * @return true on success.
     */
    bool ClearBlock(uint32_t blockNum);

    /**
     * @brief Writes raw data to a specific block.
     * @param blockNum The block number to write.
     * @param data The data to write (must be exactly block size, typically 512 bytes).
     * @return true on success.
     */
    bool WriteBlock(uint32_t blockNum, const std::vector<uint8_t>& data);

    /**
     * @brief Calculates the number of blocks required to store a file of a given size.
     * Takes into account filesystem type (OFS/FFS) for overhead calculation.
     * @param sizeBytes Size of the file data in bytes.
     * @return Number of blocks required (header + data + extensions).
     */
    [[nodiscard]] uint32_t CalculateRequiredBlocks(uint64_t sizeBytes) const;

    /**
     * @brief Calculates the total blocks required to import a list of files/directories.
     * Recursively calculates size for directories.
     * @param paths List of host filesystem paths.
     * @return Total blocks required.
     */
    [[nodiscard]] uint32_t CalculateRequiredBlocks(const std::vector<std::string>& paths) const;

    /**
     * @brief Builds the uniform "not enough space" alert message used across the
     *        app (import, paste, save, ...).
     * @param operation Verb shown in the message, e.g. "Import", "Paste", "Save".
     * @param requiredBlocks Blocks the operation needs.
     * @param freeBlocks Blocks currently free on the disk.
     */
    [[nodiscard]] static std::string InsufficientSpaceMessage(const std::string& operation,
                                                              uint32_t requiredBlocks,
                                                              uint32_t freeBlocks);

    /**
     * @brief Gets the total number of blocks on the disk.
     * @return Total block count.
     */
    [[nodiscard]] uint32_t GetTotalBlocks() const;

    /**
     * @brief Gets the number of free blocks on the disk.
     */
    [[nodiscard]] uint32_t GetFreeBlocks() const;

    /**
     * @brief Scans for deleted/orphaned file header blocks.
     * @return Vector of FileNodes representing deleted files.
     */
    [[nodiscard]] std::vector<FileNode> GetDeletedFiles() const;

    /**
     * @brief Gets current disk statistics.
     * @return DiskInfo struct.
     */
    DiskInfo GetDiskInfo() const;

    /**
     * @brief Gets root block technical details.
     * @return RootBlockInfo struct.
     */
    RootBlockInfo GetRootBlockInfo() const;

    /**
     * @brief Reads a raw block from the disk.
     * @param blockNum The block number to read.
     * @return Vector of bytes containing the block data. Empty if read fails.
     */
    std::vector<uint8_t> ReadBlock(uint32_t blockNum) const;

    /**
     * @brief Reads a raw block from the original file on disk (bypassing memory buffer).
     * @param blockNum The block number to read.
     * @return Vector of bytes containing the block data. Empty if read fails.
     */
    std::vector<uint8_t> ReadBlockFromDisk(uint32_t blockNum) const;

    /**
     * @brief Gets the allocation status of all blocks.
     * @return Vector of bools (true = used, false = free).
     */
    [[nodiscard]] std::vector<bool> GetBlockUsage() const;

    /**
     * @brief Gets the current file content from the memory device (for saving).
     */
    [[nodiscard]] const std::vector<uint8_t>& GetFileContent() const;

    /**
     * @brief Gets the simplified type of all blocks for visualization.
     * Results are cached; call InvalidateBlockTypesCache() after disk modifications.
     * @return Vector of string codes matching colors_blocks.json keys.
     */
    [[nodiscard]] std::vector<std::string> GetAllBlockTypes() const;

    /**
     * @brief Invalidates the cached block types, forcing a rescan on next call.
     */
    void InvalidateBlockTypesCache();

    /**
     * @brief Checks all blocks to see if they are zero-filled.
     * @return Vector of bools (true = zero-filled, false = has data).
     */
    [[nodiscard]] std::vector<bool> GetZeroFilledBlocks() const;

    /**
     * @brief Retrieves all sectors allocated to a file.
     * @param headerBlock The file's header sector number.
     * @return List of sector indices (header, data, and extensions).
     */
    [[nodiscard]] std::vector<uint32_t> GetFileBlocks(uint32_t headerBlock) const;

    /**
     * @brief Reads the complete content of a file.
     * @param headerBlock The file's header sector number.
     * @return The file content as a string.
     */
    [[nodiscard]] std::string ReadFileContent(uint32_t headerBlock) const;

    /**
     * @brief Analyzes a raw block to determine its type and metadata.
     * @param blockNum The physical block index.
     * @param data The 512-byte block data.
     * @return BlockDetails structure.
     */
    [[nodiscard]] BlockDetails AnalyzeBlock(uint32_t blockNum, std::span<const uint8_t> data) const;

    /**
     * @brief Checks if a specific block has been modified since loading.
     * @param blockNum The block number to check.
     * @return true if the block has been modified.
     */
    [[nodiscard]] bool IsBlockModified(uint32_t blockNum) const;

    /**
     * @brief Checks if a specific block is currently cached/loaded (for lazy loading).
     * @param blockNum The block number to check.
     * @return true if the block is loaded in memory.
     */
    [[nodiscard]] bool IsBlockCached(uint32_t blockNum) const;

    /**
     * @brief Reverts a modified block to its original state from disk.
     * @param blockNum The block number to revert.
     * @return true if successful or if block wasn't modified.
     */
    bool RevertBlock(uint32_t blockNum);

    /**
     * @brief Gets the set of all modified block numbers.
     * @return Set of modified block numbers.
     */
    [[nodiscard]] const std::set<uint32_t>& GetModifiedBlocks() const;

    /**
     * @brief Gets modification status for all blocks.
     * @return Vector of bools (true = modified, false = unmodified).
     */
    [[nodiscard]] std::vector<bool> GetBlockModificationStatus() const;

    /**
     * @brief Checks if there are any unsaved changes.
     * @return true if any blocks have been modified.
     */
    [[nodiscard]] bool HasUnsavedChanges() const;

    /**
     * @brief Sets write protection on this image.
     *
     * Enforced here rather than in the UI because FileOperationService,
     * DocumentTransfer and ViewerLauncher all hold a bare AdfHandler& and never
     * see the owning document - a flag anywhere else would be bypassable. Every
     * mutator calls RequireWritable() first.
     *
     * Save()/SaveAs() are deliberately NOT gated: a user who unlocks, edits and
     * re-locks must still be able to persist changes they already authorised.
     */
    void SetWriteProtected(bool writeProtected);

    /**
     * @return true if this image refuses modification.
     */
    [[nodiscard]] bool IsWriteProtected() const;

    /**
     * @brief Saves the current state to the original file.
     * @return true on success.
     */
    bool Save();

    /**
     * @brief Saves the current state to a new file.
     * @param path The path to save to.
     * @return true on success.
     */
    bool SaveAs(const std::string& path);

    /**
     * @brief Clears the modified blocks tracking (call after save).
     */
    void ClearModifiedState();

    /**
     * @brief Gets the path of the currently loaded file.
     * @return The file path, or empty string if no file is loaded.
     */
    [[nodiscard]] const std::string& GetFilePath() const;

    /**
     * @brief Gets the current disk image content from memory.
     */
    [[nodiscard]] const std::vector<uint8_t>& GetImageContent() const;

    /**
     * @brief Export result for batch operations.
     */
    struct ExportResult
    {
        int filesExported = 0;
        int directoriesCreated = 0;
        int errors = 0;
        std::vector<std::string> errorMessages;
    };

    /**
     * @brief Exports a single file to the host filesystem.
     * @param headerSector The file's header sector.
     * @param destPath The destination file path on the host.
     * @return true on success.
     */
    bool ExportFile(uint32_t headerSector, const std::string& destPath) const;

    /**
     * @brief Exports a file or directory (recursively) to the host filesystem.
     * @param node The FileNode to export.
     * @param destPath The destination directory path on the host.
     * @param preserveStructure If true, creates subdirectories; if false, flattens.
     * @return ExportResult with counts and any error messages.
     */
    ExportResult ExportNode(const FileNode& node, const std::string& destPath,
                            bool preserveStructure = true) const;

    /**
     * @brief Exports multiple files/directories to the host filesystem.
     * @param nodes Vector of FileNodes to export.
     * @param destPath The destination directory path on the host.
     * @param preserveStructure If true, creates subdirectories; if false, flattens.
     * @return ExportResult with counts and any error messages.
     */
    ExportResult ExportNodes(const std::vector<FileNode>& nodes, const std::string& destPath,
                             bool preserveStructure = true) const;

private:
    // Logs and returns false when the image is write-protected. `op` names the
    // refused operation so the log says what was blocked.
    [[nodiscard]] bool RequireWritable(const char* op) const;

    // Images open protected; Application lowers it from the padlock in the
    // document window, the drive chip, or the emulator's Drives menu.
    bool m_writeProtected = true;
    std::unique_ptr<MemoryDevice> m_memDevice;
    struct AdfDevice* m_device = nullptr;
    struct AdfVolume* m_volume = nullptr;
    mutable std::map<uint32_t, uint32_t> m_blockToHeaderMap;
    mutable std::vector<std::string> m_blockTypesCache;
    mutable bool m_blockTypesCacheValid = false;
    // adfCountFreeBlocks scans the whole allocation bitmap, and GetDiskInfo() runs once per
    // frame per open document — cache the count and invalidate with the block-types cache.
    mutable int64_t m_freeBlocksCache = -1;
    // Which RDB partition is mounted (0 for a floppy); -1 while the whole image is shown raw.
    int m_partitionIndex = 0;
    // GetModifiedBlocks() hands out a reference, so the partition-relative copy lives here.
    mutable std::set<uint32_t> m_partitionModifiedBlocks;
};

} // namespace core
} // namespace adm
