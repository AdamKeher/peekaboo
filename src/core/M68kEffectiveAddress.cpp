#include "M68kEffectiveAddress.h"

namespace adm::core {

namespace {

enum class Use {
    None,
    Read,
    Write,
    ReadWrite,
};

// Reads the instruction stream, tracking both the byte cursor and the address
// the next extension word lives at (PC-relative modes need the latter).
class Stream
{
public:
    Stream(const uint8_t* raw, size_t len, uint32_t pc) : m_raw(raw), m_len(len), m_pc(pc) {}

    bool Has(size_t words) const { return m_pos + words * 2 <= m_len; }
    uint32_t ExtAddr() const { return m_pc + static_cast<uint32_t>(m_pos); }

    uint16_t Word(size_t index) const
    {
        const size_t off = m_pos + index * 2;
        if (off + 2 > m_len)
            return 0;
        return static_cast<uint16_t>((m_raw[off] << 8) | m_raw[off + 1]);
    }

    uint32_t Long(size_t index) const
    {
        return (static_cast<uint32_t>(Word(index)) << 16) | Word(index + 1);
    }

    void Skip(size_t words) { m_pos += words * 2; }

private:
    const uint8_t* m_raw;
    size_t m_len;
    uint32_t m_pc;
    size_t m_pos = 2; // past the opcode word
};

struct Ea
{
    bool isMemory = false;
    bool valid = true; // false when the extension words were not captured
    uint32_t addr = 0;
};

// Index register contribution of a brief extension word (68000 form; the 68020
// scale field is ignored because a 68000 never sets it).
uint32_t BriefIndex(uint16_t ext, const M68kRegisters& regs)
{
    const int reg = (ext >> 12) & 7;
    const bool isAddr = (ext & 0x8000) != 0;
    const uint32_t full = isAddr ? regs.a[reg] : regs.d[reg];
    if (ext & 0x0800)
        return full; // long index
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(full & 0xFFFF)));
}

// Resolves one effective address, consuming its extension words from `s`.
// `opSize` is the operand width in bytes, needed by the (An)+ / -(An) modes.
Ea ResolveEa(int mode, int reg, int opSize, const M68kRegisters& regs, Stream& s)
{
    Ea ea;
    switch (mode) {
        case 0: // Dn
        case 1: // An
            return ea;
        case 2: // (An)
            ea.isMemory = true;
            ea.addr = regs.a[reg];
            return ea;
        case 3: // (An)+ — the trace captured An before the increment
            ea.isMemory = true;
            ea.addr = regs.a[reg];
            return ea;
        case 4: { // -(An) — the decrement happens before the access
            // The stack pointer always moves in even steps, even for a byte.
            const int step = (reg == 7 && opSize == 1) ? 2 : opSize;
            ea.isMemory = true;
            ea.addr = regs.a[reg] - static_cast<uint32_t>(step);
            return ea;
        }
        case 5: { // (d16,An)
            if (!s.Has(1)) {
                ea.valid = false;
                return ea;
            }
            const int32_t d16 = static_cast<int16_t>(s.Word(0));
            s.Skip(1);
            ea.isMemory = true;
            ea.addr = regs.a[reg] + static_cast<uint32_t>(d16);
            return ea;
        }
        case 6: { // (d8,An,Xn)
            if (!s.Has(1)) {
                ea.valid = false;
                return ea;
            }
            const uint16_t ext = s.Word(0);
            s.Skip(1);
            const int32_t d8 = static_cast<int8_t>(ext & 0xFF);
            ea.isMemory = true;
            ea.addr = regs.a[reg] + BriefIndex(ext, regs) + static_cast<uint32_t>(d8);
            return ea;
        }
        case 7:
            switch (reg) {
                case 0: { // (xxx).W
                    if (!s.Has(1)) {
                        ea.valid = false;
                        return ea;
                    }
                    ea.addr = static_cast<uint32_t>(
                        static_cast<int32_t>(static_cast<int16_t>(s.Word(0))));
                    s.Skip(1);
                    ea.isMemory = true;
                    return ea;
                }
                case 1: { // (xxx).L
                    if (!s.Has(2)) {
                        ea.valid = false;
                        return ea;
                    }
                    ea.addr = s.Long(0);
                    s.Skip(2);
                    ea.isMemory = true;
                    return ea;
                }
                case 2: { // (d16,PC)
                    if (!s.Has(1)) {
                        ea.valid = false;
                        return ea;
                    }
                    const uint32_t base = s.ExtAddr();
                    const int32_t d16 = static_cast<int16_t>(s.Word(0));
                    s.Skip(1);
                    ea.isMemory = true;
                    ea.addr = base + static_cast<uint32_t>(d16);
                    return ea;
                }
                case 3: { // (d8,PC,Xn)
                    if (!s.Has(1)) {
                        ea.valid = false;
                        return ea;
                    }
                    const uint32_t base = s.ExtAddr();
                    const uint16_t ext = s.Word(0);
                    s.Skip(1);
                    const int32_t d8 = static_cast<int8_t>(ext & 0xFF);
                    ea.isMemory = true;
                    ea.addr = base + BriefIndex(ext, regs) + static_cast<uint32_t>(d8);
                    return ea;
                }
                case 4: // #immediate
                    s.Skip(opSize == 4 ? 2 : 1);
                    return ea;
                default:
                    ea.valid = false;
                    return ea;
            }
        default:
            ea.valid = false;
            return ea;
    }
}

