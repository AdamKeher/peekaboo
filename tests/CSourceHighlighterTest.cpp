#include <gtest/gtest.h>

#include "core/CSourceHighlighter.h"

#include <algorithm>
#include <string>
#include <vector>

using adm::core::ArColor;
using adm::core::CLexState;
using adm::core::CToken;
using adm::core::CTokenColor;
using adm::core::CTokenType;
using adm::core::IsCommentOrBlankLine;
using adm::core::ScanCLineStates;
using adm::core::TokenizeCLine;
using adm::core::TokenizeCLines;

namespace {

// Returns the type of the first token whose exact text equals `sub`, or Default when no
// token matches.
CTokenType TypeOf(const std::string& text, const std::vector<CToken>& toks, const std::string& sub)
{
    for (const auto& t : toks)
        if (text.substr(t.col, t.len) == sub)
            return t.type;
    return CTokenType::Default;
}

// Tokens must tile [0, size) edge-to-edge: no gaps, no overlaps, no empty spans.
void ExpectContiguous(const std::string& text, const std::vector<CToken>& toks)
{
    int cursor = 0;
    for (const auto& t : toks) {
        EXPECT_EQ(t.col, cursor) << "gap/overlap before token at col " << t.col << " in [" << text
                                 << "]";
        EXPECT_GT(t.len, 0) << "empty token at col " << t.col;
        cursor = t.col + t.len;
    }
    EXPECT_EQ(cursor, static_cast<int>(text.size())) << "did not reach end of [" << text << "]";
}

std::vector<CToken> Tok(const std::string& text)
{
    CLexState st;
    auto toks = TokenizeCLine(text, st);
    ExpectContiguous(text, toks);
    return toks;
}

} // namespace

TEST(CSourceHighlighterTest, SimpleDeclaration)
{
    const std::string text = "int x = 42;";
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "int"), CTokenType::Type);
    EXPECT_EQ(TypeOf(text, toks, "x"), CTokenType::Identifier);
    EXPECT_EQ(TypeOf(text, toks, "="), CTokenType::Operator);
    EXPECT_EQ(TypeOf(text, toks, "42"), CTokenType::Number);
    EXPECT_EQ(TypeOf(text, toks, ";"), CTokenType::Punctuation);
}

TEST(CSourceHighlighterTest, KeywordsAndAmigaTypes)
{
    const std::string text = "for (UWORD i = 0; i < n; ++i)";
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "for"), CTokenType::Keyword);
    EXPECT_EQ(TypeOf(text, toks, "UWORD"), CTokenType::Type);

    const std::string apt = "APTR p = (STRPTR)q;";
    auto t2 = Tok(apt);
    EXPECT_EQ(TypeOf(apt, t2, "APTR"), CTokenType::Type);
    EXPECT_EQ(TypeOf(apt, t2, "STRPTR"), CTokenType::Type);
}

TEST(CSourceHighlighterTest, NumberForms)
{
    for (const char* lit : {"0xFF", "0b1010", "042", "123u", "1.5e-3f", "1.0", "0x1p-3"}) {
        const std::string text = std::string("x = ") + lit + ";";
        auto toks = Tok(text);
        EXPECT_EQ(TypeOf(text, toks, lit), CTokenType::Number) << lit;
    }
}

TEST(CSourceHighlighterTest, DigitSeparatorIsNotACharLiteral)
{
    const std::string text = "long v = 1'000'000;";
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "1'000'000"), CTokenType::Number);
    for (const auto& t : toks)
        EXPECT_NE(t.type, CTokenType::Char);
}

TEST(CSourceHighlighterTest, StringsAndEscapes)
{
    const std::string text = "puts(\"a\\\"b\");"; // puts("a\"b");
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "\"a\\\"b\""), CTokenType::String);
    EXPECT_EQ(TypeOf(text, toks, "puts"), CTokenType::FunctionCall);
}

TEST(CSourceHighlighterTest, ApostropheInsideStringIsNotACharLiteral)
{
    const std::string text = "const char* s = \"it's fine\"; int y = 1;";
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "\"it's fine\""), CTokenType::String);
    EXPECT_EQ(TypeOf(text, toks, "y"), CTokenType::Identifier);
}

TEST(CSourceHighlighterTest, CharLiterals)
{
    const std::string text = "c = '\\n'; d = '\\'';";
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "'\\n'"), CTokenType::Char);
    EXPECT_EQ(TypeOf(text, toks, "'\\''"), CTokenType::Char);
}

