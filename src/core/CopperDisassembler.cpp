#include "CopperDisassembler.h"
#include <cstdio>
#include <string>

namespace adm::core {

CopperInstruction CopperDisassembler::Disassemble(uint16_t word1, uint16_t word2,
                                                  const nlohmann::json& customRegsDef)
{
    CopperInstruction inst;
    inst.word1 = word1;
    inst.word2 = word2;

    char hexWords[32];
    snprintf(hexWords, sizeof(hexWords), "$%04X,$%04X", word1, word2);
    inst.formattedWords = hexWords;

    inst.isColor = false;

    if (word1 == 0x01FE) {
        inst.type = CopperInstruction::Type::NOOP;
        inst.name = "NOOP";
        inst.details = "No Operation";
    } else if (word1 == 0x0001 && word2 == 0x7F00) {
        inst.type = CopperInstruction::Type::WAIT;
        inst.name = "WAIT";
        inst.details = "Wait for blitter finished";
    } else if ((word1 & 1) == 0) {
        inst.type = CopperInstruction::Type::MOVE;
        inst.name = "MOVE";

        uint16_t regOffset = word1 & 0x1FE;
        char hexOffset[16];
        snprintf(hexOffset, sizeof(hexOffset), "%03X", regOffset);

        std::string regName = "UNKNOWN";
        if (customRegsDef.contains(hexOffset)) {
            auto& def = customRegsDef[hexOffset];
            if (def.contains("name") && def["name"].is_string()) {
                regName = def["name"].get<std::string>();
            }
        }
        inst.registerName = regName;

        char details[256];
        snprintf(details, sizeof(details), "%s = $%04X", regName.c_str(), word2);
        inst.details = details;

        if (regOffset >= 0x180 && regOffset <= 0x1BE) {
            inst.isColor = true;
            inst.r = ((word2 >> 8) & 0xF) / 15.0f;
            inst.g = ((word2 >> 4) & 0xF) / 15.0f;
            inst.b = ((word2 >> 0) & 0xF) / 15.0f;
        }
    } else {
        // WAIT (IR2 bit 0 == 0) and SKIP (IR2 bit 0 == 1) share the same position-compare encoding:
        //   IR1: VP = bits 15-8 (vertical pos),  HP = bits 7-1 (horizontal pos)
        //   IR2: BFD = bit 15,  VE = bits 14-8 (vertical mask),  HE = bits 7-1 (horizontal mask)
        // The compare only tests the bits set in the VE/HE masks; when BFD is clear the copper also
        // waits for the blitter to finish.
        const bool isSkip = (word2 & 1) != 0;
        inst.type = isSkip ? CopperInstruction::Type::SKIP : CopperInstruction::Type::WAIT;
        inst.name = isSkip ? "SKIP" : "WAIT";

        if (!isSkip && word1 == 0xFFFF && word2 == 0xFFFE) {
            inst.details = "End of Copperlist";
        } else {
            const uint8_t vp = (word1 >> 8) & 0xFF;
            const uint8_t hp = word1 & 0xFE;
            const uint8_t he = word2 & 0xFE;
            const bool blitterWait = (word2 & 0x8000) == 0; // BFD clear → also wait for blitter

            const char* verb = isSkip ? "Skip if" : "Wait for";
            char details[256];
            int n;
            if (he == 0)
                n = snprintf(details, sizeof(details), "%s vpos >= %d", verb, vp);
            else
                n = snprintf(details, sizeof(details), "%s vpos >= %d and hpos >= %d", verb, vp,
                             hp);
            if (blitterWait && n > 0 && n < static_cast<int>(sizeof(details)))
                snprintf(details + n, sizeof(details) - static_cast<size_t>(n),
                         " (+ blitter done)");
            inst.details = details;
        }
    }

    return inst;
}

} // namespace adm::core
