#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace adm::core {

// Annotates Amiga library calls of the form `JSR/JMP (aN,-$xxx)` with the
// resolved function name, e.g. "  ; exec/DoIO()". The negative displacement off
// the base register is the library vector offset (LVO); each .library has its
// own LVO->name table, loaded from JSON (assets/amiga_lvos.json).
//
// Two resolution strategies (Layer 1, a + c):
//   (a) default: assume the call targets exec.library.
//   (c) refine:  read the live base-register value and the library node's name
//       string from emulated memory to pick the correct table. Falls back to (a)
//       when no runtime context is supplied or memory can't be read.
class LibraryCallAnnotator
{
public:
    // Optional runtime context for strategy (c). Either hook may be null, in
    // which case Annotate falls back to the exec default (strategy a).
    struct Context
    {
        // Live value of address register aReg (0..7); false if unavailable.
        std::function<bool(int aReg, uint32_t& val)> readAReg;
        // One byte from emulated Amiga memory; false if the address is unmapped.
        std::function<bool(uint32_t addr, uint8_t& out)> readByte;
    };

    // Loads LVO tables from JSON. Returns false if the file is missing or
    // unparseable (Annotate then no-ops). Safe to call repeatedly; reloads.
    bool LoadTables(const std::string& path = "assets/amiga_lvos.json");
    bool Loaded() const { return !m_tables.empty(); }

    // Returns "  ; <lib>/<func>()" for a library-call mnemonic, or nullopt when
    // the line isn't a library call or the offset isn't in the chosen table.
    std::optional<std::string> Annotate(const std::string& mnemonic, const Context& ctx = {}) const;

    // Parses a `(aN,-$xxx)` / `-$xxx(aN)` base+displacement library call out of a
    // disassembled JSR/JMP. Returns false if the line isn't that shape. Exposed
    // for unit testing. aReg in [0,7]; lvo is the positive vector offset.
    static bool ParseLibCall(const std::string& mnemonic, int& aReg, uint32_t& lvo);

private:
    // Reads the library node name at base (a Library begins with struct Node, so
    // ln_Name is the APTR at offset 10). Empty when unreadable or implausible.
    std::string ReadLibraryName(uint32_t base, const Context& ctx) const;

    // libraryName -> (positive lvo -> function name)
    std::map<std::string, std::map<uint32_t, std::string>> m_tables;
};

} // namespace adm::core
