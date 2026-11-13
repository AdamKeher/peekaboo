#include "DebugLines.h"
#include "BigEndian.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace adm::core {

namespace {

// --- ELF32 constants (mirrors DebugSymbols; only what we need to find sections) ---
constexpr uint8_t kElfMag[4] = {0x7f, 'E', 'L', 'F'};
constexpr int kEiClass = 4;
constexpr int kEiData = 5;
constexpr uint8_t kElfClass32 = 1;
constexpr uint8_t kElfData2Msb = 2;
constexpr size_t kShdrSize = 40;
constexpr size_t kShName = 0;
constexpr size_t kShOffset = 16;
constexpr size_t kShSize = 20;

// --- DWARF line-number opcodes ---
constexpr uint8_t kLnsCopy = 1;
constexpr uint8_t kLnsAdvancePc = 2;
constexpr uint8_t kLnsAdvanceLine = 3;
constexpr uint8_t kLnsSetFile = 4;
constexpr uint8_t kLnsSetColumn = 5;
constexpr uint8_t kLnsNegateStmt = 6;
constexpr uint8_t kLnsSetBasicBlock = 7;
constexpr uint8_t kLnsConstAddPc = 8;
constexpr uint8_t kLnsFixedAdvancePc = 9;
constexpr uint8_t kLnsSetPrologueEnd = 10;
constexpr uint8_t kLnsSetEpilogueBegin = 11;
constexpr uint8_t kLnsSetIsa = 12;

constexpr uint8_t kLneEndSequence = 1;
constexpr uint8_t kLneSetAddress = 2;
constexpr uint8_t kLneSetDiscriminator = 4;

// --- DWARF line content-type + form codes we handle ---
constexpr uint64_t kLnctPath = 1;
constexpr uint64_t kLnctDirectoryIndex = 2;

constexpr uint64_t kFormString = 0x08;
constexpr uint64_t kFormStrp = 0x0e;
constexpr uint64_t kFormUdata = 0x0f;
constexpr uint64_t kFormData1 = 0x0b;
constexpr uint64_t kFormData2 = 0x05;
constexpr uint64_t kFormData4 = 0x06;
constexpr uint64_t kFormData8 = 0x07;
constexpr uint64_t kFormData16 = 0x1e;
constexpr uint64_t kFormBlock = 0x09;
constexpr uint64_t kFormLineStrp = 0x1f;

// Big-endian cursor over a byte span with LEB128 and DWARF form helpers. All
// multi-byte scalars follow the ELF container endianness (m68k ELF = big-endian).
class Cursor
{
public:
    Cursor(std::span<const uint8_t> data, size_t pos, size_t end)
        : m_data(data), m_pos(pos), m_end(std::min(end, data.size())), m_ok(true)
    {
    }

    bool Ok() const { return m_ok; }
    size_t Pos() const { return m_pos; }
    // Clamped: several callers seek to file-derived offsets (header length, extended-opcode
    // length), which must not escape the cursor's window.
    void Seek(size_t p) { m_pos = std::min(p, m_end); }
    bool AtEnd() const { return m_pos >= m_end; }

    uint8_t U8()
    {
        if (m_pos + 1 > m_end) {
            m_ok = false;
            return 0;
        }
        return m_data[m_pos++];
    }
    uint16_t U16()
    {
        if (m_pos + 2 > m_end) {
            m_ok = false;
            return 0;
        }
        uint16_t v = be::U16(m_data.data() + m_pos);
        m_pos += 2;
        return v;
    }
    uint32_t U32()
    {
        if (m_pos + 4 > m_end) {
            m_ok = false;
            return 0;
        }
        uint32_t v = be::U32(m_data.data() + m_pos);
        m_pos += 4;
        return v;
    }
    uint64_t U64()
    {
        uint64_t hi = U32();
        uint64_t lo = U32();
        return (hi << 32) | lo;
    }

