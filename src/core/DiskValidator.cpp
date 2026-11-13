#include "DiskValidator.h"
#include <functional>
#include "AdfWrapper.h"
#include "BigEndian.h"
#include "BlockAnalyzer.h"
#include "Logger.h"

#include <fmt/core.h>
#include <algorithm>
#include <map>
#include <set>

namespace adm {
namespace core {

uint32_t DiskValidator::ReadU32(const std::vector<uint8_t>& data, size_t offset)
{
    if (offset > data.size() || data.size() - offset < 4) // subtraction form: offset+4 can wrap
        return 0;
    return be::U32(data.data() + offset);
}

int32_t DiskValidator::ReadS32(const std::vector<uint8_t>& data, size_t offset)
{
    return static_cast<int32_t>(ReadU32(data, offset));
}

void DiskValidator::AddIssue(ValidationResult& result, ValidationSeverity severity,
                             ValidationCategory category, uint32_t block,
                             const std::string& message, const std::string& details,
                             bool repairable)
{
    ValidationIssue issue;
    issue.severity = severity;
    issue.category = category;
    issue.blockNumber = block;
    issue.message = message;
    issue.details = details;
    issue.isRepairable = repairable;
    result.issues.push_back(issue);

    switch (severity) {
        case ValidationSeverity::Success:
            result.successCount++;
            break;
        case ValidationSeverity::Info:
            result.infoCount++;
            break;
        case ValidationSeverity::Warning:
            result.warningCount++;
            break;
        case ValidationSeverity::Error:
            result.errorCount++;
            break;
    }
}

std::string DiskValidator::GetSeverityName(ValidationSeverity severity)
{
    switch (severity) {
        case ValidationSeverity::Success:
            return "OK";
        case ValidationSeverity::Info:
            return "Info";
        case ValidationSeverity::Warning:
            return "Warning";
        case ValidationSeverity::Error:
            return "Error";
        default:
            return "Unknown";
    }
}

std::string DiskValidator::GetCategoryName(ValidationCategory category)
{
    switch (category) {
        case ValidationCategory::BootBlock:
            return "Boot Block";
        case ValidationCategory::RootBlock:
            return "Root Block";
        case ValidationCategory::Bitmap:
            return "Bitmap";
        case ValidationCategory::FileHeader:
            return "File Header";
        case ValidationCategory::DataBlock:
            return "Data Block";
        case ValidationCategory::DirectoryHash:
            return "Directory Hash";
        case ValidationCategory::BlockChain:
            return "Block Chain";
        case ValidationCategory::Orphan:
            return "Orphaned Block";
        case ValidationCategory::CrossLink:
            return "Cross-linked Block";
        default:
            return "Unknown";
    }
}

ValidationResult DiskValidator::Validate(const AdfHandler& volume)
{
    ValidationResult result;

    if (!volume.IsMounted()) {
        AddIssue(result, ValidationSeverity::Error, ValidationCategory::BootBlock, 0,
                 "No disk mounted", "Cannot validate - no disk is currently mounted.");
        return result;
    }

    // Run all validation checks
    Logger::Info("Validating Boot Block...");
    ValidateBootBlock(volume, result);
    Logger::Info("Validating Root Block...");
    ValidateRootBlock(volume, result);
    Logger::Info("Validating Bitmap...");
    ValidateBitmap(volume, result);
    Logger::Info("Validating File Structure...");
    ValidateFileStructure(volume, result);

    result.completed = true;
    return result;
}

void DiskValidator::ValidateBootBlock(const AdfHandler& volume, ValidationResult& result)
{
    auto bootData = volume.ReadBlock(0);
    if (bootData.size() < 512) {
        AddIssue(result, ValidationSeverity::Error, ValidationCategory::BootBlock, 0,
                 "Failed to read boot block", "Could not read block 0 from disk.");
        return;
    }

    // Check DOS magic
    std::string dosType;
    dosType += static_cast<char>(bootData[0]);
    dosType += static_cast<char>(bootData[1]);
    dosType += static_cast<char>(bootData[2]);

    if (dosType != "DOS") {
        AddIssue(result, ValidationSeverity::Warning, ValidationCategory::BootBlock, 0,
                 "Non-standard DOS type", "Boot block does not start with 'DOS'. Type: " + dosType);
    } else {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::BootBlock, 0,
                 "DOS signature valid", "Boot block starts with 'DOS'");
    }

