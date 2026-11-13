#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace adm::core {

struct CopperInstruction
{
    enum class Type { MOVE, WAIT, SKIP, NOOP };

    Type type;
    std::string name;           // "MOVE", "WAIT", "SKIP", "NOOP"
    std::string details;        // Formatted details (e.g. "BPLCON0 = $4200")
    std::string registerName;   // MOVE destination register (empty for WAIT/SKIP/NOOP)
    std::string formattedWords; // "$xxxx,$xxxx"
    uint16_t word1;
    uint16_t word2;

    // For COLORxx registers or highlighting
    bool isColor;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

class CopperDisassembler
{
public:
    static CopperInstruction Disassemble(uint16_t word1, uint16_t word2,
                                         const nlohmann::json& customRegsDef);
};

} // namespace adm::core