    uint64_t Uleb()
    {
        uint64_t result = 0;
        int shift = 0;
        while (m_pos < m_end) {
            uint8_t b = m_data[m_pos++];
            if (shift < 64) {
                result |= static_cast<uint64_t>(b & 0x7f) << shift;
            }
            shift += 7;
            if ((b & 0x80) == 0) {
                return result;
            }
        }
        m_ok = false;
        return result;
    }
    int64_t Sleb()
    {
        int64_t result = 0;
        int shift = 0;
        uint8_t b = 0;
        while (m_pos < m_end) {
            b = m_data[m_pos++];
            if (shift < 64) {
                result |= static_cast<int64_t>(b & 0x7f) << shift;
            }
            shift += 7;
            if ((b & 0x80) == 0) {
                break;
            }
        }
        if (shift < 64 && (b & 0x40)) {
            result |= -(static_cast<int64_t>(1) << shift); // sign-extend
        }
        return result;
    }

    // Read an inline NUL-terminated string.
    std::string CStr()
    {
        std::string s;
        while (m_pos < m_end && m_data[m_pos] != 0) {
            s += static_cast<char>(m_data[m_pos++]);
        }
        if (m_pos < m_end) {
            ++m_pos; // consume NUL
        } else {
            m_ok = false;
        }
        return s;
    }

    void Skip(size_t n)
    {
        if (m_pos + n > m_end) {
            m_ok = false;
            m_pos = m_end;
        } else {
            m_pos += n;
        }
    }

private:
    std::span<const uint8_t> m_data;
    size_t m_pos;
    size_t m_end;
    bool m_ok;
};

// Fetch a NUL-terminated string from a string-table section at a byte offset.
std::string StrAt(std::span<const uint8_t> section, uint32_t off)
{
    std::string s;
    for (size_t i = off; i < section.size() && section[i] != 0; ++i) {
        s += static_cast<char>(section[i]);
    }
    return s;
}

// Read a directory/file-table entry field encoded with the given DWARF form.
// Path-bearing forms return their string; integer forms return their value; both
// are provided and the caller picks based on the content type.
struct FormValue
{
    std::string str;
    uint64_t num = 0;
};

FormValue ReadForm(Cursor& c, uint64_t form, std::span<const uint8_t> debugStr,
                   std::span<const uint8_t> debugLineStr)
{
    FormValue v;
    switch (form) {
        case kFormString:
            v.str = c.CStr();
            break;
        case kFormStrp:
            v.str = StrAt(debugStr, c.U32());
            break;
        case kFormLineStrp:
            v.str = StrAt(debugLineStr, c.U32());
            break;
        case kFormUdata:
            v.num = c.Uleb();
            break;
        case kFormData1:
            v.num = c.U8();
            break;
        case kFormData2:
            v.num = c.U16();
            break;
        case kFormData4:
            v.num = c.U32();
            break;
        case kFormData8:
            v.num = c.U64();
            break;
        case kFormData16:
            c.Skip(16); // MD5 checksum — not needed
            break;
        case kFormBlock: {
            uint64_t n = c.Uleb();
            c.Skip(static_cast<size_t>(n));
            break;
        }
        default:
            // Unknown form: we can't know its width, so bail out safely.
            c.Skip(0);
            break;
    }
    return v;
}

// Join a directory and a (possibly relative) file path for display/opening.
std::string JoinPath(const std::string& dir, const std::string& file)
{
    if (file.empty()) {
        return dir;
    }
    // Absolute already? (unix "/", or windows "X:\" / "X:/")
    if (file[0] == '/' || (file.size() > 1 && file[1] == ':')) {
        return file;
    }
    if (dir.empty()) {
        return file;
    }
    char sep =
        (dir.find('\\') != std::string::npos && dir.find('/') == std::string::npos) ? '\\' : '/';
    return dir + sep + file;
}

} // namespace

bool DebugLines::LoadFromElf(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        Clear();
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    return LoadFromElf(bytes);
}

