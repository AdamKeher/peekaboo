#pragma once

#include <cstdint>
#include <functional>
#include <vector>

// Forward declaration
struct AdfVolume;
struct AdfDevice;

namespace adm {
namespace core {

/**
 * @brief Handles raw block I/O operations for Amiga disk images.
 *
 * Provides low-level block reading and analysis utilities.
 */
class BlockReader
{
public:
    /**
     * @brief Reads a raw block from the disk.
     * @param vol The mounted volume (may be nullptr for raw access).
     * @param dev The device (used when volume is nullptr).
     * @param blockNum The block number to read.
     * @return Vector of bytes containing the block data. Empty if read fails.
     */
    static std::vector<uint8_t> ReadBlock(struct AdfVolume* vol, struct AdfDevice* dev,
                                          uint32_t blockNum);

    /**
     * @brief Gets the allocation status of all blocks.
     * @param vol The mounted volume.
     * @return Vector of bools (true = used, false = free).
     */
    static std::vector<bool> GetBlockUsage(struct AdfVolume* vol);

    /**
     * @brief Checks all blocks to see if they are zero-filled.
     * @param vol The mounted volume (may be nullptr).
     * @param dev The device (used when volume is nullptr).
     * @param isCached Optional function to check if a block is cached.
     * @return Vector of bools (true = zero-filled, false = has data).
     */
    static std::vector<bool> GetZeroFilledBlocks(struct AdfVolume* vol, struct AdfDevice* dev,
                                                 std::function<bool(uint32_t)> isCached = nullptr);
};

} // namespace core
} // namespace adm
