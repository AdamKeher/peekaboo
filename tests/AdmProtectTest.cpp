// Tests for the memory-protection table that scripts/build_puae.{ps1,sh} inject into
// memory.c (patches/adm_protect.inc).
//
// That code normally only exists inside the PuAE DLL, so a bug in it would otherwise cost a
// full core rebuild to find. Compiling it here against stand-ins for the three UAE typedefs it
// touches lets the part that is easy to get wrong — the big-endian byte splicing that lets a
// forced byte survive a wider write that straddles it — be tested directly, with no emulator
// and no rebuild.
//
// If these fail after a PuAE bump, the algorithm is wrong. If these pass but a lock still has
// no effect in the running machine, the problem is in the hook sites (which anchors the build
// scripts patch into memory.c), not in here.

#include <gtest/gtest.h>

#include <cstdint>

// ---- stand-ins for the UAE declarations adm_protect.inc relies on -------------------------
namespace admprotect_test {
typedef uint32_t uae_u32;
typedef uint8_t uae_u8;
typedef uint32_t uaecptr;
} // namespace admprotect_test

using admprotect_test::uae_u32;
using admprotect_test::uae_u8;
using admprotect_test::uaecptr;

#include "../patches/adm_protect.inc" // NOLINT — deliberately compiled into the test

