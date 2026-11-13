#include "DiskSearcher.h"
#include <fmt/core.h>
#include <algorithm>
#include <cctype>

namespace adm::core {

DiskSearcher::DiskSearcher() {}

void DiskSearcher::SetDiskInfo(uint32_t totalBlocks, ReadBlockCallback readCallback)
{
    m_totalBlocks = totalBlocks;
    m_readCallback = readCallback;
}

std::vector<std::vector<uint8_t>> DiskSearcher::SnapshotBlocks() const
{
    std::vector<std::vector<uint8_t>> blocks;
    if (!m_readCallback)
        return blocks;
    blocks.reserve(m_totalBlocks);
    for (uint32_t b = 0; b < m_totalBlocks; ++b)
        blocks.push_back(m_readCallback(b));
    return blocks;
}

void DiskSearcher::SetFilesystemInfo(std::vector<FilesystemFileInfo> files,
                                     ReadFileContentCallback readFileCallback)
{
    m_filesystemFiles = std::move(files);
    m_readFileCallback = std::move(readFileCallback);
}

std::vector<uint8_t> DiskSearcher::ParseHex(const std::string& hexStr)
{
    std::vector<uint8_t> result;
    std::string hex = hexStr;
    // Remove spaces
    hex.erase(std::remove_if(hex.begin(), hex.end(), ::isspace), hex.end());

    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        // strtol does not throw; invalid digits parse as 0.
        uint8_t byte = (uint8_t)std::strtol(byteString.c_str(), nullptr, 16);
        result.push_back(byte);
    }
    return result;
}

std::vector<uint8_t> DiskSearcher::ReadBytes(uint64_t globalOffset, size_t length) const
{
    std::vector<uint8_t> buffer;
    buffer.reserve(length);

    uint32_t blk = (uint32_t)(globalOffset / 512);
    size_t off = (size_t)(globalOffset % 512);

    while (buffer.size() < length && blk < m_totalBlocks) {
        std::vector<uint8_t> data = m_readCallback(blk);
        if (data.empty() || off >= data.size())
            break;

        size_t copyLen = std::min(length - buffer.size(), data.size() - off);
        buffer.insert(buffer.end(), data.begin() + off, data.begin() + off + copyLen);

        off = 0;
        blk++;
    }
    return buffer;
}

bool DiskSearcher::MatchAtGlobal(uint64_t globalOffset, const std::vector<uint8_t>& pattern,
                                 bool caseSensitive, std::vector<uint8_t>& cache,
                                 uint32_t& cacheIdx, bool& blockEmpty) const
{
    uint32_t block = (uint32_t)(globalOffset / 512);
    size_t local = (size_t)(globalOffset % 512);

    if (block != cacheIdx) {
        cache = m_readCallback(block);
        cacheIdx = block;
    }
    if (cache.empty()) {
        blockEmpty = true;
        return false;
    }
    blockEmpty = false;

    if (local + pattern.size() > 512) {
        // Pattern straddles the 512-byte block boundary; assemble the contiguous run.
        std::vector<uint8_t> seq = ReadBytes(globalOffset, pattern.size());
        return seq.size() == pattern.size() && MatchesAt(seq, 0, pattern, caseSensitive);
    }
    return MatchesAt(cache, local, pattern, caseSensitive);
}

bool DiskSearcher::MatchesAt(const std::vector<uint8_t>& data, size_t offset,
                             const std::vector<uint8_t>& pattern, bool caseSensitive) const
{
    if (offset + pattern.size() > data.size())
        return false;

    for (size_t i = 0; i < pattern.size(); ++i) {
        uint8_t d = data[offset + i];
        uint8_t p = pattern[i];

        if (!caseSensitive) {
            d = (uint8_t)std::toupper(d);
            p = (uint8_t)std::toupper(p);
        }

        if (d != p)
            return false;
    }
    return true;
}

