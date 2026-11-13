#include "UaeAssembler.h"

namespace adm::core {

UaeAssembler::UaeAssembler(Backend backend) : m_backend(std::move(backend)) {}

bool UaeAssembler::AssembleOne(const std::string& source, uint32_t addr, std::vector<uint8_t>& out,
                               std::string& err)
{
    if (!m_backend) {
        err = "ASSEMBLER UNAVAILABLE";
        return false;
    }

    uint16_t words[16] = {};
    const int n = m_backend(source, addr, words, 16);
    if (n <= 0) {
        if (n == -1)
            err = "LINE TOO LONG";
        else if (n == -2)
            err = "ASSEMBLER UNAVAILABLE";
        else
            err = "SYNTAX ERROR";
        return false;
    }

    // Emit big-endian byte pairs — Amiga memory byte order, matching put_word()
    out.clear();
    out.reserve(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        out.push_back(static_cast<uint8_t>(words[i] >> 8));
        out.push_back(static_cast<uint8_t>(words[i] & 0xFF));
    }
    return true;
}

} // namespace adm::core
