#include "AdfWrapper.h"

#include "BlockAnalyzer.h"
#include "BlockReader.h"
#include "Constants.h"
#include "FileOperations.h"
#include "Logger.h"
#include "MemoryDevice.h"

// adflib headers - workaround for C 'class' keyword and include necessary definitions
extern "C" {
// Macro hack to handle 'class' member variable in adflib structs
#define class klass
#include <adf_bitm.h>
#include <adf_dev_driver_nativ.h>
#define BUILDING_WITH_CMAKE
#include <adf_file.h>
#include <adf_file_block.h>
#include <adf_util.h>
#include <adflib.h>
#undef class
}
#undef min
#undef max

#include <fmt/core.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace adm::core {

// Local helper: international uppercase (matches ADFLib's static adfIntlToUpper)
static inline uint8_t intlToUpper(uint8_t c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 224 && c <= 254 && c != 247)) ? c - ('a' - 'A') : c;
}

// Validates an Amiga file/dir name (length, reserved separators, control chars).
// On failure prints a diagnostic and returns false.
static bool ValidateAmigaName(const std::string& name)
{
    if (name.empty()) {
        Logger::Error("Error: Filename is empty.");
        return false;
    }
    if (name.length() > ADF_MAX_NAME_LEN) {
        Logger::Error("Error: Filename '{}' exceeds {} characters.", name, ADF_MAX_NAME_LEN);
        return false;
    }
    if (name.find_first_of("/:") != std::string::npos) {
        Logger::Error("Error: Filename '{}' contains invalid characters.", name);
        return false;
    }
    for (char c : name) {
        if (std::iscntrl(static_cast<unsigned char>(c))) {
            Logger::Error("Error: Filename '{}' contains control characters.", name);
            return false;
        }
    }
    return true;
}

// Local hash function (matches ADFLib's adfGetHashValue which is static)
static unsigned calcHashValue(const uint8_t* name, bool intl)
{
    uint32_t hash, len;
    len = hash = (uint32_t)strlen((const char*)name);
    for (unsigned i = 0; i < len; i++) {
        uint8_t upper;
        if (intl) {
            upper = intlToUpper(name[i]);
        } else {
            upper = (uint8_t)toupper(name[i]);
        }
        hash = (hash * 13 + upper) & 0x7ff;
    }
    hash = hash % ADF_HT_SIZE;
    return hash;
}

// Route ADFLib's internal verbose/warning/error output through our Logger. The library's
// default handlers write raw "Warning <...>" / "Error <...>" lines straight to stderr, bypassing
// the timestamped/categorised log and the in-app LogView. These adapters format the printf-style
// varargs into a buffer and forward them as an "ADFlib" log category.
static void AdfLogVerbose(const char* const format, ...)
{
    va_list args;
    va_start(args, format);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Logger::Debug("ADFlib: {}", buf);
}

static void AdfLogWarn(const char* const format, ...)
{
    va_list args;
    va_start(args, format);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Logger::Warn("ADFlib: {}", buf);
}

static void AdfLogError(const char* const format, ...)
{
    va_list args;
    va_start(args, format);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Logger::Error("ADFlib: {}", buf);
}

AdfHandler::AdfHandler() : m_memDevice(std::make_unique<MemoryDevice>())
{
    static bool initialized = false;
    if (!initialized) {
        adfLibInit();
        adfEnvSetFct(AdfLogError, AdfLogWarn, AdfLogVerbose, nullptr);
        adfAddDeviceDriver(&adfDeviceDriverNative);
        initialized = true;
    }
}

const std::vector<uint8_t>& AdfHandler::GetImageContent() const
{
    static const std::vector<uint8_t> empty;
    return m_memDevice ? m_memDevice->GetData() : empty;
}

AdfHandler::~AdfHandler() { Unmount(); }

AdfHandler::AdfHandler(AdfHandler&& other) noexcept
    : m_writeProtected(other.m_writeProtected), m_memDevice(std::move(other.m_memDevice)),
      m_device(other.m_device), m_volume(other.m_volume)
{
    other.m_device = nullptr;
    other.m_volume = nullptr;
}

AdfHandler& AdfHandler::operator=(AdfHandler&& other) noexcept
{
    if (this != &other) {
        Unmount();
        m_writeProtected = other.m_writeProtected;
        m_memDevice = std::move(other.m_memDevice);
        m_device = other.m_device;
        m_volume = other.m_volume;
        other.m_device = nullptr;
        other.m_volume = nullptr;
    }
    return *this;
}

void AdfHandler::SetWriteProtected(bool writeProtected) { m_writeProtected = writeProtected; }

bool AdfHandler::IsWriteProtected() const { return m_writeProtected; }

bool AdfHandler::RequireWritable(const char* op) const
{
    if (!m_writeProtected)
        return true;
    // Warn rather than Error: a refused write is the protection working, not a fault. The
    // operation name is what makes the line useful when a caller silently does nothing.
    Logger::Warn("Blocked {} on write-protected image '{}'", op,
                 m_memDevice ? m_memDevice->GetFilePath() : std::string());
    return false;
}

static AdfVolume* MountWritableVolume(AdfDevice* device, int partition = 0)
{
    // ADFlib's read-write wrapper calls adfVolCheckFilesystem() even when its internal
    // read-only mount returned null. Optimized builds expose that null dereference for NDOS and
    // malformed images. Probe read-only first, then perform the normal checked read-write mount.
    if (!device || partition < 0 || partition >= device->nVol)
        return nullptr;
    AdfVolume* probe = adfVolMount(device, partition, ADF_ACCESS_MODE_READONLY);
    if (!probe)
        return nullptr;
    adfVolUnMount(probe);
    return adfVolMount(device, partition, ADF_ACCESS_MODE_READWRITE);
}

bool AdfHandler::Mount(std::vector<uint8_t> data)
{
    Unmount();

    // Use MemoryDevice to load the buffer into a memory device
    m_device = m_memDevice->Open(std::move(data));
    if (!m_device) {
        Logger::Error("Failed to open device from memory buffer");
        return false;
    }

    // Crucial step: Mount the device itself first to detect volumes.
    // Only attempt volume mount if device mount succeeds; calling adfVolMount
    // on a device with an empty volume list (NDOS disk) crashes adflib.
    m_partitionIndex = 0;
    if (adfDevMount(m_device) == ADF_RC_OK) {
        m_volume = MountWritableVolume(m_device);
    } else {
        Logger::Info("AdfWrapper: NDOS disk — no AmigaDOS filesystem, opening in raw block mode");
    }

    return true;
}

bool AdfHandler::MountLazy(GreaseweazelReader* reader)
{
    Unmount();

    m_device = m_memDevice->OpenLazy(reader);
    if (!m_device) {
        Logger::Error("Failed to open lazy device for floppy reader");
        return false;
    }

    m_partitionIndex = 0;
    if (adfDevMount(m_device) == ADF_RC_OK) {
        m_volume = MountWritableVolume(m_device);
    } else {
        Logger::Info(
            "AdfWrapper: NDOS disk (lazy) — no AmigaDOS filesystem, opening in raw block mode");
    }

    return true;
}

bool AdfHandler::Mount(const std::string& path)
{
    Unmount();

    // Use MemoryDevice to load the file into a memory buffer
    // All changes will be made to the buffer, not the original file
    m_device = m_memDevice->Open(path);
    if (!m_device) {
        Logger::Error("Failed to open device into memory: {}", path);
        return false;
    }

    // Crucial step: Mount the device itself first to detect volumes.
    // Only attempt volume mount if device mount succeeds; calling adfVolMount
    // on a device with an empty volume list (NDOS disk) crashes adflib.
    m_partitionIndex = 0;
    if (adfDevMount(m_device) == ADF_RC_OK) {
        m_volume = MountWritableVolume(m_device);
    } else {
        Logger::Info(
            "AdfWrapper: NDOS disk '{}' — no AmigaDOS filesystem, opening in raw block mode", path);
    }

    return true;
}

