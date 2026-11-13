#include "BlockAnalyzer.h"
#include "Constants.h"

extern "C" {
#define class klass
#include <adf_bitm.h>
#define BUILDING_WITH_CMAKE
#include <adf_util.h>
#include <adflib.h>
#undef class
}
#undef min
#undef max

#include <fmt/core.h>
#include <set>

namespace adm {
namespace core {

std::string BlockAnalyzer::GetPrimaryTypeLabel(int32_t type)
{
    switch (type) {
        case ADF_T_HEADER:
            return "T_SHORT";
        case ADF_T_DATA:
            return "T_DATA";
        case ADF_T_LIST:
            return "T_LIST";
        case ADF_T_DIRC:
            return "T_DIRC";
        default:
            return "Unknown";
    }
}

std::string BlockAnalyzer::GetSecondaryTypeLabel(int32_t secType)
{
    switch (secType) {
        case ADF_ST_ROOT:
            return "ST_ROOT";
        case ADF_ST_DIR:
            return "ST_USERDIR";
        case ADF_ST_FILE:
            return "ST_FILE";
        case ADF_ST_LFILE:
            return "ST_LFILE";
        case ADF_ST_LDIR:
            return "ST_LDIR";
        case ADF_ST_LSOFT:
            return "ST_LSOFT";
        default:
            return "None";
    }
}

BlockDetails BlockAnalyzer::AnalyzeBlock(struct AdfVolume* vol, struct AdfDevice* dev,
                                         uint32_t blockNum, std::span<const uint8_t> data,
                                         std::function<std::vector<uint8_t>(uint32_t)> readBlockFn)
{
    BlockDetails det;
    det.blockNum = blockNum;
    det.isChecksumValid = false;
    det.isDataBlock = false;
    det.isBootBlock = false;

    if (data.size() < 512) {
        det.typeLabel = "Short Block";
        return det;
    }

    auto getLong = [&](int offset) {
        return (int32_t)swapUint32fromPtr(const_cast<uint8_t*>(data.data()) + offset);
    };

    int32_t type = getLong(0);
    int32_t secType = getLong(508);

    det.primaryType = type;
    det.secondaryType = secType;
    det.checksum = (uint32_t)getLong(20);

    det.typeLabel = GetPrimaryTypeLabel(type);
    det.secondaryTypeLabel = GetSecondaryTypeLabel(secType);

    // Checksum Validation — only for known ADF block types (not NDOS/raw blocks)
    bool isKnownAdfType =
        (type == ADF_T_HEADER || type == ADF_T_DATA || type == ADF_T_LIST || type == ADF_T_DIRC);
    if (isKnownAdfType) {
        std::vector<uint8_t> temp(data.begin(), data.end());
        temp[20] = temp[21] = temp[22] = temp[23] = 0;
        det.isChecksumValid = ((uint32_t)adfNormalSum(temp.data(), 20, 512) == det.checksum);
        det.hasChecksum = true;
    }

    auto getName = [&](int lenOffset, int strOffset) -> std::string {
        if (lenOffset < 0 || static_cast<size_t>(lenOffset) >= data.size())
            return std::string();
        int len = data[lenOffset];
        if (len > 30)
            len = 30;
        if (strOffset < 0 || static_cast<size_t>(strOffset + len) > data.size()) {
            len = static_cast<int>(data.size()) - strOffset;
        }
        if (len <= 0)
            return std::string();
        return std::string((const char*)data.data() + strOffset, len);
    };

    auto formatDate = [&](int32_t days, int32_t mins, int32_t ticks) {
        int y, m, d;
        adfDays2Date(days, &y, &m, &d);
        int hh = mins / 60;
        int mm = mins % 60;
        int ss = ticks / 50;
        const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        return fmt::format("{:02d}-{}-{:04d} {:02d}:{:02d}:{:02d}", d, months[(m - 1) % 12], y, hh,
                           mm, ss);
    };

    // Specific Parsing
    if (type == ADF_T_HEADER) {
        det.name = getName(432, 433);
        det.parent = (uint32_t)getLong(500);
        det.extension = (uint32_t)getLong(504);

        if (secType == ADF_ST_ROOT) {
            det.isBitmapValid = (getLong(312) == -1);
            for (int i = 0; i < 25; i++) {
                uint32_t p = (uint32_t)getLong(316 + i * 4);
                if (p != 0)
                    det.bitmapBlocks.push_back(p);
            }
            det.dateStr = formatDate(getLong(420), getLong(424), getLong(428));
        } else if (secType == ADF_ST_FILE) {
            det.byteSize = (uint32_t)getLong(324);
            det.access = (uint8_t)data[320];
            det.firstData = (uint32_t)getLong(16);
            int commentLen = data[328];
            if (commentLen > 79)
                commentLen = 79;
            det.comment = std::string((const char*)data.data() + 329, commentLen);

            det.highSeq = getLong(8);
            for (int i = 0; i < 72; i++) {
                uint32_t p = (uint32_t)getLong(24 + i * 4);
                if (p != 0)
                    det.dataBlocks.push_back(p);
            }
        }
    } else if (type == ADF_T_DATA) {
        det.isDataBlock = true;
        det.parent = (uint32_t)getLong(4);
        det.seqNum = getLong(8);
        det.dataSize = (uint32_t)getLong(12);
        det.nextBlock = (uint32_t)getLong(16);
    } else if (blockNum == 0 || blockNum == 1) {
        // Boot Block Check logic requires both Block 0 and Block 1
        std::vector<uint8_t> block0, block1;
        if (blockNum == 0) {
            block0.assign(data.begin(), data.end());
            block1 = readBlockFn(1);
        } else {
            block0 = readBlockFn(0);
            block1.assign(data.begin(), data.end());
        }

        if (block0.size() >= 512 && block0[0] == 'D' && block0[1] == 'O' && block0[2] == 'S') {
            det.isBootBlock = true;
            det.hasChecksum = true;
            det.dosType = fmt::format("DOS\\{}", (int)block0[3]);
            det.checksum = (uint32_t)swapUint32fromPtr(block0.data() + 4);
            det.rootPtr = (uint32_t)swapUint32fromPtr(block0.data() + 8);

            // Calculate Bootblock Checksum (over 1024 bytes / 2 blocks)
            if (block1.size() >= 512) {
                std::vector<uint8_t> combined(1024);
                std::copy(block0.begin(), block0.end(), combined.begin());
                std::copy(block1.begin(), block1.end(), combined.begin() + 512);

                det.isChecksumValid = ValidateBootBlockChecksum(combined);
            }
        }
    }

    // Unified File Header detection
    uint32_t headerKey = (uint32_t)getLong(4);
    if (type == ADF_T_HEADER) {
        if (secType == ADF_ST_FILE || secType == ADF_ST_DIR) {
            det.fileHeader = blockNum;
        }
    } else if (type == ADF_T_DATA || type == ADF_T_LIST || type == ADF_T_DIRC) {
        det.fileHeader = headerKey;
    }

    return det;
}

std::vector<std::string> BlockAnalyzer::GetAllBlockTypes(
    struct AdfVolume* vol, struct AdfDevice* dev, const RootBlockInfo& rootInfo,
    std::function<bool(uint32_t)> isCached, std::map<uint32_t, uint32_t>* blockToHeaderMap)
{
    std::vector<std::string> types;
    if (!vol && !dev)
        return types;

    uint32_t totalBlocks = 0;
    if (vol)
        totalBlocks = adfVolGetSizeInBlocks(vol);
    else
        totalBlocks = dev->sizeBlocks;

    types.resize(totalBlocks, "Free");

    std::set<uint32_t> bitmapBlocks(rootInfo.bmPages.begin(), rootInfo.bmPages.end());

    std::vector<uint8_t> buffer(512);

    // Initial pass: Identifier what we can from headers/signatures
    // For FFS, we specifically look for file headers to trace their data blocks
    bool isFFS = (vol && adfVolIsFFS(vol));

    // We will build a set of known data blocks if FFS
    std::set<uint32_t> ffsDataBlocks;

    for (uint32_t i = 0; i < totalBlocks; ++i) {
        // Blocks 0 and 1 are always readable (bootblock) if mounted effectively
        // but let's check cache anyway if provided
        if (isCached && !isCached(i)) {
            types[i] = "Unread"; // New status for unread blocks
            continue;
        }

        if (i == 0 || i == 1) {
            types[i] = "BootBlock";
            continue;
        }

        if (vol && adfIsBlockFree(vol, i)) {
            types[i] = "Free";
            continue;
        }

        if (bitmapBlocks.count(i)) {
            types[i] = "BitmapBlock";
            continue;
        }

        if (vol) {
            if (adfVolReadBlock(vol, i, buffer.data()) != ADF_RC_OK)
                continue;
        } else {
            if (adfDevReadBlock(dev, (long)i, 512, buffer.data()) != ADF_RC_OK)
                continue;
        }

        int32_t primary = (int32_t)swapUint32fromPtr(buffer.data());
        int32_t secondary = (int32_t)swapUint32fromPtr(buffer.data() + 508);

        if (primary == ADF_T_HEADER) {
            switch (secondary) {
                case ADF_ST_ROOT:
                    types[i] = "ST_ROOT";
                    break;
                case ADF_ST_DIR:
                    types[i] = "ST_USERDIR";
                    break;
                case ADF_ST_FILE:
                    types[i] = "ST_FILE";
                    // If FFS, trace data blocks
                    if (isFFS && vol) {
                        // Gather data blocks from this header

                        // Data blocks in header
                        for (int d = 0; d < 72; d++) {
                            uint32_t dBlock =
                                (uint32_t)swapUint32fromPtr(buffer.data() + 24 + d * 4);
                            if (dBlock != 0) {
                                ffsDataBlocks.insert(dBlock);
                                if (blockToHeaderMap)
                                    (*blockToHeaderMap)[dBlock] = i;
                            }
                        }

                        // Extensions — with cycle detection
                        uint32_t extBlock = (uint32_t)swapUint32fromPtr(buffer.data() + 504);
                        std::set<uint32_t> visitedExt;
                        while (extBlock != 0) {
                            // visitedExt alone is complete cycle detection; the size cap is only
                            // a belt-and-braces bound and must track the volume, not DD's 1760.
                            // A hardfile chain longer than that used to stop here, silently
                            // under-reporting the file's blocks.
                            if (visitedExt.count(extBlock) || visitedExt.size() >= totalBlocks)
                                break;
                            visitedExt.insert(extBlock);

                            // Check if extension block is cached before reading
                            if (isCached && !isCached(extBlock)) {
                                break;
                            }

                            // Read ext block
                            std::vector<uint8_t> extBuf(512);
                            if (adfVolReadBlock(vol, extBlock, extBuf.data()) != ADF_RC_OK)
                                break;

                            // Data blocks in extension
                            for (int d = 0; d < 72; d++) {
                                uint32_t dBlock =
                                    (uint32_t)swapUint32fromPtr(extBuf.data() + 24 + d * 4);
                                if (dBlock != 0) {
                                    ffsDataBlocks.insert(dBlock);
                                    if (blockToHeaderMap)
                                        (*blockToHeaderMap)[dBlock] = i;
                                }
                            }

                            // Next extension
                            extBlock = (uint32_t)swapUint32fromPtr(extBuf.data() + 504);
                        }
                    }
                    break;
                case ADF_ST_LFILE:
                    types[i] = "ST_LFILE";
                    break;
                case ADF_ST_LDIR:
                    types[i] = "ST_LDIR";
                    break;
                case ADF_ST_LSOFT:
                    types[i] = "ST_LSOFT";
                    break;
                default:
                    types[i] = "T_HEADER";
                    break;
            }
        } else if (primary == ADF_T_DATA) {
            types[i] = "T_DATA";
        } else if (primary == ADF_T_LIST) {
            // Extension block
            types[i] = "T_LIST";
        } else if (primary == ADF_T_DIRC) {
            types[i] = "T_DIRC";
        } else {
            types[i] = "Unknown";
        }
    }

    // Pass 2: Apply FFS data blocks
    if (isFFS) {
        for (uint32_t block : ffsDataBlocks) {
            // Only update if the block is loaded or we inferred it
            // If it's effectively unread but we know it's data, maybe we can mark it?
            if (block < totalBlocks && types[block] != "Unread" && types[block] == "Unknown") {
                types[block] = "T_DATA";
            }
        }
    }

    return types;
}

bool BlockAnalyzer::ValidateChecksum(std::span<const uint8_t> data, uint32_t checksumOffset)
{
    if (data.size() < constants::disk::BlockSize || checksumOffset + 4 > data.size()) {
        return false;
    }

    // Read stored checksum from the specified offset
    uint32_t storedChecksum =
        (uint32_t)swapUint32fromPtr(const_cast<uint8_t*>(data.data()) + checksumOffset);

    // Create a temporary buffer with checksum field zeroed
    std::vector<uint8_t> temp(data.begin(), data.end());
    temp[checksumOffset] = temp[checksumOffset + 1] = temp[checksumOffset + 2] =
        temp[checksumOffset + 3] = 0;

    // Calculate checksum using adfNormalSum
    uint32_t calculatedChecksum = (uint32_t)adfNormalSum(temp.data(), checksumOffset, 512);

    return calculatedChecksum == storedChecksum;
}

bool BlockAnalyzer::ValidateBootBlockChecksum(std::span<const uint8_t> data)
{
    if (data.size() < 1024)
        return false;

    uint32_t checksum = (uint32_t)swapUint32fromPtr(const_cast<uint8_t*>(data.data()) + 4);
    uint32_t sum = 0;
    // Iterate 256 longwords
    for (int i = 0; i < 256; ++i) {
        if (i == 1)
            continue; // Skip checksum field at offset 4

        uint32_t val = (uint32_t)swapUint32fromPtr(const_cast<uint8_t*>(data.data()) + i * 4);
        uint32_t prevSum = sum;
        sum += val;
        if (sum < prevSum)
            sum++;
    }
    return (~sum == checksum);
}

uint32_t BlockAnalyzer::CalculateChecksum(std::span<const uint8_t> data, uint32_t checksumOffset)
{
    if (data.size() < constants::disk::BlockSize || checksumOffset + 4 > data.size()) {
        return 0;
    }

    // Create a temporary buffer with checksum field zeroed
    std::vector<uint8_t> temp(data.begin(), data.end());
    temp[checksumOffset] = temp[checksumOffset + 1] = temp[checksumOffset + 2] =
        temp[checksumOffset + 3] = 0;

    // Calculate checksum using adfNormalSum
    return (uint32_t)adfNormalSum(temp.data(), checksumOffset, 512);
}

uint32_t BlockAnalyzer::CalculateBootBlockChecksum(std::span<const uint8_t> data)
{
    if (data.size() < 1024)
        return 0;

    uint32_t sum = 0;
    // Iterate 256 longwords
    for (int i = 0; i < 256; ++i) {
        if (i == 1)
            continue; // Skip checksum field at offset 4

        uint32_t val = (uint32_t)swapUint32fromPtr(const_cast<uint8_t*>(data.data()) + i * 4);
        uint32_t prevSum = sum;
        sum += val;
        if (sum < prevSum)
            sum++;
    }
    return ~sum;
}

} // namespace core
} // namespace adm
