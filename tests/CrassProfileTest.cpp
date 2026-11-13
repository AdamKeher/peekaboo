#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../src/core/CrassProfile.h"

using adm::core::CrassProfileRam;
using adm::core::CrassProfileStats;
using adm::core::ReadCrassProfile;

namespace {

// A synthetic big-endian Amiga RAM image: one chip bank at $000400, one fast bank at $200000,
// with a gap between them so an out-of-range pointer is genuinely out of range.
constexpr uint32_t kChipBase = 0x000400;
constexpr uint32_t kChipSize = 0x080000;
constexpr uint32_t kFastBase = 0x200000;
constexpr uint32_t kFastSize = 0x100000;

class FakeRam
{
public:
    std::vector<CrassProfileRam> Banks() const
    {
        return {{kChipBase, kChipSize}, {kFastBase, kFastSize}};
    }

    void W8(uint32_t addr, uint8_t v) { m_bytes[addr] = v; }
    void W16(uint32_t addr, uint16_t v)
    {
        W8(addr, static_cast<uint8_t>(v >> 8));
        W8(addr + 1, static_cast<uint8_t>(v));
    }
    void W32(uint32_t addr, uint32_t v)
    {
        W16(addr, static_cast<uint16_t>(v >> 16));
        W16(addr + 2, static_cast<uint16_t>(v));
    }
    // Writes a C string WITHOUT its terminator; callers add one (or deliberately do not).
    void WStr(uint32_t addr, const char* s)
    {
        for (uint32_t i = 0; s[i]; ++i)
            W8(addr + i, static_cast<uint8_t>(s[i]));
    }

    // Reads like the emulator's: every byte must have been written, and must lie in a bank.
    // Unwritten-but-mapped bytes read as 0, which is what freshly cleared RAM does.
    adm::core::CrassProfileRead Reader() const
    {
        return [this](uint32_t addr, uint8_t* out, uint32_t len) {
            for (uint32_t i = 0; i < len; ++i) {
                const uint32_t a = addr + i;
                const bool mapped = (a >= kChipBase && a < kChipBase + kChipSize) ||
                                    (a >= kFastBase && a < kFastBase + kFastSize);
                if (!mapped)
                    return false;
                auto it = m_bytes.find(a);
                out[i] = (it == m_bytes.end()) ? 0 : it->second;
            }
            return true;
        };
    }

private:
    std::map<uint32_t, uint8_t> m_bytes;
};

// Layout constants, kept literal so a change to the documented format has to be made here too.
constexpr uint32_t kBlockAddr = 0x001000;
constexpr uint32_t kFramesAddr = 0x001100;
constexpr uint32_t kSectionA = 0x001200;
constexpr uint32_t kSectionB = 0x001300;
constexpr uint32_t kNameA = 0x001400;
constexpr uint32_t kNameB = 0x001420;

void WriteBlock(FakeRam& ram, uint32_t sectionsHead, uint32_t magic = 0x50524631u,
                uint16_t version = 1, uint16_t linesPer2Fields = 626, uint16_t timingOk = 1,
                uint32_t framesPtr = kFramesAddr)
{
    ram.W32(kBlockAddr + 0, magic);
    ram.W16(kBlockAddr + 4, version);
    ram.W16(kBlockAddr + 6, 250); // window_fields
    ram.W16(kBlockAddr + 8, linesPer2Fields);
    ram.W16(kBlockAddr + 10, timingOk);
    ram.W32(kBlockAddr + 12, framesPtr);
    ram.W32(kBlockAddr + 16, sectionsHead);
}

void WriteFrames(FakeRam& ram, uint16_t rendered, uint16_t worstGap, uint16_t lastFields,
                 uint16_t lastRendered, uint16_t lastDropped, uint16_t lastWorstGap,
                 uint32_t totalRendered, uint32_t totalDropped)
{
    ram.W16(kFramesAddr + 0, 1); // based
    ram.W32(kFramesAddr + 2, 7); // last (internal)
    ram.W32(kFramesAddr + 6, 3); // window_start (internal)
    ram.W16(kFramesAddr + 10, rendered);
    ram.W16(kFramesAddr + 12, worstGap);
    ram.W16(kFramesAddr + 14, lastFields);
    ram.W16(kFramesAddr + 16, lastRendered);
    ram.W16(kFramesAddr + 18, lastDropped);
    ram.W16(kFramesAddr + 20, lastWorstGap);
    ram.W32(kFramesAddr + 22, totalRendered);
    ram.W32(kFramesAddr + 26, totalDropped);
}

struct SectionFields
{
    uint32_t namePtr = 0;
    uint32_t next = 0;
    uint32_t total = 0;
    uint32_t count = 0;
    uint16_t min = 0xFFFF;
    uint16_t max = 0;
    uint16_t lastMin = 0;
    uint16_t lastAvg = 0;
    uint16_t lastMax = 0;
    uint32_t lastCount = 0;
    uint16_t lastLines = 0;
};

void WriteSection(FakeRam& ram, uint32_t addr, const SectionFields& f)
{
    ram.W32(addr + 0, f.namePtr);
    ram.W32(addr + 4, f.next);
    ram.W16(addr + 8, 1);  // registered
    ram.W32(addr + 10, 0); // start (internal)
    ram.W32(addr + 14, f.total);
    ram.W32(addr + 18, f.count);
    ram.W16(addr + 22, f.min);
    ram.W16(addr + 24, f.max);
    ram.W16(addr + 26, f.lastMin);
    ram.W16(addr + 28, f.lastAvg);
    ram.W16(addr + 30, f.lastMax);
    ram.W32(addr + 32, f.lastCount);
    ram.W16(addr + 36, f.lastLines);
}

// A complete, valid image: two sections, one completed window.
FakeRam MakeValid()
{
    FakeRam ram;
    WriteBlock(ram, kSectionA);
    WriteFrames(ram, 37, 1, 250, 250, 0, 1, 1250, 3);

    ram.WStr(kNameA, "cv_build");
    ram.W8(kNameA + 8, 0);
    ram.WStr(kNameB, "cv_blits");
    ram.W8(kNameB + 8, 0);

    SectionFields a;
    a.namePtr = kNameA;
    a.next = kSectionB;
    a.total = 38500;
    a.count = 250;
    a.min = 96;
    a.max = 161;
    a.lastMin = 96;
    a.lastAvg = 154;
    a.lastMax = 161;
    a.lastCount = 250;
    a.lastLines = 154;
    WriteSection(ram, kSectionA, a);

    SectionFields b;
    b.namePtr = kNameB;
    b.next = 0;
    b.lastMin = 60;
    b.lastAvg = 153;
    b.lastMax = 205;
    b.lastCount = 250;
    b.lastLines = 153;
    WriteSection(ram, kSectionB, b);
    return ram;
}

bool Read(const FakeRam& ram, CrassProfileStats& out, uint32_t blockAddr = kBlockAddr)
{
    return ReadCrassProfile(ram.Reader(), ram.Banks(), blockAddr, out);
}

} // namespace

