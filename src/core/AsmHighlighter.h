#pragma once

#include "ArConsoleTypes.h"

#include <string>
#include <vector>

namespace adm::core {

// Token categories for 68k disassembly syntax highlighting. The taxonomy follows
// the common assembler/TextMate convention: mnemonic, register, numeric constant
// (hex/decimal/immediate) and punctuation.
enum class AsmTokenType {
    Default,
    Opcode,
    SizeSuffix,
    Register,
    Immediate,
    Address,
    Number,
    Punctuation,
};

// One contiguous run of characters of a single token type. Spans returned by
// TokenizeAsm tile the operand region edge-to-edge (no gaps, no overlaps), so a
// renderer can walk them in order to colour the whole string.
struct AsmToken
{
    int col = 0;
    int len = 0;
    AsmTokenType type = AsmTokenType::Default;
};

// Tokenizes the disassembly text from column `start` to its end. Pass
// start = 0 for a bare mnemonic string ("JMP $00FC00D2") or
// Disassembler::kMnemonicColumn for a full FormatLine string. Unrecognised
// characters fall back to AsmTokenType::Default so output never breaks.
std::vector<AsmToken> TokenizeAsm(const std::string& text, int start = 0);

// Maps a token type onto a console semantic colour. Kept here (ImGui-free) so the
// AR console interpreter can build coloured spans without depending on the UI.
ArColor AsmTokenColor(AsmTokenType type);

} // namespace adm::core
