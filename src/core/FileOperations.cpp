#include "FileOperations.h"
#include "BigEndian.h"

extern "C" {
#define class klass
#include <adf_file.h>
#include <adf_file_block.h>
#define BUILDING_WITH_CMAKE
#include <adf_util.h>
#include <adflib.h>
#undef class
}
#undef min
#undef max

#include <algorithm>
#include <cstdlib>
#include <set>

namespace adm {
namespace core {

// Big-endian reads from a raw entry-block buffer (callers stay within the 512-byte block).
static uint32_t readU32(const uint8_t* buf, int offset) { return be::U32(buf + offset); }

static int32_t readI32(const uint8_t* buf, int offset) { return be::S32(buf + offset); }

// Build a FileNode from a raw 512-byte entry block buffer.
// 'blockNum' is the sector this block was read from; 'parentSector' is the
// sector of the directory that contains this entry.
FileNode FileOperations::ParseEntryBlock(const uint8_t* buf, uint32_t blockNum, long parentSector)
{
    FileNode node;

    int32_t secType = readI32(buf, 508);
    node.isDir = (secType == ADF_ST_DIR);

    int nameLen = std::min((int)buf[432], 30);
    node.name = std::string(reinterpret_cast<const char*>(buf + 433), nameLen);

    node.size = readI32(buf, 324);
    node.access = readI32(buf, 320);

    int commentLen = std::min((int)buf[328], 79);
    node.comment = std::string(reinterpret_cast<const char*>(buf + 329), commentLen);

    node.startSector = (int32_t)blockNum;
    node.entryId = DocumentEntryId::AdfBlock(blockNum);
    node.parentSector = (int32_t)parentSector;

    int32_t days = readI32(buf, 420);
    int32_t mins = readI32(buf, 424);
    int32_t ticks = readI32(buf, 428);
    // Guard against bogus date values that could crash adfDays2Date
    if (days >= 0 && days < 50000) {
        adfDays2Date(days, &node.modified.year, &node.modified.month, &node.modified.day);
    }
    node.modified.hour = (mins >= 0 && mins < 1440) ? (mins / 60) : 0;
    node.modified.min = (mins >= 0 && mins < 1440) ? (mins % 60) : 0;
    node.modified.sec = (ticks >= 0 && ticks < 3000) ? (ticks / 50) : 0;

    return node;
}

// Cycle-safe walk of a directory block's hash table + nextSameHash chains. For each valid
// entry header block encountered, invokes visit(entrySector, entryBuffer). Bypasses adflib's
// adfGetDirEnt, which hangs on disks with cyclic hash chains.
static void WalkDirEntries(struct AdfVolume* vol, uint32_t dirSector,
                           const std::function<void(uint32_t, const uint8_t*)>& visit)
{
    uint8_t dirBuf[512];
    if (adfVolReadBlock(vol, dirSector, dirBuf) != ADF_RC_OK)
        return;

    uint32_t totalBlocks = adfVolGetSizeInBlocks(vol);

    // Hash table: 72 entries at offset 24 (same layout for root and userdir blocks)
    constexpr int HT_SIZE = 72;

    // Track every entry block seen across all hash slots to guard against a
    // block appearing in multiple chains (corrupt but possible).
    std::set<uint32_t> seenEntryBlocks;
    uint8_t entBuf[512];

    for (int i = 0; i < HT_SIZE; ++i) {
        uint32_t block = readU32(dirBuf, 24 + i * 4);

        while (block != 0) {
            if (block >= totalBlocks)
                break;

            if (!seenEntryBlocks.insert(block).second)
                break; // cycle detected

            if (adfVolReadBlock(vol, block, entBuf) != ADF_RC_OK)
                break;

            // Validate: must be a header block (primary type T_HEADER = 2)
            int32_t primaryType = readI32(entBuf, 0);
            if (primaryType != ADF_T_HEADER)
                break;

            visit(block, entBuf);

            // Follow nextSameHash chain (offset 496 from block start)
            block = readU32(entBuf, 496);
        }
    }
}

std::vector<FileOperations::DirEntry> FileOperations::EnumerateDir(struct AdfVolume* vol,
                                                                   uint32_t dirSector)
{
    std::vector<DirEntry> entries;
    if (!vol)
        return entries;

    WalkDirEntries(vol, dirSector, [&](uint32_t block, const uint8_t* buf) {
        DirEntry e;
        e.sector = block;
        e.secType = readI32(buf, 508);
        e.isDir = (e.secType == ADF_ST_DIR);
        int nameLen = std::min((int)buf[432], 30);
        e.name = std::string(reinterpret_cast<const char*>(buf + 433), nameLen);
        entries.push_back(std::move(e));
    });

    return entries;
}

void FileOperations::ScanDirectoryRecursive(struct AdfVolume* vol, long sector,
                                            std::vector<FileNode>& outChildren,
                                            std::set<long>& visitedSectors)
{
    WalkDirEntries(vol, (uint32_t)sector, [&](uint32_t block, const uint8_t* entBuf) {
        FileNode node = ParseEntryBlock(entBuf, block, sector);

        if (node.isDir) {
            if (visitedSectors.insert((long)block).second)
                ScanDirectoryRecursive(vol, (long)block, node.children, visitedSectors);
        }

        outChildren.push_back(std::move(node));
    });
}

FileNode FileOperations::GetFileTree(struct AdfVolume* vol)
{
    FileNode root;
    root.name = "Root";
    root.isDir = true;
    root.size = 0;
    root.startSector = 0;
    root.entryId = DocumentEntryId::AdfBlock(0);

    if (!vol)
        return root;

    std::set<long> visitedSectors = {vol->curDirPtr};
    ScanDirectoryRecursive(vol, vol->curDirPtr, root.children, visitedSectors);

    return root;
}

std::vector<uint32_t> FileOperations::GetFileBlocks(struct AdfVolume* vol, uint32_t headerBlock)
{
    std::vector<uint32_t> blocks;
    if (!vol)
        return blocks;

    struct AdfEntryBlock ent;
    if (adfReadEntryBlock(vol, headerBlock, &ent) != ADF_RC_OK) {
        return blocks;
    }

    if (ent.secType != ADF_ST_FILE) {
        blocks.push_back(headerBlock);
        return blocks;
    }

    struct AdfFileBlocks fileBlocks;
    struct AdfFileHeaderBlock* fhdr = reinterpret_cast<struct AdfFileHeaderBlock*>(&ent);

    if (adfGetFileBlocks(vol, fhdr, &fileBlocks) == ADF_RC_OK) {
        blocks.push_back(fileBlocks.header);

        for (unsigned int i = 0; i < fileBlocks.data.nItems; i++) {
            blocks.push_back(fileBlocks.data.sectors[i]);
        }

        for (unsigned int i = 0; i < fileBlocks.extens.nItems; i++) {
            blocks.push_back(fileBlocks.extens.sectors[i]);
        }

        if (fileBlocks.data.sectors)
            free(fileBlocks.data.sectors);
        if (fileBlocks.extens.sectors)
            free(fileBlocks.extens.sectors);
    }

    return blocks;
}

std::string
FileOperations::ReadFileContent(struct AdfVolume* vol, uint32_t headerBlock,
                                std::function<std::vector<uint8_t>(uint32_t)> readBlockFn)
{
    if (!vol)
        return "";

    struct AdfEntryBlock ent;
    if (adfReadEntryBlock(vol, headerBlock, &ent) != ADF_RC_OK) {
        return "";
    }

    if (ent.secType != ADF_ST_FILE) {
        return "";
    }

    struct AdfFileHeaderBlock* fhdr = reinterpret_cast<struct AdfFileHeaderBlock*>(&ent);
    uint32_t totalBytes = fhdr->byteSize;

    struct AdfFileBlocks fileBlocks;
    if (adfGetFileBlocks(vol, fhdr, &fileBlocks) != ADF_RC_OK) {
        return "";
    }

    std::string content;
    content.reserve(totalBytes);

    bool isFFS = adfVolIsFFS(vol);
    uint32_t bytesRead = 0;

    for (unsigned int i = 0; i < fileBlocks.data.nItems && bytesRead < totalBytes; i++) {
        uint32_t sector = fileBlocks.data.sectors[i];
        auto blockData = readBlockFn(sector);
        if (blockData.size() < 512)
            continue;

        uint32_t chunkSize = 0;
        const uint8_t* dataPtr = nullptr;

        if (isFFS) {
            chunkSize = 512;
            dataPtr = blockData.data();
        } else {
            chunkSize = 488;
            dataPtr = blockData.data() + 24;
        }

        if (bytesRead + chunkSize > totalBytes) {
            chunkSize = totalBytes - bytesRead;
        }

        if (dataPtr && chunkSize > 0) {
            content.append(reinterpret_cast<const char*>(dataPtr), chunkSize);
            bytesRead += chunkSize;
        }
    }

    if (fileBlocks.data.sectors)
        free(fileBlocks.data.sectors);
    if (fileBlocks.extens.sectors)
        free(fileBlocks.extens.sectors);

    return content;
}

} // namespace core
} // namespace adm