TEST(CSourceHighlighterTest, LineComment)
{
    const std::string text = "x = 1; // set it up";
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "// set it up"), CTokenType::Comment);
}

TEST(CSourceHighlighterTest, SingleLineBlockComment)
{
    const std::string text = "a = /* why */ b;";
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "/* why */"), CTokenType::Comment);
    EXPECT_EQ(TypeOf(text, toks, "b"), CTokenType::Identifier);
}

TEST(CSourceHighlighterTest, BlockCommentSpansLines)
{
    const std::vector<std::string> lines = {"/* opening", "still inside", "b */ int x;", "int y;"};
    CLexState st;

    auto l0 = TokenizeCLine(lines[0], st);
    ExpectContiguous(lines[0], l0);
    ASSERT_EQ(l0.size(), 1u);
    EXPECT_EQ(l0[0].type, CTokenType::Comment);
    EXPECT_TRUE(st.inBlockComment);

    auto l1 = TokenizeCLine(lines[1], st);
    ExpectContiguous(lines[1], l1);
    ASSERT_EQ(l1.size(), 1u);
    EXPECT_EQ(l1[0].type, CTokenType::Comment);
    EXPECT_TRUE(st.inBlockComment);

    auto l2 = TokenizeCLine(lines[2], st);
    ExpectContiguous(lines[2], l2);
    EXPECT_FALSE(st.inBlockComment);
    EXPECT_EQ(TypeOf(lines[2], l2, "b */"), CTokenType::Comment);
    EXPECT_EQ(TypeOf(lines[2], l2, "int"), CTokenType::Type);
    EXPECT_EQ(TypeOf(lines[2], l2, "x"), CTokenType::Identifier);

    auto l3 = TokenizeCLine(lines[3], st);
    ExpectContiguous(lines[3], l3);
    for (const auto& t : l3)
        EXPECT_NE(t.type, CTokenType::Comment);
}

TEST(CSourceHighlighterTest, PreprocessorInclude)
{
    const std::string text = "#include <stdio.h>";
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "#include"), CTokenType::Preprocessor);
    EXPECT_EQ(TypeOf(text, toks, "<stdio.h>"), CTokenType::String);

    const std::string q = "#include \"local.h\"";
    auto qt = Tok(q);
    EXPECT_EQ(TypeOf(q, qt, "#include"), CTokenType::Preprocessor);
    EXPECT_EQ(TypeOf(q, qt, "\"local.h\""), CTokenType::String);
}

TEST(CSourceHighlighterTest, PreprocessorDefineAndContinuation)
{
    const std::string text = "#define FOO 1";
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "#define"), CTokenType::Preprocessor);
    EXPECT_EQ(TypeOf(text, toks, "1"), CTokenType::Number);

    CLexState st;
    const std::string cont = "#define BAR(x) \\";
    auto ct = TokenizeCLine(cont, st);
    ExpectContiguous(cont, ct);
    EXPECT_TRUE(st.inPreprocessor);
}

TEST(CSourceHighlighterTest, LessThanIsAnOperatorOutsideInclude)
{
    const std::string text = "if (a < b) return;";
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "<"), CTokenType::Operator);
}

TEST(CSourceHighlighterTest, StringPrefixes)
{
    const std::string text = "auto s = u8\"hi\";";
    auto toks = Tok(text);
    // The prefix must colour as part of the literal, not as a stray identifier.
    EXPECT_EQ(TypeOf(text, toks, "u8"), CTokenType::String);
    EXPECT_EQ(TypeOf(text, toks, "\"hi\""), CTokenType::String);
}

TEST(CSourceHighlighterTest, RawStringSingleLine)
{
    const std::string text = "auto s = R\"(a\"b)\";";
    auto toks = Tok(text);
    EXPECT_EQ(TypeOf(text, toks, "R"), CTokenType::String);
    EXPECT_EQ(TypeOf(text, toks, "\"(a\"b)\""), CTokenType::String);
}