// Standard ADFLib FSMASK definitions if missing
#ifndef FSMASK_FFS
#define FSMASK_FFS      1
#define FSMASK_INTL     2
#define FSMASK_DIRCACHE 4
#endif

bool AdfHandler::CreateBlank(const std::string& path, const std::string& label, bool useOFS)
{
    // 1. Create empty file first
    {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f)
            return false;
        // 80 cyl * 2 heads * 11 sec * 512 bytes = 901120 bytes
        constexpr long kAdfSize = 80L * 2 * 11 * 512;
        if (fseek(f, kAdfSize - 1, SEEK_SET) != 0 || fputc(0, f) == EOF) {
            Logger::Error("Failed to size new ADF file: {}", path);
            fclose(f);
            return false;
        }
        if (fclose(f) != 0) {
            Logger::Error("Failed to flush new ADF file: {}", path);
            return false;
        }
    }

    // 2. Open device
    struct AdfDevice* dev = adfDevOpen(const_cast<char*>(path.c_str()), ADF_ACCESS_MODE_READWRITE);
    if (!dev)
        return false;

    // 3. Format as FFS or OFS
    long fsMask = useOFS ? 0 : FSMASK_FFS;
    ADF_RETCODE rc = adfCreateFlop(dev, const_cast<char*>(label.c_str()), fsMask);

    // 4. Close device
    adfDevClose(dev);

    return (rc == ADF_RC_OK);
}

bool AdfHandler::Format(const std::string& label, bool useOFS)
{
    if (!RequireWritable("format"))
        return false;

    if (!m_device)
        return false;

    // Format as FFS or OFS
    long fsMask = useOFS ? 0 : FSMASK_FFS;
    ADF_RETCODE rc = adfCreateFlop(m_device, const_cast<char*>(label.c_str()), fsMask);

    if (rc == ADF_RC_OK) {
        // Remount the volume after formatting
        if (m_volume) {
            adfVolUnMount(m_volume);
        }
        m_volume = adfVolMount(m_device, 0, ADF_ACCESS_MODE_READWRITE);
        InvalidateBlockTypesCache();
    }

    return (rc == ADF_RC_OK);
}

bool AdfHandler::SetVolumeName(const std::string& name)
{
    if (!RequireWritable("set volume name"))
        return false;

    if (!m_volume)
        return false;

    struct AdfRootBlock root;
    if (adfReadRootBlock(m_volume, static_cast<uint32_t>(m_volume->rootBlock), &root) != ADF_RC_OK)
        return false;

    std::string trimmed = name.substr(0, ADF_MAX_NAME_LEN);
    root.nameLen = static_cast<uint8_t>(trimmed.size());
    memset(root.diskName, 0, sizeof(root.diskName));
    memcpy(root.diskName, trimmed.data(), trimmed.size());

    // adfWriteRootBlock recomputes the block checksum before writing.
    if (adfWriteRootBlock(m_volume, static_cast<uint32_t>(m_volume->rootBlock), &root) != ADF_RC_OK)
        return false;

    InvalidateBlockTypesCache();
    return true;
}

bool AdfHandler::RenameFile(uint32_t parentSector, uint32_t fileSector, const std::string& oldName,
                            const std::string& newName)
{
    if (!RequireWritable("rename"))
        return false;

    if (!m_volume)
        return false;

    // 1. Validation
    if (!ValidateAmigaName(newName))
        return false;

    // 2. Perform Rename
    ADF_RETCODE rc = adfRenameEntry(m_volume, parentSector, const_cast<char*>(oldName.c_str()),
                                    parentSector, const_cast<char*>(newName.c_str()));

    if (rc != ADF_RC_OK) {
        Logger::Error("Failed to rename file: {} -> {}", oldName, newName);
        return false;
    }

    // 3. Sanitization (Zero out name buffer)
    struct AdfEntryBlock entry;
    rc = adfReadEntryBlock(m_volume, fileSector, &entry);
    if (rc != ADF_RC_OK) {
        Logger::Error("Rename successful, but failed to read entry block {} for sanitization.",
                      fileSector);
        return true;
    }

    // Zero out the name buffer
    memset(entry.name, 0, ADF_MAX_NAME_LEN + 1);

    // Copy new name
    size_t len = newName.length();
    if (len > ADF_MAX_NAME_LEN)
        len = ADF_MAX_NAME_LEN;
    strncpy(entry.name, newName.c_str(), len);
    entry.nameLen = static_cast<uint8_t>(len);

    // Rewrite block
    rc = adfWriteEntryBlock(m_volume, fileSector, &entry);
    if (rc != ADF_RC_OK) {
        Logger::Error("Rename successful, but failed to write sanitized entry block {}.",
                      fileSector);
        return true;
    }

    return true;
}

bool AdfHandler::MoveFile(uint32_t srcParentSector, const std::string& fileName,
                          uint32_t destParentSector, const std::string& newName)
{
    if (!RequireWritable("move"))
        return false;

    if (!m_volume)
        return false;

    // Use original name if no new name specified
    std::string targetName = newName.empty() ? fileName : newName;

    // Resolve sectors (0 -> Root)
    uint32_t srcSector = (srcParentSector == 0) ? m_volume->rootBlock : srcParentSector;
    uint32_t destSector = (destParentSector == 0) ? m_volume->rootBlock : destParentSector;

    // Validation
    if (!ValidateAmigaName(targetName))
        return false;

    // Resolve the entry being moved so we can guard against moving a directory into itself or
    // one of its own descendants (which would orphan the subtree).
    uint32_t movingSector = FindFile(srcSector, fileName);
    if (movingSector != 0) {
        struct AdfEntryBlock movingEntry;
        if (adfReadEntryBlock(m_volume, movingSector, &movingEntry) == ADF_RC_OK &&
            movingEntry.secType == ADF_ST_DIR) {
            for (uint32_t walk = destSector; walk != 0 && walk != (uint32_t)m_volume->rootBlock;) {
                if (walk == movingSector) {
                    Logger::Error(
                        "Error: cannot move directory '{}' into itself or a subdirectory.",
                        fileName);
                    return false;
                }
                struct AdfEntryBlock destEntry;
                if (adfReadEntryBlock(m_volume, walk, &destEntry) != ADF_RC_OK)
                    break;
                walk = (uint32_t)destEntry.parent;
            }
        }
    }

    // Reject a name collision in the destination up front (adfRenameEntry would fail, but this
    // gives a clearer diagnostic and avoids relying on adflib's error behaviour).
    if (FindFile(destSector, targetName) != 0) {
        Logger::Error("Error: '{}' already exists in the destination directory.", targetName);
        return false;
    }

    // Perform move using adfRenameEntry (which supports moving between directories)
    ADF_RETCODE rc = adfRenameEntry(m_volume, srcSector, const_cast<char*>(fileName.c_str()),
                                    destSector, const_cast<char*>(targetName.c_str()));

    if (rc != ADF_RC_OK) {
        Logger::Error("Failed to move file '{}' from sector {} to sector {}", fileName, srcSector,
                      destSector);
        return false;
    }

    InvalidateBlockTypesCache();
    return true;
}

