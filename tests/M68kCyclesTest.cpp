#include "core/M68kCycles.h"

#include <gtest/gtest.h>

using adm::core::EstimateM68000Cycles;

namespace {

// Helper: estimate from a list of instruction words.
int Cyc(std::initializer_list<uint16_t> words, bool taken = false, const uint32_t* dregs = nullptr)
{
    return EstimateM68000Cycles(words.begin(), (int)words.size(), taken, dregs);
}

} // namespace

// Values cross-checked against the M68000 Programmer's Reference Manual and against
// real measured trace deltas (1 CCK == 2 CPU cycles).

TEST(M68kCycles, MoveRegToReg)
{
    // MOVE.L a3,d5  -> 2A0B ; 4 cycles (measured 2 CCK)
    EXPECT_EQ(Cyc({0x2A0B}), 4);
    // MOVE.L a5,d0  -> 200D
    EXPECT_EQ(Cyc({0x200D}), 4);
}

TEST(M68kCycles, MoveAbsLong)
{
    // MOVE.L $00044fe4,d0 -> 2039 0004 4FE4 ; 4 + 16 = 20 (measured 10 CCK)
    EXPECT_EQ(Cyc({0x2039, 0x0004, 0x4FE4}), 20);
    // MOVE.L d0,$00dff050 -> 23C0 00DF F050 ; 4 + 0 + 16 = 20
    EXPECT_EQ(Cyc({0x23C0, 0x00DF, 0xF050}), 20);
    // MOVE.W #$1081,$00dff058 -> 33FC 1081 00DF F058 ; 4 + 4 + 12 = 20
    EXPECT_EQ(Cyc({0x33FC, 0x1081, 0x00DF, 0xF058}), 20);
}

TEST(M68kCycles, AddRegSources)
{
    // ADD.L a1,d0 -> D089 ; An source, long = 8 (measured 4 CCK)
    EXPECT_EQ(Cyc({0xD089}), 8);
}

TEST(M68kCycles, AddiLong)
{
    // ADD.L #$00000198,d1 (ADDI.L #,Dn) -> 0681 0000 0198 ; 16 (the famously stalled one)
    EXPECT_EQ(Cyc({0x0681, 0x0000, 0x0198}), 16);
    // ADD.L #$000000e4,d5 -> 0685 0000 00E4 ; 16
    EXPECT_EQ(Cyc({0x0685, 0x0000, 0x00E4}), 16);
}

TEST(M68kCycles, ShiftImmediate)
{
    // LSL.W #8,d6 -> E14E ; 6 + 2*8 = 22 (measured 11 CCK)
    EXPECT_EQ(Cyc({0xE14E}), 22);
    // ASL.W #1,d0 -> E340 ; 6 + 2*1 = 8
    EXPECT_EQ(Cyc({0xE340}), 8);
}

TEST(M68kCycles, ShiftDynamic)
{
    uint32_t dregs[8] = {0};
    dregs[2] = 5; // d2 = 5
    // LSL.L d2,d3 -> E5AB ; 8 + 2*5 = 18
    EXPECT_EQ(Cyc({0xE5AB}, false, dregs), 18);
    // Without registers, dynamic count is unknown.
    EXPECT_EQ(Cyc({0xE5AB}), -1);
}

TEST(M68kCycles, ControlFlow)
{
    EXPECT_EQ(Cyc({0x4E75}), 16);                 // RTS
    EXPECT_EQ(Cyc({0x4E71}), 4);                  // NOP
    EXPECT_EQ(Cyc({0x4EF9, 0x00FC, 0x5A58}), 12); // JMP $00fc5a58 (xxx).L
    EXPECT_EQ(Cyc({0x4EAE, 0xFF1C}), 18);         // JSR (d16,a6)
}

TEST(M68kCycles, Lea)
{
    // LEA (a2,$0010),a1 -> 43EA 0010 ; (d16,An) = 8 (measured 4 CCK)
    EXPECT_EQ(Cyc({0x43EA, 0x0010}), 8);
    // LEA (a2,$0020),a6 -> 4DEA 0020 ; 8
    EXPECT_EQ(Cyc({0x4DEA, 0x0020}), 8);
}

TEST(M68kCycles, BtstImmediateToMemory)
{
    // BTST.B #$0006,$00dff002 -> 0839 0006 00DF F002 ; 8 + 12 (abs.l byte) = 20 (10 CCK)
    EXPECT_EQ(Cyc({0x0839, 0x0006, 0x00DF, 0xF002}), 20);
}

TEST(M68kCycles, BranchesTakenAndNot)
{
    // BNE.B #$02 -> 6602 ; taken = 10, not taken (byte) = 8
    EXPECT_EQ(Cyc({0x6602}, /*taken=*/true), 10);
    EXPECT_EQ(Cyc({0x6602}, /*taken=*/false), 8);
    // BSR always 18, BRA always 10.
    EXPECT_EQ(Cyc({0x6100, 0x0040}), 18); // BSR.W
    EXPECT_EQ(Cyc({0x6040}), 10);         // BRA.B
}

TEST(M68kCycles, MulDivWorstCase)
{
    // MULU.W d3,d1 -> C2C3 ; documented max 70
    EXPECT_EQ(Cyc({0xC2C3}), 70);
}

TEST(M68kCycles, Moveq)
{
    EXPECT_EQ(Cyc({0x7000}), 4); // MOVEQ #0,d0
}

TEST(M68kCycles, UnmodeledReturnsNegative)
{
    EXPECT_EQ(Cyc({0xA000}), -1); // line-A
    EXPECT_EQ(EstimateM68000Cycles(nullptr, 0, false, nullptr), -1);
}
