#include <gtest/gtest.h>

#include "core/Disassembler.h"

#include <cstring>
#include <map>

using adm::core::Disassembler;

namespace {

// Fake backend over a small synthetic instruction map: addr -> (hex, mnemonic, length)
struct FakeIns
{
    const char* hex;
    const char* mnemo;
    uint32_t length;
};

Disassembler MakeFake(const std::map<uint32_t, FakeIns>& code)
{
    return Disassembler(
        [code](uint32_t pc, char* mnemo, int mnemoSz, char* hex, int hexSz, uint32_t& nextPc) {
            auto it = code.find(pc);
            if (it == code.end())
                return false;
            snprintf(mnemo, mnemoSz, "%s", it->second.mnemo);
            snprintf(hex, hexSz, "%s", it->second.hex);
            nextPc = pc + it->second.length;
            return true;
        });
}

const std::map<uint32_t, FakeIns> kCode = {
    {0x00FC0000, {"4EF9 00FC 00D2 ", "JMP $00FC00D2", 6}},
    {0x00FC0006, {"4E71 ", "NOP", 2}},
    {0x00FC0008, {"203C 0000 0001 ", "MOVE.L #$00000001,D0", 6}},
};

} // namespace

TEST(DisassemblerTest, DisassembleOneTrimsTrailingSpaces)
{
    auto dis = MakeFake(kCode);
    Disassembler::Line line;
    ASSERT_TRUE(dis.DisassembleOne(0x00FC0000, line));
    EXPECT_EQ(line.address, 0x00FC0000u);
    EXPECT_EQ(line.nextAddress, 0x00FC0006u);
    EXPECT_EQ(line.hex, "4EF9 00FC 00D2");
    EXPECT_EQ(line.mnemonic, "JMP $00FC00D2");
}

TEST(DisassemblerTest, DisassembleOneFailsOnUnknownAddress)
{
    auto dis = MakeFake(kCode);
    Disassembler::Line line;
    EXPECT_FALSE(dis.DisassembleOne(0x12345678, line));
}

TEST(DisassemblerTest, NoBackendFails)
{
    Disassembler dis;
    EXPECT_FALSE(dis.HasBackend());
    Disassembler::Line line;
    EXPECT_FALSE(dis.DisassembleOne(0, line));
}

TEST(DisassemblerTest, RangeWalksConsecutiveInstructions)
{
    auto dis = MakeFake(kCode);
    auto lines = dis.DisassembleRange(0x00FC0000, 10);
    ASSERT_EQ(lines.size(), 3u); // stops when backend fails past the last instruction
    EXPECT_EQ(lines[0].address, 0x00FC0000u);
    EXPECT_EQ(lines[1].address, 0x00FC0006u);
    EXPECT_EQ(lines[2].address, 0x00FC0008u);
}

TEST(DisassemblerTest, RangeHonoursMaxLines)
{
    auto dis = MakeFake(kCode);
    auto lines = dis.DisassembleRange(0x00FC0000, 2);
    EXPECT_EQ(lines.size(), 2u);
}

TEST(DisassemblerTest, RangeStopsAtEndAddress)
{
    auto dis = MakeFake(kCode);
    auto lines = dis.DisassembleRange(0x00FC0000, 10, 0x00FC0006);
    ASSERT_EQ(lines.size(), 2u); // 0xFC0008 > endAddr, excluded
    EXPECT_EQ(lines.back().address, 0x00FC0006u);
}

TEST(DisassemblerTest, RangeStopsWhenAddressDoesNotAdvance)
{
    // Backend that reports a zero-length instruction
    Disassembler dis(
        [](uint32_t pc, char* mnemo, int mnemoSz, char* hex, int hexSz, uint32_t& nextPc) {
            snprintf(mnemo, mnemoSz, "BAD");
            snprintf(hex, hexSz, "0000");
            nextPc = pc; // does not advance
            return true;
        });
    auto lines = dis.DisassembleRange(0x1000, 10);
    EXPECT_TRUE(lines.empty());
}

