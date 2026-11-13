#include "ArCommandInterpreter.h"
#include "HardfileInspector.h"

#include "AsmHighlighter.h"
#include "BitplaneDecoder.h"
#include "Constants.h"
#include "EmulatorOptions.h"
#include "MemoryAnnotations.h"
#include "MemoryLocks.h"
#include "MemwatchSource.h"
#include "PrivateFeatures.h"
#include "TrainerFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fmt/format.h>

namespace adm::core {

namespace {

std::string Trim(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

std::string ToUpper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool IsHexDigit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// Parses exactly n hex chars at line[pos]; returns nullopt on any non-hex char.
std::optional<uint32_t> ParseHexAt(const std::string& line, size_t pos, size_t n)
{
    if (pos + n > line.size())
        return std::nullopt;
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = line[pos + i];
        if (!IsHexDigit(c))
            return std::nullopt;
        v = (v << 4) | static_cast<uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

char PrintableChar(uint8_t b) { return (b >= 32 && b < 127) ? static_cast<char>(b) : '.'; }

bool LooksLikeRegPair(const std::string& s, size_t pos)
{
    // (D|A)[0-7]= or PC=/SR= at pos, followed by at least one hex digit
    if (pos + 3 > s.size())
        return false;
    char c0 = static_cast<char>(std::toupper(static_cast<unsigned char>(s[pos])));
    char c1 = static_cast<char>(std::toupper(static_cast<unsigned char>(s[pos + 1])));
    bool reg = ((c0 == 'D' || c0 == 'A') && c1 >= '0' && c1 <= '7') || (c0 == 'P' && c1 == 'C') ||
               (c0 == 'S' && c1 == 'R');
    return reg && pos + 2 < s.size() && s[pos + 2] == '=' && pos + 3 < s.size() &&
           IsHexDigit(s[pos + 3]);
}

// "USP=00C7F000 ..." - an R/RC/RM line of named registers beyond D/A/PC/SR. FPU registers are
// display-only, so an RF line stays a plain line.
bool LooksLikeExtRegLine(const std::string& s)
{
    const size_t eq = s.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= s.size() || !IsHexDigit(s[eq + 1]))
        return false;
    const std::string name = s.substr(0, eq);
    if (name.find(' ') != std::string::npos)
        return false;
    const CpuRegDef* def = FindCpuRegister(name);
    return def && def->group != CpuRegGroup::Fpu;
}

constexpr int kHexFieldCol = 10; // first hex digit of word group 0
constexpr int kAsciiCol = 51;    // ascii tail in M lines
constexpr int kScrollLinesPerTick = 32;
constexpr int kSearchMatchCap = 256;
constexpr int kCompareDiffCap = 100;
constexpr int kCopperBlock = 16;
constexpr uint32_t kCustomBase = ArMemoryAccess::kCustomBase;

// Amiga Exec structure offsets live in ExecIntrospect.h so the console, the MCP
// tools and the resident scanner all read the same numbers. Pulled into scope
// here so the command bodies below stay unchanged.
using namespace adm::core::exec; // NOLINT(google-build-using-namespace)

constexpr int kExecListCap = exec::kListCap; // safety bound for list walks

uint32_t Crc32Mem(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return ~crc;
}

uint16_t Crc16Mem(const uint8_t* data, size_t len)
{
    // CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection.
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

// Recursive-descent evaluator for ? expressions. Supports + - * / and
// parentheses with standard precedence; numbers use ParseNumber prefixes
// ($/0x hex default, ! decimal, % binary). 32-bit wrapping arithmetic.
struct ExprEval
{
    const std::string& s;
    size_t i = 0;
    bool ok = true;

    void skip()
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
    }
    uint32_t number()
    {
        skip();
        size_t start = i;
        while (i < s.size()) {
            char c = s[i];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '$' || c == '%' || c == '!')
                ++i;
            else
                break;
        }
        if (i == start) {
            ok = false;
            return 0;
        }
        auto v = ArCommandInterpreter::ParseNumber(s.substr(start, i - start));
        if (!v) {
            ok = false;
            return 0;
        }
        return *v;
    }
    uint32_t factor()
    {
        skip();
        if (i < s.size() && s[i] == '(') {
            ++i;
            uint32_t v = expr();
            skip();
            if (i < s.size() && s[i] == ')')
                ++i;
            else
                ok = false;
            return v;
        }
        return number();
    }
    uint32_t term()
    {
        uint32_t v = factor();
        for (;;) {
            skip();
            if (i < s.size() && (s[i] == '*' || s[i] == '/')) {
                char op = s[i++];
                uint32_t r = factor();
                if (op == '*') {
                    v *= r;
                } else if (r == 0) {
                    ok = false;
                    return 0;
                } else {
                    v /= r;
                }
            } else {
                break;
            }
        }
        return v;
    }
    uint32_t expr()
    {
        uint32_t v = term();
        for (;;) {
            skip();
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                char op = s[i++];
                uint32_t r = term();
                if (op == '+')
                    v += r;
                else
                    v -= r;
            } else {
                break;
            }
        }
        return v;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

std::optional<uint32_t> ArCommandInterpreter::ParseNumber(const std::string& token)
{
    std::string t = Trim(token);
    if (t.empty())
        return std::nullopt;

    int base = 16;
    if (t[0] == '$') {
        t.erase(0, 1);
    } else if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        t.erase(0, 2);
    } else if (t[0] == '!') {
        base = 10;
        t.erase(0, 1);
    } else if (t[0] == '%') {
        base = 2;
        t.erase(0, 1);
    }
    if (t.empty())
        return std::nullopt;

    try {
        size_t parsed = 0;
        unsigned long v = std::stoul(t, &parsed, base);
        if (parsed != t.size() || v > 0xFFFFFFFFul)
            return std::nullopt;
        return static_cast<uint32_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::string> ArCommandInterpreter::Tokenize(const std::string& s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= s.size())
            break;

        std::string token;
        bool quoted = false;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') {
                quoted = !quoted;
                continue;
            }
            if (c == '\\' && quoted && i < s.size() && (s[i] == '"' || s[i] == '\\')) {
                token.push_back(s[i++]);
                continue;
            }
            if (!quoted && std::isspace(static_cast<unsigned char>(c)))
                break;
            token.push_back(c);
        }
        out.push_back(std::move(token));
    }
    return out;
}

bool ArCommandInterpreter::SplitAtLastComma(const std::string& s, std::string& left,
                                            std::string& right)
{
    bool inQuotes = false;
    size_t lastComma = std::string::npos;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"')
            inQuotes = !inQuotes;
        else if (s[i] == ',' && !inQuotes)
            lastComma = i;
    }
    if (lastComma == std::string::npos)
        return false;
    left = Trim(s.substr(0, lastComma));
    right = Trim(s.substr(lastComma + 1));
    return true;
}

bool ArCommandInterpreter::ParsePattern(const std::string& s, std::vector<uint8_t>& out)
{
    out.clear();
    std::string t = Trim(s);
    if (t.empty())
        return false;

    if (t[0] == '"') {
        size_t close = t.find('"', 1);
        if (close == std::string::npos)
            close = t.size();
        for (size_t i = 1; i < close; ++i)
            out.push_back(static_cast<uint8_t>(t[i]));
        return !out.empty();
    }

    // Hex byte list: "DE AD BE EF" or "DEAD BEEF"
    for (const auto& tok : Tokenize(t)) {
        std::string h = tok;
        if (!h.empty() && h[0] == '$')
            h.erase(0, 1);
        if (h.empty() || h.size() % 2 != 0) {
            if (h.size() == 1)
                h.insert(h.begin(), '0');
            else
                return false;
        }
        for (char c : h) {
            if (!IsHexDigit(c))
                return false;
        }
        for (size_t i = 0; i + 1 < h.size(); i += 2) {
            auto v = ParseHexAt(h, i, 2);
            if (!v)
                return false;
            out.push_back(static_cast<uint8_t>(*v));
        }
    }
    return !out.empty();
}

std::string ArCommandInterpreter::FormatMemHexText(uint32_t addr, const uint8_t* bytes,
                                                   const std::vector<bool>& valid)
{
    std::string out = fmt::format("{:08X}: ", addr);
    for (int k = 0; k < kMemBytesPerLine / 2; ++k) {
        for (int b = 0; b < 2; ++b) {
            int idx = k * 2 + b;
            bool ok = idx < static_cast<int>(valid.size()) && valid[idx];
            if (ok)
                out += fmt::format("{:02X}", bytes[idx]);
            else
                out += "??";
        }
        out.push_back(' ');
    }
    out.push_back(' ');
    for (int i = 0; i < kMemBytesPerLine; ++i) {
        bool ok = i < static_cast<int>(valid.size()) && valid[i];
        out.push_back(ok ? PrintableChar(bytes[i]) : '.');
    }
    return out;
}

bool ArCommandInterpreter::ParseMemHexLine(const std::string& line, uint32_t& addr,
                                           uint8_t bytes[kMemBytesPerLine],
                                           bool provided[kMemBytesPerLine], char* asciiChars,
                                           bool* asciiPresent)
{
    if (line.size() < static_cast<size_t>(kHexFieldCol + 39)) // 8 groups: 8*5-1 = 39 chars
        return false;
    auto a = ParseHexAt(line, 0, 8);
    if (!a || line[8] != ':' || line[9] != ' ')
        return false;

    for (int k = 0; k < kMemBytesPerLine / 2; ++k) {
        size_t groupPos = static_cast<size_t>(kHexFieldCol + k * 5);
        // separator between groups must be a space
        if (k > 0 && line[groupPos - 1] != ' ')
            return false;
        for (int b = 0; b < 2; ++b) {
            int idx = k * 2 + b;
            size_t pos = groupPos + static_cast<size_t>(b * 2);
            if (line[pos] == '?' && line[pos + 1] == '?') {
                bytes[idx] = 0;
                provided[idx] = false;
                continue;
            }
            auto v = ParseHexAt(line, pos, 2);
            if (!v)
                return false;
            bytes[idx] = static_cast<uint8_t>(*v);
            provided[idx] = true;
        }
    }

    // ASCII tail (may be truncated: trailing whitespace gets trimmed)
    if (asciiChars && asciiPresent) {
        for (int i = 0; i < kMemBytesPerLine; ++i) {
            size_t pos = static_cast<size_t>(kAsciiCol + i);
            asciiPresent[i] = pos < line.size();
            asciiChars[i] = asciiPresent[i] ? line[pos] : ' ';
        }
    }

    addr = *a;
    return true;
}

std::string ArCommandInterpreter::FormatMemAsciiText(uint32_t addr, const uint8_t* bytes,
                                                     const std::vector<bool>& valid,
                                                     int64_t displayOffset)
{
    std::string out = fmt::format("{:08X}: \"", addr);
    for (int i = 0; i < kMemBytesPerLine; ++i) {
        bool ok = i < static_cast<int>(valid.size()) && valid[i];
        uint8_t shown = static_cast<uint8_t>((bytes[i] + displayOffset) & 0xFF);
        out.push_back(ok ? PrintableChar(shown) : '.');
    }
    out.push_back('"');
    return out;
}

bool ArCommandInterpreter::ParseMemAsciiLine(const std::string& line, uint32_t& addr,
                                             char chars[kMemBytesPerLine])
{
    // AAAAAAAA: "cccccccccccccccc"
    if (line.size() < static_cast<size_t>(kHexFieldCol + kMemBytesPerLine + 2))
        return false;
    auto a = ParseHexAt(line, 0, 8);
    if (!a || line[8] != ':' || line[9] != ' ' || line[10] != '"')
        return false;
    if (line[kHexFieldCol + 1 + kMemBytesPerLine] != '"')
        return false;
    for (int i = 0; i < kMemBytesPerLine; ++i)
        chars[i] = line[static_cast<size_t>(kHexFieldCol + 1 + i)];
    addr = *a;
    return true;
}

std::string ArCommandInterpreter::FormatMemBinText(uint32_t addr, const uint8_t* bytes,
                                                   const std::vector<bool>& valid, int count)
{
    std::string out = fmt::format("{:08X}: ", addr);
    for (int i = 0; i < count; ++i) {
        if (i)
            out.push_back(' ');
        bool ok = i < static_cast<int>(valid.size()) && valid[i];
        if (!ok) {
            out += "????????";
            continue;
        }
        for (int b = 7; b >= 0; --b)
            out.push_back((bytes[i] & (1u << b)) ? '1' : '0');
    }
    return out;
}

bool ArCommandInterpreter::ParseMemBinLine(const std::string& line, uint32_t& addr,
                                           uint8_t bytes[kMemBinMaxBytes], int& count)
{
    // AAAAAAAA: bbbbbbbb bbbbbbbb ... (single space between 8-bit groups)
    if (line.size() < static_cast<size_t>(kHexFieldCol + 8))
        return false;
    auto a = ParseHexAt(line, 0, 8);
    if (!a || line[8] != ':' || line[9] != ' ')
        return false;

    count = 0;
    size_t pos = kHexFieldCol;
    while (count < kMemBinMaxBytes && pos + 8 <= line.size()) {
        uint8_t v = 0;
        for (int b = 0; b < 8; ++b) {
            char c = line[pos + static_cast<size_t>(b)];
            if (c != '0' && c != '1')
                return false;
            v = static_cast<uint8_t>((v << 1) | (c - '0'));
        }
        bytes[count++] = v;
        pos += 8;
        if (pos >= line.size())
            break;
        if (line[pos] != ' ') // groups must be single-space separated
            return false;
        ++pos;
    }
    if (count == 0)
        return false;
    addr = *a;
    return true;
}

ArCommandInterpreter::LineKind ArCommandInterpreter::ClassifyLine(const std::string& raw)
{
    std::string line = raw;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    if (Trim(line).empty())
        return LineKind::Blank;

    // Comment line (';' prefix, e.g. a code-section separator): no-op on re-parse.
    if (Trim(line).front() == ';')
        return LineKind::Blank;

    // Addressed line? "AAAAAAAA: ..."
    if (line.size() > 10 && ParseHexAt(line, 0, 8) && line[8] == ':' && line[9] == ' ') {
        uint32_t addr;
        uint8_t bytes[kMemBytesPerLine];
        bool provided[kMemBytesPerLine];
        if (ParseMemHexLine(line, addr, bytes, provided))
            return LineKind::MemHexEdit;
        char chars[kMemBytesPerLine];
        if (ParseMemAsciiLine(line, addr, chars))
            return LineKind::MemAsciiEdit;
        uint8_t binBytes[kMemBinMaxBytes];
        int binCount = 0;
        if (ParseMemBinLine(line, addr, binBytes, binCount))
            return LineKind::MemBinEdit;
        return LineKind::AddressedListing; // disasm or copper output
    }

    // Custom register line? "DFFxxx NAME ... = vvvv"
    if (line.size() >= 6) {
        std::string head = ToUpper(line.substr(0, 3));
        if (head == "DFF" && ParseHexAt(line, 3, 3) && line.find(" = ") != std::string::npos &&
            line.size() > 6 && line[6] == ' ')
            return LineKind::CustomRegEdit;
    }

    // Register line? contains Xn=hex pairs
    for (size_t i = 0; i + 3 < line.size(); ++i) {
        if (LooksLikeRegPair(line, i))
            return LineKind::RegisterEdit;
    }
    if (LooksLikeExtRegLine(line))
        return LineKind::RegisterEdit;

    return LineKind::Command;
}

// ---------------------------------------------------------------------------
// Construction / output
// ---------------------------------------------------------------------------

ArCommandInterpreter::ArCommandInterpreter(ArMemoryAccess* mem, ArHost host)
    : m_mem(mem), m_host(std::move(host))
{
}

void ArCommandInterpreter::Emit(const ArLine& line)
{
    if (emitLine)
        emitLine(line);
}

void ArCommandInterpreter::Emit(const std::string& text, ArColor color)
{
    Emit(color == ArColor::Default ? ArLine(text) : ArLine(text, color));
}

void ArCommandInterpreter::Error(const std::string& text) { Emit(text, ArColor::Error); }

void ArCommandInterpreter::Info(const std::string& text) { Emit(text, ArColor::Info); }

void ArCommandInterpreter::EmitTableHead(const std::string& header)
{
    Emit(header, ArColor::Highlight);
    Emit(std::string(header.size(), '-'));
}

void ArCommandInterpreter::RewriteCurrent(const ArLine& line)
{
    if (rewriteLine)
        rewriteLine(line);
    else
        Emit(line);
}

bool ArCommandInterpreter::RequireRunning()
{
    if (!m_host.isRunning || !m_host.isRunning()) {
        Error("Error: emulator not running");
        return false;
    }
    return true;
}

void ArCommandInterpreter::StateChanged()
{
    if (m_host.onStateChanged)
        m_host.onStateChanged();
}

int ArCommandInterpreter::PreferredCursorColumn() const
{
    switch (m_mode) {
        case Mode::MemHex:
            return kHexFieldCol;
        case Mode::MemAscii:
            return kHexFieldCol + 1;
        case Mode::MemBin:
            return kHexFieldCol;
        case Mode::CustomRegs:
            return 22; // value column
        case Mode::Assemble:
            return Disassembler::kMnemonicColumn;
        case Mode::Disasm:
        case Mode::CopperList:
            return 0;
        default:
            return -1; // fresh blank line
    }
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void ArCommandInterpreter::ExecuteLine(const std::string& lineText)
{
    const LineKind kind = ClassifyLine(lineText);

    if (m_mode == Mode::Assemble && kind != LineKind::Blank) {
        HandleAssembleLine(Trim(lineText));
        return;
    }

    switch (kind) {
        case LineKind::Blank:
            // While stepping a listing, Return on a blank or ';====' separator line
            // continues it — otherwise the section separators would halt step-by-step
            // disassembly when the cursor lands on them.
            if (m_mode == Mode::Disasm || m_mode == Mode::CopperList)
                HandleAddressedListing();
            break;
        case LineKind::Command:
            HandleCommand(Trim(lineText));
            break;
        case LineKind::MemHexEdit:
            HandleMemHexEdit(lineText);
            break;
        case LineKind::MemAsciiEdit:
            HandleMemAsciiEdit(lineText);
            break;
        case LineKind::MemBinEdit:
            HandleMemBinEdit(lineText);
            break;
        case LineKind::RegisterEdit:
            HandleRegisterEdit(lineText);
            break;
        case LineKind::CustomRegEdit:
            HandleCustomRegEdit(lineText);
            break;
        case LineKind::AddressedListing:
            HandleAddressedListing();
            break;
    }
}

void ArCommandInterpreter::CancelInteractive()
{
    if (m_mode != Mode::None)
        Info("Cancelled.");
    m_mode = Mode::None;
    m_endAddr.reset();
}

void ArCommandInterpreter::Tick()
{
    if (!IsScrolling())
        return;
    const bool ascii = (m_mode == Mode::ScrollAscii);
    for (int i = 0; i < kScrollLinesPerTick; ++i) {
        if (m_curAddr > m_scrollEnd) {
            m_mode = Mode::None;
            return;
        }
        if (ascii)
            EmitMemAsciiLine(m_curAddr);
        else
            EmitMemHexLine(m_curAddr);
        // EmitMem*Line advances m_curAddr; stop on wrap
        if (m_curAddr == 0) {
            m_mode = Mode::None;
            return;
        }
    }
}

void ArCommandInterpreter::OnDebugBreak()
{
    if (m_tempTraceBp) {
        if (m_host.removeBreakpoint)
            m_host.removeBreakpoint(*m_tempTraceBp);
        m_tempTraceBp.reset();
        StateChanged();
    }

    // FB trap: announce the bootblock entry before the standard break banner.
    const bool fbHit = m_host.consumeBootblockHit && m_host.consumeBootblockHit();

    // Exception / beam breaks: read-clear which (if either) caused this break.
    std::optional<int> excVec;
    if (m_host.consumeExceptionHit)
        excVec = m_host.consumeExceptionHit();
    const bool beamHit = m_host.consumeBeamHit && m_host.consumeBeamHit();

    // Disk DMA trap: read-clear the transfer that stopped us (if this break was one).
    std::optional<ArHost::DiskBreakHit> diskHit;
    if (m_host.consumeDiskHit)
        diskHit = m_host.consumeDiskHit();

    // Memory watchpoint: read-clear the triggering access (if this break was one).
    std::optional<ArHost::WatchpointHit> watchHit;
    if (m_host.consumeWatchpointHit)
        watchHit = m_host.consumeWatchpointHit();

    // KPrintF text trap: read-clear the debug-output line that stopped us (if this break was one).
    std::optional<KprintfHit> kprintfHit;
    if (m_host.consumeKprintfHit)
        kprintfHit = m_host.consumeKprintfHit();

    ArCpuState st;
    if (!m_host.getCpuState || !m_host.getCpuState(st))
        return;

    Emit("");
    if (fbHit) {
        const uint32_t entry = m_host.bootblockEntry ? m_host.bootblockEntry() : st.pc;
        Emit(fmt::format("** BOOTBLOCK ENTRY @ {:06X} **", entry), ArColor::Highlight);
    }
    if (excVec) {
        Emit(fmt::format("*** EXCEPTION {} ({}) ***", *excVec, exec::VectorName(*excVec)),
             ArColor::Highlight);
    }
    if (beamHit) {
        Emit("*** BEAM POSITION REACHED ***", ArColor::Highlight);
    }
    if (diskHit) {
        Emit(fmt::format("*** DISK DMA {} df{}: cyl {} side {} -> ${:06X} ({} bytes) ***",
                         diskHit->isWrite ? "WRITE" : "READ", diskHit->drive, diskHit->cyl,
                         diskHit->side, diskHit->dskpt, diskHit->bytes),
             ArColor::Highlight);
        // The break fires as DSKLEN is written, so this is the arming instruction itself.
        Emit("    armed by:");
        EmitDisasmLine(diskHit->pc);
    }
    if (kprintfHit) {
        // Matching is line-based, so the PC below is wherever the program got to by the end of
        // the KPrintF line — not the instruction inside KPrintF itself.
        Emit(fmt::format("*** KPRINTF MATCH \"{}\" ***", kprintfHit->text), ArColor::Highlight);
        Emit(fmt::format("    {}", kprintfHit->line));
    }
    if (watchHit) {
        // A data watchpoint stops *after* the access completes, so st.pc is the next
        // instruction. Name the access and the agent that caused it.
        const char sz = watchHit->size == 1 ? 'B' : (watchHit->size == 2 ? 'W' : 'L');
        const int digits = watchHit->size == 1 ? 2 : (watchHit->size == 2 ? 4 : 8);
        // For a Copper/Blitter/DMA access, watchHit->pc is the unrelated CPU instruction
        // that happened to be running — not the writer. Name the real agent instead and
        // skip disassembling that PC as if it performed the write.
        const char* src = core::MemwatchSourceName(watchHit->accessMask);
        const bool cpuAccess = (watchHit->accessMask == 0) || (watchHit->accessMask & 0x7u);
        if (watchHit->rwi & 2) {
            Emit(fmt::format("*** WATCH {:08X}.{} WROTE {:0{}X} by {} ***", watchHit->addr, sz,
                             watchHit->val, digits, src ? src : "CPU"),
                 ArColor::Highlight);
        } else {
            Emit(fmt::format("*** WATCH {:08X}.{} READ by {} ***", watchHit->addr, sz,
                             src ? src : "CPU"),
                 ArColor::Highlight);
        }
        if (cpuAccess) {
            EmitDisasmLine(watchHit->pc); // the CPU instruction that performed the access
        } else {
            // The CPU was mid-instruction elsewhere; show it as context, not as the cause.
            Emit(fmt::format("    (CPU was at {:08X})", watchHit->pc));
        }
    }
    Emit(fmt::format("*** BREAK AT {:08X} ***", st.pc), ArColor::Highlight);
    EmitRegisters(st);
    EmitDisasmAtPc();
}

// ---------------------------------------------------------------------------
// Line-kind handlers
// ---------------------------------------------------------------------------

void ArCommandInterpreter::HandleCommand(const std::string& line)
{
    if (line.empty())
        return;

    // A new command always leaves the previous interactive mode.
    m_mode = Mode::None;
    m_endAddr.reset();

    size_t sp = line.find_first_of(" \t");
    const std::string cmd = ToUpper(line.substr(0, sp));
    const std::string args = (sp == std::string::npos) ? "" : Trim(line.substr(sp + 1));

    // clang-format off
    if      (cmd == "M")       CmdMem(args, 1, false);
    else if (cmd == "MM")      CmdMem(args, 8, false);
    else if (cmd == "MMM")     CmdMem(args, 16, false);
    else if (cmd == "N")       CmdMem(args, 1, true);
    else if (cmd == "NN")      CmdMem(args, 8, true);
    else if (cmd == "NNN")     CmdMem(args, 16, true);
    else if (cmd == "NO")      CmdAsciiOffset(args);
    else if (cmd == "Y")       CmdMemBin(args, 1);
    else if (cmd == "YY")      CmdMemBin(args, 8);
    else if (cmd == "YYY")     CmdMemBin(args, 16);
    else if (cmd == "YS")      CmdBinSize(args);
    else if (cmd == "MQ")      CmdMemQuick(args, false);
    else if (cmd == "NQ")      CmdMemQuick(args, true);
    else if (cmd == "D")       CmdDisasm(args, 1);
    else if (cmd == "DD")      CmdDisasm(args, 8);
    else if (cmd == "DDD")     CmdDisasm(args, 16);
    else if (cmd == "R")       CmdRegisters(args);
    else if (cmd == "RC")      CmdCpuExtRegs('C');
    else if (cmd == "RF")      CmdCpuExtRegs('F');
    else if (cmd == "RM")      CmdCpuExtRegs('M');
    else if (cmd == "E")       CmdCustomRegs(args);
    else if (cmd == "C")       CmdCopper(args);
    else if (cmd == "G")       CmdGo(args);
    else if (cmd == "X")       CmdGo("");
    else if (cmd == "STOP")    CmdStop(args);
    else if (cmd == "START")   CmdStart(args);
    else if (cmd == "TURBO")   CmdTurbo(args);
    else if (cmd == "WARP")    CmdTurbo(args);
    else if (cmd == "SF")      CmdStepFrame(args);
    else if (cmd == "STATE")   CmdState(args);
    else if (cmd == "STATES")  CmdState(args);
    else if (cmd == "EF")      CmdEndFrame(args);
    else if (cmd == "RUNTO")   CmdRunUntilFrame(args);
    else if (cmd == "PAUSE")   CmdPause(args);
    else if (cmd == "S")       CmdStep(args, false);
    else if (cmd == "ST")      CmdStep(args, false);
    else if (cmd == "TR")      CmdStep(args, true);
    else if (cmd == "TA")      CmdTraceUntil(args);
    else if (cmd == "BS")      CmdBreakSet(args);
    else if (cmd == "BD")      CmdBreakDelete(args);
    else if (cmd == "B")       CmdBreakList(args);
    else if (cmd == "BDA")     CmdBreakDeleteAll(args);
    else if (cmd == "BKS")     CmdKprintfSet(args);
    else if (cmd == "BKD")     CmdKprintfDelete(args);
    else if (cmd == "BK")      CmdKprintfList(args);
    else if (cmd == "BKDA")    CmdKprintfDeleteAll(args);
    else if (cmd == "MS")      CmdWatchSet(args);
    else if (cmd == "MD")      CmdWatchDelete(args);
    else if (cmd == "MW")      CmdWatchList(args);
    else if (cmd == "MDA")     CmdWatchDeleteAll(args);
    else if (cmd == "F")       CmdFind(args, false);
    else if (cmd == "FS")      CmdFind(args, true);
    else if (cmd == "FR")      CmdFindRelative(args);
    else if (cmd == "FA")      CmdFindAccess(args, false);
    else if (cmd == "FAQ")     CmdFindAccess(args, true);
    else if (cmd == "COMP")    CmdCompare(args);
    else if (cmd == "TRANS")   CmdTransfer(args);
    else if (cmd == "EX")      CmdExchange(args);
    else if (cmd == "O")       CmdFill(args);
    else if (cmd == "WS")      CmdWriteString(args);
    else if (cmd == "MEMCODE") CmdMemCode(args);
    else if (cmd == "ADD")     CmdAdd(args);
    else if (cmd == "LM")      CmdLoadMem(args);
    else if (cmd == "SM")      CmdSaveMem(args);
    else if (cmd == "A")       CmdAssemble(args);
    else if (cmd == "FB" || cmd == "TRB") CmdBreakBootblock(args);
    else if (cmd == "EB")      CmdExceptionBreak(args);
    else if (cmd == "BL")      CmdBeamBreak(args);
    else if (cmd == "BC")      CmdCopperBreak(args);
    else if (cmd == "PR" || cmd == "PROTECT") CmdProtect(args);
    else if (cmd == "TS")      CmdTrainerStart(args);
    else if (cmd == "T")       CmdTrainerContinue(args);
    else if (cmd == "TX")      CmdTrainerExit(args);
    else if (cmd == "TF")      CmdTrainerFind(args, /*remove=*/false);
    else if (cmd == "TFD")     CmdTrainerFind(args, /*remove=*/true);
    else if (cmd == "TDS")     CmdDeepStart(args);
    else if (cmd == "TDC")     CmdDeepChanged(args);
    else if (cmd == "TD")      CmdDeepList(args);
    else if (cmd == "TDI")     CmdDeepInspect(args);
    else if (cmd == "TDD")     CmdDeepDelete(args);
    else if (cmd == "TDX")     CmdDeepExit(args);
    else if (cmd == "LOAD" || cmd == "L")  CmdLoad(args);
    else if (cmd == "LIST")  CmdList(args);
    else if (cmd == "RESET") CmdReset(args);
    else if (cmd == "DOCS")  CmdDocs(args);
    else if (cmd == "HD")    CmdHardDrives(args);
    else if (cmd == "MODEL") CmdModel(args);
    else if (cmd == "WHD" || cmd == "WHDLOAD") CmdWhdload(args);
    else if (cmd == "CD") CmdCd(args);
    else if (cmd == "MACHINE") CmdMachine(args);
    else if (cmd == "SERIAL") CmdSerial(args);
    else if (cmd == "PARALLEL") CmdParallel(args);
    else if (cmd == "MIDI") CmdMidi(args);
    else if (cmd == "NETWORK") CmdNetwork(args);
    else if (cmd == "SOUNDCARD") CmdSoundCard(args);
    else if (cmd == "CLOSE") CmdCloseDoc(args);
    else if (cmd == "?")       { if (args.empty()) CmdHelp(args); else CmdCalc(args); }
    else if (cmd == "HELP" || cmd == "H") CmdHelp(args);
    else if (cmd == "KEYS")  CmdKeys(args);
    else if (cmd == "CLS" || cmd == "CLEAR") CmdClear(args);
    else if (cmd == "VERSION" || cmd == "VER" || cmd == "V") CmdVersion(args);
    else if (cmd == "FRAME") CmdFrame(args);
    else if (cmd == "HISTORY" || cmd == "HIST") CmdHistory(args);
    else if (cmd == "REWIND" || cmd == "RW") CmdRewind(args);
    else if (cmd == "GOTO") CmdGoto(args);
    else if (cmd == "DIFF") CmdFrameDiff(args);
    else if (cmd == "LOOP") CmdLoop(args);
    else if (cmd == "RT")      CmdReadTrack(args);
    else if (cmd == "RS")      CmdReadSectors(args);
    else if (cmd == "RB")      CmdReadBytes(args);
    else if (cmd == "WT")      CmdWriteTrack(args);
    else if (cmd == "WDS")     CmdWriteSectors(args);
    else if (cmd == "WDB")     CmdWriteBytes(args);
    else if (cmd == "BOOTCHK") CmdBootCheck(args);
    else if (cmd == "DATACHK") CmdDataCheck(args);
    else if (cmd == "BAMCHK")  CmdBamCheck(args);
    else if (cmd == "CRC16")   CmdCrc(args, false);
    else if (cmd == "CRC32")   CmdCrc(args, true);
    else if (cmd == "W")       CmdCia(args);
    else if (cmd == "PALETTE" || cmd == "PAL") CmdPalette(args);
    else if (cmd == "STACK")   CmdStack(args);
    else if (cmd == "CALL")    CmdCall(args);
    else if (cmd == "PROFILE" || cmd == "PROF") CmdProfile(args);
    else if (cmd == "CALLSTACK" || cmd == "CS") CmdCallStack(args);
    else if (cmd == "BLIT" || cmd == "BLITLOG") CmdBlitLog(args);
    else if (cmd == "REGLOG" || cmd == "REGS") CmdRegLog(args);
    else if (cmd == "AUDIO" || cmd == "AUD") CmdAudioStats(args);
    else if (cmd == "AUDREC") CmdAudioRecord(args);
    else if (cmd == "SCEN" || cmd == "SCENARIO") CmdScenario(args);
    else if (cmd == "PAULA") CmdPaulaState(args);
    else if (cmd == "DSKLOG") CmdDiskLog(args);
    else if (cmd == "LIVE" || cmd == "FLOPPYBRIDGE") CmdFloppyBridge(args);
    else if (cmd == "BDSK") CmdDiskBreak(args);
    else if ((cmd == "EXO" || cmd == "EXOCET") && PrivateFeatures()) CmdExocet(args);
    else if (cmd == "BPL" || cmd == "BITPLANES") CmdDisplayMask(args, kBitplaneGroup);
    else if (cmd == "SPR" || cmd == "SPRITES")   CmdDisplayMask(args, kSpriteGroup);
    else if (cmd == "PF"  || cmd == "PLAYFIELDS") CmdDisplayMask(args, kPlayfieldGroup);
    else if (cmd == "DISPLAY" || cmd == "DISP")  CmdDisplay(args);
    else if (cmd == "TRACE")   CmdTrace(args);
    else if (cmd == "COV" || cmd == "COVERAGE") CmdCoverage(args);
    else if (cmd == "EXECBASE")   CmdExecBase(args);
    else if (cmd == "AVAIL")      CmdAvail(args);
    else if (cmd == "LIBRARIES" || cmd == "LIBS") CmdLibraries(args);
    else if (cmd == "RESOURCES")  CmdResources(args);
    else if (cmd == "DEVICES")    CmdDevices(args);
    else if (cmd == "TASKS")      CmdTasks(args);
    else if (cmd == "PORTS")      CmdPorts(args);
    else if (cmd == "INTERRUPTS" || cmd == "INTS") CmdInterrupts(args);
    else if (cmd == "EXCEPTIONS") CmdExceptions(args);
    else if (cmd == "CHIPREGS")   CmdChipRegs(args);
    else if (cmd == "DCHIP")      CmdDChip(args);
    else if (cmd == "KICKVER")    CmdKickVer(args);
    else if (cmd == "TM")      CmdTagMem(args);
    else if (cmd == "TL")      CmdTagLabel(args);
    else if (cmd == "TMS")     CmdTagMemShow(args);
    else if (cmd == "TMD")     CmdTagMemDelete(args);
    else if (cmd == "TB")      CmdTagBlock(args);
    else if (cmd == "TBD")     CmdTagBlockDelete(args);
    else if (IsGluedTrainerCommand(cmd, args)) {} // "TS6"/"T5" already dispatched
    else Error("Error: unknown command (type help)");
    // clang-format on
}

// Manual examples glue the value to the command ("TS6", "T5", "T0"). Exact
// command names take precedence ("TF", "TD", "TA", ...), so this only fires
// for T/TS followed by digits once every other match has failed.
bool ArCommandInterpreter::IsGluedTrainerCommand(const std::string& cmd, const std::string& args)
{
    size_t prefix = 0;
    if (cmd.size() > 2 && cmd[0] == 'T' && cmd[1] == 'S')
        prefix = 2;
    else if (cmd.size() > 1 && cmd[0] == 'T')
        prefix = 1;
    else
        return false;
    const std::string glued = cmd.substr(prefix);
    if (!ParseNumber(glued))
        return false;
    const std::string fullArgs = args.empty() ? glued : glued + " " + args;
    if (prefix == 2)
        CmdTrainerStart(fullArgs);
    else
        CmdTrainerContinue(fullArgs);
    return true;
}

void ArCommandInterpreter::HandleMemHexEdit(const std::string& line)
{
    if (!RequireRunning())
        return;

    uint32_t addr;
    uint8_t edited[kMemBytesPerLine];
    bool provided[kMemBytesPerLine];
    char asciiChars[kMemBytesPerLine];
    bool asciiPresent[kMemBytesPerLine];
    if (!ParseMemHexLine(line, addr, edited, provided, asciiChars, asciiPresent)) {
        Error("Error: bad hex line");
        return;
    }

    uint8_t current[kMemBytesPerLine];
    std::vector<bool> valid;
    m_mem->Read(addr, current, kMemBytesPerLine, &valid);

    bool changedAny = false;
    for (int i = 0; i < kMemBytesPerLine; ++i) {
        if (!valid[i])
            continue;

        uint8_t newValue = current[i];
        if (provided[i] && edited[i] != current[i]) {
            // Hex column edit takes precedence
            newValue = edited[i];
        } else if (asciiPresent[i] && asciiChars[i] != '.' &&
                   asciiChars[i] != PrintableChar(current[i])) {
            // ASCII tail edit ('.' is the unprintable placeholder — ambiguous,
            // so treated as unchanged)
            newValue = static_cast<uint8_t>(asciiChars[i]);
        }
        if (newValue == current[i])
            continue;

        std::string err;
        if (!m_mem->Write(addr + static_cast<uint32_t>(i), &newValue, 1, err)) {
            Error("Error: " + err);
            return;
        }
        changedAny = true;
    }
    if (changedAny)
        StateChanged();

    // Refresh the executed line in place so both columns reflect memory
    RewriteCurrent(MakeMemHexArLine(addr));

    if (m_mode == Mode::MemHex) {
        uint32_t next = addr + kMemBytesPerLine;
        if ((m_endAddr && next > *m_endAddr) || next < addr) {
            m_mode = Mode::None;
            m_endAddr.reset();
        } else {
            EmitMemHexLine(next);
        }
    }
}

void ArCommandInterpreter::HandleMemAsciiEdit(const std::string& line)
{
    if (!RequireRunning())
        return;

    uint32_t addr;
    char chars[kMemBytesPerLine];
    if (!ParseMemAsciiLine(line, addr, chars)) {
        Error("Error: bad ascii line");
        return;
    }

    uint8_t current[kMemBytesPerLine];
    std::vector<bool> valid;
    m_mem->Read(addr, current, kMemBytesPerLine, &valid);

    bool changedAny = false;
    for (int i = 0; i < kMemBytesPerLine; ++i) {
        if (!valid[i])
            continue;
        char shown = PrintableChar(static_cast<uint8_t>((current[i] + m_asciiOffset) & 0xFF));
        if (chars[i] == shown)
            continue;
        if (chars[i] == '.')
            continue; // '.' is ambiguous (unprintable placeholder) — treat as unchanged
        uint8_t value =
            static_cast<uint8_t>((static_cast<uint8_t>(chars[i]) - m_asciiOffset) & 0xFF);
        std::string err;
        if (!m_mem->Write(addr + static_cast<uint32_t>(i), &value, 1, err)) {
            Error("Error: " + err);
            return;
        }
        changedAny = true;
    }
    if (changedAny)
        StateChanged();

    // Refresh the executed line in place
    RewriteCurrent(MakeMemAsciiArLine(addr));

    if (m_mode == Mode::MemAscii) {
        uint32_t next = addr + kMemBytesPerLine;
        if ((m_endAddr && next > *m_endAddr) || next < addr) {
            m_mode = Mode::None;
            m_endAddr.reset();
        } else {
            EmitMemAsciiLine(next);
        }
    }
}

void ArCommandInterpreter::HandleMemBinEdit(const std::string& line)
{
    if (!RequireRunning())
        return;

    uint32_t addr;
    uint8_t edited[kMemBinMaxBytes];
    int count = 0;
    if (!ParseMemBinLine(line, addr, edited, count)) {
        Error("Error: bad binary line");
        return;
    }

    uint8_t current[kMemBinMaxBytes];
    std::vector<bool> valid;
    m_mem->Read(addr, current, static_cast<size_t>(count), &valid);

    bool changedAny = false;
    for (int i = 0; i < count; ++i) {
        if (i >= static_cast<int>(valid.size()) || !valid[i])
            continue;
        if (edited[i] == current[i])
            continue;
        std::string err;
        if (!m_mem->Write(addr + static_cast<uint32_t>(i), &edited[i], 1, err)) {
            Error("Error: " + err);
            return;
        }
        changedAny = true;
    }
    if (changedAny)
        StateChanged();

    // Refresh the executed line in place (preserve the typed width)
    RewriteCurrent(MakeMemBinArLine(addr, count));

    if (m_mode == Mode::MemBin) {
        uint32_t next = addr + static_cast<uint32_t>(count);
        if ((m_endAddr && next > *m_endAddr) || next < addr) {
            m_mode = Mode::None;
            m_endAddr.reset();
        } else {
            EmitMemBinLine(next);
        }
    }
}

void ArCommandInterpreter::HandleRegisterEdit(const std::string& line)
{
    if (!RequireRunning())
        return;
    if (!m_host.getCpuState || !m_host.setCpuRegister) {
        Error("Error: register access unavailable");
        return;
    }

    ArCpuState st;
    if (!m_host.getCpuState(st)) {
        Error("Error: cannot read CPU state");
        return;
    }

    int appliedCount = 0;

    // Named registers beyond D/A/PC/SR ("USP=... VBR=..."), whole tokens only.
    if (LooksLikeExtRegLine(line)) {
        CpuExtendedState ext;
        if (!m_host.getCpuExtState || !m_host.getCpuExtState(ext)) {
            Error("Error: cannot read CPU state");
            return;
        }
        for (const auto& token : Tokenize(line)) {
            const size_t eq = token.find('=');
            if (eq == std::string::npos)
                continue;
            const CpuRegDef* def = FindCpuRegister(token.substr(0, eq));
            auto value = ParseHexAt(token, eq + 1, token.size() - eq - 1);
            if (!def || !value)
                continue;
            auto current = ReadCpuRegister(ext, *def);
            if (current && current->value == *value)
                continue;
            std::string error;
            if (SetExtRegister(ext, *def, *value, error))
                ++appliedCount;
            else
                Error(error);
        }
        if (appliedCount > 0)
            StateChanged();
        return;
    }

    for (size_t i = 0; i + 3 < line.size(); ++i) {
        if (!LooksLikeRegPair(line, i))
            continue;
        std::string reg = ToUpper(line.substr(i, 2));
        size_t valStart = i + 3;
        size_t valEnd = valStart;
        while (valEnd < line.size() && IsHexDigit(line[valEnd]))
            ++valEnd;
        auto value = ParseHexAt(line, valStart, valEnd - valStart);
        i = valEnd;
        if (!value)
            continue;

        uint32_t currentVal = 0;
        if (reg == "PC")
            currentVal = st.pc;
        else if (reg == "SR")
            currentVal = st.sr;
        else if (reg[0] == 'D')
            currentVal = st.d[reg[1] - '0'];
        else
            currentVal = st.a[reg[1] - '0'];

        if (*value == currentVal)
            continue;

        std::string lower = reg;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (m_host.setCpuRegister(lower, *value))
            ++appliedCount;
    }

    // No output on success: the typed values on the line are now the live
    // values, so the screen is already correct.
    if (appliedCount > 0)
        StateChanged();
}

void ArCommandInterpreter::HandleCustomRegEdit(const std::string& line)
{
    if (!RequireRunning())
        return;

    auto offset = ParseHexAt(line, 3, 3);
    size_t eq = line.find(" = ");
    if (!offset || eq == std::string::npos) {
        Error("Error: bad register line");
        return;
    }
    auto value = ParseNumber(Trim(line.substr(eq + 3)));
    if (!value || *value > 0xFFFF) {
        Error("Error: bad value");
        return;
    }

    uint32_t off = *offset & 0x1FE;
    bool ok = false;
    uint16_t current = m_mem->ReadWord(kCustomBase + off, &ok);
    if (!ok || current != static_cast<uint16_t>(*value)) {
        uint8_t bytes[2] = {static_cast<uint8_t>(*value >> 8), static_cast<uint8_t>(*value)};
        std::string err;
        if (!m_mem->Write(kCustomBase + off, bytes, 2, err)) {
            Error("Error: " + err);
            return;
        }
        StateChanged();
    }

    // Refresh the executed line in place (value re-read after the write)
    RewriteCurrent(MakeCustomRegArLine(off));

    if (m_mode == Mode::CustomRegs) {
        uint32_t next = (off + 2) & 0x1FE;
        if (next == 0) {
            m_mode = Mode::None; // wrapped past the last register
        } else {
            EmitCustomRegLine(next);
        }
    }
}

void ArCommandInterpreter::HandleAddressedListing()
{
    if (m_mode == Mode::Disasm) {
        if (!RequireRunning())
            return;
        if (m_endAddr && m_lastDisasmAddr > *m_endAddr) {
            m_mode = Mode::None;
            m_endAddr.reset();
            return;
        }
        if (!EmitDisasmLine(m_lastDisasmAddr))
            m_mode = Mode::None;
    } else if (m_mode == Mode::CopperList) {
        if (!RequireRunning())
            return;
        for (int i = 0; i < kCopperBlock; ++i) {
            if (!EmitCopperLine(m_curAddr)) {
                m_mode = Mode::None;
                break;
            }
        }
    }
    // Outside a listing mode, Return on old listing output does nothing.
}

void ArCommandInterpreter::HandleAssembleLine(const std::string& line)
{
    if (!m_host.assembler) {
        m_mode = Mode::None;
        Error("Error: assembler not installed");
        return;
    }
    if (!RequireRunning())
        return;

    // Typed-over listing line "AAAAAAAA: <hex>   <mnemonic>": the address
    // prefix wins (cursor-up re-edits earlier lines) and the source starts at
    // the mnemonic column. A bare line assembles at the current address.
    uint32_t target = m_curAddr;
    std::string source = line;
    if (line.size() > 10 && ParseHexAt(line, 0, 8) && line[8] == ':') {
        target = *ParseHexAt(line, 0, 8) & ~1u;
        source = line.size() > static_cast<size_t>(Disassembler::kMnemonicColumn)
                     ? line.substr(Disassembler::kMnemonicColumn)
                     : line.substr(9); // short hand-typed "AAAAAAAA: <source>" line
    }
    source = Trim(source);
    m_curAddr = target;

    // Unedited (or emptied) mnemonic: step past without re-assembling —
    // disasm output is not guaranteed to round-trip through the assembler.
    bool unchanged = source.empty();
    ArLine current;
    uint32_t next = 0;
    const bool haveCurrent = MakeDisasmArLine(target, current, next);
    if (!unchanged && haveCurrent) {
        Disassembler::Line ins;
        if (m_host.disassembleOne(target, ins))
            unchanged = ToUpper(source) == ToUpper(Trim(ins.mnemonic));
    }

    if (unchanged) {
        if (!haveCurrent) {
            Error(fmt::format("Error: cannot disassemble at ${:08X}", target));
            m_mode = Mode::None;
            return;
        }
        RewriteCurrent(current); // restore colors over the typed text
        if (!EmitDisasmLine(next))
            m_mode = Mode::None;
        m_curAddr = next;
        return;
    }

    std::vector<uint8_t> encoded;
    std::string err;
    if (!m_host.assembler->AssembleOne(source, target, encoded, err)) {
        Error("Error: " + err);
        return;
    }
    if (!m_mem->Write(target, encoded.data(), encoded.size(), err)) {
        Error("Error: " + err);
        return;
    }
    StateChanged();
    // Refresh the typed line with the verified disassembly, then offer the
    // next instruction for type-over.
    next = target + static_cast<uint32_t>(encoded.size());
    ArLine verified;
    uint32_t verifiedNext = 0;
    if (MakeDisasmArLine(target, verified, verifiedNext))
        RewriteCurrent(verified);
    if (!EmitDisasmLine(next))
        m_mode = Mode::None;
    m_curAddr = next;
}

// ---------------------------------------------------------------------------
// Interactive emission
// ---------------------------------------------------------------------------

ArLine ArCommandInterpreter::MakeMemHexArLine(uint32_t addr) const
{
    uint8_t bytes[kMemBytesPerLine];
    std::vector<bool> valid;
    m_mem->Read(addr, bytes, kMemBytesPerLine, &valid);

    ArLine line(FormatMemHexText(addr, bytes, valid));
    line.Span(0, 8, ArColor::Address);
    line.Span(kHexFieldCol, 39, ArColor::HexData);
    line.Span(kAsciiCol, kMemBytesPerLine, ArColor::Ascii);
    return line;
}

ArLine ArCommandInterpreter::MakeMemAsciiArLine(uint32_t addr) const
{
    uint8_t bytes[kMemBytesPerLine];
    std::vector<bool> valid;
    m_mem->Read(addr, bytes, kMemBytesPerLine, &valid);

    ArLine line(FormatMemAsciiText(addr, bytes, valid, m_asciiOffset));
    line.Span(0, 8, ArColor::Address);
    line.Span(kHexFieldCol + 1, kMemBytesPerLine, ArColor::Ascii);
    return line;
}

ArLine ArCommandInterpreter::MakeMemHexArLineHighlighted(uint32_t addr, uint32_t hlStart,
                                                         uint32_t hlEnd) const
{
    uint8_t bytes[kMemBytesPerLine];
    std::vector<bool> valid;
    m_mem->Read(addr, bytes, kMemBytesPerLine, &valid);

    ArLine line(FormatMemHexText(addr, bytes, valid));
    line.Span(0, 8, ArColor::Address);
    for (int i = 0; i < kMemBytesPerLine; ++i) {
        const uint32_t byteAddr = addr + static_cast<uint32_t>(i);
        const bool hl = byteAddr >= hlStart && byteAddr < hlEnd;
        const int hexCol = kHexFieldCol + (i / 2) * 5 + (i % 2) * 2;
        line.Span(hexCol, 2, hl ? ArColor::Highlight : ArColor::HexData);
        line.Span(kAsciiCol + i, 1, hl ? ArColor::Highlight : ArColor::Ascii);
    }
    return line;
}

void ArCommandInterpreter::EmitMemHexLine(uint32_t addr)
{
    Emit(MakeMemHexArLine(addr));
    m_curAddr = addr + kMemBytesPerLine;
}

void ArCommandInterpreter::EmitMemAsciiLine(uint32_t addr)
{
    Emit(MakeMemAsciiArLine(addr));
    m_curAddr = addr + kMemBytesPerLine;
}

ArLine ArCommandInterpreter::MakeMemBinArLine(uint32_t addr, int count) const
{
    if (count < 1)
        count = 1;
    if (count > kMemBinMaxBytes)
        count = kMemBinMaxBytes;
    uint8_t bytes[kMemBinMaxBytes];
    std::vector<bool> valid;
    m_mem->Read(addr, bytes, static_cast<size_t>(count), &valid);

    ArLine line(FormatMemBinText(addr, bytes, valid, count));
    line.Span(0, 8, ArColor::Address);
    line.Span(kHexFieldCol, static_cast<int>(line.text.size()) - kHexFieldCol, ArColor::HexData);
    return line;
}

void ArCommandInterpreter::EmitMemBinLine(uint32_t addr)
{
    Emit(MakeMemBinArLine(addr, m_binBytesPerLine));
    m_curAddr = addr + static_cast<uint32_t>(m_binBytesPerLine);
}

bool ArCommandInterpreter::MakeDisasmArLine(uint32_t addr, ArLine& out, uint32_t& nextAddr) const
{
    if (!m_host.disassembleOne)
        return false;
    Disassembler::Line ins;
    if (!m_host.disassembleOne(addr, ins) || ins.nextAddress <= addr)
        return false;

    std::string text = Disassembler::FormatLine(ins);
    out = ArLine(text);
    out.Span(0, 8, ArColor::Address);
    out.Span(Disassembler::kHexColumn, static_cast<int>(ins.hex.size()), ArColor::HexData);
    // Syntax-highlight the mnemonic/operand region with one span per token.
    for (const auto& tok : TokenizeAsm(text, Disassembler::kMnemonicColumn))
        out.Span(tok.col, tok.len, AsmTokenColor(tok.type));

    // Append the resolved Amiga library call (e.g. "  ; exec/DoIO()") for
    // JSR/JMP (aN,-$xxx) lines, before any user remark.
    {
        std::string libSuffix = LibraryCallSuffix(ins.mnemonic);
        if (!libSuffix.empty()) {
            int start = static_cast<int>(out.text.size());
            out.text += libSuffix;
            out.Span(start, static_cast<int>(libSuffix.size()), ArColor::Comment);
        }
    }

    // Append a "<label>" reference for any branch/jump target that carries a
    // label (the target's own label renders as the line above it).
    if (m_host.annotations) {
        std::string branchLbl = m_host.annotations->BuildBranchLabelSuffix(addr, ins.mnemonic);
        if (!branchLbl.empty()) {
            int start = static_cast<int>(out.text.size());
            out.text += branchLbl;
            out.Span(start, static_cast<int>(branchLbl.size()), ArColor::Label);
        }
    }

    // Append any user comment(s) tied to this instruction or its operand addresses.
    if (m_host.annotations) {
        std::string suffix = m_host.annotations->BuildAnnotationSuffix(addr, ins.mnemonic);
        if (!suffix.empty()) {
            int start = static_cast<int>(out.text.size());
            out.text += suffix;
            out.Span(start, static_cast<int>(suffix.size()), ArColor::Info);
        }
    }

    nextAddr = ins.nextAddress;
    return true;
}

std::string ArCommandInterpreter::LibraryCallSuffix(const std::string& mnemonic) const
{
    if (!m_libAnnotatorLoaded) {
        m_libAnnotatorLoaded = true;
        m_libAnnotator.LoadTables();
    }
    if (!m_libAnnotator.Loaded())
        return {};

    // Strategy (c): resolve the target library from the live base register and
    // memory; falls back to the exec default when these aren't available.
    LibraryCallAnnotator::Context ctx;
    if (m_host.getCpuState) {
        ctx.readAReg = [this](int aReg, uint32_t& val) {
            ArCpuState st;
            if (aReg < 0 || aReg > 7 || !m_host.getCpuState(st))
                return false;
            val = st.a[aReg];
            return true;
        };
    }
    if (m_mem) {
        ctx.readByte = [this](uint32_t addr, uint8_t& out) {
            bool ok = false;
            out = m_mem->ReadByte(addr, &ok);
            return ok;
        };
    }

    auto ann = m_libAnnotator.Annotate(mnemonic, ctx);
    return ann.value_or(std::string{});
}

bool ArCommandInterpreter::EmitDisasmLine(uint32_t addr)
{
    if (!m_host.disassembleOne) {
        Error("Error: disassembler unavailable");
        return false;
    }
    ArLine line;
    uint32_t next = 0;
    if (!MakeDisasmArLine(addr, line, next)) {
        Error(fmt::format("Error: cannot disassemble at ${:08X}", addr));
        return false;
    }
    // A comment block, then a user label, render above the instruction they name.
    if (m_host.annotations) {
        if (auto block = m_host.annotations->GetBlock(addr))
            for (const auto& bl : MemoryAnnotations::BuildCommentBlockLines(*block, 47))
                Emit(ArLine(bl, ArColor::Comment));
        if (auto lbl = m_host.annotations->GetLabel(addr))
            Emit(ArLine(*lbl + ":", ArColor::Label));
    }
    Emit(line);

    // AR-style routine break: a ';====' rule after a flow-terminating instruction.
    int mnemoCol = Disassembler::kMnemonicColumn;
    if (static_cast<int>(line.text.size()) > mnemoCol &&
        Disassembler::IsSectionEnd(line.text.substr(mnemoCol))) {
        Emit(ArLine(";" + std::string(47, '='), ArColor::Comment));
    }

    m_lastDisasmAddr = next;
    m_hasLastDisasmAddr = true;
    return true;
}

ArLine ArCommandInterpreter::MakeCustomRegArLine(uint32_t offset) const
{
    offset &= 0x1FE;
    std::string name = m_host.customRegName ? m_host.customRegName(offset) : std::string();
    if (name.empty())
        name = "---";
    bool ok = false;
    uint16_t value = m_mem->ReadWord(kCustomBase + offset, &ok);

    // "DFF000 BLTDDAT      = 0000" — value at column 22
    std::string text = fmt::format("DFF{:03X} {:<12} = {:04X}", offset, name, value);
    ArLine line(text);
    line.Span(0, 6, ArColor::Address);
    if (name != "---")
        line.Span(7, static_cast<int>(name.size()), ArColor::Register, offset);
    else
        line.Span(7, 3, ArColor::Info);
    line.Span(22, 4, ArColor::HexData);
    return line;
}

void ArCommandInterpreter::EmitCustomRegLine(uint32_t offset)
{
    offset &= 0x1FE;
    Emit(MakeCustomRegArLine(offset));
    m_curAddr = offset;
}

bool ArCommandInterpreter::EmitCopperLine(uint32_t addr)
{
    bool ok1 = false, ok2 = false;
    uint16_t w1 = m_mem->ReadWord(addr, &ok1);
    uint16_t w2 = m_mem->ReadWord(addr + 2, &ok2);
    if (!ok1 || !ok2) {
        Error(fmt::format("Error: unmapped address ${:08X}", addr));
        return false;
    }

    std::string text =
        m_host.disasmCopperIns ? m_host.disasmCopperIns(w1, w2) : std::string("????");
    std::string full = fmt::format("{:08X}: {:04X} {:04X}  {}", addr, w1, w2, text);
    ArLine line(full);
    line.Span(0, 8, ArColor::Address);
    line.Span(10, 9, ArColor::HexData);
    line.Span(21, static_cast<int>(full.size()) - 21, ArColor::Mnemonic);
    // MOVE instruction: hyperlink the destination register name
    if ((w1 & 1) == 0 && m_host.customRegName) {
        uint16_t regOffset = w1 & 0x1FE;
        std::string regName = m_host.customRegName(regOffset);
        if (!regName.empty())
            line.Span(26, static_cast<int>(regName.size()), ArColor::Register, regOffset);
    }
    Emit(line);

    m_curAddr = addr + 4;

    // End of copper list: WAIT for the impossible position ($FFFF,$FFFE)
    if (w1 == 0xFFFF && w2 == 0xFFFE)
        return false;
    return true;
}

void ArCommandInterpreter::EmitRegisters(const ArCpuState& st)
{
    // Address registers (A0-A7) and PC hold memory addresses, so their values get
    // an Address span — the console terminal renders those as clickable hyperlinks
    // that jump the memory view. Data registers stay plain (general-purpose data).
    auto quad = [this](const char* prefix, const uint32_t* regs, int base, bool markAddr) {
        std::string text;
        std::vector<int> valueCols;
        for (int i = 0; i < 4; ++i) {
            if (i)
                text += " ";
            text += fmt::format("{}{}=", prefix, base + i);
            valueCols.push_back(static_cast<int>(text.size()));
            text += fmt::format("{:08X}", regs[base + i]);
        }
        ArLine line(text);
        if (markAddr)
            for (int col : valueCols)
                line.Span(col, 8, ArColor::Address);
        Emit(line);
    };
    quad("D", st.d, 0, false);
    quad("D", st.d, 4, false);
    quad("A", st.a, 0, true);
    quad("A", st.a, 4, true);

    // Status flags: T S M I=n X N Z V C
    std::string flags;
    flags += (st.sr & 0x8000) ? 'T' : '-';
    flags += (st.sr & 0x2000) ? 'S' : '-';
    flags += (st.sr & 0x1000) ? 'M' : '-';
    flags += fmt::format(" I={}", (st.sr >> 8) & 7);
    flags += ' ';
    flags += (st.sr & 0x10) ? 'X' : '-';
    flags += (st.sr & 0x08) ? 'N' : '-';
    flags += (st.sr & 0x04) ? 'Z' : '-';
    flags += (st.sr & 0x02) ? 'V' : '-';
    flags += (st.sr & 0x01) ? 'C' : '-';

    std::string text = fmt::format("PC={:08X} SR={:04X} [{}]", st.pc, st.sr, flags);
    ArLine line(text);
    line.Span(0, 3, ArColor::Highlight); // "PC=" label
    line.Span(3, 8, ArColor::Address);   // PC value — clickable hyperlink
    Emit(line);

    // The stacks A7 is not showing, and the VBR where the CPU has one.
    CpuExtendedState ext;
    if (m_host.getCpuExtState && m_host.getCpuExtState(ext)) {
        std::string text;
        std::vector<int> valueCols;
        for (auto id : {CpuRegId::Usp, CpuRegId::Isp, CpuRegId::Msp, CpuRegId::Vbr}) {
            const auto& def = GetCpuRegister(id);
            auto reading = ReadCpuRegister(ext, def);
            if (!reading)
                continue;
            if (!text.empty())
                text += ' ';
            text += fmt::format("{}=", id == CpuRegId::Isp ? IspName(ext.cpuModel) : def.name);
            valueCols.push_back(static_cast<int>(text.size()));
            text += FormatCpuRegister(*reading);
        }
        text += fmt::format("  A7={}", ActiveStackName(ext));
        ArLine stackLine(text);
        for (int col : valueCols)
            stackLine.Span(col, 8, ArColor::Address);
        Emit(stackLine);
    }
}

int ArCommandInterpreter::EmitExtRegisterPairs(const CpuExtendedState& ext,
                                               std::initializer_list<CpuRegId> ids)
{
    std::string text;
    std::vector<std::pair<int, int>> addrSpans;
    std::vector<std::string> absent;
    int inLine = 0, present = 0;
    auto flush = [&]() {
        if (text.empty())
            return;
        ArLine line(text);
        for (auto [col, len] : addrSpans)
            line.Span(col, len, ArColor::Address);
        Emit(line);
        text.clear();
        addrSpans.clear();
        inLine = 0;
    };
    for (auto id : ids) {
        const auto& def = GetCpuRegister(id);
        const char* name = id == CpuRegId::Isp ? IspName(ext.cpuModel) : def.name;
        auto reading = ReadCpuRegister(ext, def);
        if (!reading) {
            absent.emplace_back(name);
            continue;
        }
        ++present;
        if (inLine == 4)
            flush();
        if (!text.empty())
            text += ' ';
        text += fmt::format("{}=", name);
        // A 64-bit root pointer links its low word, the table address.
        if (def.isAddress)
            addrSpans.push_back(
                {static_cast<int>(text.size()) + (reading->hexDigits == 16 ? 9 : 0), 8});
        text += FormatCpuRegister(*reading);
        ++inLine;
    }
    flush();
    if (!absent.empty()) {
        std::string list;
        for (const auto& name : absent)
            list += " " + name;
        Emit(fmt::format("Not on this {}:{}", ext.cpuModel, list), ArColor::Info);
    }
    return present;
}

bool ArCommandInterpreter::SetExtRegister(const CpuExtendedState& ext, const CpuRegDef& def,
                                          uint32_t value, std::string& error)
{
    if (!ReadCpuRegister(ext, def)) {
        error = "Error: " + NotOnCpuText(def, ext.cpuModel);
        return false;
    }
    const char* uaeName = UaeSettableName(ext, def);
    if (!uaeName) {
        error = fmt::format("Error: {} is read-only here", def.name);
        return false;
    }
    if (!m_host.setCpuRegister || !m_host.setCpuRegister(uaeName, value)) {
        error = "Error: cannot set register";
        return false;
    }
    return true;
}

void ArCommandInterpreter::EmitDisasmAtPc()
{
    ArCpuState st;
    if (m_host.getCpuState && m_host.getCpuState(st)) {
        Mode saved = m_mode;
        EmitDisasmLine(st.pc);
        m_mode = saved;
    }
}

// ---------------------------------------------------------------------------
// Memory display commands
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdMem(const std::string& args, int lines, bool ascii)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error(ascii ? "Usage: N <addr> (<end>)" : "Usage: M <addr> (<end>)");
        return;
    }
    auto addr = ParseNumber(tokens[0]);
    if (!addr) {
        Error("Error: bad address");
        return;
    }

    std::optional<uint32_t> end;
    if (tokens.size() > 1) {
        end = ParseNumber(tokens[1]);
        if (!end || *end < *addr) {
            Error("Error: bad end address");
            return;
        }
    }

    m_mode = ascii ? Mode::MemAscii : Mode::MemHex;
    m_endAddr = end;

    if (end) {
        // Automatic dump of the whole range via the scroll machinery
        m_mode = ascii ? Mode::ScrollAscii : Mode::ScrollHex;
        m_scrollEnd = *end;
        m_curAddr = *addr;
        m_endAddr.reset();
        return;
    }

    uint32_t a = *addr;
    for (int i = 0; i < lines; ++i) {
        if (ascii)
            EmitMemAsciiLine(a);
        else
            EmitMemHexLine(a);
        a += kMemBytesPerLine;
    }
}

void ArCommandInterpreter::CmdMemBin(const std::string& args, int lines)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: Y <addr> (<end>)");
        return;
    }
    auto addr = ParseNumber(tokens[0]);
    if (!addr) {
        Error("Error: bad address");
        return;
    }

    std::optional<uint32_t> end;
    if (tokens.size() > 1) {
        end = ParseNumber(tokens[1]);
        if (!end || *end < *addr) {
            Error("Error: bad end address");
            return;
        }
    }

    m_mode = Mode::MemBin;
    m_endAddr = end;
    const uint32_t step = static_cast<uint32_t>(m_binBytesPerLine);

    if (end) {
        // Automatic dump of the whole range (capped), no interactive mode left
        constexpr int kRangeCap = 2000;
        uint32_t a = *addr;
        int emitted = 0;
        while (a <= *end && emitted < kRangeCap) {
            Emit(MakeMemBinArLine(a, m_binBytesPerLine));
            a += step;
            ++emitted;
            if (a < step) // wrapped
                break;
        }
        if (emitted == kRangeCap)
            Info("Output capped.");
        m_mode = Mode::None;
        m_endAddr.reset();
        return;
    }

    uint32_t a = *addr;
    for (int i = 0; i < lines; ++i) {
        EmitMemBinLine(a);
        a += step;
    }
}

void ArCommandInterpreter::CmdBinSize(const std::string& args)
{
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Info(fmt::format("Binary width: {} bytes/line.", m_binBytesPerLine));
        return;
    }
    auto v = ParseNumber(tokens[0]);
    if (!v || *v == 0) {
        Error("Error: bad value");
        return;
    }
    // Accept either a byte count (1-8) or a bit count (multiple of 8).
    int bytes = (*v > kMemBinMaxBytes) ? static_cast<int>(*v / 8) : static_cast<int>(*v);
    if (bytes < 1)
        bytes = 1;
    if (bytes > kMemBinMaxBytes)
        bytes = kMemBinMaxBytes;
    m_binBytesPerLine = bytes;
    Info(fmt::format("Binary width: {} bytes/line.", m_binBytesPerLine));
}

void ArCommandInterpreter::CmdMemQuick(const std::string& args, bool ascii)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: MQ <addr> (<end>)");
        return;
    }
    auto addr = ParseNumber(tokens[0]);
    if (!addr) {
        Error("Error: bad address");
        return;
    }
    m_scrollEnd = 0xFFFFFFFF;
    if (tokens.size() > 1) {
        auto end = ParseNumber(tokens[1]);
        if (!end || *end < *addr) {
            Error("Error: bad end address");
            return;
        }
        m_scrollEnd = *end;
    }
    m_curAddr = *addr;
    m_mode = ascii ? Mode::ScrollAscii : Mode::ScrollHex;
    Info("Scrolling - press ESC to stop.");
}

void ArCommandInterpreter::CmdAsciiOffset(const std::string& args)
{
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Info(fmt::format("ASCII offset: {}.", m_asciiOffset));
        return;
    }
    // Allow negative decimal offsets like !-3 or -3
    std::string t = tokens[0];
    bool negative = false;
    if (!t.empty() && t[0] == '-') {
        negative = true;
        t.erase(0, 1);
    } else if (t.size() > 1 && t[0] == '!' && t[1] == '-') {
        negative = true;
        t.erase(1, 1);
    }
    auto v = ParseNumber(t);
    if (!v || *v > 255) {
        Error("Error: bad offset");
        return;
    }
    m_asciiOffset = negative ? -static_cast<int64_t>(*v) : static_cast<int64_t>(*v);
    Info(fmt::format("ASCII offset: {}.", m_asciiOffset));
}

// ---------------------------------------------------------------------------
// Disassembly / registers / custom / copper
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdDisasm(const std::string& args, int lines)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);

    uint32_t start;
    if (!tokens.empty()) {
        auto addr = ParseNumber(tokens[0]);
        if (!addr) {
            Error("Error: bad address");
            return;
        }
        start = *addr;
    } else if (m_hasLastDisasmAddr) {
        start = m_lastDisasmAddr;
    } else {
        ArCpuState st;
        if (!m_host.getCpuState || !m_host.getCpuState(st)) {
            Error("Error: cannot read CPU state");
            return;
        }
        start = st.pc;
    }

    std::optional<uint32_t> end;
    if (tokens.size() > 1) {
        end = ParseNumber(tokens[1]);
        if (!end || *end < start) {
            Error("Error: bad end address");
            return;
        }
    }

    m_mode = Mode::Disasm;
    m_endAddr = end;

    if (end) {
        // Automatic range disassembly (capped)
        constexpr int kRangeCap = 1000;
        uint32_t a = start;
        int emitted = 0;
        while (a <= *end && emitted < kRangeCap) {
            if (!EmitDisasmLine(a))
                break;
            a = m_lastDisasmAddr;
            ++emitted;
        }
        if (emitted == kRangeCap)
            Info("Output capped.");
        m_mode = Mode::None;
        m_endAddr.reset();
        return;
    }

    uint32_t a = start;
    for (int i = 0; i < lines; ++i) {
        if (!EmitDisasmLine(a)) {
            m_mode = Mode::None;
            return;
        }
        a = m_lastDisasmAddr;
    }
}

void ArCommandInterpreter::CmdRegisters(const std::string& args)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);

    if (tokens.size() >= 2) {
        // R <reg> <value>
        auto value = ParseNumber(tokens[1]);
        if (!value) {
            Error("Error: bad value");
            return;
        }
        if (const CpuRegDef* def = FindCpuRegister(tokens[0])) {
            CpuExtendedState ext;
            if (!m_host.getCpuExtState || !m_host.getCpuExtState(ext)) {
                Error("Error: cannot read CPU state");
                return;
            }
            std::string error;
            if (!SetExtRegister(ext, *def, *value, error)) {
                Error(error);
                return;
            }
            Info(fmt::format("Set {}=${:08X}", ToUpper(tokens[0]), *value));
            StateChanged();
            return;
        }
        std::string reg = tokens[0];
        std::transform(reg.begin(), reg.end(), reg.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!m_host.setCpuRegister || !m_host.setCpuRegister(reg, *value)) {
            Error("Error: cannot set register");
            return;
        }
        Info(fmt::format("Set {}=${:08X}", ToUpper(reg), *value));
        StateChanged();
        return;
    }

    ArCpuState st;
    if (!m_host.getCpuState || !m_host.getCpuState(st)) {
        Error("Error: cannot read CPU state");
        return;
    }
    EmitRegisters(st);
    EmitDisasmAtPc();
}

void ArCommandInterpreter::CmdCustomRegs(const std::string& args)
{
    if (!RequireRunning())
        return;
    uint32_t offset = 0;
    auto tokens = Tokenize(args);
    if (!tokens.empty()) {
        auto v = ParseNumber(tokens[0]);
        if (!v || *v >= 0x200) {
            Error("Error: bad offset (000-1FE)");
            return;
        }
        offset = *v & 0x1FE;
    }
    m_mode = Mode::CustomRegs;
    EmitCustomRegLine(offset);
}

void ArCommandInterpreter::CmdCopper(const std::string& args)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: C <1|2|ADDR>");
        return;
    }

    uint32_t addr = 0;
    if (tokens[0] == "1" || tokens[0] == "2") {
        if (!m_host.getCopperLc || !m_host.getCopperLc(tokens[0] == "1" ? 1 : 2, addr)) {
            Error("Error: cannot read copper pointers");
            return;
        }
        Info(fmt::format("COP{}LC = ${:08X}", tokens[0], addr));
    } else {
        auto v = ParseNumber(tokens[0]);
        if (!v) {
            Error("Error: bad address");
            return;
        }
        addr = *v;
    }

    m_mode = Mode::CopperList;
    m_curAddr = addr & ~1u;
    for (int i = 0; i < kCopperBlock; ++i) {
        if (!EmitCopperLine(m_curAddr)) {
            m_mode = Mode::None;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Execution control
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdGo(const std::string& args)
{
    const bool running = m_host.isRunning && m_host.isRunning();
    if (!running) {
        if (!m_host.startEmulator) {
            Error("Error: emulator not running");
            return;
        }
        Info("Starting emulator...");
        m_host.startEmulator();
        return;
    }
    auto tokens = Tokenize(args);
    std::string cmd = "g";
    if (!tokens.empty()) {
        auto addr = ParseNumber(tokens[0]);
        if (!addr) {
            Error("Error: bad address");
            return;
        }
        cmd = fmt::format("g {:x}", *addr);
    }
    std::string out;
    if (m_host.execUaeCommand)
        m_host.execUaeCommand(cmd, out);
    StateChanged();
    if (m_host.resume)
        m_host.resume();
    Info("Emulator running.");
}

void ArCommandInterpreter::CmdStop(const std::string&)
{
    if (!RequireRunning())
        return;
    if (!m_host.stopEmulator) {
        Error("Error: stop unavailable");
        return;
    }
    Info("Emulator stopped.");
    m_host.stopEmulator();
    StateChanged();
}

void ArCommandInterpreter::CmdStart(const std::string&)
{
    if (m_host.isRunning && m_host.isRunning()) {
        Error("Error: emulator already running");
        return;
    }
    if (!m_host.startEmulator) {
        Error("Error: start unavailable");
        return;
    }
    Info("Starting emulator...");
    m_host.startEmulator();
}

void ArCommandInterpreter::CmdTurbo(const std::string& args)
{
    if (!m_host.toggleTurbo || !m_host.isTurbo) {
        Error("Error: turbo unavailable");
        return;
    }
    auto tokens = Tokenize(args);
    bool enable;
    if (tokens.empty()) {
        enable = !m_host.isTurbo();
    } else {
        std::string tok = tokens[0];
        for (auto& c : tok)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (tok == "ON" || tok == "1")
            enable = true;
        else if (tok == "OFF" || tok == "0")
            enable = false;
        else {
            Error("Usage: TURBO/WARP <on|off>");
            return;
        }
    }
    if (enable == m_host.isTurbo()) {
        Info(fmt::format("Turbo already {}.", enable ? "on" : "off"));
        return;
    }
    m_host.toggleTurbo();
    // Report what actually happened, not what was asked for: turbo is refused outright
    // while a drive is backed by real hardware, and a "Turbo on." that did nothing is
    // exactly the silent no-op not to present.
    if (m_host.isTurbo() != enable) {
        Error("Error: turbo refused - a drive is backed by real hardware, which has to be "
              "serviced in real time to stay in step with the disk's rotation");
        return;
    }
    Info(fmt::format("Turbo {}.", enable ? "on" : "off"));
}

void ArCommandInterpreter::CmdStepFrame(const std::string& args)
{
    if (!m_host.stepFrame) {
        Error("Error: SF unavailable");
        return;
    }
    if (!RequireRunning())
        return;
    if (!m_host.isPaused || !m_host.isPaused()) {
        Error("Error: emulator not paused");
        return;
    }
    const auto tokens = Tokenize(args);
    uint32_t count = 1;
    if (!tokens.empty()) {
        auto v = ParseNumber(tokens[0]);
        if (!v || *v < 1) {
            Error("Error: bad frame count");
            return;
        }
        count = *v;
    }
    for (uint32_t i = 0; i < count; ++i)
        m_host.stepFrame();
    if (m_host.getFrameCount)
        Info(fmt::format("Frame: {}", m_host.getFrameCount()));
    StateChanged();
}

void ArCommandInterpreter::CmdState(const std::string& args)
{
    if (!m_host.listStates || !m_host.saveState || !m_host.loadState || !m_host.deleteState) {
        Error("Error: state snapshots unavailable");
        return;
    }

    // A bare STATE lists, and so does STATE LIST — the explicit spelling reads better in a
    // script and sits alongside the other verbs.
    const auto tokens = Tokenize(args);
    const std::string action = tokens.empty() ? "LIST" : ToUpper(tokens[0]);

    if (action == "LIST") {
        if (tokens.size() > 1) {
            Error("Usage: STATE LIST");
            return;
        }
        const auto states = m_host.listStates();
        if (states.empty()) {
            Info("No snapshots. STATE SAVE <name> takes one.");
            return;
        }
        // Snapshots are whole machines — a megabyte or so each — so MB is the useful unit.
        const auto size = [](size_t bytes) {
            return bytes >= 1024 * 1024 ? fmt::format("{:.1f}M", bytes / (1024.0 * 1024.0))
                                        : fmt::format("{:.0f}K", bytes / 1024.0);
        };
        EmitTableHead("Name                       Frame     Size  Where   Created");
        for (const auto& s : states) {
            Emit(fmt::format("  {:<24} {:>6}  {:>7}  {:<6}  {}", s.name, s.frame, size(s.bytes),
                             s.persisted ? "disk" : "memory", s.created));
        }
        return;
    }

    if (action != "SAVE" && action != "LOAD" && action != "DELETE" && action != "DEL") {
        Error("Usage: STATE (LIST)   |   STATE SAVE <name> (PERSIST)   |   "
              "STATE LOAD|DELETE <name>");
        return;
    }
    if (tokens.size() < 2) {
        Error(fmt::format("Usage: STATE {} <name>", action));
        return;
    }
    const std::string& name = tokens[1];

    if (action == "DELETE" || action == "DEL") {
        if (!m_host.deleteState(name)) {
            Error(fmt::format("Error: no snapshot named '{}'", name));
            return;
        }
        Info(fmt::format("Deleted snapshot '{}'.", name));
        StateChanged();
        return;
    }

    if (action == "SAVE") {
        // PERSIST also writes the snapshot to disk, so a hard-won boot state survives a
        // restart. Without it the snapshot lives only for this session.
        bool persist = false;
        for (size_t i = 2; i < tokens.size(); ++i) {
            const std::string opt = ToUpper(tokens[i]);
            if (opt == "PERSIST" || opt == "P") {
                persist = true;
            } else {
                Error(fmt::format("Error: unknown option '{}' (expected PERSIST)", tokens[i]));
                return;
            }
        }
        std::string warning;
        const std::string error = m_host.saveState(name, persist, warning);
        if (!error.empty()) {
            Error("Error: " + error);
            return;
        }
        Info(fmt::format("Saved snapshot '{}'{}.", name, persist ? " to disk" : ""));
        if (!warning.empty())
            Error("Warning: " + warning);
        StateChanged();
        return;
    }

    const std::string error = m_host.loadState(name);
    if (!error.empty()) {
        Error("Error: " + error);
        return;
    }
    Info(fmt::format("Restored snapshot '{}'.", name));
    if (m_host.getFrameCount)
        Info(fmt::format("Frame: {}", m_host.getFrameCount()));
    StateChanged();
}

void ArCommandInterpreter::CmdRunUntilFrame(const std::string& args)
{
    if (!m_host.runUntilFrame) {
        Error("Error: RUNTO unavailable");
        return;
    }
    if (!RequireRunning())
        return;

    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: RUNTO <frame> [T]  (T = turbo). See FRAME for the current frame.");
        return;
    }
    // Frame numbers are decimal here (to match the FRAME command's decimal output),
    // unlike the hex-default addresses elsewhere in the console.
    const std::string& frameTok = tokens[0];
    if (frameTok.empty() || !std::all_of(frameTok.begin(), frameTok.end(),
                                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
        Error("Error: bad frame number (decimal)");
        return;
    }
    const uint64_t target = std::strtoull(frameTok.c_str(), nullptr, 10);

    bool turbo = false;
    if (tokens.size() > 1) {
        std::string tok = tokens[1];
        for (auto& c : tok)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (tok == "T" || tok == "TURBO" || tok == "WARP" || tok == "ON" || tok == "1")
            turbo = true;
        else {
            Error("Usage: RUNTO <frame> [T]  (T = turbo)");
            return;
        }
    }

    const uint64_t cur = m_host.getFrameCount ? m_host.getFrameCount() : 0;
    if (target <= cur) {
        Error(fmt::format("Error: already at/past frame {} (current {})", target, cur));
        return;
    }

    // Runs asynchronously: the emulator free-runs until the target frame, then auto-pauses
    // and OnDebugBreak() reports the resulting state (same path as a breakpoint stop).
    if (!m_host.runUntilFrame(target, turbo)) {
        Error("Error: RUNTO failed");
        return;
    }
    StateChanged();
    Info(fmt::format("Running to frame {}{}...", target, turbo ? " (turbo)" : ""));
}

void ArCommandInterpreter::CmdEndFrame(const std::string&)
{
    if (!m_host.endFrame) {
        Error("Error: EF unavailable");
        return;
    }
    if (!RequireRunning())
        return;
    if (!m_host.isPaused || !m_host.isPaused()) {
        Error("Error: emulator not paused");
        return;
    }
    // Complete the current (possibly mid-frame) frame, stepping past breakpoints.
    m_host.endFrame();
    if (m_host.getFrameCount)
        Info(fmt::format("Frame: {} (completed)", m_host.getFrameCount()));
    StateChanged();
}

void ArCommandInterpreter::CmdPause(const std::string&)
{
    if (!RequireRunning())
        return;
    const bool paused = m_host.isPaused && m_host.isPaused();
    if (paused) {
        if (!m_host.resume) {
            Error("Error: resume unavailable");
            return;
        }
        Info("Resumed.");
        m_host.resume();
    } else {
        if (!m_host.pause) {
            Error("Error: pause unavailable");
            return;
        }
        Info("Paused.");
        m_host.pause();
    }
    StateChanged();
}

void ArCommandInterpreter::CmdStep(const std::string& args, bool over)
{
    if (!RequireRunning())
        return;
    auto& step = over ? m_host.stepOver : m_host.stepInto;
    if (!step) {
        Error("Error: step unavailable");
        return;
    }

    int n = 1;
    auto tokens = Tokenize(args);
    if (!tokens.empty()) {
        auto v = ParseNumber(tokens[0]);
        if (!v || *v == 0 || *v > 65536) {
            Error("Error: bad step count (1-10000 hex)");
            return;
        }
        n = static_cast<int>(*v);
    }

    if (m_host.isPaused && !m_host.isPaused() && m_host.pause)
        m_host.pause();

    if (over) {
        // Step-over of a subroutine call (BSR/JSR) resumes execution with a one-shot
        // trace at the return address (UAE 'z' → TRACE_MATCH_PC). The callee can span
        // many emulated frames, so that runs asynchronously: OnDebugBreak() reports
        // the resulting state once the return (or a breakpoint inside the callee) is
        // reached. Non-call instructions single-step synchronously and are reported
        // here. The repeat count is not honoured for step-over.
        bool async = step();
        StateChanged();
        if (async)
            Info("Stepping over...");
        else
            EmitDisasmAtPc();
        return;
    }

    for (int i = 0; i < n; ++i) {
        if (!step())
            break;
    }
    StateChanged();
    EmitDisasmAtPc();
}

void ArCommandInterpreter::CmdTraceUntil(const std::string& args)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: TA <addr> (<end>)");
        return;
    }
    auto addr = ParseNumber(tokens[0]);
    if (!addr) {
        Error("Error: bad address");
        return;
    }

    if (tokens.size() > 1) {
        auto end = ParseNumber(tokens[1]);
        if (!end || *end < *addr) {
            Error("Error: bad end address");
            return;
        }
        // UAE range trace: step until PC is inside [addr, end]
        std::string out;
        if (m_host.execUaeCommand &&
            m_host.execUaeCommand(fmt::format("f {:x} {:x}", *addr, *end), out)) {
            StateChanged();
            if (m_host.resume)
                m_host.resume();
            Info(fmt::format("Tracing to ${:08X}-${:08X}...", *addr, *end));
        } else {
            Error("Error: trace unavailable");
        }
        return;
    }

    // Single address: temporary breakpoint, removed when it fires
    if (!m_host.addBreakpoint) {
        Error("Error: breakpoints unavailable");
        return;
    }
    if (m_host.addBreakpoint(*addr))
        m_tempTraceBp = *addr;
    StateChanged();
    if (m_host.resume)
        m_host.resume();
    Info(fmt::format("Tracing to ${:08X}...", *addr));
}

// ---------------------------------------------------------------------------
// Breakpoints / watchpoints
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdBreakSet(const std::string& args)
{
    static const char* kUsage =
        "Usage: BS <addr> [IF <reg> <op> <val> [mask]] [N <count>] [ONESHOT]";
    auto tokens = Tokenize(args);
    std::optional<uint32_t> addr = tokens.empty() ? std::nullopt : ParseNumber(tokens[0]);
    if (!addr) {
        Error(kUsage);
        return;
    }
    if (!m_host.addBreakpoint) {
        Error("Error: breakpoints unavailable");
        return;
    }

    auto upper = [](std::string s) {
        for (auto& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    };

    // Parse the optional condition / ignore-count / one-shot clauses.
    bool condEnabled = false, oneShot = false;
    std::string reg, oper;
    uint32_t value = 0, mask = 0xFFFFFFFFu;
    int ignore = 0;
    for (size_t i = 1; i < tokens.size();) {
        const std::string kw = upper(tokens[i]);
        if (kw == "IF") {
            if (i + 3 >= tokens.size()) {
                Error(kUsage);
                return;
            }
            reg = upper(tokens[i + 1]);
            oper = tokens[i + 2];
            auto v = ParseNumber(tokens[i + 3]);
            if (!v) {
                Error("Error: bad condition value");
                return;
            }
            value = *v;
            condEnabled = true;
            i += 4;
            // Optional AND-mask: a trailing number that isn't the next keyword.
            if (i < tokens.size()) {
                const std::string nx = upper(tokens[i]);
                if (nx != "N" && nx != "ONESHOT" && nx != "ONCE") {
                    if (auto m = ParseNumber(tokens[i])) {
                        mask = *m;
                        ++i;
                    }
                }
            }
        } else if (kw == "N") {
            if (i + 1 >= tokens.size()) {
                Error("Error: N needs a count");
                return;
            }
            ignore = std::atoi(tokens[i + 1].c_str()); // decimal hit count
            i += 2;
        } else if (kw == "ONESHOT" || kw == "ONCE") {
            oneShot = true;
            ++i;
        } else {
            Error(kUsage);
            return;
        }
    }

    const bool smart = condEnabled || ignore > 0 || oneShot;
    if (!smart) {
        if (!m_host.addBreakpoint(*addr)) {
            Error(fmt::format("Error: breakpoint already set at ${:08X}", *addr));
            return;
        }
        Info(fmt::format("Breakpoint set at ${:08X}", *addr));
        StateChanged();
        return;
    }

    if (!m_host.addSmartBreakpoint) {
        Error("Error: conditional breakpoints unavailable");
        return;
    }
    if (!m_host.addSmartBreakpoint(*addr, condEnabled, reg, oper, value, mask, ignore, oneShot)) {
        Error(fmt::format("Error: breakpoint already set at ${:08X}", *addr));
        return;
    }
    std::string msg = fmt::format("Breakpoint set at ${:08X}", *addr);
    if (condEnabled) {
        if (mask != 0xFFFFFFFFu)
            msg += fmt::format(" if ({} & ${:X}) {} ${:X}", reg, mask, oper, value);
        else
            msg += fmt::format(" if {} {} ${:X}", reg, oper, value);
    }
    if (ignore > 0)
        msg += fmt::format(", ignore {}", ignore);
    if (oneShot)
        msg += ", one-shot";
    Info(msg);
    StateChanged();
}

void ArCommandInterpreter::CmdBreakDelete(const std::string& args)
{
    auto tokens = Tokenize(args);
    std::optional<uint32_t> addr = tokens.empty() ? std::nullopt : ParseNumber(tokens[0]);
    if (!addr) {
        Error("Usage: BD <addr>");
        return;
    }
    if (!m_host.removeBreakpoint || !m_host.removeBreakpoint(*addr)) {
        Error(fmt::format("Error: no breakpoint at ${:08X}", *addr));
        return;
    }
    Info(fmt::format("Breakpoint removed at ${:08X}", *addr));
    StateChanged();
}

void ArCommandInterpreter::CmdBreakList(const std::string&)
{
    if (!m_host.listBreakpoints) {
        Error("Error: breakpoints unavailable");
        return;
    }
    auto list = m_host.listBreakpoints();
    if (list.empty()) {
        Info("No breakpoints set.");
        return;
    }
    EmitTableHead("Address    State");
    for (const auto& [addr, enabled] : list) {
        ArLine line(fmt::format("${:08X}  {}", addr, enabled ? "Enabled" : "Disabled"));
        line.Span(0, 9, ArColor::Address);
        Emit(line);
    }
}

void ArCommandInterpreter::CmdBreakDeleteAll(const std::string&)
{
    if (m_host.clearAllBreakpoints)
        m_host.clearAllBreakpoints();
    Info("All breakpoints removed.");
    StateChanged();
}

// BKS [/CONTAINS|/EXACT|/PREFIX|/REGEX] [/CASE] [/ONCE] <text>
//
// Flags are leading '/' tokens; everything after them is the literal text to match, spaces and
// all (optionally wrapped in double quotes so leading/trailing spaces can be included).
void ArCommandInterpreter::CmdKprintfSet(const std::string& args)
{
    static const char* kUsage = "Usage: BKS [/EXACT|/PREFIX|/REGEX] [/CASE] [/ONCE] <text>";
    if (!m_host.addKprintfBreakpoint) {
        Error("Error: KPrintF breakpoints unavailable");
        return;
    }

    KprintfMatch match = KprintfMatch::Contains;
    bool caseSensitive = false, oneShot = false;

    size_t pos = 0;
    while (pos < args.size() && args[pos] == '/') {
        const size_t end = args.find_first_of(" \t", pos);
        const std::string flag = ToUpper(
            args.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1));
        if (flag == "CONTAINS")
            match = KprintfMatch::Contains;
        else if (flag == "EXACT")
            match = KprintfMatch::Exact;
        else if (flag == "PREFIX" || flag == "STARTS")
            match = KprintfMatch::Prefix;
        else if (flag == "REGEX" || flag == "RE")
            match = KprintfMatch::Regex;
        else if (flag == "CASE")
            caseSensitive = true;
        else if (flag == "ONCE" || flag == "ONESHOT")
            oneShot = true;
        else {
            Error(fmt::format("Error: unknown flag /{}", flag));
            Error(kUsage);
            return;
        }
        if (end == std::string::npos) {
            pos = args.size();
            break;
        }
        pos = args.find_first_not_of(" \t", end);
        if (pos == std::string::npos)
            pos = args.size();
    }

    std::string text = Trim(args.substr(pos));
    // Quoted form keeps significant leading/trailing spaces.
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        text = text.substr(1, text.size() - 2);
    if (text.empty()) {
        Error(kUsage);
        return;
    }

    const int id = m_host.addKprintfBreakpoint(text, match, caseSensitive, oneShot);
    if (id == 0) {
        Error(match == KprintfMatch::Regex ? "Error: duplicate rule or invalid regular expression"
                                           : "Error: that KPrintF breakpoint already exists");
        return;
    }

    std::string msg = fmt::format("KPrintF breakpoint {} set: {} \"{}\"", id,
                                  KprintfBreakpoints::MatchName(match), text);
    if (caseSensitive)
        msg += ", case-sensitive";
    if (oneShot)
        msg += ", one-shot";
    Info(msg);
    StateChanged();
}

void ArCommandInterpreter::CmdKprintfDelete(const std::string& args)
{
    if (!m_host.removeKprintfBreakpoint || !m_host.listKprintfBreakpoints) {
        Error("Error: KPrintF breakpoints unavailable");
        return;
    }
    const std::string arg = Trim(args);
    if (arg.empty()) {
        Error("Usage: BKD <n|text>   (n = the number shown by BK)");
        return;
    }

    auto list = m_host.listKprintfBreakpoints();
    int id = 0;
    // An all-digits argument is the 1-based row number from BK; anything else is matched
    // against the rule text verbatim.
    const bool numeric = arg.find_first_not_of("0123456789") == std::string::npos;
    if (numeric) {
        const int n = std::atoi(arg.c_str());
        if (n < 1 || n > static_cast<int>(list.size())) {
            Error(fmt::format("Error: no KPrintF breakpoint #{}", n));
            return;
        }
        id = list[static_cast<size_t>(n - 1)].id;
    } else {
        for (const auto& e : list) {
            if (e.text == arg) {
                id = e.id;
                break;
            }
        }
        if (id == 0) {
            Error(fmt::format("Error: no KPrintF breakpoint matching \"{}\"", arg));
            return;
        }
    }

    if (!m_host.removeKprintfBreakpoint(id)) {
        Error("Error: KPrintF breakpoint not found");
        return;
    }
    Info("KPrintF breakpoint removed.");
    StateChanged();
}

void ArCommandInterpreter::CmdKprintfList(const std::string& args)
{
    // "BK <text>" is a convenience alias for "BKS <text>" so the common case is one word.
    if (!Trim(args).empty()) {
        CmdKprintfSet(args);
        return;
    }
    if (!m_host.listKprintfBreakpoints) {
        Error("Error: KPrintF breakpoints unavailable");
        return;
    }
    auto list = m_host.listKprintfBreakpoints();
    if (list.empty()) {
        Info("No KPrintF breakpoints set.");
        return;
    }
    EmitTableHead("  # State    Match     Case One Hits  Text");
    int n = 0;
    for (const auto& e : list) {
        ArLine line(
            fmt::format("{:>3} {:<8} {:<9} {:<4} {:<3} {:<5} {}", ++n,
                        e.enabled ? "Enabled" : "Disabled", KprintfBreakpoints::MatchName(e.match),
                        e.caseSensitive ? "yes" : "no", e.oneShot ? "yes" : "no", e.hits, e.text));
        if (e.enabled)
            line.Span(0, 3, ArColor::Address);
        else
            line.Span(0, static_cast<int>(line.text.size()), ArColor::Comment);
        Emit(line);
        if (!e.lastLine.empty())
            Emit(fmt::format("    last: {}", e.lastLine));
    }
}

void ArCommandInterpreter::CmdKprintfDeleteAll(const std::string&)
{
    if (!m_host.clearKprintfBreakpoints) {
        Error("Error: KPrintF breakpoints unavailable");
        return;
    }
    m_host.clearKprintfBreakpoints();
    Info("All KPrintF breakpoints removed.");
    StateChanged();
}

void ArCommandInterpreter::CmdWatchSet(const std::string& args)
{
    auto tokens = Tokenize(args);
    std::optional<uint32_t> addr = tokens.empty() ? std::nullopt : ParseNumber(tokens[0]);
    if (!addr) {
        Error("Usage: MS <addr>");
        return;
    }
    if (!m_host.addWatchpoint) {
        Error("Error: watchpoints unavailable");
        return;
    }
    if (!m_host.addWatchpoint(*addr)) {
        Error(fmt::format("Error: watchpoint already set at ${:08X}", *addr));
        return;
    }
    Info(fmt::format("Watchpoint set at ${:08X}", *addr));
    StateChanged();
}

void ArCommandInterpreter::CmdWatchDelete(const std::string& args)
{
    auto tokens = Tokenize(args);
    std::optional<uint32_t> addr = tokens.empty() ? std::nullopt : ParseNumber(tokens[0]);
    if (!addr) {
        Error("Usage: MD <addr>");
        return;
    }
    if (!m_host.removeWatchpoint || !m_host.removeWatchpoint(*addr)) {
        Error(fmt::format("Error: no watchpoint at ${:08X}", *addr));
        return;
    }
    Info(fmt::format("Watchpoint removed at ${:08X}", *addr));
    StateChanged();
}

void ArCommandInterpreter::CmdWatchList(const std::string&)
{
    if (!m_host.listWatchpoints) {
        Error("Error: watchpoints unavailable");
        return;
    }
    auto list = m_host.listWatchpoints();
    if (list.empty()) {
        Info("No memory watchpoints set.");
        return;
    }
    EmitTableHead("Address    Type");
    for (uint32_t addr : list) {
        ArLine line(fmt::format("${:08X}  Write", addr));
        line.Span(0, 9, ArColor::Address);
        Emit(line);
    }
}

void ArCommandInterpreter::CmdWatchDeleteAll(const std::string&)
{
    if (m_host.clearAllWatchpoints)
        m_host.clearAllWatchpoints();
    Info("All memory watchpoints removed.");
    StateChanged();
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

bool ArCommandInterpreter::ResolveSearchRange(const std::vector<std::string>& tokens,
                                              size_t firstTokenIdx,
                                              std::vector<std::pair<uint32_t, uint32_t>>& ranges)
{
    ranges.clear();
    if (tokens.size() > firstTokenIdx) {
        auto start = ParseNumber(tokens[firstTokenIdx]);
        if (!start)
            return false;
        uint32_t end = 0xFFFFFFFF;
        if (tokens.size() > firstTokenIdx + 1) {
            auto e = ParseNumber(tokens[firstTokenIdx + 1]);
            if (!e || *e < *start)
                return false;
            end = *e;
        }
        ranges.push_back({*start, end});
        return true;
    }
    // Default: all writable (RAM) regions
    for (const auto& r : m_mem->Regions()) {
        if (r.writable)
            ranges.push_back({r.base, r.base + r.size - 1});
    }
    return true;
}

void ArCommandInterpreter::ReportMatches(const std::vector<uint32_t>& matches)
{
    if (matches.empty()) {
        Info("Not found.");
        return;
    }
    std::string row;
    int inRow = 0;
    for (uint32_t m : matches) {
        if (inRow)
            row += "  ";
        row += fmt::format("${:08X}", m);
        if (++inRow == 4) {
            Emit(row, ArColor::Address);
            row.clear();
            inRow = 0;
        }
    }
    if (inRow)
        Emit(row, ArColor::Address);
    if (matches.size() >= kSearchMatchCap)
        Info(fmt::format("Output capped at {} matches.", kSearchMatchCap));
    Info(fmt::format("{} match{}.", matches.size(), matches.size() == 1 ? "" : "es"));
}

void ArCommandInterpreter::CmdFind(const std::string& args, bool caseInsensitive)
{
    if (!RequireRunning())
        return;

    // -L: long results — dump memory around each match with the term highlighted
    std::string rest = Trim(args);
    bool longMode = false;
    if (rest.size() >= 2 && rest[0] == '-' && (rest[1] == 'l' || rest[1] == 'L') &&
        (rest.size() == 2 || rest[2] == ' ')) {
        longMode = true;
        rest = Trim(rest.substr(2));
    }

    std::string patternPart = rest, rangePart;
    std::string left, right;
    if (SplitAtLastComma(rest, left, right)) {
        patternPart = left;
        rangePart = right;
    }

    std::vector<uint8_t> pattern;
    if (!ParsePattern(patternPart, pattern)) {
        Error("Usage: F (-L) <string|bytes>(,<start> <end>)");
        return;
    }

    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    if (!ResolveSearchRange(Tokenize(rangePart), 0, ranges)) {
        Error("Error: bad range");
        return;
    }

    auto matchByte = [caseInsensitive](uint8_t mem, uint8_t pat) {
        if (mem == pat)
            return true;
        if (!caseInsensitive)
            return false;
        return std::tolower(mem) == std::tolower(pat);
    };

    std::vector<uint32_t> matches;
    for (const auto& [start, end] : ranges) {
        for (const auto& r : m_mem->Regions()) {
            uint32_t lo = std::max(start, r.base);
            uint32_t hi = std::min(end, r.base + r.size - 1);
            if (lo > hi || hi - lo + 1 < pattern.size())
                continue;
            const uint8_t* base = r.hostPtr + (lo - r.base);
            size_t span = hi - lo + 1;
            for (size_t i = 0; i + pattern.size() <= span; ++i) {
                bool hit = true;
                for (size_t j = 0; j < pattern.size(); ++j) {
                    if (!matchByte(base[i + j], pattern[j])) {
                        hit = false;
                        break;
                    }
                }
                if (hit) {
                    matches.push_back(lo + static_cast<uint32_t>(i));
                    if (matches.size() >= kSearchMatchCap)
                        goto done;
                }
            }
        }
    }
done:
    if (!longMode) {
        ReportMatches(matches);
        return;
    }

    if (matches.empty()) {
        Info("Not found.");
        return;
    }

    // Long results: 32 bytes from each match address as two editable M-format
    // lines, with the matched bytes highlighted.
    constexpr size_t kLongResultCap = 20;
    const size_t shown = std::min(matches.size(), kLongResultCap);
    for (size_t m = 0; m < shown; ++m) {
        const uint32_t a = matches[m];
        const uint32_t hlEnd = a + static_cast<uint32_t>(pattern.size());
        Emit(MakeMemHexArLineHighlighted(a, a, hlEnd));
        Emit(MakeMemHexArLineHighlighted(a + kMemBytesPerLine, a, hlEnd));
    }
    if (matches.size() > shown) {
        Info(fmt::format("Showing first {} of {}{} matches.", shown, matches.size(),
                         matches.size() >= kSearchMatchCap ? "+" : ""));
    } else {
        Info(fmt::format("{} match{}.", matches.size(), matches.size() == 1 ? "" : "es"));
    }
}

void ArCommandInterpreter::CmdFindRelative(const std::string& args)
{
    if (!RequireRunning())
        return;

    std::string patternPart = args, rangePart;
    std::string left, right;
    if (SplitAtLastComma(args, left, right)) {
        patternPart = left;
        rangePart = right;
    }

    std::vector<uint8_t> pattern;
    if (!ParsePattern(patternPart, pattern) || pattern.size() < 2) {
        Error("Usage: FR <string>(,<start> <end>)");
        return;
    }

    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    if (!ResolveSearchRange(Tokenize(rangePart), 0, ranges)) {
        Error("Error: bad range");
        return;
    }

    // Deltas between consecutive pattern bytes (mod 256)
    std::vector<uint8_t> deltas(pattern.size() - 1);
    for (size_t i = 0; i + 1 < pattern.size(); ++i)
        deltas[i] = static_cast<uint8_t>(pattern[i + 1] - pattern[i]);

    int found = 0;
    for (const auto& [start, end] : ranges) {
        for (const auto& r : m_mem->Regions()) {
            uint32_t lo = std::max(start, r.base);
            uint32_t hi = std::min(end, r.base + r.size - 1);
            if (lo > hi || hi - lo + 1 < pattern.size())
                continue;
            const uint8_t* base = r.hostPtr + (lo - r.base);
            size_t span = hi - lo + 1;
            for (size_t i = 0; i + pattern.size() <= span; ++i) {
                bool hit = true;
                for (size_t j = 0; j < deltas.size(); ++j) {
                    if (static_cast<uint8_t>(base[i + j + 1] - base[i + j]) != deltas[j]) {
                        hit = false;
                        break;
                    }
                }
                if (hit) {
                    uint8_t offset = static_cast<uint8_t>(base[i] - pattern[0]);
                    Emit(fmt::format("${:08X} (offset +{:02X})", lo + static_cast<uint32_t>(i),
                                     offset),
                         ArColor::Address);
                    if (++found >= kSearchMatchCap) {
                        Info("Output capped.");
                        return;
                    }
                }
            }
        }
    }
    if (!found)
        Info("Not found.");
    else
        Info(fmt::format("{} match{}.", found, found == 1 ? "" : "es"));
}

void ArCommandInterpreter::CmdFindAccess(const std::string& args, bool quick)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: FA <addr> (<start> <end>)");
        return;
    }
    auto target = ParseNumber(tokens[0]);
    if (!target) {
        Error("Error: bad address");
        return;
    }

    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    if (!ResolveSearchRange(tokens, 1, ranges)) {
        Error("Error: bad range");
        return;
    }

    if (quick) {
        // Quick: scan for the literal 32-bit big-endian address value
        uint8_t pat[4] = {static_cast<uint8_t>(*target >> 24), static_cast<uint8_t>(*target >> 16),
                          static_cast<uint8_t>(*target >> 8), static_cast<uint8_t>(*target)};
        std::vector<uint32_t> matches;
        for (const auto& [start, end] : ranges) {
            for (const auto& r : m_mem->Regions()) {
                uint32_t lo = std::max(start, r.base);
                uint32_t hi = std::min(end, r.base + r.size - 1);
                if (lo > hi || hi - lo + 1 < 4)
                    continue;
                const uint8_t* base = r.hostPtr + (lo - r.base);
                size_t span = hi - lo + 1;
                for (size_t i = 0; i + 4 <= span; ++i) {
                    if (memcmp(base + i, pat, 4) == 0) {
                        matches.push_back(lo + static_cast<uint32_t>(i));
                        if (matches.size() >= kSearchMatchCap)
                            goto qdone;
                    }
                }
            }
        }
    qdone:
        ReportMatches(matches);
        return;
    }

    if (!m_host.disassembleOne) {
        Error("Error: disassembler unavailable");
        return;
    }

    // Full: disassemble through the range and match instructions that
    // reference the target address in their operand text.
    const std::string needlePadded = fmt::format("${:08X}", *target);
    const std::string needleShort = fmt::format("${:X}", *target);
    constexpr int kFaOutputCap = 100;
    constexpr int kFaInstructionCap = 2000000;

    int found = 0;
    int seen = 0;
    Mode saved = m_mode;
    for (const auto& [start, end] : ranges) {
        uint32_t a = start & ~1u;
        while (a <= end && seen < kFaInstructionCap) {
            Disassembler::Line ins;
            if (!m_host.disassembleOne(a, ins) || ins.nextAddress <= a) {
                a += 2;
                continue;
            }
            ++seen;
            if (ins.mnemonic.find(needlePadded) != std::string::npos ||
                ins.mnemonic.find(needleShort) != std::string::npos) {
                std::string text = Disassembler::FormatLine(ins);
                ArLine line(text);
                line.Span(0, 8, ArColor::Address);
                Emit(line);
                if (++found >= kFaOutputCap) {
                    Info("Output capped.");
                    m_mode = saved;
                    return;
                }
            }
            a = ins.nextAddress;
        }
    }
    m_mode = saved;
    if (!found)
        Info("Not found.");
    else
        Info(fmt::format("{} match{}.", found, found == 1 ? "" : "es"));
}

// ---------------------------------------------------------------------------
// Block operations
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdCompare(const std::string& args)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.size() != 3) {
        Error("Usage: COMP <start> <end> <dest>");
        return;
    }
    auto start = ParseNumber(tokens[0]);
    auto end = ParseNumber(tokens[1]);
    auto dest = ParseNumber(tokens[2]);
    if (!start || !end || !dest || *end < *start) {
        Error("Error: bad address");
        return;
    }

    size_t len = static_cast<size_t>(*end - *start) + 1;
    int diffs = 0;
    int shown = 0;
    for (size_t i = 0; i < len; ++i) {
        bool okA = false, okB = false;
        uint8_t a = m_mem->ReadByte(*start + static_cast<uint32_t>(i), &okA);
        uint8_t b = m_mem->ReadByte(*dest + static_cast<uint32_t>(i), &okB);
        if (!okA || !okB) {
            Error(fmt::format("Error: unmapped address at offset ${:X}", i));
            return;
        }
        if (a != b) {
            ++diffs;
            if (shown < kCompareDiffCap) {
                ArLine line(fmt::format("${:08X}: {:02X}  ${:08X}: {:02X}",
                                        *start + static_cast<uint32_t>(i), a,
                                        *dest + static_cast<uint32_t>(i), b));
                line.Span(0, 9, ArColor::Address);
                line.Span(15, 9, ArColor::Address);
                line.Span(11, 2, ArColor::HexData);
                line.Span(26, 2, ArColor::Highlight);
                Emit(line);
                ++shown;
            }
        }
    }
    if (diffs > shown)
        Info(fmt::format("Output capped at {} differences.", kCompareDiffCap));
    Info(fmt::format("{} difference{}.", diffs, diffs == 1 ? "" : "s"));
}

void ArCommandInterpreter::CmdTransfer(const std::string& args)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.size() != 3) {
        Error("Usage: TRANS <start> <end> <dest>");
        return;
    }
    auto start = ParseNumber(tokens[0]);
    auto end = ParseNumber(tokens[1]);
    auto dest = ParseNumber(tokens[2]);
    if (!start || !end || !dest || *end < *start) {
        Error("Error: bad address");
        return;
    }

    size_t len = static_cast<size_t>(*end - *start) + 1;
    std::vector<uint8_t> buf(len);
    std::vector<bool> valid;
    if (m_mem->Read(*start, buf.data(), len, &valid) != len) {
        Error("Error: source range unmapped");
        return;
    }
    std::string err;
    if (!m_mem->Write(*dest, buf.data(), len, err)) {
        Error("Error: " + err);
        return;
    }
    StateChanged();
    Info(fmt::format("{} bytes copied to ${:08X}", len, *dest));
}

void ArCommandInterpreter::CmdExchange(const std::string& args)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.size() != 3) {
        Error("Usage: EX <start> <end> <dest>");
        return;
    }
    auto start = ParseNumber(tokens[0]);
    auto end = ParseNumber(tokens[1]);
    auto dest = ParseNumber(tokens[2]);
    if (!start || !end || !dest || *end < *start) {
        Error("Error: bad address");
        return;
    }

    size_t len = static_cast<size_t>(*end - *start) + 1;
    std::vector<uint8_t> a(len), b(len);
    if (m_mem->Read(*start, a.data(), len) != len || m_mem->Read(*dest, b.data(), len) != len) {
        Error("Error: range unmapped");
        return;
    }
    std::string err;
    if (!m_mem->Write(*start, b.data(), len, err) || !m_mem->Write(*dest, a.data(), len, err)) {
        Error("Error: " + err);
        return;
    }
    StateChanged();
    Info(fmt::format("{} bytes exchanged.", len));
}

void ArCommandInterpreter::CmdFill(const std::string& args)
{
    if (!RequireRunning())
        return;
    std::string left, right;
    if (!SplitAtLastComma(args, left, right)) {
        Error("Usage: O <string|bytes>, <start> <end>");
        return;
    }
    std::vector<uint8_t> pattern;
    if (!ParsePattern(left, pattern)) {
        Error("Error: bad pattern");
        return;
    }
    auto tokens = Tokenize(right);
    if (tokens.size() != 2) {
        Error("Usage: O <string|bytes>, <start> <end>");
        return;
    }
    auto start = ParseNumber(tokens[0]);
    auto end = ParseNumber(tokens[1]);
    if (!start || !end || *end < *start) {
        Error("Error: bad address");
        return;
    }

    size_t len = static_cast<size_t>(*end - *start) + 1;
    std::vector<uint8_t> buf(len);
    for (size_t i = 0; i < len; ++i)
        buf[i] = pattern[i % pattern.size()];
    std::string err;
    if (!m_mem->Write(*start, buf.data(), len, err)) {
        Error("Error: " + err);
        return;
    }
    StateChanged();
    Info(fmt::format("{} bytes filled.", len));
}

void ArCommandInterpreter::CmdWriteString(const std::string& args)
{
    if (!RequireRunning())
        return;
    std::string left, right;
    if (!SplitAtLastComma(args, left, right)) {
        Error("Usage: WS <string|bytes>, <addr>");
        return;
    }
    std::vector<uint8_t> pattern;
    if (!ParsePattern(left, pattern)) {
        Error("Error: bad pattern");
        return;
    }
    auto addr = ParseNumber(right);
    if (!addr) {
        Error("Error: bad address");
        return;
    }
    std::string err;
    if (!m_mem->Write(*addr, pattern.data(), pattern.size(), err)) {
        Error("Error: " + err);
        return;
    }
    StateChanged();
    Info(fmt::format("{} bytes written to ${:08X}", pattern.size(), *addr));
}

void ArCommandInterpreter::CmdMemCode(const std::string& args)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.size() != 3) {
        Error("Usage: MEMCODE <start> <end> <code>");
        return;
    }
    auto start = ParseNumber(tokens[0]);
    auto end = ParseNumber(tokens[1]);
    auto code = ParseNumber(tokens[2]);
    if (!start || !end || !code || *end < *start || *code > 0xFF) {
        Error("Error: bad arguments");
        return;
    }

    size_t len = static_cast<size_t>(*end - *start) + 1;
    std::vector<uint8_t> buf(len);
    if (m_mem->Read(*start, buf.data(), len) != len) {
        Error("Error: range unmapped");
        return;
    }
    for (auto& b : buf)
        b ^= static_cast<uint8_t>(*code);
    std::string err;
    if (!m_mem->Write(*start, buf.data(), len, err)) {
        Error("Error: " + err);
        return;
    }
    StateChanged();
    Info(fmt::format("{} bytes EOR'd with ${:02X}", len, *code));
}

void ArCommandInterpreter::CmdAdd(const std::string& args)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.size() != 3) {
        Error("Usage: ADD <start> <end> <value>");
        return;
    }
    auto start = ParseNumber(tokens[0]);
    auto end = ParseNumber(tokens[1]);
    auto value = ParseNumber(tokens[2]);
    if (!start || !end || !value || *end < *start || *value > 0xFF) {
        Error("Error: bad arguments");
        return;
    }

    size_t len = static_cast<size_t>(*end - *start) + 1;
    std::vector<uint8_t> buf(len);
    if (m_mem->Read(*start, buf.data(), len) != len) {
        Error("Error: range unmapped");
        return;
    }
    for (auto& b : buf)
        b = static_cast<uint8_t>(b + *value);
    std::string err;
    if (!m_mem->Write(*start, buf.data(), len, err)) {
        Error("Error: " + err);
        return;
    }
    StateChanged();
    Info(fmt::format("Added ${:02X} to {} bytes", *value, len));
}

// ---------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdLoadMem(const std::string& args)
{
    if (!RequireRunning())
        return;
    std::string file, addrPart;
    if (!SplitAtLastComma(args, file, addrPart)) {
        Error("Usage: LM <file>,<dest-addr>");
        return;
    }
    auto addr = ParseNumber(addrPart);
    if (!addr) {
        Error("Error: bad address");
        return;
    }
    if (file.empty()) {
        if (!m_host.requestOpenFile) {
            Error("Error: no file given");
            return;
        }
        auto picked = m_host.requestOpenFile();
        if (!picked)
            return; // dialog cancelled
        file = *picked;
    }

    std::ifstream f(file, std::ios::binary);
    if (!f.is_open()) {
        Error("Error: cannot open file");
        return;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (data.empty()) {
        Error("Error: file is empty");
        return;
    }
    std::string err;
    if (!m_mem->Write(*addr, data.data(), data.size(), err)) {
        Error("Error: " + err);
        return;
    }
    StateChanged();
    Info(fmt::format("{} bytes loaded to ${:08X}", data.size(), *addr));
}

void ArCommandInterpreter::CmdSaveMem(const std::string& args)
{
    if (!RequireRunning())
        return;
    std::string file, rangePart;
    if (!SplitAtLastComma(args, file, rangePart)) {
        Error("Usage: SM <file>,<start> <end>");
        return;
    }
    auto tokens = Tokenize(rangePart);
    if (tokens.size() != 2) {
        Error("Usage: SM <file>,<start> <end>");
        return;
    }
    auto start = ParseNumber(tokens[0]);
    auto end = ParseNumber(tokens[1]);
    if (!start || !end || *end < *start) {
        Error("Error: bad address");
        return;
    }
    if (file.empty()) {
        if (!m_host.requestSaveFile) {
            Error("Error: no file given");
            return;
        }
        auto picked = m_host.requestSaveFile();
        if (!picked)
            return;
        file = *picked;
    }

    size_t len = static_cast<size_t>(*end - *start) + 1;
    std::vector<uint8_t> buf(len);
    if (m_mem->Read(*start, buf.data(), len) != len) {
        Error("Error: range unmapped");
        return;
    }
    std::ofstream f(file, std::ios::binary);
    if (!f.is_open()) {
        Error("Error: cannot create file");
        return;
    }
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    Info(fmt::format("{} bytes saved.", len));
}

// ---------------------------------------------------------------------------
// Trainer (AR5 manual chapter 8): TS/T/TX search, TF/TFD sub finder,
// TDS/TDC/TD/TDI/TDD/TDX deep trainer
// ---------------------------------------------------------------------------

namespace {
constexpr size_t kTrainerShowCap = 32; // addresses displayed per listing
} // namespace

void ArCommandInterpreter::EmitTrainerAddresses(const std::vector<uint32_t>& addrs)
{
    const size_t shown = std::min(addrs.size(), kTrainerShowCap);
    std::string row;
    int inRow = 0;
    for (size_t i = 0; i < shown; ++i) {
        if (inRow)
            row += "  ";
        row += fmt::format("${:08X}", addrs[i]);
        if (++inRow == 4) {
            Emit(row, ArColor::Address);
            row.clear();
            inRow = 0;
        }
    }
    if (inRow)
        Emit(row, ArColor::Address);
    if (addrs.size() > shown)
        Emit(fmt::format("... and {} more", addrs.size() - shown), ArColor::Default);
}

void ArCommandInterpreter::EmitTrainerStatus(const std::string& text)
{
    Emit(text, ArColor::Highlight);
}

void ArCommandInterpreter::EmitLabeledAddress(const std::string& prefix, uint32_t addr,
                                              int hexDigits)
{
    ArLine line(prefix + fmt::format("${:0{}X}", addr, hexDigits));
    line.Span(static_cast<int>(prefix.size()), hexDigits + 1, ArColor::Address);
    Emit(line);
}

// ---------------------------------------------------------------------------
// Memory protection (trainer locks) - PR / PROTECT
//
// Grammar mirrors ENGINE9000's `protect`, which is where the block/set split comes from.
// Unlike the TS/TD searches above, which only *find* addresses, these turn a found address
// into a live cheat: block and set are enforced inside the core's memory write path, so they
// hold sub-frame rather than being a once-per-frame clamp.
// ---------------------------------------------------------------------------

namespace {

// "SIZE=16" is in bits, matching ENGINE9000 and how trainer codes are usually written.
// Returns 0 when the token is not a SIZE= at all, or the width is not 8/16/32.
uint8_t ParseSizeBits(const std::string& tok)
{
    if (tok.rfind("SIZE=", 0) != 0)
        return 0;
    const std::string v = tok.substr(5);
    if (v == "8")
        return 1;
    if (v == "16")
        return 2;
    if (v == "32")
        return 4;
    return 0;
}

} // namespace

void ArCommandInterpreter::CmdProtect(const std::string& args)
{
    if (!m_host.locks) {
        Error("Error: memory locks unavailable");
        return;
    }
    MemoryLocks& locks = *m_host.locks;

    LockContext ctx;
    ctx.mem = m_mem;
    if (m_host.getSegments)
        ctx.segments = m_host.getSegments();

    auto tokens = Tokenize(args);
    auto upper = [](std::string t) {
        for (auto& c : t)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return t;
    };

    // --- PR : list -----------------------------------------------------------------
    if (tokens.empty()) {
        if (locks.Empty()) {
            Info("No memory locks set.");
            return;
        }
        EmitTableHead("Id    On   Mode   Address    Size  Value      State            Description");
        for (const auto& e : locks.All()) {
            const std::string addr =
                e.resolved ? fmt::format("${:08X}", *e.resolved) : std::string("-        ");
            std::string sizeCol;
            std::string valueCol;
            switch (e.mode) {
                case LockMode::Block:
                    sizeCol = fmt::format("{}", e.len);
                    valueCol = "-";
                    break;
                case LockMode::Set:
                    sizeCol = fmt::format("{}", e.size);
                    valueCol = fmt::format("${:0{}X}", e.value, e.size * 2);
                    break;
                case LockMode::Patch:
                    sizeCol = fmt::format("{}", e.bytes.size());
                    valueCol = e.applied ? "applied" : "-";
                    break;
            }
            ArLine line(fmt::format("{:<5} {:<4} {:<6} {:<10} {:<5} {:<10} {:<16} {}", e.id,
                                    e.enabled ? "on" : "off", ToString(e.mode), addr, sizeCol,
                                    valueCol, ToString(e.state), e.desc));
            if (e.resolved)
                line.Span(23, 9, ArColor::Address);
            Emit(line);
        }
        // The state word alone rarely says enough; the reason is what the user acts on.
        for (const auto& e : locks.All()) {
            if (!e.note.empty() && e.state != ResolveState::Resolved)
                Emit(fmt::format("  {}: {}", e.id, e.note), ArColor::Default);
        }
        if (m_host.supportsProtections && !m_host.supportsProtections()) {
            Emit("Note: this PuAE core has no protection hooks - rebuild it with "
                 "scripts/build_puae, or locks will have no effect.",
                 ArColor::Highlight);
        }
        return;
    }

    const std::string verb = upper(tokens[0]);

    // --- PR CLEAR ------------------------------------------------------------------
    if (verb == "CLEAR") {
        // Revert patches first, or their bytes stay in guest memory with nothing left that
        // remembers how to put the originals back.
        for (const auto& e : locks.All()) {
            if (e.mode == LockMode::Patch && e.applied) {
                std::string err;
                locks.SetEnabled(e.id, false, ctx, err);
            }
        }
        locks.Clear();
        Info("All memory locks removed.");
        StateChanged();
        return;
    }

    // --- PR RESCAN -----------------------------------------------------------------
    if (verb == "RESCAN") {
        if (!m_mem || !m_mem->HasRegions()) {
            Error("Error: no memory (is the emulator running?)");
            return;
        }
        locks.ResolveAll(ctx);
        size_t ok = 0;
        for (const auto& e : locks.All()) {
            if (e.state == ResolveState::Resolved)
                ++ok;
        }
        Info(fmt::format("Re-resolved {} lock(s): {} resolved, {} not.", locks.Size(), ok,
                         locks.Size() - ok));
        StateChanged();
        return;
    }

    // --- PR ON|OFF <id> ------------------------------------------------------------
    if (verb == "ON" || verb == "OFF") {
        if (tokens.size() < 2) {
            Error(fmt::format("Usage: PR {} <id>", verb));
            return;
        }
        std::string err;
        if (!locks.SetEnabled(tokens[1], verb == "ON", ctx, err)) {
            Error(fmt::format("Error: {}", err));
            return;
        }
        Info(fmt::format("Lock {} {}.", tokens[1], verb == "ON" ? "enabled" : "disabled"));
        StateChanged();
        return;
    }

    // --- PR DEL <addr|id> ----------------------------------------------------------
    if (verb == "DEL" || verb == "DELETE") {
        if (tokens.size() < 2) {
            Error("Usage: PR DEL <addr|id>");
            return;
        }
        // Either form works, because the listing shows both.
        if (locks.Find(tokens[1])) {
            std::string err;
            locks.SetEnabled(tokens[1], false, ctx, err);
            locks.Remove(tokens[1]);
            Info(fmt::format("Lock {} removed.", tokens[1]));
            StateChanged();
            return;
        }
        auto addr = ParseNumber(tokens[1]);
        if (!addr) {
            Error("Usage: PR DEL <addr|id>");
            return;
        }
        // Revert any patch that covers the address before dropping the entry.
        for (const auto& e : locks.All()) {
            if (e.mode == LockMode::Patch && e.applied && e.resolved && *addr >= *e.resolved &&
                *addr - *e.resolved < e.Span()) {
                std::string err;
                locks.SetEnabled(e.id, false, ctx, err);
            }
        }
        const size_t n = locks.RemoveCovering(*addr);
        if (n == 0) {
            Error(fmt::format("Error: no lock at ${:08X}", *addr));
            return;
        }
        Info(fmt::format("Removed {} lock(s) at ${:08X}.", n, *addr));
        StateChanged();
        return;
    }

    // --- PR LOAD|SAVE|IMPORT <path> ------------------------------------------------
    if (verb == "LOAD" || verb == "SAVE" || verb == "IMPORT") {
        if (tokens.size() < 2) {
            Error(fmt::format("Usage: PR {} <path>", verb));
            return;
        }
        const std::string path = tokens[1];
        std::string err;

        if (verb == "SAVE") {
            TrainerDoc doc;
            doc.entries = locks.All();
            if (!TrainerFile::Save(path, doc, err)) {
                Error(fmt::format("Error: {}", err));
                return;
            }
            Info(fmt::format("Saved {} lock(s) to {}.", doc.entries.size(), path));
            return;
        }

        int skipped = 0;
        auto doc = verb == "LOAD" ? TrainerFile::Load(path, err)
                                  : TrainerFile::ImportCht(path, err, &skipped);
        if (!doc) {
            Error(fmt::format("Error: {}", err));
            return;
        }
        // Everything arrives disabled. A trainer authored against some other release has to
        // resolve and verify against *this* one before it is allowed to write to memory.
        for (auto& e : doc->entries) {
            e.enabled = false;
            e.applied = false;
            e.id.clear();
        }
        const size_t n = doc->entries.size();
        locks.ReplaceAll(std::move(doc->entries));
        if (m_mem && m_mem->HasRegions())
            locks.ResolveAll(ctx);
        Info(fmt::format("Loaded {} entr{} from {} - all disabled; review, then PR ON <id>.", n,
                         n == 1 ? "y" : "ies", path));
        if (skipped > 0)
            Emit(fmt::format("  {} cheat(s) skipped: no Amiga equivalent.", skipped),
                 ArColor::Highlight);
        if (!err.empty())
            Emit(fmt::format("  warnings: {}", err), ArColor::Highlight);
        StateChanged();
        return;
    }

    // --- PR <addr> BLOCK|SET=<v> ... -----------------------------------------------
    auto addr = ParseNumber(tokens[0]);
    if (!addr) {
        Error("Usage: PR (<addr> BLOCK|SET=<value> | DEL <addr> | ON|OFF <id> | CLEAR | "
              "RESCAN | LOAD|SAVE|IMPORT <path>)");
        return;
    }

    LockEntry e;
    e.anchor.kind = AnchorKind::Absolute;
    e.anchor.addr = *addr;
    bool haveMode = false;

    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string tok = upper(tokens[i]);
        if (tok == "BLOCK") {
            e.mode = LockMode::Block;
            haveMode = true;
        } else if (tok.rfind("SET=", 0) == 0) {
            auto v = ParseNumber(tokens[i].substr(4));
            if (!v) {
                Error(fmt::format("Error: '{}' is not a value", tokens[i].substr(4)));
                return;
            }
            e.mode = LockMode::Set;
            e.value = *v;
            haveMode = true;
        } else if (tok.rfind("LEN=", 0) == 0) {
            auto v = ParseNumber(tokens[i].substr(4));
            if (!v || *v == 0) {
                Error("Error: LEN must be at least 1");
                return;
            }
            e.len = *v;
        } else if (const uint8_t sz = ParseSizeBits(tok)) {
            e.size = sz;
        } else if (tok.rfind("SIZE=", 0) == 0) {
            Error("Error: SIZE must be 8, 16 or 32");
            return;
        } else {
            // Anything left over is the description, so `PR 1000 BLOCK infinite lives` reads
            // the way it looks.
            if (!e.desc.empty())
                e.desc += ' ';
            e.desc += tokens[i];
        }
    }

    if (!haveMode) {
        Error("Usage: PR <addr> BLOCK (LEN=<n>) | PR <addr> SET=<value> (SIZE=8|16|32)");
        return;
    }
    if (e.mode == LockMode::Set) {
        // A forced value protects exactly its own width, so LEN would be meaningless.
        e.len = e.size;
        const uint32_t max = e.size == 1 ? 0xFFu : (e.size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
        if (e.value > max) {
            Error(fmt::format("Error: ${:X} does not fit in {} bits", e.value, e.size * 8));
            return;
        }
    }
    if (e.desc.empty()) {
        e.desc = e.mode == LockMode::Block ? fmt::format("block ${:08X}", *addr)
                                           : fmt::format("set ${:08X}", *addr);
    }

    const std::string id = locks.Add(e);
    std::string err;
    if (!locks.SetEnabled(id, true, ctx, err)) {
        // Kept, so the user can see it and fix the anchor - but off, and nothing was written.
        Error(fmt::format("Error: {} (lock {} added, disabled)", err, id));
        StateChanged();
        return;
    }
    const LockEntry* added = locks.Find(id);
    const uint32_t at = added && added->resolved ? *added->resolved : *addr;
    if (e.mode == LockMode::Block)
        Info(fmt::format("Lock {} set: blocking {} byte(s) at ${:08X}.", id, e.len, at));
    else
        Info(fmt::format("Lock {} set: forcing ${:0{}X} at ${:08X}.", id, e.value, e.size * 2, at));

    if (m_host.supportsProtections && !m_host.supportsProtections()) {
        Emit("Note: this PuAE core has no protection hooks - rebuild it with "
             "scripts/build_puae, or the lock will have no effect.",
             ArColor::Highlight);
    }
    StateChanged();
}

void ArCommandInterpreter::CmdTrainerStart(const std::string& args)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    std::optional<uint32_t> value = tokens.empty() ? std::nullopt : ParseNumber(tokens[0]);
    if (!value || *value > 0xFF) {
        Error("Usage: TS <value> (<start>)");
        return;
    }
    std::optional<uint32_t> startAddr;
    if (tokens.size() > 1) {
        startAddr = ParseNumber(tokens[1]);
        if (!startAddr) {
            Error("Error: bad start address");
            return;
        }
    }

    m_trainer.active = true;
    m_trainer.candidates.clear();
    const uint8_t target = static_cast<uint8_t>(*value);
    uint32_t searchedTo = startAddr.value_or(0);
    for (const auto& r : m_mem->Regions()) {
        if (!r.writable)
            continue;
        uint32_t lo = r.base;
        const uint32_t hi = r.base + r.size - 1;
        if (startAddr) {
            if (*startAddr > hi)
                continue;
            lo = std::max(lo, *startAddr);
        }
        const uint8_t* p = r.hostPtr + (lo - r.base);
        for (uint32_t a = lo; a <= hi; ++a, ++p) {
            if (*p == target)
                m_trainer.candidates.push_back(a);
        }
        searchedTo = std::max(searchedTo, hi);
    }
    EmitTrainerStatus(fmt::format("Trainer started — {} candidates holding ${:02X}.",
                                  m_trainer.candidates.size(), target));
    EmitLabeledAddress("Searched up to: ", searchedTo, 6);
    Emit("Change the value in-game, then run  T <newvalue>  to narrow.");
}

void ArCommandInterpreter::CmdTrainerContinue(const std::string& args)
{
    if (!RequireRunning())
        return;
    if (!m_trainer.active) {
        Error("Error: not in trainer mode (use TS <value>)");
        return;
    }
    auto tokens = Tokenize(args);
    std::optional<uint32_t> value = tokens.empty() ? std::nullopt : ParseNumber(tokens[0]);
    if (!value || *value > 0xFF) {
        Error("Usage: T <value>");
        return;
    }

    const uint8_t target = static_cast<uint8_t>(*value);
    std::vector<uint32_t> kept;
    for (uint32_t a : m_trainer.candidates) {
        bool ok = false;
        if (m_mem->ReadByte(a, &ok) == target && ok)
            kept.push_back(a);
    }
    m_trainer.candidates = std::move(kept);

    if (m_trainer.candidates.empty()) {
        EmitTrainerStatus(fmt::format("No candidates match ${:02X}.", target));
        return;
    }
    EmitTrainerStatus(
        fmt::format("{} candidate{} still match ${:02X}:", m_trainer.candidates.size(),
                    m_trainer.candidates.size() == 1 ? "" : "s", target));
    EmitTrainerAddresses(m_trainer.candidates);
}

void ArCommandInterpreter::CmdTrainerExit(const std::string&)
{
    if (!m_trainer.active) {
        Error("Error: not in trainer mode");
        return;
    }
    m_trainer = {};
    EmitTrainerStatus("Trainer cleared.");
}

void ArCommandInterpreter::CmdTrainerFind(const std::string& args, bool remove)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    std::optional<uint32_t> addr = tokens.empty() ? std::nullopt : ParseNumber(tokens[0]);
    if (!addr) {
        Error(remove ? "Usage: TFD <addr> (<start> <end>)" : "Usage: TF <addr> (<start> <end>)");
        return;
    }
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    if (!ResolveSearchRange(tokens, 1, ranges)) {
        Error("Error: bad range");
        return;
    }

    const uint32_t target = *addr;
    auto rd16 = [](const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); };
    auto rd32 = [](const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
               static_cast<uint32_t>(p[2]) << 8 | p[3];
    };
    auto sext = [](uint16_t w) { return static_cast<uint32_t>(static_cast<int16_t>(w)); };

    struct Hit
    {
        uint32_t at;
        uint32_t len;
    };
    std::vector<Hit> hits;
    for (const auto& [start, end] : ranges) {
        for (const auto& r : m_mem->Regions()) {
            if (!r.writable)
                continue;
            uint32_t lo = std::max(start, r.base);
            const uint32_t hi = std::min(end, r.base + r.size - 1);
            if (lo > hi)
                continue;
            lo += lo & 1; // 68k instructions are word-aligned
            for (uint32_t a = lo; a + 3 <= hi; a += 2) {
                const uint8_t* p = r.hostPtr + (a - r.base);
                const uint32_t room = hi - a + 1;
                const uint16_t op = rd16(p);
                uint32_t len = 0;
                // SUBQ.B/W/L #1, abs.w / abs.l
                if (op == 0x5338 || op == 0x5378 || op == 0x53B8) {
                    if (room >= 4 && sext(rd16(p + 2)) == target)
                        len = 4;
                } else if (op == 0x5339 || op == 0x5379 || op == 0x53B9) {
                    if (room >= 6 && rd32(p + 2) == target)
                        len = 6;
                }
                // SUBI.B/W #1, abs.w / abs.l (immediate word 0001 first)
                else if (op == 0x0438 || op == 0x0478) {
                    if (room >= 6 && rd16(p + 2) == 0x0001 && sext(rd16(p + 4)) == target)
                        len = 6;
                } else if (op == 0x0439 || op == 0x0479) {
                    if (room >= 8 && rd16(p + 2) == 0x0001 && rd32(p + 4) == target)
                        len = 8;
                }
                // SUBI.L #1, abs.w / abs.l (immediate long 00000001 first)
                else if (op == 0x04B8) {
                    if (room >= 8 && rd32(p + 2) == 1 && sext(rd16(p + 6)) == target)
                        len = 8;
                } else if (op == 0x04B9) {
                    if (room >= 10 && rd32(p + 2) == 1 && rd32(p + 6) == target)
                        len = 10;
                }
                if (len)
                    hits.push_back({a, len});
            }
        }
    }

    for (const auto& h : hits)
        EmitLabeledAddress("Decrement found at ", h.at, 8);
    if (hits.empty()) {
        EmitTrainerStatus("No decrement instructions found.");
        return;
    }
    if (!remove)
        return;
    static const uint8_t kNops[10] = {0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71};
    for (const auto& h : hits) {
        std::string err;
        if (!m_mem->Write(h.at, kNops, h.len, err)) {
            Error("Error: " + err);
            return;
        }
    }
    StateChanged();
    EmitTrainerStatus(
        fmt::format("{} decrement instruction{} NOP'd.", hits.size(), hits.size() == 1 ? "" : "s"));
}

bool ArCommandInterpreter::DeepSnapshot()
{
    m_deep.snapshot.clear();
    for (const auto& r : m_mem->Regions()) {
        if (r.writable)
            m_deep.snapshot.push_back(
                {r.base, std::vector<uint8_t>(r.hostPtr, r.hostPtr + r.size)});
    }
    return !m_deep.snapshot.empty();
}

void ArCommandInterpreter::CmdDeepStart(const std::string&)
{
    if (!RequireRunning())
        return;
    m_deep = {};
    if (!DeepSnapshot()) {
        Error("Error: no RAM to inspect");
        return;
    }
    m_deep.active = true;
    EmitTrainerStatus("Deep trainer armed — snapshotted writable RAM.");
    Emit("Change the value in-game, then run TDC to find what changed.");
}

void ArCommandInterpreter::CmdDeepChanged(const std::string&)
{
    if (!RequireRunning())
        return;
    if (!m_deep.active) {
        Error("Error: deep trainer not active (use TDS)");
        return;
    }

    // The snapshot is only comparable while the memory layout is unchanged
    for (const auto& s : m_deep.snapshot) {
        const auto* r = m_mem->FindRegion(s.base);
        if (!r || r->base != s.base || r->size != s.bytes.size()) {
            m_deep = {};
            Error("Error: memory layout changed - deep trainer cleared");
            return;
        }
    }

    if (m_deep.firstPass) {
        m_deep.candidates.clear();
        for (const auto& s : m_deep.snapshot) {
            const auto* r = m_mem->FindRegion(s.base);
            for (uint32_t i = 0; i < s.bytes.size(); ++i) {
                if (r->hostPtr[i] != s.bytes[i])
                    m_deep.candidates.push_back(s.base + i);
            }
        }
        m_deep.firstPass = false;
    } else {
        std::vector<uint32_t> kept;
        for (uint32_t a : m_deep.candidates) {
            for (const auto& s : m_deep.snapshot) {
                if (a >= s.base && a - s.base < s.bytes.size()) {
                    const auto* r = m_mem->FindRegion(s.base);
                    if (r->hostPtr[a - s.base] != s.bytes[a - s.base])
                        kept.push_back(a);
                    break;
                }
            }
        }
        m_deep.candidates = std::move(kept);
    }
    DeepSnapshot();

    if (m_deep.candidates.empty()) {
        EmitTrainerStatus("No addresses changed since last snapshot.");
        return;
    }
    EmitTrainerStatus(
        fmt::format("{} address{} changed since last snapshot:", m_deep.candidates.size(),
                    m_deep.candidates.size() == 1 ? "" : "es"));
    if (m_deep.candidates.size() <= kTrainerShowCap)
        EmitTrainerAddresses(m_deep.candidates);
}

void ArCommandInterpreter::CmdDeepList(const std::string&)
{
    if (!RequireRunning())
        return;
    if (!m_deep.active) {
        Error("Error: deep trainer not active (use TDS)");
        return;
    }
    if (m_deep.firstPass) {
        EmitTrainerStatus("Run TDC first to narrow the candidate set.");
        return;
    }
    if (m_deep.candidates.empty()) {
        EmitTrainerStatus("No candidates remain.");
        return;
    }
    const size_t shown = std::min(m_deep.candidates.size(), kTrainerShowCap);
    for (size_t i = 0; i < shown; ++i) {
        const uint32_t a = m_deep.candidates[i];
        bool ok = false;
        const uint8_t v = m_mem->ReadByte(a, &ok);
        ArLine line(fmt::format("${:08X} = {:02X}", a, v));
        line.Span(0, 9, ArColor::Address);
        line.Span(12, 2, ArColor::HexData);
        Emit(line);
    }
    if (m_deep.candidates.size() > shown)
        Info(fmt::format("... and {} more", m_deep.candidates.size() - shown));
}

void ArCommandInterpreter::CmdDeepInspect(const std::string&)
{
    if (!RequireRunning())
        return;
    if (!m_deep.active) {
        Error("Error: deep trainer not active (use TDS)");
        return;
    }
    if (m_deep.firstPass) {
        EmitTrainerStatus("Run TDC first to narrow the candidate set.");
        return;
    }
    if (m_deep.candidates.empty()) {
        EmitTrainerStatus("No candidates remain.");
        return;
    }
    EmitTrainerStatus(fmt::format("{} candidate{}:", m_deep.candidates.size(),
                                  m_deep.candidates.size() == 1 ? "" : "s"));
    EmitTrainerAddresses(m_deep.candidates);
}

void ArCommandInterpreter::CmdDeepDelete(const std::string& args)
{
    if (!m_deep.active) {
        Error("Error: deep trainer not active (use TDS)");
        return;
    }
    auto tokens = Tokenize(args);
    std::optional<uint32_t> start = tokens.empty() ? std::nullopt : ParseNumber(tokens[0]);
    std::optional<uint32_t> end = tokens.size() > 1 ? ParseNumber(tokens[1]) : std::nullopt;
    if (!start || !end || *end < *start) {
        Error("Usage: TDD <start> <end>");
        return;
    }
    auto& c = m_deep.candidates;
    c.erase(
        std::remove_if(c.begin(), c.end(), [&](uint32_t a) { return a >= *start && a <= *end; }),
        c.end());
    EmitTrainerStatus(fmt::format("{} candidate{} remain.", c.size(), c.size() == 1 ? "" : "s"));
}

void ArCommandInterpreter::CmdDeepExit(const std::string&)
{
    if (!m_deep.active) {
        Error("Error: deep trainer not active");
        return;
    }
    m_deep = {};
    EmitTrainerStatus("Deep trainer cleared.");
}

// ---------------------------------------------------------------------------
// Assembler / help
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdAssemble(const std::string& args)
{
    if (!m_host.assembler) {
        Error("Error: assembler not installed");
        return;
    }
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    std::optional<uint32_t> addr = tokens.empty() ? std::nullopt : ParseNumber(tokens[0]);
    if (!addr) {
        Error("Usage: A <addr>");
        return;
    }
    m_curAddr = *addr & ~1u;
    m_mode = Mode::Assemble;
    Info(fmt::format("Assembling at ${:08X} - type over, ESC to exit.", m_curAddr));
    // Offer the existing instruction for type-over (AR style)
    if (!EmitDisasmLine(m_curAddr))
        m_mode = Mode::None;
}

void ArCommandInterpreter::CmdBreakBootblock(const std::string&)
{
    if (!RequireRunning())
        return;
    if (!m_host.armBootblockBreak) {
        Error("Error: FB unavailable (rebuild core)");
        return;
    }
    // Toggle: disarm if already armed, otherwise arm and run to boot.
    if (m_host.isBootblockArmed && m_host.isBootblockArmed()) {
        m_host.armBootblockBreak(false);
        Info("Bootblock break disarmed.");
        return;
    }
    m_host.armBootblockBreak(true);
    Info("Break on bootblock entry armed - resume to boot.");
    if (m_host.resume)
        m_host.resume();
}

void ArCommandInterpreter::CmdExceptionBreak(const std::string& args)
{
    if (!m_host.armExceptionBreak) {
        Error("Error: EB unavailable (rebuild core)");
        return;
    }
    // name/alias -> 68k vector number
    struct Alias
    {
        const char* k;
        int v;
    };
    static const Alias kAliases[] = {
        {"BUS", 2},  {"ADDR", 3},      {"ADDRESS", 3}, {"ILLEGAL", 4}, {"ILL", 4},
        {"ZERO", 5}, {"DIV", 5},       {"DIVZERO", 5}, {"CHK", 6},     {"TRAPV", 7},
        {"PRIV", 8}, {"PRIVILEGE", 8}, {"TRACE", 9},   {"LINEA", 10},  {"LINEF", 11},
    };

    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: BUS/ADDR/ILLEGAL/ZERO/CHK/TRAPV/PRIV/TRACE/LINEA/LINEF/TRAP0..15, ALL, OFF");
        Error("e.g. EB ILLEGAL ZERO   |   EB ALL   |   EB OFF");
        return;
    }

    uint64_t mask = 0;
    std::string first = ToUpper(tokens[0]);
    if (first == "OFF") {
        mask = 0;
    } else if (first == "ALL") {
        mask = ~0ull; // core masks out interrupt vectors 24..31
    } else {
        for (const auto& t : tokens) {
            std::string up = ToUpper(t);
            int vec = -1;
            if (up.rfind("TRAP", 0) == 0 && up.size() > 4) {
                int n = std::atoi(up.c_str() + 4);
                if (n >= 0 && n <= 15)
                    vec = 32 + n;
            }
            if (vec < 0) {
                for (const auto& a : kAliases)
                    if (up == a.k) {
                        vec = a.v;
                        break;
                    }
            }
            if (vec < 0) {
                // plain decimal vector number
                if (auto v = ParseNumber(t))
                    vec = static_cast<int>(*v);
            }
            if (vec < 0 || vec > 63) {
                Error(fmt::format("Error: unknown exception '{}'", t));
                return;
            }
            mask |= (1ull << vec);
        }
    }

    m_host.armExceptionBreak(mask);
    if (mask == 0)
        Info("Exception breaks disarmed.");
    else
        Info(fmt::format("Exception break armed (mask ${:08X}{:08X}).",
                         static_cast<uint32_t>(mask >> 32), static_cast<uint32_t>(mask)));
    StateChanged();
}

void ArCommandInterpreter::CmdBeamBreak(const std::string& args)
{
    if (!m_host.armBeamBreak) {
        Error("Error: BL unavailable (rebuild core)");
        return;
    }
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: BL <scanline> [hpos]   |   BL OFF   (hex: !227 for decimal 227)");
        return;
    }
    if (ToUpper(tokens[0]) == "OFF") {
        m_host.armBeamBreak(-1, -1);
        Info("Beam break disarmed.");
        StateChanged();
        return;
    }
    auto vp = ParseNumber(tokens[0]);
    if (!vp) {
        Error("Usage: BL <scanline> [hpos]   |   BL OFF   (hex: !227 for decimal 227)");
        return;
    }
    int hpos = -1;
    if (tokens.size() >= 2) {
        if (auto hp = ParseNumber(tokens[1]))
            hpos = static_cast<int>(*hp);
    }
    m_host.armBeamBreak(static_cast<int>(*vp), hpos);
    if (hpos >= 0)
        Info(fmt::format("Beam break armed at scanline {} (hpos {} best-effort) - resume to run.",
                         *vp, hpos));
    else
        Info(fmt::format("Beam break armed at scanline {} - resume to run.", *vp));
    StateChanged();
}

void ArCommandInterpreter::CmdCopperBreak(const std::string& args)
{
    if (!m_host.toggleCopperBreakpoint) {
        Error("Error: copper breakpoints unavailable");
        return;
    }
    auto tokens = Tokenize(args);
    std::optional<uint32_t> addr = tokens.empty() ? std::nullopt : ParseNumber(tokens[0]);
    if (!addr) {
        Error("Usage: BC <copper-addr>");
        return;
    }
    m_host.toggleCopperBreakpoint(*addr);
    Info(fmt::format("Toggled Copper breakpoint at ${:08X}", *addr));
    StateChanged();
}

void ArCommandInterpreter::CmdLoad(const std::string& args)
{
    std::string path = args;

    // A trailing "!" or " FORCE" replaces the open document even when it has
    // unsaved changes (mirrors the MCP force flag). Strip it before the path is
    // interpreted so it works with plain paths and the file:/empty forms.
    bool force = false;
    {
        auto rtrim = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };
        rtrim(path);
        if (!path.empty() && path.back() == '!') {
            force = true;
            path.pop_back();
            rtrim(path);
        } else {
            size_t sp = path.find_last_of(" \t");
            const std::string tail = sp == std::string::npos ? path : path.substr(sp + 1);
            std::string tailLower = tail;
            std::transform(tailLower.begin(), tailLower.end(), tailLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (tailLower == "force") {
                force = true;
                path = sp == std::string::npos ? std::string{} : path.substr(0, sp);
                rtrim(path);
            }
        }
    }

    auto parseDecimal = [](const std::string& s) -> std::optional<size_t> {
        if (s.empty())
            return std::nullopt;
        try {
            size_t parsed = 0;
            unsigned long v = std::stoul(s, &parsed, 10);
            if (parsed != s.size())
                return std::nullopt;
            return static_cast<size_t>(v);
        } catch (...) {
            return std::nullopt;
        }
    };

    // file:{path} — strip prefix, fall through to regular load
    if (path.size() > 5 && path.substr(0, 5) == "file:") {
        path = path.substr(5);
    }
    // doc:{name} — find open document by filename, boot it
    else if (path.size() > 4 && path.substr(0, 4) == "doc:") {
        const std::string name = path.substr(4);
        if (!m_host.getOpenDocuments) {
            Error("Error: docs not available");
            return;
        }
        const auto docs = m_host.getOpenDocuments();
        int found = -1;
        for (size_t i = 0; i < docs.size(); ++i) {
            const std::string n =
                docs[i].empty() ? "(untitled)" : std::filesystem::path(docs[i]).filename().string();
            if (n == name) {
                found = static_cast<int>(i);
                break;
            }
        }
        if (found < 0) {
            Error(fmt::format("Error: doc not found: {}", name));
            return;
        }
        if (!m_host.runDocumentByIndex) {
            Error("Error: run document not available");
            return;
        }
        const std::string docName = docs[found].empty()
                                        ? "(untitled)"
                                        : std::filesystem::path(docs[found]).filename().string();
        Info(fmt::format("Loading {} into emulator...", docName));
        m_host.runDocumentByIndex(found);
        StateChanged();
        return;
    }
    // f<n>[: ...] — load recent file by 1-based index (tolerates trailing ": file:..." from
    // type-over)
    else if (path.size() >= 2 && (path[0] == 'f' || path[0] == 'F')) {
        size_t end = 1;
        while (end < path.size() && std::isdigit(static_cast<unsigned char>(path[end])))
            ++end;
        auto num = end > 1 ? parseDecimal(path.substr(1, end - 1)) : std::nullopt;
        if (num) {
            if (!m_host.getRecentFiles) {
                Error("Error: recent files not available");
                return;
            }
            const auto files = m_host.getRecentFiles();
            if (*num < 1 || *num > files.size()) {
                Error(fmt::format("Error: bad file number (1-{})", files.size()));
                return;
            }
            path = files[*num - 1];
        }
    }
    // d<n>[: ...] — boot open document by 1-based index (tolerates trailing ": doc:..." from
    // type-over)
    else if (path.size() >= 2 && (path[0] == 'd' || path[0] == 'D')) {
        size_t end = 1;
        while (end < path.size() && std::isdigit(static_cast<unsigned char>(path[end])))
            ++end;
        auto num = end > 1 ? parseDecimal(path.substr(1, end - 1)) : std::nullopt;
        if (num) {
            if (!m_host.getOpenDocuments) {
                Error("Error: docs not available");
                return;
            }
            const auto docs = m_host.getOpenDocuments();
            if (*num < 1 || *num > docs.size()) {
                Error(fmt::format("Error: bad doc number (1-{})", docs.size()));
                return;
            }
            if (!m_host.runDocumentByIndex) {
                Error("Error: run document not available");
                return;
            }
            const int idx = static_cast<int>(*num - 1);
            const std::string name = docs[idx].empty()
                                         ? "(untitled)"
                                         : std::filesystem::path(docs[idx]).filename().string();
            Info(fmt::format("Loading {} into emulator...", name));
            m_host.runDocumentByIndex(idx);
            StateChanged();
            return;
        }
    }

    if (path.empty()) {
        if (!m_host.requestOpenFile) {
            Error("Error: no file dialog available");
            return;
        }
        auto result = m_host.requestOpenFile();
        if (!result) {
            Info("Load cancelled.");
            return;
        }
        path = *result;
    }
    if (!m_host.loadDocument) {
        Error("Error: load not available");
        return;
    }
    Info(fmt::format("Loading {}...", path));
    if (!m_host.loadDocument(path, force))
        Error(fmt::format("Error: failed to open {} (unsaved changes? add FORCE or ! to replace)",
                          std::filesystem::path(path).filename().string()));
}

void ArCommandInterpreter::CmdList(const std::string&)
{
    if (!m_host.getRecentFiles) {
        Error("Error: recent files not available");
        return;
    }
    const auto files = m_host.getRecentFiles();
    if (files.empty()) {
        Info("No recent files.");
        return;
    }
    EmitTableHead("Recent files");
    for (size_t i = 0; i < files.size(); ++i)
        Emit(fmt::format("  {:<4} file:{}", fmt::format("f{}:", i + 1), files[i]));
}

void ArCommandInterpreter::CmdReset(const std::string&)
{
    if (!RequireRunning())
        return;
    if (!m_host.resetEmulator) {
        Error("Error: reset unavailable");
        return;
    }
    Info("Resetting...");
    m_host.resetEmulator();
    StateChanged();
}

namespace {

std::string DescribeHardDrive(const HostDirectoryMount& mount)
{
    if (!mount.enabled)
        return "off";
    if (!mount.IsHardfile())
        return fmt::format("DIR {} '{}' PRI {} {}", mount.readOnly ? "RO" : "RW", mount.volumeLabel,
                           mount.bootPriority, mount.hostDirectory.string());
    std::string geometry = HardfileGeometryModeId(mount.geometry.mode);
    if (mount.geometry.mode == HardfileGeometryMode::Manual)
        geometry = fmt::format("{}/{}/{}/{}", mount.geometry.sectors, mount.geometry.surfaces,
                               mount.geometry.reserved, mount.geometry.blockSize);
    return fmt::format("HDF {} {} unit {} GEOM {} PRI {} {}", mount.readOnly ? "RO" : "RW",
                       HardDriveControllerId(mount.controller), mount.controllerUnit, geometry,
                       mount.bootPriority, mount.hardfilePath.string());
}

bool LooksLikeHardfile(const std::string& path)
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(std::filesystem::u8path(path), ec))
        return true;
    std::string extension = std::filesystem::u8path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".hdf" || extension == ".hdz";
}

} // namespace

void ArCommandInterpreter::CmdHardDrives(const std::string& args)
{
    if (!m_host.getConfiguredHostMounts || !m_host.getEffectiveHostMounts) {
        Error("Error: hard-drive configuration unavailable");
        return;
    }

    const auto parseDevice = [](const std::string& token) -> std::optional<HostMountDevice> {
        const std::string upper = ToUpper(token);
        if (upper == "DH0" || upper == "DH0:")
            return HostMountDevice::DH0;
        if (upper == "DH1" || upper == "DH1:")
            return HostMountDevice::DH1;
        return std::nullopt;
    };
    const auto tokens = Tokenize(args);
    if (tokens.empty()) {
        const auto configured = m_host.getConfiguredHostMounts();
        const auto effective = m_host.getEffectiveHostMounts();
        EmitTableHead("Hard drives (configured / effective)");
        for (size_t i = 0; i < configured.size(); ++i) {
            const bool pending = configured[i] != effective[i];
            Emit(fmt::format("  {}  C: {}", HostMountDeviceName(configured[i].device),
                             DescribeHardDrive(configured[i])));
            Emit(fmt::format("       E: {}{}", DescribeHardDrive(effective[i]),
                             pending ? "  [restart pending]" : ""));
            if (configured[i].enabled && configured[i].IsHardfile() && m_host.getModelId) {
                const std::string reason = UaeConfigBuilder::ControllerUnavailableReason(
                    configured[i].controller, m_host.getModelId());
                if (!reason.empty())
                    Emit(fmt::format("       ! {}", reason));
            }
        }
        if (m_host.getHardDriveControllerRoms) {
            const auto roms = m_host.getHardDriveControllerRoms();
            Emit(fmt::format("  ROM A590/A2091: {}",
                             roms.a590A2091.empty() ? std::string("-") : roms.a590A2091.string()));
            Emit(fmt::format("  ROM A4091:      {}",
                             roms.a4091.empty() ? std::string("-") : roms.a4091.string()));
        }
        return;
    }

    const std::string action = ToUpper(tokens[0]);
    if (action == "APPLY") {
        if (tokens.size() != 1 || !m_host.applyHostMounts) {
            Error(tokens.size() != 1 ? "Usage: HD APPLY" : "Error: HD APPLY unavailable");
            return;
        }
        std::string error;
        if (!m_host.applyHostMounts(error)) {
            Error("Error: " + (error.empty() ? std::string("mount restart failed") : error));
            return;
        }
        Info("Hard-drive profile applied by cold restart.");
        StateChanged();
        return;
    }

    if (action == "ROM") {
        if (!m_host.getHardDriveControllerRoms || !m_host.setHardDriveControllerRoms) {
            Error("Error: controller ROMs unavailable");
            return;
        }
        auto roms = m_host.getHardDriveControllerRoms();
        if (tokens.size() == 1) {
            Emit(fmt::format("  A590/A2091: {}",
                             roms.a590A2091.empty() ? std::string("-") : roms.a590A2091.string()));
            Emit(fmt::format("  A4091:      {}",
                             roms.a4091.empty() ? std::string("-") : roms.a4091.string()));
            return;
        }
        const auto controller = tokens.size() == 3 ? ParseHardDriveController(tokens[1])
                                                   : std::optional<HardDriveController>{};
        if (!controller || (*controller != HardDriveController::ScsiA590A2091 &&
                            *controller != HardDriveController::ScsiA4091)) {
            Error("Usage: HD ROM (A590|A2091|A4091 <path>|CLEAR)");
            return;
        }
        const bool clear = ToUpper(tokens[2]) == "CLEAR";
        auto& rom = *controller == HardDriveController::ScsiA4091 ? roms.a4091 : roms.a590A2091;
        rom = clear ? std::filesystem::path() : std::filesystem::u8path(tokens[2]);
        std::string error;
        if (!m_host.setHardDriveControllerRoms(roms, error)) {
            Error("Error: " + (error.empty() ? std::string("could not set the ROM") : error));
            return;
        }
        Info(fmt::format("{} boot ROM {}.", HardDriveControllerName(*controller),
                         clear ? "cleared" : "set"));
        StateChanged();
        return;
    }

    if (action == "INFO") {
        if (tokens.size() != 2) {
            Error("Usage: HD INFO DH0|DH1|<hardfile>");
            return;
        }
        std::filesystem::path path;
        if (const auto device = parseDevice(tokens[1])) {
            const auto mount = m_host.getConfiguredHostMounts()[static_cast<size_t>(*device)];
            if (!mount.IsHardfile() || mount.hardfilePath.empty()) {
                Error(fmt::format("Error: {} is not set to a hardfile",
                                  HostMountDeviceName(*device)));
                return;
            }
            path = mount.hardfilePath;
        } else {
            path = std::filesystem::u8path(tokens[1]);
        }
        std::string error;
        const auto info = HardfileInspector::Inspect(path, error);
        if (!info) {
            Error("Error: " + error);
            return;
        }
        EmitTableHead(fmt::format("Hardfile {}", path.string()));
        Emit(fmt::format("  Size      {} bytes ({:.1f} MB){}", info->sizeBytes,
                         static_cast<double>(info->sizeBytes) / (1024.0 * 1024.0),
                         info->compressed ? ", gzip-compressed (.hdz)" : ""));
        if (!info->hasRdb) {
            Emit(fmt::format("  RDB       none; block 0 holds {}",
                             HardfileInspector::DosTypeText(info->rawDosType)));
            Emit(fmt::format("  Geometry  automatic {}/{}/{}/{} (sectors/surfaces/reserved/block)",
                             info->guessSectors, info->guessSurfaces, info->guessReserved,
                             info->guessBlockSize));
        } else {
            Emit(fmt::format("  RDB       block {}{}, {} cylinders x {} heads x {} sectors",
                             info->rdbBlock, info->rdbChecksumValid ? "" : " (BAD CHECKSUM)",
                             info->cylinders, info->heads, info->sectors));
            if (!info->diskVendor.empty() || !info->diskProduct.empty())
                Emit(fmt::format("  Disk      {} {}", info->diskVendor, info->diskProduct));
            Emit("  Part  DosType  Cylinders        Size     Boot  Flags");
            for (const auto& part : info->partitions) {
                std::string flags;
                if (part.noMount)
                    flags += "nomount ";
                if (!part.checksumValid)
                    flags += "badsum";
                Emit(fmt::format("  {:<5} {:<8} {:>6}-{:<6} {:>8.1f}MB {:>5}  {}", part.driveName,
                                 HardfileInspector::DosTypeText(part.dosType), part.lowCylinder,
                                 part.highCylinder,
                                 static_cast<double>(part.blockCount * 512) / (1024.0 * 1024.0),
                                 part.bootable ? std::to_string(part.bootPriority) : "no", flags));
            }
            for (const auto& fs : info->filesystems)
                Emit(fmt::format("  FSHD      {} version {}.{}",
                                 HardfileInspector::DosTypeText(fs.dosType), fs.version >> 16,
                                 fs.version & 0xFFFF));
        }
        for (const auto& warning : info->warnings)
            Emit("  ! " + warning);
        return;
    }

    if (tokens.size() < 2) {
        Error("Usage: HD MOUNT|UNMOUNT|SET|INFO|OPEN|REFRESH DH0|DH1 ... | HD ROM | HD APPLY");
        return;
    }
    const auto device = parseDevice(tokens[1]);
    if (!device) {
        Error("Error: device must be DH0 or DH1");
        return;
    }

    if (action == "OPEN" || action == "REFRESH") {
        const auto& callback = action == "OPEN" ? m_host.openHostMount : m_host.refreshHostMount;
        if (tokens.size() != 2 || !callback || !callback(*device)) {
            Error(fmt::format("Error: HD {} failed", action));
            return;
        }
        StateChanged();
        return;
    }

    if (!m_host.setHostMount) {
        Error("Error: hard-drive configuration is read-only");
        return;
    }
    auto mounts = m_host.getConfiguredHostMounts();
    HostDirectoryMount mount = mounts[static_cast<size_t>(*device)];
    mount.device = *device;

    if (action == "UNMOUNT") {
        if (tokens.size() != 2) {
            Error("Usage: HD UNMOUNT DH0|DH1");
            return;
        }
        mount.enabled = false;
    } else if (action == "MOUNT") {
        if (tokens.size() < 3) {
            Error("Usage: HD MOUNT DH0|DH1 <directory|hardfile> [RO|RW] [LABEL <name>] [PRI <n>] "
                  "[CTRL UAE|IDE|A590|A2091|A4091] [UNIT <n>] [GEOM AUTO|RDB|<s>/<h>/<r>/<b>]");
            return;
        }
        mount.enabled = true;
        if (LooksLikeHardfile(tokens[2])) {
            mount.backingType = HostMountBackingType::HardfileImage;
            mount.hardfilePath = std::filesystem::u8path(tokens[2]);
        } else {
            mount.backingType = HostMountBackingType::Directory;
            mount.hostDirectory = std::filesystem::path(tokens[2]);
        }
        if (mount.volumeLabel.empty())
            mount.volumeLabel = HostMountDeviceName(*device);
    } else if (action != "SET") {
        Error("Usage: HD MOUNT|UNMOUNT|SET|INFO|OPEN|REFRESH|ROM|APPLY");
        return;
    }

    size_t option = action == "MOUNT" ? 3 : 2;
    if (action == "SET" && option >= tokens.size()) {
        Error("Usage: HD SET DH0|DH1 RO|RW|LABEL <name>|PRI <n>|CTRL <c>|UNIT <n>|GEOM <g>");
        return;
    }
    const auto parseInt = [&](const std::string& text, int lo, int hi) -> std::optional<int> {
        try {
            size_t parsed = 0;
            const int value = std::stoi(text, &parsed, 10);
            if (parsed != text.size() || value < lo || value > hi)
                return std::nullopt;
            return value;
        } catch (...) {
            return std::nullopt;
        }
    };
    bool unitGiven = false;
    while (option < tokens.size()) {
        const std::string key = ToUpper(tokens[option++]);
        if (key == "RO" || key == "RW") {
            mount.readOnly = key == "RO";
        } else if (key == "LABEL" && option < tokens.size()) {
            mount.volumeLabel = tokens[option++];
        } else if (key == "PRI" && option < tokens.size()) {
            const auto priority = parseInt(tokens[option++], -128, 127);
            if (!priority) {
                Error("Error: priority must be -128 through 127");
                return;
            }
            mount.bootPriority = *priority;
        } else if ((key == "CTRL" || key == "CONTROLLER") && option < tokens.size()) {
            const auto controller = ParseHardDriveController(tokens[option++]);
            if (!controller) {
                Error("Error: controller must be UAE, IDE, A590, A2091 or A4091");
                return;
            }
            mount.controller = *controller;
            if (!unitGiven)
                mount.controllerUnit = std::min(mount.controllerUnit,
                                                UaeConfigBuilder::MaxControllerUnit(*controller));
        } else if (key == "UNIT" && option < tokens.size()) {
            const auto unit = parseInt(tokens[option++], 0, 6);
            if (!unit) {
                Error("Error: unit must be 0 through 6");
                return;
            }
            mount.controllerUnit = *unit;
            unitGiven = true;
        } else if ((key == "GEOM" || key == "GEOMETRY") && option < tokens.size()) {
            const std::string value = tokens[option++];
            if (auto mode = ParseHardfileGeometryMode(value)) {
                mount.geometry.mode = *mode;
            } else {
                int fields[4] = {};
                size_t start = 0;
                bool ok = true;
                for (int f = 0; f < 4 && ok; ++f) {
                    const size_t slash = value.find('/', start);
                    const std::string part = value.substr(
                        start, slash == std::string::npos ? std::string::npos : slash - start);
                    const auto parsed = parseInt(part, 0, 65535);
                    ok = parsed.has_value() &&
                         (f == 3 ? slash == std::string::npos : slash != std::string::npos);
                    if (ok)
                        fields[f] = *parsed;
                    start = slash == std::string::npos ? value.size() : slash + 1;
                }
                if (!ok) {
                    Error("Error: GEOM is AUTO, RDB, or <sectors>/<surfaces>/<reserved>/<block>");
                    return;
                }
                mount.geometry.mode = HardfileGeometryMode::Manual;
                mount.geometry.sectors = fields[0];
                mount.geometry.surfaces = fields[1];
                mount.geometry.reserved = fields[2];
                mount.geometry.blockSize = fields[3];
            }
        } else {
            Error("Error: expected RO, RW, LABEL <name>, PRI <n>, CTRL <c>, UNIT <n> or GEOM <g>");
            return;
        }
    }

    std::string error;
    if (!m_host.setHostMount(mount, error)) {
        Error("Error: " + (error.empty() ? std::string("invalid mount") : error));
        return;
    }
    Info(fmt::format("{} configuration updated; use HD APPLY to restart.",
                     HostMountDeviceName(*device)));
    if (mount.enabled && mount.IsHardfile() && m_host.getModelId) {
        const std::string reason =
            UaeConfigBuilder::ControllerUnavailableReason(mount.controller, m_host.getModelId());
        if (!reason.empty())
            Info(fmt::format("Warning: {} - the drive will not appear on this machine.", reason));
    }
    StateChanged();
}

void ArCommandInterpreter::CmdModel(const std::string& args)
{
    if (!m_host.listModels) {
        Error("Error: machine selection unavailable");
        return;
    }
    const auto models = m_host.listModels();
    const std::string current = m_host.getModelId ? m_host.getModelId() : std::string();
    const auto tokens = Tokenize(args);
    if (tokens.empty()) {
        EmitTableHead("Machines (MODEL <id> selects one; a running machine restarts)");
        for (const auto& model : models) {
            Emit(fmt::format(
                "  {} {:<8} {}{}", model.id == current ? "*" : " ", model.id, model.name,
                model.available ? "" : "  [unavailable: " + model.unavailableReason + "]"));
        }
        return;
    }
    if (tokens.size() != 1) {
        Error("Usage: MODEL (<id>)");
        return;
    }
    const std::string wanted = ToUpper(tokens[0]);
    const auto it = std::find_if(models.begin(), models.end(), [&](const ArHost::ModelInfo& model) {
        return ToUpper(model.id) == wanted;
    });
    if (it == models.end()) {
        Error(fmt::format("Error: unknown machine '{}' (type MODEL for the list)", tokens[0]));
        return;
    }
    if (!it->available) {
        Error(fmt::format("Error: {} is unavailable: {}", it->id, it->unavailableReason));
        return;
    }
    std::string error;
    if (!m_host.selectModel || !m_host.selectModel(it->id, error)) {
        Error("Error: " + (error.empty() ? std::string("could not select the machine") : error));
        return;
    }
    Info(fmt::format("Machine set to {}.", it->name));
    StateChanged();
}

void ArCommandInterpreter::CmdWhdload(const std::string& args)
{
    if (!m_host.launchWhdload || !m_host.getWhdloadSettings) {
        Error("Error: WHDLoad unavailable");
        return;
    }
    const auto tokens = Tokenize(args);
    auto settings = m_host.getWhdloadSettings();
    const auto onOff = [](bool value) { return value ? "ON" : "OFF"; };

    if (tokens.empty()) {
        EmitTableHead("WHDLoad");
        const auto status = m_host.getWhdloadStatus ? m_host.getWhdloadStatus()
                                                    : std::optional<ArHost::WhdloadStatus>{};
        if (status)
            Emit(fmt::format("  Running    '{}' on DH0 ({}) {}", status->volumeLabel,
                             status->archive ? "archive" : "directory", status->path));
        else
            Emit("  Running    - (WHD <archive> to launch)");
        Emit(fmt::format("  BUTTONWAIT {}   NOWRITECACHE {}   PREFS {}   THEME {}",
                         onOff(settings.buttonWait), onOff(settings.noWriteCache),
                         ToUpper(WhdloadPrefsModeName(settings.prefsMode)),
                         settings.theme ? "NATIVE" : "DEFAULT"));
        if (m_host.getWhdloadSavesDirectory)
            Emit("  WHDSaves:  " + m_host.getWhdloadSavesDirectory());
        return;
    }

    const std::string action = ToUpper(tokens[0]);
    if (action == "OFF" && tokens.size() == 1) {
        std::string error;
        if (!m_host.endWhdload || !m_host.endWhdload(error)) {
            Error("Error: " + (error.empty() ? std::string("could not restart") : error));
            return;
        }
        Info("WHDLoad launch ended; restarted with the saved drives.");
        StateChanged();
        return;
    }

    if (action == "SET") {
        if (tokens.size() != 3 || !m_host.setWhdloadSettings) {
            Error("Usage: WHD SET BUTTONWAIT|NOWRITECACHE ON|OFF | WHD SET PREFS "
                  "NONE|CONFIG|SPLASH|BOTH | WHD SET THEME DEFAULT|NATIVE");
            return;
        }
        const std::string key = ToUpper(tokens[1]);
        const std::string value = ToUpper(tokens[2]);
        const auto parseOnOff = [&](bool& target) {
            if (value != "ON" && value != "OFF")
                return false;
            target = value == "ON";
            return true;
        };
        bool ok = false;
        if (key == "BUTTONWAIT") {
            ok = parseOnOff(settings.buttonWait);
        } else if (key == "NOWRITECACHE") {
            ok = parseOnOff(settings.noWriteCache);
        } else if (key == "PREFS") {
            for (int mode = 0; mode < 4; ++mode) {
                if (value == ToUpper(WhdloadPrefsModeName(mode))) {
                    settings.prefsMode = mode;
                    ok = true;
                }
            }
        } else if (key == "THEME") {
            ok = value == "DEFAULT" || value == "NATIVE";
            settings.theme = value == "NATIVE" ? 1 : 0;
        }
        if (!ok) {
            Error(
                "Error: expected BUTTONWAIT|NOWRITECACHE ON|OFF, PREFS NONE|CONFIG|SPLASH|BOTH or "
                "THEME DEFAULT|NATIVE");
            return;
        }
        m_host.setWhdloadSettings(settings);
        Info("WHDLoad settings updated; they apply at the next launch.");
        return;
    }

    if (tokens.size() != 1) {
        Error("Usage: WHD (<archive|directory|slave>) | WHD OFF | WHD SET <key> <value>");
        return;
    }
    std::string error;
    if (!m_host.launchWhdload(tokens[0], error)) {
        Error("Error: " + (error.empty() ? std::string("launch failed") : error));
        return;
    }
    Info("WHDLoad launched; DH0 holds the game until WHD OFF or Apply and Restart.");
    StateChanged();
}

void ArCommandInterpreter::CmdCd(const std::string& args)
{
    if (!m_host.getCdStatus || !m_host.insertCd || !m_host.ejectCd) {
        Error("Error: CD drive unavailable");
        return;
    }
    const auto tokens = Tokenize(args);
    if (tokens.empty()) {
        const auto status = m_host.getCdStatus();
        EmitTableHead("CD0");
        Emit("  Disc      " + (status.source.empty() ? std::string("-") : status.source));
        Emit("  Image     " + (status.image.empty() ? std::string("-") : status.image));
        std::string machine = !status.running            ? "powered off"
                              : !status.hasDrive         ? "no CD drive (started without one)"
                              : status.inMachine.empty() ? "drive empty"
                                                         : "has " + status.inMachine;
        Emit("  Machine   " + machine);
        if (status.extracting)
            Emit("  Busy      extracting the archive into the disc cache");
        if (status.restartPending)
            Emit("  !         the disc goes in at the next power-on (START or RESET)");
        if (!status.error.empty())
            Emit("  !         " + status.error);
        return;
    }
    const std::string action = ToUpper(tokens[0]);
    if (action == "EJECT" && tokens.size() == 1) {
        m_host.ejectCd();
        Info("CD0 ejected.");
        StateChanged();
        return;
    }
    if (action == "INSERT" && tokens.size() == 2) {
        std::string error;
        if (!m_host.insertCd(tokens[1], error)) {
            Error("Error: " + (error.empty() ? std::string("could not insert the disc") : error));
            return;
        }
        const auto status = m_host.getCdStatus();
        if (status.extracting)
            Info("Extracting the archive; the disc goes in when it finishes (CD shows progress).");
        else if (status.restartPending || !status.running)
            Info("Disc chosen; it goes in at the next power-on.");
        else
            Info("Disc inserted in CD0.");
        StateChanged();
        return;
    }
    Error("Usage: CD | CD INSERT <image.cue|ccd|mds|nrg|chd|iso|7z|zip> | CD EJECT");
}

void ArCommandInterpreter::CmdMachine(const std::string& args)
{
    if (!m_host.getMachineStatus || !m_host.setMachineSetting || !m_host.restartMachine) {
        Error("Error: machine configuration unavailable");
        return;
    }
    auto status = m_host.getMachineStatus();
    if (!status.supported) {
        Error("Error: this PuAE core has no chipset options (core 1.86.0 or later)");
        return;
    }
    const auto configured = [&](const MachineSettingKey& setting) {
        const auto it = status.settings.find(setting.key);
        return it == status.settings.end() ? std::string("auto") : it->second;
    };
    const auto running = [&](const MachineSettingKey& setting) -> std::string {
        if (!status.running)
            return "-";
        const auto& config = *status.running;
        const std::string_view key = setting.key;
        if (key == "puae_chipset")
            return ChipsetName(config.chipsetMask);
        if (key == "puae_chipset_compatible")
            return ChipsetCompatibleName(config.compatible);
        if (key == "puae_agnus_model")
            return AgnusModelName(config.agnusModel);
        if (key == "puae_agnus_size")
            return AgnusSizeName(config.agnusSize);
        if (key == "puae_mmu_model")
            return MmuName(config);
        return DeniseModelName(config.deniseModel);
    };
    const auto valueList = [](const EmulatorOptionInfo& option) {
        std::string list;
        for (size_t i = 0; i < option.valueCount; ++i)
            list += (i ? "|" : "") + ToUpper(option.values[i].value);
        return list;
    };

    const auto tokens = Tokenize(args);
    if (tokens.empty()) {
        EmitTableHead("Machine (MACHINE <setting> <value> changes one; APPLY restarts with them)");
        Emit(fmt::format("  {:<14} {:<10} {:<14} {}", "Setting", "Keyword", "Configured",
                         "Running"));
        for (const auto& setting : kMachineSettingKeys)
            Emit(fmt::format("  {:<14} {:<10} {:<14} {}", setting.label, setting.keyword,
                             configured(setting), running(setting)));
        if (status.running) {
            const auto& config = *status.running;
            Emit(fmt::format("  {:<14} {:<10} {:<14} {} (FPU {}, MMU {}, {}-bit addressing)", "CPU",
                             "", "", config.cpuModel,
                             config.fpuModel ? std::to_string(config.fpuModel) : "none",
                             MmuName(config), AddressBits(config)));
        } else {
            Emit("  Running is filled in once the machine is powered on.");
        }
        return;
    }

    const std::string action = ToUpper(tokens[0]);
    if (action == "APPLY" && tokens.size() == 1) {
        std::string error;
        if (!m_host.restartMachine(error)) {
            Error("Error: " + (error.empty() ? std::string("could not restart") : error));
            return;
        }
        Info("Machine restarted with the configured settings.");
        StateChanged();
        return;
    }
    if (action == "DEFAULTS" && tokens.size() == 1) {
        for (const auto& setting : kMachineSettingKeys)
            m_host.setMachineSetting(setting.key, "auto");
        Info("Every setting follows the model again; it takes effect at the next power-on "
             "(MACHINE APPLY restarts now).");
        StateChanged();
        return;
    }

    const auto* setting = FindMachineSettingByKeyword(tokens[0]);
    const auto* option = setting ? FindEmulatorOption(setting->key) : nullptr;
    if (!setting || !option || tokens.size() > 2) {
        Error("Usage: MACHINE | MACHINE CHIPSET|EXTRA|AGNUS|AGNUSSIZE|DENISE|MMU (<value>) | "
              "MACHINE APPLY | MACHINE DEFAULTS");
        return;
    }
    if (tokens.size() == 1) {
        Emit(fmt::format("  {} is {} (running: {}). Values: {}", setting->keyword,
                         configured(*setting), running(*setting), valueList(*option)));
        return;
    }
    const std::string wanted = ToUpper(tokens[1]);
    for (size_t i = 0; i < option->valueCount; ++i) {
        if (ToUpper(option->values[i].value) != wanted)
            continue;
        m_host.setMachineSetting(setting->key, option->values[i].value);
        Info(fmt::format("{} set to {}; it takes effect at the next power-on (MACHINE APPLY "
                         "restarts now).",
                         setting->label, option->values[i].label));
        StateChanged();
        return;
    }
    Error(fmt::format("Error: {} takes {}", setting->keyword, valueList(*option)));
}

void ArCommandInterpreter::CmdSerial(const std::string& args)
{
    if (!m_host.getHostPorts || !m_host.setSerialPort) {
        Error("Error: serial port unavailable");
        return;
    }
    const auto info = m_host.getHostPorts();
    if (!info.supported) {
        Error("Error: this PuAE core has no host serial port (core 1.89.0 or later)");
        return;
    }
    const auto tokens = Tokenize(args);
    if (tokens.empty()) {
        EmitTableHead("Serial port (SERIAL LISTEN|CONNECT|<device>|OFF changes it)");
        const std::string target = info.serial.ToCoreName();
        Emit("  Target   " + (target.empty() ? std::string("off") : target));
        Emit("  Status   " + (info.status ? DescribeSerialStatus(*info.status)
                                          : std::string("the machine is not running")));
        if (info.status && !info.status->serialError.empty() && info.status->serialOpen)
            Emit("  Note     " + info.status->serialError);
        std::string devices;
        for (const auto& device : info.devices)
            devices += (devices.empty() ? "" : " ") + device;
        Emit("  Devices  " + (devices.empty() ? std::string("none found") : devices));
        return;
    }

    std::string spec;
    const std::string verb = ToUpper(tokens[0]);
    if ((verb == "LISTEN" || verb == "CONNECT") && tokens.size() == 2) {
        std::string address = tokens[1];
        if (address.find(':') == std::string::npos)
            address = "127.0.0.1:" + address; // a wider listen is asked for by its address
        spec = "TCP://" + address + (verb == "CONNECT" ? "/connect" : "");
    } else if (tokens.size() == 1) {
        spec = tokens[0];
    }
    const auto target = spec.empty() ? std::nullopt : SerialPortTarget::Parse(spec);
    if (!target) {
        Error("Usage: SERIAL | SERIAL LISTEN [host:]port | SERIAL CONNECT host:port | "
              "SERIAL <device> | SERIAL TCP://host:port(/connect) | SERIAL OFF");
        return;
    }
    m_host.setSerialPort(*target);
    const std::string name = target->ToCoreName();
    if (target->ListensBeyondThisComputer())
        Info("Note: listening on " + target->host +
             " - other machines on the network can reach the Amiga's serial port, at every "
             "launch until it is changed. SERIAL LISTEN <port> listens on this computer only.");
    Info(name.empty() ? std::string("Serial port disconnected.")
                      : "Serial port set to " + name +
                            (info.status ? "; SERIAL shows whether it opened."
                                         : "; it opens when the machine runs."));
    StateChanged();
}

void ArCommandInterpreter::CmdParallel(const std::string& args)
{
    if (!m_host.getHostPorts || !m_host.setParallelOutput) {
        Error("Error: parallel port unavailable");
        return;
    }
    const auto info = m_host.getHostPorts();
    if (!info.supported) {
        Error("Error: this PuAE core has no host parallel port (core 1.89.0 or later)");
        return;
    }
    const auto tokens = Tokenize(args);
    if (tokens.empty()) {
        EmitTableHead("Parallel port (PARALLEL <file>|OFF changes it)");
        Emit("  Output   " +
             (info.parallelPath.empty() ? std::string("nothing connected") : info.parallelPath));
        if (info.status) {
            Emit(fmt::format("  Written  {} bytes", info.status->parallelBytes));
            if (!info.status->parallelError.empty())
                Emit("  !        " + info.status->parallelError);
        }
        return;
    }
    if (tokens.size() != 1) {
        Error("Usage: PARALLEL | PARALLEL <file> | PARALLEL OFF");
        return;
    }
    const std::string upper = ToUpper(tokens[0]);
    const bool off = upper == "OFF" || upper == "NONE";
    m_host.setParallelOutput(off ? std::string() : tokens[0]);
    Info(off ? std::string("Parallel port disconnected.")
             : "Parallel port output appends to " + tokens[0] + ".");
    StateChanged();
}

void ArCommandInterpreter::CmdMidi(const std::string& args)
{
    if (!m_host.getMidi || !m_host.setMidiDevices) {
        Error("Error: MIDI unavailable");
        return;
    }
    const auto info = m_host.getMidi();
    if (!info.supported) {
        Error("Error: this PuAE core has no host MIDI (core 1.90.0 or later)");
        return;
    }
    const auto nameOf = [&](int id, bool output) {
        if (output ? id < -1 : id < 0)
            return std::string("none");
        if (output && id == -1)
            return std::string("system mapper");
        for (const auto& device : output ? info.outputs : info.inputs) {
            if (device.id == id)
                return fmt::format("{} ({})", device.id, device.name);
        }
        return fmt::format("{} (not present)", id);
    };
    const auto tokens = Tokenize(args);
    if (tokens.empty()) {
        EmitTableHead("MIDI (MIDI OUT <n>|MAPPER|NONE, MIDI IN <n>|NONE)");
        Emit("  Out      " + nameOf(info.outDevice, true));
        Emit("  In       " + nameOf(info.inDevice, false));
        if (!info.status)
            Emit("  Status   the machine is not running");
        else if (!info.status->midiError.empty())
            Emit("  Status   " + info.status->midiError);
        else if (info.status->midiActive)
            Emit(fmt::format("  Status   active - {} messages out, {} bytes in",
                             info.status->midiOutMessages, info.status->midiInBytes));
        else
            Emit("  Status   idle - the Amiga is not running its serial port at MIDI speed");
        for (const auto& device : info.outputs)
            Emit(fmt::format("  out {:<3}  {}", device.id, device.name));
        for (const auto& device : info.inputs)
            Emit(fmt::format("  in  {:<3}  {}", device.id, device.name));
        return;
    }

    int outDevice = info.outDevice;
    int inDevice = info.inDevice;
    const auto usage = [&] { Error("Usage: MIDI | MIDI OUT <n>|MAPPER|NONE | MIDI IN <n>|NONE"); };
    if (tokens.size() % 2 != 0) {
        usage();
        return;
    }
    for (size_t i = 0; i < tokens.size(); i += 2) {
        const std::string which = ToUpper(tokens[i]);
        const std::string value = ToUpper(tokens[i + 1]);
        int device = 0;
        if (value == "NONE" || value == "OFF")
            device = which == "OUT" ? -2 : -1;
        else if (value == "MAPPER" && which == "OUT")
            device = -1;
        else if (!value.empty() &&
                 std::all_of(value.begin(), value.end(),
                             [](unsigned char c) { return std::isdigit(c) != 0; }) &&
                 value.size() <= 3 && std::stoi(value) <= 255)
            device = std::stoi(value); // three digits at most: stoi cannot overflow
        else {
            usage();
            return;
        }
        if (which == "OUT")
            outDevice = device;
        else if (which == "IN")
            inDevice = device;
        else {
            usage();
            return;
        }
    }
    m_host.setMidiDevices(outDevice, inDevice);
    Info(fmt::format("MIDI out {}, in {}.", nameOf(outDevice, true), nameOf(inDevice, false)));
    StateChanged();
}

void ArCommandInterpreter::CmdNetwork(const std::string& args)
{
    if (!m_host.getNetwork || !m_host.setNetworkSetting || !m_host.restartMachine) {
        Error("Error: networking unavailable");
        return;
    }
    const auto info = m_host.getNetwork();
    if (!info.supported && !info.cardSupported) {
        Error("Error: this PuAE core has no networking (core 1.91.0 or later)");
        return;
    }
    const auto tokens = Tokenize(args);
    if (tokens.empty()) {
        EmitTableHead("Network (NETWORK <setting> <value>, then NETWORK APPLY)");
        if (info.supported)
            Emit(std::string("  bsdsocket.library  ") +
                 (info.bsdsocket ? "on - programs use the host's TCP/IP stack" : "off"));
        if (info.cardSupported) {
            const char* card = info.a2065 == "slirp"           ? "NAT - the Amiga is 10.0.2.15"
                               : info.a2065 == "slirp_inbound" ? "NAT, host ports 21-23 and 80 open"
                                                               : "off";
            Emit(std::string("  A2065              ") + card);
            Emit(std::string("  uaenet.device      ") +
                 (info.uaenet ? "on - unit 0 NAT, unit 1 with open ports" : "off"));
        } else {
            Emit("  A2065, uaenet.device  not in this core (core 1.92.0, Windows)");
        }
        return;
    }
    // One setting for the next power-on; the core option key and its value.
    const auto set = [&](const char* what, const char* key, const std::string& value,
                         const std::string& shown) {
        m_host.setNetworkSetting(key, value);
        Info(std::string(what) + " " + shown +
             "; it takes effect at the next power-on (NETWORK APPLY restarts now).");
        StateChanged();
    };
    const std::string verb = ToUpper(tokens[0]);
    if (verb == "APPLY" && tokens.size() == 1) {
        std::string error;
        if (!m_host.restartMachine(error)) {
            Error("Error: " + (error.empty() ? std::string("could not restart") : error));
            return;
        }
        Info("Machine restarted with the network settings.");
        StateChanged();
        return;
    }
    const std::string value = tokens.size() == 2 ? ToUpper(tokens[1]) : std::string();
    if ((verb == "BSDSOCKET" && !info.supported) ||
        ((verb == "A2065" || verb == "UAENET") && !info.cardSupported)) {
        Error("Error: this PuAE core has no " + verb +
              (verb == "BSDSOCKET" ? " (core 1.91.0 or later)" : " (core 1.92.0, Windows)"));
        return;
    }
    if (verb == "BSDSOCKET" && (value == "ON" || value == "OFF")) {
        set("bsdsocket.library", "puae_bsdsocket", value == "ON" ? "enabled" : "disabled",
            value == "ON" ? "on" : "off");
        return;
    }
    if (verb == "A2065" && (value == "OFF" || value == "NAT" || value == "OPEN")) {
        set("A2065", "puae_a2065",
            value == "NAT" ? "slirp" : (value == "OPEN" ? "slirp_inbound" : "disabled"),
            value == "NAT" ? "on NAT" : (value == "OPEN" ? "on NAT with open ports" : "off"));
        return;
    }
    if (verb == "UAENET" && (value == "ON" || value == "OFF")) {
        set("uaenet.device", "puae_sana2", value == "ON" ? "enabled" : "disabled",
            value == "ON" ? "on" : "off");
        return;
    }
    Error("Usage: NETWORK | NETWORK BSDSOCKET ON|OFF | NETWORK A2065 OFF|NAT|OPEN | "
          "NETWORK UAENET ON|OFF | NETWORK APPLY");
}

void ArCommandInterpreter::CmdSoundCard(const std::string& args)
{
    if (!m_host.getSoundCard || !m_host.setSoundCard || !m_host.restartMachine) {
        Error("Error: sound cards unavailable");
        return;
    }
    const auto info = m_host.getSoundCard();
    if (!info.supported) {
        Error("Error: this PuAE core has no sound cards (core 1.93.0 or later)");
        return;
    }
    // Console names and the core's board names, in the order the dialog lists them.
    static const std::pair<const char*, const char*> kBoards[] = {
        {"OFF", "disabled"},     {"TOCCATA", "toccata"},
        {"PRELUDE", "prelude"},  {"PRELUDE1200", "prelude1200"},
        {"UAESND", "uaesnd_z2"}, {"UAESNDZ3", "uaesnd_z3"}};
    const auto tokens = Tokenize(args);
    if (tokens.empty()) {
        EmitTableHead("Sound card (SOUNDCARD OFF|TOCCATA|PRELUDE|PRELUDE1200|UAESND|UAESNDZ3, "
                      "then SOUNDCARD APPLY)");
        std::string shown = "none";
        for (const auto& [name, board] : kBoards)
            if (info.board == board && info.board != "disabled")
                shown = name;
        Emit("  " + shown);
        return;
    }
    const std::string verb = ToUpper(tokens[0]);
    if (tokens.size() == 1 && verb == "APPLY") {
        std::string error;
        if (!m_host.restartMachine(error)) {
            Error("Error: " + (error.empty() ? std::string("could not restart") : error));
            return;
        }
        Info("Machine restarted with the sound card setting.");
        StateChanged();
        return;
    }
    if (tokens.size() == 1) {
        for (const auto& [name, board] : kBoards) {
            if (verb != name)
                continue;
            m_host.setSoundCard(board);
            Info(std::string("Sound card ") + (verb == "OFF" ? "off" : name) +
                 "; it takes effect at the next power-on (SOUNDCARD APPLY restarts now).");
            StateChanged();
            return;
        }
    }
    Error("Usage: SOUNDCARD | SOUNDCARD OFF|TOCCATA|PRELUDE|PRELUDE1200|UAESND|UAESNDZ3 | "
          "SOUNDCARD APPLY");
}

void ArCommandInterpreter::CmdDocs(const std::string& args)
{
    if (!m_host.getOpenDocuments) {
        Error("Error: docs not available");
        return;
    }
    const auto docs = m_host.getOpenDocuments();
    const auto details = m_host.getOpenDocumentInfo ? m_host.getOpenDocumentInfo()
                                                    : std::vector<ArHost::DocumentInfo>{};
    if (docs.empty()) {
        Info("No open documents.");
        return;
    }

    const auto tokens = Tokenize(args);
    if (tokens.empty()) {
        EmitTableHead("Open documents");
        for (size_t i = 0; i < docs.size(); ++i) {
            const std::string path = docs[i].empty() ? "(untitled)" : docs[i];
            if (i < details.size()) {
                const std::string assignment = details[i].assignedDevice
                                                   ? HostMountDeviceName(*details[i].assignedDevice)
                                                   : "-";
                Emit(fmt::format("  {:<4} {:<14} {:<3} doc:{}", fmt::format("d{}:", i + 1),
                                 details[i].kind, assignment, path));
            } else {
                Emit(fmt::format("  {:<4} doc:{}", fmt::format("d{}:", i + 1), path));
            }
        }
        return;
    }

    auto v = ParseNumber(tokens[0]);
    if (!v || *v < 1 || *v > static_cast<uint32_t>(docs.size())) {
        Error(fmt::format("Error: bad doc number (1-{})", docs.size()));
        return;
    }
    const int idx = static_cast<int>(*v - 1);
    if (!m_host.runDocumentByIndex) {
        Error("Error: run document not available");
        return;
    }
    const std::string name =
        docs[static_cast<size_t>(idx)].empty()
            ? "(untitled)"
            : std::filesystem::path(docs[static_cast<size_t>(idx)]).filename().string();
    Info(fmt::format("Loading {} into emulator...", name));
    m_host.runDocumentByIndex(idx);
    StateChanged();
}

void ArCommandInterpreter::CmdCloseDoc(const std::string& args)
{
    if (!m_host.getOpenDocuments) {
        Error("Error: docs not available");
        return;
    }
    const auto docs = m_host.getOpenDocuments();
    if (docs.empty()) {
        Info("No open documents.");
        return;
    }

    const auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Error: usage CLOSE dN (see DOCS)");
        return;
    }

    // Accept the "dN" / "dN:" form shown by DOCS, as well as a bare number.
    std::string tok = tokens[0];
    if (!tok.empty() && (tok[0] == 'd' || tok[0] == 'D'))
        tok = tok.substr(1);
    if (!tok.empty() && tok.back() == ':')
        tok.pop_back();

    auto v = ParseNumber(tok);
    if (!v || *v < 1 || *v > static_cast<uint32_t>(docs.size())) {
        Error(fmt::format("Error: bad doc number (1-{})", docs.size()));
        return;
    }
    if (!m_host.closeDocumentByIndex) {
        Error("Error: close document not available");
        return;
    }
    const int idx = static_cast<int>(*v - 1);
    const std::string name =
        docs[static_cast<size_t>(idx)].empty()
            ? "(untitled)"
            : std::filesystem::path(docs[static_cast<size_t>(idx)]).filename().string();
    Info(fmt::format("Closing {}...", name));
    m_host.closeDocumentByIndex(idx);
    StateChanged();
}

void ArCommandInterpreter::CmdHelp(const std::string&)
{
    auto section = [this](const char* title) { Emit(title, ArColor::Highlight); };
    auto entry = [this](const char* cmd, const char* desc) {
        ArLine line(fmt::format("  {:<28} {}", cmd, desc));
        line.Span(2, 28, ArColor::Prompt);
        Emit(line);
    };

    section("MEMORY");
    entry("M/MM/MMM <addr> (<end>)", "Hex dump 1/8/16 lines — edit + Return writes back");
    entry("N/NN/NNN <addr> (<end>)", "ASCII dump 1/8/16 lines — edit + Return writes back");
    entry("Y/YY/YYY <addr> (<end>)", "Binary dump 1/8/16 lines — edit + Return writes back");
    entry("YS (<bytes>)", "Set binary dump width (1-8 bytes/line)");
    entry("NO (<offset>)", "ASCII display offset for N commands");
    entry("MQ/NQ <addr> (<end>)", "Scrolling hex/ASCII dump — Escape stops");
    entry("? <expr>", "Calculator: + - * / and ( ) — shows hex/dec/bin");
    entry("CRC16/CRC32 <s> <e>", "Checksum a memory range");
    section("CODE");
    entry("D/DD/DDD (<addr>) (<end>)", "Disassemble 1/8/16 lines — Return continues");
    entry("A <addr>", "Assemble — type over lines, Return writes, ESC exits");
    entry("R (<reg> <value>)", "Show/set CPU registers — edit lines + Return");
    entry("RC / RF / RM", "Stack + control / FPU / MMU registers for the configured CPU");
    entry("STACK (<n>)", "Dump n longwords from A7 with region hints");
    entry("CALL (<n>)", "Heuristic call chain from BSR/JSR return addresses");
    section("PROFILING (needs CPU trace; symbols from a sibling .elf)");
    entry("TRACE (ON|OFF)", "Show/toggle CPU instruction trace (required for profiling)");
    entry("COV <file>:<line> [frame]",
          "Did a source line run? Shows every stretch of code it produced and how often each "
          "executed - the question a breakpoint cannot answer on optimised code");
    entry("PROFILE (<view>) (<n>)",
          "Profiler snapshot; view = FUNC|INSTR|FLAT|FLAME (default FUNC)");
    entry("CALLSTACK / CS (LIVE|TRACE)",
          "Call stack: LIVE unwinds the halted CPU (DWARF, no trace needed), TRACE "
          "reconstructs it at the frame-scrubber playhead. Defaults to LIVE when paused");
    section("TRAINER");
    entry("TS <value> (<start>)", "Start trainer search for lives value");
    entry("T <value>", "Continue search with new value");
    entry("TX", "Exit trainer search");
    entry("TF/TFD <addr> (<s> <e>)", "Find (TFD: remove) instruction decrementing addr");
    entry("TDS / TDC / TDX", "Deep trainer: start / value changed / exit");
    entry("TD / TDI / TDD <s> <e>", "Deep trainer: list values / matches / delete range");
    entry("PR", "List memory locks (protections)");
    entry("PR <addr> BLOCK (LEN=<n>)", "Block guest writes to an address or range");
    entry("PR <addr> SET=<v> (SIZE=8|16|32)", "Force a value at an address");
    entry("PR ON|OFF <id> / PR DEL <addr>", "Enable / disable / remove a lock");
    entry("PR CLEAR / PR RESCAN", "Remove every lock / re-resolve every anchor");
    entry("PR LOAD|SAVE|IMPORT <path>", "Trainer file: read / write / import a RetroArch .cht");
    Emit("  Locks catch CPU writes and copper MOVEs. Blitter and disk DMA bypass both hooks");
    Emit("  and are not blocked - which only matters for a range in chip RAM, not a register.");
    // Hidden with the rest of Exocet unless private features are on (core/PrivateFeatures.h)
    if (PrivateFeatures()) {
        section("EXOCET (the sync tracker; a place is pp:rr, pp:rr.pass, L1:rr, s:pp:rr, #row, "
                "F<frame> or @<bookmark slot|name>)");
        entry("EXO", "The live block, the pack and whether they are one build");
        entry("EXO TL (<place>) (<n>)", "Timeline: songs, playhead, played rows, cuts, parts");
        entry("EXO GRID (<place>) (<n>) (<track|group>...) (CH)",
              "The grid: values and keys by row (CH adds the tracker channels)");
        entry("EXO KEYS (<track>)", "Every key, or one track's");
        entry("EXO KEY <track> <place> <value> (<interp>) (T<tick>)", "Set a key (not saved)");
        entry("EXO DELKEY <track> <place> (T<tick>)", "Remove a key");
        entry("EXO EVERYHIT <track> <sample> <place>", "Copy a hit's keys onto every hit");
        entry("EXO CLEAR <track>", "Remove every key on a track");
        entry("EXO UNDO / REDO / SAVE", "The .exocet's undo, redo and save");
        entry("EXO CHECK (TRUST)", "Run the kit's check (TRUST: trust the kit first)");
        entry("EXO SEEK <place> (T<tick>)", "Rewind the frame history to a recorded place");
        entry("EXO BR", "List Exocet breakpoints and the last one hit");
        entry("EXO BR ROW <place> / KEY <track>", "Break as the demo starts a row / a keyed row");
        entry("EXO BR EVENT <track> (<value|name>)", "Break as an event track fires");
        entry("EXO BR CUT (<cut|name>) (READY)", "Break as a director cut is made (or ready)");
        entry("EXO BD ROW|KEY|EVENT|CUT ... / EXO BR CLEAR", "Remove one / all");
        entry("EXO BM", "List the .exocet's bookmarks");
        entry("EXO BM <place> (S<slot>) (<name>)", "Bookmark a row, or set its slot / name");
        entry("EXO BMD <place> / EXO BM CLEAR", "Remove one bookmark / all");
        entry("EXO MUSIC (<sample>)", "Samples: packing, hits, channel lights");
        entry("EXO DISK (<kind>) (ENTRIES)", "The production disk's map");
        entry("EXO MEM (CHIP|FAST)", "The disk's memory map: regions and allocations");
        entry("EXO PROOF / ISSUES", "Proofing (cuts, events, divergence) / the issues panel");
        entry("EXO PUSH / INJECT (<module>) / WRITE (<path>)",
              "Push keys, inject music, write ADF");
    }
    section("HARDWARE");
    entry("E (<offset>)", "View/edit custom chip registers");
    entry("C <1|2|addr>", "Disassemble copper list");
    entry("BLIT (<n> | ALL)", "This frame's blitter operations log (mirrors the Blitter window)");
    entry("REGLOG (<n> | ALL)",
          "This frame's custom-register writes (mirrors the Register Operations window)");
    entry("AUDIO", "Audio path health: ring level, underruns, drops, producer jitter");
    entry("AUDREC (CORE|DEVICE|BOTH) <file.wav> [secs]",
          "Record audio: CORE = what PuAE synthesised, DEVICE = what you hear");
    entry("AUDREC STOP", "Stop audio recording and close the WAV files");
    entry("SCEN REC <file.pkscn> [COLD] [notes]",
          "Record this session - input plus a restorable seed - to a scenario file");
    entry("SCEN STOP", "Finish the recording and write the file");
    entry("SCEN PLAY <file.pkscn>", "Replay a scenario; reports against its goldens if blessed");
    entry("PAULA", "Paula channel state: AUDxLC/LEN/PER/VOL, DMA, mute mask, filter");
    entry("DSKLOG (<n>|ALL) (READ|WRITE)",
          "This session's disk DMA transfers (mirrors the Disk I/O window)");
    entry("LIVE (<df> <type> <port> [mode] [cable] [cache] [smart] | <df> OFF)",
          "Back a drive slot with a real floppy via Greaseweazle/DrawBridge/SCP; LIVE alone lists");
    entry("W", "Dump CIA-A/CIA-B registers (read-only)");
    entry("PALETTE / PAL", "Dump the 32 OCS/ECS colour registers");
    section("DISPLAY MASKING (cosmetic — hides layers, never changes emulation)");
    entry("DISPLAY (ALL|NONE)",
          "Screen mode (640x256 hires 4bpl ...) + show/set every layer toggle at once");
    entry("BPL (ALL|NONE|<1-8>..)", "Show/toggle bitplanes — BPL 3 OFF, BPL MASK $0F");
    entry("SPR (ALL|NONE|<0-7>..)", "Show/toggle sprites — SPR 0 OFF, SPR MASK %10000001");
    entry("PF (ALL|NONE|<1-2>..)", "Show/toggle playfields (PF1 = BPL1/3/5, PF2 = BPL2/4/6)");
    section("EXECUTION");
    entry("G (<addr>) / X", "Resume/start emulator (optionally at address)");
    entry("S/ST (<steps>)", "Step into (trace) — shows next instruction");
    entry("TR (<steps>)", "Step over subroutines");
    entry("SF (<n>)", "Step n frames (default 1) — emulator must be paused");
    entry("RUNTO <frame> (T)", "Run until emulator reaches decimal frame # then pause (T = turbo)");
    entry("EF", "End frame: finish the current mid-execution frame (skips breakpoints)");
    entry("PAUSE", "Toggle pause/resume");
    entry("TA <addr> (<end>)", "Trace until PC reaches address/range");
    entry("START", "Start the emulator with the effective ADF/directory profile");
    entry("STOP", "Stop the running emulator");
    entry("RESET", "Reset the running emulator session");
    entry("TURBO/WARP (<ON|OFF>)", "Toggle/set turbo (warp) mode");
    entry("TRB / FB", "Toggle break on bootblock entry (address-agnostic)");
    section("BREAKPOINTS");
    entry("BS/BD <addr>", "Set/delete breakpoint");
    entry("BS .. IF <r> <op> <v>", "Conditional breakpoint, e.g. BS $400 IF D0 == 5");
    entry("BS .. N <n> / ONESHOT", "Ignore n hits / remove after first hit");
    entry("B / BDA", "List / delete all breakpoints");
    entry("BK (<text>) / BKS <text>", "List / set break on KPrintF debug output text");
    entry("BKS /EXACT /PREFIX /REGEX", "Match mode (default: contains) — also /CASE, /ONCE");
    entry("BKD <n|text> / BKDA", "Delete one (by BK row # or text) / all KPrintF breakpoints");
    entry("EB <vec..|ALL|OFF>", "Break on CPU exception, e.g. EB ILLEGAL ZERO");
    entry("BL <line> [hpos]", "Break at raster scanline, hex (BL OFF to clear)");
    entry("BDSK <READ|WRITE|ANY> [k=v]",
          "Break on disk DMA; track=/cyl=/side=/sector=/addr=$lo-$hi (BDSK OFF to clear)");
    entry("BC <addr>", "Toggle break on Copper instruction at addr");
    entry("MS/MD <addr>", "Set/delete memory write watchpoint");
    entry("MW / MDA", "List / delete all watchpoints");
    section("SEARCH");
    entry("F/FS (-L) <str|bytes>(,<s> <e>)", "Find (FS = case-insensitive; -L dumps matches)");
    entry("FR <str>(,<s> <e>)", "Relative (offset) string search");
    entry("FA/FAQ <addr> (<s> <e>)", "Find instructions accessing address");
    section("BLOCKS");
    entry("COMP/TRANS/EX <s> <e> <d>", "Compare/copy/exchange memory blocks");
    entry("O <str|bytes>, <s> <e>", "Fill memory");
    entry("WS <str|bytes>, <addr>", "Write bytes/string");
    entry("MEMCODE <s> <e> <code>", "EOR memory with byte");
    entry("ADD <s> <e> <value>", "Add value to each byte");
    section("FILES");
    entry("LOAD / L (<path|fx|dx|file:|doc:>) (FORCE|!)",
          "Open ADF/EXE/DMS/directory — fx: recent, dx: open doc, file:/doc: prefixes; "
          "FORCE/! replaces a modified doc");
    entry("LIST", "List recent files as f1:/f2:... with file: paths");
    entry("DOCS (<dx>)", "List open docs as d1:/d2:... — LOAD dx to boot one");
    entry("HD (MOUNT|UNMOUNT|SET|INFO|OPEN|REFRESH|ROM|APPLY)",
          "List or configure DH0:/DH1: - host directories or .hdf/.hdz hardfiles");
    entry("HD MOUNT DH0 <dir|hdf> (RO|RW) (PRI n) (LABEL s)",
          "Hardfile options: CTRL UAE|IDE|A590|A2091|A4091, UNIT n, GEOM AUTO|RDB|s/h/r/b");
    entry("HD INFO DH0|DH1|<hdf>", "Show a hardfile's RDB partition table and geometry");
    entry("HD ROM (A590|A2091|A4091 <path>|CLEAR)", "Boot ROM for a SCSI card");
    entry("MODEL (<id>)", "List machine presets, or select one (restarts a running machine)");
    entry("WHD (<lha|dir|slave>|OFF)", "Boot a WHDLoad install on DH0, show status, or end it");
    entry("CD (INSERT <image|7z>|EJECT)", "Show CD0, or change its disc (archives are extracted)");
    entry("SERIAL (LISTEN [host:]port|CONNECT host:port|<device>|<TCP://..>|OFF)",
          "Show the serial port, or connect it to TCP or a host serial device");
    entry("PARALLEL (<file>|OFF)", "Show the parallel port, or capture its output to a file");
    entry("SOUNDCARD (OFF|TOCCATA|PRELUDE|PRELUDE1200|UAESND|UAESNDZ3|APPLY)",
          "Show or change the expansion sound card beside Paula");
    entry("NETWORK (BSDSOCKET ON|OFF|A2065 OFF|NAT|OPEN|UAENET ON|OFF|APPLY)",
          "Show or change networking: bsdsocket.library, an A2065 card, uaenet.device");
    entry("MIDI (OUT <n>|MAPPER|NONE) (IN <n>|NONE)",
          "Show or choose host MIDI devices for the serial port at MIDI speed");
    entry("MACHINE (<setting> (<value>)|APPLY|DEFAULTS)",
          "Below the model: CHIPSET, EXTRA, AGNUS, AGNUSSIZE, DENISE, MMU");
    entry("WHD SET <key> <value>", "BUTTONWAIT|NOWRITECACHE ON|OFF, PREFS NONE|CONFIG|SPLASH|BOTH, "
                                   "THEME DEFAULT|NATIVE");
    entry("CLOSE <dx>", "Close the open document dx (see DOCS)");
    entry("LM <file>,<addr>", "Load file into memory");
    entry("SM <file>,<s> <e>", "Save memory to file");
    entry("VERSION / VER / V", "Show PEEKaboo, PuAE core and console versions");
    entry("KEYS", "Show cursor-movement & line-editing key bindings");
    entry("CLS / CLEAR", "Clear the console (same as the Clear button)");
    entry("FRAME", "Show current emulator frame count (same as Display window overlay)");
    entry("HISTORY / HIST",
          "Frame-history timeline: recorded range, memory used, how far back it reaches");
    entry("HISTORY CLEAR", "Throw the recorded timeline away and start filling it again");
    entry("REWIND / RW [n]",
          "Step back n recorded frames (decimal, default 1) and re-run that frame");
    entry("GOTO <frame>", "Seek to a recorded frame (decimal) and re-run it");
    entry("DIFF <a> <b>", "RAM addresses that differ between two recorded frames (decimal)");
    entry("LOOP <a> <b> | OFF", "Replay a recorded frame range on repeat");
    entry("LOOP (PLAY|PAUSE|STEP <n>|SPEED <x>|DIR (FWD|REV|PP))",
          "Transport for the replay: run/hold it, step frames, set rate or direction");
    entry("STATE (LIST|SAVE|LOAD|DELETE <name>)",
          "Machine snapshots: LIST (or no args) lists them; SAVE ... PERSIST writes to disk");
    section("DISK (DF0:, 160 TRACKS, 1760 SECTORS, 880KB)");
    entry("RT <trk> (<n> <addr>)", "Read n tracks (0-!159) into memory; alloc buffer if no addr");
    entry("RS <sec> (<n> <addr>)", "Read n sectors (0-!1759) into memory");
    entry("RB <off> (<n> <addr>)", "Read n bytes at ADF byte offset (0-!901119)");
    entry("WT <trk> <n> <addr>", "Write n tracks to disk from addr (T<t>/S<s> for buf refs)");
    entry("WDS <sec> <n> <addr>", "Write n sectors to disk from addr");
    entry("WDB <off> <n> <addr>", "Write n bytes to disk at ADF byte offset");
    entry("BOOTCHK <addr>", "Fix bootblock checksum (1024 bytes, chksum at +4)");
    entry("DATACHK <addr>", "Fix OFS data block checksum (512 bytes, chksum at +20)");
    entry("BAMCHK <addr>", "Fix bitmap block checksum (512 bytes, chksum at +0)");
    section("SYSTEM (needs a booted Kickstart/AmigaOS)");
    entry("EXECBASE", "Dump key ExecBase fields");
    entry("KICKVER", "Kickstart version, date and ROM address range");
    entry("AVAIL", "Chip/Fast RAM totals and free space");
    entry("LIBRARIES / DEVICES", "List loaded libraries / devices");
    entry("RESOURCES", "List system resources");
    entry("TASKS", "List running/ready/waiting tasks");
    entry("PORTS", "List public message ports");
    entry("INTERRUPTS / INTS", "List Exec interrupt vectors");
    entry("EXCEPTIONS", "List 68000 exception vector table");
    entry("CHIPREGS", "Dump all custom chip registers with values");
    entry("DCHIP <reg>", "Decode one custom register bit-by-bit");
    section("ANNOTATIONS");
    entry("TM <addr> \"<comment>\"", "Tag an address with a comment (trailing ; in disassembly)");
    entry("TL <addr> \"<label>\"", "Tag an address with a label (line above; shown on branches)");
    entry("TB <addr> \"<text>\"", "Set a multi-line comment block (use \\n for line breaks)");
    entry("TMS", "Show all labels and comments sorted by address");
    entry("TMD <addr>", "Delete the label and comment at an address");
    entry("TBD <addr>", "Delete the comment block at an address");
    Emit("");
    Info("Numbers are hex. $/0x = hex, ! = decimal, % = binary. ESC cancels modes.");
}

void ArCommandInterpreter::CmdClear(const std::string&)
{
    // Same action as the Clear toolbar button (clears the terminal + reprints the banner).
    if (clearScreen)
        clearScreen();
    else
        Error("Error: clear unavailable");
}

void ArCommandInterpreter::CmdKeys(const std::string&)
{
    auto section = [this](const char* title) { Emit(title, ArColor::Highlight); };
    auto entry = [this](const char* keys, const char* desc) {
        ArLine line(fmt::format("  {:<22} {}", keys, desc));
        line.Span(2, 22, ArColor::Prompt);
        Emit(line);
    };

    section("CURSOR MOVEMENT");
    entry("Left / Right", "Move one character");
    entry("Up / Down", "Move one line");
    entry("Home / End", "Start / end of line");
    entry("Page Up / Page Down", "Move up / down one screen");
    entry("Tab", "Jump to next 8-column tab stop");
#ifdef __APPLE__
    entry("Ctrl-A / Ctrl-E", "Start / end of line");
    entry("Ctrl-B / Ctrl-F", "Move one character left / right");
    entry("Ctrl-P / Ctrl-N", "Move one line up / down");
    entry("Opt-Left / Opt-Right", "Move to start of prev / next word");
    entry("Opt-B / Opt-F", "Move to start of prev / next word");
    entry("Cmd-Left / Cmd-Right", "Start / end of line");
    entry("Cmd-Up / Cmd-Down", "Top / bottom of buffer");
    section("EDITING");
    entry("Insert", "Toggle insert / overwrite mode");
    entry("Backspace / Ctrl-H", "Delete character before cursor");
    entry("Delete / Ctrl-D", "Delete character at cursor");
    entry("Ctrl-K", "Delete to end of line");
    entry("Ctrl-U", "Delete to start of line");
    entry("Ctrl-W / Opt-Bksp", "Delete word before cursor");
    entry("Opt-D", "Delete word after cursor");
    section("CLIPBOARD");
    entry("Cmd-C / Cmd-V", "Copy selection / paste");
    entry("Cmd-A", "Select all");
#else
    entry("Ctrl-Left / Ctrl-Right", "Move one word left / right");
    entry("Ctrl-Home / Ctrl-End", "Top / bottom of buffer");
    section("EDITING");
    entry("Insert", "Toggle insert / overwrite mode");
    entry("Backspace", "Delete character before cursor");
    entry("Delete", "Delete character at cursor");
    entry("Ctrl-Backspace", "Delete word before cursor");
    entry("Ctrl-Delete", "Delete word after cursor");
    section("CLIPBOARD");
    entry("Ctrl-C / Ctrl-V", "Copy selection / paste");
    entry("Ctrl-A", "Select all");
#endif
    entry("Enter", "Submit the line under the cursor");
    entry("Escape", "Clear selection / cancel mode");
    Emit("");
    Info("Default mode is overwrite (OVR); the Insert key toggles to INS.");
}

void ArCommandInterpreter::CmdFrame(const std::string&)
{
    if (!m_host.getFrameCount) {
        Error("Error: frame count not available");
        return;
    }
    Info(fmt::format("Frame: {}", m_host.getFrameCount()));
}

// ---------------------------------------------------------------------------
// Frame history: HISTORY / REWIND / GOTO / DIFF / LOOP
// ---------------------------------------------------------------------------

namespace {

std::string HumanBytes(size_t bytes)
{
    if (bytes >= 1024u * 1024u)
        return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    if (bytes >= 1024u)
        return fmt::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
    return fmt::format("{} B", bytes);
}

// PAL is what the recorded range is almost always measuring, so seconds are a more useful
// second opinion on "how far back does this reach" than a frame count alone.
std::string HumanFrames(uint64_t frames)
{
    return fmt::format("{} frames (~{:.1f}s PAL)", frames, static_cast<double>(frames) / 50.0);
}

// Frame numbers are decimal everywhere the user meets them - HISTORY prints them, the
// scrubber shows them, get_frame_history returns them - so they are parsed decimal here
// even though the console defaults to hex for addresses. Without this, HISTORY reporting
// "parked on 1201" followed by GOTO 1201 would land on frame 4609. An explicit $ / 0x / %
// prefix still selects a base, so nothing is taken away.
std::optional<uint64_t> ParseFrameNumber(const std::string& token)
{
    const std::string t = Trim(token);
    if (t.empty())
        return std::nullopt;
    if (t[0] == '$' || t[0] == '%' || (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')))
        return ArCommandInterpreter::ParseNumber(t);

    const std::string digits = (t[0] == '!') ? t.substr(1) : t;
    if (digits.empty())
        return std::nullopt;
    try {
        size_t parsed = 0;
        const unsigned long long v = std::stoull(digits, &parsed, 10);
        if (parsed != digits.size())
            return std::nullopt;
        return static_cast<uint64_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

void ArCommandInterpreter::CmdHistory(const std::string& args)
{
    if (ToUpper(Trim(args)) == "CLEAR") {
        if (!m_host.frameHistoryClear) {
            Error("Error: frame history not available");
            return;
        }
        m_host.frameHistoryClear();
        Info("Frame history cleared");
        return;
    }

    if (!m_host.frameHistoryInfo) {
        Error("Error: frame history not available");
        return;
    }
    const auto info = m_host.frameHistoryInfo();
    const auto& st = info.stats;

    if (!st.recording) {
        Info("Frame history: not recording (open a debugger window to arm it)");
        return;
    }
    if (st.empty) {
        Info("Frame history: recording, nothing captured yet");
        if (!st.hint.empty())
            Error(fmt::format("  {}", st.hint));
        return;
    }

    Info(fmt::format("Frame history: {}-{} ({}), parked on {}", st.oldestFrame, st.newestFrame,
                     HumanFrames(st.frameCount), info.position));
    Info(fmt::format("  Memory     {} of {} ({} keyframes, save state {})",
                     HumanBytes(st.bytesUsed), HumanBytes(st.bytesBudget), st.keyframeCount,
                     HumanBytes(st.blobSize)));
    Info(fmt::format("  Per frame  {} delta, {:.2f} ms to record",
                     HumanBytes(static_cast<size_t>(st.avgDeltaBytes)), st.avgRecordMs));
    Info(fmt::format("  Reach      {} at this rate", HumanFrames(st.estimatedCapacityFrames)));
    if (st.evictedFrames > 0) {
        Info(fmt::format("  Dropped    {} older frames to stay inside the budget",
                         st.evictedFrames));
    }
    if (st.gapCount > 0) {
        // Frames the machine never completed under their own number - a mid-frame
        // breakpoint stop is the usual cause. Playback steps over them; the count is here
        // because a timeline full of them used to be invisible, and it is the first thing
        // to look at when a seek lands somewhere other than where it was asked.
        Info(fmt::format("  Gaps       {} place{} where frame numbers skip", st.gapCount,
                         st.gapCount == 1 ? "" : "s"));
    }
    if (info.loopEnd > 0)
        Info(fmt::format("  Looping    {}-{}", info.loopStart, info.loopEnd));
    {
        static const char* kModes[] = {"forward", "reverse", "ping-pong"};
        const int mode = (info.playMode >= 0 && info.playMode <= 2) ? info.playMode : 0;
        Info(fmt::format("  Playback   {} {} at {:g}x", info.playing ? "playing" : "paused",
                         kModes[mode], info.playSpeed));
    }
    if (!st.hint.empty())
        Error(fmt::format("  {}", st.hint));
}

void ArCommandInterpreter::CmdRewind(const std::string& args)
{
    if (!m_host.frameHistoryRewind) {
        Error("Error: frame history not available");
        return;
    }
    uint64_t count = 1;
    const std::string t = Trim(args);
    if (!t.empty()) {
        auto n = ParseFrameNumber(t);
        if (!n || *n == 0) {
            Error("Usage: REWIND [frames]   (decimal)");
            return;
        }
        count = *n;
    }

    const std::string err = m_host.frameHistoryRewind(count);
    if (!err.empty()) {
        Error(fmt::format("Error: {}", err));
        return;
    }
    if (m_host.frameHistoryInfo)
        Info(fmt::format("Frame {}", m_host.frameHistoryInfo().position));
}

void ArCommandInterpreter::CmdGoto(const std::string& args)
{
    if (!m_host.frameHistorySeek) {
        Error("Error: frame history not available");
        return;
    }
    auto n = ParseFrameNumber(args);
    if (!n) {
        Error("Usage: GOTO <frame>   (decimal)");
        return;
    }

    const std::string err = m_host.frameHistorySeek(*n);
    if (!err.empty()) {
        Error(fmt::format("Error: {}", err));
        return;
    }
    // Report where it actually landed, not what was asked for: frame numbers are not always
    // consecutive (a mid-frame breakpoint never completes one under its number) and a seek
    // snaps onto the nearest frame that was really recorded.
    if (m_host.frameHistoryInfo) {
        const uint64_t at = m_host.frameHistoryInfo().position;
        Info(at == *n ? fmt::format("Frame {}", at)
                      : fmt::format("Frame {} (nearest recorded to {})", at, *n));
    } else {
        Info(fmt::format("Frame {}", *n));
    }
}

void ArCommandInterpreter::CmdLoop(const std::string& args)
{
    if (!m_host.frameHistoryLoop) {
        Error("Error: frame history not available");
        return;
    }
    const std::string t = Trim(args);
    if (t.empty() || ToUpper(t) == "OFF") {
        m_host.frameHistoryLoop(0, 0);
        Info("Loop cleared");
        return;
    }

    auto tokens = Tokenize(t);

    // Transport sub-commands. They act on the timeline whether or not a range is armed - an
    // armed A-B only decides where playback wraps.
    const std::string head = ToUpper(tokens[0]);
    if (head == "PLAY" || head == "PAUSE") {
        if (!m_host.frameHistoryPlay) {
            Error("Error: playback control not available");
            return;
        }
        m_host.frameHistoryPlay(head == "PLAY");
        Info(head == "PLAY" ? "Playing" : "Paused");
        return;
    }
    if (head == "STEP") {
        if (!m_host.frameHistoryStep) {
            Error("Error: playback control not available");
            return;
        }
        int count = 1;
        if (tokens.size() >= 2) {
            try {
                count = std::stoi(tokens[1]);
            } catch (const std::exception&) {
                Error("Usage: LOOP STEP <frames>   (negative steps back)");
                return;
            }
        }
        const int delta = count < 0 ? -1 : 1;
        // 64-bit magnitude: "LOOP STEP -2147483648" would make -count signed-overflow UB.
        const int64_t n = std::abs(static_cast<int64_t>(count));
        for (int64_t i = 0; i < n; ++i) {
            const std::string err = m_host.frameHistoryStep(delta);
            if (!err.empty()) {
                Error(fmt::format("Error: {}", err));
                return;
            }
        }
        Info(fmt::format("Stepped {} frames", count));
        return;
    }
    if (head == "SPEED") {
        if (!m_host.frameHistoryPlaySpeed || tokens.size() < 2) {
            Error("Usage: LOOP SPEED <0.125-2>");
            return;
        }
        try {
            const double x = std::stod(tokens[1]);
            m_host.frameHistoryPlaySpeed(x);
            Info(fmt::format("Speed {:g}x", x));
        } catch (const std::exception&) {
            Error("Usage: LOOP SPEED <0.125-2>");
        }
        return;
    }
    if (head == "DIR") {
        if (!m_host.frameHistoryPlayMode || tokens.size() < 2) {
            Error("Usage: LOOP DIR (FWD|REV|PP)");
            return;
        }
        const std::string d = ToUpper(tokens[1]);
        const int mode = d == "REV" ? 1 : (d == "PP" || d == "PINGPONG") ? 2 : 0;
        if (d != "FWD" && d != "REV" && d != "PP" && d != "PINGPONG") {
            Error("Usage: LOOP DIR (FWD|REV|PP)");
            return;
        }
        m_host.frameHistoryPlayMode(mode);
        Info(fmt::format("Direction {}", d));
        return;
    }

    if (tokens.size() != 2) {
        Error("Usage: LOOP <startFrame> <endFrame>   |   LOOP OFF   |   LOOP "
              "(PLAY|PAUSE|STEP <n>|SPEED <x>|DIR (FWD|REV|PP))");
        return;
    }
    auto a = ParseFrameNumber(tokens[0]);
    auto b = ParseFrameNumber(tokens[1]);
    if (!a || !b) {
        Error("Usage: LOOP <startFrame> <endFrame>   (decimal)   |   LOOP OFF");
        return;
    }

    const std::string err = m_host.frameHistoryLoop(*a, *b);
    if (!err.empty()) {
        Error(fmt::format("Error: {}", err));
        return;
    }
    Info(fmt::format("Looping frames {}-{}", *a, *b));
}

void ArCommandInterpreter::CmdFrameDiff(const std::string& args)
{
    if (!m_host.frameHistoryDiff) {
        Error("Error: frame history not available");
        return;
    }
    auto tokens = Tokenize(Trim(args));
    if (tokens.size() != 2) {
        Error("Usage: DIFF <frameA> <frameB>");
        return;
    }
    auto a = ParseFrameNumber(tokens[0]);
    auto b = ParseFrameNumber(tokens[1]);
    if (!a || !b) {
        Error("Usage: DIFF <frameA> <frameB>   (decimal)");
        return;
    }

    // ENGINE9000 caps its diff output at 4096 lines; an uncapped diff across a screen wipe
    // would bury the console.
    constexpr size_t kMaxRuns = 4096;
    std::vector<FrameDiffRun> runs;
    bool truncated = false;
    const std::string err = m_host.frameHistoryDiff(*a, *b, runs, kMaxRuns, truncated);
    if (!err.empty()) {
        Error(fmt::format("Error: {}", err));
        return;
    }

    if (runs.empty()) {
        Info(fmt::format("Frames {} and {} have identical RAM", *a, *b));
        return;
    }

    size_t totalBytes = 0;
    for (const auto& r : runs)
        totalBytes += r.length;
    Info(fmt::format("Frames {} -> {}: {} changed ranges, {} in total", *a, *b, runs.size(),
                     HumanBytes(totalBytes)));
    for (const auto& r : runs) {
        Info(fmt::format("  {:08X}-{:08X}  {:>8}  {}", r.address, r.address + r.length - 1,
                         HumanBytes(r.length), r.region));
    }
    if (truncated)
        Info(fmt::format("  ... stopped at {} ranges", kMaxRuns));
}

void ArCommandInterpreter::CmdVersion(const std::string&)
{
    auto get = [](const std::function<std::string()>& fn) -> std::string {
        return (fn ? fn() : std::string());
    };
    const std::string app = get(m_host.getAppVersion);
    const std::string core = get(m_host.getCoreVersion);

    auto line = [this](const char* label, const std::string& ver) {
        ArLine l(fmt::format("  {:<16} {}", label, ver.empty() ? "unknown" : ver));
        l.Span(2, 16, ArColor::Prompt);
        Emit(l);
    };
    line("PEEKaboo", app);
    line("PuAE core", core);
    line("Console", std::string(constants::ConsoleVersion));
}

// ---------------------------------------------------------------------------
// Exec / system structure commands (EXECBASE, LIBRARIES, TASKS, ...)
// ---------------------------------------------------------------------------

exec::Readers ArCommandInterpreter::ExecReaders() const
{
    exec::Readers r;
    r.readLong = [this](uint32_t addr, bool& ok) { return m_mem->ReadLong(addr, &ok); };
    r.readByte = [this](uint32_t addr, bool& ok) { return m_mem->ReadByte(addr, &ok); };
    r.isMapped = [this](uint32_t addr) { return m_mem->FindRegion(addr) != nullptr; };
    return r;
}

uint32_t ArCommandInterpreter::ExecBaseAddr()
{
    if (!RequireRunning())
        return 0;
    auto base = exec::ExecBase(ExecReaders());
    if (!base) {
        Error("Error: ExecBase unavailable (no OS booted?)");
        return 0;
    }
    return *base;
}

std::string ArCommandInterpreter::ReadCString(uint32_t addr, size_t maxLen) const
{
    return exec::ReadCString(ExecReaders(), addr, maxLen);
}

void ArCommandInterpreter::ForEachExecNode(uint32_t listAddr, int maxNodes,
                                           const std::function<void(uint32_t)>& fn) const
{
    exec::ForEachNode(ExecReaders(), listAddr, maxNodes, fn);
}

void ArCommandInterpreter::DumpLibraryList(uint32_t listAddr, const char* emptyMsg)
{
    EmitTableHead("Address    Version  Open   Name");
    int count = 0;
    ForEachExecNode(listAddr, kExecListCap, [&](uint32_t node) {
        uint16_t ver = m_mem->ReadWord(node + LIB_VERSION);
        uint16_t rev = m_mem->ReadWord(node + LIB_REVISION);
        uint16_t open = m_mem->ReadWord(node + LIB_OPENCNT);
        std::string name = ReadCString(m_mem->ReadLong(node + LN_NAME));
        ArLine l(fmt::format("${:08X}  {:<7}  {:<5}  {}", node, fmt::format("{}.{}", ver, rev),
                             open, name));
        l.Span(0, 9, ArColor::Address);
        Emit(l);
        ++count;
    });
    if (count == 0)
        Info(emptyMsg);
}

void ArCommandInterpreter::CmdExecBase(const std::string&)
{
    uint32_t base = ExecBaseAddr();
    if (!base)
        return;
    uint16_t ver = m_mem->ReadWord(base + LIB_VERSION);
    uint16_t rev = m_mem->ReadWord(base + LIB_REVISION);
    uint16_t attn = m_mem->ReadWord(base + EB_ATTNFLAGS);
    uint16_t sysf = m_mem->ReadWord(base + EB_SYSFLAGS);
    uint32_t maxloc = m_mem->ReadLong(base + EB_MAXLOCMEM);
    uint32_t thistask = m_mem->ReadLong(base + EB_THISTASK);
    std::string idstr = ReadCString(m_mem->ReadLong(base + LIB_IDSTRING));

    static const char* kAttn[8] = {"68010", "68020", "68030", "68040",
                                   "68881", "68882", "FPU40", "68060"};
    std::string flags;
    for (int b = 0; b < 8; ++b)
        if (attn & (1u << b)) {
            if (!flags.empty())
                flags += ", ";
            flags += kAttn[b];
        }

    EmitTableHead(fmt::format("ExecBase at ${:08X}", base));
    Emit(fmt::format("SoftVer  : {}.{:03d}", ver, rev));
    Emit(fmt::format("AttnFlags: ${:04X}{}", attn,
                     flags.empty() ? "" : fmt::format(" ({})", flags)));
    Emit(fmt::format("SysFlags : ${:04X}        MaxLocMem: ${:08X}", sysf, maxloc));
    Emit(fmt::format("ThisTask : ${:08X}    LibList  : ${:08X}", thistask, base + EB_LIBLIST));
    Emit(fmt::format("MemList  : ${:08X}    ResourceL: ${:08X}", base + EB_MEMLIST,
                     base + EB_RESOURCELIST));
    Emit(fmt::format("DeviceLis: ${:08X}    PortList : ${:08X}", base + EB_DEVICELIST,
                     base + EB_PORTLIST));
    Emit(fmt::format("TaskReady: ${:08X}    TaskWait : ${:08X}", base + EB_TASKREADY,
                     base + EB_TASKWAIT));
    Emit(fmt::format("IDString : '{}'", idstr));
}

void ArCommandInterpreter::CmdAvail(const std::string&)
{
    uint32_t base = ExecBaseAddr();
    if (!base)
        return;
    EmitTableHead("Type       Attributes    Total       Free");
    uint64_t totTotal = 0, totFree = 0;
    ForEachExecNode(base + EB_MEMLIST, kExecListCap, [&](uint32_t node) {
        uint16_t attr = m_mem->ReadWord(node + MH_ATTRIBUTES);
        uint32_t lower = m_mem->ReadLong(node + MH_LOWER);
        uint32_t upper = m_mem->ReadLong(node + MH_UPPER);
        uint32_t freeB = m_mem->ReadLong(node + MH_FREE);
        uint32_t total = (upper >= lower) ? (upper - lower) : 0;
        const char* type = (attr & 0x2) ? "CHIP" : (attr & 0x4) ? "FAST" : "MEM";
        totTotal += total;
        totFree += freeB;
        Emit(fmt::format("{:<10} ${:08X}  {:>9}  {:>9}", type, attr, total, freeB));
    });
    Emit(fmt::format("{:<10} {:<9}   {:>9}  {:>9}", "TOTAL", "", totTotal, totFree));
}

void ArCommandInterpreter::CmdLibraries(const std::string&)
{
    uint32_t base = ExecBaseAddr();
    if (base)
        DumpLibraryList(base + EB_LIBLIST, "(no libraries)");
}

void ArCommandInterpreter::CmdDevices(const std::string&)
{
    uint32_t base = ExecBaseAddr();
    if (base)
        DumpLibraryList(base + EB_DEVICELIST, "(no devices)");
}

void ArCommandInterpreter::CmdResources(const std::string&)
{
    uint32_t base = ExecBaseAddr();
    if (!base)
        return;
    EmitTableHead("Address    Version  Name");
    int count = 0;
    ForEachExecNode(base + EB_RESOURCELIST, kExecListCap, [&](uint32_t node) {
        uint16_t ver = m_mem->ReadWord(node + LIB_VERSION);
        uint16_t rev = m_mem->ReadWord(node + LIB_REVISION);
        std::string name = ReadCString(m_mem->ReadLong(node + LN_NAME));
        ArLine l(fmt::format("${:08X}  {:<7}  {}", node, fmt::format("{}.{}", ver, rev), name));
        l.Span(0, 9, ArColor::Address);
        Emit(l);
        ++count;
    });
    if (count == 0)
        Info("(no resources)");
}

void ArCommandInterpreter::CmdTasks(const std::string&)
{
    uint32_t base = ExecBaseAddr();
    if (!base)
        return;
    EmitTableHead("Address    Pri  State    Signals    Name");
    auto stateName = [](uint8_t s) -> const char* {
        switch (s) {
            case 1:
                return "Added";
            case 2:
                return "Run";
            case 3:
                return "Ready";
            case 4:
                return "Wait";
            case 5:
                return "Except";
            case 6:
                return "Removed";
            default:
                return "?";
        }
    };
    auto emitTask = [&](uint32_t node, const char* forced) {
        int pri = static_cast<int8_t>(m_mem->ReadByte(node + LN_PRI));
        uint8_t st = m_mem->ReadByte(node + TC_STATE);
        uint32_t sig = m_mem->ReadLong(node + TC_SIGWAIT);
        std::string name = ReadCString(m_mem->ReadLong(node + LN_NAME));
        ArLine l(fmt::format("${:08X}  {:>3}  {:<7}  ${:08X}  {}", node, pri,
                             forced ? forced : stateName(st), sig, name));
        l.Span(0, 9, ArColor::Address);
        Emit(l);
    };
    uint32_t thistask = m_mem->ReadLong(base + EB_THISTASK);
    if (thistask)
        emitTask(thistask, "Run");
    ForEachExecNode(base + EB_TASKREADY, kExecListCap, [&](uint32_t n) { emitTask(n, nullptr); });
    ForEachExecNode(base + EB_TASKWAIT, kExecListCap, [&](uint32_t n) { emitTask(n, nullptr); });
}

void ArCommandInterpreter::CmdPorts(const std::string&)
{
    uint32_t base = ExecBaseAddr();
    if (!base)
        return;
    EmitTableHead("Address    Pri  MsgCnt  Name");
    int count = 0;
    ForEachExecNode(base + EB_PORTLIST, kExecListCap, [&](uint32_t node) {
        int pri = static_cast<int8_t>(m_mem->ReadByte(node + LN_PRI));
        int msgs = 0;
        ForEachExecNode(node + MP_MSGLIST, kExecListCap, [&](uint32_t) { ++msgs; });
        std::string name = ReadCString(m_mem->ReadLong(node + LN_NAME));
        ArLine l(fmt::format("${:08X}  {:>3}  {:>6}  {}", node, pri, msgs, name));
        l.Span(0, 9, ArColor::Address);
        Emit(l);
        ++count;
    });
    if (count == 0)
        Info("(no public ports)");
}

void ArCommandInterpreter::CmdInterrupts(const std::string&)
{
    uint32_t base = ExecBaseAddr();
    if (!base)
        return;
    static const char* kIntName[16] = {"TBE",     "DSKBLK", "SOFTINT", "PORTS", "COPER", "VERTB",
                                       "BLIT",    "AUD0",   "AUD1",    "AUD2",  "AUD3",  "RBF",
                                       "DSKSYNC", "EXTER",  "INTEN",   "NMI"};
    EmitTableHead("INT   PRI   NAME               SERVER / CODE");
    for (int i = 0; i < 16; ++i) {
        uint32_t iv = base + EB_INTVECTS + static_cast<uint32_t>(i) * IV_SIZE;
        uint32_t code = m_mem->ReadLong(iv + IV_CODE);
        uint32_t nodeP = m_mem->ReadLong(iv + IV_NODE);
        std::string name = kIntName[i];
        std::string pri = "-";
        if (nodeP) {
            std::string nn = ReadCString(m_mem->ReadLong(nodeP + LN_NAME));
            if (!nn.empty())
                name = nn;
            pri = fmt::format(
                "{}", static_cast<int>(static_cast<int8_t>(m_mem->ReadByte(nodeP + LN_PRI))));
        }
        ArLine l(fmt::format("{:02d}    {:<5} {:<18} ${:08X}", i, pri, name, code));
        l.Span(static_cast<int>(l.text.size()) - 9, 9, ArColor::Address);
        Emit(l);
    }
}

void ArCommandInterpreter::CmdExceptions(const std::string&)
{
    if (!RequireRunning())
        return;
    struct Vec
    {
        int num;
        const char* name;
    };
    static const Vec kVecs[] = {
        {2, "Bus Error"},      {3, "Address Error"},  {4, "Illegal Instruct."}, {5, "Zero Divide"},
        {6, "CHK Instruct."},  {7, "TRAPV"},          {8, "Privilege Viol."},   {9, "Trace"},
        {10, "Line-A (1010)"}, {11, "Line-F (1111)"}, {24, "Spurious Int"},     {25, "Level 1 Int"},
        {26, "Level 2 Int"},   {27, "Level 3 Int"},   {28, "Level 4 Int"},      {29, "Level 5 Int"},
        {30, "Level 6 Int"},   {31, "Level 7 Int"},
    };
    // Locate the Kickstart ROM so we can flag redirected (hooked) vectors.
    uint32_t romLo = 0, romHi = 0;
    for (const auto& r : m_mem->Regions())
        if (r.name.find("Kickstart") != std::string::npos ||
            r.name.find("ROM") != std::string::npos) {
            romLo = r.base;
            romHi = r.base + r.size;
            break;
        }
    auto emitVec = [&](int num, const std::string& name) {
        uint32_t off = static_cast<uint32_t>(num) * 4;
        bool ok = false;
        uint32_t addr = m_mem->ReadLong(off, &ok);
        if (!ok)
            return;
        bool hooked = romHi && addr != 0 && (addr < romLo || addr >= romHi);
        ArLine l(fmt::format("${:02X}  {:02d}   {:<18} ${:08X}{}", off, num, name, addr,
                             hooked ? " [hooked]" : ""));
        l.Span(0, 3, ArColor::Address);
        Emit(l);
    };
    EmitTableHead("VEC  NUM  NAME               ADDRESS");
    for (const auto& v : kVecs)
        emitVec(v.num, v.name);
    for (int t = 0; t < 16; ++t)
        emitVec(32 + t, fmt::format("TRAP #{}", t));
}

void ArCommandInterpreter::CmdChipRegs(const std::string&)
{
    if (!RequireRunning())
        return;
    if (!m_host.customRegName) {
        Error("Error: register names unavailable");
        return;
    }
    EmitTableHead("REG      OFFSET  NAME      VALUE  DESCRIPTION");
    for (uint32_t off = 0; off < 0x200; off += 2) {
        std::string name = m_host.customRegName(off);
        if (name.empty())
            continue;
        uint16_t val = m_mem->ReadWord(kCustomBase + off);
        std::string desc = m_host.customRegDesc ? m_host.customRegDesc(off) : std::string();
        ArLine l(fmt::format("$DFF{:03X}  ${:03X}  {:<9} ${:04X}  {}", off, off, name, val, desc));
        l.Span(0, 7, ArColor::Address);
        l.Span(15, static_cast<int>(name.size()), ArColor::Register, off);
        Emit(l);
    }
}

std::optional<uint32_t> ArCommandInterpreter::ResolveCustomReg(const std::string& name) const
{
    std::string up = ToUpper(Trim(name));
    if (!up.empty() && m_host.customRegName)
        for (uint32_t off = 0; off < 0x200; off += 2)
            if (ToUpper(m_host.customRegName(off)) == up)
                return off;
    if (auto v = ParseNumber(name); v && *v < 0x200)
        return *v & 0x1FE;
    return std::nullopt;
}

void ArCommandInterpreter::CmdDChip(const std::string& args)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: DCHIP <register>");
        return;
    }
    auto off = ResolveCustomReg(tokens[0]);
    if (!off) {
        Error(fmt::format("Error: unknown register '{}'", tokens[0]));
        return;
    }
    std::string name = m_host.customRegName ? m_host.customRegName(*off) : std::string();
    if (name.empty())
        name = ToUpper(tokens[0]);
    uint16_t val = m_mem->ReadWord(kCustomBase + *off);
    Emit(fmt::format("{} ($DFF{:03X}) = ${:04X}", name, *off, val), ArColor::Highlight);
    std::vector<ArHost::RegBitField> bits =
        m_host.customRegBits ? m_host.customRegBits(*off) : std::vector<ArHost::RegBitField>{};
    if (bits.empty()) {
        Info("(no bit definition for this register)");
        return;
    }
    EmitTableHead("Bits        Field            Value");
    for (const auto& f : bits) {
        int width = f.hi - f.lo + 1;
        uint32_t mask = (width >= 16) ? 0xFFFFu : ((1u << width) - 1u);
        uint32_t fv = (val >> f.lo) & mask;
        std::string label = (f.hi == f.lo) ? fmt::format("Bit {:02d}", f.lo)
                                           : fmt::format("Bit {:02d}-{:02d}", f.hi, f.lo);
        Emit(fmt::format("{:<10}: {:<14} = {}", label, f.name, fv));
    }
}

void ArCommandInterpreter::CmdKickVer(const std::string&)
{
    uint32_t base = ExecBaseAddr();
    if (!base)
        return;
    uint16_t ver = m_mem->ReadWord(base + LIB_VERSION);
    uint16_t rev = m_mem->ReadWord(base + LIB_REVISION);
    std::string idstr = ReadCString(m_mem->ReadLong(base + LIB_IDSTRING));

    auto osName = [](uint16_t v) -> const char* {
        switch (v) {
            case 33:
                return "AmigaOS 1.2";
            case 34:
            case 35:
                return "AmigaOS 1.3";
            case 36:
                return "AmigaOS 2.0";
            case 37:
                return "AmigaOS 2.04";
            case 38:
                return "AmigaOS 2.1";
            case 39:
                return "AmigaOS 3.0";
            case 40:
                return "AmigaOS 3.1";
            case 45:
                return "AmigaOS 3.5";
            case 46:
                return "AmigaOS 3.9";
            case 47:
                return "AmigaOS 3.1.4";
            case 50:
            case 51:
            case 52:
            case 53:
            case 54:
                return "AmigaOS 3.2";
            default:
                return "AmigaOS";
        }
    };
    // Revision date lives in the IDString parentheses, e.g. "exec 40.68 (15.10.93)".
    std::string date;
    auto p = idstr.find('(');
    auto q = (p == std::string::npos) ? std::string::npos : idstr.find(')', p);
    if (p != std::string::npos && q != std::string::npos && q > p + 1)
        date = idstr.substr(p + 1, q - p - 1);

    uint32_t romLo = 0, romHi = 0;
    for (const auto& r : m_mem->Regions())
        if (r.name.find("Kickstart") != std::string::npos ||
            r.name.find("ROM") != std::string::npos) {
            romLo = r.base;
            romHi = r.base + r.size;
            break;
        }

    EmitTableHead(fmt::format("Kickstart Version: {}.{} ({})", ver, rev, osName(ver)));
    if (!date.empty())
        Emit(fmt::format("Revision Date    : {}", date));
    if (romHi)
        Emit(fmt::format("ROM Address      : ${:08X} - ${:08X}", romLo, romHi - 1));
    if (!idstr.empty())
        Emit(fmt::format("ID String        : {}", idstr));
}

// ---------------------------------------------------------------------------
// Memory annotations (TM / TMS / TMD)
// ---------------------------------------------------------------------------

// Parses the shared `<addr> "<text>"` form used by TM and TL. Emits the right
// usage/error and returns false on any problem; the `what` word names the kind
// of text ("comment"/"label") for messages.
bool ArCommandInterpreter::ParseTagArgs(const std::string& args, const char* cmd, const char* what,
                                        uint32_t& addrOut, std::string& textOut)
{
    std::string a = Trim(args);
    size_t sp = a.find_first_of(" \t");
    if (sp == std::string::npos) {
        Error(fmt::format("Usage: {} <addr> \"<{}>\"", cmd, what));
        return false;
    }
    auto addr = ParseNumber(a.substr(0, sp));
    if (!addr) {
        Error("Error: invalid address");
        return false;
    }
    std::string rest = Trim(a.substr(sp + 1));
    if (rest.size() < 2 || rest.front() != '"' || rest.back() != '"') {
        Error(fmt::format("Error: {} must be in double quotes", what));
        return false;
    }
    std::string text = rest.substr(1, rest.size() - 2);
    if (text.empty()) {
        Error(fmt::format("Error: empty {}", what));
        return false;
    }
    addrOut = *addr;
    textOut = std::move(text);
    return true;
}

void ArCommandInterpreter::CmdTagMem(const std::string& args)
{
    if (!m_host.annotations) {
        Error("Error: annotations unavailable");
        return;
    }
    // TM <addr> "<comment>"
    uint32_t addr = 0;
    std::string text;
    if (!ParseTagArgs(args, "TM", "comment", addr, text))
        return;
    m_host.annotations->SetComment(addr, text);
    Info(fmt::format("Tagged ${:08X}: {}", addr, text));
    StateChanged();
}

void ArCommandInterpreter::CmdTagLabel(const std::string& args)
{
    if (!m_host.annotations) {
        Error("Error: annotations unavailable");
        return;
    }
    // TL <addr> "<label>"
    uint32_t addr = 0;
    std::string text;
    if (!ParseTagArgs(args, "TL", "label", addr, text))
        return;
    m_host.annotations->SetLabel(addr, text);
    Info(fmt::format("Labelled ${:08X}: {}", addr, text));
    StateChanged();
}

void ArCommandInterpreter::CmdTagMemShow(const std::string&)
{
    if (!m_host.annotations) {
        Error("Error: annotations unavailable");
        return;
    }
    const auto& all = m_host.annotations->All();
    if (all.empty()) {
        Info("No annotations defined.");
        return;
    }
    EmitTableHead("Address    Label            Comment");
    constexpr int kConsoleDividerCols = 60;
    for (const auto& [addr, ann] : all) {
        ArLine l(fmt::format("${:08X}  {:<15}  {}", addr, ann.label, ann.comment));
        l.Span(0, 9, ArColor::Address);
        if (!ann.label.empty())
            l.Span(11, 15, ArColor::Label);
        Emit(l);
        // Multi-line comment block, framed and dimmed like in the disassembly view.
        if (!ann.block.empty())
            for (const auto& line :
                 MemoryAnnotations::BuildCommentBlockLines(ann.block, kConsoleDividerCols))
                Emit(ArLine("  " + line, ArColor::Comment));
    }
}

void ArCommandInterpreter::CmdTagMemDelete(const std::string& args)
{
    if (!m_host.annotations) {
        Error("Error: annotations unavailable");
        return;
    }
    auto addr = ParseNumber(Trim(args));
    if (!addr) {
        Error("Error: invalid address");
        return;
    }
    // Per spec: deleting a non-existent annotation is a silent no-op.
    if (m_host.annotations->Remove(*addr)) {
        Info(fmt::format("Removed annotation at ${:08X}", *addr));
        StateChanged();
    }
}

namespace {
// Converts literal "\n" (and "\\") escape sequences in a console-entered string
// into real newlines, so a single-line command can carry a multi-line block.
std::string UnescapeNewlines(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            char n = in[i + 1];
            if (n == 'n') {
                out.push_back('\n');
                ++i;
                continue;
            }
            if (n == '\\') {
                out.push_back('\\');
                ++i;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}
} // namespace

void ArCommandInterpreter::CmdTagBlock(const std::string& args)
{
    if (!m_host.annotations) {
        Error("Error: annotations unavailable");
        return;
    }
    // TB <addr> "<text>"  — \n in the text becomes a line break.
    uint32_t addr = 0;
    std::string text;
    if (!ParseTagArgs(args, "TB", "block", addr, text))
        return;
    text = UnescapeNewlines(text);
    m_host.annotations->SetBlock(addr, text);
    Info(fmt::format("Block set at ${:08X}", addr));
    StateChanged();
}

void ArCommandInterpreter::CmdTagBlockDelete(const std::string& args)
{
    if (!m_host.annotations) {
        Error("Error: annotations unavailable");
        return;
    }
    auto addr = ParseNumber(Trim(args));
    if (!addr) {
        Error("Error: invalid address");
        return;
    }
    if (m_host.annotations->RemoveBlock(*addr)) {
        Info(fmt::format("Removed comment block at ${:08X}", *addr));
        StateChanged();
    }
}

// ---------------------------------------------------------------------------
// Disk I/O helpers
// ---------------------------------------------------------------------------

std::optional<uint32_t> ArCommandInterpreter::ParseDiskAddr(const std::string& token) const
{
    if (token.empty())
        return std::nullopt;
    char type = static_cast<char>(std::toupper(static_cast<unsigned char>(token[0])));
    if ((type == 'T' || type == 'S') && token.size() >= 2) {
        auto n = ParseNumber(token.substr(1));
        if (!n)
            return std::nullopt;
        if (m_diskBufAddr == 0) {
            // T/S references require a previously allocated buffer
            return std::nullopt;
        }
        if (type == 'T')
            return m_diskBufAddr + *n * static_cast<uint32_t>(kDiskTrackBytes);
        else
            return m_diskBufAddr + *n * static_cast<uint32_t>(kDiskSectorBytes);
    }
    return ParseNumber(token);
}

uint32_t ArCommandInterpreter::AllocateDiskBuffer(size_t size) const
{
    const ArMemoryAccess::Region* best = nullptr;
    for (const auto& r : m_mem->Regions()) {
        if (r.writable && r.size >= size) {
            if (!best || r.size > best->size)
                best = &r;
        }
    }
    if (!best)
        return 0;
    uint32_t aligned = static_cast<uint32_t>((size + 511u) & ~511u);
    return (best->base + best->size - aligned) & ~511u;
}

// Read numBytes disk bytes starting at diskByteOffset into Amiga memory at amigaDest.
bool ArCommandInterpreter::DiskReadRange(uint32_t diskByteOffset, uint32_t numBytes,
                                         uint32_t amigaDest)
{
    if (!m_host.readDiskTrack) {
        Error("Error: disk read not available");
        return false;
    }
    if (diskByteOffset + numBytes > kDiskTotalBytes) {
        Error("Error: range exceeds disk size");
        return false;
    }

    uint8_t trackBuf[kDiskTrackBytes];
    uint32_t remaining = numBytes;
    uint32_t diskPos = diskByteOffset;
    uint32_t destPos = amigaDest;

    while (remaining > 0) {
        int track = static_cast<int>(diskPos / kDiskTrackBytes);
        int trackOff = static_cast<int>(diskPos % kDiskTrackBytes);

        int r = m_host.readDiskTrack(0, track, trackBuf, kDiskTrackBytes);
        if (r < 0) {
            Error(fmt::format("Error: track {} read failed", track));
            return false;
        }

        uint32_t canRead = std::min(static_cast<uint32_t>(kDiskTrackBytes - trackOff), remaining);
        std::string err;
        if (!m_mem->Write(destPos, trackBuf + trackOff, canRead, err)) {
            Error("Error: " + err);
            return false;
        }

        diskPos += canRead;
        destPos += canRead;
        remaining -= canRead;
    }
    return true;
}

// Read Amiga memory at amigaSrc and write numBytes to disk at diskByteOffset.
bool ArCommandInterpreter::DiskWriteRange(uint32_t diskByteOffset, uint32_t numBytes,
                                          uint32_t amigaSrc)
{
    if (!m_host.writeDiskBytes) {
        Error("Error: disk write not available");
        return false;
    }
    if (diskByteOffset + numBytes > kDiskTotalBytes) {
        Error("Error: range exceeds disk size");
        return false;
    }

    std::vector<uint8_t> buf(numBytes);
    if (m_mem->Read(amigaSrc, buf.data(), numBytes) != numBytes) {
        Error("Error: source range unmapped");
        return false;
    }
    if (!m_host.writeDiskBytes(0, diskByteOffset, buf.data(), numBytes)) {
        Error("Error: disk write failed");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// RT/RS/RB — read disk into Amiga memory
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdReadTrack(const std::string& args)
{
    if (!RequireRunning())
        return;
    if (!m_host.readDiskTrack) {
        Error("Error: disk read not available");
        return;
    }

    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: RT <track> (<num-tracks> <dest-addr>)");
        return;
    }

    auto startTrack = ParseNumber(tokens[0]);
    if (!startTrack || *startTrack >= static_cast<uint32_t>(kDiskNumTracks)) {
        Error("Error: bad track (0-!159)");
        return;
    }

    uint32_t numTracks = 1;
    if (tokens.size() >= 2) {
        auto n = ParseNumber(tokens[1]);
        if (!n || *n == 0) {
            Error("Error: bad count");
            return;
        }
        numTracks = *n;
    }
    if (*startTrack + numTracks > static_cast<uint32_t>(kDiskNumTracks)) {
        Error("Error: track range exceeds disk (0-!159)");
        return;
    }

    std::optional<uint32_t> destAddr;
    if (tokens.size() >= 3) {
        destAddr = ParseNumber(tokens[2]);
        if (!destAddr) {
            Error("Error: bad address");
            return;
        }
    }

    size_t totalBytes = numTracks * static_cast<size_t>(kDiskTrackBytes);
    if (!destAddr) {
        uint32_t buf = AllocateDiskBuffer(totalBytes);
        if (!buf) {
            Error("Error: no memory for buffer");
            return;
        }
        destAddr = buf;
        m_diskBufAddr = buf;
    }

    uint32_t diskOffset = *startTrack * static_cast<uint32_t>(kDiskTrackBytes);
    if (!DiskReadRange(diskOffset, static_cast<uint32_t>(totalBytes), *destAddr))
        return;

    StateChanged();
    uint32_t endTrack = *startTrack + numTracks - 1;
    Info(fmt::format("Track {}-{} → ${:08X} ({} bytes)", *startTrack, endTrack, *destAddr,
                     totalBytes));
}

void ArCommandInterpreter::CmdReadSectors(const std::string& args)
{
    if (!RequireRunning())
        return;
    if (!m_host.readDiskTrack) {
        Error("Error: disk read not available");
        return;
    }

    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: RS <sector> (<num-sectors> <dest-addr>)");
        return;
    }

    auto startSector = ParseNumber(tokens[0]);
    if (!startSector || *startSector >= static_cast<uint32_t>(kDiskNumSectors)) {
        Error("Error: bad sector (0-!1759)");
        return;
    }

    uint32_t numSectors = 1;
    if (tokens.size() >= 2) {
        auto n = ParseNumber(tokens[1]);
        if (!n || *n == 0) {
            Error("Error: bad count");
            return;
        }
        numSectors = *n;
    }
    if (*startSector + numSectors > static_cast<uint32_t>(kDiskNumSectors)) {
        Error("Error: sector range exceeds disk (0-!1759)");
        return;
    }

    std::optional<uint32_t> destAddr;
    if (tokens.size() >= 3) {
        destAddr = ParseNumber(tokens[2]);
        if (!destAddr) {
            Error("Error: bad address");
            return;
        }
    }

    size_t totalBytes = numSectors * static_cast<size_t>(kDiskSectorBytes);
    if (!destAddr) {
        uint32_t buf = AllocateDiskBuffer(totalBytes);
        if (!buf) {
            Error("Error: no memory for buffer");
            return;
        }
        destAddr = buf;
        m_diskBufAddr = buf;
    }

    uint32_t diskOffset = *startSector * static_cast<uint32_t>(kDiskSectorBytes);
    if (!DiskReadRange(diskOffset, static_cast<uint32_t>(totalBytes), *destAddr))
        return;

    StateChanged();
    uint32_t endSector = *startSector + numSectors - 1;
    Info(fmt::format("Sector {}-{} → ${:08X} ({} bytes)", *startSector, endSector, *destAddr,
                     totalBytes));
}

void ArCommandInterpreter::CmdReadBytes(const std::string& args)
{
    if (!RequireRunning())
        return;
    if (!m_host.readDiskTrack) {
        Error("Error: disk read not available");
        return;
    }

    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: RB <offset> (<num-bytes> <dest-addr>)");
        return;
    }

    auto startOffset = ParseNumber(tokens[0]);
    if (!startOffset || *startOffset >= kDiskTotalBytes) {
        Error("Error: bad offset (0-!901119)");
        return;
    }

    uint32_t numBytes = static_cast<uint32_t>(kDiskSectorBytes);
    if (tokens.size() >= 2) {
        auto n = ParseNumber(tokens[1]);
        if (!n || *n == 0) {
            Error("Error: bad count");
            return;
        }
        numBytes = *n;
    }
    if (*startOffset + numBytes > kDiskTotalBytes) {
        Error("Error: range exceeds disk size (0-!901119)");
        return;
    }

    std::optional<uint32_t> destAddr;
    if (tokens.size() >= 3) {
        destAddr = ParseNumber(tokens[2]);
        if (!destAddr) {
            Error("Error: bad address");
            return;
        }
    }

    if (!destAddr) {
        uint32_t buf = AllocateDiskBuffer(numBytes);
        if (!buf) {
            Error("Error: no memory for buffer");
            return;
        }
        destAddr = buf;
        m_diskBufAddr = buf;
    }

    if (!DiskReadRange(*startOffset, numBytes, *destAddr))
        return;

    StateChanged();
    Info(fmt::format("Offset ${:06X} → ${:08X} ({} bytes)", *startOffset, *destAddr, numBytes));
}

// ---------------------------------------------------------------------------
// WT/WDS/WDB — write Amiga memory to disk
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdWriteTrack(const std::string& args)
{
    if (!RequireRunning())
        return;
    if (!m_host.writeDiskBytes) {
        Error("Error: disk write not available");
        return;
    }

    auto tokens = Tokenize(args);
    if (tokens.size() < 3) {
        Error("Usage: WT <track> <num-tracks> <src-addr>");
        return;
    }

    auto startTrack = ParseNumber(tokens[0]);
    if (!startTrack || *startTrack >= static_cast<uint32_t>(kDiskNumTracks)) {
        Error("Error: bad track (0-!159)");
        return;
    }
    auto numTracks = ParseNumber(tokens[1]);
    if (!numTracks || *numTracks == 0) {
        Error("Error: bad count");
        return;
    }
    if (*startTrack + *numTracks > static_cast<uint32_t>(kDiskNumTracks)) {
        Error("Error: track range exceeds disk (0-!159)");
        return;
    }

    auto srcAddr = ParseDiskAddr(tokens[2]);
    if (!srcAddr) {
        Error("Error: bad address (use hex addr, T<track>, or S<sector>)");
        return;
    }

    uint32_t diskOffset = *startTrack * static_cast<uint32_t>(kDiskTrackBytes);
    uint32_t numBytes = *numTracks * static_cast<uint32_t>(kDiskTrackBytes);
    if (!DiskWriteRange(diskOffset, numBytes, *srcAddr))
        return;

    StateChanged();
    Info(fmt::format("Wrote {} track(s) from ${:08X}", *numTracks, *srcAddr));
}

void ArCommandInterpreter::CmdWriteSectors(const std::string& args)
{
    if (!RequireRunning())
        return;
    if (!m_host.writeDiskBytes) {
        Error("Error: disk write not available");
        return;
    }

    auto tokens = Tokenize(args);
    if (tokens.size() < 3) {
        Error("Usage: WDS <sector> <num-sectors> <src-addr>");
        return;
    }

    auto startSector = ParseNumber(tokens[0]);
    if (!startSector || *startSector >= static_cast<uint32_t>(kDiskNumSectors)) {
        Error("Error: bad sector (0-!1759)");
        return;
    }
    auto numSectors = ParseNumber(tokens[1]);
    if (!numSectors || *numSectors == 0) {
        Error("Error: bad count");
        return;
    }
    if (*startSector + *numSectors > static_cast<uint32_t>(kDiskNumSectors)) {
        Error("Error: sector range exceeds disk (0-!1759)");
        return;
    }

    auto srcAddr = ParseDiskAddr(tokens[2]);
    if (!srcAddr) {
        Error("Error: bad address (use hex addr, T<track>, or S<sector>)");
        return;
    }

    uint32_t diskOffset = *startSector * static_cast<uint32_t>(kDiskSectorBytes);
    uint32_t numBytes = *numSectors * static_cast<uint32_t>(kDiskSectorBytes);
    if (!DiskWriteRange(diskOffset, numBytes, *srcAddr))
        return;

    StateChanged();
    Info(fmt::format("Wrote {} sector(s) from ${:08X}", *numSectors, *srcAddr));
}

void ArCommandInterpreter::CmdWriteBytes(const std::string& args)
{
    if (!RequireRunning())
        return;
    if (!m_host.writeDiskBytes) {
        Error("Error: disk write not available");
        return;
    }

    auto tokens = Tokenize(args);
    if (tokens.size() < 3) {
        Error("Usage: WDB <offset> <num-bytes> <src-addr>");
        return;
    }

    auto startOffset = ParseNumber(tokens[0]);
    if (!startOffset || *startOffset >= kDiskTotalBytes) {
        Error("Error: bad offset (0-!901119)");
        return;
    }
    auto numBytes = ParseNumber(tokens[1]);
    if (!numBytes || *numBytes == 0) {
        Error("Error: bad count");
        return;
    }
    if (*startOffset + *numBytes > kDiskTotalBytes) {
        Error("Error: range exceeds disk size");
        return;
    }

    auto srcAddr = ParseDiskAddr(tokens[2]);
    if (!srcAddr) {
        Error("Error: bad address (use hex addr, T<track>, or S<sector>)");
        return;
    }

    if (!DiskWriteRange(*startOffset, *numBytes, *srcAddr))
        return;

    StateChanged();
    Info(fmt::format("Wrote {} byte(s) from ${:08X}", *numBytes, *srcAddr));
}

// ---------------------------------------------------------------------------
// BOOTCHK / DATACHK — fix AmigaDOS block checksums in Amiga memory
// ---------------------------------------------------------------------------

static uint32_t CalcAmigaChecksum(const uint8_t* block, int numLongs, int skipLong)
{
    uint32_t sum = 0;
    for (int i = 0; i < numLongs; i++) {
        if (i == skipLong)
            continue;
        uint32_t val = (uint32_t(block[i * 4 + 0]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                       (uint32_t(block[i * 4 + 2]) << 8) | block[i * 4 + 3];
        uint32_t newsum = sum + val;
        if (newsum < sum)
            newsum++; // carry
        sum = newsum;
    }
    return ~sum;
}

static void StoreBeU32(uint8_t* p, uint32_t v)
{
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

void ArCommandInterpreter::CmdBootCheck(const std::string& args)
{
    if (!RequireRunning())
        return;

    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: BOOTCHK <addr>");
        return;
    }
    auto addr = ParseNumber(tokens[0]);
    if (!addr) {
        Error("Error: bad address");
        return;
    }

    // Bootblock is 2 sectors = 1024 bytes; checksum at longword offset 1 (bytes 4-7)
    const int kBootSize = 1024;
    uint8_t block[kBootSize];
    if (m_mem->Read(*addr, block, kBootSize) != kBootSize) {
        Error("Error: range unmapped");
        return;
    }

    uint32_t chk = CalcAmigaChecksum(block, kBootSize / 4, 1 /*skip checksum longword*/);
    StoreBeU32(block + 4, chk);

    std::string err;
    if (!m_mem->Write(*addr, block, kBootSize, err)) {
        Error("Error: " + err);
        return;
    }
    StateChanged();
    Info(fmt::format("Bootblock checksum fixed: ${:08X}", chk));
}

void ArCommandInterpreter::CmdDataCheck(const std::string& args)
{
    if (!RequireRunning())
        return;

    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: DATACHK <addr>");
        return;
    }
    auto addr = ParseNumber(tokens[0]);
    if (!addr) {
        Error("Error: bad address");
        return;
    }

    // OFS data block is 512 bytes; checksum at longword offset 5 (bytes 20-23)
    const int kBlockSize = 512;
    uint8_t block[kBlockSize];
    if (m_mem->Read(*addr, block, kBlockSize) != kBlockSize) {
        Error("Error: range unmapped");
        return;
    }

    uint32_t chk = CalcAmigaChecksum(block, kBlockSize / 4, 5 /*skip checksum longword*/);
    StoreBeU32(block + 20, chk);

    std::string err;
    if (!m_mem->Write(*addr, block, kBlockSize, err)) {
        Error("Error: " + err);
        return;
    }
    StateChanged();
    Info(fmt::format("Data block checksum fixed: ${:08X}", chk));
}

void ArCommandInterpreter::CmdBamCheck(const std::string& args)
{
    if (!RequireRunning())
        return;

    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Usage: BAMCHK <addr>");
        return;
    }
    auto addr = ParseNumber(tokens[0]);
    if (!addr) {
        Error("Error: bad address");
        return;
    }

    // Bitmap block is 512 bytes; checksum at longword 0. The block is valid when
    // the simple 32-bit sum of all longwords is zero (two's-complement, no carry
    // fold — unlike the boot/data/root checksum).
    const int kBlockSize = 512;
    uint8_t block[kBlockSize];
    if (m_mem->Read(*addr, block, kBlockSize) != kBlockSize) {
        Error("Error: range unmapped");
        return;
    }

    uint32_t sum = 0;
    for (int i = 1; i < kBlockSize / 4; ++i)
        sum += (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | block[i * 4 + 3];
    uint32_t chk = 0u - sum;
    StoreBeU32(block, chk);

    std::string err;
    if (!m_mem->Write(*addr, block, kBlockSize, err)) {
        Error("Error: " + err);
        return;
    }
    StateChanged();
    Info(fmt::format("Bitmap checksum fixed: ${:08X}", chk));
}

// ---------------------------------------------------------------------------
// ? calculator
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdCalc(const std::string& args)
{
    std::string expr = Trim(args);
    if (expr.empty()) {
        Error("Usage: ? <expression>");
        return;
    }
    ExprEval ev{expr};
    uint32_t v = ev.expr();
    ev.skip();
    if (!ev.ok || ev.i != expr.size()) {
        Error("Error: bad expression");
        return;
    }
    std::string out = fmt::format("= ${:X} = !{} = %{:b}", v, v, v);
    if (v & 0x80000000u)
        out += fmt::format(" (signed !{})", static_cast<int32_t>(v));
    Info(out);
}

// ---------------------------------------------------------------------------
// CRC16 / CRC32 — checksum a memory range
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdCrc(const std::string& args, bool crc32)
{
    if (!RequireRunning())
        return;
    auto tokens = Tokenize(args);
    if (tokens.size() != 2) {
        Error(crc32 ? "Usage: CRC32 <start> <end>" : "Usage: CRC16 <start> <end>");
        return;
    }
    auto start = ParseNumber(tokens[0]);
    auto end = ParseNumber(tokens[1]);
    if (!start || !end || *end < *start) {
        Error("Error: bad address");
        return;
    }
    size_t len = static_cast<size_t>(*end - *start) + 1;
    std::vector<uint8_t> buf(len);
    if (m_mem->Read(*start, buf.data(), len) != len) {
        Error("Error: range unmapped");
        return;
    }
    if (crc32)
        Info(fmt::format("CRC32: {:08X}  ({} bytes)", Crc32Mem(buf.data(), len), len));
    else
        Info(fmt::format("CRC16: {:04X}  ({} bytes)", Crc16Mem(buf.data(), len), len));
}

// ---------------------------------------------------------------------------
// W — CIA registers (read-only dump via the UAE debugger)
// ---------------------------------------------------------------------------

std::vector<std::string> ArCommandInterpreter::CaptureUaeLines(const std::string& cmd)
{
    std::vector<std::string> out;
    if (!m_host.execUaeCommand)
        return out;
    std::string s;
    if (!m_host.execUaeCommand(cmd, s))
        return out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            std::string line = s.substr(start, i - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            out.push_back(line);
            start = i + 1;
        }
    }
    return out;
}

void ArCommandInterpreter::CmdCia(const std::string&)
{
    if (!RequireRunning())
        return;
    if (!m_host.execUaeCommand) {
        Error("Error: CIA dump unavailable");
        return;
    }
    auto lines = CaptureUaeLines("c");
    // dumpcia() emits 6 lines: A: + 2 indented, B: + 2 indented.
    size_t startIdx = lines.size();
    for (size_t k = 0; k < lines.size(); ++k) {
        std::string t = Trim(lines[k]);
        if (t.rfind("A: CRA", 0) == 0 || t.rfind("A:CRA", 0) == 0) {
            startIdx = k;
            break;
        }
    }
    if (startIdx >= lines.size()) {
        Error("Error: CIA state unavailable");
        return;
    }
    Emit(ArLine("CIA REGISTERS (read-only)", ArColor::Highlight));
    Emit("-------------------------");
    int emitted = 0;
    for (size_t k = startIdx; k < lines.size() && emitted < 6; ++k) {
        std::string t = Trim(lines[k]);
        bool cia = t.rfind("A:", 0) == 0 || t.rfind("B:", 0) == 0 || t.rfind("PRA", 0) == 0 ||
                   t.rfind("TOD", 0) == 0 || t.rfind("ALARM", 0) == 0;
        if (!cia)
            break;
        Emit(lines[k]);
        ++emitted;
    }
}

// ---------------------------------------------------------------------------
// PALETTE — dump the 32 OCS/ECS colour registers
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdPalette(const std::string&)
{
    if (!RequireRunning())
        return;
    EmitTableHead("Reg    Offset   RGB    24-bit   Swatch");
    for (int n = 0; n < 32; ++n) {
        uint32_t off = 0x180 + static_cast<uint32_t>(n) * 2; // COLOR00..COLOR31
        bool ok = false;
        uint16_t v = m_mem->ReadWord(kCustomBase + off, &ok);
        uint16_t rgb = v & 0x0FFF;
        // 4-bit channels expanded to 8-bit (replicate nibble)
        uint8_t r = static_cast<uint8_t>((rgb >> 8) & 0xF);
        uint8_t g = static_cast<uint8_t>((rgb >> 4) & 0xF);
        uint8_t b = static_cast<uint8_t>(rgb & 0xF);
        uint32_t rgb24 = (uint32_t(r << 4 | r) << 16) | (uint32_t(g << 4 | g) << 8) | (b << 4 | b);
        std::string text =
            fmt::format("COLOR{:02d} $DFF{:03X}  ${:03X}   #{:06X}  ", n, off, rgb, rgb24);
        const int swatchCol = static_cast<int>(text.size());
        text += "    "; // 4 cells for the colour block
        if (!ok)
            text += " ??";
        ArLine l(text);
        l.Span(0, 7, ArColor::Register, off);
        l.Span(8, 7, ArColor::Address);
        if (ok)
            l.Span(swatchCol, 4, ArColor::Swatch, rgb24);
        Emit(l);
    }
}

// ---------------------------------------------------------------------------
// STACK / CALL — stack introspection
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdStack(const std::string& args)
{
    if (!RequireRunning())
        return;
    ArCpuState st;
    if (!m_host.getCpuState || !m_host.getCpuState(st)) {
        Error("Error: cannot read CPU state");
        return;
    }
    int count = 16;
    auto tokens = Tokenize(args);
    if (!tokens.empty()) {
        if (auto v = ParseNumber(tokens[0]); v && *v > 0 && *v <= 256)
            count = static_cast<int>(*v);
    }
    uint32_t sp = st.a[7];
    Emit(fmt::format("Stack at A7=${:08X}", sp), ArColor::Highlight);
    EmitTableHead("Offset  Address    Value      Points to");
    for (int k = 0; k < count; ++k) {
        uint32_t a = sp + static_cast<uint32_t>(k) * 4;
        bool ok = false;
        uint32_t val = m_mem->ReadLong(a, &ok);
        if (!ok)
            break;
        // Name the region a value points into, so stack-resident pointers stand out.
        std::string note;
        if (const auto* r = m_mem->FindRegion(val))
            note = r->name;
        ArLine l(fmt::format("A7+{:03X}  ${:08X}  ${:08X}  {}", k * 4, a, val, note));
        l.Span(8, 9, ArColor::Address);
        l.Span(19, 9, ArColor::HexData);
        Emit(l);
    }
}

void ArCommandInterpreter::CmdCall(const std::string& args)
{
    if (!RequireRunning())
        return;
    if (!m_host.disassembleOne) {
        Error("Error: disassembler unavailable");
        return;
    }
    ArCpuState st;
    if (!m_host.getCpuState || !m_host.getCpuState(st)) {
        Error("Error: cannot read CPU state");
        return;
    }
    int scan = 1024;
    auto tokens = Tokenize(args);
    if (!tokens.empty()) {
        if (auto v = ParseNumber(tokens[0]); v && *v > 0 && *v <= 4096)
            scan = static_cast<int>(*v);
    }
    uint32_t sp = st.a[7];
    Emit(fmt::format("Call chain (heuristic) from A7=${:08X}", sp), ArColor::Highlight);
    EmitTableHead("Frame  Return     Call site  Instruction");
    constexpr int kFrameCap = 32;
    int frames = 0;
    for (int k = 0; k < scan && frames < kFrameCap; ++k) {
        uint32_t ret = m_mem->ReadLong(sp + static_cast<uint32_t>(k) * 4);
        if ((ret & 1) || !m_mem->FindRegion(ret))
            continue;
        // A BSR/JSR whose instruction ends exactly at `ret` is the call site.
        for (uint32_t d = 2; d <= 8; d += 2) {
            uint32_t site = ret - d;
            Disassembler::Line ins;
            if (!m_host.disassembleOne(site, ins) || ins.nextAddress != ret)
                continue;
            std::string m = ToUpper(Trim(ins.mnemonic));
            if (m.rfind("BSR", 0) != 0 && m.rfind("JSR", 0) != 0)
                continue;
            ArLine l(fmt::format("#{:<2}    ${:08X}  ${:08X}  {}", frames, ret, site,
                                 Trim(ins.mnemonic)));
            l.Span(7, 9, ArColor::Address);
            l.Span(18, 9, ArColor::Address);
            Emit(l);
            ++frames;
            break;
        }
    }
    if (frames == 0)
        Info("No probable return addresses found.");
}

// ---------------------------------------------------------------------------
// PROFILE / CALLSTACK — live source-level profiler snapshot from the debugger
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdProfile(const std::string& args)
{
    if (!m_host.getProfile) {
        Error("Error: profiler unavailable");
        return;
    }

    ProfileView view = ProfileView::Functions;
    int limit = 20;
    auto tokens = Tokenize(args);
    size_t idx = 0;
    if (idx < tokens.size() && !ParseNumber(tokens[idx])) {
        if (!ParseProfileView(tokens[idx], view)) {
            Error("Error: view must be FUNC | INSTR | FLAT | FLAME | CS");
            return;
        }
        ++idx;
    }
    if (idx < tokens.size()) {
        if (auto n = ParseNumber(tokens[idx]); n && *n > 0)
            limit = static_cast<int>(*n);
    }
    // The call-stack view lives in its own command; route the alias there for convenience.
    if (view == ProfileView::CallStack) {
        CmdCallStack("");
        return;
    }

    ProfileQuery q;
    q.view = view;
    q.limit = limit;
    ProfileSnapshot snap = m_host.getProfile(q);

    if (!snap.available) {
        Info("No profile data - enable CPU trace (Disassembly Trace/Profile) and run a frame.");
        return;
    }

    if (view == ProfileView::Functions) {
        if (!snap.haveSymbols) {
            Info("No debug symbols - load a program with a sibling .elf for named functions.");
            Info("Try PROFILE INSTR for per-instruction hot spots.");
            return;
        }
        const uint32_t tot = snap.resolvedCycles + snap.unresolvedCycles;
        int resPct = tot ? static_cast<int>(100.0 * snap.resolvedCycles / tot + 0.5) : 0;
        Emit(fmt::format("Function profile - {} functions (resolved {}% of {} CCK)",
                         snap.functions.size(), resPct, tot),
             ArColor::Highlight);
        EmitTableHead("  Cycles  %frm  Count    Avg  Function");
        for (const auto& f : snap.functions)
            Emit(fmt::format("{:>8}  {:4.1f}  {:>5}  {:5.1f}  {}", f.totalCycles,
                             f.pctFrame * 100.0f, f.count, f.AvgCycles(), f.name));
        if (snap.functions.empty())
            Info("(no resolved functions this frame)");
        return;
    }

    if (view == ProfileView::Instructions) {
        Emit(fmt::format("Instruction profile - top {} by cycles ({} CCK total)",
                         snap.instructions.size(), snap.totalCycles),
             ArColor::Highlight);
        EmitTableHead("  Cycles  Count    Avg  PC         Instruction");
        for (const auto& s : snap.instructions) {
            ArLine l(fmt::format("{:>8}  {:>5}  {:5.1f}  ${:08X}  {}", s.totalCycles, s.count,
                                 s.AvgCycles(), s.pc, s.mnemonic));
            l.Span(24, 9, ArColor::Address);
            Emit(l);
        }
        return;
    }

    // Flat / Flame — timeline bands.
    const char* label = (view == ProfileView::Flame) ? "Flame graph" : "Flat leaf profile";
    Emit(fmt::format("{} - {} bands (indent = call depth)", label, snap.frames.size()),
         ArColor::Highlight);
    EmitTableHead("Depth  L-start   L-end     Function");
    for (const auto& fr : snap.frames) {
        std::string indent(static_cast<size_t>(fr.depth) * 2, ' ');
        std::string name = fr.name.empty() ? "(unresolved)" : fr.name;
        Emit(fmt::format("{:>4}   {:>7}   {:>7}   {}{}", fr.depth, fr.startLRef, fr.endLRef, indent,
                         name));
    }
    if (snap.frames.empty())
        Info("(no bands - needs CPU trace + debug symbols)");
}

void ArCommandInterpreter::CmdCallStack(const std::string& args)
{
    if (!m_host.getProfile) {
        Error("Error: profiler unavailable");
        return;
    }

    // Two different stacks are on offer and they answer different questions, so the
    // command picks the useful one by default and lets either be named explicitly:
    //   LIVE  - unwind the halted CPU with DWARF call-frame info (no CPU trace needed)
    //   TRACE - the frame scrubber's reconstruction at the playhead
    std::string mode = ToUpper(Trim(args));
    ProfileQuery q;
    q.view = ProfileView::CallStack;
    ProfileSnapshot snap = m_host.getProfile(q);

    const bool wantTrace = (mode == "TRACE" || mode == "T");
    const bool wantLive = (mode == "LIVE" || mode == "L");
    if (!wantTrace && !wantLive && !mode.empty()) {
        Error("Usage: CALLSTACK [LIVE|TRACE]");
        return;
    }

    if (!wantTrace && snap.liveAvailable) {
        Emit("Call stack @ halted PC (live DWARF unwind)", ArColor::Highlight);
        if (snap.liveStack.empty()) {
            Info(fmt::format("({})", snap.liveStop));
            return;
        }
        EmitTableHead("  #  PC         Function / source");
        int depth = 0;
        for (const auto& f : snap.liveStack) {
            const std::string name = f.name.empty() ? "(unresolved)" : f.name;
            std::string line = fmt::format("{:>3}  ${:08X}  {}", depth++, f.pc, name);
            if (f.inlined)
                line += " (inlined)";
            if (!f.file.empty())
                line += fmt::format("  [{}:{}]", f.file, f.line);
            ArLine l(line);
            l.Span(5, 9, ArColor::Address);
            Emit(l);
        }
        Info(fmt::format("({})", snap.liveStop));
        return;
    }

    if (wantLive) {
        Info("Live unwind unavailable - pause the machine and load a program with a "
             "sibling .elf.");
        return;
    }

    if (!snap.available) {
        Info("No profile data - enable CPU trace (Disassembly Trace/Profile) and run a frame.");
        return;
    }
    Emit(fmt::format("Call stack @ L{:07X} (scrubber playhead)", snap.playheadLRef),
         ArColor::Highlight);
    if (snap.callStack.empty()) {
        Info(snap.haveSymbols ? "(nothing on the stack at the playhead - scrub the timeline)"
                              : "(no debug symbols - load a program with a sibling .elf)");
        return;
    }
    EmitTableHead("Depth  Address    Function");
    for (const auto& f : snap.callStack) {
        std::string name = f.name.empty() ? "(unresolved)" : f.name;
        ArLine l(fmt::format("{:>4}   ${:08X}  {}", f.depth, f.funcAddress, name));
        l.Span(7, 9, ArColor::Address);
        Emit(l);
    }
}

void ArCommandInterpreter::CmdBlitLog(const std::string& args)
{
    if (!m_host.getBlitLog) {
        Error("Error: blitter log unavailable");
        return;
    }

    // Optional arg: a decimal/hex row count, or ALL/* for every blit. Default caps the dump
    // so a busy frame doesn't flood the console; ALL shows the lot.
    int limit = 16;
    auto tokens = Tokenize(args);
    if (!tokens.empty()) {
        std::string a = ToUpper(tokens[0]);
        if (a == "ALL" || a == "*") {
            limit = 0;
        } else if (auto n = ParseNumber(tokens[0]); n) {
            limit = static_cast<int>(*n);
        } else {
            Error("Error: usage BLIT (<count> | ALL)");
            return;
        }
    }

    BlitLogSnapshot snap = m_host.getBlitLog(limit);
    if (!snap.available) {
        Info("No blitter data - start the emulator and run a frame.");
        return;
    }
    if (snap.totalBlits == 0) {
        Info("No blits this frame.");
        return;
    }

    Emit(fmt::format("Blitter log - {} blits (CPU {}, Copper {}) | Bus {:.1f}% ({}/71,364 CCK)",
                     snap.totalBlits, snap.cpuBlits, snap.copperBlits, snap.busPercent,
                     snap.totalDmaCycles),
         ArColor::Highlight);
    EmitTableHead("  #  L-Ref     Ln  Clk  Trig    Op          Size  Mnt    Cyc  Channels");

    int idx = 0;
    for (const auto& e : snap.blits) {
        ++idx;

        // Active channels with address (+ signed modulo when non-zero), matching the window.
        std::string chan;
        auto addChan = [&chan](const char* name, bool used, uint32_t ptr, int16_t mod) {
            if (!used)
                return;
            if (!chan.empty())
                chan += ' ';
            chan += fmt::format("{}:${:06X}", name, ptr);
            if (mod)
                chan += fmt::format("({:+d})", mod);
        };
        addChan("A", e.useA, e.aptr, e.amod);
        addChan("B", e.useB, e.bptr, e.bmod);
        addChan("C", e.useC, e.cptr, e.cmod);
        addChan("D", e.useD, e.dptr, e.dmod);

        std::string size = fmt::format("{}x{}", e.widthPx, e.rows);
        ArLine l(
            fmt::format("{:>3}  L{:07X}  {:>3}  {:>3}  {:<6}  {:<5}  {:>9}  ${:02X}  {:>5}  {}",
                        idx, e.startCycle, e.startLine, e.startClock,
                        e.fromCopper ? "Copper" : "CPU", e.op, size, e.minterm, e.dmaCycles, chan));
        // Colour the L-ref timestamp like other addressed listings.
        l.Span(5, 8, ArColor::Address);
        Emit(l);
    }

    if (snap.truncated)
        Info(fmt::format("... {} of {} blits shown - use BLIT ALL for the full log.",
                         snap.blits.size(), snap.totalBlits));
}

void ArCommandInterpreter::CmdRegLog(const std::string& args)
{
    if (!m_host.getRegLog) {
        Error("Error: register log unavailable");
        return;
    }

    // Optional arg: a decimal/hex row count, or ALL/* for every write. A frame easily runs to
    // thousands of register writes, so the default cap is deliberately small.
    int limit = 32;
    auto tokens = Tokenize(args);
    if (!tokens.empty()) {
        std::string a = ToUpper(tokens[0]);
        if (a == "ALL" || a == "*") {
            limit = 0;
        } else if (auto n = ParseNumber(tokens[0]); n) {
            limit = static_cast<int>(*n);
        } else {
            Error("Error: usage REGLOG (<count> | ALL)");
            return;
        }
    }

    RegLogSnapshot snap = m_host.getRegLog(limit);
    if (!snap.available) {
        Info("No register data - start the emulator and run a frame.");
        return;
    }
    if (!snap.coreSupported) {
        Error("Error: the loaded PuAE core predates the register-log patch - rebuild it.");
        return;
    }
    if (snap.totalWrites == 0) {
        Info("No register writes this frame.");
        return;
    }

    Emit(fmt::format("Register log - {} writes (CPU {}, Copper {}) | {} changed | {} registers",
                     snap.totalWrites, snap.cpuWrites, snap.copperWrites, snap.changedWrites,
                     snap.distinctRegisters),
         ArColor::Highlight);
    EmitTableHead("  #  L-Ref     Ln  Clk  Src     Register  Value  Was     PC");

    int idx = 0;
    for (const auto& e : snap.writes) {
        ++idx;
        // "first" for the frame's opening write, "same" when the write changed nothing —
        // the column that separates the write that mattered from the ones that didn't.
        std::string was =
            e.firstWrite ? "first"
                         : (e.changed ? fmt::format("${:04X}", e.prevValue) : std::string("same"));
        ArLine l(fmt::format("{:>3}  L{:07X}  {:>3}  {:>3}  {:<6}  {:<8}  ${:04X}  {:<6}  ${:06X}",
                             idx, e.lRef, e.line, e.clock, e.fromCopper ? "Copper" : "CPU", e.name,
                             e.value, was, e.pc));
        // Colour the L-ref timestamp like other addressed listings.
        l.Span(5, 8, ArColor::Address);
        Emit(l);
    }

    if (snap.truncated)
        Info(fmt::format("... {} of {} writes shown - use REGLOG ALL for the full log.",
                         snap.writes.size(), snap.totalWrites));
}

// ---------------------------------------------------------------------------
// AUDIO / AUDREC — health of the audio path, and the two debug capture taps

void ArCommandInterpreter::CmdPaulaState(const std::string& args)
{
    (void)args;
    if (!m_host.getPaulaState) {
        Error("Error: Paula state unavailable");
        return;
    }

    PaulaStateSnapshot s = m_host.getPaulaState();
    if (!s.available) {
        Info("No Paula data - start the emulator.");
        return;
    }

    static const char* kModels[4] = {"none", "A500", "A1200", "A500 fixed"};
    const char* model = kModels[s.filterModel >= 0 && s.filterModel < 4 ? s.filterModel : 0];
    Emit(fmt::format("Paula - audio DMA {}, low-pass {} ({}){}", s.masterDmaOn ? "ON" : "OFF",
                     s.ledFilterOn ? "ON" : "off", model, s.hostMuted ? " [HOST MUTED]" : ""),
         ArColor::Highlight);
    Emit(fmt::format("Channel mask ${:X} ({}{}{}{})", s.channelMask,
                     (s.channelMask & 1) ? "0" : "-", (s.channelMask & 2) ? "1" : "-",
                     (s.channelMask & 4) ? "2" : "-", (s.channelMask & 8) ? "3" : "-"));
    if (!s.traceAvailable)
        Info("Core has no Paula tap - peak/rms columns read 0 (rebuild PuAE).");

    Emit("Ch  AUDxLC   LEN   PER    Hz  Note  VOL  DMA  INT  Att   Peak    RMS",
         ArColor::Highlight);
    for (const auto& c : s.channels) {
        // A muted voice is still reported with its real registers; the '*' marks what you hear.
        Emit(fmt::format(
            "{}{}  ${:06X} {:5} {:5} {:5} {:>5} {:4} {:>4} {:>4} {:>3} {:6.3f} {:6.3f}",
            c.audible ? ' ' : '*', c.index, c.lc, c.len, c.per, static_cast<int>(c.hz), c.note,
            c.vol, c.dmaOn ? "on" : "off", c.intPending ? "req" : (c.intEnabled ? "ena" : "-"),
            std::string(c.attachVolume ? "V" : "") + (c.attachPeriod ? "P" : "") +
                (!c.attachVolume && !c.attachPeriod ? "-" : ""),
            c.peak, c.rms));
    }
    if (s.channelMask != 0x0F)
        Info("* = muted by the Audio window's channel mask.");
}

void ArCommandInterpreter::CmdAudioStats(const std::string& args)
{
    (void)args;
    if (!m_host.getAudioStats) {
        Error("Error: audio statistics unavailable");
        return;
    }

    AudioLogSnapshot s = m_host.getAudioStats();
    if (!s.available) {
        Info("No audio data - start the emulator.");
        return;
    }

    Emit(fmt::format("Audio path - core {:.0f} Hz -> device {} Hz{}{} via {}", s.coreSampleRate,
                     s.deviceSampleRate, s.resampling ? " (resampling)" : " (direct)",
                     s.muted ? " [MUTED]" : "", s.backend),
         ArColor::Highlight);
    if (!s.initialized)
        Info("Host audio device is not open - only the core tap can capture.");

    Emit(fmt::format("  Target FPS      {:.4f}", s.targetFps));

    // Producer: how evenly the core hands us audio. A guest that delivers uneven blocks starves
    // the ring even when the long-run average is correct.
    Emit(fmt::format("  Blocks          {} (frames/block min {} mean {:.1f} max {})", s.blockCount,
                     s.minBlockFrames, s.meanBlockFrames, s.maxBlockFrames));
    Emit(fmt::format("  Produced        {} frames", s.framesProduced));
    Emit(fmt::format("  Delivery gap    mean {:.1f} ms, max {:.1f} ms", s.produceGapMeanMs,
                     s.produceGapMaxMs));
    Emit(fmt::format("  Emulation cost  mean {:.1f} ms, max {:.1f} ms ({} over 40 ms)",
                     s.updateMeanMs, s.updateMaxMs, s.updateSlow));
    {
        ArLine l(fmt::format("  Stalls (>40ms)  {}", s.produceStalls));
        if (s.produceStalls > 0)
            l.Span(0, l.text.size(), ArColor::Error);
        Emit(l);
    }
    {
        // Emulated time we never ran. Unlike a ring underrun, no buffering can cover this.
        ArLine l(
            fmt::format("  Time discarded  {} frames in {} events (hitch {}, accum {}, cap {})",
                        s.timeDiscardedFrames, s.timeDiscardEvents, s.timeDiscardElapsedClamp,
                        s.timeDiscardAccumulatorClamp, s.timeDiscardFrameStopCap));
        if (s.timeDiscardedFrames > 0)
            l.Span(0, l.text.size(), ArColor::Error);
        Emit(l);
    }
    {
        ArLine l(fmt::format("  Dropped (over)  {} frames", s.framesDroppedOverrun));
        if (s.framesDroppedOverrun > 0)
            l.Span(0, l.text.size(), ArColor::Error);
        Emit(l);
    }

    // Consumer: the ring never having drained is the healthy case. ringLevelMin near zero means
    // the device callback ran dry, and every dry frame is filled by repeating the last sample.
    Emit(fmt::format("  Ring            {} / {} frames (min {} max {})", s.ringLevel, s.ringSize,
                     s.ringLevelMin, s.ringLevelMax));
    Emit(fmt::format("  Ring target     {} frames, mean {:.0f}, min {}; trimmed {} in {} events",
                     s.ringTarget, s.ringLevelMean, s.ringLevelMinPlaying, s.framesTrimmed,
                     s.trimEvents));
    Emit(fmt::format("  Pace            {:+.0f} ppm", s.pacePpm));
    {
        std::string hist;
        for (size_t i = 0; i < s.produceGapHist.size(); ++i) {
            const std::string label = i < s.produceGapEdgesMs.size()
                                          ? fmt::format("<{:.0f}", s.produceGapEdgesMs[i])
                                          : fmt::format(">={:.0f}", s.produceGapEdgesMs.back());
            hist += fmt::format(" {}:{}", label, s.produceGapHist[i]);
        }
        Emit("  Gaps (ms)      " + hist);
    }
    // How late the sound runs against the picture: everything queued ahead of a new block (the
    // ring plus the device's queue, or with push output the device's queue alone).
    Emit(fmt::format("  Latency         {:.1f} ms (device period {} buffer {} frames)", s.latencyMs,
                     s.devicePeriodFrames, s.deviceBufferFrames));
    Emit(fmt::format("  Callbacks       {} ({} underrun)", s.deviceCallbacks, s.underrunCallbacks));
    Emit(fmt::format("  Consumed        {} frames", s.framesConsumed));
    {
        ArLine l(fmt::format("  Held (underrun) {} frames", s.framesHeld));
        if (s.framesHeld > 0)
            l.Span(0, l.text.size(), ArColor::Error);
        Emit(l);
    }
    {
        ArLine l(fmt::format("  Dropped (resmp) {} frames", s.framesDroppedResampler));
        if (s.framesDroppedResampler > 0)
            l.Span(0, l.text.size(), ArColor::Error);
        Emit(l);
    }

    auto tapLine = [this](const char* name, const AudioTapStatus& t) {
        if (!t.recording) {
            Emit(fmt::format("  {:<6} tap     idle", name));
            return;
        }
        Emit(fmt::format("  {:<6} tap     REC {} Hz, {:.2f}s, {} frames{} -> {}", name,
                         t.sampleRate, t.seconds, t.framesWritten,
                         t.framesDroppedStaging ? fmt::format(" ({} LOST)", t.framesDroppedStaging)
                                                : std::string(),
                         t.path));
    };
    tapLine("Core", s.coreTap);
    tapLine("Device", s.deviceTap);

    if (s.framesHeld > 0 || s.framesDroppedOverrun > 0 || s.framesDroppedResampler > 0) {
        Info("Non-zero held/dropped counts mean the host audio path is corrupting the stream, "
             "independently of what the emulated program produced.");
    }
}

void ArCommandInterpreter::CmdScenario(const std::string& args)
{
    if (!m_host.scenario) {
        Error("Error: scenario recording unavailable");
        return;
    }

    auto tokens = Tokenize(args);
    ScenarioRequest req;

    if (tokens.empty()) {
        req.verb = ScenarioRequest::Verb::Status;
    } else {
        const std::string which = ToUpper(tokens[0]);
        if (which == "REC" || which == "RECORD") {
            if (tokens.size() < 2) {
                Error("Error: usage SCEN REC <file.pkscn> [COLD] [notes...]");
                return;
            }
            req.verb = ScenarioRequest::Verb::Record;
            req.path = tokens[1];
            size_t next = 2;
            if (tokens.size() > 2 && ToUpper(tokens[2]) == "COLD") {
                req.coldBoot = true;
                next = 3;
            }
            for (size_t i = next; i < tokens.size(); ++i)
                req.notes += (req.notes.empty() ? "" : " ") + tokens[i];
        } else if (which == "STOP") {
            req.verb = ScenarioRequest::Verb::Stop;
        } else if (which == "PLAY") {
            if (tokens.size() < 2) {
                Error("Error: usage SCEN PLAY <file.pkscn>");
                return;
            }
            req.verb = ScenarioRequest::Verb::Play;
            req.path = tokens[1];
        } else if (which == "STATUS") {
            req.verb = ScenarioRequest::Verb::Status;
        } else {
            Error("Error: usage SCEN [REC <file> [COLD] [notes] | STOP | PLAY <file> | STATUS]");
            return;
        }
    }

    const ScenarioResult result = m_host.scenario(req);
    if (result.ok)
        Info(result.message);
    else
        Error(result.message.empty() ? "Error: scenario command failed" : result.message);
}

void ArCommandInterpreter::CmdAudioRecord(const std::string& args)
{
    if (!m_host.audioRecord) {
        Error("Error: audio recording unavailable");
        return;
    }

    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Error("Error: usage AUDREC (CORE | DEVICE | BOTH) <file.wav> [seconds] | AUDREC STOP");
        return;
    }

    AudioRecordRequest req;
    const std::string which = ToUpper(tokens[0]);

    if (which == "STOP") {
        req.stop = true;
        AudioRecordResult r = m_host.audioRecord(req);
        if (r.ok)
            Info(r.message);
        else
            Error("Error: " + r.message);
        return;
    }

    if (which == "CORE") {
        req.tapCore = true;
    } else if (which == "DEVICE" || which == "DEV") {
        req.tapDevice = true;
    } else if (which == "BOTH") {
        req.tapCore = true;
        req.tapDevice = true;
    } else {
        Error("Error: usage AUDREC (CORE | DEVICE | BOTH) <file.wav> [seconds] | AUDREC STOP");
        return;
    }

    if (tokens.size() < 2) {
        Error("Error: AUDREC needs an output .wav path");
        return;
    }
    req.path = tokens[1];

    if (tokens.size() >= 3) {
        try {
            req.seconds = std::stod(tokens[2]);
        } catch (const std::exception&) {
            Error("Error: seconds must be a number");
            return;
        }
        if (req.seconds < 0.0) {
            Error("Error: seconds must not be negative");
            return;
        }
    }

    AudioRecordResult r = m_host.audioRecord(req);
    if (r.ok)
        Info(r.message);
    else
        Error("Error: " + r.message);
}

// ---------------------------------------------------------------------------
// DSKLOG / BDSK — the emulated program's own disk DMA traffic

// FB - FloppyBridge live physical floppy drive (Milestone 3).
//
//   LIVE                            list hardware types, serial ports and slot status
//   LIVE <df> <type> <port> [mode] [cable] [cache] [smart]   attach DFn,
//                                   e.g. LIVE 0 1 COM3 0 1
//   LIVE <df> OFF                   detach DFn back to a file-backed drive
//
// Omitted fields default to mode 0 (Fast), cache on, smart on - except type 4, the
// XCopy buffered driver, whose defaults are cache off and smart off (its measured
// best; see the window).
//
// Attaching only writes the core's changed_prefs; PuAE opens the hardware on the
// next emulated vertical blank, so the drive must be running for anything to
// happen. Failures to open surface in the core log, not here.
//
// Kept alongside the Live Floppy Drive window rather than replaced by it: this is the
// scriptable route, reachable from the MCP exec_console tool. Both go through
// EmulatorView::AttachBridgeDrive, so a slot bridged either way is recorded the same
// and picks up the turbo and save-state guards.
void ArCommandInterpreter::CmdFloppyBridge(const std::string& args)
{
    if (!m_host.fbAttach || !m_host.fbDriverList) {
        Error("Error: FloppyBridge unavailable (core older than PuAE 1.36.0?)");
        return;
    }

    const auto toks = Tokenize(args);

    if (toks.empty()) {
        // Empty only means unsupported once a core is actually loaded: with none
        // loaded the host answers from the bundled core's known driver list, so
        // this cannot fire just because the emulator has not been started yet.
        const auto drivers = m_host.fbDriverList();
        if (drivers.empty()) {
            Info("No FloppyBridge drivers - the loaded core reports no bridge support.");
            return;
        }
        EmitTableHead("Type  Hardware");
        for (size_t i = 0; i < drivers.size(); ++i)
            Emit(fmt::format("{:<5} {}", i, drivers[i]));

        const auto ports = m_host.fbComPorts ? m_host.fbComPorts() : std::vector<std::string>{};
        Emit("");
        if (ports.empty())
            Info("No serial ports found - is the Greaseweazle plugged in?");
        else {
            std::string list;
            for (const auto& p : ports)
                list += (list.empty() ? "" : ", ") + p;
            Emit("Ports: " + list, ArColor::Highlight);
        }

        Emit("");
        EmitTableHead("Drive  Backing     Motor  Cyl  Media  Prot  Core  Drift  Write");
        for (int d = 0; d < 4; ++d) {
            const ArHost::FloppyBridgeStatus st =
                m_host.fbStatus ? m_host.fbStatus(d) : ArHost::FloppyBridgeStatus{};
            const char* backing = !st.bridged ? "file" : st.pending ? "pending" : "live drive";
            // Write, for a live slot: off = the attach gate is shut and the machine sees a
            // protected drive whatever the tab says; wp = the gate is open and the disk's
            // own tab is closed; on = the machine can change the platter.
            const char* write = !st.bridged          ? "-"
                                : !st.allowWrites    ? "off"
                                : st.bridgeProtected ? "wp"
                                                     : "on";
            Emit(fmt::format("DF{}    {:<11} {:<6} {:<4} {:<6} {:<5} {:<5} {:<6} {}", d, backing,
                             st.motor ? "on" : "off",
                             st.cyl < 0 ? std::string("-") : std::to_string(st.cyl),
                             st.media ? "yes" : "no", st.hostProtected ? "on" : "off",
                             st.coreProtected ? "on" : "off", st.protectDrift, write));
        }
        Emit("");
        // Prot is the padlock; Core is drive_writeprotected() asked of the core, which
        // is the /WPRO bit the guest reads before every track write. On a locked slot
        // holding a disk they must agree - if Core says off, the emulated machine
        // thinks it may write and its writes are silently discarded. Drift counts the
        // frames they disagreed; anything but 0 is a core path clearing the flag.
        Info("Prot = requested, Core = what the emulated machine sees, Drift = frames they");
        Info("disagreed. An empty drive always reads Core=on.");
        Info("Write (live slots) = off: the attach gate is shut; wp: gate open, disk's tab");
        Info("closed; on: the emulated machine can change the physical disk.");
        Emit("");
        Info("LIVE <df> <type> <port> [mode] [cable] [cache] [smart] [write]   attach");
        Info("  [write] 1 lets the emulated machine WRITE to the physical disk (default 0).");
        Info("LIVE <df> OFF                                            detach");
        Info("Any number of slots can be live at once, one device per port. Attaching with the");
        Info("machine stopped records the slot as pending; it opens when the emulator starts, and");
        Info("stays assigned across stop/start until detached.");
        Info("mode:  0 Fast (default), 1 Compatible, 2 TurboAmigaDOS, 3 Stalling");
        Info("cable: 0 drive A / IBM PC (default), 1 drive B, 2-5 Shugart drive 0-3.");
        Info("       Wrong cable select fails with \"Failed to find track 0\" after ~0.8 s.");
        Info("cache: background read-ahead, 1 on / 0 off. Default on; off for type 4.");
        Info("smart: serve evenly-timed tracks 10x faster, 1 on / 0 off. Default on; off for type "
             "4.");
        Info("       Type 4 (XCopy buffered) defaults to Fast with both off - its measured-best");
        Info("       raw per-track configuration. Turn smart off if a protected disk stops "
             "loading.");
        return;
    }

    auto drv = ParseNumber(toks[0]);
    if (!drv || *drv > 3) {
        Error("Error: usage LIVE <df 0-3> (<type> <port> [mode] | OFF)");
        return;
    }
    const int drive = static_cast<int>(*drv);

    if (toks.size() >= 2 && (ToUpper(toks[1]) == "OFF" || ToUpper(toks[1]) == "DETACH")) {
        if (m_host.fbDetach)
            m_host.fbDetach(drive);
        Info(fmt::format("DF{}: detached - back to a file-backed drive, slot empty.", drive));
        return;
    }

    if (toks.size() < 3) {
        Error("Error: usage LIVE <df 0-3> <type> <port> [mode] [cable] [cache] [smart] "
              "[write]   (LIVE alone lists types)");
        return;
    }

    ArHost::FloppyBridgeAttach req;
    req.drive = drive;
    auto type = ParseNumber(toks[1]);
    if (!type) {
        Error("Error: <type> must be an index from the LIVE list");
        return;
    }
    req.driverIndex = static_cast<int>(*type);
    req.comPort = toks[2];
    // XCopy Standalone (buffered), driver 4: its measured-best configuration is Fast
    // with read-ahead and smart speed off, so
    // those are its defaults here, as in the Live Floppy Drive window. Explicit
    // [cache] [smart] fields below still win.
    if (req.driverIndex == 4) {
        req.bridgeMode = 0;
        req.autoCache = false;
        req.smartSpeed = false;
    }
    if (toks.size() >= 4) {
        auto mode = ParseNumber(toks[3]);
        if (!mode || *mode > 3) {
            Error("Error: mode must be 0-3");
            return;
        }
        req.bridgeMode = static_cast<int>(*mode);
    }
    if (toks.size() >= 5) {
        auto cable = ParseNumber(toks[4]);
        if (!cable || *cable > 5) {
            Error("Error: cable must be 0-5 (0 = drive A / IBM PC, 1 = drive B, 2-5 = Shugart)");
            return;
        }
        req.driveCable = static_cast<int>(*cable);
    }
    if (toks.size() >= 6) {
        auto cache = ParseNumber(toks[5]);
        if (!cache || *cache > 1) {
            Error("Error: cache must be 0 (off) or 1 (on)");
            return;
        }
        req.autoCache = (*cache != 0);
    }
    if (toks.size() >= 7) {
        auto smart = ParseNumber(toks[6]);
        if (!smart || *smart > 1) {
            Error("Error: smart must be 0 (off) or 1 (on)");
            return;
        }
        req.smartSpeed = (*smart != 0);
    }
    if (toks.size() >= 8) {
        auto write = ParseNumber(toks[7]);
        if (!write || *write > 1) {
            Error("Error: write must be 0 (the machine sees a protected drive) or 1 (it may "
                  "write to the physical disk)");
            return;
        }
        req.allowWrites = (*write != 0);
    }

    if (!m_host.fbAttach(req)) {
        Error(fmt::format("Error: DF{}: attach rejected (bad slot, type, port name too long, or "
                          "the port is already used by another live slot)",
                          drive));
        return;
    }
    const bool pending = m_host.fbStatus && m_host.fbStatus(drive).pending;
    Info(fmt::format("DF{}: attach {} - type {} on {}, mode {}, cable {}, cache {}, smart {}, "
                     "write {}. The hardware opens",
                     drive, pending ? "recorded" : "queued", req.driverIndex, req.comPort,
                     req.bridgeMode, req.driveCable, req.autoCache ? "on" : "off",
                     req.smartSpeed ? "on" : "off", req.allowWrites ? "ALLOWED" : "off"));
    if (req.allowWrites)
        Info("Writes are ALLOWED: the emulated machine can change the physical disk.");
    if (pending)
        Info("when the emulator starts; check the core log if the drive stays silent.");
    else
        Info("on the next emulated vertical blank; check the core log if the drive stays silent.");
}

void ArCommandInterpreter::CmdDiskLog(const std::string& args)
{
    if (!m_host.getDiskLog) {
        Error("Error: disk log unavailable");
        return;
    }

    // Optional args: a row count (or ALL/*), and/or READ|WRITE to filter direction.
    DiskLogQuery q;
    q.limit = 16;
    // The console table reads top-to-bottom as time passing, so keep it chronological
    // even though the snapshot now defaults to newest-first for the MCP tool.
    q.newestFirst = false;
    for (const auto& tok : Tokenize(args)) {
        std::string a = ToUpper(tok);
        if (a == "ALL" || a == "*") {
            q.limit = 0;
        } else if (a == "READ" || a == "R") {
            q.direction = 2;
        } else if (a == "WRITE" || a == "W") {
            q.direction = 3;
        } else if (auto n = ParseNumber(tok); n) {
            q.limit = static_cast<int>(*n);
        } else {
            Error("Error: usage DSKLOG (<count> | ALL) (READ | WRITE)");
            return;
        }
    }

    DiskLogSnapshot snap = m_host.getDiskLog(q);
    if (!snap.available) {
        Info("No disk data - start the emulator (a core older than PuAE 1.19.0 has no disk log).");
        return;
    }
    if (snap.totalTransfers == 0) {
        Info("No disk transfers recorded.");
        return;
    }

    Emit(fmt::format("Disk log - {} transfers ({} read / {} write) | {} B read, {} B written",
                     snap.reads + snap.writes, snap.reads, snap.writes, snap.bytesRead,
                     snap.bytesWritten),
         ArColor::Highlight);
    if (snap.anyWrites)
        Emit(fmt::format("*** {} WRITE transfer(s) - this program wrote back to the disk ***",
                         snap.writes),
             ArColor::Highlight);
    if (snap.droppedEvents)
        Info("Warning: the core event ring overflowed - some transfers are missing.");
    EmitTableHead("  #  Seq  Frame  Dir    Trig    PC       Drv Cyl/Sd  DSKPT    Bytes  Sectors  "
                  "SHA-1");

    int idx = 0;
    for (const auto& e : snap.transfers) {
        ++idx;
        std::string sectors;
        if (e.sectorCount > 0)
            sectors = fmt::format("{}{}", e.sectorCount, e.standardTrack ? " ok" : "?");
        else
            sectors = "-";

        std::string flags;
        if (e.aborted)
            flags += " ABORTED";
        if (e.turbo)
            flags += " turbo";
        if (e.direction == "write" && !e.persistedToFile)
            flags += " (not persisted)";

        ArLine l(fmt::format(
            "{:>3}  {:>3}  {:>5}  {:<5}  {:<6}  {:06X}  {:>3} {:>3}/{:<3}  ${:06X}  {:>5}  "
            "{:<7}  {}{}",
            idx, e.seq, e.frame, e.direction, e.trigger, e.pc, e.drive, e.cyl, e.side, e.dskpt,
            e.bytes, sectors, e.sha1.substr(0, 12), flags));
        l.Span(31, 6, ArColor::Address); // the arming PC
        Emit(l);
    }

    if (snap.truncated)
        Info(fmt::format("... {} of {} matching transfers shown - use DSKLOG ALL for the "
                         "full log.",
                         snap.transfers.size(), snap.matched));
}

void ArCommandInterpreter::CmdDiskBreak(const std::string& args)
{
    if (!m_host.armDiskBreak) {
        Error("Error: BDSK unavailable (rebuild core)");
        return;
    }
    auto tokens = Tokenize(args);
    const char* usage = "Usage: BDSK <READ|WRITE|ANY> [track=N] [cyl=N] [side=N] [sector=N] "
                        "[addr=$lo-$hi]   |   BDSK OFF";

    // track/cyl/side/sector are counts, so they read as decimal ($ still forces hex).
    // Addresses keep the console's usual hex default. Without this split, "track=81"
    // would silently mean cylinder 64 and the breakpoint would simply never fire.
    auto parseCount = [](const std::string& v) -> std::optional<uint32_t> {
        if (!v.empty() && (v[0] == '$' || v[0] == '%' || v[0] == '!'))
            return ParseNumber(v);
        if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
            return ParseNumber(v);
        return ParseNumber("!" + v);
    };
    if (tokens.empty()) {
        Error(usage);
        return;
    }

    if (ToUpper(tokens[0]) == "OFF") {
        m_host.armDiskBreak(-1, -1, -1, -1, 0, 0);
        Info("Disk breakpoint disarmed.");
        StateChanged();
        return;
    }

    int access;
    const std::string acc = ToUpper(tokens[0]);
    if (acc == "READ" || acc == "R")
        access = 1;
    else if (acc == "WRITE" || acc == "W")
        access = 2;
    else if (acc == "ANY" || acc == "*")
        access = 0;
    else {
        Error(usage);
        return;
    }

    int cyl = -1, side = -1, sector = -1;
    uint32_t addrLo = 0, addrHi = 0;
    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string t = tokens[i];
        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            Error(usage);
            return;
        }
        const std::string key = ToUpper(t.substr(0, eq));
        const std::string val = t.substr(eq + 1);

        if (key == "ADDR") {
            const size_t dash = val.find('-', val[0] == '$' ? 1 : 0);
            if (dash == std::string::npos) {
                Error("Error: addr must be a range, e.g. addr=$40000-$50000");
                return;
            }
            auto lo = ParseNumber(val.substr(0, dash));
            auto hi = ParseNumber(val.substr(dash + 1));
            if (!lo || !hi || *hi < *lo) {
                Error("Error: addr must be a range, e.g. addr=$40000-$50000");
                return;
            }
            addrLo = static_cast<uint32_t>(*lo);
            addrHi = static_cast<uint32_t>(*hi);
            continue;
        }

        auto n = parseCount(val);
        if (!n) {
            Error(usage);
            return;
        }
        if (key == "TRACK") {
            if (*n > 159) {
                Error("Error: track must be 0-159");
                return;
            }
            cyl = static_cast<int>(*n / 2);
            side = static_cast<int>(*n % 2);
        } else if (key == "CYL") {
            cyl = static_cast<int>(*n);
        } else if (key == "SIDE") {
            side = static_cast<int>(*n);
        } else if (key == "SECTOR") {
            sector = static_cast<int>(*n);
        } else {
            Error(usage);
            return;
        }
    }

    if (!m_host.armDiskBreak(access, cyl, side, sector, addrLo, addrHi)) {
        Error("Error: BDSK unavailable (rebuild core)");
        return;
    }

    std::string what = (access == 1) ? "read" : (access == 2) ? "write" : "any";
    std::string where;
    if (cyl >= 0)
        where += fmt::format(" cyl {}", cyl);
    if (side >= 0)
        where += fmt::format(" side {}", side);
    if (sector >= 0)
        where += fmt::format(" sector {}", sector);
    if (addrHi)
        where += fmt::format(" addr ${:06X}-${:06X}", addrLo, addrHi);
    Info(fmt::format("Disk break armed on {} DMA{} - resume to run.", what, where));
    if (sector >= 0) {
        // Sector headers only exist once the data has arrived.
        Info("Note: a sector filter breaks at transfer completion, not at the arming "
             "instruction (the log still reports the arming PC).");
    }
    StateChanged();
}

// ---------------------------------------------------------------------------
// BPL / SPR / PF / DISPLAY — debug display masking
//
// Cosmetic only: the emulated machine keeps fetching and drawing everything, the core just
// filters what Denise composites. Because the filtering happens inside the renderer, anything
// read back from the display texture (the emulator window, GIF recording, the MCP `screenshot`
// tool) shows the masked image.
// ---------------------------------------------------------------------------

const ArCommandInterpreter::DisplayMaskGroup ArCommandInterpreter::kBitplaneGroup{
    "BPL", "Bitplanes", "BPL", 8, 1, 0xFF, &DisplayMasks::bitplanes};
const ArCommandInterpreter::DisplayMaskGroup ArCommandInterpreter::kSpriteGroup{
    "SPR", "Sprites", "SPR", 8, 0, 0xFF, &DisplayMasks::sprites};
const ArCommandInterpreter::DisplayMaskGroup ArCommandInterpreter::kPlayfieldGroup{
    "PF", "Playfields", "PF", 2, 1, 0x03, &DisplayMasks::playfields};

bool ArCommandInterpreter::GetDisplayMasksOrError(DisplayMasks& out)
{
    if (!m_host.getDisplayMasks || !m_host.setDisplayMasks) {
        Error("Error: display masking unavailable");
        return false;
    }
    out = m_host.getDisplayMasks();
    return true;
}

void ArCommandInterpreter::EmitDisplayMaskLine(const DisplayMaskGroup& g, uint8_t mask)
{
    std::string items;
    for (int i = 0; i < g.count; ++i) {
        if (!items.empty())
            items += ' ';
        items +=
            fmt::format("{}{}:{}", g.item, g.firstIndex + i, (mask & (1u << i)) ? "on" : "OFF");
    }
    ArLine l(fmt::format("{:<11} ${:02X}  {}", g.title, mask & g.allMask, items));
    l.Span(12, 3, ArColor::Address);
    Emit(l);
}

void ArCommandInterpreter::EmitEffectiveBitplaneNote(const DisplayMasks& masks)
{
    const uint8_t effective = EffectiveBitplaneMask(masks);
    if (effective == masks.bitplanes)
        return;
    Info(fmt::format("Effective bitplane mask ${:02X} - a hidden playfield also hides its planes "
                     "(PF1 = BPL1/3/5, PF2 = BPL2/4/6).",
                     effective));
}

void ArCommandInterpreter::CmdDisplayMask(const std::string& args, const DisplayMaskGroup& g)
{
    DisplayMasks masks;
    if (!GetDisplayMasksOrError(masks))
        return;

    const int lastIndex = g.firstIndex + g.count - 1;
    const std::string usage =
        fmt::format("Usage: {0} ALL | NONE | <{1}-{2}> (ON|OFF) | MASK <value>", g.cmd,
                    g.firstIndex, lastIndex);

    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        EmitDisplayMaskLine(g, masks.*(g.field));
        EmitEffectiveBitplaneNote(masks);
        Info(usage);
        return;
    }

    uint8_t& value = masks.*(g.field);
    const std::string first = ToUpper(tokens[0]);

    if (first == "ALL" || first == "ON" || first == "*") {
        value = g.allMask;
    } else if (first == "NONE" || first == "OFF") {
        value = 0;
    } else if (first == "MASK") {
        std::optional<uint32_t> n;
        if (tokens.size() >= 2)
            n = ParseNumber(tokens[1]);
        if (!n) {
            Error("Error: " + usage);
            return;
        }
        value = static_cast<uint8_t>(*n) & g.allMask;
    } else {
        auto n = ParseNumber(tokens[0]);
        if (!n || static_cast<int>(*n) < g.firstIndex || static_cast<int>(*n) > lastIndex) {
            Error("Error: " + usage);
            return;
        }
        const int bit = static_cast<int>(*n) - g.firstIndex;
        bool on = (value & (1u << bit)) == 0; // no ON/OFF given -> toggle
        if (tokens.size() >= 2) {
            const std::string state = ToUpper(tokens[1]);
            if (state == "ON" || state == "1")
                on = true;
            else if (state == "OFF" || state == "0")
                on = false;
            else {
                Error("Error: " + usage);
                return;
            }
        }
        if (on)
            value |= static_cast<uint8_t>(1u << bit);
        else
            value &= static_cast<uint8_t>(~(1u << bit));
    }

    m_host.setDisplayMasks(masks);
    EmitDisplayMaskLine(g, masks.*(g.field));
    EmitEffectiveBitplaneNote(masks);
}

void ArCommandInterpreter::CmdDisplay(const std::string& args)
{
    DisplayMasks masks;
    if (!GetDisplayMasksOrError(masks))
        return;

    auto tokens = Tokenize(args);
    if (!tokens.empty()) {
        const std::string first = ToUpper(tokens[0]);
        if (first == "ALL" || first == "ON" || first == "RESET") {
            masks = DisplayMasks{};
        } else if (first == "NONE" || first == "OFF") {
            masks = DisplayMasks{0x00, 0x00, 0x00};
        } else {
            Error("Error: usage DISPLAY (ALL | NONE)  - see BPL / SPR / PF for single toggles");
            return;
        }
        m_host.setDisplayMasks(masks);
    }

    // Screen mode first: it is the context the masks apply to, and reading it off DIWSTRT /
    // DDFSTRT by hand is the trap that made a 640x256 hires screen look like 320x256.
    Custom regs{};
    if (m_host.getCustomRegisters && m_host.getCustomRegisters(regs)) {
        const ScreenMode mode = DescribeScreenMode(regs);
        Info(fmt::format("Screen: {}{}", ScreenModeSummary(mode),
                         mode.bitplaneDma ? "" : "  (bitplane DMA off)"));
        if (mode.fetchWidth > 0 && mode.fetchWidth != mode.displayWidth) {
            Info(fmt::format("        DDF fetches {}px per row ({}px displayed)", mode.fetchWidth,
                             mode.displayWidth));
        }
    }

    Emit("Display masking - cosmetic only, the machine still renders everything",
         ArColor::Highlight);
    EmitDisplayMaskLine(kBitplaneGroup, masks.bitplanes);
    EmitDisplayMaskLine(kSpriteGroup, masks.sprites);
    EmitDisplayMaskLine(kPlayfieldGroup, masks.playfields);
    EmitEffectiveBitplaneNote(masks);
}

// COV <file>:<line> [frame]
//
// The retrospective counterpart to a breakpoint. On a -Ofast -flto build one source line often
// becomes several disjoint stretches of machine code, so a breakpoint -- being a single address
// -- can only report whether execution passed that one stretch. This asks the recorded trace
// about all of them at once.
void ArCommandInterpreter::CmdCoverage(const std::string& args)
{
    if (!m_host.lineCoverage) {
        Error("Error: line coverage unavailable (no debug info loaded)");
        return;
    }
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Info("Usage: COV <file>:<line> [frame]");
        Info("  e.g. COV crass_vector.c:830        every recorded frame");
        Info("       COV crass_vector.c:830 3085   just that frame");
        return;
    }

    const std::string& location = tokens[0];
    const size_t colon = location.rfind(':');
    if (colon == std::string::npos || colon + 1 >= location.size()) {
        Error("Error: COV <file>:<line> [frame]");
        return;
    }
    uint32_t line = 0;
    uint64_t frame = 0;
    try {
        line = static_cast<uint32_t>(std::stoul(location.substr(colon + 1)));
        if (tokens.size() > 1)
            frame = std::stoull(tokens[1]);
    } catch (...) {
        Error("Error: COV <file>:<line> [frame]");
        return;
    }

    // Coverage is read from the trace, so it needs the trace. Turning it on here rather than
    // making the user find two separate prerequisites first -- but say so, because it only
    // starts recording from now and the answer for a line that ran earlier will be empty.
    bool justEnabled = false;
    if (m_host.isCpuTrace && !m_host.isCpuTrace() && m_host.setCpuTrace) {
        m_host.setCpuTrace(true);
        justEnabled = true;
    }
    if (m_host.isTraceHistory && !m_host.isTraceHistory() && m_host.setTraceHistory) {
        m_host.setTraceHistory(true);
        justEnabled = true;
    }

    const nlohmann::json r = m_host.lineCoverage(location.substr(0, colon), line, frame);
    if (r.contains("error")) {
        Error("Error: " + r["error"].get<std::string>());
        return;
    }

    const std::string status = r.value("status", std::string{"none"});
    if (status == "file_unknown") {
        Error(
            fmt::format("Error: no file matching \"{}\" in this build", location.substr(0, colon)));
        return;
    }
    if (status == "none") {
        Info(fmt::format("Line {} generated no code, so it can never be reached.", line));
        Info("  Heavy optimisation folds many lines into their callers.");
        return;
    }
    if (status == "moved")
        Info(fmt::format("Line {} generated no code; showing line {} instead.", line,
                         r.value("resolvedLine", line)));

    Info(fmt::format("Line {}: {} hit(s) across {} of {} stretch(es)", r.value("requestedLine", 0u),
                     r.value("totalHits", 0ull), r.value("executedSites", 0ull),
                     r.value("siteCount", 0ull)));

    for (const auto& site : r.value("sites", nlohmann::json::array())) {
        std::string row = fmt::format(
            "  ${:<8} {:>8} hit(s){}", site.value("runtimeLo", site.value("lo", std::string{})),
            site.value("hits", 0ull), site.value("isStmt", false) ? "  (stmt)" : "");
        if (site.contains("inlineStack")) {
            row += "  [";
            bool first = true;
            for (const auto& fn : site["inlineStack"]) {
                if (!first)
                    row += " > ";
                row += fn.get<std::string>();
                first = false;
            }
            row += "]";
        }
        if (site.value("hits", 0ull) == 0)
            Info(row + "   <- never executed");
        else
            Info(row);
    }

    if (r.contains("hint"))
        Info("  note: " + r["hint"].get<std::string>());
    if (justEnabled)
        Info("  (CPU trace was off and has been enabled; run a frame and try COV again)");
}

void ArCommandInterpreter::CmdTrace(const std::string& args)
{
    if (!m_host.setCpuTrace || !m_host.isCpuTrace) {
        Error("Error: CPU trace unavailable");
        return;
    }
    auto tokens = Tokenize(args);
    if (tokens.empty()) {
        Info(fmt::format("CPU trace is {}", m_host.isCpuTrace() ? "ON" : "OFF"));
        Info("Usage: TRACE ON | OFF  (records every instruction; feeds PROFILE / CALLSTACK)");
        return;
    }
    const std::string a = ToUpper(tokens[0]);
    if (a == "ON" || a == "1" || a == "ENABLE") {
        m_host.setCpuTrace(true);
        Info("CPU trace enabled - run a frame, then PROFILE / CALLSTACK.");
    } else if (a == "OFF" || a == "0" || a == "DISABLE") {
        m_host.setCpuTrace(false);
        Info("CPU trace disabled.");
    } else {
        Error("Error: TRACE ON | OFF");
    }
}

// ---------------------------------------------------------------------------
// RC / RF / RM — stack + control, FPU and MMU registers for the configured CPU
// ---------------------------------------------------------------------------

void ArCommandInterpreter::CmdCpuExtRegs(char kind)
{
    if (!RequireRunning())
        return;
    CpuExtendedState ext;
    if (!m_host.getCpuExtState || !m_host.getCpuExtState(ext)) {
        Error("Error: extended registers unavailable (core 1.102.0 or later needed)");
        return;
    }

    if (kind == 'C') {
        Emit(ArLine(fmt::format("CPU {} CONTROL REGISTERS", ext.cpuModel), ArColor::Highlight));
        Emit("-------------------------");
        EmitExtRegisterPairs(ext, {CpuRegId::Usp, CpuRegId::Isp, CpuRegId::Msp});
        EmitExtRegisterPairs(ext, {CpuRegId::Vbr, CpuRegId::Sfc, CpuRegId::Dfc, CpuRegId::Cacr,
                                   CpuRegId::Caar, CpuRegId::Buscr, CpuRegId::Pcr});
        Emit(fmt::format("A7={} IPL={}{}", ActiveStackName(ext), ext.intmask,
                         ext.stopped ? " STOPPED" : ""));
    } else if (kind == 'F') {
        const auto& fpcr = GetCpuRegister(CpuRegId::Fpcr);
        if (!ext.fpuPresent) {
            Info(NotOnCpuText(fpcr, ext.cpuModel));
            return;
        }
        Emit(ArLine(fmt::format("FPU {} REGISTERS", FpuName(ext.fpuModel, ext.cpuModel)),
                    ArColor::Highlight));
        Emit("-------------------------");
        for (int i = 0; i < 8; ++i)
            Emit(fmt::format("FP{} {}  {}", i, FormatFpRaw(ext.fp[i]), FormatFpValue(ext.fp[i])));
        Emit(fmt::format("FPCR  {:08X}  {}", ext.fpcr, DecodeFpcr(ext.fpcr)));
        Emit(fmt::format("FPSR  {:08X}  {}", ext.fpsr, DecodeFpsr(ext.fpsr)));
        ArLine fpiar(fmt::format("FPIAR {:08X}", ext.fpiar));
        fpiar.Span(6, 8, ArColor::Address);
        Emit(fpiar);
    } else { // 'M' — MMU
        const bool any = ReadCpuRegister(ext, GetCpuRegister(CpuRegId::Tc)).has_value();
        if (!any) {
            Info(fmt::format("No MMU registers on a {} (68030, 68040, 68060)", ext.cpuModel));
            return;
        }
        std::string state = ext.mmuModel == 0 ? std::string("translation off")
                            : ext.mmuEc       ? CpuMmuName(ext) + ", no translation"
                                              : CpuMmuName(ext);
        Emit(ArLine(fmt::format("MMU REGISTERS ({})", state), ArColor::Highlight));
        Emit("-------------------------");
        EmitExtRegisterPairs(ext, {CpuRegId::Tc, CpuRegId::Crp, CpuRegId::Srp, CpuRegId::Urp});
        EmitExtRegisterPairs(ext, {CpuRegId::Tt0, CpuRegId::Tt1, CpuRegId::Itt0, CpuRegId::Itt1,
                                   CpuRegId::Dtt0, CpuRegId::Dtt1, CpuRegId::Mmusr});
    }
}

} // namespace adm::core