    // Check filesystem type
    uint8_t fsType = bootData[3];
    std::string fsName;
    switch (fsType) {
        case 0:
            fsName = "OFS";
            break;
        case 1:
            fsName = "FFS";
            break;
        case 2:
            fsName = "OFS-INTL";
            break;
        case 3:
            fsName = "FFS-INTL";
            break;
        case 4:
            fsName = "OFS-DC";
            break;
        case 5:
            fsName = "FFS-DC";
            break;
        default:
            fsName = "Unknown";
            AddIssue(result, ValidationSeverity::Warning, ValidationCategory::BootBlock, 0,
                     "Unknown filesystem type",
                     "Filesystem type byte is " + std::to_string(fsType) + ", expected 0-5.");
    }

    if (fsType <= 5) {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::BootBlock, 0,
                 "Filesystem type valid", "Filesystem: " + fsName);
    }

    // Check boot block checksum
    uint32_t storedChecksum = ReadU32(bootData, 4);

    // Create copy with checksum zeroed for calculation
    std::vector<uint8_t> checksumData = bootData;
    checksumData[4] = checksumData[5] = checksumData[6] = checksumData[7] = 0;

    uint32_t calculatedSum = 0;
    for (size_t i = 0; i < 512; i += 4) {
        calculatedSum += ReadU32(checksumData, i);
        // Handle overflow like Amiga does
        if (calculatedSum < ReadU32(checksumData, i)) {
            calculatedSum++; // Add carry
        }
    }
    uint32_t expectedChecksum = ~calculatedSum;

    // Also check second boot block (block 1)
    auto boot2Data = volume.ReadBlock(1);
    if (boot2Data.size() >= 512) {
        for (size_t i = 0; i < 512; i += 4) {
            calculatedSum += ReadU32(boot2Data, i);
            if (calculatedSum < ReadU32(boot2Data, i)) {
                calculatedSum++;
            }
        }
        expectedChecksum = ~calculatedSum;
    }

    // Check if there's boot code (non-zero checksum area)
    bool hasBootCode = false;
    for (size_t i = 12; i < bootData.size(); ++i) {
        if (bootData[i] != 0) {
            hasBootCode = true;
            break;
        }
    }

    if (storedChecksum == 0 && !hasBootCode) {
        // Empty boot block is fine
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::BootBlock, 0,
                 "Boot block empty (no boot code)", "Non-bootable disk, which is normal");
    } else if (storedChecksum != expectedChecksum) {
        AddIssue(
            result, ValidationSeverity::Warning, ValidationCategory::BootBlock, 0,
            "Boot block checksum mismatch",
            fmt::format("Stored: 0x{:08X}, Expected: 0x{:08X}", storedChecksum, expectedChecksum),
            true);
    } else {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::BootBlock, 0,
                 "Boot block checksum valid", fmt::format("Checksum: 0x{:08X}", storedChecksum));
    }

    // Check root block pointer
    uint32_t rootPtr = ReadU32(bootData, 8);
    uint32_t totalBlocks = volume.GetTotalBlocks();
    uint32_t expectedRoot = totalBlocks / 2;

    if (rootPtr != expectedRoot && rootPtr != 0) {
        AddIssue(result, ValidationSeverity::Info, ValidationCategory::BootBlock, 0,
                 "Non-standard root block location",
                 "Root block at " + std::to_string(rootPtr) + ", expected " +
                     std::to_string(expectedRoot));
    }
}

