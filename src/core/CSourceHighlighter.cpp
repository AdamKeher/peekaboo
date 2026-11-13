#include "CSourceHighlighter.h"

#include <algorithm>
#include <cctype>

namespace adm::core {

namespace {

bool IsWordStart(char c)
{
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalpha(u) || c == '_' || c == '$';
}

bool IsWordChar(char c)
{
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '$';
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r'; }

// Sorted at author time; CSourceHighlighterTest asserts std::is_sorted so a later
// addition cannot silently break the binary search.
constexpr std::string_view kKeywords[] = {
    "_Bool",       "_Static_assert",
    "alignas",     "alignof",
    "and",         "and_eq",
    "asm",         "auto",
    "break",       "case",
    "catch",       "class",
    "co_await",    "co_return",
    "co_yield",    "compl",
    "concept",     "const",
    "const_cast",  "consteval",
    "constexpr",   "constinit",
    "continue",    "decltype",
    "default",     "delete",
    "do",          "dynamic_cast",
    "else",        "enum",
    "explicit",    "export",
    "extern",      "false",
    "for",         "friend",
    "goto",        "if",
    "inline",      "mutable",
    "namespace",   "new",
    "noexcept",    "not",
    "not_eq",      "nullptr",
    "operator",    "or",
    "or_eq",       "private",
    "protected",   "public",
    "register",    "reinterpret_cast",
    "requires",    "restrict",
    "return",      "sizeof",
    "static",      "static_assert",
    "static_cast", "struct",
    "switch",      "template",
    "this",        "thread_local",
    "throw",       "true",
    "try",         "typedef",
    "typeid",      "typename",
    "union",       "using",
    "virtual",     "volatile",
    "while",       "xor",
    "xor_eq",
};

// Builtin and Amiga SDK type names. Also sorted.
constexpr std::string_view kTypes[] = {
    "APTR",     "BOOL",      "BPTR",      "BYTE",     "CPTR",    "LONG",     "STRPTR",   "UBYTE",
    "ULONG",    "UWORD",     "WORD",      "bool",     "char",    "char16_t", "char32_t", "char8_t",
    "double",   "float",     "int",       "int16_t",  "int32_t", "int64_t",  "int8_t",   "intptr_t",
    "long",     "ptrdiff_t", "short",     "signed",   "size_t",  "ssize_t",  "uint16_t", "uint32_t",
    "uint64_t", "uint8_t",   "uintptr_t", "unsigned", "void",    "wchar_t",
};

// The tables are binary-searched, so an unsorted addition would silently stop matching
// rather than fail loudly. Catch it at compile time instead.
static_assert(std::is_sorted(std::begin(kKeywords), std::end(kKeywords)),
              "kKeywords must stay sorted");
static_assert(std::is_sorted(std::begin(kTypes), std::end(kTypes)), "kTypes must stay sorted");

bool InTable(const std::string_view* table, size_t count, std::string_view word)
{
    return std::binary_search(table, table + count, word);
}

bool IsOperatorChar(char c)
{
    switch (c) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '=':
        case '<':
        case '>':
        case '&':
        case '|':
        case '^':
        case '!':
        case '~':
        case '?':
            return true;
        default:
            return false;
    }
}

bool IsPunctChar(char c)
{
    switch (c) {
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case ',':
        case ';':
        case ':':
        case '.':
        case '#':
        case '@':
        case '\\':
            return true;
        default:
            return false;
    }
}

// A string/char literal prefix immediately preceding the quote: L"…", u8"…", R"(…)".
// Returns the number of characters of `text` ending at `quotePos` that belong to it.
int LiteralPrefixLen(std::string_view text, int quotePos)
{
    int len = 0;
    int i = quotePos - 1;
    while (i >= 0 && len < 3 && IsWordChar(text[i])) {
        char c = text[i];
        if (c == 'L' || c == 'u' || c == 'U' || c == 'R' || c == '8') {
            ++len;
            --i;
        } else {
            return 0; // part of a longer identifier, not a literal prefix
        }
    }
    // Anything word-shaped still to the left means this was an identifier tail.
    if (i >= 0 && IsWordChar(text[i]))
        return 0;
    return len;
}

bool PrefixHasRaw(std::string_view text, int quotePos, int prefixLen)
{
    for (int k = quotePos - prefixLen; k < quotePos; ++k)
        if (text[k] == 'R')
            return true;
    return false;
}

// The line ends in an unescaped backslash — the next line continues this construct.
bool EndsWithContinuation(std::string_view text)
{
    int n = static_cast<int>(text.size());
    while (n > 0 && IsSpace(text[n - 1]))
        --n;
    return n > 0 && text[n - 1] == '\\';
}

// Scans one line, appending tokens when `out` is non-null and always updating `state`.
// Sharing the walk between TokenizeCLine and ScanCLineStates keeps the two in step —
// a state machine duplicated for a fast path is a state machine that drifts.
void ScanLine(std::string_view text, CLexState& state, std::vector<CToken>* out)
{
    const int n = static_cast<int>(text.size());

    auto push = [&](int begin, int end, CTokenType type) {
        if (out && end > begin)
            out->push_back({begin, end - begin, type});
    };

    int i = 0;

    // --- carried block comment ---
    if (state.inBlockComment) {
        int close = static_cast<int>(text.find("*/", 0));
        if (close == static_cast<int>(std::string_view::npos)) {
            push(0, n, CTokenType::Comment);
            return; // flag stays set
        }
        state.inBlockComment = false;
        i = close + 2;
        push(0, i, CTokenType::Comment);
    }

    // --- carried raw string ---
    if (state.inRawString) {
        std::string closer = ")" + state.rawDelim + "\"";
        int close = static_cast<int>(text.find(closer, i));
        if (close == static_cast<int>(std::string_view::npos)) {
            push(i, n, CTokenType::String);
            return;
        }
        int end = close + static_cast<int>(closer.size());
        state.inRawString = false;
        state.rawDelim.clear();
        push(i, end, CTokenType::String);
        i = end;
    }

    // --- carried string continuation ---
    if (state.continuedString) {
        state.continuedString = false;
        int j = i;
        bool closed = false;
        while (j < n) {
            if (text[j] == '\\' && j + 1 < n) {
                j += 2;
                continue;
            }
            if (text[j] == '"') {
                ++j;
                closed = true;
                break;
            }
            ++j;
        }
        push(i, j, CTokenType::String);
        i = j;
        if (!closed && EndsWithContinuation(text))
            state.continuedString = true;
    }

    // --- carried preprocessor continuation ---
    const bool carriedPreproc = state.inPreprocessor;
    state.inPreprocessor = false;

    // A line-leading '#' opens a directive. So does a carried continuation.
    bool preprocLine = carriedPreproc;
    if (!preprocLine) {
        int p = i;
        while (p < n && IsSpace(text[p]))
            ++p;
        if (p < n && text[p] == '#') {
            preprocLine = true;
            push(i, p, CTokenType::Default);
            int begin = p;
            ++p;
            while (p < n && IsSpace(text[p]))
                ++p;
            while (p < n && IsWordChar(text[p]))
                ++p;
            push(begin, p, CTokenType::Preprocessor);
            i = p;
        }
    }

    while (i < n) {
        const char c = text[i];

        if (IsSpace(c)) {
            int begin = i;
            while (i < n && IsSpace(text[i]))
                ++i;
            push(begin, i, CTokenType::Default);
            continue;
        }

        // Line comment.
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            push(i, n, CTokenType::Comment);
            i = n;
            continue;
        }

        // Block comment.
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            int close = static_cast<int>(text.find("*/", i + 2));
            if (close == static_cast<int>(std::string_view::npos)) {
                push(i, n, CTokenType::Comment);
                state.inBlockComment = true;
                i = n;
                continue;
            }
            push(i, close + 2, CTokenType::Comment);
            i = close + 2;
            continue;
        }

