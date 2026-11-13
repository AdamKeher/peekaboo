#pragma once

#include <functional>

#include "Assembler.h"

namespace adm::core {

// Assembler backed by UAE's m68k_asm() (exported as adm_asm_one). Like
// Disassembler, the emulator call is injected as a backend so the class stays
// testable without a running core.
class UaeAssembler : public Assembler
{
public:
    // Returns word count (>0), 0 = syntax error, -1 = line too long,
    // -2 = backend unavailable. Words are host-endian opcode words.
    using Backend =
        std::function<int(const std::string& src, uint32_t addr, uint16_t* words, int maxWords)>;

    explicit UaeAssembler(Backend backend);

    bool AssembleOne(const std::string& source, uint32_t addr, std::vector<uint8_t>& out,
                     std::string& err) override;

private:
    Backend m_backend;
};

} // namespace adm::core
