#include "ElfSymbolResolver.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace adm::validation {
namespace {

struct Reader
{
    std::span<const uint8_t> data;
    bool big = false;

    uint16_t U16(size_t offset) const
    {
        if (offset + 2 > data.size())
            return 0;
        return big ? static_cast<uint16_t>((data[offset] << 8) | data[offset + 1])
                   : static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
    }
    uint32_t U32(size_t offset) const
    {
        if (offset + 4 > data.size())
            return 0;
        if (big)
            return (static_cast<uint32_t>(data[offset]) << 24) |
                   (static_cast<uint32_t>(data[offset + 1]) << 16) |
                   (static_cast<uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
        return data[offset] | (static_cast<uint32_t>(data[offset + 1]) << 8) |
               (static_cast<uint32_t>(data[offset + 2]) << 16) |
               (static_cast<uint32_t>(data[offset + 3]) << 24);
    }
    bool Range(size_t offset, size_t size) const
    {
        return offset <= data.size() && size <= data.size() - offset;
    }
};

struct Section
{
    uint32_t type = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t link = 0;
    uint32_t entrySize = 0;
};

} // namespace

bool ElfSymbolResolver::Load(const std::filesystem::path& path, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open ELF: " + path.string();
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    return Parse(bytes, error);
}

bool ElfSymbolResolver::Parse(std::span<const uint8_t> bytes, std::string& error)
{
    m_symbols.clear();
    if (bytes.size() < 52 || bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' ||
        bytes[3] != 'F' || bytes[4] != 1 || (bytes[5] != 1 && bytes[5] != 2)) {
        error = "expected an ELF32 image";
        return false;
    }
    Reader reader{bytes, bytes[5] == 2};
    const uint32_t sectionOffset = reader.U32(32);
    const uint16_t sectionEntrySize = reader.U16(46);
    const uint16_t sectionCount = reader.U16(48);
    if (sectionEntrySize < 40 ||
        !reader.Range(sectionOffset, static_cast<size_t>(sectionEntrySize) * sectionCount)) {
        error = "ELF section table is truncated";
        return false;
    }
    std::vector<Section> sections;
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const size_t base = sectionOffset + static_cast<size_t>(i) * sectionEntrySize;
        sections.push_back({reader.U32(base + 4), reader.U32(base + 16), reader.U32(base + 20),
                            reader.U32(base + 24), reader.U32(base + 36)});
    }
    for (size_t sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex) {
        const auto& table = sections[sectionIndex];
        if (table.type != 2 || table.entrySize < 16 || table.link >= sections.size() ||
            !reader.Range(table.offset, table.size))
            continue;
        const auto& strings = sections[table.link];
        if (!reader.Range(strings.offset, strings.size))
            continue;
        for (uint32_t offset = 0; offset + 16 <= table.size; offset += table.entrySize) {
            const size_t base = table.offset + offset;
            const uint32_t nameOffset = reader.U32(base);
            const uint32_t address = reader.U32(base + 4);
            const uint32_t size = reader.U32(base + 8);
            const uint16_t symbolSection = reader.U16(base + 14);
            if (nameOffset >= strings.size || symbolSection == 0)
                continue;
            const char* begin =
                reinterpret_cast<const char*>(bytes.data() + strings.offset + nameOffset);
            const char* limit =
                reinterpret_cast<const char*>(bytes.data() + strings.offset + strings.size);
            const char* end = std::find(begin, limit, '\0');
            if (begin == end || end == limit)
                continue;
            m_symbols.push_back({std::string(begin, end), address, size, symbolSection});
        }
    }
    std::sort(m_symbols.begin(), m_symbols.end(), [](const auto& a, const auto& b) {
        if (a.address != b.address)
            return a.address < b.address;
        return a.name < b.name;
    });
    if (m_symbols.empty()) {
        error = "ELF contains no usable symbol table";
        return false;
    }
    return true;
}

const ElfSymbol* ElfSymbolResolver::Find(const std::string& name) const
{
    auto found = std::find_if(m_symbols.begin(), m_symbols.end(),
                              [&](const auto& symbol) { return symbol.name == name; });
    return found == m_symbols.end() ? nullptr : &*found;
}

std::optional<uint32_t>
ElfSymbolResolver::ResolveRuntime(const std::string& name,
                                  std::span<const RuntimeSegment> segments) const
{
    const auto* symbol = Find(name);
    if (!symbol)
        return std::nullopt;
    for (const auto& segment : segments) {
        if (segment.elfSection == symbol->section && symbol->address >= segment.linkAddress &&
            symbol->address - segment.linkAddress < segment.size)
            return segment.runtimeAddress + symbol->address - segment.linkAddress;
    }
    return std::nullopt;
}

std::string ElfSymbolResolver::NormalizeAddress(uint32_t runtimeAddress,
                                                std::span<const RuntimeSegment> segments) const
{
    const ElfSymbol* best = nullptr;
    uint32_t bestRuntime = 0;
    for (const auto& symbol : m_symbols) {
        for (const auto& segment : segments) {
            if (segment.elfSection != symbol.section || symbol.address < segment.linkAddress ||
                symbol.address - segment.linkAddress >= segment.size)
                continue;
            const uint32_t candidate =
                segment.runtimeAddress + symbol.address - segment.linkAddress;
            if (candidate <= runtimeAddress && (!best || candidate > bestRuntime)) {
                best = &symbol;
                bestRuntime = candidate;
            }
        }
    }
    if (!best) {
        std::ostringstream stream;
        stream << "$" << std::hex << runtimeAddress;
        return stream.str();
    }
    return best->name + "+" + std::to_string(runtimeAddress - bestRuntime);
}

} // namespace adm::validation