bool AdfHandler::ImportFile(uint32_t parentSector, const std::string& hostPath)
{
    if (!RequireWritable("import"))
        return false;

    if (!m_volume)
        return false;

    // Open and read the host file, then delegate the actual ADF write to the buffer overload.
    std::filesystem::path path(hostPath);
    std::string filename = path.filename().string();
    uintmax_t fileSize = 0;

    try {
        fileSize = std::filesystem::file_size(path);
    } catch (const std::filesystem::filesystem_error& e) {
        Logger::Error("Error getting file size: {}", e.what());
        return false;
    }

    if (fileSize > constants::disk::MaxImportFileSize) {
        Logger::Error("File too large: {} bytes (max {} bytes)", fileSize,
                      constants::disk::MaxImportFileSize);
        return false;
    }

    std::ifstream inputFile(hostPath, std::ios::binary);
    if (!inputFile) {
        Logger::Error("Failed to open host file: {}", hostPath);
        return false;
    }

    std::vector<uint8_t> buffer(fileSize);
    if (fileSize > 0) {
        inputFile.read(reinterpret_cast<char*>(buffer.data()), fileSize);
        if (!inputFile) {
            Logger::Error("Failed to read full content of file: {}", hostPath);
            return false;
        }
    }

    return ImportFile(parentSector, filename, buffer);
}

bool AdfHandler::ImportFile(uint32_t parentSector, const std::string& fileName,
                            const std::vector<uint8_t>& data)
{
    if (!RequireWritable("import"))
        return false;

    if (!m_volume)
        return false;

    // 1. Resolve Parent Sector (0 -> Root)
    uint32_t targetSector = (parentSector == 0) ? m_volume->rootBlock : parentSector;

    // 2. Prepare ADF for Writing.
    // Set current directory to target by directly manipulating the volume struct;
    // adfChangeDir only accepts names, but we have the sector.
    ADF_SECTNUM oldDir = m_volume->curDirPtr;
    m_volume->curDirPtr = targetSector;

    // 3. Create/Open File in ADF
    struct AdfFile* file =
        adfFileOpen(m_volume, const_cast<char*>(fileName.c_str()), ADF_FILE_MODE_WRITE);
    if (!file) {
        Logger::Error("Failed to create file in ADF: {}", fileName);
        m_volume->curDirPtr = oldDir; // Restore
        return false;
    }

    // 4. Write Content
    long bytesWritten = 0;
    if (!data.empty()) {
        bytesWritten = adfFileWrite(file, data.size(), const_cast<unsigned char*>(data.data()));
    }

    adfFileClose(file);
    m_volume->curDirPtr = oldDir; // Restore

    if (!data.empty() && bytesWritten != (long)data.size()) {
        Logger::Error("Error writing to ADF. Expected {} bytes, wrote {}", data.size(),
                      bytesWritten);
        DeleteFile(targetSector, fileName);
        return false;
    }

    InvalidateBlockTypesCache();
    return true;
}

bool AdfHandler::CreateDirectory(uint32_t parentSector, const std::string& name)
{
    if (!RequireWritable("create directory"))
        return false;

    if (!m_volume)
        return false;

    // 1. Resolve Parent Sector
    uint32_t targetSector = (parentSector == 0) ? m_volume->rootBlock : parentSector;

    // 2. Create Directory
    // adfCreateDir(volume, parentDirSector, name)
    ADF_RETCODE rc = adfCreateDir(m_volume, targetSector, const_cast<char*>(name.c_str()));

    if (rc != ADF_RC_OK) {
        Logger::Error("Failed to create directory '{}' in sector {}", name, targetSector);
        return false;
    }

    InvalidateBlockTypesCache();
    return true;
}

uint32_t AdfHandler::FindFile(uint32_t parentSector, const std::string& name) const
{
    if (!m_volume)
        return 0;

    // 1. Resolve Parent Sector
    uint32_t targetSector = (parentSector == 0) ? m_volume->rootBlock : parentSector;

    // 2. Scan Directory (cycle-safe; adfGetDirEnt hangs on cyclic hash chains)
    for (const auto& e : FileOperations::EnumerateDir(m_volume, targetSector)) {
        if (e.name == name)
            return e.sector;
    }

    return 0;
}

bool AdfHandler::DeleteFile(uint32_t parentSector, const std::string& name)
{
    if (!RequireWritable("delete"))
        return false;

    if (!m_volume)
        return false;

    // 1. Resolve Parent Sector
    uint32_t targetSector = (parentSector == 0) ? m_volume->rootBlock : parentSector;

    // 2. Locate the entry to identify if it is a directory (cycle-safe enumeration)
    long entrySector = -1;
    bool isDir = false;
    for (const auto& e : FileOperations::EnumerateDir(m_volume, targetSector)) {
        if (e.name == name) {
            entrySector = e.sector;
            isDir = e.isDir;
            break;
        }
    }

    if (entrySector == -1) {
        // Entry not found
        Logger::Error("Check failed: '{}' not found in sector {}", name, targetSector);
        return false;
    }

    // 3. If Directory, Recursively Delete Contents
    if (isDir) {
        for (const auto& child : FileOperations::EnumerateDir(m_volume, (uint32_t)entrySector)) {
            // Recursively delete the child
            // Note: We ignore return value to try to delete as much as possible
            DeleteFile((uint32_t)entrySector, child.name);
        }
    }

    // 4. Remove Entry (File or now-empty Directory)
    ADF_RETCODE rc = adfRemoveEntry(m_volume, targetSector, const_cast<char*>(name.c_str()));

    if (rc != ADF_RC_OK) {
        Logger::Error("Failed to delete '{}' from sector {}", name, targetSector);
        return false;
    }

    InvalidateBlockTypesCache();
    return true;
}

bool AdfHandler::SetPermissions(uint32_t sector, int32_t access)
{
    if (!RequireWritable("set permissions"))
        return false;

    if (!m_volume)
        return false;

    // Read the entry block
    struct AdfEntryBlock entry;
    ADF_RETCODE rc = adfReadEntryBlock(m_volume, sector, &entry);
    if (rc != ADF_RC_OK) {
        Logger::Error("Failed to read entry block {} for permission change.", sector);
        return false;
    }

    // Update the access field
    entry.access = access;

    // Write the entry block back
    rc = adfWriteEntryBlock(m_volume, sector, &entry);
    if (rc != ADF_RC_OK) {
        Logger::Error("Failed to write entry block {} for permission change.", sector);
        return false;
    }

    return true;
}

bool AdfHandler::IsBlockFree(uint32_t blockNum) const
{
    if (!m_volume || blockNum < 2)
        return false;
    return adfIsBlockFree(m_volume, blockNum);
}