bool DebugLines::LoadFromElf(std::span<const uint8_t> data)
{
    Clear();
    if (data.size() < 52 || std::memcmp(data.data(), kElfMag, 4) != 0) {
        return false;
    }
    if (data[kEiClass] != kElfClass32 || data[kEiData] != kElfData2Msb) {
        return false; // only 32-bit big-endian (m68k) ELFs
    }

    auto beU16 = [&](size_t o) -> uint16_t {
        return static_cast<uint16_t>((data[o] << 8) | data[o + 1]);
    };
    auto beU32 = [&](size_t o) -> uint32_t {
        return (static_cast<uint32_t>(data[o]) << 24) | (static_cast<uint32_t>(data[o + 1]) << 16) |
               (static_cast<uint32_t>(data[o + 2]) << 8) | static_cast<uint32_t>(data[o + 3]);
    };

    const uint32_t shoff = beU32(0x20);
    const uint16_t shentsize = beU16(0x2e);
    const uint16_t shnum = beU16(0x30);
    const uint16_t shstrndx = beU16(0x32);
    if (shentsize < kShdrSize || shnum == 0 || shstrndx >= shnum) {
        return false;
    }
    if (shoff == 0 || shoff > data.size() ||
        static_cast<uint64_t>(shnum) * shentsize > data.size() - shoff) {
        return false;
    }

    // Section-name string table.
    const size_t shstrSh = shoff + static_cast<size_t>(shstrndx) * shentsize;
    const size_t shstrOff = beU32(shstrSh + kShOffset);
    const size_t shstrSize = beU32(shstrSh + kShSize);
    if (shstrOff > data.size() || shstrSize > data.size() - shstrOff) {
        return false;
    }
    auto sectionName = [&](size_t sh) -> std::string {
        uint32_t nameOff = beU32(sh + kShName);
        if (nameOff >= shstrSize) {
            return {};
        }
        return StrAt(data.subspan(shstrOff, shstrSize), nameOff);
    };

    // Locate the sections we need by name.
    std::span<const uint8_t> lineSec, strSec, lineStrSec;
    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t sh = shoff + static_cast<size_t>(i) * shentsize;
        std::string name = sectionName(sh);
        const size_t off = beU32(sh + kShOffset);
        const size_t sz = beU32(sh + kShSize);
        if (off > data.size() || sz > data.size() - off) {
            continue;
        }
        if (name == ".debug_line") {
            lineSec = data.subspan(off, sz);
        } else if (name == ".debug_str") {
            strSec = data.subspan(off, sz);
        } else if (name == ".debug_line_str") {
            lineStrSec = data.subspan(off, sz);
        }
    }
    if (lineSec.empty()) {
        return false; // no line info
    }

    // Walk every compilation unit's line program.
    size_t off = 0;
    while (off < lineSec.size()) {
        bool ok = true;
        size_t next = ParseUnit(lineSec, off, strSec, lineStrSec, ok);
        if (!ok || next <= off) {
            break; // malformed — stop rather than loop
        }
        off = next;
    }

    // Stable, and tie-broken by sequence: rows sharing an address would otherwise land in an
    // arbitrary order, making the "next row in this sequence" lookup non-deterministic.
    std::stable_sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) {
        if (a.address != b.address)
            return a.address < b.address;
        return a.seq < b.seq;
    });
    return !m_rows.empty();
}

