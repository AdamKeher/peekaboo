#include "ElfHunkMap.h"

#include <cstdio>
#include <fstream>

#include "HunkParser.h"

namespace adm::core {

namespace {

constexpr uint32_t kShfAlloc = 0x2;
constexpr uint32_t kShtNobits = 8;

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

struct ElfReader
{
    std::span<const uint8_t> d;
    bool bigEndian = true;

    bool Ok(size_t off, size_t len) const { return off + len <= d.size(); }

    uint16_t U16(size_t off) const
    {
        if (!Ok(off, 2))
            return 0;
        return bigEndian ? static_cast<uint16_t>((d[off] << 8) | d[off + 1])
                         : static_cast<uint16_t>((d[off + 1] << 8) | d[off]);
    }
    uint32_t U32(size_t off) const
    {
        if (!Ok(off, 4))
            return 0;
        if (bigEndian)
            return (static_cast<uint32_t>(d[off]) << 24) |
                   (static_cast<uint32_t>(d[off + 1]) << 16) |
                   (static_cast<uint32_t>(d[off + 2]) << 8) | d[off + 3];
        return (static_cast<uint32_t>(d[off + 3]) << 24) |
               (static_cast<uint32_t>(d[off + 2]) << 16) |
               (static_cast<uint32_t>(d[off + 1]) << 8) | d[off];
    }
};

std::string Hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

// AmigaDOS rounds every allocation up to a longword, and elf2hunk may pad a little further, so
// an exact size match is the wrong test. The hunk must be at least as large as the section and
// not wildly larger.
bool SizesAgree(uint32_t sectionSize, uint32_t hunkSize)
{
    const uint32_t rounded = (sectionSize + 3u) & ~3u;
    if (hunkSize < rounded)
        return false;
    return hunkSize - rounded <= 16u;
}

} // namespace

void ElfHunkMap::Clear()
{
    m_sections.clear();
    m_relocated = false;
}

bool ElfHunkMap::Build(const std::string& elfPath, const std::string& exePath,
                       std::string& outError)
{
    std::vector<uint8_t> elf, exe;
    if (!ReadFileBytes(elfPath, elf)) {
        outError = "could not read ELF: " + elfPath;
        return false;
    }
    if (!ReadFileBytes(exePath, exe)) {
        outError = "could not read executable: " + exePath;
        return false;
    }
    return BuildFromBytes(elf, exe, outError);
}

bool ElfHunkMap::BuildFromBytes(std::span<const uint8_t> elf, std::span<const uint8_t> exe,
                                std::string& outError)
{
    Clear();

    if (elf.size() < 52 || elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
        outError = "not an ELF file";
        return false;
    }
    if (elf[4] != 1) {
        outError = "only 32-bit ELF is supported";
        return false;
    }

    ElfReader r{elf, elf[5] == 2};
    const uint32_t shoff = r.U32(0x20);
    const uint16_t shentsize = r.U16(0x2E);
    const uint16_t shnum = r.U16(0x30);
    const uint16_t shstrndx = r.U16(0x32);
    if (shoff == 0 || shnum == 0 || shentsize < 40) {
        outError = "ELF has no section headers";
        return false;
    }
    const uint32_t shstrOff = r.U32(shoff + static_cast<size_t>(shstrndx) * shentsize + 0x10);

    // Collect allocated sections in section-header order: that is the order elf2hunk emits hunks.
    std::vector<MappedSection> sections;
    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t o = shoff + static_cast<size_t>(i) * shentsize;
        const uint32_t nameOff = r.U32(o + 0x00);
        const uint32_t type = r.U32(o + 0x04);
        const uint32_t flags = r.U32(o + 0x08);
        const uint32_t addr = r.U32(o + 0x0C);
        const uint32_t size = r.U32(o + 0x14);
        if (!(flags & kShfAlloc) || size == 0)
            continue;

        std::string name;
        size_t p = shstrOff + nameOff;
        while (p < elf.size() && elf[p] != 0)
            name.push_back(static_cast<char>(elf[p++]));

        MappedSection ms;
        ms.name = name;
        ms.linkAddr = addr;
        ms.size = size;
        ms.bss = (type == kShtNobits);
        sections.push_back(std::move(ms));
    }
    if (sections.empty()) {
        outError = "ELF has no allocated sections";
        return false;
    }

    // Hunk side.
    const std::vector<uint8_t> exeVec(exe.begin(), exe.end());
    const HunkParseResult hunks = HunkParser::Parse(exeVec);
    if (!hunks.success) {
        outError = "could not parse executable: " + hunks.errorMessage;
        return false;
    }
    std::vector<const HunkInfo*> loadable;
    for (const auto& h : hunks.hunks) {
        if (h.type == HunkType::Code || h.type == HunkType::Data || h.type == HunkType::BSS)
            loadable.push_back(&h);
    }

    if (loadable.size() != sections.size()) {
        outError = "section/hunk count mismatch: ELF has " + std::to_string(sections.size()) +
                   " allocated sections but the executable has " + std::to_string(loadable.size()) +
                   " loadable hunks";
        return false;
    }