void Emit(std::vector<M68kAccess>& out, const Ea& ea, int size, Use use)
{
    if (!ea.isMemory || !ea.valid || use == Use::None)
        return;
    M68kAccess acc;
    acc.addr = ea.addr;
    acc.size = static_cast<uint8_t>(size);
    acc.read = use == Use::Read || use == Use::ReadWrite;
    acc.write = use == Use::Write || use == Use::ReadWrite;
    out.push_back(acc);
}

// Pushes onto the stack: the address is SP-4 because the trace captured A7
// before the push happened.
void EmitStackPush(std::vector<M68kAccess>& out, const M68kRegisters& regs, int size)
{
    M68kAccess acc;
    acc.addr = regs.a[7] - static_cast<uint32_t>(size);
    acc.size = static_cast<uint8_t>(size);
    acc.write = true;
    out.push_back(acc);
}

// 0 -> byte, 1 -> word, 2 -> long; 3 is not a data size.
int SizeFromField(int field)
{
    switch (field) {
        case 0:
            return 1;
        case 1:
            return 2;
        case 2:
            return 4;
        default:
            return 0;
    }
}

void DecodeImmediateGroup(uint16_t op, const M68kRegisters& regs, Stream& s,
                          std::vector<M68kAccess>& out)
{
    const int mode = (op >> 3) & 7;
    const int reg = op & 7;

    if ((op & 0xF138) == 0x0108)
        return; // MOVEP — register/port transfer, no useful single address

    if (op & 0x0100) {
        // Dynamic bit operation: BTST/BCHG/BCLR/BSET with a Dn bit number.
        const int kind = (op >> 6) & 3;
        const Use use = kind == 0 ? Use::Read : Use::ReadWrite;
        Ea ea = ResolveEa(mode, reg, 1, regs, s);
        Emit(out, ea, 1, use);
        return;
    }

    const int selector = (op >> 9) & 7;
    if (selector == 4) {
        // Static bit operation: the bit number is an immediate word.
        if (!s.Has(1))
            return;
        s.Skip(1);
        const int kind = (op >> 6) & 3;
        const Use use = kind == 0 ? Use::Read : Use::ReadWrite;
        Ea ea = ResolveEa(mode, reg, 1, regs, s);
        Emit(out, ea, 1, use);
        return;
    }

    const int size = SizeFromField((op >> 6) & 3);
    if (size == 0)
        return;
    if (mode == 7 && reg == 4)
        return; // ORI/ANDI/EORI to CCR/SR — no memory operand

    const size_t immWords = size == 4 ? 2 : 1;
    if (!s.Has(immWords))
        return;
    s.Skip(immWords);

    // 6 is CMPI, which only reads; the rest are read-modify-write.
    const Use use = selector == 6 ? Use::Read : Use::ReadWrite;
    Ea ea = ResolveEa(mode, reg, size, regs, s);
    Emit(out, ea, size, use);
}

