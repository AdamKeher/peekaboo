#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/ElfHunkMap.h"
#include "core/SegListResolver.h"

using adm::core::AmigaSegment;
using adm::core::ElfHunkMap;

namespace {

constexpr uint32_t kHunkfChip = 0x40000000;

void PutU32(std::vector<uint8_t>& v, size_t off, uint32_t x)
{
    v[off + 0] = static_cast<uint8_t>(x >> 24);
    v[off + 1] = static_cast<uint8_t>(x >> 16);
    v[off + 2] = static_cast<uint8_t>(x >> 8);
    v[off + 3] = static_cast<uint8_t>(x);
}
void PutU16(std::vector<uint8_t>& v, size_t off, uint16_t x)
{
    v[off + 0] = static_cast<uint8_t>(x >> 8);
    v[off + 1] = static_cast<uint8_t>(x);
}
void AppendU32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

struct SectionSpec
{
    std::string name;
    uint32_t addr = 0;
    uint32_t size = 0;
    bool nobits = false; // .bss
};

// A minimal big-endian ELF32 carrying just the section header table the mapper reads.
std::vector<uint8_t> MakeElf(const std::vector<SectionSpec>& secs)
{
    constexpr uint32_t kShfAlloc = 0x2;
    constexpr uint32_t kShtProgbits = 1, kShtNobits = 8, kShtStrtab = 3;

    // Section 0 is the mandatory null entry; the last is .shstrtab.
    std::string strtab;
    strtab.push_back('\0');
    std::vector<uint32_t> nameOffsets;
    for (const auto& s : secs) {
        nameOffsets.push_back(static_cast<uint32_t>(strtab.size()));
        strtab += s.name;
        strtab.push_back('\0');
    }
    const uint32_t shstrNameOff = static_cast<uint32_t>(strtab.size());
    strtab += ".shstrtab";
    strtab.push_back('\0');

    const uint16_t shnum = static_cast<uint16_t>(secs.size() + 2);
    const uint32_t ehsize = 52, shentsize = 40;
    const uint32_t strtabOff = ehsize;
    const uint32_t shoff = strtabOff + static_cast<uint32_t>(strtab.size());

    std::vector<uint8_t> elf(shoff + static_cast<size_t>(shnum) * shentsize, 0);
    // e_ident
    elf[0] = 0x7F;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 1; // ELFCLASS32
    elf[5] = 2; // ELFDATA2MSB (m68k is big-endian)
    elf[6] = 1;
    PutU16(elf, 0x10, 2); // ET_EXEC
    PutU16(elf, 0x12, 4); // EM_68K
    PutU32(elf, 0x14, 1);
    PutU32(elf, 0x20, shoff);
    PutU16(elf, 0x28, static_cast<uint16_t>(ehsize));
    PutU16(elf, 0x2E, static_cast<uint16_t>(shentsize));
    PutU16(elf, 0x30, shnum);
    PutU16(elf, 0x32, static_cast<uint16_t>(shnum - 1)); // .shstrtab is last

    std::memcpy(elf.data() + strtabOff, strtab.data(), strtab.size());

    for (size_t i = 0; i < secs.size(); ++i) {
        const size_t o = shoff + (i + 1) * shentsize;
        PutU32(elf, o + 0x00, nameOffsets[i]);
        PutU32(elf, o + 0x04, secs[i].nobits ? kShtNobits : kShtProgbits);
        PutU32(elf, o + 0x08, kShfAlloc);
        PutU32(elf, o + 0x0C, secs[i].addr);
        PutU32(elf, o + 0x10, 0);
        PutU32(elf, o + 0x14, secs[i].size);
    }
    const size_t so = shoff + static_cast<size_t>(shnum - 1) * shentsize;
    PutU32(elf, so + 0x00, shstrNameOff);
    PutU32(elf, so + 0x04, kShtStrtab);
    PutU32(elf, so + 0x10, strtabOff);
    PutU32(elf, so + 0x14, static_cast<uint32_t>(strtab.size()));
    return elf;
}

struct HunkSpec
{
    uint32_t bytes = 0;
    bool bss = false;
    bool chip = false;
};

// A minimal AmigaDOS HUNK executable: header + one CODE/DATA/BSS hunk per entry.
std::vector<uint8_t> MakeExe(const std::vector<HunkSpec>& hunks)
{
    constexpr uint32_t HUNK_HEADER = 0x3F3, HUNK_CODE = 0x3E9, HUNK_BSS = 0x3EB, HUNK_END = 0x3F2;
    std::vector<uint8_t> e;
    AppendU32(e, HUNK_HEADER);
    AppendU32(e, 0);                                       // empty resident-library name list
    AppendU32(e, static_cast<uint32_t>(hunks.size()));     // table size
    AppendU32(e, 0);                                       // first
    AppendU32(e, static_cast<uint32_t>(hunks.size() - 1)); // last
    for (const auto& h : hunks) {
        const uint32_t lw = (h.bytes + 3u) / 4u;
        AppendU32(e, lw | (h.chip ? kHunkfChip : 0u));
    }
    for (const auto& h : hunks) {
        const uint32_t lw = (h.bytes + 3u) / 4u;
        AppendU32(e, (h.bss ? HUNK_BSS : HUNK_CODE) | (h.chip ? kHunkfChip : 0u));
        AppendU32(e, lw);
        if (!h.bss)
            e.insert(e.end(), static_cast<size_t>(lw) * 4, 0x4E); // filler opcodes
        AppendU32(e, HUNK_END);
    }
    return e;
}

// The shape a real bartman build has: six sections, two of them forced into chip RAM.
std::vector<SectionSpec> BartmanSections()
{
    return {
        {".text", 0x00000000, 0x7CDC, false},
        {".rodata", 0x00007CDC, 0x751E, false},
        {".MEMF_CHIP", 0x0000F1FA, 0x001C, false},
        {".data", 0x00011218, 0x05E8, false},
        {".INCBIN.MEMF_CHIP", 0x00011800, 0x4671, false},
        {".bss", 0x00015E74, 0x1630, true},
    };
}

std::vector<HunkSpec> BartmanHunks()
{
    return {
        {0x7CDC, false, false}, {0x751E, false, false}, {0x001C, false, true},
        {0x05E8, false, false}, {0x4671, false, true},  {0x1630, true, false},
    };
}

// Runtime bases as AmigaDOS would produce them: chip hunks low, the rest far away.
std::vector<AmigaSegment> BartmanSegments()
{
    return {
        {0x00C0CB68, 0x7CDC}, {0x00C18E40, 0x7520}, {0x00015060, 0x0020},
        {0x00C203C0, 0x05E8}, {0x00015088, 0x4674}, {0x00C216C8, 0x1630},
    };
}

} // namespace