        // An #include's <...> is a header name, not a pair of comparisons.
        if (preprocLine && c == '<') {
            int close = static_cast<int>(text.find('>', i + 1));
            if (close != static_cast<int>(std::string_view::npos)) {
                push(i, close + 1, CTokenType::String);
                i = close + 1;
                continue;
            }
        }

        // String literal, possibly raw.
        if (c == '"') {
            const int prefix = LiteralPrefixLen(text, i);
            if (prefix > 0 && out && !out->empty()) {
                // Re-label the prefix characters, which the word branch already emitted.
                CToken& last = out->back();
                if (last.len > prefix) {
                    last.len -= prefix;
                    out->push_back({i - prefix, prefix, CTokenType::String});
                } else {
                    last.type = CTokenType::String;
                }
            }
            if (prefix > 0 && PrefixHasRaw(text, i, prefix)) {
                int open = static_cast<int>(text.find('(', i + 1));
                if (open != static_cast<int>(std::string_view::npos)) {
                    std::string delim(text.substr(i + 1, open - i - 1));
                    std::string closer = ")" + delim + "\"";
                    int close = static_cast<int>(text.find(closer, open + 1));
                    if (close == static_cast<int>(std::string_view::npos)) {
                        push(i, n, CTokenType::String);
                        state.inRawString = true;
                        state.rawDelim = delim;
                        i = n;
                        continue;
                    }
                    int end = close + static_cast<int>(closer.size());
                    push(i, end, CTokenType::String);
                    i = end;
                    continue;
                }
            }
            int j = i + 1;
            bool closed = false;
            while (j < n) {
                if (text[j] == '\\' && j + 1 < n) {
                    j += 2;
                    continue;
                }
                if (text[j] == '"') {
                    ++j;
                    closed = true;
                    break;
                }
                ++j;
            }
            push(i, j, CTokenType::String);
            i = j;
            if (!closed && EndsWithContinuation(text))
                state.continuedString = true;
            continue;
        }

