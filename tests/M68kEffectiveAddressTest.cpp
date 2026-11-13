#include <gtest/gtest.h>

#include <vector>

#include "../src/core/M68kEffectiveAddress.h"

using adm::core::M68kAccess;
using adm::core::M68kEffectiveAddress;
using adm::core::M68kRegisters;

namespace {

// Assembles a big-endian instruction from its words.
std::vector<uint8_t> Words(std::initializer_list<uint16_t> words)
{
    std::vector<uint8_t> out;
    for (uint16_t w : words) {
        out.push_back(static_cast<uint8_t>(w >> 8));
        out.push_back(static_cast<uint8_t>(w));
    }
    return out;
}

std::vector<M68kAccess> Decode(std::initializer_list<uint16_t> words, const M68kRegisters& regs,
                               uint32_t pc = 0x1000)
{
    auto raw = Words(words);
    return M68kEffectiveAddress::Decode(raw.data(), raw.size(), pc, regs);
}

M68kRegisters MakeRegs()
{
    M68kRegisters r;
    r.a[0] = 0x00020000;
    r.a[1] = 0x00030000;
    r.a[7] = 0x00040000;
    r.d[0] = 0x00000010;
    r.d[1] = 0xFFFF8000; // negative when treated as a word index
    return r;
}

} // namespace

TEST(M68kEffectiveAddressTest, AbsoluteLongDestinationIsAWrite)
{
    // MOVE.W D0,$00012340
    auto acc = Decode({0x33C0, 0x0001, 0x2340}, MakeRegs());
    ASSERT_EQ(acc.size(), 1u);
    EXPECT_EQ(acc[0].addr, 0x00012340u);
    EXPECT_EQ(acc[0].size, 2);
    EXPECT_TRUE(acc[0].write);
    EXPECT_FALSE(acc[0].read);
}

TEST(M68kEffectiveAddressTest, AbsoluteShortSignExtends)
{
    // MOVE.B $FF00.W,D0 — the short form sign-extends into the top of memory.
    auto acc = Decode({0x1038, 0xFF00}, MakeRegs());
    ASSERT_EQ(acc.size(), 1u);
    EXPECT_EQ(acc[0].addr, 0xFFFFFF00u);
    EXPECT_TRUE(acc[0].read);
}

TEST(M68kEffectiveAddressTest, RegisterIndirectModes)
{
    auto regs = MakeRegs();

    // MOVE.L (A0),D0
    auto ind = Decode({0x2010}, regs);
    ASSERT_EQ(ind.size(), 1u);
    EXPECT_EQ(ind[0].addr, 0x00020000u);
    EXPECT_EQ(ind[0].size, 4);

    // MOVE.W (A0)+,D0 — the captured A0 is the address actually used.
    auto post = Decode({0x3018}, regs);
    ASSERT_EQ(post.size(), 1u);
    EXPECT_EQ(post[0].addr, 0x00020000u);

    // MOVE.W D0,-(A0) — the decrement happens before the write.
    auto pre = Decode({0x3100}, regs);
    ASSERT_EQ(pre.size(), 1u);
    EXPECT_EQ(pre[0].addr, 0x0001FFFEu);
    EXPECT_TRUE(pre[0].write);
}

TEST(M68kEffectiveAddressTest, BytePushOntoTheStackStillMovesTwo)
{
    auto regs = MakeRegs();
    // MOVE.B D0,-(A7): A7 must stay even, so the byte lands at A7-2.
    auto acc = Decode({0x1F00}, regs);
    ASSERT_EQ(acc.size(), 1u);
    EXPECT_EQ(acc[0].addr, 0x0003FFFEu);
    EXPECT_EQ(acc[0].size, 1);
}