void DecodeMove(uint16_t op, const M68kRegisters& regs, Stream& s, std::vector<M68kAccess>& out)
{
    int size = 0;
    switch ((op >> 12) & 3) {
        case 1:
            size = 1;
            break;
        case 3:
            size = 2;
            break;
        case 2:
            size = 4;
            break;
        default:
            return;
    }

    // Source extension words always precede the destination's.
    Ea src = ResolveEa((op >> 3) & 7, op & 7, size, regs, s);
    Ea dst = ResolveEa((op >> 6) & 7, (op >> 9) & 7, size, regs, s);
    Emit(out, src, size, Use::Read);
    Emit(out, dst, size, Use::Write);
}

void DecodeMovem(uint16_t op, const M68kRegisters& regs, Stream& s, std::vector<M68kAccess>& out)
{
    if (!s.Has(1))
        return;
    const uint16_t mask = s.Word(0);
    s.Skip(1);

    const bool toRegisters = (op & 0x0400) != 0;
    const int size = (op & 0x0040) ? 4 : 2;
    const int mode = (op >> 3) & 7;
    const int reg = op & 7;

    Ea base = ResolveEa(mode, reg, size, regs, s);
    if (!base.isMemory || !base.valid)
        return;

    int count = 0;
    for (int i = 0; i < 16; ++i)
        if (mask & (1u << i))
            ++count;
    if (count == 0)
        return;

    const Use use = toRegisters ? Use::Read : Use::Write;
    if (mode == 4) {
        // Predecrement: the block ends at the original An and grows downward, so
        // ResolveEa's single-operand guess is not the block start.
        const uint32_t start = regs.a[reg] - static_cast<uint32_t>(count * size);
        for (int i = 0; i < count; ++i) {
            Ea slot;
            slot.isMemory = true;
            slot.addr = start + static_cast<uint32_t>(i * size);
            Emit(out, slot, size, use);
        }
        return;
    }
    for (int i = 0; i < count; ++i) {
        Ea slot;
        slot.isMemory = true;
        slot.addr = base.addr + static_cast<uint32_t>(i * size);
        Emit(out, slot, size, use);
    }
}

void DecodeMisc(uint16_t op, const M68kRegisters& regs, Stream& s, std::vector<M68kAccess>& out)
{
    const int mode = (op >> 3) & 7;
    const int reg = op & 7;

    if ((op & 0xFFC0) == 0x4E80) { // JSR
        ResolveEa(mode, reg, 4, regs, s);
        EmitStackPush(out, regs, 4);
        return;
    }
    if ((op & 0xFFC0) == 0x4EC0) // JMP — control transfer only
        return;
    if ((op & 0xF1C0) == 0x41C0) { // LEA — computes an address, touches nothing
        ResolveEa(mode, reg, 4, regs, s);
        return;
    }
    if ((op & 0xFFC0) == 0x4840) { // PEA
        ResolveEa(mode, reg, 4, regs, s);
        EmitStackPush(out, regs, 4);
        return;
    }
    if ((op & 0xFB80) == 0x4880) { // MOVEM
        DecodeMovem(op, regs, s, out);
        return;
    }
    if ((op & 0xFFC0) == 0x4AC0) { // TAS
        Ea ea = ResolveEa(mode, reg, 1, regs, s);
        Emit(out, ea, 1, Use::ReadWrite);
        return;
    }

    const int size = SizeFromField((op >> 6) & 3);
    if (size == 0)
        return;

    Use use = Use::None;
    switch (op & 0xFF00) {
        case 0x4000: // NEGX
        case 0x4400: // NEG
        case 0x4600: // NOT
            use = Use::ReadWrite;
            break;
        case 0x4200: // CLR — writes without a meaningful read
            use = Use::Write;
            break;
        case 0x4A00: // TST
            use = Use::Read;
            break;
        default:
            return;
    }
    Ea ea = ResolveEa(mode, reg, size, regs, s);
    Emit(out, ea, size, use);
}

