#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adm::core {

// Counterpart to Disassembler: assembles one 68k source line into machine code.
// No implementation ships yet — the AR console's `A` command takes any object
// implementing this interface (see ArHost::assembler). A future backend can be
// a table-driven line assembler or a wrapper around an external one; either way
// results are verifiable by round-tripping through the shared Disassembler.
class Assembler
{
public:
    virtual ~Assembler() = default;

    // Assemble one source line at addr; on success fills the encoded bytes.
    virtual bool AssembleOne(const std::string& source, uint32_t addr, std::vector<uint8_t>& out,
                             std::string& err) = 0;
};

} // namespace adm::core
