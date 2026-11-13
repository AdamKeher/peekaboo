#include "DebugInline.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace adm::core {

namespace {

// --- ELF32 constants (locate .debug_* sections by name) ---
constexpr uint8_t kElfMag[4] = {0x7f, 'E', 'L', 'F'};
constexpr int kEiClass = 4;
constexpr int kEiData = 5;
constexpr uint8_t kElfClass32 = 1;
constexpr uint8_t kElfData2Msb = 2;
constexpr size_t kShdrSize = 40;
constexpr size_t kShName = 0;
constexpr size_t kShOffset = 16;
constexpr size_t kShSize = 20;

// --- DWARF tags / attributes / forms we care about ---
constexpr uint32_t kTagCompileUnit = 0x11;
constexpr uint32_t kTagSubprogram = 0x2e;
constexpr uint32_t kTagInlinedSubroutine = 0x1d;

constexpr uint32_t kAtName = 0x03;
constexpr uint32_t kAtLowPc = 0x11;
constexpr uint32_t kAtHighPc = 0x12;
constexpr uint32_t kAtAbstractOrigin = 0x31;
// Where the inlined body was called FROM. Without these an inlined frame and its caller both
// report the inlined body's line, so a backtrace shows the same file:line twice and never says
// where in the caller the call actually happened.
constexpr uint32_t kAtCallFile = 0x58;
constexpr uint32_t kAtCallLine = 0x59;
constexpr uint32_t kAtStmtList = 0x10;
constexpr uint32_t kAtSpecification = 0x47;
constexpr uint32_t kAtRanges = 0x55;

constexpr uint32_t kFormAddr = 0x01;
constexpr uint32_t kFormBlock2 = 0x03;
constexpr uint32_t kFormBlock4 = 0x04;
constexpr uint32_t kFormData2 = 0x05;
constexpr uint32_t kFormData4 = 0x06;
constexpr uint32_t kFormData8 = 0x07;
constexpr uint32_t kFormString = 0x08;
constexpr uint32_t kFormBlock = 0x09;
constexpr uint32_t kFormBlock1 = 0x0a;
constexpr uint32_t kFormData1 = 0x0b;
constexpr uint32_t kFormFlag = 0x0c;
constexpr uint32_t kFormSdata = 0x0d;
constexpr uint32_t kFormStrp = 0x0e;
constexpr uint32_t kFormUdata = 0x0f;
constexpr uint32_t kFormRefAddr = 0x10;
constexpr uint32_t kFormRef1 = 0x11;
constexpr uint32_t kFormRef2 = 0x12;
constexpr uint32_t kFormRef4 = 0x13;
constexpr uint32_t kFormRef8 = 0x14;
constexpr uint32_t kFormRefUdata = 0x15;
constexpr uint32_t kFormIndirect = 0x16;
constexpr uint32_t kFormSecOffset = 0x17;
constexpr uint32_t kFormExprloc = 0x18;
constexpr uint32_t kFormFlagPresent = 0x19;
constexpr uint32_t kFormData16 = 0x1e;
constexpr uint32_t kFormLineStrp = 0x1f;
constexpr uint32_t kFormImplicitConst = 0x21;
constexpr uint32_t kFormStrx = 0x1a;
constexpr uint32_t kFormAddrx = 0x1b;
constexpr uint32_t kFormLoclistx = 0x22;
constexpr uint32_t kFormRnglistx = 0x23;
constexpr uint32_t kFormStrx1 = 0x25;
constexpr uint32_t kFormStrx2 = 0x26;
constexpr uint32_t kFormStrx3 = 0x27;
constexpr uint32_t kFormStrx4 = 0x28;
constexpr uint32_t kFormAddrx1 = 0x29;
constexpr uint32_t kFormAddrx2 = 0x2a;
constexpr uint32_t kFormAddrx3 = 0x2b;
constexpr uint32_t kFormAddrx4 = 0x2c;

// --- DWARF 5 range-list entry kinds ---
constexpr uint8_t kRleEndOfList = 0x00;
constexpr uint8_t kRleBaseAddressx = 0x01;
constexpr uint8_t kRleStartxEndx = 0x02;
constexpr uint8_t kRleStartxLength = 0x03;
constexpr uint8_t kRleOffsetPair = 0x04;
constexpr uint8_t kRleBaseAddress = 0x05;
constexpr uint8_t kRleStartEnd = 0x06;
constexpr uint8_t kRleStartLength = 0x07;

// Big-endian cursor over a byte span with LEB128 (m68k ELF = big-endian).
class Cursor
{
public:
    Cursor(std::span<const uint8_t> data, size_t pos, size_t end)
        : m_data(data), m_pos(pos), m_end(std::min(end, data.size())), m_ok(true)
    {
    }
    bool Ok() const { return m_ok; }
    size_t Pos() const { return m_pos; }
    bool AtEnd() const { return m_pos >= m_end; }