namespace {

// Convenience: build a one-entry table and run a check against it.
struct Check
{
    uae_u32 value;
    bool blocked;
};

adm_protect_entry_t Block(uae_u32 addr, uae_u32 len)
{
    adm_protect_entry_t e{};
    e.addr = addr;
    e.len = len;
    e.mode = ADM_PROTECT_MODE_BLOCK;
    e.size = 1;
    e.value = 0;
    return e;
}

adm_protect_entry_t Set(uae_u32 addr, uae_u8 size, uae_u32 value)
{
    adm_protect_entry_t e{};
    e.addr = addr;
    e.len = size;
    e.mode = ADM_PROTECT_MODE_SET;
    e.size = size;
    e.value = value;
    return e;
}

Check TryWrite(uaecptr addr, int width, uae_u32 v)
{
    Check c{v, false};
    c.blocked = adm_protect_check(addr, width, &c.value) != 0;
    return c;
}

class AdmProtect : public ::testing::Test
{
protected:
    void TearDown() override { adm_protect_clear(); }
};

TEST_F(AdmProtect, NoEntriesLeavesTheWriteAlone)
{
    adm_protect_clear();
    const Check c = TryWrite(0x1000, 4, 0x11223344);
    EXPECT_FALSE(c.blocked);
    EXPECT_EQ(c.value, 0x11223344u);
    // The hot-path short-circuit in memory_put_* keys off this being zero.
    EXPECT_EQ(adm_protect_count, 0);
}

TEST_F(AdmProtect, BlockStopsAWriteAtTheProtectedByte)
{
    adm_protect_entry_t e = Block(0x1000, 1);
    adm_protect_set(&e, 1);

    EXPECT_TRUE(TryWrite(0x1000, 1, 0x55).blocked);
    EXPECT_FALSE(TryWrite(0x1001, 1, 0x55).blocked) << "neighbouring byte must be untouched";
}

TEST_F(AdmProtect, BlockStopsAWiderWriteThatStraddlesTheProtectedByte)
{
    // A long write covering $0FFE..$1001 overlaps the protected byte at $1000, so the whole
    // access is dropped. Partially applying it would corrupt the three bytes either side.
    adm_protect_entry_t e = Block(0x1000, 1);
    adm_protect_set(&e, 1);

    EXPECT_TRUE(TryWrite(0x0FFE, 4, 0xAABBCCDD).blocked);
}

TEST_F(AdmProtect, BlockCoversTheWholeRangeAndNothingOutsideIt)
{
    adm_protect_entry_t e = Block(0x2000, 16);
    adm_protect_set(&e, 1);

    EXPECT_FALSE(TryWrite(0x1FFF, 1, 1).blocked) << "byte before the range";
    EXPECT_TRUE(TryWrite(0x2000, 1, 1).blocked) << "first byte of the range";
    EXPECT_TRUE(TryWrite(0x200F, 1, 1).blocked) << "last byte of the range";
    EXPECT_FALSE(TryWrite(0x2010, 1, 1).blocked) << "byte after the range";
}

TEST_F(AdmProtect, SetRewritesRatherThanBlocks)
{
    adm_protect_entry_t e = Set(0x3000, 1, 0x63);
    adm_protect_set(&e, 1);

    const Check c = TryWrite(0x3000, 1, 0x00);
    EXPECT_FALSE(c.blocked) << "a forced value must let the write through, rewritten";
    EXPECT_EQ(c.value, 0x63u);
}

TEST_F(AdmProtect, SetSplicesIntoAWiderWriteAtTheRightByteLane)
{
    // The Amiga is big-endian: for a width-byte access at addr, the byte at address b sits at
    // bit ((width - 1) - (b - addr)) * 8. Getting this backwards is the easy mistake, so pin
    // every lane.
    adm_protect_entry_t e = Set(0x3000, 1, 0x63);
    adm_protect_set(&e, 1);

    EXPECT_EQ(TryWrite(0x3000, 2, 0x1122).value, 0x6322u) << "forced byte is the high half";
    EXPECT_EQ(TryWrite(0x2FFF, 2, 0x1122).value, 0x1163u) << "forced byte is the low half";
    EXPECT_EQ(TryWrite(0x2FFE, 4, 0xAABBCCDD).value, 0xAABB63DDu) << "forced byte mid-long";
    EXPECT_EQ(TryWrite(0x3000, 4, 0xAABBCCDD).value, 0x63BBCCDDu) << "forced byte tops the long";
}

TEST_F(AdmProtect, SetOfAWordForcesBothBytesIndividually)
{
    adm_protect_entry_t e = Set(0x4000, 2, 0x270F); // 9999
    adm_protect_set(&e, 1);

    EXPECT_EQ(TryWrite(0x4000, 2, 0x0000).value, 0x270Fu);
    EXPECT_EQ(TryWrite(0x4000, 1, 0x00).value, 0x27u) << "byte write to the high half";
    EXPECT_EQ(TryWrite(0x4001, 1, 0x00).value, 0x0Fu) << "byte write to the low half";
    EXPECT_EQ(TryWrite(0x4000, 4, 0xAABBCCDD).value, 0x270FCCDDu) << "long write keeps the tail";
}

TEST_F(AdmProtect, SetOfALongForcesAllFourBytes)
{
    adm_protect_entry_t e = Set(0x5000, 4, 0xDEADBEEF);
    adm_protect_set(&e, 1);

    EXPECT_EQ(TryWrite(0x5000, 4, 0x00000000).value, 0xDEADBEEFu);
    EXPECT_EQ(TryWrite(0x5002, 2, 0x0000).value, 0xBEEFu) << "word write to the bottom half";
}

TEST_F(AdmProtect, BlockWinsWhenABlockAndASetOverlap)
{
    // Order in the table must not decide the outcome: a blocked byte stays blocked even if a
    // Set entry for the same address is scanned first.
    adm_protect_entry_t e[2] = {Set(0x6000, 1, 0x99), Block(0x6000, 1)};
    adm_protect_set(e, 2);
    EXPECT_TRUE(TryWrite(0x6000, 1, 0x11).blocked);

    adm_protect_entry_t swapped[2] = {Block(0x6000, 1), Set(0x6000, 1, 0x99)};
    adm_protect_set(swapped, 2);
    EXPECT_TRUE(TryWrite(0x6000, 1, 0x11).blocked);
}

TEST_F(AdmProtect, ClearRemovesEverything)
{
    adm_protect_entry_t e = Block(0x6000, 1);
    adm_protect_set(&e, 1);
    ASSERT_TRUE(TryWrite(0x6000, 1, 0x11).blocked);

    adm_protect_clear();
    EXPECT_EQ(adm_protect_count, 0);
    EXPECT_FALSE(TryWrite(0x6000, 1, 0x11).blocked);
}

TEST_F(AdmProtect, SetClampsToTheTableCapacityRatherThanOverrunning)
{
    static adm_protect_entry_t big[ADM_PROTECT_MAX + 8];
    for (auto& e : big)
        e = Block(0x7000, 1);

    adm_protect_set(big, ADM_PROTECT_MAX + 8);
    EXPECT_EQ(adm_protect_count, ADM_PROTECT_MAX);
}

TEST_F(AdmProtect, SetWithNoEntriesDisarmsTheTable)
{
    adm_protect_entry_t e = Block(0x8000, 1);
    adm_protect_set(&e, 1);
    ASSERT_EQ(adm_protect_count, 1);

    adm_protect_set(nullptr, 0);
    EXPECT_EQ(adm_protect_count, 0);

    adm_protect_set(&e, 1);
    adm_protect_set(&e, -1);
    EXPECT_EQ(adm_protect_count, 0) << "a negative count must not be trusted as a length";
}

// The host mirrors this struct in AmigaEmulator.h and memcpy's into it, so the layout is ABI.
TEST_F(AdmProtect, EntryLayoutMatchesTheHostMirror)
{
    EXPECT_EQ(sizeof(adm_protect_entry_t), 16u);
    EXPECT_EQ(offsetof(adm_protect_entry_t, addr), 0u);
    EXPECT_EQ(offsetof(adm_protect_entry_t, len), 4u);
    EXPECT_EQ(offsetof(adm_protect_entry_t, mode), 8u);
    EXPECT_EQ(offsetof(adm_protect_entry_t, size), 9u);
    EXPECT_EQ(offsetof(adm_protect_entry_t, value), 12u);
}

} // namespace