size_t DebugLines::ParseUnit(std::span<const uint8_t> data, size_t unitOff,
                             std::span<const uint8_t> debugStr,
                             std::span<const uint8_t> debugLineStr, bool& ok)
{
    Cursor c(data, unitOff, data.size());

    uint32_t unitLen = c.U32();
    if (unitLen == 0xFFFFFFFFu) {
        ok = false; // 64-bit DWARF not supported (toolchain emits 32-bit)
        return data.size();
    }
    const size_t unitEnd = c.Pos() + unitLen;
    if (unitEnd > data.size()) {
        ok = false;
        return data.size();
    }
    // Constrain the cursor to this unit.
    c = Cursor(data, c.Pos(), unitEnd);

    const uint16_t version = c.U16();
    if (version < 2 || version > 5) {
        return unitEnd; // unknown line-program version — skip rather than misparse
    }
    if (version >= 5) {
        c.U8(); // address_size
        c.U8(); // segment_selector_size
    }
    const uint32_t headerLen = c.U32(); // prologue length
    const size_t programStart = c.Pos() + headerLen;

    const uint8_t minInstLen = c.U8();
    if (version >= 4) {
        c.U8(); // maximum_operations_per_instruction (VLIW; m68k is always 1)
    }
    const uint8_t defaultIsStmt = c.U8();
    const int8_t lineBase = static_cast<int8_t>(c.U8());
    const uint8_t lineRange = c.U8();
    const uint8_t opcodeBase = c.U8();
    // These come straight from the file and feed divisions (adj / lineRange) and the special-
    // opcode decode below — a crafted unit with lineRange 0 is a division by zero, not a parse
    // error we can shrug off.
    if (lineRange == 0 || opcodeBase == 0) {
        return unitEnd;
    }
    // standard_opcode_lengths[opcode_base-1]
    std::vector<uint8_t> stdOpLens(opcodeBase > 0 ? opcodeBase - 1 : 0);
    for (auto& v : stdOpLens) {
        v = c.U8();
    }

    // --- directory and file tables ---
    //
    // v5 encodes both as form-described entry lists; v2-4 use plain NUL-terminated string
    // lists and index them from 1, with index 0 meaning "the compilation unit's own file /
    // directory" -- which lives in .debug_info, not here. A placeholder is pushed at index 0
    // for those versions so the same 0-based lookup works for both encodings.
    //
    // This matters for real builds: a project with hand-written .s files gets v2/v3 units
    // from the assembler alongside v5 units from the C compiler, in one .debug_line.
    std::vector<std::string> dirs;
    std::vector<std::string> files; // resolved full paths

    if (version < 5) {
        dirs.emplace_back(); // index 0 = DW_AT_comp_dir, not in this table
        while (c.Ok()) {
            std::string d = c.CStr();
            if (d.empty()) {
                break; // empty string terminates the list
            }
            dirs.push_back(std::move(d));
        }

        files.emplace_back(); // index 0 = the CU's primary source file, not in this table
        while (c.Ok()) {
            std::string name = c.CStr();
            if (name.empty()) {
                break;
            }
            const uint64_t dirIdx = c.Uleb();
            c.Uleb(); // mtime
            c.Uleb(); // length
            // Two lvalues, not `cond ? dirs[i] : std::string()` — that ternary's common type
            // is a prvalue, so the "found" branch silently copied the whole string.
            static const std::string kNoDir;
            const std::string& dir =
                dirIdx < dirs.size() ? dirs[static_cast<size_t>(dirIdx)] : kNoDir;
            files.push_back(JoinPath(dir, name));
        }
    } else {
        uint8_t fmtCount = c.U8();
        std::vector<std::pair<uint64_t, uint64_t>> fmt(fmtCount);
        for (auto& p : fmt) {
            p.first = c.Uleb();  // content type
            p.second = c.Uleb(); // form
        }
        uint64_t count = c.Uleb();
        dirs.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count && c.Ok(); ++i) {
            std::string path;
            for (auto& [ct, form] : fmt) {
                FormValue v = ReadForm(c, form, debugStr, debugLineStr);
                if (ct == kLnctPath) {
                    path = std::move(v.str);
                }
            }
            dirs.push_back(std::move(path));
        }

        uint8_t fileFmtCount = c.U8();
        std::vector<std::pair<uint64_t, uint64_t>> fileFmt(fileFmtCount);
        for (auto& p : fileFmt) {
            p.first = c.Uleb();
            p.second = c.Uleb();
        }
        uint64_t fileCount = c.Uleb();
        files.reserve(static_cast<size_t>(fileCount));
        for (uint64_t i = 0; i < fileCount && c.Ok(); ++i) {
            std::string name;
            uint64_t dirIdx = 0;
            for (auto& [ct, form] : fileFmt) {
                FormValue v = ReadForm(c, form, debugStr, debugLineStr);
                if (ct == kLnctPath) {
                    name = std::move(v.str);
                } else if (ct == kLnctDirectoryIndex) {
                    dirIdx = v.num;
                }
            }
            // Two lvalues, not `cond ? dirs[i] : std::string()` — that ternary's common type
            // is a prvalue, so the "found" branch silently copied the whole string.
            static const std::string kNoDir;
            const std::string& dir =
                dirIdx < dirs.size() ? dirs[static_cast<size_t>(dirIdx)] : kNoDir;
            files.push_back(JoinPath(dir, name));
        }
    }

    if (!c.Ok()) {
        ok = false;
        return unitEnd;
    }

    // Pre-intern this unit's files to global ids.
    std::vector<uint32_t> fileIds(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        fileIds[i] = InternFile(files[i]);
    }
    auto globalFile = [&](uint64_t local) -> uint32_t {
        return local < fileIds.size() ? fileIds[static_cast<size_t>(local)] : 0u;
    };
    // Keep this unit's table addressable by its own offset. DW_AT_call_file in .debug_info
    // is an index into the file table of the unit's line program -- which is this one, found
    // via the CU's DW_AT_stmt_list -- so a flattened global list cannot resolve it.
    m_unitFiles[unitOff] = fileIds;

    // --- run the line-number state machine ---
    c.Seek(programStart);
    // Registers (DWARF5 initial state).
    uint32_t address = 0;
    uint64_t file = 1;
    int64_t line = 1;
    bool endSeq = false;

    // The line register is signed and both advance forms below can decrement it — a plain
    // static_cast<uint32_t> of a negative value yields 4294967295, which downstream reads as a
    // real line number. That is not theoretical: an LTO build of the crass engine produced one,
    // and DebuggerDisassemblyView then looped from firstLine to 0xFFFFFFFF, wedging the whole UI
    // (the loop counter is uint32_t, so it wraps and never terminates). 0 is the established
    // "unknown line" value, so out-of-range becomes 0 and the row is simply not attributed.
    auto emitLine = [](int64_t v) -> uint32_t {
        return (v > 0 && v <= 0xFFFFFF) ? static_cast<uint32_t>(v) : 0u;
    };
    bool isStmt = defaultIsStmt != 0;
    // Rows from different compilation units interleave once flattened, so each sequence gets an
    // id and a row's extent ends at the next row carrying the same one.
    uint32_t seq = m_nextSeq++;
    auto resetRegs = [&]() {
        address = 0;
        file = 1;
        line = 1;
        endSeq = false;
        isStmt = defaultIsStmt != 0;
        seq = m_nextSeq++;
    };

    while (c.Pos() < unitEnd && c.Ok()) {
        uint8_t op = c.U8();
        if (op == 0) {
            // Extended opcode: length, then sub-opcode. `len` is an unbounded ULEB from the
            // file: unchecked, a huge value wraps `after` to a position BEHIND the cursor and
            // the loop never terminates. Clamp to this unit.
            uint64_t len = c.Uleb();
            if (len > unitEnd - c.Pos()) {
                return unitEnd; // malformed — abandon this unit
            }
            size_t after = c.Pos() + static_cast<size_t>(len);
            uint8_t sub = c.U8();
            switch (sub) {
                case kLneEndSequence:
                    endSeq = true;
                    m_rows.push_back(
                        {address, emitLine(line), globalFile(file), true, isStmt, seq});
                    resetRegs();
                    break;
                case kLneSetAddress:
                    address = c.U32(); // address_size == 4
                    break;
                case kLneSetDiscriminator:
                    c.Uleb();
                    break;
                default:
                    break;
            }
            c.Seek(after); // robust against unhandled extended opcodes
        } else if (op < opcodeBase) {
            switch (op) {
                case kLnsCopy:
                    m_rows.push_back(
                        {address, emitLine(line), globalFile(file), false, isStmt, seq});
                    break;
                case kLnsAdvancePc:
                    address += static_cast<uint32_t>(c.Uleb() * minInstLen);
                    break;
                case kLnsAdvanceLine:
                    line += c.Sleb();
                    break;
                case kLnsSetFile:
                    file = c.Uleb();
                    break;
                case kLnsSetColumn:
                    c.Uleb();
                    break;
                case kLnsNegateStmt:
                    isStmt = !isStmt;
                    break;
                case kLnsSetBasicBlock:
                case kLnsSetPrologueEnd:
                case kLnsSetEpilogueBegin:
                    break;
                case kLnsConstAddPc: {
                    uint8_t adj = static_cast<uint8_t>(255 - opcodeBase);
                    address += static_cast<uint32_t>((adj / lineRange) * minInstLen);
                    break;
                }
                case kLnsFixedAdvancePc:
                    address += c.U16();
                    break;
                case kLnsSetIsa:
                    c.Uleb();
                    break;
                default:
                    // Unknown standard opcode: skip its ULEB args per the header table.
                    if (op - 1 < static_cast<int>(stdOpLens.size())) {
                        for (uint8_t a = 0; a < stdOpLens[op - 1]; ++a) {
                            c.Uleb();
                        }
                    }
                    break;
            }
        } else {
            // Special opcode: advance address + line, emit a row.
            uint8_t adjusted = static_cast<uint8_t>(op - opcodeBase);
            address += static_cast<uint32_t>((adjusted / lineRange) * minInstLen);
            line += lineBase + (adjusted % lineRange);
            m_rows.push_back({address, emitLine(line), globalFile(file), false, isStmt, seq});
        }
    }

    ok = c.Ok();
    return unitEnd;
}

