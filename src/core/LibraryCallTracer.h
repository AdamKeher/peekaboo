#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "CpuTraceHistory.h"
#include "ExecIntrospect.h"

namespace adm::core {

// How a register should be rendered when a traced call is reported.
enum class LvoArgType {
    Long,  // signed
    ULong, // unsigned
    Ptr,   // an address
    CStr,  // NUL-terminated string in emulated memory
    BStr,  // BCPL string given as a BPTR (dos.library filenames)
    BPtr,  // BCPL pointer (address >> 2)
};

struct LvoArgSpec
{
    std::string reg; // "d1", "a1", ...
    std::string name;
    LvoArgType type = LvoArgType::ULong;
};

struct LvoSpec
{
    std::string name; // function name from the metadata (may differ in case only)
    std::vector<LvoArgSpec> args;
    bool haveResult = false;
    LvoArgSpec result;
};

// A decoded argument or result value.
struct LibraryCallValue
{
    std::string name;
    std::string reg;
    uint32_t value = 0;
    std::string text; // dereferenced string, when the type is one
};

struct LibraryCallEntry
{
    uint64_t seq = 0; // trace sequence of the jump-table instruction
    uint64_t frame = 0;
    uint32_t cycle = 0;
    uint32_t lRef = 0;
    uint32_t callerPc = 0; // return address - the call site is just before it
    uint32_t entryPc = 0;  // libBase - lvo
    std::string library;
    std::string function;
    uint32_t lvo = 0;
    std::vector<LibraryCallValue> args;
    bool haveResult = false;
    LibraryCallValue result;
};

struct LibraryCallSnapshot
{
    bool available = false;
    std::string hint;
    int librariesScanned = 0;
    uint32_t totalCalls = 0;
    bool truncated = false;
    std::vector<LibraryCallEntry> calls;
};

struct LibraryCallQuery
{
    std::string library;                // empty = every open library
    std::vector<std::string> functions; // empty = every function
    uint64_t sinceSeq = 0;
    int limit = 64; // <= 0 means all
};

/**
 * @brief Reconstructs library calls, with decoded arguments, from the CPU trace.
 *
 * A call through a library's jump table executes the JMP at `libBase - lvo`, so
 * every trace record whose PC equals one of those addresses is a call. Because
 * the trace also captured D0-D7/A0-A7 at that instant, the arguments are still
 * there to read — including the strings they point at — and D0 at the return
 * address gives the result. No emulator hook and no core rebuild is involved.
 */
class LibraryCallTracer
{
public:
    // Loads the per-function argument metadata. Missing or unparseable file is
    // not fatal: calls still trace, they just report raw register values.
    bool LoadArgTables(const std::string& path = "assets/amiga_lvo_args.json");
    bool ArgTablesLoaded() const { return !m_argTables.empty(); }

    // Loads the generated LVO name tables (the same file LibraryCallAnnotator
    // uses). Without it, functions are reported by offset only.
    bool LoadNameTables(const std::string& path = "assets/amiga_lvos.json");

    // Resolve one function to its jump-table entry address in the running
    // machine, e.g. ("dos.library", "Open") -> libBase - 30. nullopt when the
    // library is not open or the name is unknown.
    std::optional<uint32_t> ResolveEntry(const exec::Readers& r, uint32_t execBase,
                                         const std::string& library, const std::string& function,
                                         uint32_t* outLvo = nullptr) const;

    // Look up the argument metadata for one function, if any.
    const LvoSpec* FindSpec(const std::string& library, uint32_t lvo) const;

    LibraryCallSnapshot Trace(const CpuTraceHistory& history, const exec::Readers& r,
                              uint32_t execBase, const LibraryCallQuery& query) const;

    // Renders one register through an argument spec, dereferencing strings.
    static LibraryCallValue DecodeValue(const LvoArgSpec& spec, uint32_t raw,
                                        const exec::Readers& r);

    static bool ParseArgType(const std::string& text, LvoArgType& out);

private:
    // library name -> lvo -> spec / function name
    std::map<std::string, std::map<uint32_t, LvoSpec>> m_argTables;
    std::map<std::string, std::map<uint32_t, std::string>> m_nameTables;
};

} // namespace adm::core
