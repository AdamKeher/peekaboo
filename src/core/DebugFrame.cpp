#include "DebugFrame.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace adm::core {

namespace {

// --- ELF32 constants (mirrors DebugSymbols/DebugLines; only what we need to find sections) ---
constexpr uint8_t kElfMag[4] = {0x7f, 'E', 'L', 'F'};
constexpr int kEiClass = 4;
constexpr int kEiData = 5;
constexpr uint8_t kElfClass32 = 1;
constexpr uint8_t kElfData2Msb = 2;
constexpr size_t kShdrSize = 40;
constexpr size_t kShName = 0;
constexpr size_t kShOffset = 16;
constexpr size_t kShSize = 20;

// --- DWARF call-frame instructions (DWARF 5 section 6.4.2) ---
// The high two bits select the three "primary" opcodes; the rest are extended.
constexpr uint8_t kCfaAdvanceLoc = 0x1; // high bits 01, delta in the low 6
constexpr uint8_t kCfaOffset = 0x2;     // high bits 10, register in the low 6
constexpr uint8_t kCfaRestore = 0x3;    // high bits 11, register in the low 6

constexpr uint8_t kCfaNop = 0x00;
constexpr uint8_t kCfaSetLoc = 0x01;
constexpr uint8_t kCfaAdvanceLoc1 = 0x02;
constexpr uint8_t kCfaAdvanceLoc2 = 0x03;
constexpr uint8_t kCfaAdvanceLoc4 = 0x04;
constexpr uint8_t kCfaOffsetExtended = 0x05;
constexpr uint8_t kCfaRestoreExtended = 0x06;
constexpr uint8_t kCfaUndefined = 0x07;
constexpr uint8_t kCfaSameValue = 0x08;
constexpr uint8_t kCfaRegister = 0x09;
constexpr uint8_t kCfaRememberState = 0x0a;
constexpr uint8_t kCfaRestoreState = 0x0b;
constexpr uint8_t kCfaDefCfa = 0x0c;
constexpr uint8_t kCfaDefCfaRegister = 0x0d;
constexpr uint8_t kCfaDefCfaOffset = 0x0e;
constexpr uint8_t kCfaDefCfaExpression = 0x0f;
constexpr uint8_t kCfaExpression = 0x10;
constexpr uint8_t kCfaOffsetExtendedSf = 0x11;
constexpr uint8_t kCfaDefCfaSf = 0x12;
constexpr uint8_t kCfaDefCfaOffsetSf = 0x13;
constexpr uint8_t kCfaValOffset = 0x14;
constexpr uint8_t kCfaValOffsetSf = 0x15;
constexpr uint8_t kCfaValExpression = 0x16;
constexpr uint8_t kCfaGnuArgsSize = 0x2e;
constexpr uint8_t kCfaGnuNegativeOffsetExtended = 0x2f;

constexpr uint32_t kCieId32 = 0xffffffffu;

// Big-endian cursor with LEB128, over a byte span. m68k ELF is big-endian, and DWARF
// scalars follow the container's endianness.
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
        uint16_t v = static_cast<uint16_t>((m_data[m_pos] << 8) | m_data[m_pos + 1]);
        m_pos += 2;
        return v;
    }
    uint32_t U32()
    {
        if (m_pos + 4 > m_end) {
            m_ok = false;
            return 0;
        }
        uint32_t v = (static_cast<uint32_t>(m_data[m_pos]) << 24) |
                     (static_cast<uint32_t>(m_data[m_pos + 1]) << 16) |
                     (static_cast<uint32_t>(m_data[m_pos + 2]) << 8) |
                     static_cast<uint32_t>(m_data[m_pos + 3]);
        m_pos += 4;
        return v;
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
        bool complete = false;
        while (m_pos < m_end) {
            b = m_data[m_pos++];
            if (shift < 64) {
                result |= static_cast<int64_t>(b & 0x7f) << shift;
            }
            shift += 7;
            if ((b & 0x80) == 0) {
                complete = true;
                break;
            }
        }
        if (!complete) {
            m_ok = false;
            return result;
        }
        if (shift < 64 && (b & 0x40)) {
            result |= -(static_cast<int64_t>(1) << shift); // sign-extend
        }
        return result;
    }

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
        if (n > m_end - std::min(m_pos, m_end)) {
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

std::string StrAt(std::span<const uint8_t> section, uint32_t off)
{
    std::string s;
    for (size_t i = off; i < section.size() && section[i] != 0; ++i) {
        s += static_cast<char>(section[i]);
    }
    return s;
}

} // namespace