bool AdfHandler::RestoreDeletedFile(uint32_t headerBlock)
{
    if (!RequireWritable("restore"))
        return false;

    if (!m_volume)
        return false;

    // 1. Read the file header block
    struct AdfFileHeaderBlock fileHeader;
    if (adfReadEntryBlock(m_volume, headerBlock, (struct AdfEntryBlock*)&fileHeader) != ADF_RC_OK) {
        Logger::Error("Failed to read file header block {}", headerBlock);
        return false;
    }

    // Verify it's a file header
    if (fileHeader.type != ADF_T_HEADER || fileHeader.secType != ADF_ST_FILE) {
        Logger::Error("Block {} is not a file header", headerBlock);
        return false;
    }

    // 2. Get original filename and parent
    std::string originalName((const char*)fileHeader.fileName,
                             std::min((int)fileHeader.nameLen, 30));
    uint32_t parentSector = fileHeader.parent;

    // 3. Check if file already exists in parent directory, generate unique name if needed
    //    (cycle-safe enumeration; adfGetDirEnt hangs on cyclic hash chains)
    std::vector<FileOperations::DirEntry> siblings =
        FileOperations::EnumerateDir(m_volume, parentSector);
    auto nameExists = [&](const std::string& n) {
        return std::any_of(siblings.begin(), siblings.end(),
                           [&](const FileOperations::DirEntry& e) { return e.name == n; });
    };

    std::string restoreName = originalName;
    int suffix = 1;
    while (nameExists(restoreName) && suffix < 100) {
        // Generate new name with suffix
        std::string baseName = originalName;
        std::string ext;
        size_t dotPos = originalName.rfind('.');
        if (dotPos != std::string::npos && dotPos > 0) {
            baseName = originalName.substr(0, dotPos);
            ext = originalName.substr(dotPos);
        }
        restoreName = fmt::format("{}_{}{}", baseName, suffix, ext);
        if (restoreName.length() > 30) {
            // Truncate base name to fit
            size_t maxBase = 30 - ext.length() - fmt::format("_{}", suffix).length();
            baseName = baseName.substr(0, maxBase);
            restoreName = fmt::format("{}_{}{}", baseName, suffix, ext);
        }
        suffix++;
    }

    if (suffix >= 100) {
        Logger::Error("Could not find unique name for restored file");
        return false;
    }

    // 4. Mark all file blocks as used in bitmap
    // First, mark the header block
    if (adfIsBlockFree(m_volume, headerBlock)) {
        adfSetBlockUsed(m_volume, headerBlock);
    }

    // Mark data blocks from the header
    uint32_t totalBlocks = GetTotalBlocks();
    for (int i = 0; i < ADF_MAX_DATABLK; i++) {
        uint32_t dataBlock = fileHeader.dataBlocks[i];
        if (dataBlock != 0 && dataBlock < totalBlocks && adfIsBlockFree(m_volume, dataBlock)) {
            adfSetBlockUsed(m_volume, dataBlock);
        }
    }

    // Follow extension blocks with cycle detection
    uint32_t extBlock = fileHeader.extension;
    std::set<uint32_t> visitedExt;
    while (extBlock != 0 && extBlock < totalBlocks) {
        // visitedExt alone is complete cycle detection; the size cap is only a belt-and-braces
        // bound and must track the device, not DD's 1760. A hardfile chain longer than that used
        // to stop here, leaving real data blocks marked free for the bitmap repair to reclaim.
        if (visitedExt.count(extBlock) || visitedExt.size() >= totalBlocks)
            break;
        visitedExt.insert(extBlock);

        if (adfIsBlockFree(m_volume, extBlock)) {
            adfSetBlockUsed(m_volume, extBlock);
        }

        struct AdfFileExtBlock extHeader;
        if (adfReadFileExtBlock(m_volume, extBlock, &extHeader) != ADF_RC_OK) {
            break;
        }

        for (int i = 0; i < ADF_MAX_DATABLK; i++) {
            uint32_t dataBlock = extHeader.dataBlocks[i];
            if (dataBlock != 0 && dataBlock < totalBlocks && adfIsBlockFree(m_volume, dataBlock)) {
                adfSetBlockUsed(m_volume, dataBlock);
            }
        }

        extBlock = extHeader.extension;
    }

    // 5. Update filename in header if changed
    if (restoreName != originalName) {
        memset(fileHeader.fileName, 0, sizeof(fileHeader.fileName));
        fileHeader.nameLen = (char)restoreName.length();
        memcpy(fileHeader.fileName, restoreName.c_str(), restoreName.length());

        // Recalculate checksum
        fileHeader.checkSum = 0;
        fileHeader.checkSum =
            adfNormalSum((uint8_t*)&fileHeader, offsetof(struct AdfFileHeaderBlock, checkSum),
                         m_volume->blockSize);

        if (adfWriteFileHdrBlock(m_volume, headerBlock, &fileHeader) != ADF_RC_OK) {
            Logger::Error("Failed to update file header with new name");
            return false;
        }
    }

    // 6. Add entry to parent directory hash table
    // calcHashValue already reduces the hash modulo ADF_HT_SIZE, so it is the slot index
    // directly. The on-disk hash table (root and userdir blocks alike) has exactly
    // ADF_HT_SIZE (72) entries, so the slot is guaranteed in-bounds for parentBlock.hashTable.
    uint32_t hashSlot = calcHashValue((uint8_t*)restoreName.c_str(), adfVolHasINTL(m_volume));

    // Read parent directory block
    struct AdfEntryBlock parentBlock;
    if (adfReadEntryBlock(m_volume, parentSector, &parentBlock) != ADF_RC_OK) {
        Logger::Error("Failed to read parent directory block {}", parentSector);
        return false;
    }

    int32_t* hashTable = parentBlock.hashTable;
    uint32_t oldHead = hashTable[hashSlot];

    // If slot is occupied, need to add to chain
    if (oldHead != 0) {
        // Update the file header's nextSameHash to point to old head
        fileHeader.nextSameHash = oldHead;
        fileHeader.checkSum = 0;
        fileHeader.checkSum =
            adfNormalSum((uint8_t*)&fileHeader, offsetof(struct AdfFileHeaderBlock, checkSum),
                         m_volume->blockSize);
        if (adfWriteFileHdrBlock(m_volume, headerBlock, &fileHeader) != ADF_RC_OK) {
            Logger::Error("Failed to update file header hash chain");
            return false;
        }
    }

    // Point hash table slot to our restored file
    hashTable[hashSlot] = (int32_t)headerBlock;

    // Recalculate parent checksum and write
    parentBlock.checkSum = 0;
    parentBlock.checkSum = adfNormalSum(
        (uint8_t*)&parentBlock, offsetof(struct AdfEntryBlock, checkSum), m_volume->blockSize);
    if (adfWriteEntryBlock(m_volume, parentSector, &parentBlock) != ADF_RC_OK) {
        Logger::Error("Failed to update parent directory hash table");
        return false;
    }

    // 7. Update bitmap on disk
    if (adfUpdateBitmap(m_volume) != ADF_RC_OK) {
        Logger::Error("Warning: Failed to update bitmap on disk");
    }

    InvalidateBlockTypesCache();
    fmt::print("Restored file '{}' to sector {}\n", restoreName, headerBlock);
    return true;
}

// Recursively collect every block belonging to a directory subtree or file.
// Used by ScrubFile so that directory scrubs zero children's blocks too.
static void CollectAllBlocksInTree(struct AdfVolume* vol, uint32_t blockNum,
                                   std::vector<uint32_t>& blocks, std::set<uint32_t>& visited)
{
    if (!vol || !visited.insert(blockNum).second)
        return;

    struct AdfEntryBlock entry;
    if (adfReadEntryBlock(vol, blockNum, &entry) != ADF_RC_OK)
        return;

    if (entry.secType == ADF_ST_FILE) {
        struct AdfFileHeaderBlock* fhdr = reinterpret_cast<struct AdfFileHeaderBlock*>(&entry);
        struct AdfFileBlocks fileBlocks;
        if (adfGetFileBlocks(vol, fhdr, &fileBlocks) == ADF_RC_OK) {
            blocks.push_back(fileBlocks.header);
            for (unsigned int i = 0; i < fileBlocks.data.nItems; i++)
                blocks.push_back(fileBlocks.data.sectors[i]);
            for (unsigned int i = 0; i < fileBlocks.extens.nItems; i++)
                blocks.push_back(fileBlocks.extens.sectors[i]);
            if (fileBlocks.data.sectors)
                free(fileBlocks.data.sectors);
            if (fileBlocks.extens.sectors)
                free(fileBlocks.extens.sectors);
        } else {
            blocks.push_back(blockNum);
        }
    } else if (entry.secType == ADF_ST_DIR || entry.secType == ADF_ST_ROOT) {
        blocks.push_back(blockNum);
        for (const auto& child : FileOperations::EnumerateDir(vol, blockNum))
            CollectAllBlocksInTree(vol, child.sector, blocks, visited);
    } else {
        blocks.push_back(blockNum);
    }
}

