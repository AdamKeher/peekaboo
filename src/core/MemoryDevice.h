#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

// Forward declarations from adflib
struct AdfDevice;

namespace adm {
namespace core {
class GreaseweazelReader;
} // namespace core
} // namespace adm

namespace adm {
namespace core {

/**
 * @brief Memory-backed device driver for ADFLib.
 *
 * This class provides a memory buffer that intercepts all ADFLib disk I/O,
 * allowing changes to be made in memory without writing to the original file.
 * Changes can be saved explicitly via Save() or SaveAs().
 */
class MemoryDevice
{
public:
    MemoryDevice();
    ~MemoryDevice();

    MemoryDevice(const MemoryDevice&) = delete;
    MemoryDevice& operator=(const MemoryDevice&) = delete;

    /**
     * @brief Load an ADF file into the memory buffer.
     * @param path Path to the ADF file.
     * @return AdfDevice pointer on success, nullptr on failure.
     */
    AdfDevice* Open(const std::string& path);

    /**
     * @brief Load from an existing memory buffer (e.g., from ZIP).
     * @param data The file content.
     * @return AdfDevice pointer on success, nullptr on failure.
     */
    AdfDevice* Open(std::vector<uint8_t> data);

    /**
     * @brief Open in lazy-loading mode backed by a GreaseweazelReader.
     * The reader's buffer is used directly; unloaded tracks return zeros.
     * @param reader Pointer to the reader (must outlive this device).
     * @return AdfDevice pointer on success, nullptr on failure.
     */
    AdfDevice* OpenLazy(GreaseweazelReader* reader);

    /**
     * @brief Close the device and free resources.
     */
    void Close();

    /**
     * @brief Check if a device is currently open.
     */
    [[nodiscard]] bool IsOpen() const;

    /**
     * @brief Save the memory buffer to the original file.
     * @return true on success.
     */
    bool Save();

    /**
     * @brief Save the memory buffer to a new file.
     * @param path Path to save to.
     * @return true on success.
     */
    bool SaveAs(const std::string& path);

    /**
     * @brief Get the path of the currently loaded file.
     */
    [[nodiscard]] const std::string& GetFilePath() const { return m_filePath; }

    /**
     * @brief Set the file path (used after SaveAs to update the current file).
     */
    void SetFilePath(const std::string& path) { m_filePath = path; }

    /**
     * @brief Check if a block is currently cached in memory (for lazy loading).
     */
    [[nodiscard]] bool IsBlockCached(uint32_t block) const;

    /**
     * @brief Check if a specific block has been modified.
     */
    [[nodiscard]] bool IsBlockModified(uint32_t block) const;

    /**
     * @brief Get all modified block numbers.
     */
    [[nodiscard]] const std::set<uint32_t>& GetModifiedBlocks() const { return m_modifiedBlocks; }

    /**
     * @brief Check if there are any unsaved changes.
     */
    [[nodiscard]] bool HasUnsavedChanges() const { return !m_modifiedBlocks.empty(); }

    /**
     * @brief Clear the modified blocks tracking (call after save).
     */
    void ClearModifiedState() { m_modifiedBlocks.clear(); }

    /**
     * @brief Get modification status for all blocks.
     */
    [[nodiscard]] std::vector<bool> GetBlockModificationStatus() const;

    /**
     * @brief Get the total number of blocks.
     */
    [[nodiscard]] uint32_t GetTotalBlocks() const;

    /**
     * @brief Revert a modified block to its original state from disk.
     * @param block The block number to revert.
     * @return true if successful, false otherwise.
     */
    bool RevertBlock(uint32_t block);

    /**
     * @brief Read a block directly from the original file on disk.
     * @param block The block number to read.
     * @return Vector of bytes, or empty if read fails.
     */
    [[nodiscard]] std::vector<uint8_t> ReadBlockFromDisk(uint32_t block) const;

    /**
     * @brief Get the current memory buffer (for saving back to ZIP/file).
     */
    [[nodiscard]] const std::vector<uint8_t>& GetData() const { return m_buffer; }

    // Internal methods called by the ADFLib driver callbacks
    bool ReadSector(uint32_t sector, uint32_t size, uint8_t* buf);
    bool WriteSector(uint32_t sector, uint32_t size, const uint8_t* buf);

private:
    std::string m_filePath;
    std::vector<uint8_t> m_buffer;
    std::vector<uint8_t> m_originalData; // For RevertBlock when using memory-only mode
    std::set<uint32_t> m_modifiedBlocks;
    AdfDevice* m_device = nullptr;
    uint32_t m_blockSize = 512;
    uint32_t m_totalBlocks = 0;

    // Sectors per track for the geometry this buffer represents (11 DD, 22 HD). Derived by
    // GeometrySectorsPerTrack() so the lazy read path and the ADFLib driver's geometry agree;
    // ReadSector needs it to map a block back to the cylinder it must ask the reader for.
    uint32_t m_sectorsPerTrack = 11;

    // Lazy-loading mode (floppy reader)
    GreaseweazelReader* m_lazyReader = nullptr;
};

} // namespace core
} // namespace adm
