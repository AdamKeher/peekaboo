#pragma once

// Break-on-KPrintF-text: a host-side breakpoint store matched against the debug output the
// emulated program writes to the serial port (KPrintF / RawPutChar / kprintf all end up there).
//
// Unlike PC breakpoints and memory watchpoints there is no UAE-side node for this — PuAE hands
// the serial bytes to the host (adm_drain_serial_output), AmigaEmulator assembles them into
// lines, and each *completed* line is offered to MatchLine() here. A match raises a host-side
// debug break, so the emulator stops on the frame that produced the text.
//
// Kept free of ImGui / emulator dependencies so the matching rules can be unit-tested against
// adm_core without linking the executable target.

#include <cstdint>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace adm::core {

// How a rule's text is compared against a KPrintF line.
enum class KprintfMatch {
    Contains, // line contains the text anywhere (default — what you usually want)
    Exact,    // whole line equals the text
    Prefix,   // line starts with the text
    Regex,    // ECMAScript regular expression searched anywhere in the line
};

struct KprintfBreakpoint
{
    int id = 0;       // stable non-zero handle, assigned by Add()
    std::string text; // the pattern (literal text, or a regex when match == Regex)
    KprintfMatch match = KprintfMatch::Contains;
    bool caseSensitive = false; // default off: debug output casing is rarely worth matching on
    bool enabled = true;
    bool oneShot = false; // remove the rule once it has fired
    uint64_t hits = 0;    // how many lines this rule has matched
    std::string lastLine; // the most recent line that matched (for the UI tooltip / banner)

    // Compiled form of `text` for Regex rules; null for the literal modes. Shared so the entry
    // stays cheap to copy out of the store.
    std::shared_ptr<const std::regex> compiled;
};

// The line that fired a rule, reported once to whoever renders the break banner.
struct KprintfHit
{
    int id = 0;       // rule that matched (0 if the rule was one-shot and is already gone)
    std::string text; // the rule's pattern
    std::string line; // the KPrintF line that matched
    KprintfMatch match = KprintfMatch::Contains;
};

class KprintfBreakpoints
{
public:
    // Adds a rule. Returns its id, or 0 when the text is empty, an identical rule already exists
    // (same text + match mode + case sensitivity), or the regex fails to compile.
    int Add(const std::string& text, KprintfMatch match, bool caseSensitive, bool oneShot);

    bool Remove(int id);
    // Removes the first rule whose pattern equals `text` exactly. Returns false if none matched.
    bool RemoveByText(const std::string& text);
    void Clear();

    bool SetEnabled(int id, bool enabled);
    // Zeroes the hit counter and last-matched line of a rule (id 0 = every rule).
    void ResetHits(int id = 0);

    const std::vector<KprintfBreakpoint>& Entries() const { return m_entries; }
    bool Empty() const { return m_entries.empty(); }
    const KprintfBreakpoint* Find(int id) const;

    // Offers one completed KPrintF line to the enabled rules, in insertion order. On the first
    // match: bumps that rule's hit count, records the line, drops the rule if it is one-shot, and
    // returns the hit. Returns nullopt when nothing matched.
    std::optional<KprintfHit> MatchLine(const std::string& line);

    // Session persistence (hit counts are transient and not serialized).
    nlohmann::json Export() const;
    void Import(const nlohmann::json& j);

    static const char* MatchName(KprintfMatch m);
    // Parses a mode name ("contains"/"exact"/"prefix"/"regex", any case). nullopt if unknown.
    static std::optional<KprintfMatch> ParseMatch(std::string_view name);

private:
    std::vector<KprintfBreakpoint> m_entries;
    int m_nextId = 1;
};

} // namespace adm::core