    // Positional pairing, verified rather than assumed.
    constexpr uint32_t kHunkfChip = 0x40000000;
    for (size_t i = 0; i < sections.size(); ++i) {
        const HunkInfo* h = loadable[i];
        MappedSection& s = sections[i];
        const uint32_t hunkBytes = h->allocSize ? h->allocSize : h->size;

        if (!SizesAgree(s.size, hunkBytes)) {
            outError = "section/hunk size mismatch at index " + std::to_string(i) + ": section " +
                       s.name + " is " + std::to_string(s.size) + " bytes but hunk " +
                       std::to_string(i) + " is " + std::to_string(hunkBytes);
            return false;
        }
        // An independent cross-check: a .MEMF_CHIP section must have become a chip hunk. This
        // catches a reordering that happened to preserve sizes.
        const bool hunkChip = (h->memoryFlags & kHunkfChip) != 0;
        const bool nameChip = s.name.find("MEMF_CHIP") != std::string::npos;
        if (nameChip != hunkChip) {
            outError = "section/hunk placement mismatch at index " + std::to_string(i) + ": " +
                       s.name + (nameChip ? " expects chip RAM" : " expects any RAM") +
                       " but the hunk says otherwise";
            return false;
        }
        s.chip = hunkChip;
        s.hunkIndex = static_cast<int>(i);
    }

    m_sections = std::move(sections);
    return true;
}

bool ElfHunkMap::Relocate(const std::vector<AmigaSegment>& segments, std::string& outError)
{
    if (m_sections.empty()) {
        outError = "no section map has been built";
        return false;
    }
    if (segments.size() != m_sections.size()) {
        outError = "segment count mismatch: mapped " + std::to_string(m_sections.size()) +
                   " sections but the running process has " + std::to_string(segments.size()) +
                   " segments";
        return false;
    }
    for (size_t i = 0; i < m_sections.size(); ++i) {
        // The seglist reports the usable payload length, which must still cover the section.
        if (segments[i].size != 0 && !SizesAgree(m_sections[i].size, segments[i].size)) {
            outError = "segment " + std::to_string(i) + " is " + std::to_string(segments[i].size) +
                       " bytes, too small for section " + m_sections[i].name + " (" +
                       std::to_string(m_sections[i].size) + ")";
            return false;
        }
    }
    for (size_t i = 0; i < m_sections.size(); ++i) {
        m_sections[i].runtimeBase = segments[i].base;
        m_sections[i].relocated = true;
    }
    m_relocated = true;
    return true;
}

std::string ElfHunkMap::AddSymbolFileArgs() const
{
    if (!m_relocated)
        return {};
    std::string out;
    for (const auto& s : m_sections) {
        if (!out.empty())
            out += ' ';
        out += "-s " + s.name + " " + Hex(s.runtimeBase);
    }
    return out;
}

std::string ElfHunkMap::DescribeSections() const
{
    std::string out;
    char line[256];
    for (const auto& s : m_sections) {
        if (m_relocated) {
            std::snprintf(line, sizeof(line), "%-20s link 0x%08x  runtime 0x%08x  size %-8u %s\n",
                          s.name.c_str(), s.linkAddr, s.runtimeBase, s.size, s.chip ? "CHIP" : "");
        } else {
            std::snprintf(line, sizeof(line), "%-20s link 0x%08x  (not relocated)  size %-8u %s\n",
                          s.name.c_str(), s.linkAddr, s.size, s.chip ? "CHIP" : "");
        }
        out += line;
    }
    return out;
}

std::string ElfHunkMap::QOffsetsReply() const
{
    if (!m_relocated)
        return {};
    // Three offsets cannot describe independently allocated hunks. Rather than pick three and
    // hope, say nothing whenever the layout is genuinely richer than Text/Data/Bss.
    const MappedSection* text = nullptr;
    const MappedSection* data = nullptr;
    const MappedSection* bss = nullptr;
    for (const auto& s : m_sections) {
        if (s.chip)
            return {}; // chip hunks live nowhere near the others
        if (s.name == ".text")
            text = &s;
        else if (s.name == ".data")
            data = &s;
        else if (s.name == ".bss")
            bss = &s;
        else
            return {}; // some other allocated section qOffsets has no slot for
    }
    if (!text || !data || !bss)
        return {};
    return "Text=" + Hex(text->runtimeBase).substr(2) +
           ";Data=" + Hex(data->runtimeBase).substr(2) + ";Bss=" + Hex(bss->runtimeBase).substr(2) +
           ";";
}

std::optional<uint32_t> ElfHunkMap::ToRuntime(uint32_t linkAddr) const
{
    if (!m_relocated)
        return std::nullopt;
    for (const auto& s : m_sections) {
        if (linkAddr >= s.linkAddr && linkAddr < s.linkAddr + s.size)
            return s.runtimeBase + (linkAddr - s.linkAddr);
    }
    return std::nullopt;
}

std::optional<uint32_t> ElfHunkMap::ToLink(uint32_t runtimeAddr) const
{
    if (!m_relocated)
        return std::nullopt;
    for (const auto& s : m_sections) {
        if (runtimeAddr >= s.runtimeBase && runtimeAddr < s.runtimeBase + s.size)
            return s.linkAddr + (runtimeAddr - s.runtimeBase);
    }
    return std::nullopt;
}

} // namespace adm::core