TEST(DisassemblerTest, FormatLineUsesFixedColumns)
{
    Disassembler::Line line;
    line.address = 0x00FC0000;
    line.hex = "4EF9 00FC 00D2";
    line.mnemonic = "JMP $00FC00D2";
    std::string text = Disassembler::FormatLine(line);

    EXPECT_EQ(text.substr(0, 10), "00FC0000: ");
    EXPECT_EQ(text.find("4EF9"), static_cast<size_t>(Disassembler::kHexColumn));
    EXPECT_EQ(text.find("JMP"), static_cast<size_t>(Disassembler::kMnemonicColumn));
}

TEST(DisassemblerTest, IsSectionEndDetectsFlowTerminators)
{
    // Returns, unconditional jumps, traps/halts — with and without operands/size suffix.
    EXPECT_TRUE(Disassembler::IsSectionEnd("RTS"));
    EXPECT_TRUE(Disassembler::IsSectionEnd("RTE"));
    EXPECT_TRUE(Disassembler::IsSectionEnd("RTR"));
    EXPECT_TRUE(Disassembler::IsSectionEnd("RTD #$8"));
    EXPECT_TRUE(Disassembler::IsSectionEnd("BRA.S $00001000"));
    EXPECT_TRUE(Disassembler::IsSectionEnd("JMP (A0)"));
    EXPECT_TRUE(Disassembler::IsSectionEnd("TRAP #$0"));
    EXPECT_TRUE(Disassembler::IsSectionEnd("TRAPV"));
    EXPECT_TRUE(Disassembler::IsSectionEnd("ILLEGAL"));
    EXPECT_TRUE(Disassembler::IsSectionEnd("STOP #$2000"));
    EXPECT_TRUE(Disassembler::IsSectionEnd("RESET"));
    EXPECT_TRUE(Disassembler::IsSectionEnd("  rts")); // leading space, lowercase
}

TEST(DisassemblerTest, IsSectionEndIgnoresContinuingInstructions)
{
    EXPECT_FALSE(Disassembler::IsSectionEnd("NOP"));
    EXPECT_FALSE(Disassembler::IsSectionEnd("MOVE.L #$1,D0"));
    EXPECT_FALSE(Disassembler::IsSectionEnd("BNE.S $00001000"));   // conditional branch
    EXPECT_FALSE(Disassembler::IsSectionEnd("DBRA D0,$00001000")); // DBcc, not BRA
    EXPECT_FALSE(Disassembler::IsSectionEnd("JSR $00FC00D2"));     // call, returns
    EXPECT_FALSE(Disassembler::IsSectionEnd("BSR.S $00001000"));
    EXPECT_FALSE(Disassembler::IsSectionEnd(""));
}