bool AdfHandler::ScrubFile(uint32_t headerBlock)
{
    if (!RequireWritable("scrub"))
        return false;

    if (!m_volume)
        return false;

    // 1. Read the entry to determine type and collect ALL blocks to zero.
    //    For directories this must happen BEFORE deletion so we can still walk the tree.
    struct AdfEntryBlock entry;
    if (adfReadEntryBlock(m_volume, headerBlock, &entry) != ADF_RC_OK) {
        Logger::Error("Scrub failed: Could not read entry block {}", headerBlock);
        return false;
    }

    std::vector<uint32_t> blocks;
    if (entry.secType == ADF_ST_FILE) {
        blocks = GetFileBlocks(headerBlock);
        if (blocks.empty()) {
            Logger::Error("Scrub failed: Could not retrieve file blocks for header {}",
                          headerBlock);
            return false;
        }
    } else {
        // Directory (or unknown): walk the entire subtree before deletion so that
        // children's file-header blocks get zeroed and don't surface as deleted files.
        std::set<uint32_t> visited;
        CollectAllBlocksInTree(m_volume, headerBlock, blocks, visited);
        if (blocks.empty()) {
            Logger::Error("Scrub failed: Could not collect blocks for directory {}", headerBlock);
            return false;
        }
    }

    // 2. Check if file/dir is active or already deleted
    bool isDeleted = adfIsBlockFree(m_volume, headerBlock);

    // 3. If Active: Delete it first (standard method)
    if (!isDeleted) {
        std::string name((const char*)entry.name, std::min((int)entry.nameLen, 30));

        if (!DeleteFile(entry.parent, name)) {
            Logger::Error("Scrub failed: Standard delete failed for '{}'", name);
            return false;
        }
    }

    // 4. Overwrite all blocks with zeros.
    //    For an already-deleted entry the block list comes from the stale header chain, so a
    //    block may since have been reallocated to a live file. Zeroing such a block would
    //    destroy the new owner's data, so skip any block that is no longer free. (For the
    //    active case we just deleted the entry above, so all its blocks are free.)
    std::vector<uint8_t> zeros(512, 0);

    int scrubbedCount = 0;
    for (uint32_t sector : blocks) {
        if (isDeleted && !adfIsBlockFree(m_volume, sector)) {
            Logger::Error("Scrub: skipping sector {} — reallocated to a live file", sector);
            continue;
        }
        if (adfDevWriteBlock(m_volume->dev, sector + GetPartitionOffset(), 512, zeros.data()) !=
            ADF_RC_OK) {
            Logger::Error("Warning: Failed to scrub sector {}", sector);
        } else {
            scrubbedCount++;
        }
    }

    InvalidateBlockTypesCache();
    fmt::print("Scrubbed {} blocks for header {}\n", scrubbedCount, headerBlock);
    return true;
}

void AdfHandler::Unmount()
{
    m_blockTypesCacheValid = false;
    m_blockTypesCache.clear();

    if (m_volume) {
        adfVolUnMount(m_volume);
        m_volume = nullptr;
    }

    // Don't call adfDevClose here - MemoryDevice owns the device and will close it
    m_device = nullptr;

    // Close the memory device (which will close the underlying AdfDevice)
    if (m_memDevice) {
        m_memDevice->Close();
    }
}

bool AdfHandler::IsMounted() const { return m_device != nullptr; }

bool AdfHandler::HasFileSystem() const { return m_volume != nullptr; }

bool AdfHandler::IsPartitionedDevice() const
{
    return m_device &&
           (m_device->klass == ADF_DEVCLASS_HARDDISK || m_device->klass == ADF_DEVCLASS_HARDFILE);
}

std::vector<AdfHandler::PartitionInfo> AdfHandler::GetPartitions() const
{
    std::vector<PartitionInfo> partitions;
    if (!IsPartitionedDevice() || !m_device->mounted || !m_device->volList)
        return partitions;
    for (int i = 0; i < m_device->nVol; ++i) {
        const AdfVolume* vol = m_device->volList[i];
        if (!vol)
            continue;
        PartitionInfo info;
        info.index = i;
        info.name = vol->volName ? vol->volName : fmt::format("Partition {}", i);
        info.firstBlock = static_cast<uint32_t>(vol->firstBlock);
        info.lastBlock = static_cast<uint32_t>(vol->lastBlock);
        info.dosFilesystem = std::strncmp(vol->fs.id, "DOS", 3) == 0;
        std::string id;
        for (int k = 0; k < 3 && vol->fs.id[k]; ++k)
            id.push_back(std::isprint(static_cast<unsigned char>(vol->fs.id[k])) ? vol->fs.id[k]
                                                                                 : '?');
        info.filesystem = vol->fs.type < 10 ? fmt::format("{}{}", id, vol->fs.type)
                                            : fmt::format("{}${:02X}", id, vol->fs.type);
        partitions.push_back(std::move(info));
    }
    return partitions;
}

bool AdfHandler::SelectPartition(int index)
{
    if (!m_device)
        return false;
    if (!IsPartitionedDevice())
        return index == 0 && m_volume != nullptr;
    if (index >= m_device->nVol || index < -1)
        return false;

    m_blockTypesCacheValid = false;
    m_blockTypesCache.clear();
    m_blockToHeaderMap.clear();
    m_freeBlocksCache = -1;
    if (m_volume) {
        adfVolUnMount(m_volume);
        m_volume = nullptr;
    }
    m_partitionIndex = index;
    if (index < 0)
        return true;
    m_volume = MountWritableVolume(m_device, index);
    if (!m_volume) {
        Logger::Warn("AdfWrapper: partition {} has no filesystem adflib can mount; showing the "
                     "whole image raw",
                     index);
        m_partitionIndex = -1;
        return false;
    }
    Logger::Info("AdfWrapper: browsing partition {} ('{}')", index,
                 m_volume->volName ? m_volume->volName : "");
    return true;
}

uint32_t AdfHandler::GetPartitionOffset() const
{
    // Mounted volumes address blocks from their own first block (adfVolReadBlock adds it);
    // the raw device is addressed from block 0. A floppy's volume starts at 0 either way.
    return m_volume ? static_cast<uint32_t>(m_volume->firstBlock) : 0;
}

FileNode AdfHandler::GetFileTree() const { return FileOperations::GetFileTree(m_volume); }