void DiskValidator::ValidateRootBlock(const AdfHandler& volume, ValidationResult& result)
{
    auto rootInfo = volume.GetRootBlockInfo();

    if (!rootInfo.hasRoot) {
        AddIssue(result, ValidationSeverity::Error, ValidationCategory::RootBlock, 0,
                 "No root block found", "Disk does not have a valid root block.");
        return;
    }

    auto rootData = volume.ReadBlock(rootInfo.rootPtr);
    if (rootData.size() < 512) {
        AddIssue(result, ValidationSeverity::Error, ValidationCategory::RootBlock, rootInfo.rootPtr,
                 "Failed to read root block",
                 "Could not read root block at " + std::to_string(rootInfo.rootPtr));
        return;
    }

    // Check primary type (should be 2 = T_HEADER)
    int32_t primaryType = ReadS32(rootData, 0);
    if (primaryType != 2) {
        AddIssue(result, ValidationSeverity::Error, ValidationCategory::RootBlock, rootInfo.rootPtr,
                 "Invalid root block type",
                 "Primary type is " + std::to_string(primaryType) + ", expected 2 (T_HEADER)");
    } else {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::RootBlock,
                 rootInfo.rootPtr, "Root block primary type valid", "Type is 2 (T_HEADER)");
    }

    // Check secondary type (should be 1 = ST_ROOT)
    int32_t secondaryType = ReadS32(rootData, 508);
    if (secondaryType != 1) {
        AddIssue(result, ValidationSeverity::Error, ValidationCategory::RootBlock, rootInfo.rootPtr,
                 "Invalid root block secondary type",
                 "Secondary type is " + std::to_string(secondaryType) + ", expected 1 (ST_ROOT)");
    } else {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::RootBlock,
                 rootInfo.rootPtr, "Root block secondary type valid", "Type is 1 (ST_ROOT)");
    }

    // Check checksum - Amiga checksum: sum of all longwords should equal 0
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; i += 4) {
        sum += ReadU32(rootData, i);
    }

    if (sum != 0) {
        uint32_t storedChecksum = ReadU32(rootData, 20);
        AddIssue(result, ValidationSeverity::Error, ValidationCategory::RootBlock, rootInfo.rootPtr,
                 "Root block checksum invalid",
                 "Checksum verification failed. Sum: 0x" + fmt::format("{:08X}", sum) +
                     ", Stored checksum: 0x" + fmt::format("{:08X}", storedChecksum),
                 true);
    } else {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::RootBlock,
                 rootInfo.rootPtr, "Root block checksum valid",
                 "Block checksum verification passed");
    }

    // Check bitmap flag
    if (rootInfo.bmFlag == 0) {
        AddIssue(result, ValidationSeverity::Warning, ValidationCategory::RootBlock,
                 rootInfo.rootPtr, "Bitmap flag indicates invalid state",
                 "The bitmap valid flag (bm_flag) is 0, indicating bitmap may be stale.");
    } else {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::RootBlock,
                 rootInfo.rootPtr, "Bitmap flag valid", "Bitmap is marked as valid");
    }

    // Check volume name
    if (rootInfo.volumeName.empty()) {
        AddIssue(result, ValidationSeverity::Warning, ValidationCategory::RootBlock,
                 rootInfo.rootPtr, "Volume has no name", "The volume name is empty.");
    } else {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::RootBlock,
                 rootInfo.rootPtr, "Volume name present", "Volume name: " + rootInfo.volumeName);
    }
}