TEST(DisassemblerTest, ParseRelativeBranchResolvesTargets)
{
    uint32_t target = 0;
    bool conditional = true;

    // Word branches: target = instrAddr + 2 + displacement.
    // BSR.W #$0128 at $0000157C -> $157C + 2 + $128 = $16A6.
    EXPECT_TRUE(Disassembler::ParseRelativeBranch("BSR.W #$0128", 0x0000157C, target, conditional));
    EXPECT_EQ(target, 0x000016A6u);
    EXPECT_FALSE(conditional);

    // BNE.W #$0166 at $00001584 -> $1584 + 2 + $166 = $16EC.
    EXPECT_TRUE(Disassembler::ParseRelativeBranch("BNE.W #$0166", 0x00001584, target, conditional));
    EXPECT_EQ(target, 0x000016ECu);
    EXPECT_TRUE(conditional);

    // Backward branch (negative word displacement): #$FF00 = -256.
    // BRA.W #$FF00 at $00002000 -> $2000 + 2 - $100 = $1F02.
    EXPECT_TRUE(Disassembler::ParseRelativeBranch("BRA.W #$FF00", 0x00002000, target, conditional));
    EXPECT_EQ(target, 0x00001F02u);
    EXPECT_FALSE(conditional);

    // Short branch (8-bit displacement): #$FE = -2.
    // BNE.S #$FE at $00001000 -> $1000 + 2 - 2 = $1000 (tight backward loop).
    EXPECT_TRUE(Disassembler::ParseRelativeBranch("BNE.S #$FE", 0x00001000, target, conditional));
    EXPECT_EQ(target, 0x00001000u);
    EXPECT_TRUE(conditional);

    // DBcc: word displacement, target after the data register operand.
    // DBRA D0,#$FFFA at $00002000 -> $2000 + 2 - 6 = $1FFC.
    EXPECT_TRUE(
        Disassembler::ParseRelativeBranch("DBRA D0,#$FFFA", 0x00002000, target, conditional));
    EXPECT_EQ(target, 0x00001FFCu);
    EXPECT_TRUE(conditional);

    // HS/LO are CC/CS aliases and must resolve like any other Bcc (previously these
    // were missing from the branch table while the control-flow gutter treated them
    // as branches — the two are now driven by one shared condition-code set).
    EXPECT_TRUE(Disassembler::ParseRelativeBranch("BHS.S #$10", 0x00001000, target, conditional));
    EXPECT_EQ(target, 0x00001012u);
    EXPECT_TRUE(conditional);
    EXPECT_TRUE(Disassembler::ParseRelativeBranch("BLO.W #$0166", 0x00001584, target, conditional));
    EXPECT_EQ(target, 0x000016ECu);
    EXPECT_TRUE(conditional);
}

TEST(DisassemblerTest, LeadingOpcodeSplitsOpAndSize)
{
    EXPECT_EQ(Disassembler::LeadingOpcode("BNE.S $1000").op, "BNE");
    EXPECT_EQ(Disassembler::LeadingOpcode("BNE.S $1000").size, 'S');
    EXPECT_EQ(Disassembler::LeadingOpcode("  move.l #$1,d0").op, "MOVE");
    EXPECT_EQ(Disassembler::LeadingOpcode("  move.l #$1,d0").size, 'L');
    EXPECT_EQ(Disassembler::LeadingOpcode("rts").op, "RTS");
    EXPECT_EQ(Disassembler::LeadingOpcode("rts").size, '\0');
    EXPECT_TRUE(Disassembler::LeadingOpcode("").empty());
    EXPECT_TRUE(Disassembler::LeadingOpcode("   ").empty());
}

TEST(DisassemblerTest, ParseRelativeBranchRejectsNonRelative)
{
    uint32_t target = 0;
    bool conditional = false;
    // JMP/JSR are not PC-relative branches — excluded by design.
    EXPECT_FALSE(Disassembler::ParseRelativeBranch("JMP $00FC00D2", 0x1000, target, conditional));
    EXPECT_FALSE(Disassembler::ParseRelativeBranch("JSR $00FC00D2", 0x1000, target, conditional));
    EXPECT_FALSE(Disassembler::ParseRelativeBranch("JMP (A0)", 0x1000, target, conditional));
    // Not a branch at all.
    EXPECT_FALSE(Disassembler::ParseRelativeBranch("MOVE.L #$1,D0", 0x1000, target, conditional));
    EXPECT_FALSE(Disassembler::ParseRelativeBranch("RTS", 0x1000, target, conditional));
    EXPECT_FALSE(Disassembler::ParseRelativeBranch("", 0x1000, target, conditional));
    // Register-indirect Bcc with no displacement literal.
    EXPECT_FALSE(Disassembler::ParseRelativeBranch("BNE.S (A0)", 0x1000, target, conditional));
}

TEST(DisassemblerTest, FormatLineLongHexStillSeparated)
{
    Disassembler::Line line;
    line.address = 0x1000;
    line.hex = "AAAA BBBB CCCC DDDD EEEE FFFF"; // longer than the mnemonic column
    line.mnemonic = "FAKE";
    std::string text = Disassembler::FormatLine(line);
    // Mnemonic must still be preceded by at least one space
    EXPECT_NE(text.find(" FAKE"), std::string::npos);
}
