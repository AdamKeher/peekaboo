#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adm::validation {

struct ElfSymbol
{
    std::string name;
    uint32_t address = 0;
    uint32_t size = 0;
    uint16_t section = 0;
};

struct RuntimeSegment
{
    uint16_t elfSection = 0;
    uint32_t linkAddress = 0;
    uint32_t runtimeAddress = 0;
    uint32_t size = 0;
};

class ElfSymbolResolver
{
public:
    bool Load(const std::filesystem::path& path, std::string& error);
    bool Parse(std::span<const uint8_t> bytes, std::string& error);
    [[nodiscard]] const ElfSymbol* Find(const std::string& name) const;
    [[nodiscard]] std::optional<uint32_t>
    ResolveRuntime(const std::string& name, std::span<const RuntimeSegment> segments) const;
    [[nodiscard]] std::string NormalizeAddress(uint32_t runtimeAddress,
                                               std::span<const RuntimeSegment> segments) const;
    [[nodiscard]] const std::vector<ElfSymbol>& Symbols() const { return m_symbols; }

private:
    std::vector<ElfSymbol> m_symbols;
};

} // namespace adm::validation
