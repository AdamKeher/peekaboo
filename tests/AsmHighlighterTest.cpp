#include <gtest/gtest.h>

#include "core/AsmHighlighter.h"
#include "core/Disassembler.h"

#include <string>

using adm::core::ArColor;
using adm::core::AsmToken;
using adm::core::AsmTokenColor;
using adm::core::AsmTokenType;
using adm::core::Disassembler;
using adm::core::TokenizeAsm;

namespace {

// Returns the type of the first token whose exact text equals `sub`, or Default
// when no token matches.
AsmTokenType TypeOf(const std::string& text, const std::vector<AsmToken>& toks,
                    const std::string& sub)
{
    for (const auto& t : toks)
        if (text.substr(t.col, t.len) == sub)
            return t.type;
    return AsmTokenType::Default;
}

// Tokens must tile [start, len) edge-to-edge: no gaps, no overlaps.
void ExpectContiguous(const std::string& text, const std::vector<AsmToken>& toks, int start)
{
    int cursor = start;
    for (const auto& t : toks) {
        EXPECT_EQ(t.col, cursor) << "gap/overlap before token at col " << t.col;
        EXPECT_GT(t.len, 0);
        cursor = t.col + t.len;
    }
    EXPECT_EQ(cursor, static_cast<int>(text.size()));
}

} // namespace

TEST(AsmHighlighterTest, MoveImmediateToRegister)
{
    std::string text = "MOVE.L #$1234,D0";
    auto toks = TokenizeAsm(text);
    ExpectContiguous(text, toks, 0);
    EXPECT_EQ(TypeOf(text, toks, "MOVE"), AsmTokenType::Opcode);
    EXPECT_EQ(TypeOf(text, toks, ".L"), AsmTokenType::SizeSuffix);
    EXPECT_EQ(TypeOf(text, toks, "#$1234"), AsmTokenType::Immediate);
    EXPECT_EQ(TypeOf(text, toks, ","), AsmTokenType::Punctuation);
    EXPECT_EQ(TypeOf(text, toks, "D0"), AsmTokenType::Register);
}

TEST(AsmHighlighterTest, JumpToAbsoluteAddress)
{
    std::string text = "JMP $00FC00D2";
    auto toks = TokenizeAsm(text);
    ExpectContiguous(text, toks, 0);
    EXPECT_EQ(TypeOf(text, toks, "JMP"), AsmTokenType::Opcode);
    EXPECT_EQ(TypeOf(text, toks, "$00FC00D2"), AsmTokenType::Address);
}

TEST(AsmHighlighterTest, IndexedAddressingWithRegisters)
{
    std::string text = "LEA (A0,D1.W),A2";
    auto toks = TokenizeAsm(text);
    ExpectContiguous(text, toks, 0);
    EXPECT_EQ(TypeOf(text, toks, "LEA"), AsmTokenType::Opcode);
    EXPECT_EQ(TypeOf(text, toks, "A0"), AsmTokenType::Register);
    EXPECT_EQ(TypeOf(text, toks, "D1"), AsmTokenType::Register);
    EXPECT_EQ(TypeOf(text, toks, ".W"), AsmTokenType::SizeSuffix);
    EXPECT_EQ(TypeOf(text, toks, "A2"), AsmTokenType::Register);
}

TEST(AsmHighlighterTest, OpcodeOnlyInstruction)
{
    std::string text = "RTS";
    auto toks = TokenizeAsm(text);
    ExpectContiguous(text, toks, 0);
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].type, AsmTokenType::Opcode);
}

TEST(AsmHighlighterTest, DecimalNumberOperand)
{
    std::string text = "ASL.W #3,D2";
    auto toks = TokenizeAsm(text);
    ExpectContiguous(text, toks, 0);
    EXPECT_EQ(TypeOf(text, toks, "#3"), AsmTokenType::Immediate);
    EXPECT_EQ(TypeOf(text, toks, "D2"), AsmTokenType::Register);
}

TEST(AsmHighlighterTest, RespectsStartColumnOnFormattedLine)
{
    Disassembler::Line line;
    line.address = 0x00FC0000;
    line.hex = "203C 0000 1234";
    line.mnemonic = "MOVE.L #$1234,D0";
    std::string text = Disassembler::FormatLine(line);

    auto toks = TokenizeAsm(text, Disassembler::kMnemonicColumn);
    ASSERT_FALSE(toks.empty());
    // Nothing before the mnemonic column is tokenized (address/hex have their own spans).
    EXPECT_GE(toks.front().col, Disassembler::kMnemonicColumn);
    ExpectContiguous(text, toks, Disassembler::kMnemonicColumn);
    EXPECT_EQ(TypeOf(text, toks, "MOVE"), AsmTokenType::Opcode);
    EXPECT_EQ(TypeOf(text, toks, "$1234"),
              AsmTokenType::Default); // part of "#$1234" immediate, not standalone
    EXPECT_EQ(TypeOf(text, toks, "#$1234"), AsmTokenType::Immediate);
}

TEST(AsmHighlighterTest, EmptyAndOutOfRange)
{
    EXPECT_TRUE(TokenizeAsm("").empty());
    EXPECT_TRUE(TokenizeAsm("ABC", 10).empty());
}

TEST(AsmHighlighterTest, TokenColorMapping)
{
    EXPECT_EQ(AsmTokenColor(AsmTokenType::Opcode), ArColor::AsmOpcode);
    EXPECT_EQ(AsmTokenColor(AsmTokenType::Register), ArColor::AsmRegister);
    EXPECT_EQ(AsmTokenColor(AsmTokenType::Immediate), ArColor::AsmImmediate);
    EXPECT_EQ(AsmTokenColor(AsmTokenType::Address), ArColor::AsmAddress);
    EXPECT_EQ(AsmTokenColor(AsmTokenType::Default), ArColor::Default);
}
