#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace adm::core {

/// DWARF register numbers for m68k, as GCC emits them.
/// d0-d7 = 0-7, a0-a7 = 8-15, fp0-fp7 = 16-23, and 24 is the return-address
/// column (the PC). a7 is the stack pointer; there is no separate SP number.
inline constexpr uint32_t kDwarfRegD0 = 0;
inline constexpr uint32_t kDwarfRegA0 = 8;
inline constexpr uint32_t kDwarfRegSp = 15; // a7
inline constexpr uint32_t kDwarfRegPc = 24;
inline constexpr uint32_t kCfiRegCount = 25; // 0..24 inclusive

/// How a register's value in the *caller* is recovered.
enum class CfiRegRuleKind : uint8_t {
    Undefined,    // not recoverable (the caller's copy is gone)
    SameValue,    // unchanged by this frame
    OffsetCfa,    // saved in memory at CFA + offset
    ValOffsetCfa, // the value *is* CFA + offset (not a memory location)
    Register,     // held in another register
};

struct CfiRegRule
{
    CfiRegRuleKind kind = CfiRegRuleKind::Undefined;
    int32_t offset = 0; // for OffsetCfa / ValOffsetCfa
    uint32_t reg = 0;   // for Register
};

/**
 * @brief The unwind rules in force at one code address: how to compute this frame's
 *        CFA, and where each register the caller cares about was saved.
 *
 * The CFA (Canonical Frame Address) is by DWARF's definition the caller's stack
 * pointer at the point of the call, which is what makes it the anchor for both the
 * return address and the caller's SP.
 */
struct CfiRow
{
    uint32_t cfaReg = kDwarfRegSp;
    int32_t cfaOffset = 0;
    bool cfaValid = false;
    uint32_t raRegister = kDwarfRegPc; // from the CIE; 24 for every m68k CIE seen
    std::array<CfiRegRule, kCfiRegCount> reg{};
};

/**
 * @brief Reads DWARF call-frame information (`.debug_frame`) from an ELF and answers
 *        "how do I unwind out of the frame at this address?".
 *
 * This is the piece that lets Peekaboo produce a real backtrace at a halted PC without
 * a gdb in the loop. It matters more than it might sound: the bartman m68k toolchain
 * builds with no frame pointer, so a6 is just another callee-saved register and there
 * is no linked list of frames to walk. Every CFA in a bartman build is expressed as
 * `a7 + N` with N changing at practically every push and pop -- which only CFI records.
 *
 * Sits beside DebugSymbols / DebugLines / DebugInline: dependency-free, decoupled from
 * the emulator, and resolving by **link-time** address (runtime PC minus the load base).
 * Like DebugLines it reads raw section bytes with no relocation pass, which is correct
 * because this toolchain links `.text` at 0, so a stored `initial_location` already is
 * the final `.text` offset.
 *
 * Scope: `.debug_frame` only, not `.eh_frame` (which the toolchain does not emit).
 * CIE versions 1, 3 and 4 with an empty augmentation string. DWARF *expressions*
 * (DW_CFA_def_cfa_expression / DW_CFA_expression) are deliberately not evaluated --
 * ResolveRow reports failure instead, because an honest "no CFI here" is worth more to
 * a debugger than a fabricated frame. GCC does not emit them for this target.
 */
class DebugFrame
{
public:
    bool LoadFromElf(const std::string& path);
    bool LoadFromElf(std::span<const uint8_t> data);
    void Clear();

    [[nodiscard]] bool IsLoaded() const { return !m_fdes.empty(); }
    [[nodiscard]] size_t FdeCount() const { return m_fdes.size(); }
    [[nodiscard]] size_t CieCount() const { return m_cies.size(); }

    /// True when some FDE covers this link-time address.
    [[nodiscard]] bool Covers(uint32_t linkAddr) const;

    /**
     * @brief Run the CFI state machine up to `linkAddr` and report the rules in force.
     * @return false when no FDE covers the address, or when the rules need a DWARF
     *         expression we decline to evaluate.
     */
    [[nodiscard]] bool ResolveRow(uint32_t linkAddr, CfiRow& out) const;

private:
    struct Cie
    {
        uint32_t codeAlign = 1;
        int32_t dataAlign = 1;
        uint32_t raRegister = kDwarfRegPc;
        size_t instrBegin = 0; // initial instructions, as offsets into m_section
        size_t instrEnd = 0;
        bool usable = false; // false for augmentations we decline to parse
    };

    struct Fde
    {
        uint32_t loc = 0;   // link-time start address
        uint32_t range = 0; // byte length of the covered code
        size_t cieIndex = 0;
        size_t instrBegin = 0;
        size_t instrEnd = 0;
    };

    // Execute CFI opcodes over [begin, end), stopping once `location` passes `target`.
    // Returns false if an opcode we refuse to guess at is encountered.
    bool RunProgram(size_t begin, size_t end, const Cie& cie, uint32_t target, uint32_t& location,
                    CfiRow& row, const CfiRow& initial) const;

    std::vector<uint8_t> m_section; // raw .debug_frame bytes
    std::vector<Cie> m_cies;
    std::vector<Fde> m_fdes; // sorted ascending by loc
};

} // namespace adm::core