void DebugFrame::Clear()
{
    m_section.clear();
    m_cies.clear();
    m_fdes.clear();
}

bool DebugFrame::LoadFromElf(const std::string& path)
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

bool DebugFrame::LoadFromElf(std::span<const uint8_t> data)
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

    const size_t shstrSh = shoff + static_cast<size_t>(shstrndx) * shentsize;
    const size_t shstrOff = beU32(shstrSh + kShOffset);
    const size_t shstrSize = beU32(shstrSh + kShSize);
    if (shstrOff > data.size() || shstrSize > data.size() - shstrOff) {
        return false;
    }

    std::span<const uint8_t> frameSec;
    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t sh = shoff + static_cast<size_t>(i) * shentsize;
        const uint32_t nameOff = beU32(sh + kShName);
        if (nameOff >= shstrSize) {
            continue;
        }
        const std::string name = StrAt(data.subspan(shstrOff, shstrSize), nameOff);
        const size_t off = beU32(sh + kShOffset);
        const size_t sz = beU32(sh + kShSize);
        if (off > data.size() || sz > data.size() - off) {
            continue;
        }
        if (name == ".debug_frame") {
            frameSec = data.subspan(off, sz);
            break;
        }
    }
    if (frameSec.empty()) {
        return false; // no call-frame info
    }

    m_section.assign(frameSec.begin(), frameSec.end());
    std::span<const uint8_t> sec(m_section);

    // Two passes so a CIE referenced by an FDE that precedes it still resolves. The
    // toolchain emits CIE-before-FDE, but the format does not require it and a
    // mis-resolved CIE would silently produce wrong alignment factors.
    std::unordered_map<size_t, size_t> cieAtOffset;

    for (int pass = 0; pass < 2; ++pass) {
        size_t p = 0;
        while (p + 8 <= sec.size()) {
            Cursor c(sec, p, sec.size());
            const uint32_t length = c.U32();
            if (!c.Ok() || length == 0) {
                break; // terminator or truncation
            }
            if (length == 0xfffffff0u) {
                return false; // 64-bit DWARF; this toolchain does not emit it
            }
            const size_t entryEnd = c.Pos() + length;
            if (entryEnd > sec.size()) {
                break;
            }
            const uint32_t id = c.U32();
            if (!c.Ok()) {
                break;
            }

            if (id == kCieId32) {
                if (pass == 0) {
                    Cie cie;
                    const uint8_t version = c.U8();
                    const std::string augmentation = c.CStr();
                    // 'z'-augmented CIEs carry a length-prefixed augmentation blob whose
                    // contents change how the FDE is laid out. GCC does not emit them for
                    // m68k .debug_frame; skipping is safer than parsing them blind.
                    if (!c.Ok() || !augmentation.empty()) {
                        cie.usable = false;
                    } else {
                        cie.codeAlign = static_cast<uint32_t>(c.Uleb());
                        cie.dataAlign = static_cast<int32_t>(c.Sleb());
                        // DWARF 2 (version 1) encodes the return-address register as a single
                        // ubyte; version 3 and later use ULEB128. Both CIE versions appear in a
                        // single bartman ELF, so this is not a theoretical case.
                        cie.raRegister = (version == 1) ? static_cast<uint32_t>(c.U8())
                                                        : static_cast<uint32_t>(c.Uleb());
                        cie.instrBegin = c.Pos();
                        cie.instrEnd = entryEnd;
                        cie.usable = c.Ok() && cie.codeAlign != 0;
                    }
                    cieAtOffset[p] = m_cies.size();
                    m_cies.push_back(cie);
                }
            } else if (pass == 1) {
                // In .debug_frame the CIE pointer is a byte offset from the section start
                // (unlike .eh_frame, where it is relative to the pointer's own position).
                auto it = cieAtOffset.find(static_cast<size_t>(id));
                if (it != cieAtOffset.end() && m_cies[it->second].usable) {
                    Fde fde;
                    fde.cieIndex = it->second;
                    fde.loc = c.U32();
                    fde.range = c.U32();
                    fde.instrBegin = c.Pos();
                    fde.instrEnd = entryEnd;
                    if (c.Ok() && fde.range != 0) {
                        m_fdes.push_back(fde);
                    }
                }
            }
            p = entryEnd;
        }
    }

    std::sort(m_fdes.begin(), m_fdes.end(),
              [](const Fde& a, const Fde& b) { return a.loc < b.loc; });
    return !m_fdes.empty();
}