uint32_t DebugLines::RowEnd(size_t index) const
{
    // A row covers addresses up to the next row *of the same sequence*. Scanning forward is
    // cheap because rows of one sequence cluster together in address order.
    const Row& row = m_rows[index];
    for (size_t i = index + 1; i < m_rows.size(); ++i) {
        if (m_rows[i].seq == row.seq)
            return m_rows[i].address;
    }
    return row.address; // last row of its sequence: no extent to report
}

int DebugLines::PathScore(std::string_view dwarfPath, std::string_view queryPath)
{
    auto split = [](std::string_view path) {
        std::vector<std::string> parts;
        std::string current;
        for (char c : path) {
            if (c == '/' || c == '\\') {
                if (!current.empty())
                    parts.push_back(current);
                current.clear();
            } else {
                current.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)))); // Windows paths are case-blind
            }
        }
        if (!current.empty())
            parts.push_back(current);
        return parts;
    };

    const std::vector<std::string> a = split(dwarfPath);
    const std::vector<std::string> b = split(queryPath);
    if (a.empty() || b.empty())
        return -1;

    // Compare from the right: "../engine/foo.c" and "C:\proj\engine\foo.c" agree on the two
    // trailing components, which is a far stronger signal than either absolute path.
    int matched = 0;
    size_t i = a.size(), j = b.size();
    while (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
        ++matched;
        --i;
        --j;
    }
    return matched > 0 ? matched : -1;
}