    uint8_t U8()
    {
        if (m_pos + 1 > m_end)
            return Fail();
        return m_data[m_pos++];
    }
    uint16_t U16()
    {
        if (m_pos + 2 > m_end)
            return static_cast<uint16_t>(Fail());
        uint16_t v = static_cast<uint16_t>((m_data[m_pos] << 8) | m_data[m_pos + 1]);
        m_pos += 2;
        return v;
    }
    uint32_t U24()
    {
        uint32_t hi = U8();
        uint32_t mid = U8();
        uint32_t lo = U8();
        return (hi << 16) | (mid << 8) | lo;
    }
    uint32_t U32()
    {
        if (m_pos + 4 > m_end)
            return Fail();
        uint32_t v = (static_cast<uint32_t>(m_data[m_pos]) << 24) |
                     (static_cast<uint32_t>(m_data[m_pos + 1]) << 16) |
                     (static_cast<uint32_t>(m_data[m_pos + 2]) << 8) |
                     static_cast<uint32_t>(m_data[m_pos + 3]);
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
        uint64_t r = 0;
        int shift = 0;
        while (m_pos < m_end) {
            uint8_t b = m_data[m_pos++];
            if (shift < 64)
                r |= static_cast<uint64_t>(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) == 0)
                return r;
        }
        m_ok = false;
        return r;
    }
    int64_t Sleb()
    {
        int64_t r = 0;
        int shift = 0;
        uint8_t b = 0;
        while (m_pos < m_end) {
            b = m_data[m_pos++];
            if (shift < 64)
                r |= static_cast<int64_t>(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) == 0)
                break;
        }
        if (shift < 64 && (b & 0x40))
            r |= -(static_cast<int64_t>(1) << shift);
        return r;
    }
    std::string CStr()
    {
        std::string s;
        while (m_pos < m_end && m_data[m_pos] != 0)
            s += static_cast<char>(m_data[m_pos++]);
        if (m_pos < m_end)
            ++m_pos;
        else
            m_ok = false;
        return s;
    }
    void Skip(size_t n)
    {
        if (m_pos + n > m_end) {
            m_ok = false;
            m_pos = m_end;
        } else
            m_pos += n;
    }
    void Seek(size_t p) { m_pos = std::min(p, m_end); }

private:
    uint32_t Fail()
    {
        m_ok = false;
        return 0;
    }
    std::span<const uint8_t> m_data;
    size_t m_pos;
    size_t m_end;
    bool m_ok;
};

std::string StrAt(std::span<const uint8_t> section, uint64_t off)
{
    std::string s;
    for (size_t i = off; i < section.size() && section[i] != 0; ++i)
        s += static_cast<char>(section[i]);
    return s;
}

struct AttrSpec
{
    uint32_t attr;
    uint32_t form;
    int64_t implicitConst;
};
struct AbbrevDecl
{
    uint32_t tag = 0;
    bool hasChildren = false;
    std::vector<AttrSpec> attrs;
};
using AbbrevTable = std::unordered_map<uint64_t, AbbrevDecl>;

