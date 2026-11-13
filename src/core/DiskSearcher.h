#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace adm::core {

struct SearchResult
{
    uint32_t block;
    size_t offset;
    size_t length;
    std::string context; // Hex or ASCII preview
    // Set for MOD files found via the ADF filesystem (not raw block scan)
    bool fromFilesystem = false;
    uint32_t filesystemHeaderBlock = 0;
};

class DiskSearcher
{
public:
    enum class SearchType { ASCII, Hex, MOD };

    using ReadBlockCallback = std::function<std::vector<uint8_t>(uint32_t)>;

    // Info about a file in the ADF filesystem, used for filesystem-based MOD searching
    struct FilesystemFileInfo
    {
        std::string name;
        uint32_t headerBlock;
        int32_t size;
    };
    using ReadFileContentCallback = std::function<std::vector<uint8_t>(uint32_t headerBlock)>;

    DiskSearcher();

    void SetDiskInfo(uint32_t totalBlocks, ReadBlockCallback readCallback);

    [[nodiscard]] uint32_t TotalBlocks() const { return m_totalBlocks; }

    // Copies every block through the read callback — on the CALLER's thread, because the
    // callbacks (adflib) are not safe to run concurrently with the UI. The snapshot lets a
    // worker thread scan via a local DiskSearcher without touching the live volume.
    [[nodiscard]] std::vector<std::vector<uint8_t>> SnapshotBlocks() const;
    // Provide filesystem file list + reader for MOD search on filesystem disks.
    // Pass empty files + nullptr callback to clear (NDOS disks).
    void SetFilesystemInfo(std::vector<FilesystemFileInfo> files,
                           ReadFileContentCallback readFileCallback);

    // Quick search (for Hex Editor "Find Next/Prev")
    // Returns index in block, or -1 if not found.
    // If found, outputBlock is updated.
    bool FindNext(const std::vector<uint8_t>& pattern, bool caseSensitive, bool reverse,
                  uint32_t startBlock, size_t startOffset, uint32_t& outputBlock,
                  size_t& outputOffset);

    // Full scan (for Advanced Find)
    std::vector<SearchResult> FindAll(const std::vector<uint8_t>& pattern, bool caseSensitive,
                                      SearchType type,
                                      std::function<void(float)> progressCallback = nullptr);

    // Helper to parse hex string
    static std::vector<uint8_t> ParseHex(const std::string& hexStr);

private:
    uint32_t m_totalBlocks = 0;
    ReadBlockCallback m_readCallback;

    std::vector<FilesystemFileInfo> m_filesystemFiles;
    ReadFileContentCallback m_readFileCallback;

    bool MatchesAt(const std::vector<uint8_t>& data, size_t offset,
                   const std::vector<uint8_t>& pattern, bool caseSensitive) const;

    // Read `length` bytes starting at a global byte offset, spanning blocks as needed.
    std::vector<uint8_t> ReadBytes(uint64_t globalOffset, size_t length) const;

    // Test whether `pattern` matches at `globalOffset`. Uses cache/cacheIdx as a
    // one-block read cache (updated in place); sets blockEmpty when the containing
    // block could not be read so callers can skip it.
    bool MatchAtGlobal(uint64_t globalOffset, const std::vector<uint8_t>& pattern,
                       bool caseSensitive, std::vector<uint8_t>& cache, uint32_t& cacheIdx,
                       bool& blockEmpty) const;
};

} // namespace adm::core