void DecodeQuickAndScc(uint16_t op, const M68kRegisters& regs, Stream& s,
                       std::vector<M68kAccess>& out)
{
    const int mode = (op >> 3) & 7;
    const int reg = op & 7;
    const int sizeField = (op >> 6) & 3;

    if (sizeField == 3) {
        if (mode == 1)
            return; // DBcc
        Ea ea = ResolveEa(mode, reg, 1, regs, s);
        Emit(out, ea, 1, Use::Write); // Scc
        return;
    }
    const int size = SizeFromField(sizeField);
    Ea ea = ResolveEa(mode, reg, size, regs, s); // ADDQ / SUBQ
    Emit(out, ea, size, Use::ReadWrite);
}

void DecodeAlu(uint16_t op, const M68kRegisters& regs, Stream& s, std::vector<M68kAccess>& out)
{
    const int family = (op >> 12) & 0xF;
    const int opmode = (op >> 6) & 7;
    const int mode = (op >> 3) & 7;
    const int reg = op & 7;

    if (opmode == 3 || opmode == 7) {
        // An-destination arithmetic (ADDA/SUBA/CMPA) and MULU/DIVU: the EA is a
        // source in every case.
        int size = 2;
        if (family == 0x9 || family == 0xB || family == 0xD)
            size = opmode == 3 ? 2 : 4;
        Ea ea = ResolveEa(mode, reg, size, regs, s);
        Emit(out, ea, size, Use::Read);
        return;
    }

    const int size = SizeFromField(opmode & 3);
    if (size == 0)
        return;

    if (opmode < 3) { // EA is the source, a data register the destination
        Ea ea = ResolveEa(mode, reg, size, regs, s);
        Emit(out, ea, size, Use::Read);
        return;
    }

    // opmode 4-6: memory destination — but the same encoding also carries the
    // BCD and extended-precision forms, which address memory differently.
    if (family == 0xB) {
        if (mode == 1) { // CMPM (Ay)+,(Ax)+
            M68kAccess src{regs.a[reg], static_cast<uint8_t>(size), true, false};
            M68kAccess dst{regs.a[(op >> 9) & 7], static_cast<uint8_t>(size), true, false};
            out.push_back(src);
            out.push_back(dst);
            return;
        }
        Ea ea = ResolveEa(mode, reg, size, regs, s); // EOR
        Emit(out, ea, size, Use::ReadWrite);
        return;
    }
    if (mode <= 1)
        return; // ABCD/SBCD/ADDX/SUBX — skipped rather than mis-reported

    Ea ea = ResolveEa(mode, reg, size, regs, s);
    Emit(out, ea, size, Use::ReadWrite);
}

void DecodeShift(uint16_t op, const M68kRegisters& regs, Stream& s, std::vector<M68kAccess>& out)
{
    if (((op >> 6) & 3) != 3)
        return; // register form — no memory operand
    Ea ea = ResolveEa((op >> 3) & 7, op & 7, 2, regs, s);
    Emit(out, ea, 2, Use::ReadWrite);
}

} // namespace

std::vector<M68kAccess> M68kEffectiveAddress::Decode(const uint8_t* raw, size_t rawLen, uint32_t pc,
                                                     const M68kRegisters& regs)
{
    std::vector<M68kAccess> out;
    if (!raw || rawLen < 2)
        return out;

    const uint16_t op = static_cast<uint16_t>((raw[0] << 8) | raw[1]);
    Stream s(raw, rawLen, pc);

    switch ((op >> 12) & 0xF) {
        case 0x0:
            DecodeImmediateGroup(op, regs, s, out);
            break;
        case 0x1:
        case 0x2:
        case 0x3:
            DecodeMove(op, regs, s, out);
            break;
        case 0x4:
            DecodeMisc(op, regs, s, out);
            break;
        case 0x5:
            DecodeQuickAndScc(op, regs, s, out);
            break;
        case 0x6:
            if (((op >> 8) & 0xF) == 1) // BSR pushes a return address
                EmitStackPush(out, regs, 4);
            break;
        case 0x8:
        case 0x9:
        case 0xB:
        case 0xC:
        case 0xD:
            DecodeAlu(op, regs, s, out);
            break;
        case 0xE:
            DecodeShift(op, regs, s, out);
            break;
        default:
            break; // MOVEQ, line-A, line-F
    }
    return out;
}

} // namespace adm::core