void DiskValidator::ValidateBitmap(const AdfHandler& volume, ValidationResult& result)
{
    auto blockUsage = volume.GetBlockUsage(); // true = allocated in bitmap
    uint32_t totalBlocks = volume.GetTotalBlocks();

    if (blockUsage.size() != totalBlocks) {
        AddIssue(result, ValidationSeverity::Error, ValidationCategory::Bitmap, 0,
                 "Block usage data size mismatch",
                 "Expected " + std::to_string(totalBlocks) + " blocks.");
        return;
    }

    // Get root block info to know bitmap block locations
    auto rootInfo = volume.GetRootBlockInfo();
    std::set<uint32_t> bitmapBlocks(rootInfo.bmPages.begin(), rootInfo.bmPages.end());

    int allocatedButEmpty = 0;
    int usedButFree = 0;
    std::vector<uint32_t> allocatedEmptyBlocks;
    std::vector<uint32_t> usedFreeBlocks;

    // Count allocated/free blocks
    int allocatedCount = 0;
    int freeCount = 0;

    for (uint32_t i = 2; i < totalBlocks; ++i) { // Skip boot blocks
        bool isAllocatedInBitmap = blockUsage[i];

        if (isAllocatedInBitmap) {
            allocatedCount++;
        } else {
            freeCount++;
        }

        // Read the block to determine its actual content type
        auto blockData = volume.ReadBlock(i);
        if (blockData.size() < 512) {
            continue;
        }

        // Check if block appears to contain valid data (not all zeros)
        bool isZeroFilled = true;
        for (size_t j = 0; j < blockData.size(); ++j) {
            if (blockData[j] != 0) {
                isZeroFilled = false;
                break;
            }
        }

        // Analyze block type from content
        int32_t primaryType = ReadS32(blockData, 0);
        int32_t secondaryType = ReadS32(blockData, 508);

        bool looksLikeValidBlock = false;

        // Check if it's a known block type
        if (primaryType == 2) { // T_HEADER
            // Valid header types: ST_ROOT(1), ST_USERDIR(2), ST_FILE(-3), ST_LFILE(4), ST_LDIR(-4),
            // ST_LSOFT(3)
            if (secondaryType == 1 || secondaryType == 2 || secondaryType == -3 ||
                secondaryType == 4 || secondaryType == -4 || secondaryType == 3) {
                looksLikeValidBlock = true;
            }
        } else if (primaryType == 8) { // T_DATA (OFS data block)
            looksLikeValidBlock = true;
        } else if (primaryType == 16) { // T_LIST (extension block)
            looksLikeValidBlock = true;
        } else if (primaryType == 33) { // T_DIRC (directory cache)
            looksLikeValidBlock = true;
        }

        // Skip bitmap blocks and root block from this check
        if (bitmapBlocks.count(i) || i == rootInfo.rootPtr) {
            continue;
        }

        // Check for allocated but completely empty blocks (wasted space)
        if (isAllocatedInBitmap && isZeroFilled) {
            allocatedButEmpty++;
            if (allocatedEmptyBlocks.size() < 10) {
                allocatedEmptyBlocks.push_back(i);
            }
        }

        // Check for blocks with valid structure but marked as free (data corruption risk!)
        if (!isAllocatedInBitmap && looksLikeValidBlock && !isZeroFilled) {
            usedButFree++;
            if (usedFreeBlocks.size() < 10) {
                usedFreeBlocks.push_back(i);
            }
        }
    }

    // Report bitmap statistics
    AddIssue(result, ValidationSeverity::Success, ValidationCategory::Bitmap, 0,
             "Bitmap block allocation",
             fmt::format("{} blocks allocated, {} blocks free", allocatedCount, freeCount));

    if (allocatedButEmpty > 0) {
        std::string blockList;
        for (uint32_t b : allocatedEmptyBlocks) {
            if (!blockList.empty())
                blockList += ", ";
            blockList += std::to_string(b);
        }
        if (allocatedButEmpty > 10) {
            blockList += "...";
        }

        AddIssue(result, ValidationSeverity::Warning, ValidationCategory::Bitmap, 0,
                 std::to_string(allocatedButEmpty) + " blocks allocated but zero-filled",
                 "Blocks: " + blockList + ". These may be wasted space or pending writes.", true);
    } else {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::Bitmap, 0,
                 "No wasted allocated blocks", "All allocated blocks contain data");
    }

    if (usedButFree > 0) {
        std::string blockList;
        for (uint32_t b : usedFreeBlocks) {
            if (!blockList.empty())
                blockList += ", ";
            blockList += std::to_string(b);
        }
        if (usedButFree > 10) {
            blockList += "...";
        }

        // Warning, not Error: OFS/FFS do not zero freed blocks, so a free block that
        // still "looks like" a valid block is most often a leftover from a deleted
        // file rather than live data the bitmap lost track of.
        AddIssue(result, ValidationSeverity::Warning, ValidationCategory::Bitmap, 0,
                 std::to_string(usedButFree) + " free blocks still contain block-like data",
                 "Blocks: " + blockList +
                     ". Usually remnants of deleted files; only a concern if these were "
                     "expected to be live.",
                 true);
    } else {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::Bitmap, 0,
                 "No bitmap/content mismatches", "All blocks with data are properly allocated");
    }
}

