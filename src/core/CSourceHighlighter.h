#pragma once

#include "ArConsoleTypes.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace adm::core {

// Token categories for C/C++ syntax highlighting. Follows the usual TextMate-style
// taxonomy, with types split out from keywords: Amiga source is full of UBYTE/APTR/
// STRPTR, and colouring those as types is most of what makes a listing readable.
enum class CTokenType : uint8_t {
    Default,      // whitespace and anything unclassified — never breaks the tiling
    Keyword,      // if / for / return / class / namespace / true / nullptr / ...
    Type,         // int / float / uint32_t / size_t / UBYTE / APTR / ...
    Preprocessor, // the '#' plus the directive word
    String,       // "..." with escapes, raw strings, and <...> on an #include line
    Char,         // '...' with escapes
    Number,       // 42, 0xFF, 0b1010, 1.5e-3f, 1'000'000
    Comment,      // // to end of line, and /* ... */ including across lines
    Identifier,   // every other word-shaped run
    FunctionCall, // an identifier immediately followed by '('
    Operator,     // + - * / = < > & | ! ~ ^ % ? :
    Punctuation,  // ( ) [ ] { } , ; . -> ::
};

// One contiguous run of characters of a single token type. Spans returned by
// TokenizeCLine tile [0, text.size()) edge-to-edge (no gaps, no overlaps), so a
// renderer can walk them in order to colour the whole line. Same contract as AsmToken.
struct CToken
{
    int col = 0;
    int len = 0;
    CTokenType type = CTokenType::Default;
};

// Lexer carry-state across a line boundary. The disassembly listing tokenizes lines
// individually and out of order, so the parts of C that survive a newline — a block
// comment, a raw string, a backslash-continued directive — have to be threaded
// explicitly. A default-constructed state means "start of file".
struct CLexState
{
    bool inBlockComment = false;
    bool inRawString = false;
    std::string rawDelim;         // the delimiter that closes the raw string
    bool continuedString = false; // a "..." ran to end of line behind a trailing backslash
    bool inPreprocessor = false;  // a #directive continued with a trailing backslash

    bool operator==(const CLexState&) const = default;
};

// Tokenizes exactly one line of C/C++. `text` excludes the line terminator. `state` is
// the carry from the previous line on entry and the carry for the next line on exit.
std::vector<CToken> TokenizeCLine(std::string_view text, CLexState& state);

// Tokenizes a whole file, threading the state. For tests and whole-file consumers.
std::vector<std::vector<CToken>> TokenizeCLines(const std::vector<std::string>& lines);

// The incoming CLexState for every line, in one pass, without building tokens. This is
// what makes random-access line highlighting affordable: run it once per file (O(file)),
// keep the vector, and any single line then costs O(line). Returns lines.size() + 1
// entries — [n] is the state entering 0-based line n, and the last entry is the state
// at end of file.
std::vector<CLexState> ScanCLineStates(const std::vector<std::string>& lines);

// True when the line contributes nothing but comment and whitespace. Drives the
// "Code only" density mode, which drops such lines from an interleaved source run.
bool IsCommentOrBlankLine(std::string_view text, CLexState state);

// Maps a token type onto a console semantic colour. Kept here (ImGui-free) so core code
// can build coloured spans without depending on the UI. Mirrors AsmTokenColor.
ArColor CTokenColor(CTokenType type);

} // namespace adm::core