DiskInfo AdfHandler::GetDiskInfo() const
{
    DiskInfo info;
    info.sizeBytes = 0;
    info.freeBytes = 0;
    info.usedBytes = 0;
    info.usedPercentage = 0.0f;
    info.isReadOnly = false;
    info.cylinders = 0;
    info.heads = 0;
    info.sectors = 0;

    struct AdfDevice* dev = m_device;
    if (m_volume && m_volume->dev) {
        dev = m_volume->dev;
    }

    if (dev) {
        info.cylinders = dev->geometry.cylinders;
        info.heads = dev->geometry.heads;
        info.sectors = dev->geometry.sectors;
        // A partition covers only some of the image's cylinders. Report those, so
        // cylinders * heads * sectors is the partition's block count like it is a floppy's.
        const uint32_t cylinderBlocks = info.heads * info.sectors;
        if (m_volume && IsPartitionedDevice() && cylinderBlocks > 0)
            info.cylinders = GetTotalBlocks() / cylinderBlocks;
    }

    if (m_volume) {
        info.name = m_volume->volName ? m_volume->volName : "Unnamed";
        // A hard-disk partition's volName is the RDB drive name (DH0); the label a user knows is
        // in its root block.
        if (IsPartitionedDevice()) {
            struct AdfRootBlock rootBlk;
            if (adfReadRootBlock(m_volume, static_cast<uint32_t>(m_volume->rootBlock), &rootBlk) ==
                    ADF_RC_OK &&
                rootBlk.nameLen > 0) {
                const size_t length = std::min<size_t>(rootBlk.nameLen, sizeof(rootBlk.diskName));
                info.name.assign(rootBlk.diskName, rootBlk.diskName + length);
            }
        }

        if (adfVolIsFFS(m_volume)) {
            info.type = "FFS";
        } else if (adfVolIsOFS(m_volume)) {
            info.type = "OFS";
        } else if (adfVolHasDIRCACHE(m_volume)) {
            info.type = "DC";
        } else {
            info.type = "AmigaDOS";
        }

        uint32_t blockSize = m_volume->blockSize;
        uint32_t totalBlocks = adfVolGetSizeInBlocks(m_volume);
        uint32_t freeBlocks = GetFreeBlocks(); // cached — this runs once per frame per document
        uint32_t usedBlocks = totalBlocks - freeBlocks;

        info.sizeBytes = totalBlocks * blockSize;
        info.freeBytes = freeBlocks * blockSize;
        info.usedBytes = usedBlocks * blockSize;

        if (totalBlocks > 0)
            info.usedPercentage = (float)usedBlocks / (float)totalBlocks;

        info.isReadOnly = m_volume->readOnly;
    } else if (m_device) {
        info.name = "Raw Device";
        info.type = "NDOS";
        uint32_t blockSize = 512;
        info.sizeBytes = m_device->sizeBlocks * blockSize;
        info.isReadOnly = m_device->readOnly;
        info.usedBytes = info.sizeBytes;
        info.usedPercentage = 1.0f;
    }

    return info;
}

RootBlockInfo AdfHandler::GetRootBlockInfo() const
{
    RootBlockInfo info;
    info.hasRoot = false;
    info.blockSize = 0;
    info.rootPtr = 0;
    info.diskType = 0;
    info.firstData = 0;
    info.lastData = 0;

    if (m_volume) {
        info.hasRoot = true;
        info.volumeName = m_volume->volName ? m_volume->volName : "";
        info.blockSize = m_volume->blockSize;
        info.rootPtr = m_volume->rootBlock;
        info.firstData = m_volume->firstBlock;
        info.lastData = m_volume->lastBlock;

        // Manually read root block to get dates
        struct AdfRootBlock rootBlk;
        if (adfReadRootBlock(m_volume, (uint32_t)m_volume->rootBlock, &rootBlk) == ADF_RC_OK) {
            auto convert = [](int32_t days, int32_t mins, int32_t ticks) {
                DateTime dt;
                adfDays2Date(days, &dt.year, &dt.month, &dt.day);
                dt.hour = mins / 60;
                dt.min = mins % 60;
                dt.sec = ticks / 50;
                return dt;
            };

            info.creationDate = convert(rootBlk.coDays, rootBlk.coMins, rootBlk.coTicks);
            info.modificationDate = convert(rootBlk.days, rootBlk.mins, rootBlk.ticks);

            // Extended Info
            info.checkSum = rootBlk.checkSum;
            info.bmFlag = rootBlk.bmFlag;
            info.bmExt = rootBlk.bmExt;
            info.dirCachePtr = rootBlk.extension;

            // Hash Table (72 entries)
            info.hashTable.assign(std::begin(rootBlk.hashTable), std::end(rootBlk.hashTable));

            // Bitmap Pages (25 entries)
            info.bmPages.assign(std::begin(rootBlk.bmPages), std::end(rootBlk.bmPages));
        }
    }

    return info;
}

bool AdfHandler::RevertBlock(uint32_t block)
{
    if (!RequireWritable("revert block"))
        return false;

    if (!m_memDevice)
        return false;

    return m_memDevice->RevertBlock(block + GetPartitionOffset());
}

std::vector<uint8_t> AdfHandler::ReadBlock(uint32_t blockNum) const
{
    return BlockReader::ReadBlock(m_volume, m_device, blockNum);
}

std::vector<uint8_t> AdfHandler::ReadBlockFromDisk(uint32_t blockNum) const
{
    if (!m_memDevice) {
        return {};
    }
    return m_memDevice->ReadBlockFromDisk(blockNum + GetPartitionOffset());
}

std::vector<bool> AdfHandler::GetBlockUsage() const { return BlockReader::GetBlockUsage(m_volume); }

std::vector<std::string> AdfHandler::GetAllBlockTypes() const
{
    if (m_blockTypesCacheValid)
        return m_blockTypesCache;

    m_blockToHeaderMap.clear();
    RootBlockInfo rootInfo = GetRootBlockInfo();
    auto isCached = [this](uint32_t block) { return IsBlockCached(block); };
    m_blockTypesCache = BlockAnalyzer::GetAllBlockTypes(m_volume, m_device, rootInfo, isCached,
                                                        &m_blockToHeaderMap);
    m_blockTypesCacheValid = true;
    return m_blockTypesCache;
}

void AdfHandler::InvalidateBlockTypesCache()
{
    m_blockTypesCacheValid = false;
    m_freeBlocksCache = -1;
}

std::vector<bool> AdfHandler::GetZeroFilledBlocks() const
{
    auto isCached = [this](uint32_t block) { return IsBlockCached(block); };
    return BlockReader::GetZeroFilledBlocks(m_volume, m_device, isCached);
}

std::vector<uint32_t> AdfHandler::GetFileBlocks(uint32_t headerBlock) const
{
    return FileOperations::GetFileBlocks(m_volume, headerBlock);
}

std::string AdfHandler::ReadFileContent(uint32_t headerBlock) const
{
    return FileOperations::ReadFileContent(m_volume, headerBlock,
                                           [this](uint32_t block) { return ReadBlock(block); });
}

BlockDetails AdfHandler::AnalyzeBlock(uint32_t blockNum, std::span<const uint8_t> data) const
{
    BlockDetails details = BlockAnalyzer::AnalyzeBlock(
        m_volume, m_device, blockNum, data, [this](uint32_t block) { return ReadBlock(block); });

    // Inject parent info for FFS Data blocks if known
    if (details.fileHeader == 0 && m_blockToHeaderMap.count(blockNum)) {
        details.fileHeader = m_blockToHeaderMap.at(blockNum);
        details.isDataBlock = true;
    }

    // Detect deleted files: FileHeader blocks that are marked as free in the bitmap
    if (m_volume && blockNum >= 2 && details.primaryType == ADF_T_HEADER &&
        details.secondaryType == ADF_ST_FILE && adfIsBlockFree(m_volume, blockNum)) {
        details.isDeletedFile = true;
    }

    return details;
}