void DiskValidator::ValidateFileStructure(const AdfHandler& volume, ValidationResult& result)
{
    // Track which blocks are referenced and by what
    std::map<uint32_t, std::vector<uint32_t>>
        blockReferences; // block -> list of headers that ref it
    std::set<uint32_t> visitedHeaders;
    std::set<uint32_t> allReferencedBlocks; // All blocks referenced by filesystem
    uint32_t totalBlocks = volume.GetTotalBlocks();

    // Get root block info for bitmap blocks
    auto rootInfo = volume.GetRootBlockInfo();
    std::set<uint32_t> bitmapBlocks(rootInfo.bmPages.begin(), rootInfo.bmPages.end());

    // Statistics for summary
    int fileCount = 0;
    int dirCount = 0;
    int headerErrors = 0;
    int checksumErrors = 0;
    int blockChainErrors = 0;
    int hashTableErrors = 0;

    // Mark system blocks as referenced
    allReferencedBlocks.insert(0);                // Boot block 0
    allReferencedBlocks.insert(1);                // Boot block 1
    allReferencedBlocks.insert(rootInfo.rootPtr); // Root block
    for (uint32_t bm : bitmapBlocks) {
        allReferencedBlocks.insert(bm);
    }

    // Get the file tree and traverse it
    auto root = volume.GetFileTree();

    std::function<void(const FileNode&, uint32_t)> validateNode = [&](const FileNode& node,
                                                                      uint32_t parentHeader) {
        if (node.startSector <= 0 || static_cast<uint32_t>(node.startSector) >= totalBlocks) {
            if (node.startSector > 0) { // Ignore root placeholder
                AddIssue(result, ValidationSeverity::Error, ValidationCategory::FileHeader, 0,
                         "Invalid file header sector",
                         "File '" + node.name + "' has invalid sector " +
                             std::to_string(node.startSector));
                headerErrors++;
            }
            // Still recurse into children for the root node
            for (const auto& child : node.children) {
                validateNode(child, node.startSector > 0 ? static_cast<uint32_t>(node.startSector)
                                                         : rootInfo.rootPtr);
            }
            return;
        }

        uint32_t headerSector = static_cast<uint32_t>(node.startSector);
        allReferencedBlocks.insert(headerSector);

        // Check for duplicate headers (shouldn't happen in valid tree)
        if (visitedHeaders.count(headerSector)) {
            AddIssue(result, ValidationSeverity::Error, ValidationCategory::FileHeader,
                     headerSector, "Duplicate file header reference",
                     "File '" + node.name + "' header is referenced multiple times.");
            headerErrors++;
            return;
        }
        visitedHeaders.insert(headerSector);

        if (node.isDir) {
            dirCount++;
        } else {
            fileCount++;
        }

        // Read and validate file header
        Logger::Info("Reading header for node: {} (Sector: {})", node.name, headerSector);
        auto headerData = volume.ReadBlock(headerSector);
        if (headerData.size() < 512) {
            AddIssue(result, ValidationSeverity::Error, ValidationCategory::FileHeader,
                     headerSector, "Failed to read file header",
                     "Could not read header for '" + node.name + "'");
            headerErrors++;
            return;
        }
        Logger::Info("Header read success for node: {}", node.name);

        // Check primary type
        int32_t primaryType = ReadS32(headerData, 0);
        if (primaryType != 2) { // T_HEADER
            AddIssue(result, ValidationSeverity::Error, ValidationCategory::FileHeader,
                     headerSector, "Invalid file header type",
                     "File '" + node.name + "' has primary type " + std::to_string(primaryType) +
                         ", expected 2");
            headerErrors++;
        }

        // Check secondary type
        int32_t secondaryType = ReadS32(headerData, 508);
        if (node.isDir) {
            if (secondaryType != 2) { // ST_USERDIR
                AddIssue(result, ValidationSeverity::Warning, ValidationCategory::FileHeader,
                         headerSector, "Unexpected directory secondary type",
                         "Directory '" + node.name + "' has type " + std::to_string(secondaryType) +
                             ", expected 2");
            }
        } else {
            if (secondaryType != -3) { // ST_FILE
                AddIssue(result, ValidationSeverity::Warning, ValidationCategory::FileHeader,
                         headerSector, "Unexpected file secondary type",
                         "File '" + node.name + "' has type " + std::to_string(secondaryType) +
                             ", expected -3");
            }
        }

        // Check header checksum - sum all longwords, result should be 0
        uint32_t sum = 0;
        for (size_t i = 0; i < 512; i += 4) {
            sum += ReadU32(headerData, i);
        }

        if (sum != 0) {
            uint32_t storedChecksum = ReadU32(headerData, 20);
            AddIssue(result, ValidationSeverity::Error, ValidationCategory::FileHeader,
                     headerSector, "File header checksum invalid",
                     "File '" + node.name + "' checksum failed. Stored: 0x" +
                         fmt::format("{:08X}", storedChecksum),
                     true);
            checksumErrors++;
        }

        // Validate parent pointer
        uint32_t storedParent = ReadU32(headerData, 500);
        if (storedParent != parentHeader && parentHeader != 0) {
            AddIssue(result, ValidationSeverity::Warning, ValidationCategory::FileHeader,
                     headerSector, "Parent pointer mismatch",
                     "File '" + node.name + "' parent is " + std::to_string(storedParent) +
                         ", expected " + std::to_string(parentHeader));
        }

        // For directories, validate hash table
        if (node.isDir || secondaryType == 1) { // ST_USERDIR or ST_ROOT
            // Hash table is at offsets 24-495 (72 entries * 4 bytes + some header fields)
            int hashTableSize = 72;
            // Clamp to what actually fits in the block data
            if (24 + static_cast<size_t>(hashTableSize) * 4 > headerData.size()) {
                hashTableSize = static_cast<int>((headerData.size() - 24) / 4);
            }
            if (hashTableSize < 0)
                hashTableSize = 0;

            for (int i = 0; i < hashTableSize; ++i) {
                int32_t hashEntry = ReadS32(headerData, 24 + i * 4);
                if (hashEntry != 0) {
                    if (hashEntry < 0 || static_cast<uint32_t>(hashEntry) >= totalBlocks) {
                        AddIssue(result, ValidationSeverity::Error,
                                 ValidationCategory::DirectoryHash, headerSector,
                                 "Invalid hash table entry",
                                 "Directory '" + node.name + "' has invalid hash entry " +
                                     std::to_string(hashEntry) + " at slot " + std::to_string(i));
                        hashTableErrors++;
                    }
                }
            }
        }

        // For files, check data block chain
        if (!node.isDir && node.size > 0) {
            Logger::Info("Checking block chain for file: {}", node.name);
            auto fileBlocks = volume.GetFileBlocks(headerSector);

            // Check if we found as many blocks as the header claims exist.
            // NOTE: high_seq in the file *header* caps at the in-header pointer count
            // (72) for files that span extension blocks, so for large files
            // fileBlocks (the full chain) is larger than highSeq+1 and this only
            // catches genuinely short chains — it won't false-positive.
            uint32_t highSeq = ReadU32(headerData, 8);
            if (highSeq > totalBlocks)
                highSeq = totalBlocks;

            if (fileBlocks.size() < highSeq + 1) {
                AddIssue(result, ValidationSeverity::Error, ValidationCategory::BlockChain,
                         headerSector, "Broken block chain",
                         "File '" + node.name + "' header claims " + std::to_string(highSeq) +
                             " data blocks, but only " + std::to_string(fileBlocks.size() - 1) +
                             " were found. The chain is likely broken.");
                blockChainErrors++;
            }

            // For OFS files, also validate the next_data chain
            // The header's dataBlocks[] array is the "official" list, but OFS also has
            // a linked list via first_data -> next_data. If these don't match, corruption.
            uint32_t firstData = ReadU32(headerData, 16); // first_data offset in file header
            if (firstData != 0 && firstData < totalBlocks) {
                auto firstBlockData = volume.ReadBlock(firstData);
                if (firstBlockData.size() >= 512) {
                    int32_t firstBlockType = ReadS32(firstBlockData, 0);
                    if (firstBlockType == 8) { // T_DATA - this is OFS
                        // Follow the next_data chain and count reachable blocks
                        uint32_t chainCount = 0;
                        uint32_t currentBlock = firstData;
                        std::set<uint32_t> visitedChain; // Detect loops
                        uint32_t lastValidBlock = 0;

                        while (currentBlock != 0 && currentBlock < totalBlocks &&
                               chainCount < highSeq + 100) { // Safety limit
                            if (visitedChain.count(currentBlock)) {
                                AddIssue(result, ValidationSeverity::Error,
                                         ValidationCategory::BlockChain, currentBlock,
                                         "Circular block chain detected",
                                         "File '" + node.name +
                                             "' has a loop in its data block chain at block " +
                                             std::to_string(currentBlock));
                                blockChainErrors++;
                                break;
                            }
                            visitedChain.insert(currentBlock);

                            auto chainBlockData = volume.ReadBlock(currentBlock);
                            if (chainBlockData.size() < 512) {
                                AddIssue(result, ValidationSeverity::Error,
                                         ValidationCategory::BlockChain, currentBlock,
                                         "Failed to read data block in chain",
                                         "File '" + node.name + "' chain block " +
                                             std::to_string(currentBlock) + " is unreadable");
                                blockChainErrors++;
                                break;
                            }

                            int32_t chainBlockType = ReadS32(chainBlockData, 0);
                            if (chainBlockType != 8) { // Not T_DATA
                                AddIssue(result, ValidationSeverity::Error,
                                         ValidationCategory::BlockChain, currentBlock,
                                         "Invalid block type in data chain",
                                         "File '" + node.name + "' chain block " +
                                             std::to_string(currentBlock) + " has type " +
                                             std::to_string(chainBlockType) +
                                             " instead of 8 (T_DATA). Chain broken at block " +
                                             std::to_string(chainCount + 1) + " of " +
                                             std::to_string(highSeq));
                                blockChainErrors++;
                                break;
                            }

                            // Validate seq_num matches position in chain
                            uint32_t seqNum = ReadU32(chainBlockData, 8);
                            if (seqNum != chainCount + 1) {
                                AddIssue(result, ValidationSeverity::Warning,
                                         ValidationCategory::BlockChain, currentBlock,
                                         "Data block sequence number mismatch",
                                         "File '" + node.name + "' block " +
                                             std::to_string(currentBlock) + " has seq_num " +
                                             std::to_string(seqNum) + ", expected " +
                                             std::to_string(chainCount + 1));
                            }

                            chainCount++;
                            lastValidBlock = currentBlock;

                            // Get next_data pointer (offset 16 in OFS data block)
                            currentBlock = ReadU32(chainBlockData, 16);
                        }

                        // Compare chain count with high_seq
                        if (chainCount < highSeq) {
                            AddIssue(
                                result, ValidationSeverity::Error, ValidationCategory::BlockChain,
                                headerSector, "OFS data chain truncated",
                                "File '" + node.name + "' header claims " +
                                    std::to_string(highSeq) + " data blocks, but only " +
                                    std::to_string(chainCount) +
                                    " are reachable via next_data chain. Chain ends at block " +
                                    std::to_string(lastValidBlock));
                            blockChainErrors++;
                        }
                    }
                }
            }

            int blockIndex = 0;

            for (uint32_t block : fileBlocks) {
                if (block >= totalBlocks) {
                    AddIssue(result, ValidationSeverity::Error, ValidationCategory::BlockChain,
                             headerSector, "Invalid block pointer in chain",
                             "File '" + node.name + "' references out-of-range block " +
                                 std::to_string(block) + " at position " +
                                 std::to_string(blockIndex));
                    blockChainErrors++;
                    continue;
                }

                allReferencedBlocks.insert(block);
                blockReferences[block].push_back(headerSector);

                // Read and validate data block (for OFS)
                auto blockData = volume.ReadBlock(block);
                if (blockData.size() >= 512) {
                    int32_t blockType = ReadS32(blockData, 0);
                    if (blockType == 8) { // T_DATA (OFS data block)
                        // Validate OFS data block structure
                        uint32_t blockHeader = ReadU32(blockData, 4);

                        if (blockHeader != headerSector) {
                            AddIssue(result, ValidationSeverity::Warning,
                                     ValidationCategory::DataBlock, block,
                                     "Data block header mismatch",
                                     "Block " + std::to_string(block) + " points to header " +
                                         std::to_string(blockHeader) + ", expected " +
                                         std::to_string(headerSector));
                        }
                    }
                }

                blockIndex++;
            }

            // Check extension blocks
            uint32_t extBlock = ReadU32(headerData, 504);
            int extCount = 0;
            while (extBlock != 0 && extBlock < totalBlocks && extCount < 1000) {
                allReferencedBlocks.insert(extBlock);
                auto extData = volume.ReadBlock(extBlock);
                if (extData.size() < 512)
                    break;

                int32_t extType = ReadS32(extData, 0);
                if (extType != 16) { // T_LIST
                    AddIssue(result, ValidationSeverity::Error, ValidationCategory::BlockChain,
                             extBlock, "Invalid extension block type",
                             "Extension block has type " + std::to_string(extType) +
                                 ", expected 16");
                    blockChainErrors++;
                    break;
                }

                // Validate extension checksum
                uint32_t extSum = 0;
                for (size_t i = 0; i < 512; i += 4) {
                    extSum += ReadU32(extData, i);
                }
                if (extSum != 0) {
                    AddIssue(result, ValidationSeverity::Error, ValidationCategory::BlockChain,
                             extBlock, "Extension block checksum invalid",
                             "Extension block for '" + node.name + "' has bad checksum");
                    checksumErrors++;
                }

                extBlock = ReadU32(extData, 504);
                extCount++;
            }

            if (extCount >= 1000) {
                AddIssue(result, ValidationSeverity::Error, ValidationCategory::BlockChain,
                         headerSector, "Extension chain too long or circular",
                         "File '" + node.name + "' has suspiciously long extension chain");
                blockChainErrors++;
            }
        }

        // Recurse into children
        for (const auto& child : node.children) {
            validateNode(child, headerSector);
        }
    };

    validateNode(root, 0);

    // Report file/directory statistics
    AddIssue(result, ValidationSeverity::Success, ValidationCategory::FileHeader, 0,
             "File structure scanned",
             fmt::format("{} files, {} directories validated", fileCount, dirCount));

    // Report header validation results
    if (headerErrors == 0) {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::FileHeader, 0,
                 "All file headers valid", "No structural errors in file headers");
    }

    if (checksumErrors == 0) {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::FileHeader, 0,
                 "All header checksums valid",
                 "All file and directory headers have valid checksums");
    }

    // Report hash table validation results
    if (hashTableErrors == 0) {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::DirectoryHash, 0,
                 "Directory hash tables valid", "All directory hash table entries are valid");
    }

    // Report block chain validation results
    if (blockChainErrors == 0) {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::BlockChain, 0,
                 "Block chains valid", "All file block chains are properly linked");
    }

    // Check for cross-linked blocks
    int crossLinkCount = 0;
    for (const auto& [block, refs] : blockReferences) {
        if (refs.size() > 1) {
            std::string refList;
            for (uint32_t r : refs) {
                if (!refList.empty())
                    refList += ", ";
                refList += std::to_string(r);
            }
            AddIssue(result, ValidationSeverity::Error, ValidationCategory::CrossLink, block,
                     "Block referenced by multiple files",
                     "Block " + std::to_string(block) + " is claimed by file headers: " + refList);
            crossLinkCount++;
        }
    }

    if (crossLinkCount == 0) {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::CrossLink, 0,
                 "No cross-linked blocks", "Each data block belongs to exactly one file");
    }

    // Check for orphaned blocks (allocated but not referenced by filesystem)
    auto blockUsage = volume.GetBlockUsage();

    if (blockUsage.size() < totalBlocks) {
        // Only report if we haven't already reported a bitmap error (to avoid double spam)
        // But here we just return to avoid crash
        return;
    }

    int orphanCount = 0;
    std::vector<uint32_t> orphanedBlocks;

    for (uint32_t i = 2; i < totalBlocks; ++i) {
        bool isAllocated = blockUsage[i];
        bool isReferenced = allReferencedBlocks.count(i) > 0;

        if (isAllocated && !isReferenced) {
            orphanCount++;
            if (orphanedBlocks.size() < 10) {
                orphanedBlocks.push_back(i);
            }
        }
    }

    if (orphanCount > 0) {
        std::string blockList;
        for (uint32_t b : orphanedBlocks) {
            if (!blockList.empty())
                blockList += ", ";
            blockList += std::to_string(b);
        }
        if (orphanCount > 10) {
            blockList += "...";
        }

        AddIssue(result, ValidationSeverity::Warning, ValidationCategory::Orphan, 0,
                 std::to_string(orphanCount) + " orphaned blocks found",
                 "Blocks: " + blockList + ". These are allocated but not referenced by any file.",
                 true);
    } else {
        AddIssue(result, ValidationSeverity::Success, ValidationCategory::Orphan, 0,
                 "No orphaned blocks", "All allocated blocks are referenced by the filesystem");
    }
}

} // namespace core
} // namespace adm