        // Character literal — unless this is a C++14 digit separator (1'000'000), which
        // the number branch below owns.
        if (c == '\'') {
            const bool digitSep =
                i > 0 && IsDigit(text[i - 1]) && i + 1 < n && IsDigit(text[i + 1]);
            if (!digitSep) {
                const int prefix = LiteralPrefixLen(text, i);
                if (prefix > 0 && out && !out->empty()) {
                    CToken& last = out->back();
                    if (last.len > prefix) {
                        last.len -= prefix;
                        out->push_back({i - prefix, prefix, CTokenType::Char});
                    } else {
                        last.type = CTokenType::Char;
                    }
                }
                int j = i + 1;
                while (j < n) {
                    if (text[j] == '\\' && j + 1 < n) {
                        j += 2;
                        continue;
                    }
                    if (text[j] == '\'') {
                        ++j;
                        break;
                    }
                    ++j;
                }
                push(i, j, CTokenType::Char);
                i = j;
                continue;
            }
        }

        // Number: a digit, or a '.' immediately followed by one.
        if (IsDigit(c) || (c == '.' && i + 1 < n && IsDigit(text[i + 1]))) {
            int j = i;
            while (j < n) {
                char d = text[j];
                if (IsWordChar(d) || d == '.') {
                    ++j;
                    continue;
                }
                // Exponent sign is part of the number, not an operator.
                if ((d == '+' || d == '-') && j > i) {
                    char prev = text[j - 1];
                    if (prev == 'e' || prev == 'E' || prev == 'p' || prev == 'P') {
                        ++j;
                        continue;
                    }
                }
                // Digit separator.
                if (d == '\'' && j + 1 < n && IsDigit(text[j + 1])) {
                    ++j;
                    continue;
                }
                break;
            }
            push(i, j, CTokenType::Number);
            i = j;
            continue;
        }