// Parse the abbreviation table at `off` in .debug_abbrev.
AbbrevTable ParseAbbrev(std::span<const uint8_t> abbrev, size_t off)
{
    AbbrevTable table;
    Cursor c(abbrev, off, abbrev.size());
    while (c.Ok()) {
        uint64_t code = c.Uleb();
        if (code == 0)
            break;
        AbbrevDecl d;
        d.tag = static_cast<uint32_t>(c.Uleb());
        d.hasChildren = c.U8() != 0;
        while (true) {
            uint32_t attr = static_cast<uint32_t>(c.Uleb());
            uint32_t form = static_cast<uint32_t>(c.Uleb());
            if (attr == 0 && form == 0)
                break;
            int64_t ic = (form == kFormImplicitConst) ? c.Sleb() : 0;
            d.attrs.push_back({attr, form, ic});
            if (!c.Ok())
                break;
        }
        table.emplace(code, std::move(d));
    }
    return table;
}

struct FormVal
{
    uint64_t num = 0;
    std::string str;
    bool haveStr = false;
};

FormVal ReadForm(Cursor& c, uint32_t form, uint32_t addrSize, int64_t implicitConst,
                 std::span<const uint8_t> debugStr, std::span<const uint8_t> debugLineStr)
{
    FormVal v;
    switch (form) {
        case kFormAddr:
            v.num = (addrSize == 8) ? c.U64() : c.U32();
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
            c.Skip(16);
            break;
        case kFormSdata:
            v.num = static_cast<uint64_t>(c.Sleb());
            break;
        case kFormUdata:
            v.num = c.Uleb();
            break;
        case kFormString:
            v.str = c.CStr();
            v.haveStr = true;
            break;
        case kFormStrp:
            v.num = c.U32();
            v.str = StrAt(debugStr, v.num);
            v.haveStr = true;
            break;
        case kFormLineStrp:
            v.num = c.U32();
            v.str = StrAt(debugLineStr, v.num);
            v.haveStr = true;
            break;
        case kFormRefAddr:
            v.num = c.U32();
            break;
        case kFormRef1:
            v.num = c.U8();
            break;
        case kFormRef2:
            v.num = c.U16();
            break;
        case kFormRef4:
            v.num = c.U32();
            break;
        case kFormRef8:
            v.num = c.U64();
            break;
        case kFormRefUdata:
            v.num = c.Uleb();
            break;
        case kFormSecOffset:
            v.num = c.U32();
            break;
        case kFormFlag:
            v.num = c.U8();
            break;
        case kFormFlagPresent:
            v.num = 1;
            break;
        case kFormExprloc: {
            uint64_t n = c.Uleb();
            c.Skip(static_cast<size_t>(n));
            break;
        }
        case kFormBlock1: {
            uint64_t n = c.U8();
            c.Skip(static_cast<size_t>(n));
            break;
        }
        case kFormBlock2: {
            uint64_t n = c.U16();
            c.Skip(static_cast<size_t>(n));
            break;
        }
        case kFormBlock4: {
            uint64_t n = c.U32();
            c.Skip(static_cast<size_t>(n));
            break;
        }
        case kFormBlock: {
            uint64_t n = c.Uleb();
            c.Skip(static_cast<size_t>(n));
            break;
        }
        case kFormImplicitConst:
            v.num = static_cast<uint64_t>(implicitConst);
            break;
        case kFormStrx:
        case kFormAddrx:
        case kFormRnglistx:
        case kFormLoclistx:
            v.num = c.Uleb();
            break;
        case kFormStrx1:
        case kFormAddrx1:
            v.num = c.U8();
            break;
        case kFormStrx2:
        case kFormAddrx2:
            v.num = c.U16();
            break;
        case kFormStrx3:
        case kFormAddrx3:
            v.num = c.U24();
            break;
        case kFormStrx4:
        case kFormAddrx4:
            v.num = c.U32();
            break;
        case kFormIndirect: {
            uint32_t f = static_cast<uint32_t>(c.Uleb());
            return ReadForm(c, f, addrSize, 0, debugStr, debugLineStr);
        }
        default:
            break; // unknown form: cannot advance — leave as-is (walk will bail on bounds)
    }
    return v;
}

