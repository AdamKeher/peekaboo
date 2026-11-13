#include "Disassembler.h"

#include <array>
#include <cctype>
#include <cstdlib>

#include <fmt/format.h>

namespace adm::core {

Disassembler::Disassembler(Backend backend) : m_backend(std::move(backend)) {}

bool Disassembler::DisassembleOne(uint32_t addr, Line& out) const
{
    if (!m_backend)
        return false;

    char mnemo[512] = "";
    char hexStr[128] = "";
    uint32_t nextPc = addr;
    if (!m_backend(addr, mnemo, sizeof(mnemo), hexStr, sizeof(hexStr), nextPc))
        return false;

    out.address = addr;
    out.nextAddress = nextPc;
    out.mnemonic = mnemo[0] ? mnemo : "???";
    out.hex = hexStr;
    // Trim trailing whitespace the backend may append after the last hex word
    while (!out.hex.empty() && out.hex.back() == ' ')
        out.hex.pop_back();
    return true;
}

std::vector<Disassembler::Line>
Disassembler::DisassembleRange(uint32_t start, int maxLines, std::optional<uint32_t> endAddr) const
{
    std::vector<Line> lines;
    uint32_t addr = start;
    for (int i = 0; i < maxLines; ++i) {
        if (endAddr && addr > *endAddr)
            break;
        Line line;
        if (!DisassembleOne(addr, line))
            break;
        if (line.nextAddress <= addr)
            break;
        lines.push_back(std::move(line));
        addr = lines.back().nextAddress;
    }
    return lines;
}

std::string Disassembler::FormatLine(const Line& line)
{
    std::string out = fmt::format("{:08X}: {}", line.address, line.hex);
    if (static_cast<int>(out.size()) < kMnemonicColumn)
        out.append(kMnemonicColumn - out.size(), ' ');
    else
        out.push_back(' ');
    out += line.mnemonic;
    return out;
}

Disassembler::OpcodeToken Disassembler::LeadingOpcode(const std::string& mnemonic)
{
    OpcodeToken tok;
    size_t begin = mnemonic.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return tok;
    // Opcode runs to the first whitespace or '.' (size suffix).
    size_t end = begin;
    while (end < mnemonic.size() && mnemonic[end] != ' ' && mnemonic[end] != '\t' &&
           mnemonic[end] != '.')
        ++end;

    tok.op = mnemonic.substr(begin, end - begin);
    for (char& c : tok.op)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (end < mnemonic.size() && mnemonic[end] == '.' && end + 1 < mnemonic.size())
        tok.size = static_cast<char>(std::toupper(static_cast<unsigned char>(mnemonic[end + 1])));
    return tok;
}

bool Disassembler::IsConditionCode(const std::string& cc)
{
    static constexpr std::array<const char*, 18> kCond = {"T",  "F",  "HI", "LS", "CC", "CS",
                                                          "NE", "EQ", "VC", "VS", "PL", "MI",
                                                          "GE", "LT", "GT", "LE", "HS", "LO"};
    for (const char* c : kCond)
        if (cc == c)
            return true;
    return false;
}

bool Disassembler::IsSectionEnd(const std::string& mnemonic)
{
    const std::string op = LeadingOpcode(mnemonic).op;
    if (op.empty())
        return false;

    static constexpr std::array<const char*, 11> kEnders = {
        "RTS", "RTE", "RTR", "RTD", "BRA", "JMP", "TRAP", "TRAPV", "ILLEGAL", "STOP", "RESET"};
    for (const char* e : kEnders)
        if (op == e)
            return true;
    return false;
}

bool Disassembler::ParseRelativeBranch(const std::string& mnemonic, uint32_t instrAddr,
                                       uint32_t& target, bool& conditional)
{
    const OpcodeToken tok = LeadingOpcode(mnemonic);
    const std::string& op = tok.op;
    if (op.empty())
        return false;

    // Size suffix ('.S'/'.B' = 8-bit disp, '.W' = 16, '.L' = 32). DBcc has no suffix
    // and always uses a 16-bit displacement.
    int dispBits = 16;
    if (tok.size == 'S' || tok.size == 'B')
        dispBits = 8;
    else if (tok.size == 'L')
        dispBits = 32;

    // Classify the branch from the shared opcode/condition-code tables so this stays
    // in lockstep with the views' control-flow gutters:
    //   BRA/BSR          -> unconditional relative branch / call
    //   B<cc>            -> conditional byte/word branch (incl. HS/LO aliases)
    //   DB<cc> / DBRA    -> decrement-and-branch (looping; always word displacement)
    bool match = false;
    conditional = true;
    if (op == "BRA" || op == "BSR") {
        match = true;
        conditional = false;
    } else if (op.size() >= 2 && op[0] == 'B' && IsConditionCode(op.substr(1))) {
        match = true;
    } else if (op.size() >= 3 && op[0] == 'D' && op[1] == 'B' &&
               (op.substr(2) == "RA" || IsConditionCode(op.substr(2)))) {
        match = true;
    }
    if (!match)
        return false;

    // Displacement is the last "$xxxx" literal (Bcc: "#$disp"; DBcc: "Dn,#$disp").
    size_t dollar = mnemonic.rfind('$');
    if (dollar == std::string::npos)
        return false;
    char* parseEnd = nullptr;
    unsigned long value = std::strtoul(mnemonic.c_str() + dollar + 1, &parseEnd, 16);
    if (parseEnd == mnemonic.c_str() + dollar + 1)
        return false; // no hex digits followed '$'

    // Sign-extend the displacement to 32 bits, then resolve against the PC base
    // (instruction address + 2), the m68k convention for branch displacements.
    int32_t disp;
    if (dispBits >= 32) {
        disp = static_cast<int32_t>(value);
    } else {
        // dispBits is 8 or 16 here, so the mask never needs the 32-bit special case.
        uint32_t signBit = 1u << (dispBits - 1);
        uint32_t mask = (1u << dispBits) - 1;
        uint32_t v = static_cast<uint32_t>(value) & mask;
        disp = static_cast<int32_t>((v ^ signBit) - signBit);
    }
    target = instrAddr + 2u + static_cast<uint32_t>(disp);
    return true;
}

} // namespace adm::core
