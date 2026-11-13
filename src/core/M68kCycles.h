#pragma once

#include <cstdint>

// Best-effort Motorola 68000 textbook instruction timing, in CPU clock cycles
// (7.09 MHz). This is the *nominal* cost from the M68000 Programmer's Reference
// Manual — no bus/DMA contention, no wait states. Compare it against the trace's
// measured color-clock delta (1 CCK == 2 CPU cycles) to spot blitter/chip-bus stalls.
//
// Header-only + inline so it links into both the app and the unit-test binary.
// Decodes straight from the instruction words (not the annotated disassembly text),
// which disambiguates ADD vs ADDI vs ADDQ, MOVE vs MOVEA, etc.
//
// Returns the cycle count, or -1 when the instruction isn't modeled (caller shows "-").
// Data-dependent instructions (MULU/MULS/DIVU/DIVS) report the documented worst case.

namespace adm::core {

namespace m68k_detail {

// Effective-address calculation/fetch time added to an instruction's base cost.
// mode = bits[5:3], reg = bits[2:0]; isLong selects the long-word column.
inline int EaTime(int mode, int reg, bool isLong)
{
    switch (mode) {
        case 0: // Dn
        case 1: // An
            return 0;
        case 2: // (An)
            return isLong ? 8 : 4;
        case 3: // (An)+
            return isLong ? 8 : 4;
        case 4: // -(An)
            return isLong ? 10 : 6;
        case 5: // (d16,An)
            return isLong ? 12 : 8;
        case 6: // (d8,An,Xn)
            return isLong ? 14 : 10;
        case 7:
            switch (reg) {
                case 0:
                    return isLong ? 12 : 8; // (xxx).W
                case 1:
                    return isLong ? 16 : 12; // (xxx).L
                case 2:
                    return isLong ? 12 : 8; // (d16,PC)
                case 3:
                    return isLong ? 14 : 10; // (d8,PC,Xn)
                case 4:
                    return isLong ? 8 : 4; // #imm
                default:
                    return 0;
            }
        default:
            return 0;
    }
}

// True for register-direct (Dn/An) or immediate source — the long-word ALU ops add a
// fixed base for these instead of the memory-read EA time.
inline bool IsDirectOrImm(int mode, int reg)
{
    return mode == 0 || mode == 1 || (mode == 7 && reg == 4);
}

inline int Popcount16(uint16_t v)
{
    int c = 0;
    while (v) {
        v &= (uint16_t)(v - 1);
        ++c;
    }
    return c;
}

} // namespace m68k_detail

// w/n: instruction words (big-endian decoded) and their count.
// branchTaken: for conditional branches / DBcc, whether the path was actually taken
//   (so the column reflects the executed cost). dregs: D0..D7 at execution time,
//   used for dynamic (register-count) shifts; may be null.
inline int EstimateM68000Cycles(const uint16_t* w, int n, bool branchTaken, const uint32_t* dregs)
{
    using namespace m68k_detail;
    if (!w || n < 1)
        return -1;

    const uint16_t op = w[0];
    const int srcMode = (op >> 3) & 7;
    const int srcReg = op & 7;
    const int sizeField = (op >> 6) & 3; // 0=byte,1=word,2=long (where applicable)
    const bool isLong = (sizeField == 2);

    switch (op >> 12) {
        // ---- 0000: immediates + bit ops + MOVEP --------------------------------
        case 0x0: {
            const int sub = (op >> 9) & 7; // ORI/ANDI/SUBI/ADDI/.../EORI/CMPI selector
            // Static bit ops: 0000 1000 ssEA (bit number in extension word)
            if (((op >> 8) & 0xF) == 0x8) {
                const int btype = (op >> 6) & 3; // 0=BTST,1=BCHG,2=BCLR,3=BSET
                const bool toDn = (srcMode == 0);
                if (toDn) {
                    switch (btype) {
                        case 0:
                            return 10; // BTST #,Dn
                        case 2:
                            return 14; // BCLR #,Dn
                        default:
                            return 12; // BCHG/BSET #,Dn
                    }
                }
                return (btype == 0 ? 8 : 12) + EaTime(srcMode, srcReg, false); // byte EA
            }
            // Dynamic bit ops: 0000 rrr1 ttEA
            if (((op >> 8) & 1) == 1 && sub <= 7) {
                const int btype = (op >> 6) & 3;
                const bool toDn = (srcMode == 0);
                if (toDn) {
                    switch (btype) {
                        case 0:
                            return 6; // BTST Dn,Dn
                        case 2:
                            return 10; // BCLR Dn,Dn
                        default:
                            return 8; // BCHG/BSET Dn,Dn
                    }
                }
                return (btype == 0 ? 4 : 8) + EaTime(srcMode, srcReg, false);
            }
            // Immediate ALU: ORI/ANDI/SUBI/ADDI/EORI/CMPI #,<ea>
            const bool isCmpi = (sub == 6); // 0000 1100
            const bool toDn = (srcMode == 0);
            if (isCmpi) {
                if (toDn)
                    return isLong ? 14 : 8;
                return (isLong ? 12 : 8) + EaTime(srcMode, srcReg, isLong);
            }
            // ORI(0) ANDI(1) SUBI(2) ADDI(3) EORI(5)
            if (sub == 0 || sub == 1 || sub == 2 || sub == 3 || sub == 5) {
                if (toDn)
                    return isLong ? 16 : 8;
                return (isLong ? 20 : 12) + EaTime(srcMode, srcReg, isLong);
            }
            return -1;
        }

        // ---- 0001/0010/0011: MOVE.B / MOVE.L / MOVE.W --------------------------
        case 0x1:
        case 0x2:
        case 0x3: {
            const bool moveLong = ((op >> 12) == 0x2);
            const int dstReg = (op >> 9) & 7;
            const int dstMode = (op >> 6) & 7;
            // base 4 + source read EA + destination EA (Dn/An == 0).
            return 4 + EaTime(srcMode, srcReg, moveLong) + EaTime(dstMode, dstReg, moveLong);
        }

        // ---- 0100: miscellaneous ----------------------------------------------
        case 0x4: {
            // Full no-operand / control opcodes.
            if (op == 0x4E71)
                return 4; // NOP
            if (op == 0x4E75)
                return 16; // RTS
            if (op == 0x4E77)
                return 20; // RTR
            if (op == 0x4E73)
                return 20; // RTE
            if (op == 0x4E70)
                return 132; // RESET
            if ((op & 0xFFF8) == 0x4E50)
                return 16; // LINK
            if ((op & 0xFFF8) == 0x4E58)
                return 12; // UNLK
            if ((op & 0xFFF0) == 0x4E40)
                return 34; // TRAP
            if ((op & 0xFFF8) == 0x4840)
                return 4; // SWAP
            if ((op & 0xFFB8) == 0x4880)
                return 4;                  // EXT
            if ((op & 0xFF00) == 0x4A00) { // TST
                if (srcMode == 0)
                    return 4;
                return 4 + EaTime(srcMode, srcReg, isLong);
            }
            if ((op & 0xFF00) == 0x4200) { // CLR
                if (srcMode == 0)
                    return isLong ? 6 : 4;
                return (isLong ? 12 : 8) + EaTime(srcMode, srcReg, isLong);
            }
            if ((op & 0xFF00) == 0x4400 || (op & 0xFF00) == 0x4000 ||
                (op & 0xFF00) == 0x4600) { // NEG / NEGX / NOT
                if (srcMode == 0)
                    return isLong ? 6 : 4;
                return (isLong ? 12 : 8) + EaTime(srcMode, srcReg, isLong);
            }
            if ((op & 0xFFC0) == 0x4840) { // PEA
                switch (srcMode) {
                    case 2:
                        return 12;
                    case 5:
                        return 16;
                    case 6:
                        return 20;
                    case 7:
                        if (srcReg == 0)
                            return 16; // (xxx).W
                        if (srcReg == 1)
                            return 20; // (xxx).L
                        if (srcReg == 2)
                            return 16; // (d16,PC)
                        if (srcReg == 3)
                            return 20; // (d8,PC,Xn)
                        return -1;
                    default:
                        return -1;
                }
            }
            if ((op & 0xF1C0) == 0x41C0) { // LEA
                switch (srcMode) {
                    case 2:
                        return 4;
                    case 5:
                        return 8;
                    case 6:
                        return 12;
                    case 7:
                        if (srcReg == 0)
                            return 8; // (xxx).W
                        if (srcReg == 1)
                            return 12; // (xxx).L
                        if (srcReg == 2)
                            return 8; // (d16,PC)
                        if (srcReg == 3)
                            return 12; // (d8,PC,Xn)
                        return -1;
                    default:
                        return -1;
                }
            }
            if ((op & 0xFFC0) == 0x4EC0) { // JMP
                switch (srcMode) {
                    case 2:
                        return 8;
                    case 5:
                        return 10;
                    case 6:
                        return 14;
                    case 7:
                        if (srcReg == 0)
                            return 10;
                        if (srcReg == 1)
                            return 12;
                        if (srcReg == 2)
                            return 10;
                        if (srcReg == 3)
                            return 14;
                        return -1;
                    default:
                        return -1;
                }
            }
            if ((op & 0xFFC0) == 0x4E80) { // JSR
                switch (srcMode) {
                    case 2:
                        return 16;
                    case 5:
                        return 18;
                    case 6:
                        return 22;
                    case 7:
                        if (srcReg == 0)
                            return 18;
                        if (srcReg == 1)
                            return 20;
                        if (srcReg == 2)
                            return 18;
                        if (srcReg == 3)
                            return 22;
                        return -1;
                    default:
                        return -1;
                }
            }
            if ((op & 0xFB80) == 0x4880) { // MOVEM
                const bool memToReg = ((op >> 10) & 1) != 0;
                const int regs = (n >= 2) ? Popcount16(w[1]) : 0;
                const int per = isLong ? 8 : 4;
                int ea = EaTime(srcMode, srcReg, isLong);
                // MOVEM EA uses the control/predec/postinc forms; EaTime is close enough.
                return (memToReg ? 12 : 8) + per * regs + ea;
            }
            if ((op & 0xF1C0) == 0x4180) { // CHK
                return 10 + EaTime(srcMode, srcReg, false);
            }
            return -1;
        }

        // ---- 0101: ADDQ/SUBQ, Scc, DBcc ---------------------------------------
        case 0x5: {
            if (((op >> 6) & 3) == 3) {
                if (srcMode == 1) { // DBcc
                    if (branchTaken)
                        return 10; // condition false, loop continues
                    return 12;     // condition true (fall through)
                }
                // Scc
                if (srcMode == 0)
                    return 4; // Scc Dn (false; true = 6)
                return 8 + EaTime(srcMode, srcReg, false);
            }
            // ADDQ / SUBQ
            if (srcMode == 1) // ADDQ/SUBQ #,An
                return 8;
            if (srcMode == 0) // #,Dn
                return isLong ? 8 : 4;
            return (isLong ? 12 : 8) + EaTime(srcMode, srcReg, isLong);
        }

        // ---- 0110: Bcc / BRA / BSR --------------------------------------------
        case 0x6: {
            const int cond = (op >> 8) & 0xF;
            const bool byteDisp = (op & 0xFF) != 0; // 8-bit vs 16-bit displacement
            if (cond == 1)                          // BSR
                return 18;
            if (cond == 0) // BRA (always taken)
                return 10;
            // Bcc
            if (branchTaken)
                return 10;
            return byteDisp ? 8 : 12; // not taken: byte=8, word=12
        }

        // ---- 0111: MOVEQ -------------------------------------------------------
        case 0x7:
            return 4;

        // ---- 1000/1100: OR/AND (+ DIV/MUL/ABCD/SBCD/EXG) ----------------------
        case 0x8:   // OR / DIVU / DIVS / SBCD
        case 0xC: { // AND / MULU / MULS / ABCD / EXG
            const int opmode = (op >> 6) & 7;
            if (op >> 12 == 0x8) {
                if (opmode == 3)
                    return 140 + EaTime(srcMode, srcReg, false); // DIVU (max)
                if (opmode == 7)
                    return 158 + EaTime(srcMode, srcReg, false); // DIVS (max)
            } else {
                if (opmode == 3)
                    return 70 + EaTime(srcMode, srcReg, false); // MULU (max)
                if (opmode == 7)
                    return 70 + EaTime(srcMode, srcReg, false); // MULS (max)
                if ((op & 0x01F0) == 0x0140 || (op & 0x01F8) == 0x0148 || (op & 0x01F8) == 0x0188)
                    return 6; // EXG
            }
            if ((op & 0x01F0) == 0x0100)
                return 6; // ABCD/SBCD reg (6), mem = 18
            // generic AND/OR
            if (opmode <= 2) { // <ea>,Dn
                if (isLong)
                    return IsDirectOrImm(srcMode, srcReg) ? 8 : 6 + EaTime(srcMode, srcReg, true);
                return 4 + EaTime(srcMode, srcReg, false);
            }
            if (opmode >= 4 && opmode <= 6) { // Dn,<ea>
                return (((opmode & 3) == 2) ? 12 : 8) + EaTime(srcMode, srcReg, (opmode & 3) == 2);
            }
            return -1;
        }

        // ---- 1001/1101: SUB/ADD (+ SUBA/ADDA, SUBX/ADDX) ----------------------
        case 0x9:   // SUB / SUBA / SUBX
        case 0xD: { // ADD / ADDA / ADDX
            const int opmode = (op >> 6) & 7;
            if (opmode == 3 || opmode == 7) { // SUBA/ADDA (word/long)
                const bool aLong = (opmode == 7);
                if (aLong)
                    return IsDirectOrImm(srcMode, srcReg) ? 8 : 6 + EaTime(srcMode, srcReg, true);
                return 8 + EaTime(srcMode, srcReg, false);
            }
            // ADDX/SUBX: 1x01 rrr1 ss00 Mrrr — opmode 4/5/6 (bit8=1) with bits[5:4]==00.
            if ((op & 0x0130) == 0x0100) {
                const bool memForm = ((op >> 3) & 1) != 0; // -(An),-(An)
                if (!memForm)
                    return isLong ? 8 : 4; // Dn,Dn
                return isLong ? 30 : 18;   // -(An),-(An)
            }
            if (opmode <= 2) { // <ea>,Dn
                if (isLong)
                    return IsDirectOrImm(srcMode, srcReg) ? 8 : 6 + EaTime(srcMode, srcReg, true);
                return 4 + EaTime(srcMode, srcReg, false);
            }
            if (opmode >= 4 && opmode <= 6) { // Dn,<ea>
                return (((opmode & 3) == 2) ? 12 : 8) + EaTime(srcMode, srcReg, (opmode & 3) == 2);
            }
            return -1;
        }

        // ---- 1011: CMP / CMPA / EOR / CMPM ------------------------------------
        case 0xB: {
            const int opmode = (op >> 6) & 7;
            if (opmode == 3 || opmode == 7) { // CMPA word/long
                return 6 + EaTime(srcMode, srcReg, opmode == 7);
            }
            if (opmode <= 2) { // CMP <ea>,Dn
                return (isLong ? 6 : 4) + EaTime(srcMode, srcReg, isLong);
            }
            // opmode 4/5/6: EOR Dn,<ea> (or CMPM when mode==1)
            if (srcMode == 1) // CMPM (An)+,(An)+
                return isLong ? 20 : 12;
            if (srcMode == 0) // EOR Dn,Dn
                return isLong ? 8 : 4;
            return (isLong ? 12 : 8) + EaTime(srcMode, srcReg, isLong); // EOR Dn,mem
        }

        // ---- 1110: shifts / rotates -------------------------------------------
        case 0xE: {
            if (((op >> 6) & 3) == 3) { // memory shift (by 1, word)
                return 8 + EaTime(srcMode, srcReg, false);
            }
            // register shift/rotate
            const bool immCount = ((op >> 5) & 1) == 0;
            int count;
            if (immCount) {
                count = (op >> 9) & 7;
                if (count == 0)
                    count = 8;
            } else {
                const int cntReg = (op >> 9) & 7; // Dn holds the count
                if (!dregs)
                    return -1; // dynamic count unknown without regs
                count = (int)(dregs[cntReg] & 63);
            }
            return (isLong ? 8 : 6) + 2 * count;
        }

        default:
            return -1;
    }
}

} // namespace adm::core