std::optional<uint32_t> DebugLines::MatchFile(std::string_view path, bool* ambiguous) const
{
    if (ambiguous)
        *ambiguous = false;
    int bestScore = 0;
    std::optional<uint32_t> best;
    int bestCount = 0;
    for (uint32_t i = 0; i < m_files.size(); ++i) {
        const int score = PathScore(m_files[i], path);
        if (score <= 0)
            continue;
        if (score > bestScore) {
            bestScore = score;
            best = i;
            bestCount = 1;
        } else if (score == bestScore) {
            ++bestCount;
        }
    }
    // Several files matching equally well means we cannot tell which one the user meant. Say so
    // rather than picking one and silently binding a breakpoint in the wrong file.
    if (ambiguous && bestCount > 1)
        *ambiguous = true;
    return best;
}

std::vector<uint32_t> DebugLines::LinesWithCode(std::string_view file) const
{
    std::vector<uint32_t> out;
    bool ambiguous = false;
    const auto fileId = MatchFile(file, &ambiguous);
    if (!fileId)
        return out;
    for (const Row& row : m_rows) {
        if (row.fileId == *fileId && !row.endSequence && row.line != 0)
            out.push_back(row.line);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

LineLookup DebugLines::ResolveLineToAddresses(std::string_view file, uint32_t line, bool stmtOnly,
                                              uint32_t forwardSearch) const
{
    LineLookup result;
    result.requestedLine = line;
    result.resolvedLine = line;

    bool ambiguous = false;
    const auto fileId = MatchFile(file, &ambiguous);
    if (!fileId) {
        result.status = LineLookupStatus::FileUnknown;
        return result;
    }

    auto collect = [&](uint32_t wanted) {
        std::vector<LineSite> sites;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const Row& row = m_rows[i];
            if (row.endSequence || row.fileId != *fileId || row.line != wanted)
                continue;
            if (stmtOnly && !row.isStmt)
                continue;
            const uint32_t end = RowEnd(i);
            if (end <= row.address)
                continue; // zero-length: nothing to break on
            sites.push_back({row.address, end, row.line, row.fileId, row.isStmt});
        }
        std::sort(sites.begin(), sites.end(),
                  [](const LineSite& a, const LineSite& b) { return a.lo < b.lo; });
        return sites;
    };

    result.sites = collect(line);
    if (!result.sites.empty()) {
        result.status = LineLookupStatus::Exact;
        return result;
    }
    // Relaxing is_stmt is worth one more try: the line did generate code, just none of it
    // flagged as a recommended stopping point.
    if (stmtOnly) {
        LineLookup relaxed = ResolveLineToAddresses(file, line, /*stmtOnly=*/false, 0);
        if (relaxed.status == LineLookupStatus::Exact)
            return relaxed;
    }

    // Nothing at the requested line. Look forward a little, which is what the user almost
    // always wants, but report the move rather than performing it silently.
    for (uint32_t probe = line + 1; probe <= line + forwardSearch; ++probe) {
        auto sites = collect(probe);
        if (sites.empty() && stmtOnly) {
            LineLookup relaxed = ResolveLineToAddresses(file, probe, /*stmtOnly=*/false, 0);
            sites = relaxed.sites;
        }
        if (!sites.empty()) {
            result.status = LineLookupStatus::MovedForward;
            result.resolvedLine = probe;
            result.sites = std::move(sites);
            break;
        }
    }

    // Either way, offer nearby executable lines so the caller can say something useful.
    const std::vector<uint32_t> withCode = LinesWithCode(file);
    for (uint32_t candidate : withCode) {
        if (candidate + 40 >= line && candidate <= line + 40)
            result.nearbyLines.push_back(candidate);
    }
    if (result.status != LineLookupStatus::MovedForward) {
        // The file is present in this table, so "no code" is a real answer rather than a gap in
        // our parsing -- DebugLines skips units it cannot read, and those files never appear here.
        result.status =
            withCode.empty() ? LineLookupStatus::FileNotCovered : LineLookupStatus::NoCode;
    }
    return result;
}

std::optional<std::string> DebugLines::ResolveUnitFile(uint64_t stmtListOffset,
                                                       uint64_t fileIndex) const
{
    auto it = m_unitFiles.find(static_cast<size_t>(stmtListOffset));
    if (it == m_unitFiles.end() || fileIndex >= it->second.size()) {
        return std::nullopt;
    }
    const uint32_t id = it->second[static_cast<size_t>(fileIndex)];
    if (id >= m_files.size()) {
        return std::nullopt;
    }
    return m_files[id];
}

uint32_t DebugLines::InternFile(const std::string& path)
{
    // Hash-backed: the linear scan made loading O(files²) across compilation units, and every
    // unit re-interns its whole file table.
    auto it = m_fileIndex.find(path);
    if (it != m_fileIndex.end())
        return it->second;
    m_files.push_back(path);
    const uint32_t id = static_cast<uint32_t>(m_files.size() - 1);
    m_fileIndex.emplace(path, id);
    return id;
}

void DebugLines::Clear()
{
    m_nextSeq = 0;
    m_rows.clear();
    m_files.clear();
    m_fileIndex.clear();
    m_unitFiles.clear();
}

std::optional<SourceLine> DebugLines::ResolveLine(uint32_t linkAddr) const
{
    if (m_rows.empty()) {
        return std::nullopt;
    }
    // Last row whose start address is <= linkAddr.
    auto it = std::upper_bound(m_rows.begin(), m_rows.end(), linkAddr,
                               [](uint32_t a, const Row& r) { return a < r.address; });
    if (it == m_rows.begin()) {
        return std::nullopt;
    }
    --it;
    if (it->endSequence) {
        return std::nullopt; // past the end of a code range
    }
    SourceLine out;
    out.file = it->fileId < m_files.size() ? m_files[it->fileId] : std::string();
    out.line = it->line;
    return out;
}

} // namespace adm::core