TEST(ElfHunkMapTest, PairsSixSectionsWithSixHunks)
{
    ElfHunkMap map;
    std::string err;
    const auto elf = MakeElf(BartmanSections());
    const auto exe = MakeExe(BartmanHunks());
    ASSERT_TRUE(map.BuildFromBytes(elf, exe, err)) << err;

    ASSERT_EQ(map.Sections().size(), 6u);
    EXPECT_EQ(map.Sections()[0].name, ".text");
    EXPECT_EQ(map.Sections()[0].linkAddr, 0x0u);
    EXPECT_FALSE(map.Sections()[0].chip);
    // The two chip sections must be recognised as chip, from the hunk flags.
    EXPECT_TRUE(map.Sections()[2].chip);
    EXPECT_TRUE(map.Sections()[4].chip);
    EXPECT_TRUE(map.Sections()[5].bss);
    EXPECT_FALSE(map.IsRelocated());
}

TEST(ElfHunkMapTest, RelocatesOntoIndependentlyAllocatedHunks)
{
    ElfHunkMap map;
    std::string err;
    ASSERT_TRUE(map.BuildFromBytes(MakeElf(BartmanSections()), MakeExe(BartmanHunks()), err))
        << err;
    ASSERT_TRUE(map.Relocate(BartmanSegments(), err)) << err;
    ASSERT_TRUE(map.IsRelocated());

    EXPECT_EQ(map.Sections()[0].runtimeBase, 0x00C0CB68u);
    EXPECT_EQ(map.Sections()[2].runtimeBase, 0x00015060u); // chip, nowhere near .text
    EXPECT_EQ(map.Sections()[5].runtimeBase, 0x00C216C8u);

    const std::string args = map.AddSymbolFileArgs();
    EXPECT_NE(args.find("-s .text 0xc0cb68"), std::string::npos);
    EXPECT_NE(args.find("-s .MEMF_CHIP 0x15060"), std::string::npos);
    EXPECT_NE(args.find("-s .bss 0xc216c8"), std::string::npos);
}

TEST(ElfHunkMapTest, TranslatesBothWays)
{
    ElfHunkMap map;
    std::string err;
    ASSERT_TRUE(map.BuildFromBytes(MakeElf(BartmanSections()), MakeExe(BartmanHunks()), err));
    ASSERT_TRUE(map.Relocate(BartmanSegments(), err));

    // A .text address (this is how a DWARF line address becomes a breakpoint address).
    ASSERT_TRUE(map.ToRuntime(0x100).has_value());
    EXPECT_EQ(*map.ToRuntime(0x100), 0x00C0CB68u + 0x100u);
    ASSERT_TRUE(map.ToLink(0x00C0CB68u + 0x100u).has_value());
    EXPECT_EQ(*map.ToLink(0x00C0CB68u + 0x100u), 0x100u);

    // A chip-RAM section: the offset is relative to its own base, not a global slide.
    ASSERT_TRUE(map.ToRuntime(0x0000F1FA).has_value());
    EXPECT_EQ(*map.ToRuntime(0x0000F1FA), 0x00015060u);

    // Round-trip every section boundary.
    for (const auto& s : map.Sections()) {
        ASSERT_TRUE(map.ToRuntime(s.linkAddr).has_value()) << s.name;
        EXPECT_EQ(*map.ToLink(*map.ToRuntime(s.linkAddr)), s.linkAddr) << s.name;
    }
    // An address in no section resolves to nothing rather than guessing.
    EXPECT_FALSE(map.ToRuntime(0x00F00000).has_value());
    EXPECT_FALSE(map.ToLink(0x00F00000).has_value());
}