// Resolve a range list at section offset `off` into concrete [lo,hi) intervals.
void ParseRnglist(std::span<const uint8_t> rnglists, uint64_t off, uint32_t cuBase,
                  std::vector<std::pair<uint32_t, uint32_t>>& out)
{
    Cursor c(rnglists, off, rnglists.size());
    uint32_t base = cuBase;
    for (int guard = 0; guard < 100000 && c.Ok(); ++guard) {
        uint8_t kind = c.U8();
        if (kind == kRleEndOfList)
            return;
        switch (kind) {
            case kRleBaseAddress:
                base = c.U32();
                break;
            case kRleOffsetPair: {
                uint64_t s = c.Uleb();
                uint64_t e = c.Uleb();
                if (e > s)
                    out.emplace_back(base + s, base + e);
                break;
            }
            case kRleStartEnd: {
                uint32_t s = c.U32();
                uint32_t e = c.U32();
                if (e > s)
                    out.emplace_back(s, e);
                break;
            }
            case kRleStartLength: {
                uint32_t s = c.U32();
                uint64_t len = c.Uleb();
                if (len > 0)
                    out.emplace_back(s, static_cast<uint32_t>(s + len));
                break;
            }
            // Indexed variants need .debug_addr, which this toolchain doesn't emit.
            case kRleBaseAddressx:
                c.Uleb();
                break;
            case kRleStartxEndx:
            case kRleStartxLength:
                c.Uleb();
                c.Uleb();
                break;
            default:
                return; // unknown entry — stop
        }
    }
}

} // namespace

bool DebugInline::LoadFromElf(const std::string& path)
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

