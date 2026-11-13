#include "RetroKeyNames.h"

#include <cctype>
#include <iterator>
#include <unordered_map>

namespace adm::core {

namespace {

struct KeyName
{
    const char* name;
    int code;
};

// Mirrors AmigaEmulator::RetroKey. Codes are RETROK_* values; the enum is not included
// here so this table stays usable from the MCP layer without pulling in the emulator.
const KeyName kKeyNames[] = {
    {"Backspace", 8},
    {"Tab", 9},
    {"Clear", 12},
    {"Return", 13},
    {"Pause", 19},
    {"Escape", 27},
    {"Space", 32},
    {"Exclaim", 33},
    {"DoubleQuote", 34},
    {"Hash", 35},
    {"Dollar", 36},
    {"Ampersand", 38},
    {"Quote", 39},
    {"LeftParen", 40},
    {"RightParen", 41},
    {"Asterisk", 42},
    {"Plus", 43},
    {"Comma", 44},
    {"Minus", 45},
    {"Period", 46},
    {"Slash", 47},
    {"Num0", 48},
    {"Num1", 49},
    {"Num2", 50},
    {"Num3", 51},
    {"Num4", 52},
    {"Num5", 53},
    {"Num6", 54},
    {"Num7", 55},
    {"Num8", 56},
    {"Num9", 57},
    {"Colon", 58},
    {"Semicolon", 59},
    {"Less", 60},
    {"Equals", 61},
    {"Greater", 62},
    {"Question", 63},
    {"At", 64},
    {"LeftBracket", 91},
    {"Backslash", 92},
    {"RightBracket", 93},
    {"Caret", 94},
    {"Underscore", 95},
    {"Backquote", 96},
    {"A", 97},
    {"B", 98},
    {"C", 99},
    {"D", 100},
    {"E", 101},
    {"F", 102},
    {"G", 103},
    {"H", 104},
    {"I", 105},
    {"J", 106},
    {"K", 107},
    {"L", 108},
    {"M", 109},
    {"N", 110},
    {"O", 111},
    {"P", 112},
    {"Q", 113},
    {"R", 114},
    {"S", 115},
    {"T", 116},
    {"U", 117},
    {"V", 118},
    {"W", 119},
    {"X", 120},
    {"Y", 121},
    {"Z", 122},
    {"Delete", 127},
    {"Kp0", 256},
    {"Kp1", 257},
    {"Kp2", 258},
    {"Kp3", 259},
    {"Kp4", 260},
    {"Kp5", 261},
    {"Kp6", 262},
    {"Kp7", 263},
    {"Kp8", 264},
    {"Kp9", 265},
    {"KpPeriod", 266},
    {"KpDivide", 267},
    {"KpMultiply", 268},
    {"KpMinus", 269},
    {"KpPlus", 270},
    {"KpEnter", 271},
    {"KpEquals", 272},
    {"Up", 273},
    {"Down", 274},
    {"Right", 275},
    {"Left", 276},
    {"Insert", 277},
    {"Home", 278},
    {"End", 279},
    {"PageUp", 280},
    {"PageDown", 281},
    {"F1", 282},
    {"F2", 283},
    {"F3", 284},
    {"F4", 285},
    {"F5", 286},
    {"F6", 287},
    {"F7", 288},
    {"F8", 289},
    {"F9", 290},
    {"F10", 291},
    {"F11", 292},
    {"F12", 293},
    {"F13", 294},
    {"F14", 295},
    {"F15", 296},
    {"NumLock", 300},
    {"CapsLock", 301},
    {"ScrollLock", 302},
    {"RShift", 303},
    {"LShift", 304},
    {"RCtrl", 305},
    {"LCtrl", 306},
    {"RAlt", 307},
    {"LAlt", 308},
    {"RMeta", 309},
    {"LMeta", 310},
    {"LSuper", 311},
    {"RSuper", 312},
    {"Mode", 313},
    {"Compose", 314},
    {"Help", 315},
    {"Print", 316},
    {"SysReq", 317},
    {"Break", 318},
    {"Menu", 319},
    {"Power", 320},
    {"Euro", 321},
    {"Undo", 322},
};

// Lower-case and strip separators so "page up" / "Page_Up" / "PAGEUP" all match.
std::string Normalize(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '_' || c == '-')
            continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

const std::unordered_map<std::string, int>& NameIndex()
{
    static const std::unordered_map<std::string, int> index = [] {
        std::unordered_map<std::string, int> m;
        for (const auto& k : kKeyNames)
            m.emplace(Normalize(k.name), k.code);
        // Aliases for names callers reach for that differ from the canonical table.
        m.emplace("enter", 13);
        m.emplace("esc", 27);
        m.emplace("spacebar", 32);
        m.emplace("del", 127);
        m.emplace("ins", 277);
        m.emplace("pgup", 280);
        m.emplace("pgdn", 281);
        m.emplace("pgdown", 281);
        m.emplace("apostrophe", 39);
        m.emplace("grave", 96);
        m.emplace("numpadenter", 271);
        for (int d = 0; d <= 9; ++d)
            m.emplace(std::string(1, static_cast<char>('0' + d)), 48 + d);
        return m;
    }();
    return index;
}

} // namespace

bool LookupRetroKey(const std::string& name, int& outCode)
{
    const auto& index = NameIndex();
    auto it = index.find(Normalize(name));
    if (it == index.end())
        return false;
    outCode = it->second;
    return true;
}

const char* RetroKeyName(int code)
{
    for (const auto& k : kKeyNames) {
        if (k.code == code)
            return k.name;
    }
    return nullptr;
}

std::vector<std::string> AllRetroKeyNames()
{
    std::vector<std::string> out;
    out.reserve(std::size(kKeyNames));
    for (const auto& k : kKeyNames)
        out.emplace_back(k.name);
    return out;
}

bool AsciiToKeyStroke(char c, KeyStroke& out)
{
    // Letters: the RetroKey code is the lower-case letter; upper case adds Shift.
    if (c >= 'a' && c <= 'z') {
        out = {97 + (c - 'a'), false};
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        out = {97 + (c - 'A'), true};
        return true;
    }
    if (c >= '0' && c <= '9') {
        out = {48 + (c - '0'), false};
        return true;
    }

    // US layout, which is what PuAE's default keyboard translation assumes.
    switch (c) {
        case ' ':
            out = {32, false};
            return true;
        case '\n':
            out = {13, false};
            return true;
        case '\r':
            out = {13, false};
            return true;
        case '\t':
            out = {9, false};
            return true;
        case '\b':
            out = {8, false};
            return true;
        case '\'':
            out = {39, false};
            return true;
        case ',':
            out = {44, false};
            return true;
        case '-':
            out = {45, false};
            return true;
        case '.':
            out = {46, false};
            return true;
        case '/':
            out = {47, false};
            return true;
        case ';':
            out = {59, false};
            return true;
        case '=':
            out = {61, false};
            return true;
        case '[':
            out = {91, false};
            return true;
        case '\\':
            out = {92, false};
            return true;
        case ']':
            out = {93, false};
            return true;
        case '`':
            out = {96, false};
            return true;
        case '!':
            out = {49, true};
            return true; // Shift+1
        case '@':
            out = {50, true};
            return true; // Shift+2
        case '#':
            out = {51, true};
            return true; // Shift+3
        case '$':
            out = {52, true};
            return true; // Shift+4
        case '%':
            out = {53, true};
            return true; // Shift+5
        case '^':
            out = {54, true};
            return true; // Shift+6
        case '&':
            out = {55, true};
            return true; // Shift+7
        case '*':
            out = {56, true};
            return true; // Shift+8
        case '(':
            out = {57, true};
            return true; // Shift+9
        case ')':
            out = {48, true};
            return true; // Shift+0
        case '_':
            out = {45, true};
            return true;
        case '+':
            out = {61, true};
            return true;
        case '{':
            out = {91, true};
            return true;
        case '|':
            out = {92, true};
            return true;
        case '}':
            out = {93, true};
            return true;
        case ':':
            out = {59, true};
            return true;
        case '"':
            out = {39, true};
            return true;
        case '<':
            out = {44, true};
            return true;
        case '>':
            out = {46, true};
            return true;
        case '?':
            out = {47, true};
            return true;
        case '~':
            out = {96, true};
            return true;
        default:
            return false;
    }
}

bool LookupModifierKey(const std::string& name, int& outCode)
{
    static const std::unordered_map<std::string, int> mods = {
        {"shift", 304},   {"lshift", 304}, {"rshift", 303}, {"ctrl", 306},
        {"control", 306}, {"lctrl", 306},  {"rctrl", 305},  {"alt", 308},
        {"lalt", 308},    {"ralt", 307},   {"meta", 310},   {"lmeta", 310},
        {"rmeta", 309},   {"amiga", 310},  {"lamiga", 310}, {"ramiga", 309},
    };
    auto it = mods.find(Normalize(name));
    if (it == mods.end())
        return false;
    outCode = it->second;
    return true;
}

} // namespace adm::core
