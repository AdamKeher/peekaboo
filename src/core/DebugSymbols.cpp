#include "DebugSymbols.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace adm::core {

namespace {

// --- Minimal ELF32 constants (see the System V ABI / elf.h) ---
constexpr uint8_t kElfMag[4] = {0x7f, 'E', 'L', 'F'};
constexpr int kEiClass = 4; // 1 = ELFCLASS32
constexpr int kEiData = 5;  // 1 = ELFDATA2LSB, 2 = ELFDATA2MSB
constexpr uint8_t kElfClass32 = 1;
constexpr uint8_t kElfData2Msb = 2;

constexpr uint32_t kShtSymtab = 2;
constexpr uint32_t kShtStrtab = 3;
constexpr int kSttFunc = 2; // st_info & 0xf

// Elf32_Shdr layout (40 bytes)
constexpr size_t kShdrSize = 40;
constexpr size_t kShType = 4;
constexpr size_t kShOffset = 16;
constexpr size_t kShSize = 20;
constexpr size_t kShLink = 24;
constexpr size_t kShEntsize = 36;

// Elf32_Sym layout (16 bytes)
constexpr size_t kSymSize = 16;
constexpr size_t kStName = 0;
constexpr size_t kStValue = 4;
constexpr size_t kStSize = 8;
constexpr size_t kStInfo = 12;

// Endian-aware fixed-width reads over a byte span. All ELF32 multi-byte fields
// are read according to the file's declared endianness.
class ElfReader
{
public:
    ElfReader(std::span<const uint8_t> data, bool bigEndian) : m_data(data), m_be(bigEndian) {}

    uint16_t U16(size_t off) const
    {
        return m_be ? static_cast<uint16_t>((m_data[off] << 8) | m_data[off + 1])
                    : static_cast<uint16_t>((m_data[off + 1] << 8) | m_data[off]);
    }

    uint32_t U32(size_t off) const
    {
        if (m_be) {
            return (static_cast<uint32_t>(m_data[off]) << 24) |
                   (static_cast<uint32_t>(m_data[off + 1]) << 16) |
                   (static_cast<uint32_t>(m_data[off + 2]) << 8) |
                   static_cast<uint32_t>(m_data[off + 3]);
        }
        return (static_cast<uint32_t>(m_data[off + 3]) << 24) |
               (static_cast<uint32_t>(m_data[off + 2]) << 16) |
               (static_cast<uint32_t>(m_data[off + 1]) << 8) | static_cast<uint32_t>(m_data[off]);
    }

private:
    std::span<const uint8_t> m_data;
    bool m_be;
};

// Read a NUL-terminated string from a string table, bounds-checked.
std::string ReadStr(std::span<const uint8_t> data, size_t tableOff, size_t tableSize,
                    uint32_t nameOff)
{
    if (nameOff >= tableSize) {
        return {};
    }
    size_t start = tableOff + nameOff;
    std::string s;
    for (size_t i = start; i < tableOff + tableSize && i < data.size() && data[i] != 0; ++i) {
        s += static_cast<char>(data[i]);
    }
    return s;
}

} // namespace

bool DebugSymbols::LoadFromElf(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        Clear();
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (!LoadFromElf(bytes)) {
        return false;
    }
    m_sourcePath = path;
    return true;
}