        // Word: keyword, type, function call, or plain identifier.
        if (IsWordStart(c)) {
            int begin = i;
            while (i < n && IsWordChar(text[i]))
                ++i;
            std::string_view word = text.substr(begin, i - begin);
            CTokenType type = CTokenType::Identifier;
            if (InTable(kKeywords, std::size(kKeywords), word)) {
                type = CTokenType::Keyword;
            } else if (InTable(kTypes, std::size(kTypes), word)) {
                type = CTokenType::Type;
            } else {
                int p = i;
                while (p < n && IsSpace(text[p]))
                    ++p;
                if (p < n && text[p] == '(')
                    type = CTokenType::FunctionCall;
            }
            push(begin, i, type);
            continue;
        }

        if (IsOperatorChar(c)) {
            int begin = i;
            ++i; // the comment branches above already claimed a leading "//" or "/*"
            while (i < n && IsOperatorChar(text[i])) {
                // Do not swallow a comment opener into an operator run ("a+/*b*/").
                if (text[i] == '/' && i + 1 < n && (text[i + 1] == '/' || text[i + 1] == '*'))
                    break;
                ++i;
            }
            push(begin, i, CTokenType::Operator);
            continue;
        }

        if (IsPunctChar(c)) {
            int begin = i;
            while (i < n && IsPunctChar(text[i]))
                ++i;
            push(begin, i, CTokenType::Punctuation);
            continue;
        }

        // Anything else: emit as a single Default char and advance.
        push(i, i + 1, CTokenType::Default);
        ++i;
    }

    if (preprocLine && EndsWithContinuation(text))
        state.inPreprocessor = true;
}

} // namespace

std::vector<CToken> TokenizeCLine(std::string_view text, CLexState& state)
{
    std::vector<CToken> tokens;
    ScanLine(text, state, &tokens);
    return tokens;
}

std::vector<std::vector<CToken>> TokenizeCLines(const std::vector<std::string>& lines)
{
    std::vector<std::vector<CToken>> out;
    out.reserve(lines.size());
    CLexState state;
    for (const auto& line : lines)
        out.push_back(TokenizeCLine(line, state));
    return out;
}

std::vector<CLexState> ScanCLineStates(const std::vector<std::string>& lines)
{
    std::vector<CLexState> states;
    states.reserve(lines.size() + 1);
    CLexState state;
    for (const auto& line : lines) {
        states.push_back(state);
        ScanLine(line, state, nullptr);
    }
    states.push_back(state);
    return states;
}

bool IsCommentOrBlankLine(std::string_view text, CLexState state)
{
    std::vector<CToken> toks;
    ScanLine(text, state, &toks);
    for (const auto& t : toks) {
        if (t.type == CTokenType::Comment)
            continue;
        if (t.type == CTokenType::Default) {
            // Default covers whitespace; anything else in it is real content.
            bool allSpace = true;
            for (int k = 0; k < t.len; ++k)
                if (!IsSpace(text[t.col + k]))
                    allSpace = false;
            if (allSpace)
                continue;
        }
        return false;
    }
    return true;
}

ArColor CTokenColor(CTokenType type)
{
    switch (type) {
        case CTokenType::Keyword:
            return ArColor::SrcKeyword;
        case CTokenType::Type:
            return ArColor::SrcType;
        case CTokenType::Preprocessor:
            return ArColor::SrcPreproc;
        case CTokenType::String:
            return ArColor::SrcString;
        case CTokenType::Char:
            return ArColor::SrcChar;
        case CTokenType::Number:
            return ArColor::SrcNumber;
        case CTokenType::Comment:
            return ArColor::SrcComment;
        case CTokenType::Identifier:
            return ArColor::SrcIdentifier;
        case CTokenType::FunctionCall:
            return ArColor::SrcFunction;
        case CTokenType::Operator:
            return ArColor::SrcOperator;
        case CTokenType::Punctuation:
            return ArColor::SrcPunct;
        case CTokenType::Default:
        default:
            return ArColor::Default;
    }
}

} // namespace adm::core
