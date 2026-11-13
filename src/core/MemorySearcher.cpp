#include "MemorySearcher.h"
#include <fmt/core.h>
#include <algorithm>
#include <cctype>

namespace adm::core {

void MemorySearcher::SetRegions(std::vector<Region> regions)
{
    std::sort(regions.begin(), regions.end(),
              [](const Region& a, const Region& b) { return a.baseAddress < b.baseAddress; });
    m_regions = std::move(regions);
}

bool MemorySearcher::MatchesAt(const uint8_t* data, size_t dataSize, size_t offset,
                               const std::vector<uint8_t>& pattern, bool caseSensitive) const
{
    if (offset + pattern.size() > dataSize)
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

std::vector<MemorySearchResult> MemorySearcher::FindAll(const std::vector<uint8_t>& pattern,
                                                        bool caseSensitive,
                                                        DiskSearcher::SearchType type,
                                                        std::function<void(float)> progressCallback)
{
    std::vector<MemorySearchResult> results;
    if (m_regions.empty())
        return results;

    // Compute total bytes for progress reporting
    uint64_t totalBytes = 0;
    for (const auto& r : m_regions)
        totalBytes += r.size;
    uint64_t processed = 0;

    if (type == DiskSearcher::SearchType::MOD) {
        // Search for MOD tracker signatures; a match at offset X means the MOD header
        // starts at X - 1080 (Protracker layout).
        const std::vector<std::string> signatures = {"M.K.", "M!K!", "FLT4", "FLT8",
                                                     "2CHN", "4CHN", "5CHN", "6CHN",
                                                     "7CHN", "8CHN", "OCTA", "CD81"};

        for (const auto& region : m_regions) {
            if (!region.data || region.size == 0) {
                processed += region.size;
                continue;
            }
            const uint8_t* data = region.data;
            uint32_t size = region.size;

            for (uint32_t offset = 0; offset < size; ++offset) {
                if (progressCallback && (offset % 65536 == 0))
                    progressCallback((float)(processed + offset) / (float)totalBytes);

                for (const auto& sig : signatures) {
                    if (offset + sig.size() <= size &&
                        std::equal(sig.begin(), sig.end(), data + offset)) {
                        if (offset >= 1080) {
                            uint32_t modStart = offset - 1080;
                            if (modStart + 1084 <= size) {
                                const uint8_t* hdr = data + modStart;
                                bool valid = true;
                                for (int s = 0; s < 31 && valid; ++s) {
                                    int off = 20 + s * 30;
                                    if (hdr[off + 25] > 64)
                                        valid = false;
                                }
                                if (valid && hdr[950] <= 128) {
                                    uint8_t songLen = hdr[950];
                                    uint32_t numPat = 0;
                                    for (int k = 0; k < songLen && k < 128; ++k) {
                                        if (hdr[952 + k] > numPat)
                                            numPat = hdr[952 + k];
                                    }
                                    numPat++;
                                    if (numPat <= 128) {
                                        uint32_t sampleBytes = 0;
                                        for (int s = 0; s < 31; ++s) {
                                            int off = 20 + s * 30;
                                            uint16_t len =
                                                (uint16_t)((hdr[off + 22] << 8) | hdr[off + 23]);
                                            sampleBytes += len * 2u;
                                        }
                                        uint64_t modSize =
                                            1084 + (uint64_t)numPat * 1024 + sampleBytes;
                                        if (modSize <= 2u * 1024u * 1024u) {
                                            MemorySearchResult res;
                                            res.address = region.baseAddress + modStart;
                                            res.length = (size_t)std::min(
                                                modSize, (uint64_t)(size - modStart));
                                            res.regionName = region.name;
                                            std::string name;
                                            for (int k = 0; k < 20; ++k) {
                                                if (hdr[k] && std::isprint(hdr[k]))
                                                    name += (char)hdr[k];
                                            }
                                            res.context = name.empty() ? "Unknown" : name;
                                            results.push_back(res);
                                        }
                                    }
                                }
                            }
                        }
                        break; // Only test one signature per offset
                    }
                }
            }
            processed += size;
        }
        if (progressCallback)
            progressCallback(1.0f);
        return results;
    }

    // ASCII or Hex search
    if (pattern.empty())
        return results;

    for (const auto& region : m_regions) {
        if (!region.data || region.size == 0) {
            processed += region.size;
            continue;
        }
        const uint8_t* data = region.data;
        uint32_t size = region.size;

        for (size_t offset = 0; offset < size; ++offset) {
            if (progressCallback && (offset % 65536 == 0))
                progressCallback((float)(processed + offset) / (float)totalBytes);

            if (MatchesAt(data, size, offset, pattern, caseSensitive)) {
                MemorySearchResult res;
                res.address = region.baseAddress + (uint32_t)offset;
                res.length = pattern.size();
                res.regionName = region.name;

                if (type == DiskSearcher::SearchType::ASCII) {
                    size_t start = (offset > 10) ? offset - 10 : 0;
                    size_t end = std::min(offset + pattern.size() + 10, (size_t)size);
                    for (size_t k = start; k < end; ++k) {
                        char c = (char)data[k];
                        res.context += std::isprint((unsigned char)c) ? c : '.';
                    }
                } else {
                    for (size_t k = 0; k < pattern.size() && k < 8; ++k)
                        res.context += fmt::format("{:02X} ", pattern[k]);
                    if (pattern.size() > 8)
                        res.context += "...";
                }

                results.push_back(res);
                offset += pattern.size() - 1; // Non-overlapping
            }
        }
        processed += size;
    }

    if (progressCallback)
        progressCallback(1.0f);
    return results;
}

bool MemorySearcher::FindNext(const std::vector<uint8_t>& pattern, bool caseSensitive, bool reverse,
                              uint32_t startAddr, uint32_t& outAddr, size_t& outLength)
{
    if (pattern.empty() || m_regions.empty())
        return false;

    if (!reverse) {
        // Forward: two passes to allow wrapping
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& region : m_regions) {
                if (!region.data || region.size == 0)
                    continue;
                uint32_t regionEnd = region.baseAddress + region.size;
                if (pass == 0 && regionEnd <= startAddr)
                    continue; // Entirely before start address

                size_t from = 0;
                if (pass == 0 && region.baseAddress < startAddr)
                    from = (size_t)(startAddr - region.baseAddress);

                for (size_t off = from; off < region.size; ++off) {
                    if (pass == 0 && region.baseAddress + (uint32_t)off < startAddr)
                        continue;
                    if (MatchesAt(region.data, region.size, off, pattern, caseSensitive)) {
                        outAddr = region.baseAddress + (uint32_t)off;
                        outLength = pattern.size();
                        return true;
                    }
                }
            }
        }
    } else {
        // Backward: two passes to allow wrapping
        for (int pass = 0; pass < 2; ++pass) {
            for (int ri = (int)m_regions.size() - 1; ri >= 0; --ri) {
                const auto& region = m_regions[ri];
                if (!region.data || region.size == 0)
                    continue;
                if (pass == 0 && region.baseAddress >= startAddr)
                    continue; // Entirely at or after start address

                size_t upTo = region.size; // exclusive upper bound
                if (pass == 0) {
                    uint32_t regionEnd = region.baseAddress + region.size;
                    if (regionEnd > startAddr)
                        upTo = (size_t)(startAddr - region.baseAddress);
                }
                if (upTo == 0)
                    continue;

                for (int off = (int)(upTo - 1); off >= 0; --off) {
                    if (MatchesAt(region.data, region.size, (size_t)off, pattern, caseSensitive)) {
                        outAddr = region.baseAddress + (uint32_t)off;
                        outLength = pattern.size();
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

std::vector<uint8_t> MemorySearcher::ReadBytes(uint32_t address, size_t length) const
{
    std::vector<uint8_t> result;
    result.reserve(length);

    for (const auto& region : m_regions) {
        if (result.size() >= length)
            break;
        uint32_t regionEnd = region.baseAddress + region.size;
        if (address + (uint32_t)length <= region.baseAddress || address >= regionEnd)
            continue;

        uint32_t readStart = std::max(address, region.baseAddress);
        uint32_t readEnd = std::min(address + (uint32_t)length, regionEnd);
        size_t srcOffset = readStart - region.baseAddress;

        result.insert(result.end(), region.data + srcOffset,
                      region.data + srcOffset + (readEnd - readStart));
    }

    return result;
}

} // namespace adm::core