bool DebugSymbols::LoadFromElf(std::span<const uint8_t> data)
{
    Clear();

    // ELF identification.
    if (data.size() < 52) { // minimum ELF32 header size
        return false;
    }
    if (std::memcmp(data.data(), kElfMag, sizeof(kElfMag)) != 0) {
        return false;
    }
    if (data[kEiClass] != kElfClass32) {
        return false; // 64-bit ELFs are not supported (Amiga m68k is 32-bit)
    }
    const bool bigEndian = data[kEiData] == kElfData2Msb;
    ElfReader r(data, bigEndian);

    // ELF32 header fields we need.
    const uint32_t shoff = r.U32(0x20);
    const uint16_t shentsize = r.U16(0x2e);
    const uint16_t shnum = r.U16(0x30);
    if (shentsize < kShdrSize || shnum == 0) {
        return false;
    }
    // Whole section header table must fit.
    if (shoff == 0 || shoff > data.size() ||
        static_cast<uint64_t>(shnum) * shentsize > data.size() - shoff) {
        return false;
    }

    // Locate the .symtab section and its linked .strtab.
    size_t symOff = 0, symSize = 0;
    uint32_t strLink = 0;
    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t sh = shoff + static_cast<size_t>(i) * shentsize;
        if (r.U32(sh + kShType) == kShtSymtab) {
            symOff = r.U32(sh + kShOffset);
            symSize = r.U32(sh + kShSize);
            strLink = r.U32(sh + kShLink);
            // Only honour the standard 16-byte entry size.
            if (r.U32(sh + kShEntsize) != kSymSize) {
                return false;
            }
            break;
        }
    }
    if (symOff == 0 || symSize == 0 || strLink >= shnum) {
        return false; // no symbol table
    }
    if (symOff > data.size() || symSize > data.size() - symOff) {
        return false;
    }

    // Resolve the linked string table.
    const size_t strSh = shoff + static_cast<size_t>(strLink) * shentsize;
    if (r.U32(strSh + kShType) != kShtStrtab) {
        return false;
    }
    const size_t strOff = r.U32(strSh + kShOffset);
    const size_t strSize = r.U32(strSh + kShSize);
    if (strOff > data.size() || strSize > data.size() - strOff) {
        return false;
    }

    // Walk the symbol table, keeping FUNC symbols with a non-empty name.
    const size_t count = symSize / kSymSize;
    m_functions.reserve(count / 4 + 1);
    for (size_t i = 0; i < count; ++i) {
        const size_t se = symOff + i * kSymSize;
        const uint8_t info = data[se + kStInfo];
        if ((info & 0xf) != kSttFunc) {
            continue;
        }
        const uint32_t nameOff = r.U32(se + kStName);
        std::string name = ReadStr(data, strOff, strSize, nameOff);
        if (name.empty()) {
            continue;
        }
        FunctionSymbol fn;
        fn.address = r.U32(se + kStValue);
        fn.size = r.U32(se + kStSize);
        fn.name = std::move(name);
        m_functions.push_back(std::move(fn));
    }

    // Sort by address so ResolvePc can binary-search. Where two symbols share an
    // address, prefer the one with a declared size (more precise range).
    std::sort(m_functions.begin(), m_functions.end(),
              [](const FunctionSymbol& a, const FunctionSymbol& b) {
                  if (a.address != b.address) {
                      return a.address < b.address;
                  }
                  return a.size > b.size;
              });

    return !m_functions.empty();
}

void DebugSymbols::Clear()
{
    m_functions.clear();
    m_sourcePath.clear();
}

const FunctionSymbol* DebugSymbols::ResolvePc(uint32_t linkAddr) const
{
    if (m_functions.empty()) {
        return nullptr;
    }
    // Find the last function whose start address is <= linkAddr.
    auto it =
        std::upper_bound(m_functions.begin(), m_functions.end(), linkAddr,
                         [](uint32_t addr, const FunctionSymbol& fn) { return addr < fn.address; });
    if (it == m_functions.begin()) {
        return nullptr; // before the first function
    }
    --it;
    if (it->size != 0) {
        if (linkAddr >= it->address + it->size) {
            return nullptr; // fell in a gap after a sized function
        }
    } else {
        // Unsized: treat as extending to the next function's start.
        auto next = it + 1;
        if (next != m_functions.end() && linkAddr >= next->address) {
            return nullptr;
        }
    }
    return &*it;
}

std::optional<std::string> DebugSymbols::FindSiblingElf(const std::string& executablePath)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe(executablePath);
    fs::path dir = exe.parent_path();
    if (dir.empty()) {
        dir = ".";
    }
    if (!fs::is_directory(dir, ec)) {
        return std::nullopt;
    }

    // 1) Same base name: a.exe -> a.elf.
    fs::path sameName = exe;
    sameName.replace_extension(".elf");
    if (fs::is_regular_file(sameName, ec)) {
        return sameName.string();
    }

    // 2) Fall back to a lone .elf in the directory.
    std::optional<std::string> onlyElf;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        fs::path p = entry.path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".elf") {
            if (onlyElf) {
                return std::nullopt; // ambiguous: more than one .elf
            }
            onlyElf = p.string();
        }
    }
    return onlyElf;
}

bool DebugSymbols::LoadForExecutable(const std::string& executablePath)
{
    auto elf = FindSiblingElf(executablePath);
    if (!elf) {
        Clear();
        return false;
    }
    return LoadFromElf(*elf);
}

} // namespace adm::core
