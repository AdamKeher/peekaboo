#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace adm::core {

// Centralised 68k disassembly. All views and the AR console disassemble through
// this class so formatting and iteration behaviour stay in one place.
class Disassembler
{
public:
    struct Line
    {
        uint32_t address = 0;
        uint32_t nextAddress = 0; // address + instruction length
        std::string mnemonic;     // e.g. "JMP $00FC00D2"
        std::string hex;          // e.g. "4EF9 00FC 00D2"
    };

    // Matches AmigaEmulator::Disassemble's signature.
    using Backend = std::function<bool(uint32_t pc, char* mnemo, int mnemoSz, char* hex, int hexSz,
                                       uint32_t& nextPc)>;

    // Construct with a backend lambda, typically wrapping AmigaEmulator::Disassemble.
    // Kept emulator-agnostic so it links into the (GLFW-free) unit test binary.
    Disassembler() = default;
    explicit Disassembler(Backend backend);

    bool HasBackend() const { return static_cast<bool>(m_backend); }

    bool DisassembleOne(uint32_t addr, Line& out) const;

    // Disassembles up to maxLines starting at start. Stops early if the backend
    // fails, the address fails to advance, or the next instruction would start
    // beyond endAddr (when given).
    std::vector<Line> DisassembleRange(uint32_t start, int maxLines,
                                       std::optional<uint32_t> endAddr = std::nullopt) const;

    // Canonical single-line text form, fixed columns:
    // "00FC0000: 4EF9 00FC 00D2           JMP $00FC00D2"
    static std::string FormatLine(const Line& line);

    // Leading opcode of a disassembled mnemonic, split into the bare opcode token
    // (uppercased, no size suffix) and the size character. The single shared
    // tokenizer behind IsSectionEnd / ParseRelativeBranch and the views' control-
    // flow classifiers, so they can't drift apart.
    struct OpcodeToken
    {
        std::string op;   // uppercased opcode, no size suffix ("BNE", "JMP"); empty if none
        char size = '\0'; // size suffix char: 'B' 'W' 'L' 'S' (uppercased) or '\0' if none

        bool empty() const { return op.empty(); }
    };
    static OpcodeToken LeadingOpcode(const std::string& mnemonic);

    // True if `cc` (already uppercased) is a 68k condition-code suffix: the HI/LS/
    // CC/CS/NE/EQ/VC/VS/PL/MI/GE/LT/GT/LE conditions, the HS/LO aliases of CC/CS,
    // and the T/F always/never forms (Bcc.T / DBcc). The BRA/BSR pseudo-conditions
    // "RA"/"SR" are deliberately excluded — those opcodes are matched by name.
    static bool IsConditionCode(const std::string& cc);

    // True for instructions that unconditionally end a code block (AR-style routine
    // breaks): RTS RTE RTR RTD BRA JMP TRAP TRAPV ILLEGAL STOP RESET. Used to draw a
    // section separator after the instruction. Accepts a bare mnemonic ("RTS",
    // "BRA.S $1000", "JMP (A0)") — only the leading opcode token is examined.
    static bool IsSectionEnd(const std::string& mnemonic);

    // If `mnemonic` is a PC-relative branch (BRA/BSR/Bcc/DBcc), returns true and fills
    // `target` plus `conditional` (false for BRA/BSR, true for Bcc/DBcc). The branch
    // operand is the displacement immediate as printed ("#$0128"); the absolute target
    // is computed as instrAddr + 2 + signExtend(displacement), the m68k PC-relative
    // base. JMP/JSR and register-indirect branches return false — not local code.
    static bool ParseRelativeBranch(const std::string& mnemonic, uint32_t instrAddr,
                                    uint32_t& target, bool& conditional);

    // Column where the mnemonic starts in FormatLine output (address 8 + ": " 2 + hex field).
    static constexpr int kHexColumn = 10;
    static constexpr int kMnemonicColumn = 36; // 10 + 25 (5 words max) + 1 space

private:
    Backend m_backend;
};

} // namespace adm::core