TEST(CrassProfileTest, ReadsBlockFramesAndSections)
{
    FakeRam ram = MakeValid();
    CrassProfileStats s;
    ASSERT_TRUE(Read(ram, s));

    EXPECT_TRUE(s.present);
    EXPECT_EQ(s.blockAddress, kBlockAddr);
    EXPECT_EQ(s.version, 1);
    EXPECT_EQ(s.windowFields, 250);
    EXPECT_EQ(s.linesPer2Fields, 626);
    EXPECT_TRUE(s.timingOk);

    EXPECT_EQ(s.rendered, 37);
    EXPECT_EQ(s.worstGap, 1);
    EXPECT_EQ(s.lastFields, 250);
    EXPECT_EQ(s.lastRendered, 250);
    EXPECT_EQ(s.lastDropped, 0);
    EXPECT_EQ(s.lastWorstGap, 1);
    EXPECT_EQ(s.totalRendered, 1250u);
    EXPECT_EQ(s.totalDropped, 3u);

    ASSERT_EQ(s.sections.size(), 2u);
    EXPECT_FALSE(s.sectionsTruncated);

    EXPECT_EQ(s.sections[0].name, "cv_build");
    EXPECT_EQ(s.sections[0].lastLines, 154);
    EXPECT_EQ(s.sections[0].lastMin, 96);
    EXPECT_EQ(s.sections[0].lastAvg, 154);
    EXPECT_EQ(s.sections[0].lastMax, 161);
    EXPECT_EQ(s.sections[0].lastCount, 250u);
    EXPECT_EQ(s.sections[0].total, 38500u);
    EXPECT_EQ(s.sections[0].count, 250u);
    EXPECT_EQ(s.sections[0].min, 96);
    EXPECT_EQ(s.sections[0].max, 161);

    EXPECT_EQ(s.sections[1].name, "cv_blits");
    EXPECT_EQ(s.sections[1].lastMin, 60);
    EXPECT_EQ(s.sections[1].lastAvg, 153);
    EXPECT_EQ(s.sections[1].lastMax, 205);
    // The current window has not measured anything yet: min is the engine's 0xffff sentinel.
    EXPECT_EQ(s.sections[1].min, 0xFFFF);
    EXPECT_EQ(s.sections[1].count, 0u);
}

