#include "DosBreakpoints.h"

#include <algorithm>
#include <cctype>

namespace adm::core {

namespace {

std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

bool DosBreakpoints::ParseMatchMode(const std::string& text, DosNameMatch& out)
{
    const std::string t = ToLower(text);
    if (t.empty() || t == "contains") {
        out = DosNameMatch::Contains;
        return true;
    }
    if (t == "equals" || t == "exact") {
        out = DosNameMatch::Equals;
        return true;
    }
    if (t == "prefix" || t == "startswith") {
        out = DosNameMatch::Prefix;
        return true;
    }
    if (t == "suffix" || t == "endswith") {
        out = DosNameMatch::Suffix;
        return true;
    }
    return false;
}

bool DosBreakpoints::Matches(const std::string& haystack, const std::string& needle,
                             DosNameMatch mode, bool caseSensitive)
{
    if (needle.empty())
        return true;
    const std::string h = caseSensitive ? haystack : ToLower(haystack);
    const std::string n = caseSensitive ? needle : ToLower(needle);
    switch (mode) {
        case DosNameMatch::Equals:
            return h == n;
        case DosNameMatch::Prefix:
            return h.rfind(n, 0) == 0;
        case DosNameMatch::Suffix:
            return h.size() >= n.size() && h.compare(h.size() - n.size(), n.size(), n) == 0;
        case DosNameMatch::Contains:
            break;
    }
    return h.find(n) != std::string::npos;
}

int DosBreakpoints::Add(DosBreakRule rule, std::string& outError)
{
    if (rule.entryPc == 0) {
        outError = "no entry address resolved for that function";
        return 0;
    }
    if (!rule.nameMatch.empty() && rule.argReg.empty()) {
        outError = "nameMatch needs a string argument to test, but " + rule.function +
                   " has no known string parameter";
        return 0;
    }
    for (const auto& r : m_rules)
        if (r.entryPc == rule.entryPc && r.nameMatch == rule.nameMatch) {
            outError = "an identical rule is already set on " + r.library + "/" + r.function;
            return 0;
        }
    rule.id = m_nextId++;
    const int id = rule.id;
    m_rules.push_back(std::move(rule));
    return id;
}

bool DosBreakpoints::Remove(int id)
{
    auto it = std::find_if(m_rules.begin(), m_rules.end(),
                           [id](const DosBreakRule& r) { return r.id == id; });
    if (it == m_rules.end())
        return false;
    m_rules.erase(it);
    return true;
}

void DosBreakpoints::Clear() { m_rules.clear(); }

DosBreakpoints::Decision DosBreakpoints::OnBreak(uint32_t pc, const ReadRegisterFn& readReg,
                                                 const exec::Readers& readers)
{
    Decision d;
    auto it = std::find_if(m_rules.begin(), m_rules.end(),
                           [pc](const DosBreakRule& r) { return r.entryPc == pc; });
    if (it == m_rules.end())
        return d; // not one of ours — leave the stop alone

    d.ours = true;
    d.ruleId = it->id;
    ++it->hits;

    if (!it->nameMatch.empty()) {
        uint32_t raw = 0;
        if (!readReg || !readReg(it->argReg, raw)) {
            // Without the register there is nothing to test; stopping is the
            // safe answer, since silently resuming would hide the call.
            ++it->breaks;
            return d;
        }
        LvoArgSpec spec;
        spec.reg = it->argReg;
        spec.name = "name";
        spec.type = it->argType;
        d.value = LibraryCallTracer::DecodeValue(spec, raw, readers).text;
        it->lastValue = d.value;
        if (!Matches(d.value, it->nameMatch, it->match, it->caseSensitive)) {
            d.resume = true;
            return d;
        }
    }

    ++it->breaks;
    if (it->oneShot)
        m_rules.erase(it);
    return d;
}

} // namespace adm::core