bool DebugFrame::Covers(uint32_t linkAddr) const
{
    auto it = std::upper_bound(m_fdes.begin(), m_fdes.end(), linkAddr,
                               [](uint32_t v, const Fde& f) { return v < f.loc; });
    if (it == m_fdes.begin()) {
        return false;
    }
    --it;
    return linkAddr - it->loc < it->range;
}

bool DebugFrame::RunProgram(size_t begin, size_t end, const Cie& cie, uint32_t target,
                            uint32_t& location, CfiRow& row, const CfiRow& initial) const
{
    std::span<const uint8_t> sec(m_section);
    Cursor c(sec, begin, end);
    std::vector<CfiRow> stateStack;

    auto setReg = [&row](uint64_t reg, CfiRegRule rule) {
        if (reg < kCfiRegCount) {
            row.reg[static_cast<size_t>(reg)] = rule;
        }
        // Registers outside the m68k range (the FPU columns) are simply not tracked;
        // nothing in an unwind needs them, and dropping them is not a parse failure.
    };

    while (!c.AtEnd()) {
        const uint8_t op = c.U8();
        if (!c.Ok()) {
            return false;
        }
        const uint8_t high = static_cast<uint8_t>(op >> 6);
        const uint8_t low = static_cast<uint8_t>(op & 0x3f);

        if (high == kCfaAdvanceLoc) {
            const uint32_t next = location + low * cie.codeAlign;
            if (next > target) {
                return true;
            }
            location = next;
            continue;
        }
        if (high == kCfaOffset) {
            const int64_t factored = static_cast<int64_t>(c.Uleb());
            setReg(low,
                   {CfiRegRuleKind::OffsetCfa, static_cast<int32_t>(factored * cie.dataAlign), 0});
            continue;
        }
        if (high == kCfaRestore) {
            if (low < kCfiRegCount) {
                row.reg[low] = initial.reg[low];
            }
            continue;
        }

        switch (op) {
            case kCfaNop:
                break;
            case kCfaSetLoc: {
                const uint32_t next = c.U32();
                if (next > target) {
                    return true;
                }
                location = next;
                break;
            }
            case kCfaAdvanceLoc1: {
                const uint32_t next = location + c.U8() * cie.codeAlign;
                if (next > target) {
                    return true;
                }
                location = next;
                break;
            }
            case kCfaAdvanceLoc2: {
                const uint32_t next = location + c.U16() * cie.codeAlign;
                if (next > target) {
                    return true;
                }
                location = next;
                break;
            }
            case kCfaAdvanceLoc4: {
                const uint32_t next = location + c.U32() * cie.codeAlign;
                if (next > target) {
                    return true;
                }
                location = next;
                break;
            }
            case kCfaOffsetExtended: {
                const uint64_t reg = c.Uleb();
                const int64_t factored = static_cast<int64_t>(c.Uleb());
                setReg(reg, {CfiRegRuleKind::OffsetCfa,
                             static_cast<int32_t>(factored * cie.dataAlign), 0});
                break;
            }
            case kCfaGnuNegativeOffsetExtended: {
                const uint64_t reg = c.Uleb();
                const int64_t factored = static_cast<int64_t>(c.Uleb());
                setReg(reg, {CfiRegRuleKind::OffsetCfa,
                             static_cast<int32_t>(-factored * cie.dataAlign), 0});
                break;
            }
            case kCfaOffsetExtendedSf: {
                const uint64_t reg = c.Uleb();
                const int64_t factored = c.Sleb();
                setReg(reg, {CfiRegRuleKind::OffsetCfa,
                             static_cast<int32_t>(factored * cie.dataAlign), 0});
                break;
            }
            case kCfaValOffset: {
                const uint64_t reg = c.Uleb();
                const int64_t factored = static_cast<int64_t>(c.Uleb());
                setReg(reg, {CfiRegRuleKind::ValOffsetCfa,
                             static_cast<int32_t>(factored * cie.dataAlign), 0});
                break;
            }
            case kCfaValOffsetSf: {
                const uint64_t reg = c.Uleb();
                const int64_t factored = c.Sleb();
                setReg(reg, {CfiRegRuleKind::ValOffsetCfa,
                             static_cast<int32_t>(factored * cie.dataAlign), 0});
                break;
            }
            case kCfaRestoreExtended: {
                const uint64_t reg = c.Uleb();
                if (reg < kCfiRegCount) {
                    row.reg[static_cast<size_t>(reg)] = initial.reg[static_cast<size_t>(reg)];
                }
                break;
            }
            case kCfaUndefined:
                setReg(c.Uleb(), {CfiRegRuleKind::Undefined, 0, 0});
                break;
            case kCfaSameValue:
                setReg(c.Uleb(), {CfiRegRuleKind::SameValue, 0, 0});
                break;
            case kCfaRegister: {
                const uint64_t reg = c.Uleb();
                const uint64_t src = c.Uleb();
                setReg(reg, {CfiRegRuleKind::Register, 0, static_cast<uint32_t>(src)});
                break;
            }
            case kCfaRememberState:
                // DWARF says this pushes the rules for every register *and* the CFA rule.
                stateStack.push_back(row);
                break;
            case kCfaRestoreState:
                if (stateStack.empty()) {
                    return false; // malformed: restore without a matching remember
                }
                row = stateStack.back();
                stateStack.pop_back();
                break;
            case kCfaDefCfa: {
                row.cfaReg = static_cast<uint32_t>(c.Uleb());
                row.cfaOffset = static_cast<int32_t>(c.Uleb()); // unfactored
                row.cfaValid = true;
                break;
            }
            case kCfaDefCfaSf: {
                row.cfaReg = static_cast<uint32_t>(c.Uleb());
                row.cfaOffset = static_cast<int32_t>(c.Sleb() * cie.dataAlign);
                row.cfaValid = true;
                break;
            }
            case kCfaDefCfaRegister:
                row.cfaReg = static_cast<uint32_t>(c.Uleb());
                break;
            case kCfaDefCfaOffset:
                row.cfaOffset = static_cast<int32_t>(c.Uleb()); // unfactored
                break;
            case kCfaDefCfaOffsetSf:
                row.cfaOffset = static_cast<int32_t>(c.Sleb() * cie.dataAlign);
                break;
            case kCfaGnuArgsSize:
                c.Uleb(); // stack-adjustment hint for exception handling; irrelevant here
                break;
            case kCfaDefCfaExpression:
            case kCfaExpression:
            case kCfaValExpression:
                // A DWARF expression would need a full stack-machine evaluator. GCC does not
                // emit these for m68k, and reporting "no CFI" is better than a wrong frame.
                return false;
            default:
                // An unknown opcode has an unknown operand width, so the rest of the program
                // cannot be decoded. Fail rather than resynchronise onto garbage.
                return false;
        }
        if (!c.Ok()) {
            return false;
        }
    }
    return true;
}

bool DebugFrame::ResolveRow(uint32_t linkAddr, CfiRow& out) const
{
    auto it = std::upper_bound(m_fdes.begin(), m_fdes.end(), linkAddr,
                               [](uint32_t v, const Fde& f) { return v < f.loc; });
    if (it == m_fdes.begin()) {
        return false;
    }
    --it;
    if (linkAddr - it->loc >= it->range) {
        return false;
    }
    const Fde& fde = *it;
    const Cie& cie = m_cies[fde.cieIndex];
    if (!cie.usable) {
        return false;
    }

    CfiRow row;
    row.raRegister = cie.raRegister;

    // The CIE's initial instructions establish the state at function entry, and also
    // define what DW_CFA_restore restores to.
    uint32_t location = fde.loc;
    if (!RunProgram(cie.instrBegin, cie.instrEnd, cie, 0xffffffffu, location, row, row)) {
        return false;
    }
    const CfiRow initial = row;

    location = fde.loc;
    if (!RunProgram(fde.instrBegin, fde.instrEnd, cie, linkAddr, location, row, initial)) {
        return false;
    }

    if (!row.cfaValid) {
        return false; // no CFA rule means nothing can be recovered from this frame
    }
    out = row;
    return true;
}

} // namespace adm::core
