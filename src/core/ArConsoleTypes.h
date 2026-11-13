#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adm::core {

// Semantic colors for AR console output. The UI terminal maps these onto
// theme-derived ImGui colors; core code stays ImGui-free.
enum class ArColor : uint8_t {
    Default,
    Address,
    HexData,
    Ascii,
    Mnemonic,
    Error,
    Info,
    Highlight,
    Prompt,
    Register, // custom chip register name — hyperlink to Custom Registers window
    Swatch,   // filled colour block; ArSpan::payload holds a 0xRRGGBB value

    // 68k disassembly syntax-highlighting token colours (see AsmHighlighter).
    AsmOpcode,
    AsmSize,      // .b/.w/.l size suffix
    AsmRegister,  // Dn/An/SP/PC/SR/CCR/USP
    AsmImmediate, // #... operand
    AsmAddress,   // $hex absolute address
    AsmNumber,    // bare decimal number
    AsmPunct,     // ( ) , + - and other separators

    Comment, // muted; ';' lines and code-section separators

    Label, // user symbolic label (line above an instruction / branch ref)

    // C/C++ source syntax-highlighting token colours (see CSourceHighlighter). Appended
    // rather than grouped with the Asm* block so existing enumerator values stay put.
    SrcKeyword,
    SrcType,
    SrcPreproc,
    SrcString,
    SrcChar,
    SrcNumber,
    SrcComment,
    SrcIdentifier,
    SrcFunction,
    SrcOperator,
    SrcPunct,

    // Chrome for the interleaved source rows themselves.
    SrcGutter,  // the right-aligned line-number column
    SrcHeader,  // "; ---- file.c ----"
    SrcMissing, // "; file.c:42 (source not found)"
};

struct ArSpan
{
    int col = 0;
    int len = 0;
    ArColor color = ArColor::Default;
    uint32_t payload = 0; // ArColor::Register: custom reg byte offset within $DFF000
};

// One line of console output: plain text plus presentation-only color spans.
// Re-parsing on Return uses only the text; spans never affect interpretation.
struct ArLine
{
    std::string text;
    std::vector<ArSpan> spans;

    ArLine() = default;
    ArLine(std::string t) : text(std::move(t)) {}
    ArLine(std::string t, ArColor color) : text(std::move(t))
    {
        spans.push_back({0, static_cast<int>(text.size()), color});
    }

    ArLine& Span(int col, int len, ArColor color, uint32_t payload = 0)
    {
        spans.push_back({col, len, color, payload});
        return *this;
    }
};

} // namespace adm::core