TEST(CrassProfileTest, BadMagicIsNotPresent)
{
    FakeRam ram = MakeValid();
    WriteBlock(ram, kSectionA, 0x50524632u); // "PRF2"
    CrassProfileStats s;
    EXPECT_FALSE(Read(ram, s));
    EXPECT_FALSE(s.present);
}

TEST(CrassProfileTest, WrongVersionIsNotPresent)
{
    FakeRam ram = MakeValid();
    WriteBlock(ram, kSectionA, 0x50524631u, 2);
    CrassProfileStats s;
    EXPECT_FALSE(Read(ram, s));
    EXPECT_FALSE(s.present);
}

TEST(CrassProfileTest, ImplausibleLineCountIsNotPresent)
{
    FakeRam ram = MakeValid();
    WriteBlock(ram, kSectionA, 0x50524631u, 1, 512);
    CrassProfileStats s;
    EXPECT_FALSE(Read(ram, s));

    // ...and the plausible ones are accepted: long, short, interlaced.
    for (int lines : {624, 625, 626}) {
        FakeRam ok = MakeValid();
        WriteBlock(ok, kSectionA, 0x50524631u, 1, static_cast<uint16_t>(lines));
        CrassProfileStats t;
        EXPECT_TRUE(Read(ok, t)) << lines;
        EXPECT_EQ(t.linesPer2Fields, lines);
    }
}

TEST(CrassProfileTest, PointerOutOfRangeIsNotPresent)
{
    // The frames pointer lands in the gap between the two banks.
    FakeRam ram = MakeValid();
    WriteBlock(ram, kSectionA, 0x50524631u, 1, 626, 1, 0x100000);
    CrassProfileStats s;
    EXPECT_FALSE(Read(ram, s));

    // A section pointer past the end of fast RAM is caught the same way.
    FakeRam ram2 = MakeValid();
    WriteBlock(ram2, 0x900000);
    CrassProfileStats s2;
    EXPECT_FALSE(Read(ram2, s2));

    // So is a block address that is mapped but whose last bytes are not.
    FakeRam ram3 = MakeValid();
    CrassProfileStats s3;
    EXPECT_FALSE(Read(ram3, s3, kChipBase + kChipSize - 8));

    // And the block address itself being outside RAM entirely.
    FakeRam ram4 = MakeValid();
    CrassProfileStats s4;
    EXPECT_FALSE(Read(ram4, s4, 0xF80000));
}

TEST(CrassProfileTest, NullBlockOrFramesPointerIsNotPresent)
{
    FakeRam ram = MakeValid();
    WriteBlock(ram, kSectionA, 0x50524631u, 1, 626, 1, 0);
    CrassProfileStats s;
    EXPECT_FALSE(Read(ram, s));

    FakeRam ram2 = MakeValid();
    CrassProfileStats s2;
    EXPECT_FALSE(Read(ram2, s2, 0));
}

TEST(CrassProfileTest, CyclicSectionListIsNotPresent)
{
    FakeRam ram = MakeValid();
    // Point the tail back at the head.
    ram.W32(kSectionB + 4, kSectionA);
    CrassProfileStats s;
    EXPECT_FALSE(Read(ram, s));
    EXPECT_FALSE(s.present);
}

TEST(CrassProfileTest, SelfCyclicSectionIsNotPresent)
{
    FakeRam ram = MakeValid();
    ram.W32(kSectionA + 4, kSectionA);
    CrassProfileStats s;
    EXPECT_FALSE(Read(ram, s));
}

TEST(CrassProfileTest, UnterminatedNameIsNotPresent)
{
    FakeRam ram = MakeValid();
    // 40 printable characters with no NUL inside the first 32.
    for (uint32_t i = 0; i < 40; ++i)
        ram.W8(kNameA + i, 'x');
    CrassProfileStats s;
    EXPECT_FALSE(Read(ram, s));
    EXPECT_FALSE(s.present);
}

TEST(CrassProfileTest, NonPrintableOrEmptyNameIsNotPresent)
{
    FakeRam ram = MakeValid();
    ram.W8(kNameA + 2, 0x01);
    CrassProfileStats s;
    EXPECT_FALSE(Read(ram, s));

    // An immediate NUL is the shape of a pointer into freshly cleared memory.
    FakeRam ram2 = MakeValid();
    ram2.W8(kNameB, 0);
    CrassProfileStats s2;
    EXPECT_FALSE(Read(ram2, s2));

    // A null name pointer likewise.
    FakeRam ram3 = MakeValid();
    ram3.W32(kSectionA + 0, 0);
    CrassProfileStats s3;
    EXPECT_FALSE(Read(ram3, s3));
}

