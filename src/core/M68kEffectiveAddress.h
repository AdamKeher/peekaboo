#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace adm::core {

// The register file as it stood when an instruction started executing. This is
// exactly what the CPU trace records, which is what makes after-the-fact
// effective-address decoding possible at all.
struct M68kRegisters
{
    uint32_t d[8] = {};
    uint32_t a[8] = {};
};

// One memory location an instruction touches.
struct M68kAccess
{
    uint32_t addr = 0;
    uint8_t size = 0; // operand width in bytes: 1, 2 or 4
    bool read = false;
    bool write = false;

    bool Touches(uint32_t lo, uint32_t hi) const // [lo, hi)
    {
        return addr < hi && lo < addr + size;
    }
};

/**
 * @brief Works out which memory an instruction reads and writes.
 *
 * The CPU trace stores each instruction's raw bytes alongside D0-D7/A0-A7 at the
 * moment it ran, so the effective address it computed can be reconstructed
 * afterwards. That is what turns "who touched $1234?" and "what last wrote
 * here?" into questions the trace can answer without re-running anything.
 *
 * Coverage is the 68000 instruction set, minus a few forms that never carry a
 * useful data address (MOVEP, the BCD/extended-precision memory forms). Anything
 * not understood yields no accesses rather than a guess — a missing answer is
 * recoverable, a wrong one is not.
 */
class M68kEffectiveAddress
{
public:
    // `raw` is the instruction's bytes as captured (big-endian, opcode first),
    // `pc` the address it executed at, `regs` the register file before it ran.
    static std::vector<M68kAccess> Decode(const uint8_t* raw, size_t rawLen, uint32_t pc,
                                          const M68kRegisters& regs);
};

} // namespace adm::core
