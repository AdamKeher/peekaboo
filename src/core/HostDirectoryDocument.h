#pragma once

#include "HostMountService.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace adm::core {

enum class DocumentKind { DiskImage, HostDirectory };

// Storage-neutral identity used by document browsers. ADF backends use "adf:<block>" while
// directory backends use a normalized, root-relative UTF-8 path. Callers must not interpret it.
struct DocumentEntryId
{
    std::string value;

    static DocumentEntryId AdfBlock(uint32_t block);
    static std::optional<DocumentEntryId> RelativePath(const std::filesystem::path& path,
                                                       std::string& outError);
    bool operator==(const DocumentEntryId&) const = default;
    bool empty() const { return value.empty(); }
};

struct DocumentCapabilities
{
    bool browse = true;
    bool importFiles = false;
    bool createDirectory = false;
    bool rename = false;
    bool deleteEntries = false;
    bool rawBlocks = false;
    bool allocationMap = false;
    bool bootBlock = false;
    bool saveImage = false;
    // Block-level writes: scrub, permissions, restore, hex edits. Separate from
    // rawBlocks, which also drives whether the Flags column is shown by default -
    // a write-protected image must grey its buttons, not hide a column.
    bool writeBlocks = true;
};

struct DirectoryFingerprint
{
    uint64_t entryCount = 0;
    uint64_t totalBytes = 0;
    uint64_t newestWriteTime = 0;
    uint64_t nameHash = 1469598103934665603ULL;

    bool operator==(const DirectoryFingerprint&) const = default;
    std::string ToString() const;
    static std::optional<DirectoryFingerprint> Parse(const std::string& value);
};

struct HostDirectoryEntry
{
    DocumentEntryId id;
    DocumentEntryId parentId;
    std::string name;
    std::filesystem::path relativePath;
    bool isDirectory = false;
    bool isSymlink = false;
    bool symlinkConfined = true;
    bool amigaNameCollision = false;
    uint64_t size = 0;
    std::filesystem::file_time_type modified{};
};

struct HostDirectoryScan
{
    uint64_t generation = 0;
    std::vector<HostDirectoryEntry> entries;
    DirectoryFingerprint fingerprint;
    uint64_t capacityBytes = 0;
    uint64_t freeBytes = 0;
    std::string error;
};

// Filesystem backend for HostDirectory documents. All IDs and mutations are root-relative and
// revalidated immediately before use; directory symlinks are listed but never traversed.
class HostDirectoryDocument
{
public:
    HostDirectoryDocument() = default;
    ~HostDirectoryDocument();

    bool Open(const std::filesystem::path& path, bool readOnly, std::string& outError,
              std::optional<HostMountDevice> assignedDevice = std::nullopt,
              std::string volumeLabel = {});
    void SetPlaceholder(const std::filesystem::path& originalPath,
                        std::optional<HostMountDevice> assignedDevice, std::string volumeLabel,
                        bool readOnly);
    bool Locate(const std::filesystem::path& path, std::string& outError);

    const std::filesystem::path& GetCanonicalRoot() const { return m_canonicalRoot; }
    const std::filesystem::path& GetOriginalRoot() const { return m_originalRoot; }
    std::optional<HostMountDevice> GetAssignedDevice() const { return m_assignedDevice; }
    const std::string& GetVolumeLabel() const { return m_volumeLabel; }
    bool IsReadOnly() const { return m_readOnly; }
    bool IsAvailable() const { return m_available; }
    DocumentCapabilities GetCapabilities() const;

    uint64_t RequestRefresh();
    bool ApplyPendingRefresh();
    bool RefreshNow(std::string& outError);
    const HostDirectoryScan& GetScan() const { return m_scan; }

    std::optional<std::filesystem::path> Resolve(const DocumentEntryId& id,
                                                 std::string& outError) const;
    bool ReadFile(const DocumentEntryId& id, std::vector<uint8_t>& out,
                  std::string& outError) const;
    bool ReadFilePrefix(const DocumentEntryId& id, size_t maximumBytes, std::vector<uint8_t>& out,
                        std::string& outError) const;
    bool WriteFile(const DocumentEntryId& id, const std::vector<uint8_t>& data,
                   std::string& outError);
    bool CreateHostFile(const DocumentEntryId& parent, const std::string& name,
                        const std::vector<uint8_t>& data, std::string& outError);
    bool Import(const DocumentEntryId& parent, const std::filesystem::path& source,
                std::string& outError);
    bool CreateDirectory(const DocumentEntryId& parent, const std::string& name,
                         std::string& outError);
    bool Rename(const DocumentEntryId& id, const std::string& newName, std::string& outError);
    bool Move(const DocumentEntryId& id, const DocumentEntryId& newParent, std::string& outError);
    bool Remove(const DocumentEntryId& id, std::string& outError);

    static bool IsSafeRelativePath(const std::filesystem::path& path);
    static bool IsMetadataSidecar(const std::filesystem::path& path);

private:
    static HostDirectoryScan ScanDirectory(const std::filesystem::path& root, uint64_t generation);
    bool RequireWritable(std::string& outError) const;
    std::optional<std::filesystem::path> ResolveForCreate(const DocumentEntryId& parent,
                                                          const std::string& name,
                                                          std::string& outError) const;
    bool IsWithinRoot(const std::filesystem::path& path) const;

    std::filesystem::path m_originalRoot;
    std::filesystem::path m_canonicalRoot;
    std::optional<HostMountDevice> m_assignedDevice;
    std::string m_volumeLabel;
    bool m_readOnly = true;
    bool m_available = false;
    std::atomic<uint64_t> m_requestedGeneration{0};
    std::future<HostDirectoryScan> m_pendingScan;
    HostDirectoryScan m_scan;
};

} // namespace adm::core
