#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "DiskSearcher.h"

namespace adm::core {

struct MemorySearchResult
{
    uint32_t address;       // Absolute Amiga address of the match
    size_t length;          // Length of match in bytes
    std::string regionName; // Name of the memory region (e.g., "Chip RAM")
    std::string context;    // ASCII or hex preview
};

class MemorySearcher
{
public:
    struct Region
    {
        const uint8_t* data;
        uint32_t baseAddress; // Amiga base address
        uint32_t size;
        std::string name;
    };

    void SetRegions(std::vector<Region> regions);

    bool HasRegions() const { return !m_regions.empty(); }
    const std::vector<Region>& Regions() const { return m_regions; }

    // Full scan – returns all matches across all regions
    std::vector<MemorySearchResult> FindAll(const std::vector<uint8_t>& pattern, bool caseSensitive,
                                            DiskSearcher::SearchType type,
                                            std::function<void(float)> progressCallback = nullptr);

    // Incremental search (for interactive Find Next / Find Previous).
    // startAddr is the address to search FROM (exclusive start; pass result+1 for next forward).
    // Returns true and sets outAddr/outLength on success.
    bool FindNext(const std::vector<uint8_t>& pattern, bool caseSensitive, bool reverse,
                  uint32_t startAddr, uint32_t& outAddr, size_t& outLength);

    // Extract raw bytes from Amiga address space (across regions if needed).
    // Returns empty vector if address is not in any known region.
    std::vector<uint8_t> ReadBytes(uint32_t address, size_t length) const;

private:
    std::vector<Region> m_regions; // sorted by baseAddress

    bool MatchesAt(const uint8_t* data, size_t dataSize, size_t offset,
                   const std::vector<uint8_t>& pattern, bool caseSensitive) const;
};

} // namespace adm::core