TEST(ElfHunkMapTest, QOffsetsRefusesToDescribeAChipLayout)
{
    // Six independently allocated hunks, two in chip RAM, cannot be expressed as three offsets.
    // Saying nothing is correct; inventing three would put symbols at wrong addresses.
    ElfHunkMap map;
    std::string err;
    ASSERT_TRUE(map.BuildFromBytes(MakeElf(BartmanSections()), MakeExe(BartmanHunks()), err));
    ASSERT_TRUE(map.Relocate(BartmanSegments(), err));
    EXPECT_TRUE(map.QOffsetsReply().empty());
}

TEST(ElfHunkMapTest, QOffsetsWorksForAPlainThreeSectionProgram)
{
    const std::vector<SectionSpec> secs = {
        {".text", 0x0, 0x1000, false},
        {".data", 0x1000, 0x100, false},
        {".bss", 0x1100, 0x80, true},
    };
    const std::vector<HunkSpec> hunks = {
        {0x1000, false, false}, {0x100, false, false}, {0x80, true, false}};
    ElfHunkMap map;
    std::string err;
    ASSERT_TRUE(map.BuildFromBytes(MakeElf(secs), MakeExe(hunks), err)) << err;
    ASSERT_TRUE(map.Relocate({{0x21000, 0x1000}, {0x30000, 0x100}, {0x40000, 0x80}}, err)) << err;
    EXPECT_EQ(map.QOffsetsReply(), "Text=21000;Data=30000;Bss=40000;");
}

// --- the verification that stops a wrong map being produced silently ------------------------

TEST(ElfHunkMapTest, CountMismatchFailsLoudly)
{
    auto hunks = BartmanHunks();
    hunks.pop_back(); // elf2hunk merged or dropped one
    ElfHunkMap map;
    std::string err;
    EXPECT_FALSE(map.BuildFromBytes(MakeElf(BartmanSections()), MakeExe(hunks), err));
    EXPECT_NE(err.find("count mismatch"), std::string::npos) << err;
}

TEST(ElfHunkMapTest, SizeMismatchFailsLoudly)
{
    auto hunks = BartmanHunks();
    hunks[1].bytes = 0x2000; // hunk far bigger than its section
    ElfHunkMap map;
    std::string err;
    EXPECT_FALSE(map.BuildFromBytes(MakeElf(BartmanSections()), MakeExe(hunks), err));
    EXPECT_NE(err.find("size mismatch"), std::string::npos) << err;
}

TEST(ElfHunkMapTest, ChipPlacementMismatchFailsLoudly)
{
    // Sizes still line up, but the chip section became an any-RAM hunk: the positional pairing
    // has slipped and every symbol would land somewhere wrong.
    auto hunks = BartmanHunks();
    hunks[2].chip = false;
    ElfHunkMap map;
    std::string err;
    EXPECT_FALSE(map.BuildFromBytes(MakeElf(BartmanSections()), MakeExe(hunks), err));
    EXPECT_NE(err.find("placement mismatch"), std::string::npos) << err;
}

TEST(ElfHunkMapTest, RelocateRejectsAMismatchedSegmentList)
{
    ElfHunkMap map;
    std::string err;
    ASSERT_TRUE(map.BuildFromBytes(MakeElf(BartmanSections()), MakeExe(BartmanHunks()), err));

    auto segs = BartmanSegments();
    segs.pop_back();
    EXPECT_FALSE(map.Relocate(segs, err));
    EXPECT_NE(err.find("segment count mismatch"), std::string::npos) << err;
    EXPECT_FALSE(map.IsRelocated());

    // A segment too small for its section is also a refusal: it means we paired the wrong things.
    segs = BartmanSegments();
    segs[0].size = 0x10;
    EXPECT_FALSE(map.Relocate(segs, err));
    EXPECT_NE(err.find("too small"), std::string::npos) << err;
}

TEST(ElfHunkMapTest, RejectsNonElfInput)
{
    ElfHunkMap map;
    std::string err;
    const std::vector<uint8_t> junk(64, 0x00);
    EXPECT_FALSE(map.BuildFromBytes(junk, MakeExe(BartmanHunks()), err));
    EXPECT_NE(err.find("not an ELF"), std::string::npos) << err;
}

TEST(ElfHunkMapTest, UnrelocatedMapAnswersNothingRatherThanGuessing)
{
    ElfHunkMap map;
    std::string err;
    ASSERT_TRUE(map.BuildFromBytes(MakeElf(BartmanSections()), MakeExe(BartmanHunks()), err));
    EXPECT_TRUE(map.AddSymbolFileArgs().empty());
    EXPECT_TRUE(map.QOffsetsReply().empty());
    EXPECT_FALSE(map.ToRuntime(0x100).has_value());
}
