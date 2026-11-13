#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "AdfWrapper.h"

// Forward declaration
struct AdfVolume;
struct AdfDevice;

namespace adm {
namespace core {

/**
 * @brief Analyzes block types and provides detailed block information.
 *
 * Handles block type detection and analysis for Amiga disk blocks.
 */
class BlockAnalyzer
{
public:
    /**
     * @brief Analyzes a raw block to determine its type and metadata.
     * @param vol The mounted volume (may be nullptr for raw access).
     * @param dev The device (used when volume is nullptr).
     * @param blockNum The physical block index.
     * @param data The 512-byte block data.
     * @param readBlockFn Function to read additional blocks if needed.
     * @return BlockDetails structure.
     */
    static BlockDetails AnalyzeBlock(struct AdfVolume* vol, struct AdfDevice* dev,
                                     uint32_t blockNum, std::span<const uint8_t> data,
                                     std::function<std::vector<uint8_t>(uint32_t)> readBlockFn);

    /**
     * @brief Gets the simplified type of all blocks for visualization.
     * @param vol The mounted volume (may be nullptr).
     * @param dev The device (used when volume is nullptr).
     * @param rootInfo Root block info for bitmap block identification.
     * @param isCached Optional function to check if a block is cached.
     * @return Vector of string codes matching colors_blocks.json keys.
     */
    static std::vector<std::string>
    GetAllBlockTypes(struct AdfVolume* vol, struct AdfDevice* dev, const RootBlockInfo& rootInfo,
                     std::function<bool(uint32_t)> isCached = nullptr,
                     std::map<uint32_t, uint32_t>* blockToHeaderMap = nullptr);

    /**
     * @brief Validates the checksum of a block using a specified offset.
     * @param data The 512-byte block data.
     * @param checksumOffset The byte offset where the checksum is stored (e.g., 0 for bitmap, 20
     * for headers).
     * @return true if the checksum is valid.
     */
    static bool ValidateChecksum(std::span<const uint8_t> data, uint32_t checksumOffset);
    static bool ValidateBootBlockChecksum(std::span<const uint8_t> data);

    /**
     * @brief Calculates the Amiga checksum for a block.
     * @param data The block data.
     * @param checksumOffset The offset where the checksum is stored (field will be treated as 0
     * during calculation).
     * @return The calculated checksum.
     */
    static uint32_t CalculateChecksum(std::span<const uint8_t> data, uint32_t checksumOffset);

    /**
     * @brief Calculates the Boot Block checksum (requires 1024 bytes / blocks 0+1).
     * @param data The combined data of blocks 0 and 1.
     * @return The calculated checksum.
     */
    static uint32_t CalculateBootBlockChecksum(std::span<const uint8_t> data);

private:
    static std::string GetPrimaryTypeLabel(int32_t type);
    static std::string GetSecondaryTypeLabel(int32_t secType);
};

} // namespace core
} // namespace adm
