#include "BlockReader.h"

extern "C" {
#define class klass
#include <adf_bitm.h>
#include <adf_dev_driver_nativ.h>
#include <adflib.h>
#undef class
}

namespace adm {
namespace core {

std::vector<uint8_t> BlockReader::ReadBlock(struct AdfVolume* vol, struct AdfDevice* dev,
                                            uint32_t blockNum)
{
    std::vector<uint8_t> data;
    if (vol) {
        if (!adfVolIsSectNumValid(vol, blockNum)) {
            return data;
        }
        data.resize(vol->blockSize);
        if (adfVolReadBlock(vol, blockNum, data.data()) != ADF_RC_OK) {
            data.clear();
        }
    } else if (dev) {
        if (blockNum >= dev->sizeBlocks) {
            return data;
        }
        data.resize(512); // Raw device usually 512
        if (adfDevReadBlock(dev, (long)blockNum, 512, data.data()) != ADF_RC_OK) {
            data.clear();
        }
    }

    return data;
}

std::vector<bool> BlockReader::GetBlockUsage(struct AdfVolume* vol)
{
    std::vector<bool> usage;
    if (!vol) {
        return usage;
    }

    uint32_t totalBlocks = adfVolGetSizeInBlocks(vol);
    usage.resize(totalBlocks);

    for (uint32_t i = 0; i < totalBlocks; ++i) {
        // Blocks 0 and 1 are reserved (BootBlock), so they are effectively "used".
        // adfIsBlockFree asserts i >= 2.
        if (i < 2) {
            usage[i] = true;
        } else {
            usage[i] = !adfIsBlockFree(vol, i);
        }
    }

    return usage;
}

std::vector<bool> BlockReader::GetZeroFilledBlocks(struct AdfVolume* vol, struct AdfDevice* dev,
                                                   std::function<bool(uint32_t)> isCached)
{
    std::vector<bool> zeroFilled;
    if (!vol && !dev)
        return zeroFilled;

    uint32_t totalBlocks = 0;
    if (vol)
        totalBlocks = adfVolGetSizeInBlocks(vol);
    else
        totalBlocks = dev->sizeBlocks;

    zeroFilled.resize(totalBlocks, false);

    std::vector<uint8_t> buffer(512);

    for (uint32_t i = 0; i < totalBlocks; ++i) {
        // Skip unread blocks if a cache check is provided
        if (isCached && !isCached(i)) {
            continue;
        }

        bool success = false;
        if (vol) {
            success = (adfVolReadBlock(vol, i, buffer.data()) == ADF_RC_OK);
        } else {
            success = (adfDevReadBlock(dev, (long)i, 512, buffer.data()) == ADF_RC_OK);
        }

        if (success) {
            bool allZeros = true;
            for (uint8_t b : buffer) {
                if (b != 0) {
                    allZeros = false;
                    break;
                }
            }
            zeroFilled[i] = allZeros;
        }
    }

    return zeroFilled;
}

} // namespace core
} // namespace adm