TEST(M68kEffectiveAddressTest, DisplacementAndIndexedModes)
{
    auto regs = MakeRegs();

    // MOVE.W $0010(A0),D0
    auto disp = Decode({0x3028, 0x0010}, regs);
    ASSERT_EQ(disp.size(), 1u);
    EXPECT_EQ(disp[0].addr, 0x00020010u);

    // MOVE.W $FFF0(A0),D0 — negative displacement
    auto negDisp = Decode({0x3028, 0xFFF0}, regs);
    ASSERT_EQ(negDisp.size(), 1u);
    EXPECT_EQ(negDisp[0].addr, 0x0001FFF0u);

    // MOVE.W $08(A0,D0.W),D0 — D0 is 0x10
    auto indexed = Decode({0x3030, 0x0008}, regs);
    ASSERT_EQ(indexed.size(), 1u);
    EXPECT_EQ(indexed[0].addr, 0x00020018u);

    // MOVE.W $00(A0,D1.W),D0 — D1's low word is negative and sign-extends.
    auto negIndex = Decode({0x3030, 0x1000}, regs);
    ASSERT_EQ(negIndex.size(), 1u);
    EXPECT_EQ(negIndex[0].addr, 0x00020000u + 0xFFFF8000u);

    // MOVE.W $00(A0,D1.L),D0 — the long index uses all 32 bits.
    auto longIndex = Decode({0x3030, 0x1800}, regs);
    ASSERT_EQ(longIndex.size(), 1u);
    EXPECT_EQ(longIndex[0].addr, 0x00020000u + 0xFFFF8000u);
}

TEST(M68kEffectiveAddressTest, PcRelativeModesUseTheExtensionWordAddress)
{
    auto regs = MakeRegs();

    // MOVE.W $10(PC),D0 at $1000 — the base is the extension word at $1002.
    auto pcDisp = Decode({0x303A, 0x0010}, regs, 0x1000);
    ASSERT_EQ(pcDisp.size(), 1u);
    EXPECT_EQ(pcDisp[0].addr, 0x1012u);

    // MOVE.W $10(PC,D0.W),D0 — plus D0 (0x10).
    auto pcIndexed = Decode({0x303B, 0x0010}, regs, 0x1000);
    ASSERT_EQ(pcIndexed.size(), 1u);
    EXPECT_EQ(pcIndexed[0].addr, 0x1022u);
}

TEST(M68kEffectiveAddressTest, MoveWithTwoMemoryOperandsReportsBoth)
{
    auto regs = MakeRegs();
    // MOVE.L (A0),(A1) — source read then destination write, in that order.
    auto acc = Decode({0x22D0}, regs);
    ASSERT_EQ(acc.size(), 2u);
    EXPECT_EQ(acc[0].addr, 0x00020000u);
    EXPECT_TRUE(acc[0].read);
    EXPECT_EQ(acc[1].addr, 0x00030000u);
    EXPECT_TRUE(acc[1].write);
}

TEST(M68kEffectiveAddressTest, MoveExtensionWordsAreConsumedSourceFirst)
{
    auto regs = MakeRegs();
    // MOVE.W $1111.W,$2222.W — if the two extension words were read in the wrong
    // order the addresses would swap.
    auto acc = Decode({0x31F8, 0x1111, 0x2222}, regs);
    ASSERT_EQ(acc.size(), 2u);
    EXPECT_EQ(acc[0].addr, 0x00001111u);
    EXPECT_TRUE(acc[0].read);
    EXPECT_EQ(acc[1].addr, 0x00002222u);
    EXPECT_TRUE(acc[1].write);
}

TEST(M68kEffectiveAddressTest, ImmediateOperandsAreSkippedBeforeTheEa)
{
    auto regs = MakeRegs();

    // ADDI.L #$12345678,$00001000 — long immediate, then the absolute address.
    auto addi = Decode({0x06B9, 0x1234, 0x5678, 0x0000, 0x1000}, regs);
    ASSERT_EQ(addi.size(), 1u);
    EXPECT_EQ(addi[0].addr, 0x00001000u);
    EXPECT_EQ(addi[0].size, 4);
    EXPECT_TRUE(addi[0].read);
    EXPECT_TRUE(addi[0].write);

    // CMPI.W #$0001,$00001000 — a comparison never writes.
    auto cmpi = Decode({0x0C79, 0x0001, 0x0000, 0x1000}, regs);
    ASSERT_EQ(cmpi.size(), 1u);
    EXPECT_TRUE(cmpi[0].read);
    EXPECT_FALSE(cmpi[0].write);
}