bool DiskSearcher::FindNext(const std::vector<uint8_t>& pattern, bool caseSensitive, bool reverse,
                            uint32_t startBlock, size_t startOffset, uint32_t& outputBlock,
                            size_t& outputOffset)
{
    if (pattern.empty() || !m_readCallback || m_totalBlocks == 0)
        return false;

    uint64_t totalBytes = (uint64_t)m_totalBlocks * 512;
    uint64_t startGlobal = (uint64_t)startBlock * 512;
    if (startOffset == (size_t)-1) {
        startGlobal += 511; // End of start block
    } else {
        startGlobal += startOffset;
    }

    // Bounds check
    if (startGlobal >= totalBytes)
        startGlobal = reverse ? totalBytes - 1 : 0;

    std::vector<uint8_t> cache;
    uint32_t cacheIdx = (uint32_t)-1;

    uint64_t globalOffset = startGlobal;
    uint64_t bytesChecked = 0;

    while (bytesChecked < totalBytes) {
        size_t localOffset = (size_t)(globalOffset % 512);

        bool blockEmpty = false;
        if (MatchAtGlobal(globalOffset, pattern, caseSensitive, cache, cacheIdx, blockEmpty)) {
            outputBlock = (uint32_t)(globalOffset / 512);
            outputOffset = localOffset;
            return true;
        }

        if (blockEmpty) {
            // Block missing/error. Skip the rest of this block (in the scan direction).
            uint64_t skip = reverse ? (localOffset + 1) : (512 - localOffset);
            globalOffset = reverse ? (globalOffset + totalBytes - skip) % totalBytes
                                   : (globalOffset + skip) % totalBytes;
            bytesChecked += skip;
            continue;
        }

        bytesChecked++;

        // Advance one byte in the scan direction.
        globalOffset = reverse ? (globalOffset + totalBytes - 1) % totalBytes
                               : (globalOffset + 1) % totalBytes;
    }

    return false;
}