TEST(CrassProfileTest, NameAtTheVeryTopOfABankStillReads)
{
    // The read must be clipped to the bank rather than running off the end and failing.
    FakeRam ram = MakeValid();
    const uint32_t nameAddr = kFastBase + kFastSize - 5;
    ram.WStr(nameAddr, "edge");
    ram.W8(nameAddr + 4, 0);
    ram.W32(kSectionA + 0, nameAddr);
    CrassProfileStats s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_EQ(s.sections[0].name, "edge");
}

TEST(CrassProfileTest, NoSectionsYetIsStillPresent)
{
    // profile_init() runs before any section registers on its first START.
    FakeRam ram = MakeValid();
    WriteBlock(ram, 0);
    CrassProfileStats s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_TRUE(s.present);
    EXPECT_TRUE(s.sections.empty());
    EXPECT_FALSE(s.sectionsTruncated);
}

TEST(CrassProfileTest, NoCompletedWindowIsStillPresent)
{
    FakeRam ram = MakeValid();
    WriteFrames(ram, 12, 2, 0, 0, 0, 0, 12, 0);
    CrassProfileStats s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_TRUE(s.present);
    EXPECT_EQ(s.lastFields, 0); // "no window has completed yet"
    EXPECT_EQ(s.rendered, 12);
    EXPECT_EQ(s.worstGap, 2);
}

TEST(CrassProfileTest, SectionWalkIsCappedAtSixteen)
{
    FakeRam ram;
    WriteBlock(ram, kFastBase);
    WriteFrames(ram, 1, 1, 250, 250, 0, 1, 250, 0);

    // 20 linked sections, all valid.
    constexpr int kLinked = 20;
    for (int i = 0; i < kLinked; ++i) {
        const uint32_t addr = kFastBase + static_cast<uint32_t>(i) * 0x100;
        const uint32_t nameAddr = addr + 0x40;
        const std::string name = "sec" + std::to_string(i);
        ram.WStr(nameAddr, name.c_str());
        ram.W8(nameAddr + static_cast<uint32_t>(name.size()), 0);

        SectionFields f;
        f.namePtr = nameAddr;
        f.next = (i + 1 < kLinked) ? (addr + 0x100) : 0;
        f.lastCount = 250;
        WriteSection(ram, addr, f);
    }

    CrassProfileStats s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_EQ(s.sections.size(), 16u);
    EXPECT_TRUE(s.sectionsTruncated);
    EXPECT_EQ(s.sections.front().name, "sec0");
    EXPECT_EQ(s.sections.back().name, "sec15");
}

TEST(CrassProfileTest, TimingNotOkIsReportedButStillPresent)
{
    // The overlay draws "line counter not running" rather than hiding itself, so the read has
    // to succeed and simply carry the flag.
    FakeRam ram = MakeValid();
    WriteBlock(ram, kSectionA, 0x50524631u, 1, 626, 0);
    CrassProfileStats s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_TRUE(s.present);
    EXPECT_FALSE(s.timingOk);
}

TEST(CrassProfileTest, OddStructPointerIsNotPresent)
{
    // Every struct here holds word members, so the Amiga side can never put one on an odd
    // address; a pointer that is odd is garbage by definition.
    FakeRam ram = MakeValid();
    WriteBlock(ram, kSectionA + 1);
    CrassProfileStats s;
    EXPECT_FALSE(Read(ram, s));
}

TEST(CrassProfileTest, FailedReadLeavesOutputCleared)
{
    FakeRam ram = MakeValid();
    CrassProfileStats s;
    ASSERT_TRUE(Read(ram, s));
    ASSERT_FALSE(s.sections.empty());

    // A second, failing read must not leave the previous program's numbers behind.
    FakeRam bad = MakeValid();
    WriteBlock(bad, kSectionA, 0x00000000u);
    EXPECT_FALSE(Read(bad, s));
    EXPECT_FALSE(s.present);
    EXPECT_TRUE(s.sections.empty());
    EXPECT_EQ(s.lastRendered, 0);
    EXPECT_EQ(s.blockAddress, 0u);
}

TEST(CrassProfileTest, MissingReaderIsNotPresent)
{
    CrassProfileStats s;
    EXPECT_FALSE(ReadCrassProfile(nullptr, {{kChipBase, kChipSize}}, kBlockAddr, s));
    EXPECT_FALSE(s.present);
}

TEST(CrassProfileTest, NoRamBanksIsNotPresent)
{
    FakeRam ram = MakeValid();
    CrassProfileStats s;
    EXPECT_FALSE(ReadCrassProfile(ram.Reader(), {}, kBlockAddr, s));
}