std::vector<FileNode> AdfHandler::GetDeletedFiles() const
{
    std::vector<FileNode> deletedFiles;
    if (!m_volume)
        return deletedFiles;

    uint32_t totalBlocks = adfVolGetSizeInBlocks(m_volume);
    std::vector<uint8_t> buffer(512);

    for (uint32_t blockNum = 2; blockNum < totalBlocks; ++blockNum) {
        // Only check blocks that are marked as free
        if (!adfIsBlockFree(m_volume, blockNum))
            continue;

        // Skip unread blocks if using lazy loading
        if (!IsBlockCached(blockNum))
            continue;

        // Read the block
        if (adfVolReadBlock(m_volume, blockNum, buffer.data()) != ADF_RC_OK)
            continue;

        // Check if it's a file header block
        int32_t primaryType = (int32_t)swapUint32fromPtr(buffer.data());
        int32_t secondaryType = (int32_t)swapUint32fromPtr(buffer.data() + 508);

        if (primaryType != ADF_T_HEADER || secondaryType != ADF_ST_FILE)
            continue;

        // Verify checksum to ensure it's a valid file header. Recompute over the block with
        // the checksum field zeroed; save/restore those 4 bytes in place rather than copying
        // the whole 512-byte block on every iteration.
        uint32_t storedChecksum = (uint32_t)swapUint32fromPtr(buffer.data() + 20);
        const uint8_t savedChecksum[4] = {buffer[20], buffer[21], buffer[22], buffer[23]};
        buffer[20] = buffer[21] = buffer[22] = buffer[23] = 0;
        uint32_t calculatedChecksum = (uint32_t)adfNormalSum(buffer.data(), 20, 512);
        buffer[20] = savedChecksum[0];
        buffer[21] = savedChecksum[1];
        buffer[22] = savedChecksum[2];
        buffer[23] = savedChecksum[3];
        if (storedChecksum != calculatedChecksum)
            continue;

        // Extract file information via the shared header parser. The parent sector for a
        // deleted file lives in the header itself (offset 500), so read it and pass it in.
        int32_t parentSector = (int32_t)swapUint32fromPtr(buffer.data() + 500);
        FileNode node = FileOperations::ParseEntryBlock(buffer.data(), blockNum, parentSector);
        node.isDir = false;
        node.isDeleted = true;

        deletedFiles.push_back(std::move(node));
    }

    return deletedFiles;
}

bool AdfHandler::ClearBlock(uint32_t blockNum)
{
    if (!RequireWritable("clear block"))
        return false;

    if (!m_volume)
        return false;

    // We'll create a zero-filled buffer
    std::vector<uint8_t> buffer(m_volume->blockSize, 0);

    // Use adflib to write the block - MemoryDevice will track modification
    if (adfVolWriteBlock(m_volume, blockNum, buffer.data()) != ADF_RC_OK) {
        Logger::Error("Failed to clear block {}", blockNum);
        return false;
    }

    InvalidateBlockTypesCache();
    return true;
}

bool AdfHandler::WriteBlock(uint32_t blockNum, const std::vector<uint8_t>& data)
{
    if (!RequireWritable("write block"))
        return false;

    if (m_volume) {
        // Normal path: write via adflib volume layer
        if (data.size() != m_volume->blockSize) {
            Logger::Error("WriteBlock: data size {} doesn't match block size {}", data.size(),
                          m_volume->blockSize);
            return false;
        }

        if (adfVolWriteBlock(m_volume, blockNum, data.data()) != ADF_RC_OK) {
            Logger::Error("Failed to write block {}", blockNum);
            return false;
        }
    } else if (m_memDevice) {
        // Raw/NDOS path: write directly to memory device
        if (data.size() != 512) {
            Logger::Error("WriteBlock: data size {} doesn't match block size 512", data.size());
            return false;
        }

        if (!m_memDevice->WriteSector(blockNum, 1, data.data())) {
            Logger::Error("Failed to write block {}", blockNum);
            return false;
        }
    } else {
        return false;
    }

    InvalidateBlockTypesCache();
    return true;
}

uint32_t AdfHandler::GetTotalBlocks() const
{
    // Read geometry directly. Going through GetDiskInfo() here would also run
    // adfCountFreeBlocks() (a full bitmap scan) just to multiply three numbers.
    struct AdfDevice* dev = m_device;
    if (m_volume && m_volume->dev)
        dev = m_volume->dev;

    if (!dev)
        return 0;

    // A hard-disk image: the mounted partition, or the whole image when shown raw.
    if (IsPartitionedDevice())
        return m_volume ? static_cast<uint32_t>(m_volume->lastBlock - m_volume->firstBlock + 1)
                        : dev->sizeBlocks;
    return dev->geometry.cylinders * dev->geometry.heads * dev->geometry.sectors;
}

bool AdfHandler::IsBlockModified(uint32_t blockNum) const
{
    if (!m_memDevice)
        return false;
    return m_memDevice->IsBlockModified(blockNum + GetPartitionOffset());
}

const std::set<uint32_t>& AdfHandler::GetModifiedBlocks() const
{
    static std::set<uint32_t> empty;
    if (!m_memDevice)
        return empty;
    const uint32_t offset = GetPartitionOffset();
    if (offset == 0 && !IsPartitionedDevice())
        return m_memDevice->GetModifiedBlocks();
    const uint32_t total = GetTotalBlocks();
    m_partitionModifiedBlocks.clear();
    for (uint32_t block : m_memDevice->GetModifiedBlocks()) {
        if (block >= offset && block - offset < total)
            m_partitionModifiedBlocks.insert(block - offset);
    }
    return m_partitionModifiedBlocks;
}

std::vector<bool> AdfHandler::GetBlockModificationStatus() const
{
    if (!m_memDevice)
        return {};
    auto status = m_memDevice->GetBlockModificationStatus();
    const uint32_t offset = GetPartitionOffset();
    if (offset == 0 && !IsPartitionedDevice())
        return status;
    const uint32_t total = GetTotalBlocks();
    if (offset >= status.size())
        return std::vector<bool>(total, false);
    const auto begin = status.begin() + offset;
    const auto end = status.begin() + std::min<size_t>(status.size(), size_t(offset) + total);
    std::vector<bool> slice(begin, end);
    slice.resize(total, false);
    return slice;
}

bool AdfHandler::HasUnsavedChanges() const
{
    if (!m_memDevice)
        return false;
    return m_memDevice->HasUnsavedChanges();
}

bool AdfHandler::Save()
{
    if (!m_memDevice || !m_memDevice->IsOpen()) {
        Logger::Error("No device mounted, cannot save");
        return false;
    }

    return m_memDevice->Save();
}

bool AdfHandler::SaveAs(const std::string& path)
{
    if (!m_memDevice || !m_memDevice->IsOpen()) {
        Logger::Error("No device mounted, cannot save");
        return false;
    }

    if (m_memDevice->SaveAs(path)) {
        m_memDevice->SetFilePath(path);
        return true;
    }
    return false;
}

void AdfHandler::ClearModifiedState()
{
    if (m_memDevice) {
        m_memDevice->ClearModifiedState();
    }
}

const std::string& AdfHandler::GetFilePath() const
{
    static std::string empty;
    if (!m_memDevice)
        return empty;
    return m_memDevice->GetFilePath();
}

// Helper: sanitize filename for host filesystem
static std::string SanitizeFilename(const std::string& name)
{
    std::string result;
    result.reserve(name.size());

    for (char c : name) {
        // Replace characters that are problematic on various filesystems
        switch (c) {
            case '/':
            case '\\':
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
            case '|':
                result += '_';
                break;
            default:
                // Keep printable ASCII and extended chars
                if (c >= 32 || c < 0) {
                    result += c;
                } else {
                    result += '_';
                }
                break;
        }
    }

    // Avoid reserved names on Windows (CON, PRN, AUX, NUL, COM1-9, LPT1-9)
    std::string upper = result;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" ||
        (upper.size() == 4 && (upper.substr(0, 3) == "COM" || upper.substr(0, 3) == "LPT") &&
         upper[3] >= '1' && upper[3] <= '9')) {
        result = "_" + result;
    }

    return result.empty() ? "_unnamed_" : result;
}

