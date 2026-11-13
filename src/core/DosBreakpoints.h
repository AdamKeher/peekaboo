#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ExecIntrospect.h"
#include "LibraryCallTracer.h"

namespace adm::core {

// How a nameMatch predicate is applied to the argument string.
enum class DosNameMatch {
    Contains,
    Equals,
    Prefix,
    Suffix,
};

struct DosBreakRule
{
    int id = 0;
    std::string library;  // "dos.library"
    std::string function; // "Open"
    uint32_t lvo = 0;
    uint32_t entryPc = 0; // libBase - lvo, where the breakpoint sits

    // Optional predicate on a string argument. Empty nameMatch means "break on
    // every call".
    std::string nameMatch;
    DosNameMatch match = DosNameMatch::Contains;
    bool caseSensitive = false;
    std::string argReg;                    // register holding the string, e.g. "d1"
    LvoArgType argType = LvoArgType::CStr; // how to read it

    bool oneShot = false;
    uint64_t hits = 0;     // calls seen at this entry
    uint64_t breaks = 0;   // calls that actually stopped
    std::string lastValue; // the string the most recent hit carried
};

/**
 * @brief Breakpoints on AmigaOS library entry points, with a string predicate.
 *
 * "Break when anything opens a file matching loader" cannot be expressed as an
 * address plus a register comparison, because the thing being compared is a
 * string somewhere else in memory. So the breakpoint fires on every call and
 * this decides, from the argument the call carried, whether the stop was the one
 * the caller asked for — resuming silently when it was not.
 *
 * The cost is a host round-trip per call through that function, which is fine
 * for Open or LoadSeg and painful if pointed at Read.
 */
class DosBreakpoints
{
public:
    // Registers a rule. `entryPc` must already be resolved. Returns the new id,
    // or 0 with outError set.
    int Add(DosBreakRule rule, std::string& outError);
    bool Remove(int id);
    void Clear();
    const std::vector<DosBreakRule>& List() const { return m_rules; }
    bool Empty() const { return m_rules.empty(); }

    // Reads one register of the stopped CPU by name ("d1", "a1", ...).
    using ReadRegisterFn = std::function<bool(const std::string& name, uint32_t& out)>;

    // Decides what to do about a stop at `pc`.
    struct Decision
    {
        bool ours = false;   // a rule owns this address
        bool resume = false; // silently continue: the predicate did not match
        int ruleId = 0;
        std::string value; // the argument string the call carried
    };

    // Call from the debug-break filter. Updates hit counters and removes
    // one-shot rules that fired.
    Decision OnBreak(uint32_t pc, const ReadRegisterFn& readReg, const exec::Readers& readers);

    static bool ParseMatchMode(const std::string& text, DosNameMatch& out);
    static bool Matches(const std::string& haystack, const std::string& needle, DosNameMatch mode,
                        bool caseSensitive);

private:
    std::vector<DosBreakRule> m_rules;
    int m_nextId = 1;
};

} // namespace adm::core