TEST(CSourceHighlighterTest, RawStringSpansLines)
{
    CLexState st;
    const std::string open = "auto s = R\"delim(first";
    auto t0 = TokenizeCLine(open, st);
    ExpectContiguous(open, t0);
    EXPECT_TRUE(st.inRawString);
    EXPECT_EQ(st.rawDelim, "delim");

    const std::string close = "second)delim\"; int x;";
    auto t1 = TokenizeCLine(close, st);
    ExpectContiguous(close, t1);
    EXPECT_FALSE(st.inRawString);
    EXPECT_EQ(TypeOf(close, t1, "int"), CTokenType::Type);
}

TEST(CSourceHighlighterTest, FunctionCallUpgrade)
{
    const std::string call = "foo(1);";
    EXPECT_EQ(TypeOf(call, Tok(call), "foo"), CTokenType::FunctionCall);

    const std::string expr = "foo + 1;";
    EXPECT_EQ(TypeOf(expr, Tok(expr), "foo"), CTokenType::Identifier);
}

TEST(CSourceHighlighterTest, DegenerateInputStillTilesAndTerminates)
{
    for (const char* line : {"", "   ", "\t", "{};", "/", "\"", "'", "R\"", "#", "\\", "///", "/*",
                             "*/", "0x", "1'", "u8", "<<<>>>"}) {
        CLexState st;
        const std::string text = line;
        auto toks = TokenizeCLine(text, st);
        ExpectContiguous(text, toks);
    }
}

TEST(CSourceHighlighterTest, ScanLineStatesMatchesTokenizer)
{
    const std::vector<std::string> lines = {
        "int a;", "/* open", "middle", "close */ int b;", "#define X \\", "  continued", "int c;",
    };
    auto states = ScanCLineStates(lines);
    ASSERT_EQ(states.size(), lines.size() + 1);

    CLexState running;
    for (size_t i = 0; i < lines.size(); ++i) {
        EXPECT_EQ(states[i], running) << "state entering line " << i;
        TokenizeCLine(lines[i], running);
    }
    EXPECT_EQ(states.back(), running);
}

TEST(CSourceHighlighterTest, TokenizeCLinesThreadsState)
{
    const std::vector<std::string> lines = {"/* a", "b */ int x;"};
    auto all = TokenizeCLines(lines);
    ASSERT_EQ(all.size(), 2u);
    ASSERT_EQ(all[0].size(), 1u);
    EXPECT_EQ(all[0][0].type, CTokenType::Comment);
    EXPECT_EQ(TypeOf(lines[1], all[1], "int"), CTokenType::Type);
}

TEST(CSourceHighlighterTest, CommentOrBlankDetection)
{
    CLexState clean;
    EXPECT_TRUE(IsCommentOrBlankLine("", clean));
    EXPECT_TRUE(IsCommentOrBlankLine("    ", clean));
    EXPECT_TRUE(IsCommentOrBlankLine("// just a note", clean));
    EXPECT_TRUE(IsCommentOrBlankLine("  /* block */  ", clean));
    EXPECT_FALSE(IsCommentOrBlankLine("int x; // note", clean));
    EXPECT_FALSE(IsCommentOrBlankLine("}", clean));

    // A line entirely inside a carried block comment counts as comment-only.
    CLexState inComment;
    inComment.inBlockComment = true;
    EXPECT_TRUE(IsCommentOrBlankLine(" * doc text", inComment));
    // ...but one that closes the comment and then does work does not.
    EXPECT_FALSE(IsCommentOrBlankLine(" */ int x;", inComment));
}

TEST(CSourceHighlighterTest, CommentDetectionDoesNotMutateCallerState)
{
    CLexState st;
    IsCommentOrBlankLine("/* unterminated", st);
    EXPECT_FALSE(st.inBlockComment) << "IsCommentOrBlankLine takes state by value";
}

TEST(CSourceHighlighterTest, EveryTokenTypeHasADistinctColour)
{
    const CTokenType types[] = {
        CTokenType::Keyword,  CTokenType::Type,        CTokenType::Preprocessor,
        CTokenType::String,   CTokenType::Char,        CTokenType::Number,
        CTokenType::Comment,  CTokenType::Identifier,  CTokenType::FunctionCall,
        CTokenType::Operator, CTokenType::Punctuation,
    };
    std::vector<ArColor> seen;
    for (auto t : types) {
        ArColor c = CTokenColor(t);
        EXPECT_NE(c, ArColor::Default);
        EXPECT_EQ(std::find(seen.begin(), seen.end(), c), seen.end());
        seen.push_back(c);
    }
    EXPECT_EQ(CTokenColor(CTokenType::Default), ArColor::Default);
}