bool DebugInline::LoadFromElf(std::span<const uint8_t> data)
{
    Clear();
    if (data.size() < 52 || std::memcmp(data.data(), kElfMag, 4) != 0)
        return false;
    if (data[kEiClass] != kElfClass32 || data[kEiData] != kElfData2Msb)
        return false;

    auto beU16 = [&](size_t o) { return static_cast<uint16_t>((data[o] << 8) | data[o + 1]); };
    auto beU32 = [&](size_t o) {
        return (static_cast<uint32_t>(data[o]) << 24) | (static_cast<uint32_t>(data[o + 1]) << 16) |
               (static_cast<uint32_t>(data[o + 2]) << 8) | static_cast<uint32_t>(data[o + 3]);
    };

    const uint32_t shoff = beU32(0x20);
    const uint16_t shentsize = beU16(0x2e);
    const uint16_t shnum = beU16(0x30);
    const uint16_t shstrndx = beU16(0x32);
    if (shentsize < kShdrSize || shnum == 0 || shstrndx >= shnum)
        return false;
    if (shoff == 0 || shoff > data.size() ||
        static_cast<uint64_t>(shnum) * shentsize > data.size() - shoff)
        return false;

    const size_t shstrSh = shoff + static_cast<size_t>(shstrndx) * shentsize;
    const size_t shstrOff = beU32(shstrSh + kShOffset);
    const size_t shstrSize = beU32(shstrSh + kShSize);
    if (shstrOff > data.size() || shstrSize > data.size() - shstrOff)
        return false;

    std::span<const uint8_t> info, abbrev, str, lineStr, rnglists;
    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t sh = shoff + static_cast<size_t>(i) * shentsize;
        uint32_t nameOff = beU32(sh + kShName);
        std::string name = (nameOff < shstrSize) ? StrAt(data.subspan(shstrOff, shstrSize), nameOff)
                                                 : std::string();
        const size_t off = beU32(sh + kShOffset);
        const size_t sz = beU32(sh + kShSize);
        if (off > data.size() || sz > data.size() - off)
            continue;
        auto sec = data.subspan(off, sz);
        if (name == ".debug_info")
            info = sec;
        else if (name == ".debug_abbrev")
            abbrev = sec;
        else if (name == ".debug_str")
            str = sec;
        else if (name == ".debug_line_str")
            lineStr = sec;
        else if (name == ".debug_rnglists")
            rnglists = sec;
    }
    if (info.empty() || abbrev.empty())
        return false;

    // A scope (subprogram / inlined subroutine) with a PC range, before name resolution.
    struct Scope
    {
        uint64_t dieOff;
        uint64_t origin; // abstract_origin / specification target, 0 if none
        int depth;
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        std::string inlineName; // DW_AT_name on the DIE itself, if any
        // Call site of this inlined body, in its caller. stmtList identifies the CU's line
        // program, which is what DW_AT_call_file indexes into.
        uint64_t callFile = 0;
        uint32_t callLine = 0;
        uint64_t stmtList = 0;
        bool haveCallSite = false;
    };
    std::vector<Scope> scopes;
    std::unordered_map<uint64_t, std::string> nameMap; // die offset -> DW_AT_name
    std::unordered_map<uint64_t, uint64_t> originMap;  // die offset -> origin die offset
    std::unordered_map<size_t, AbbrevTable> abbrevCache;

    // Walk every compilation unit.
    size_t cuStart = 0;
    while (cuStart + 4 <= info.size()) {
        Cursor c(info, cuStart, info.size());
        uint32_t unitLen = c.U32();
        if (unitLen == 0xFFFFFFFFu || unitLen == 0)
            break; // 64-bit DWARF unsupported / malformed
        const size_t cuEnd = c.Pos() + unitLen;
        if (cuEnd > info.size())
            break;
        const uint16_t version = c.U16();
        if (version != 5) {
            cuStart = cuEnd;
            continue;
        }
        c.U8(); // unit_type
        const uint32_t addrSize = c.U8();
        const uint32_t abbrevOff = c.U32();

        auto itA = abbrevCache.find(abbrevOff);
        if (itA == abbrevCache.end())
            itA = abbrevCache.emplace(abbrevOff, ParseAbbrev(abbrev, abbrevOff)).first;
        const AbbrevTable& table = itA->second;

        uint32_t cuLowPc = 0;
        uint64_t cuStmtList = 0;
        std::vector<bool> treeStack; // per open DIE: is it a scope (subprogram/inlined)?

        while (c.Pos() < cuEnd && c.Ok()) {
            const uint64_t dieOff = c.Pos();
            uint64_t code = c.Uleb();
            if (code == 0) {
                if (!treeStack.empty())
                    treeStack.pop_back();
                continue;
            }
            auto itD = table.find(code);
            if (itD == table.end())
                break; // corrupt — stop this CU
            const AbbrevDecl& decl = itD->second;

            std::string dieName;
            bool haveName = false;
            uint64_t origin = 0;
            bool haveOrigin = false;
            uint64_t lowpc = 0, highpc = 0, rangesOff = 0;
            bool haveLow = false, haveHigh = false, highIsOffset = true, haveRanges = false;
            uint64_t callFile = 0;
            uint32_t callLine = 0;
            bool haveCallFile = false, haveCallLine = false;

            for (const AttrSpec& a : decl.attrs) {
                FormVal fv = ReadForm(c, a.form, addrSize, a.implicitConst, str, lineStr);
                switch (a.attr) {
                    case kAtName:
                        if (fv.haveStr) {
                            dieName = std::move(fv.str);
                            haveName = true;
                        }
                        break;
                    case kAtAbstractOrigin:
                    case kAtSpecification:
                        origin = (a.form == kFormRefAddr) ? fv.num : (cuStart + fv.num);
                        haveOrigin = true;
                        break;
                    case kAtLowPc:
                        lowpc = fv.num;
                        haveLow = true;
                        break;
                    case kAtHighPc:
                        highpc = fv.num;
                        haveHigh = true;
                        highIsOffset = (a.form != kFormAddr);
                        break;
                    case kAtRanges:
                        rangesOff = fv.num;
                        haveRanges = true;
                        break;
                    case kAtCallFile:
                        callFile = fv.num;
                        haveCallFile = true;
                        break;
                    case kAtCallLine:
                        callLine = static_cast<uint32_t>(fv.num);
                        haveCallLine = true;
                        break;
                    case kAtStmtList:
                        if (decl.tag == kTagCompileUnit)
                            cuStmtList = fv.num;
                        break;
                    default:
                        break;
                }
            }
            if (!c.Ok())
                break;

            if (haveName)
                nameMap[dieOff] = dieName;
            if (haveOrigin)
                originMap[dieOff] = origin;
            if (decl.tag == kTagCompileUnit && haveLow)
                cuLowPc = static_cast<uint32_t>(lowpc);

            const bool isScope = (decl.tag == kTagSubprogram || decl.tag == kTagInlinedSubroutine);
            if (isScope) {
                int depth = 0;
                for (bool s : treeStack)
                    if (s)
                        ++depth;

                std::vector<std::pair<uint32_t, uint32_t>> ranges;
                if (haveLow && haveHigh) {
                    uint32_t hi = highIsOffset ? static_cast<uint32_t>(lowpc + highpc)
                                               : static_cast<uint32_t>(highpc);
                    if (hi > lowpc)
                        ranges.emplace_back(static_cast<uint32_t>(lowpc), hi);
                } else if (haveRanges && !rnglists.empty()) {
                    ParseRnglist(rnglists, rangesOff, cuLowPc, ranges);
                }
                if (!ranges.empty()) {
                    Scope sc;
                    sc.dieOff = dieOff;
                    sc.origin = haveOrigin ? origin : 0;
                    sc.depth = depth;
                    sc.ranges = std::move(ranges);
                    sc.inlineName = haveName ? dieName : std::string();
                    sc.callFile = callFile;
                    sc.callLine = callLine;
                    sc.stmtList = cuStmtList;
                    sc.haveCallSite = haveCallFile && haveCallLine;
                    scopes.push_back(std::move(sc));
                }
            }

            if (decl.hasChildren)
                treeStack.push_back(isScope);
        }

        cuStart = cuEnd;
    }

    // Resolve each scope's name, following abstract_origin / specification chains.
    auto resolveName = [&](uint64_t start) -> std::string {
        uint64_t cur = start;
        for (int hop = 0; hop < 8; ++hop) {
            auto n = nameMap.find(cur);
            if (n != nameMap.end())
                return n->second;
            auto o = originMap.find(cur);
            if (o == originMap.end())
                break;
            cur = o->second;
        }
        return {};
    };

    // Intern names and bucket intervals by inline depth.
    std::unordered_map<std::string, uint32_t> nameIds;
    auto intern = [&](const std::string& nm) -> uint32_t {
        auto it = nameIds.find(nm);
        if (it != nameIds.end())
            return it->second;
        uint32_t id = static_cast<uint32_t>(m_names.size());
        m_names.push_back(nm);
        nameIds.emplace(nm, id);
        return id;
    };

    for (const Scope& sc : scopes) {
        std::string nm = sc.inlineName;
        if (nm.empty() && sc.origin)
            nm = resolveName(sc.origin);
        if (nm.empty())
            nm = "?";
        const uint32_t nameId = intern(nm);
        if (static_cast<int>(m_byDepth.size()) <= sc.depth)
            m_byDepth.resize(sc.depth + 1);
        for (const auto& r : sc.ranges)
            m_byDepth[sc.depth].push_back({r.first, r.second, nameId, sc.stmtList, sc.callFile,
                                           sc.callLine, sc.haveCallSite});
    }
    for (auto& depth : m_byDepth)
        std::sort(depth.begin(), depth.end(),
                  [](const Interval& a, const Interval& b) { return a.lo < b.lo; });

    return !m_byDepth.empty();
}