bool AdfHandler::ExportFile(uint32_t headerSector, const std::string& destPath) const
{
    if (!m_volume)
        return false;

    // Validate that the sector really is a file header before exporting. ReadFileContent()
    // returns an empty string both for a genuinely empty file and for an invalid/failed read,
    // so without this check a bad sector would silently produce a bogus 0-byte file.
    struct AdfEntryBlock entry;
    if (adfReadEntryBlock(m_volume, headerSector, &entry) != ADF_RC_OK) {
        Logger::Error("Export failed: cannot read entry block {}", headerSector);
        return false;
    }
    if (entry.secType != ADF_ST_FILE) {
        Logger::Error("Export failed: block {} is not a file", headerSector);
        return false;
    }

    std::string content = ReadFileContent(headerSector);

    // A header that claims a non-zero size but yields no content indicates a read failure
    // (e.g. a broken data-block chain), not a legitimately empty file — fail loudly.
    uint32_t expectedSize = reinterpret_cast<struct AdfFileHeaderBlock*>(&entry)->byteSize;
    if (expectedSize > 0 && content.empty()) {
        Logger::Error("Export failed: file at block {} claims {} bytes but no content is readable",
                      headerSector, expectedSize);
        return false;
    }

    std::ofstream outFile(destPath, std::ios::binary);
    if (!outFile) {
        return false;
    }

    outFile.write(content.data(), static_cast<std::streamsize>(content.size()));
    return outFile.good();
}

AdfHandler::ExportResult AdfHandler::ExportNode(const FileNode& node, const std::string& destPath,
                                                bool preserveStructure) const
{
    ExportResult result;
    std::filesystem::path destDir(destPath);

    // Ensure destination directory exists
    try {
        if (!std::filesystem::exists(destDir)) {
            std::filesystem::create_directories(destDir);
        }
    } catch (const std::exception& e) {
        result.errors++;
        result.errorMessages.push_back(
            fmt::format("Failed to create directory '{}': {}", destPath, e.what()));
        return result;
    }

    if (node.isDir) {
        // For directories, create the directory and recurse into children
        std::filesystem::path nodePath = destDir;
        if (preserveStructure && !node.name.empty() && node.name != "Root") {
            nodePath = destDir / SanitizeFilename(node.name);
            try {
                if (!std::filesystem::exists(nodePath)) {
                    std::filesystem::create_directory(nodePath);
                    result.directoriesCreated++;
                }
            } catch (const std::exception& e) {
                result.errors++;
                result.errorMessages.push_back(fmt::format("Failed to create directory '{}': {}",
                                                           nodePath.string(), e.what()));
                return result;
            }
        }

        // Recurse into children
        for (const auto& child : node.children) {
            auto childResult = ExportNode(child, nodePath.string(), preserveStructure);
            result.filesExported += childResult.filesExported;
            result.directoriesCreated += childResult.directoriesCreated;
            result.errors += childResult.errors;
            result.errorMessages.insert(result.errorMessages.end(),
                                        childResult.errorMessages.begin(),
                                        childResult.errorMessages.end());
        }
    } else {
        // For files, export to the destination
        std::string sanitizedName = SanitizeFilename(node.name);
        std::filesystem::path filePath = destDir / sanitizedName;

        // Handle duplicate names by appending a number
        if (std::filesystem::exists(filePath)) {
            int counter = 1;
            std::filesystem::path stem = filePath.stem();
            std::filesystem::path ext = filePath.extension();
            do {
                filePath = destDir / (stem.string() + "_" + std::to_string(counter) + ext.string());
                counter++;
            } while (std::filesystem::exists(filePath) && counter < 1000);

            // If we exhausted the counter, fail loudly rather than overwriting an existing file.
            if (std::filesystem::exists(filePath)) {
                result.errors++;
                result.errorMessages.push_back(fmt::format(
                    "Could not find a unique name to export '{}' into '{}'", node.name, destPath));
                return result;
            }
        }

        if (ExportFile(static_cast<uint32_t>(node.startSector), filePath.string())) {
            result.filesExported++;
        } else {
            result.errors++;
            result.errorMessages.push_back(
                fmt::format("Failed to export file '{}' to '{}'", node.name, filePath.string()));
        }
    }

    return result;
}

AdfHandler::ExportResult AdfHandler::ExportNodes(const std::vector<FileNode>& nodes,
                                                 const std::string& destPath,
                                                 bool preserveStructure) const
{
    ExportResult result;

    for (const auto& node : nodes) {
        auto nodeResult = ExportNode(node, destPath, preserveStructure);
        result.filesExported += nodeResult.filesExported;
        result.directoriesCreated += nodeResult.directoriesCreated;
        result.errors += nodeResult.errors;
        result.errorMessages.insert(result.errorMessages.end(), nodeResult.errorMessages.begin(),
                                    nodeResult.errorMessages.end());
    }

    return result;
}

const std::vector<uint8_t>& AdfHandler::GetFileContent() const
{
    static const std::vector<uint8_t> empty;
    if (m_memDevice && m_memDevice->IsOpen()) {
        return m_memDevice->GetData();
    }
    return empty;
}

bool AdfHandler::IsBlockCached(uint32_t blockNum) const
{
    if (m_memDevice) {
        return m_memDevice->IsBlockCached(blockNum + GetPartitionOffset());
    }
    return true; // Default to true if no memory device
}

uint32_t AdfHandler::CalculateRequiredBlocks(uint64_t sizeBytes) const
{
    if (!m_volume)
        return 0;

    // OFS: 488 bytes data per block. FFS: 512 bytes data per block.
    uint32_t dataBlockSize = adfVolIsFFS(m_volume) ? 512 : 488;

    if (sizeBytes == 0)
        return 1; // Just header

    uint32_t numDataBlocks = (sizeBytes + dataBlockSize - 1) / dataBlockSize;

    // Header holds 72 pointers
    // Extension block holds 72 pointers
    uint32_t numExtensionBlocks = 0;
    if (numDataBlocks > 72) {
        uint32_t remaining = numDataBlocks - 72;
        numExtensionBlocks = (remaining + 72 - 1) / 72;
    }

    return 1 + numDataBlocks + numExtensionBlocks; // Header + Data + Ext
}

std::string AdfHandler::InsufficientSpaceMessage(const std::string& operation,
                                                 uint32_t requiredBlocks, uint32_t freeBlocks)
{
    return fmt::format("Not enough space on disk.\n\nRequired: {} blocks\nFree: {} blocks\n\n{} "
                       "cancelled.",
                       requiredBlocks, freeBlocks, operation);
}

uint32_t AdfHandler::CalculateRequiredBlocks(const std::vector<std::string>& paths) const
{
    uint32_t total = 0;
    for (const auto& p : paths) {
        std::filesystem::path path(p);
        if (std::filesystem::exists(path)) {
            if (std::filesystem::is_directory(path)) {
                total += 1; // Directory header
                // Recurse
                std::vector<std::string> children;
                for (const auto& entry : std::filesystem::directory_iterator(path)) {
                    children.push_back(entry.path().string());
                }
                total += CalculateRequiredBlocks(children);
            } else if (std::filesystem::is_regular_file(path)) {
                total += CalculateRequiredBlocks(std::filesystem::file_size(path));
            }
        }
    }
    return total;
}

uint32_t AdfHandler::GetFreeBlocks() const
{
    if (!m_volume)
        return 0;
    if (m_freeBlocksCache < 0)
        m_freeBlocksCache = adfCountFreeBlocks(m_volume);
    return static_cast<uint32_t>(m_freeBlocksCache);
}

} // namespace adm::core
