#include "KprintfBreakpoints.h"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

namespace adm::core {

namespace {

std::string ToLower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Compiles a Regex rule's pattern. Returns null when the pattern does not compile — Add() treats
// that as a rejected rule rather than a rule that silently never fires.
std::shared_ptr<const std::regex> CompileRegex(const std::string& text, bool caseSensitive)
{
    auto flags = std::regex::ECMAScript | std::regex::optimize;
    if (!caseSensitive)
        flags |= std::regex::icase;
    try {
        return std::make_shared<const std::regex>(text, flags);
    } catch (const std::regex_error&) {
        return nullptr;
    }
}

bool LineMatches(const KprintfBreakpoint& e, const std::string& line)
{
    if (e.match == KprintfMatch::Regex) {
        if (!e.compiled)
            return false;
        try {
            return std::regex_search(line, *e.compiled);
        } catch (const std::regex_error&) {
            return false; // stack/complexity blow-up on a pathological pattern: never break
        }
    }

    const std::string hay = e.caseSensitive ? line : ToLower(line);
    const std::string needle = e.caseSensitive ? e.text : ToLower(e.text);
    switch (e.match) {
        case KprintfMatch::Exact:
            return hay == needle;
        case KprintfMatch::Prefix:
            return hay.rfind(needle, 0) == 0;
        case KprintfMatch::Contains:
        default:
            return hay.find(needle) != std::string::npos;
    }
}

} // namespace

const char* KprintfBreakpoints::MatchName(KprintfMatch m)
{
    switch (m) {
        case KprintfMatch::Exact:
            return "exact";
        case KprintfMatch::Prefix:
            return "prefix";
        case KprintfMatch::Regex:
            return "regex";
        case KprintfMatch::Contains:
        default:
            return "contains";
    }
}

std::optional<KprintfMatch> KprintfBreakpoints::ParseMatch(std::string_view name)
{
    std::string n = ToLower(std::string(name));
    if (n == "contains")
        return KprintfMatch::Contains;
    if (n == "exact")
        return KprintfMatch::Exact;
    if (n == "prefix" || n == "starts" || n == "startswith")
        return KprintfMatch::Prefix;
    if (n == "regex" || n == "re")
        return KprintfMatch::Regex;
    return std::nullopt;
}

int KprintfBreakpoints::Add(const std::string& text, KprintfMatch match, bool caseSensitive,
                            bool oneShot)
{
    if (text.empty())
        return 0;

    // One rule per (text, mode, case-sensitivity) triple — re-adding the same trap is a no-op
    // rather than a second rule that fires on the same line.
    for (const auto& e : m_entries) {
        if (e.text == text && e.match == match && e.caseSensitive == caseSensitive)
            return 0;
    }

    KprintfBreakpoint e;
    e.id = m_nextId++;
    e.text = text;
    e.match = match;
    e.caseSensitive = caseSensitive;
    e.oneShot = oneShot;
    if (match == KprintfMatch::Regex) {
        e.compiled = CompileRegex(text, caseSensitive);
        if (!e.compiled) {
            --m_nextId; // the id was never handed out
            return 0;
        }
    }
    m_entries.push_back(std::move(e));
    return m_entries.back().id;
}

bool KprintfBreakpoints::Remove(int id)
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(),
                           [id](const KprintfBreakpoint& e) { return e.id == id; });
    if (it == m_entries.end())
        return false;
    m_entries.erase(it);
    return true;
}

bool KprintfBreakpoints::RemoveByText(const std::string& text)
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(),
                           [&text](const KprintfBreakpoint& e) { return e.text == text; });
    if (it == m_entries.end())
        return false;
    m_entries.erase(it);
    return true;
}

void KprintfBreakpoints::Clear() { m_entries.clear(); }

bool KprintfBreakpoints::SetEnabled(int id, bool enabled)
{
    for (auto& e : m_entries) {
        if (e.id == id) {
            e.enabled = enabled;
            return true;
        }
    }
    return false;
}

void KprintfBreakpoints::ResetHits(int id)
{
    for (auto& e : m_entries) {
        if (id == 0 || e.id == id) {
            e.hits = 0;
            e.lastLine.clear();
        }
    }
}

const KprintfBreakpoint* KprintfBreakpoints::Find(int id) const
{
    for (const auto& e : m_entries) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

std::optional<KprintfHit> KprintfBreakpoints::MatchLine(const std::string& line)
{
    if (m_entries.empty() || line.empty())
        return std::nullopt;

    for (size_t i = 0; i < m_entries.size(); ++i) {
        KprintfBreakpoint& e = m_entries[i];
        if (!e.enabled || !LineMatches(e, line))
            continue;

        e.hits++;
        e.lastLine = line;

        KprintfHit hit;
        hit.id = e.id;
        hit.text = e.text;
        hit.line = line;
        hit.match = e.match;
        if (e.oneShot) {
            m_entries.erase(m_entries.begin() + static_cast<ptrdiff_t>(i));
            hit.id = 0; // the rule is gone; the banner still reports what it matched
        }
        return hit;
    }
    return std::nullopt;
}

nlohmann::json KprintfBreakpoints::Export() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : m_entries) {
        nlohmann::json node;
        node["text"] = e.text;
        node["match"] = MatchName(e.match);
        node["caseSensitive"] = e.caseSensitive;
        node["enabled"] = e.enabled;
        if (e.oneShot)
            node["oneShot"] = true;
        arr.push_back(std::move(node));
    }
    return arr;
}

void KprintfBreakpoints::Import(const nlohmann::json& j)
{
    Clear();
    if (!j.is_array())
        return;
    for (const auto& node : j) {
        if (!node.is_object())
            continue;
        const std::string text = node.value("text", std::string{});
        const KprintfMatch match = ParseMatch(node.value("match", std::string("contains")))
                                       .value_or(KprintfMatch::Contains);
        const bool caseSensitive = node.value("caseSensitive", false);
        const int id = Add(text, match, caseSensitive, node.value("oneShot", false));
        if (id != 0 && !node.value("enabled", true))
            SetEnabled(id, false);
    }
}

} // namespace adm::core
