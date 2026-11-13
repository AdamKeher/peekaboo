#include <gtest/gtest.h>

#include <map>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/core/ExocetBlock.h"

using adm::core::ExocetBlockState;
using adm::core::ExocetRam;
using adm::core::ReadExocetBlock;

namespace {

// A synthetic big-endian Amiga RAM image, as in CrassProfileTest: a chip bank and a fast bank
// with a gap between them, so an out-of-range pointer really is out of range.
constexpr uint32_t kChipBase = 0x000400;
constexpr uint32_t kChipSize = 0x080000;
constexpr uint32_t kFastBase = 0x200000;
constexpr uint32_t kFastSize = 0x100000;

class FakeRam
{
public:
    std::vector<ExocetRam> Banks() const
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

    // a bank's bytes as the emulator holds them, for FindExocetBlock's scan
    std::vector<uint8_t> Bytes(uint32_t base, uint32_t size) const
    {
        std::vector<uint8_t> v(size, 0);
        for (const auto& [addr, b] : m_bytes)
            if (addr >= base && addr < base + size)
                v[addr - base] = b;
        return v;
    }

    adm::core::ExocetRead Reader() const
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

// The crass repo's ExoBlock layout (engine/exocet.h), kept literal here so a change to it has to
// be made in this test too.
constexpr uint32_t kBlock = 0x001000;
constexpr uint32_t kValues = 0x001100;
constexpr uint32_t kCuts = 0x001200;
constexpr uint32_t kArena = 0x210000;
constexpr uint16_t kTracks = 4;

void WriteBlock(FakeRam& ram, uint16_t firedCount = 0, uint16_t cutCount = 5)
{
    ram.W32(kBlock + 0, 0x45584F42); // "EXOB"
    ram.W16(kBlock + 4, 4);
    ram.W16(kBlock + 6, kTracks);
    ram.W32(kBlock + 8, 0x04E044C9);
    ram.W16(kBlock + 12, 1340); // row
    ram.W16(kBlock + 14, 0x8000);
    ram.W16(kBlock + 16, 5362);       // frame
    ram.W16(kBlock + 18, 0);          // clock: the music
    ram.W32(kBlock + 20, 0x00010003); // enabled: a long, group 16 among them
    ram.W16(kBlock + 24, 2);          // generation
    ram.W16(kBlock + 26, 1);          // seen
    ram.W16(kBlock + 28, firedCount);
    ram.W32(kBlock + 30, kValues);
    ram.W32(kBlock + 34, kArena);
    ram.W32(kBlock + 38, 4096 + 1200);
    // fired[16] x (track, row, frame): slot i holds the i-th event written into it
    for (int i = 0; i < 16; ++i) {
        ram.W16(kBlock + 42 + 6 * i, static_cast<uint16_t>(i % kTracks));
        ram.W16(kBlock + 44 + 6 * i, static_cast<uint16_t>(100 + i));
        ram.W16(kBlock + 46 + 6 * i, static_cast<uint16_t>(400 + i));
    }
    ram.W16(kBlock + 138, cutCount);
    ram.W32(kBlock + 140, kCuts);
    ram.W16(kBlock + 144, 0);
    ram.W16(kBlock + 146, 0);
    ram.W16(kBlock + 148, 0);
    ram.W32(kBlock + 152, 0);
    ram.W32(kBlock + 156, 0);
    ram.W16(kBlock + 172, 0);  // flags: no optional section
    ram.W16(kBlock + 174, 50); // fps

    ram.W32(kValues + 0, 16);
    ram.W32(kValues + 4, 0xFFFFFFF8); // -8
    ram.W32(kValues + 8, 0x00018000);
    ram.W32(kValues + 12, 4);

    // cut 0 never keyed, cut 1 made, cut 2 ready only, cuts 3-4 untouched
    for (int c = 0; c < cutCount; ++c)
        for (int f = 0; f < 4; ++f)
            ram.W16(kCuts + 8 * c + 2 * f, 0xFFFF);
    ram.W16(kCuts + 8 + 0, 11);
    ram.W16(kCuts + 8 + 2, 521);
    ram.W16(kCuts + 8 + 4, 12);
    ram.W16(kCuts + 8 + 6, 577);
    ram.W16(kCuts + 16 + 0, 540);
    ram.W16(kCuts + 16 + 2, 2148);

    // the arena's exocet.bin header
    ram.W32(kArena + 0, 0x45584F31); // "EXO1"
    ram.W16(kArena + 4, 3);
    ram.W16(kArena + 6, kTracks);
    ram.W32(kArena + 8, 0x04E044C9);
    ram.W32(kArena + 12, 0x111F7093);
    ram.W16(kArena + 16, 3617);
    ram.W16(kArena + 18, 67);
    ram.W16(kArena + 20, 9);
    ram.W16(kArena + 22, 14468);
    ram.W32(kArena + 36, 0x5D90BC20); // the main song's digest
}

bool Read(const FakeRam& ram, ExocetBlockState& s)
{
    return ReadExocetBlock(ram.Reader(), ram.Banks(), kBlock, s);
}

} // namespace

TEST(ExocetBlockTest, ReadsAValidBlock)
{
    FakeRam ram;
    WriteBlock(ram, 3);
    ExocetBlockState s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_TRUE(s.present);
    EXPECT_EQ(s.blockAddress, kBlock);
    EXPECT_EQ(s.tracks, kTracks);
    EXPECT_EQ(s.manifest, 0x04E044C9u);
    EXPECT_EQ(s.row, 1340);
    EXPECT_EQ(s.frac, 0x8000);
    EXPECT_EQ(s.frame, 5362);
    EXPECT_EQ(s.clock, 0);
    EXPECT_EQ(s.enabled, 0x10003u);
    EXPECT_EQ(s.fps, 50);
    EXPECT_EQ(s.flags, 0);
    EXPECT_EQ(s.generation, 2);
    EXPECT_EQ(s.seen, 1);
    EXPECT_FALSE(s.Settled());

    ASSERT_EQ(s.values.size(), 4u);
    EXPECT_EQ(s.values[0], 16);
    EXPECT_EQ(s.values[1], -8);
    EXPECT_EQ(s.values[2], 0x18000);

    EXPECT_EQ(s.arenaAddress, kArena);
    EXPECT_TRUE(s.arena.valid);
    EXPECT_EQ(s.arena.manifest, 0x04E044C9u);
    EXPECT_EQ(s.arena.song, 0x111F7093u);
    EXPECT_EQ(s.arena.rows, 3617);
    EXPECT_EQ(s.arena.keys, 67);
    EXPECT_EQ(s.arena.spans, 9);
    EXPECT_EQ(s.arena.frames, 14468);
    EXPECT_EQ(s.arena.main, 0x5D90BC20u);

    ASSERT_EQ(s.fired.size(), 3u);
    EXPECT_EQ(s.fired[0].row, 100);
    EXPECT_EQ(s.fired[2].frame, 402);

    // only the cuts that have been ready or made
    ASSERT_EQ(s.cuts.size(), 2u);
    EXPECT_EQ(s.cuts[0].cut, 1);
    EXPECT_EQ(s.cuts[0].readyFrame, 521);
    EXPECT_EQ(s.cuts[0].cutFrame, 577);
    EXPECT_EQ(s.cuts[1].cut, 2);
    EXPECT_EQ(s.cuts[1].readyFrame, 2148);
    EXPECT_EQ(s.cuts[1].cutFrame, 0xFFFF);
}

TEST(ExocetBlockTest, FireRingComesBackOldestFirstOnceItWraps)
{
    FakeRam ram;
    WriteBlock(ram, 18); // 18 delivered: slots 2..15 then 0, 1 hold the last 16
    ExocetBlockState s;
    ASSERT_TRUE(Read(ram, s));
    ASSERT_EQ(s.fired.size(), 16u);
    EXPECT_EQ(s.fired.front().row, 102);
    EXPECT_EQ(s.fired.back().row, 101);
}

TEST(ExocetBlockTest, ArenaWithoutAHeaderIsStillABlock)
{
    // A push in progress, or a program still loading its bin: the block is there, the arena is
    // not an exocet.bin yet.
    FakeRam ram;
    WriteBlock(ram);
    ram.W32(kArena, 0);
    ExocetBlockState s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_FALSE(s.arena.valid);
}

TEST(ExocetBlockTest, RefusesWhatIsNotABlock)
{
    struct Case
    {
        const char* what;
        uint32_t addr;
        uint32_t value;
        int width;
    };
    const Case cases[] = {
        {"magic", kBlock + 0, 0x50524631, 32},
        {"version too new", kBlock + 4, 5, 16},
        {"version 3", kBlock + 4, 3, 16},
        {"no tracks", kBlock + 6, 0, 16},
        {"too many tracks", kBlock + 6, 256, 16},
        {"clock", kBlock + 18, 2, 16},
        {"values odd", kBlock + 30, kValues + 1, 32},
        {"values null", kBlock + 30, 0, 32},
        {"values in the gap", kBlock + 30, 0x100000, 32},
        {"arena odd", kBlock + 34, kArena + 1, 32},
        {"arena too small", kBlock + 38, 35, 32},
        {"arena too big", kBlock + 38, 2 * 1024 * 1024, 32},
        {"arena runs off its bank", kBlock + 38, kFastSize, 32},
        {"cut count", kBlock + 138, 33, 16},
        {"cuts outside RAM", kBlock + 140, 0x100000, 32},
        {"frame rate", kBlock + 174, 25, 16},
    };
    for (const auto& c : cases) {
        FakeRam ram;
        WriteBlock(ram);
        if (c.width == 32)
            ram.W32(c.addr, c.value);
        else
            ram.W16(c.addr, static_cast<uint16_t>(c.value));
        ExocetBlockState s;
        s.present = true;
        EXPECT_FALSE(Read(ram, s)) << c.what;
        EXPECT_FALSE(s.present) << c.what;
        EXPECT_TRUE(s.values.empty()) << c.what;
    }
}

TEST(ExocetBlockTest, OptionalSectionsAreReadOnlyWhenFlagged)
{
    FakeRam ram;
    WriteBlock(ram);
    ram.W16(kBlock + 144, 7);      // reseat: not flagged, not read
    ram.W32(kBlock + 152, 0x2000); // music
    ram.W32(kBlock + 160, 0x2000); // memmap
    ram.W16(kBlock + 164, 0x0304); // part_run
    ExocetBlockState s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_EQ(s.reseat, 0);
    EXPECT_EQ(s.musicAddress, 0u);
    EXPECT_EQ(s.memmapAddress, 0u);
    EXPECT_EQ(s.partRun, 0);
    EXPECT_TRUE(s.owners.empty());
}

TEST(ExocetBlockTest, TheMusicAndTheMemoryMapWhenFlagged)
{
    FakeRam ram;
    WriteBlock(ram);
    ram.W16(kBlock + 172, 0x0003); // music, memmap
    ram.W16(kBlock + 144, 7);
    ram.W16(kBlock + 146, 6);
    ram.W16(kBlock + 148, 812);
    ram.W16(kBlock + 150, 2);
    ram.W32(kBlock + 152, 0x2000);
    ram.W32(kBlock + 156, 104122);
    ram.W32(kBlock + 160, 0x3000);
    ExocetBlockState s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_EQ(s.reseat, 7);
    EXPECT_EQ(s.reseatSeen, 6);
    EXPECT_EQ(s.reseatRow, 812);
    EXPECT_EQ(s.reseatError, 2);
    EXPECT_EQ(s.musicAddress, 0x2000u);
    EXPECT_EQ(s.musicBytes, 104122u);
    EXPECT_EQ(s.memmapAddress, 0x3000u);
}

TEST(ExocetBlockTest, TheParts)
{
    FakeRam ram;
    WriteBlock(ram);
    ram.W16(kBlock + 172, 0x0004);
    ram.W16(kBlock + 164, 0x0304); // part 3, running
    ram.W16(kBlock + 166, 0x0501); // part 5, its blocks being read
    ram.W32(kBlock + 168, 0x3000);
    const char names[] = "logo\0credits\0anim\0";
    for (uint32_t i = 0; i < sizeof names; ++i)
        ram.W8(0x3000 + i, static_cast<uint8_t>(names[i]));
    ExocetBlockState s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_EQ(s.partRun, 0x0304);
    EXPECT_EQ(s.partLoad, 0x0501);
    ASSERT_EQ(s.partNames.size(), 3u);
    EXPECT_EQ(s.partNames[0], "logo");
    EXPECT_EQ(s.partNames[2], "anim");
}

TEST(ExocetBlockTest, ThePartsWithoutANameTableIsStillABlock)
{
    FakeRam ram;
    WriteBlock(ram);
    ram.W16(kBlock + 172, 0x0004);
    ram.W32(kBlock + 168, 0); // no table
    ExocetBlockState s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_TRUE(s.partNames.empty());
}

// Another demo's programs: the names and colours are the block's, not Peekaboo's
TEST(ExocetBlockTest, TheOwnersAreTheDemos)
{
    FakeRam ram;
    WriteBlock(ram);
    ram.W16(kBlock + 172, 0x0008);
    ram.W16(kBlock + 174, 60);
    ram.W32(kBlock + 176, 0x3000);
    const char* names[] = {"boot", "tunnel"};
    const uint8_t letters[] = {'B', 'X'};
    for (int i = 0; i < 2; ++i) {
        const uint32_t e = 0x3000 + 8 * i;
        ram.W8(e, letters[i]);
        ram.W8(e + 1, static_cast<uint8_t>(10 * (i + 1)));
        ram.W8(e + 2, 200);
        ram.W8(e + 3, 33);
        ram.W32(e + 4, 0x3101 + 16 * i); // odd: a name may be anywhere
        for (uint32_t k = 0; names[i][k]; ++k)
            ram.W8(0x3101 + 16 * i + k, static_cast<uint8_t>(names[i][k]));
    }
    ram.W8(0x3010, 0); // the end
    ExocetBlockState s;
    ASSERT_TRUE(Read(ram, s));
    EXPECT_EQ(s.fps, 60);
    ASSERT_EQ(s.owners.size(), 2u);
    EXPECT_EQ(s.owners[0].owner, 'B');
    EXPECT_EQ(s.owners[0].name, "boot");
    EXPECT_EQ(s.owners[1].name, "tunnel");
    EXPECT_EQ(s.owners[1].r, 20);
    EXPECT_EQ(s.owners[1].g, 200);
    ASSERT_NE(s.Owner('X'), nullptr);
    EXPECT_EQ(s.Owner('X')->name, "tunnel");
    EXPECT_EQ(s.Owner('P'), nullptr); // Sneezeman's player is not this demo's
}

TEST(ExocetBlockTest, RefusesAFiredEventOnATrackThatDoesNotExist)
{
    FakeRam ram;
    WriteBlock(ram, 1);
    ram.W16(kBlock + 42, kTracks);
    ExocetBlockState s;
    EXPECT_FALSE(Read(ram, s));
}

TEST(ExocetBlockTest, RefusesABlockAddressOutsideRam)
{
    FakeRam ram;
    WriteBlock(ram);
    ExocetBlockState s;
    EXPECT_FALSE(ReadExocetBlock(ram.Reader(), ram.Banks(), 0x100000, s));
    EXPECT_FALSE(ReadExocetBlock(ram.Reader(), ram.Banks(), 0, s));
    EXPECT_FALSE(ReadExocetBlock(ram.Reader(), ram.Banks(), kBlock + 1, s));
}

// A disk that ships registers nothing: the block is found by its magic. The magic is also an
// immediate in the code that writes it (a MOVE.L #'EXOB' ahead of other code), which must not
// pass for a block.
uint32_t Find(const FakeRam& ram, ExocetBlockState& s)
{
    const std::vector<uint8_t> chip = ram.Bytes(kChipBase, kChipSize);
    const std::vector<uint8_t> fast = ram.Bytes(kFastBase, kFastSize);
    return adm::core::FindExocetBlock(
        ram.Reader(), ram.Banks(),
        {{kChipBase, kChipSize, chip.data()}, {kFastBase, kFastSize, fast.data()}}, s);
}

TEST(ExocetBlockTest, FindsAnUnregisteredBlockPastTheCodeThatWritesIt)
{
    FakeRam ram;
    WriteBlock(ram, 3);
    ram.W16(kBlock - 0x100, 0x20BC);    // MOVE.L #imm,(A0)
    ram.W32(kBlock - 0xFE, 0x45584F42); // "EXOB"
    ram.W32(kBlock - 0xFA, 0x317C0002); // MOVE.W #2,... - not a block
    ExocetBlockState s;
    EXPECT_EQ(Find(ram, s), kBlock);
    EXPECT_TRUE(s.present);
    EXPECT_EQ(s.row, 1340);
    EXPECT_TRUE(s.arena.valid);
}

TEST(ExocetBlockTest, AnUnregisteredBlockNeedsAnExocetBinInItsArena)
{
    // registered, a block with no bin yet is still a block; found by a scan it is not: stale
    // bytes in RAM that happen to validate are far likelier than a program between its
    // publishing the block and its reading the bin
    FakeRam ram;
    WriteBlock(ram);
    ram.W32(kArena + 0, 0);
    ExocetBlockState s;
    EXPECT_EQ(Find(ram, s), 0u);
    EXPECT_FALSE(s.present);
}

// EXO-10: names are bytes in the demo's charset; JSON needs UTF-8. Non-printable bytes are '.'.
TEST(ExocetBlockTest, NamesFromRamAreMadePrintable)
{
    FakeRam ram;
    WriteBlock(ram);
    ram.W16(kBlock + 172, 0x0004 | 0x0008);
    ram.W32(kBlock + 168, 0x3000);
    const uint8_t part[] = {'F', 0xF6, 'r', 's', 0xF6, 'k', 0, 0};
    for (uint32_t i = 0; i < sizeof part; ++i)
        ram.W8(0x3000 + i, part[i]);
    ram.W32(kBlock + 176, 0x3100);
    ram.W8(0x3100, 0x81); // an owner letter that is no character
    ram.W32(0x3104, 0x3200);
    const uint8_t owner[] = {'c', 0xE9, 'l', 0};
    for (uint32_t i = 0; i < sizeof owner; ++i)
        ram.W8(0x3200 + i, owner[i]);
    ram.W8(0x3108, 0); // the end
    ExocetBlockState s;
    ASSERT_TRUE(Read(ram, s));
    ASSERT_EQ(s.partNames.size(), 1u);
    EXPECT_EQ(s.partNames[0], "F.rs.k");
    ASSERT_EQ(s.owners.size(), 1u);
    EXPECT_EQ(s.owners[0].owner, '.');
    EXPECT_EQ(s.owners[0].name, "c.l");
    const nlohmann::json j = {{"part", s.partNames[0]}, {"owner", s.owners[0].name}};
    EXPECT_NO_THROW((void)j.dump());
}