std::vector<SearchResult> DiskSearcher::FindAll(const std::vector<uint8_t>& pattern,
                                                bool caseSensitive, SearchType type,
                                                std::function<void(float)> progressCallback)
{
    std::vector<SearchResult> results;
    if ((type != SearchType::MOD && pattern.empty()) || !m_readCallback || m_totalBlocks == 0)
        return results;

    if (type == SearchType::MOD) {
        const std::vector<std::string> signatures = {"M.K.", "M!K!", "FLT4", "FLT8",
                                                     "2CHN", "4CHN", "5CHN", "6CHN",
                                                     "7CHN", "8CHN", "OCTA", "CD81"};

        if (m_readFileCallback) {
            // Filesystem-based search: read each file properly via the filesystem,
            // skipping OFS/FFS block overhead. Files may not be in contiguous blocks.
            for (size_t fi = 0; fi < m_filesystemFiles.size(); ++fi) {
                if (progressCallback) {
                    progressCallback((float)fi / (float)m_filesystemFiles.size());
                }

                const auto& file = m_filesystemFiles[fi];
                if (file.size < 1084)
                    continue;

                std::vector<uint8_t> content = m_readFileCallback(file.headerBlock);
                if (content.size() < 1084)
                    continue;

                // Check for a MOD signature at offset 1080 in the clean file data
                bool hasSig = false;
                for (const auto& sig : signatures) {
                    if (content.size() >= 1080 + sig.size()) {
                        if (std::equal(sig.begin(), sig.end(), content.begin() + 1080)) {
                            hasSig = true;
                            break;
                        }
                    }
                }
                if (!hasSig)
                    continue;

                // Basic MOD header validation
                bool valid = true;
                for (int s = 0; s < 31; ++s) {
                    int off = 20 + s * 30;
                    if (content[off + 25] > 64) {
                        valid = false;
                        break;
                    } // Volume > 64
                }
                if (!valid)
                    continue;
                if (content[950] > 128)
                    continue; // Song length > 128

                SearchResult res;
                res.block = file.headerBlock; // Navigate to file header in hex editor
                res.offset = 0;
                res.length = content.size();
                res.context = file.name;
                res.fromFilesystem = true;
                res.filesystemHeaderBlock = file.headerBlock;
                results.push_back(res);
            }

            if (progressCallback)
                progressCallback(1.0f);
            return results;
        }

        // NDOS / raw-block search: read consecutive 512-byte blocks directly
        for (uint32_t i = 0; i < m_totalBlocks; ++i) {
            if (progressCallback && (i % 100 == 0)) {
                progressCallback((float)i / (float)m_totalBlocks);
            }

            std::vector<uint8_t> blockData = m_readCallback(i);
            if (blockData.empty())
                continue;

            // Scan for signatures
            for (const auto& sig : signatures) {
                // Simple string search in block
                auto it = std::search(blockData.begin(), blockData.end(), sig.begin(), sig.end());
                while (it != blockData.end()) {
                    size_t sigOffsetLocal = std::distance(blockData.begin(), it);
                    uint64_t sigOffsetGlobal = (uint64_t)i * 512 + sigOffsetLocal;

                    if (sigOffsetGlobal >= 1080) {
                        uint64_t modStartGlobal = sigOffsetGlobal - 1080;

                        // Read header (1084 bytes)
                        std::vector<uint8_t> header = ReadBytes(modStartGlobal, 1084);

                        if (header.size() == 1084) {
                            // Validate
                            bool valid = true;
                            for (int s = 0; s < 31; ++s) {
                                int off = 20 + s * 30;
                                if (header[off + 25] > 64) {
                                    valid = false;
                                    break;
                                } // Volume > 64
                            }
                            if (header[950] > 128)
                                valid = false; // Song length > 128

                            // Calculate Num Patterns based on Song Length
                            // Scanning the full 128-byte table can pick up garbage in unused slots.
                            uint8_t songLength = header[950];
                            uint32_t numPatterns = 0;
                            for (int k = 0; k < songLength && k < 128; ++k) {
                                uint8_t pat = header[952 + k];
                                if (pat > numPatterns)
                                    numPatterns = pat;
                            }
                            numPatterns++; // Max index + 1
                            if (numPatterns > 64) {
                                // Protracker standard is 64. Some extended formats go to 100 or
                                // 128. If > 128, it's definitely invalid (table limit). Let's stick
                                // to 128 as hard limit.
                                if (numPatterns > 128)
                                    valid = false;
                            }

                            // Calculate sample size
                            uint32_t sampleBytes = 0;
                            for (int s = 0; s < 31; ++s) {
                                int off = 20 + s * 30;
                                // Length is big endian word at offset 22 within sample struct
                                uint16_t len = (header[off + 22] << 8) | header[off + 23];
                                sampleBytes += len * 2;
                            }

                            uint64_t totalSize = 1084 + (numPatterns * 1024) + sampleBytes;
                            uint64_t diskLimit = (uint64_t)m_totalBlocks * 512;

                            // Relaxed checks
                            if (totalSize > 2 * 1024 * 1024)
                                valid = false; // 2MB limit

                            // If valid but exceeds disk, clamp size (truncated file)
                            if (valid && modStartGlobal + totalSize > diskLimit) {
                                totalSize = diskLimit - modStartGlobal;
                            }

                            if (valid) {
                                SearchResult res;
                                res.block = (uint32_t)(modStartGlobal / 512);
                                res.offset = (size_t)(modStartGlobal % 512);
                                res.length = totalSize;

                                // Get Mod Name (first 20 bytes)
                                std::string name;
                                for (int k = 0; k < 20; ++k) {
                                    if (header[k] && std::isprint(header[k]))
                                        name += (char)header[k];
                                }
                                if (name.empty())
                                    name = "Unknown";

                                res.context = name;
                                results.push_back(res);
                            }
                        }
                    }

                    it = std::search(it + 1, blockData.end(), sig.begin(), sig.end());
                }
            }
        }

        if (progressCallback)
            progressCallback(1.0f);
        return results;
    }

    uint64_t totalBytes = (uint64_t)m_totalBlocks * 512;
    std::vector<uint8_t> cache;
    uint32_t cacheIdx = (uint32_t)-1;

    for (uint64_t globalOffset = 0; globalOffset < totalBytes;) {
        uint32_t currentBlock = (uint32_t)(globalOffset / 512);

        if (progressCallback && (globalOffset % (100 * 512) == 0)) {
            progressCallback((float)currentBlock / (float)m_totalBlocks);
        }

        size_t localOffset = (size_t)(globalOffset % 512);
        bool blockEmpty = false;
        bool matched =
            MatchAtGlobal(globalOffset, pattern, caseSensitive, cache, cacheIdx, blockEmpty);

        if (blockEmpty) {
            globalOffset += (512 - (globalOffset % 512));
            continue;
        }

        if (matched) {
            SearchResult result;
            result.block = currentBlock;
            result.offset = localOffset;
            result.length = pattern.size();

            if (type == SearchType::ASCII) {
                uint64_t start = (globalOffset > 10) ? globalOffset - 10 : 0;
                std::vector<uint8_t> ctxData = ReadBytes(start, pattern.size() + 20);
                std::string ctx;
                for (uint8_t b : ctxData) {
                    ctx += std::isprint((unsigned char)b) ? (char)b : '.';
                }
                result.context = ctx;
            } else {
                std::string ctx;
                for (size_t k = 0; k < pattern.size() && k < 8; ++k) {
                    ctx += fmt::format("{:02X} ", pattern[k]);
                }
                if (pattern.size() > 8)
                    ctx += "...";
                result.context = ctx;
            }

            results.push_back(result);
            globalOffset += pattern.size(); // Advance past match
        } else {
            globalOffset++;
        }
    }

    if (progressCallback)
        progressCallback(1.0f);
    return results;
}

} // namespace adm::core