TEST(M68kEffectiveAddressTest, ClearIsAWriteAndTestIsARead)
{
    auto regs = MakeRegs();

    auto clr = Decode({0x4279, 0x0000, 0x1000}, regs); // CLR.W $00001000
    ASSERT_EQ(clr.size(), 1u);
    EXPECT_TRUE(clr[0].write);
    EXPECT_FALSE(clr[0].read);

    auto tst = Decode({0x4A79, 0x0000, 0x1000}, regs); // TST.W $00001000
    ASSERT_EQ(tst.size(), 1u);
    EXPECT_TRUE(tst[0].read);
    EXPECT_FALSE(tst[0].write);

    auto neg = Decode({0x4479, 0x0000, 0x1000}, regs); // NEG.W $00001000
    ASSERT_EQ(neg.size(), 1u);
    EXPECT_TRUE(neg[0].read);
    EXPECT_TRUE(neg[0].write);
}

TEST(M68kEffectiveAddressTest, BitOperationsOnMemoryAreByteSized)
{
    auto regs = MakeRegs();

    // BSET #7,(A0)
    auto bset = Decode({0x08D0, 0x0007}, regs);
    ASSERT_EQ(bset.size(), 1u);
    EXPECT_EQ(bset[0].addr, 0x00020000u);
    EXPECT_EQ(bset[0].size, 1);
    EXPECT_TRUE(bset[0].write);

    // BTST #7,(A0) — reads only.
    auto btst = Decode({0x0810, 0x0007}, regs);
    ASSERT_EQ(btst.size(), 1u);
    EXPECT_TRUE(btst[0].read);
    EXPECT_FALSE(btst[0].write);

    // BSET D0,(A0) — dynamic form, no immediate word.
    auto dynamic = Decode({0x01D0}, regs);
    ASSERT_EQ(dynamic.size(), 1u);
    EXPECT_EQ(dynamic[0].addr, 0x00020000u);
    EXPECT_TRUE(dynamic[0].write);
}

TEST(M68kEffectiveAddressTest, AluMemoryDestinationsReadAndWrite)
{
    auto regs = MakeRegs();

    // ADD.W D0,(A0) — opmode 5, memory destination.
    auto add = Decode({0xD150}, regs);
    ASSERT_EQ(add.size(), 1u);
    EXPECT_EQ(add[0].addr, 0x00020000u);
    EXPECT_TRUE(add[0].read);
    EXPECT_TRUE(add[0].write);

    // ADD.W (A0),D0 — opmode 1, register destination, so the memory is read.
    auto addToReg = Decode({0xD050}, regs);
    ASSERT_EQ(addToReg.size(), 1u);
    EXPECT_TRUE(addToReg[0].read);
    EXPECT_FALSE(addToReg[0].write);

    // ADDQ.W #1,(A0)
    auto addq = Decode({0x5250}, regs);
    ASSERT_EQ(addq.size(), 1u);
    EXPECT_TRUE(addq[0].read);
    EXPECT_TRUE(addq[0].write);
}

TEST(M68kEffectiveAddressTest, MovemEnumeratesEverySlot)
{
    auto regs = MakeRegs();

    // MOVEM.L D0/D1,(A0) — two long slots written from A0 upward.
    auto store = Decode({0x48D0, 0x0003}, regs);
    ASSERT_EQ(store.size(), 2u);
    EXPECT_EQ(store[0].addr, 0x00020000u);
    EXPECT_EQ(store[1].addr, 0x00020004u);
    EXPECT_TRUE(store[0].write);

    // MOVEM.L (A0)+,D0/D1 — the same slots, read back.
    auto load = Decode({0x4CD8, 0x0003}, regs);
    ASSERT_EQ(load.size(), 2u);
    EXPECT_TRUE(load[0].read);
    EXPECT_FALSE(load[0].write);

    // MOVEM.L D0/D1,-(A7) — the block ends at the original A7 and grows down.
    auto push = Decode({0x48E7, 0xC000}, regs);
    ASSERT_EQ(push.size(), 2u);
    EXPECT_EQ(push[0].addr, 0x0003FFF8u);
    EXPECT_EQ(push[1].addr, 0x0003FFFCu);
}

