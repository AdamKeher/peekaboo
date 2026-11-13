#include "AsmHighlighter.h"

#include <cctype>

namespace adm::core {

namespace {

bool IsWordChar(char c)
{
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_';
}

bool IsHexDigit(char c)
{
    unsigned char u = static_cast<unsigned char>(c);
    return std::isxdigit(u);
}

char Upper(char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }

// True for a 68k register name: Dn/An (n 0-7), SP, PC, SR, CCR, USP, SSP.
bool IsRegisterWord(const std::string& text, int begin, int end)
{
    int len = end - begin;
    if (len == 2) {
        char a = Upper(text[begin]);
        char b = text[begin + 1];
        if ((a == 'D' || a == 'A') && b >= '0' && b <= '7')
            return true;
        std::string w = {a, Upper(b)};
        return w == "SP" || w == "PC" || w == "SR";
    }
    if (len == 3) {
        std::string w = {Upper(text[begin]), Upper(text[begin + 1]), Upper(text[begin + 2])};
        return w == "CCR" || w == "USP" || w == "SSP";
    }
    return false;
}

bool IsPunct(char c)
{
    switch (c) {
        case '(':
        case ')':
        case ',':
        case '+':
        case '-':
        case '.':
        case '/':
        case '{':
        case '}':
            return true;
        default:
            return false;
    }
}

} // namespace

std::vector<AsmToken> TokenizeAsm(const std::string& text, int start)
{
    std::vector<AsmToken> tokens;
    const int n = static_cast<int>(text.size());
    if (start < 0)
        start = 0;
    if (start >= n)
        return tokens;

    auto push = [&](int begin, int end, AsmTokenType type) {
        if (end > begin)
            tokens.push_back({begin, end - begin, type});
    };

    int i = start;

    // Leading whitespace before the opcode (rare for bare mnemonics, possible when
    // FormatLine padding is included in the slice).
    int ws = i;
    while (i < n && std::isspace(static_cast<unsigned char>(text[i])))
        ++i;
    push(ws, i, AsmTokenType::Default);

    // Opcode: the first run of word characters.
    if (i < n && IsWordChar(text[i])) {
        int op = i;
        while (i < n && IsWordChar(text[i]))
            ++i;
        push(op, i, AsmTokenType::Opcode);

        // Optional size suffix attached to the opcode: ".b/.w/.l/.s".
        if (i < n && text[i] == '.' && i + 1 < n &&
            std::isalpha(static_cast<unsigned char>(text[i + 1]))) {
            int sz = i;
            ++i; // '.'
            while (i < n && std::isalpha(static_cast<unsigned char>(text[i])))
                ++i;
            push(sz, i, AsmTokenType::SizeSuffix);
        }
    }

    // Operands.
    while (i < n) {
        char c = text[i];

        if (std::isspace(static_cast<unsigned char>(c))) {
            int begin = i;
            while (i < n && std::isspace(static_cast<unsigned char>(text[i])))
                ++i;
            push(begin, i, AsmTokenType::Default);
            continue;
        }

        if (c == '#') {
            int begin = i++;
            // Consume the immediate value: $hex, digits, or a symbol run.
            if (i < n && text[i] == '$') {
                ++i;
                while (i < n && IsHexDigit(text[i]))
                    ++i;
            } else {
                while (i < n && (IsWordChar(text[i]) || text[i] == '$'))
                    ++i;
            }
            push(begin, i, AsmTokenType::Immediate);
            continue;
        }

        if (c == '$') {
            int begin = i++;
            while (i < n && IsHexDigit(text[i]))
                ++i;
            push(begin, i, AsmTokenType::Address);
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            int begin = i;
            while (i < n && IsWordChar(text[i]))
                ++i;
            push(begin, i, AsmTokenType::Number);
            continue;
        }

        if (IsWordChar(c)) {
            int begin = i;
            while (i < n && IsWordChar(text[i]))
                ++i;
            AsmTokenType type =
                IsRegisterWord(text, begin, i) ? AsmTokenType::Register : AsmTokenType::Default;
            push(begin, i, type);

            // Index-register size suffix, e.g. "D1.W" inside (A0,D1.W).
            if (type == AsmTokenType::Register && i < n && text[i] == '.' && i + 1 < n &&
                std::isalpha(static_cast<unsigned char>(text[i + 1]))) {
                int sz = i;
                i += 2;
                push(sz, i, AsmTokenType::SizeSuffix);
            }
            continue;
        }

        if (IsPunct(c)) {
            int begin = i;
            while (i < n && IsPunct(text[i]))
                ++i;
            push(begin, i, AsmTokenType::Punctuation);
            continue;
        }

        // Anything else: emit as a single Default char and advance.
        push(i, i + 1, AsmTokenType::Default);
        ++i;
    }

    return tokens;
}

ArColor AsmTokenColor(AsmTokenType type)
{
    switch (type) {
        case AsmTokenType::Opcode:
            return ArColor::AsmOpcode;
        case AsmTokenType::SizeSuffix:
            return ArColor::AsmSize;
        case AsmTokenType::Register:
            return ArColor::AsmRegister;
        case AsmTokenType::Immediate:
            return ArColor::AsmImmediate;
        case AsmTokenType::Address:
            return ArColor::AsmAddress;
        case AsmTokenType::Number:
            return ArColor::AsmNumber;
        case AsmTokenType::Punctuation:
            return ArColor::AsmPunct;
        case AsmTokenType::Default:
        default:
            return ArColor::Default;
    }
}

} // namespace adm::core