void DebugInline::Clear()
{
    m_byDepth.clear();
    m_names.clear();
}

size_t DebugInline::ScopeCount() const
{
    size_t n = 0;
    for (const auto& d : m_byDepth)
        n += d.size();
    return n;
}

std::vector<DebugInline::InlineLevel> DebugInline::ResolveInlineLevels(uint32_t linkAddr) const
{
    std::vector<InlineLevel> out;
    for (const auto& depth : m_byDepth) {
        // Last interval with lo <= linkAddr.
        auto it = std::upper_bound(depth.begin(), depth.end(), linkAddr,
                                   [](uint32_t p, const Interval& iv) { return p < iv.lo; });
        if (it == depth.begin())
            break;
        --it;
        if (linkAddr >= it->lo && linkAddr < it->hi) {
            InlineLevel lv;
            lv.name = m_names[it->nameId];
            lv.stmtList = it->stmtList;
            lv.callFile = it->callFile;
            lv.callLine = it->callLine;
            lv.haveCallSite = it->haveCallSite;
            out.push_back(std::move(lv));
        } else {
            break; // gap at this depth — the inline stack ends here
        }
    }
    return out;
}

std::vector<std::string> DebugInline::ResolveInlineStack(uint32_t linkAddr) const
{
    std::vector<std::string> out;
    for (const InlineLevel& lv : ResolveInlineLevels(linkAddr))
        out.push_back(lv.name);
    return out;
}

} // namespace adm::core