TEST(M68kEffectiveAddressTest, SubroutineCallsRecordTheStackWrite)
{
    auto regs = MakeRegs();

    // JSR $00001000 — the return address goes to A7-4.
    auto jsr = Decode({0x4EB9, 0x0000, 0x1000}, regs);
    ASSERT_EQ(jsr.size(), 1u);
    EXPECT_EQ(jsr[0].addr, 0x0003FFFCu);
    EXPECT_EQ(jsr[0].size, 4);
    EXPECT_TRUE(jsr[0].write);

    // BSR.W does the same.
    auto bsr = Decode({0x6100, 0x0010}, regs);
    ASSERT_EQ(bsr.size(), 1u);
    EXPECT_EQ(bsr[0].addr, 0x0003FFFCu);

    // JMP transfers control without touching data.
    EXPECT_TRUE(Decode({0x4EF9, 0x0000, 0x1000}, regs).empty());
    // LEA computes an address but reads nothing.
    EXPECT_TRUE(Decode({0x41F9, 0x0000, 0x1000}, regs).empty());
    // Bcc is not a call.
    EXPECT_TRUE(Decode({0x6600, 0x0010}, regs).empty());
}

TEST(M68kEffectiveAddressTest, PeaPushesWithoutReadingItsOperand)
{
    auto regs = MakeRegs();
    auto pea = Decode({0x4879, 0x0000, 0x1000}, regs); // PEA $00001000
    ASSERT_EQ(pea.size(), 1u);
    EXPECT_EQ(pea[0].addr, 0x0003FFFCu);
    EXPECT_TRUE(pea[0].write);
}

TEST(M68kEffectiveAddressTest, MemoryShiftsReadAndWriteAWord)
{
    auto regs = MakeRegs();
    auto lsl = Decode({0xE3D0}, regs); // LSL.W (A0)
    ASSERT_EQ(lsl.size(), 1u);
    EXPECT_EQ(lsl[0].addr, 0x00020000u);
    EXPECT_EQ(lsl[0].size, 2);
    EXPECT_TRUE(lsl[0].read);
    EXPECT_TRUE(lsl[0].write);

    // The register form has no memory operand.
    EXPECT_TRUE(Decode({0xE348}, regs).empty());
}

TEST(M68kEffectiveAddressTest, RegisterOnlyInstructionsReportNothing)
{
    auto regs = MakeRegs();
    EXPECT_TRUE(Decode({0x7001}, regs).empty()); // MOVEQ #1,D0
    EXPECT_TRUE(Decode({0x4E71}, regs).empty()); // NOP
    EXPECT_TRUE(Decode({0x3001}, regs).empty()); // MOVE.W D1,D0
    EXPECT_TRUE(Decode({0xD041}, regs).empty()); // ADD.W D1,D0
}

TEST(M68kEffectiveAddressTest, TruncatedInstructionsYieldNothingRatherThanGuesses)
{
    auto regs = MakeRegs();
    // MOVE.W D0,$xxxx with the extension word missing.
    std::vector<uint8_t> raw = {0x31, 0xC0};
    EXPECT_TRUE(M68kEffectiveAddress::Decode(raw.data(), raw.size(), 0x1000, regs).empty());
    EXPECT_TRUE(M68kEffectiveAddress::Decode(nullptr, 0, 0x1000, regs).empty());
    EXPECT_TRUE(M68kEffectiveAddress::Decode(raw.data(), 1, 0x1000, regs).empty());
}

TEST(M68kEffectiveAddressTest, TouchesReportsRangeOverlap)
{
    M68kAccess acc{0x1000, 4, false, true};
    EXPECT_TRUE(acc.Touches(0x1000, 0x1001));
    EXPECT_TRUE(acc.Touches(0x1003, 0x1004)); // last byte of the longword
    EXPECT_FALSE(acc.Touches(0x1004, 0x1008));
    EXPECT_FALSE(acc.Touches(0x0FFC, 0x1000));
}
