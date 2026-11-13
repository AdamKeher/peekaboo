#include "ExocetActivity.h"

#include <algorithm>
#include <cstdio>

#include "JsonRead.h"

namespace adm::core {
namespace {

std::vector<std::pair<int, int>> Spans(const std::vector<uint8_t>& bits)
{
    std::vector<std::pair<int, int>> out;
    const int n = static_cast<int>(bits.size());
    for (int i = 0; i < n;) {
        if (!bits[i]) {
            ++i;
            continue;
        }
        int j = i;
        while (j + 1 < n && bits[j + 1])
            ++j;
        out.emplace_back(i, j);
        i = j + 1;
    }
    return out;
}

nlohmann::json SpansJson(const std::vector<uint8_t>& bits)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [a, b] : Spans(bits))
        out.push_back({a, b});
    return out;
}

// Spans into `bits`; false for a malformed list
bool FromSpans(const nlohmann::json& j, std::vector<uint8_t>& bits, int& count)
{
    if (!j.is_array())
        return false;
    for (const auto& s : j) {
        if (!s.is_array() || s.size() != 2 || !s[0].is_number_integer() ||
            !s[1].is_number_integer())
            return false;
        const int a = s[0].get<int>(), b = s[1].get<int>();
        if (a < 0 || b < a || b >= static_cast<int>(bits.size()))
            return false;
        for (int i = a; i <= b; ++i)
            if (!bits[i]) {
                bits[i] = 1;
                ++count;
            }
    }
    return true;
}

} // namespace

void ExocetActivity::SetSong(uint32_t digest, int rows)
{
    if (digest == m_song && rows == m_rows)
        return;
    m_song = digest;
    m_rows = std::max(0, rows);
    Clear();
}

void ExocetActivity::Clear()
{
    m_seen.assign(m_rows, 0);
    m_seenCount = 0;
    m_active.clear();
    m_activeCount.clear();
    ++m_revision;
}

void ExocetActivity::Mark(int row, uint32_t enabled, const std::vector<std::string>& groups)
{
    if (row < 0 || row >= m_rows)
        return;
    bool changed = false;
    if (!m_seen[row]) {
        m_seen[row] = 1;
        ++m_seenCount;
        changed = true;
    }
    for (size_t g = 0; g < groups.size() && g < 32; ++g) {
        if (!(enabled & (1u << g)))
            continue;
        auto& bits = m_active[groups[g]];
        if (bits.size() != static_cast<size_t>(m_rows))
            bits.assign(m_rows, 0);
        if (!bits[row]) {
            bits[row] = 1;
            ++m_activeCount[groups[g]];
            changed = true;
        }
    }
    if (changed)
        ++m_revision;
}

ExocetActivity::State ExocetActivity::At(const std::string& group, int row) const
{
    if (!Seen(row))
        return State::Unknown;
    auto it = m_active.find(group);
    return it != m_active.end() && it->second[row] ? State::Active : State::Inactive;
}

bool ExocetActivity::Seen(int row) const { return row >= 0 && row < m_rows && m_seen[row]; }

int ExocetActivity::ActiveRows(const std::string& group) const
{
    auto it = m_activeCount.find(group);
    return it == m_activeCount.end() ? 0 : it->second;
}

std::vector<std::pair<int, int>> ExocetActivity::ActiveSpans(const std::string& group) const
{
    auto it = m_active.find(group);
    return it == m_active.end() ? std::vector<std::pair<int, int>>{} : Spans(it->second);
}

std::vector<std::pair<int, int>> ExocetActivity::SeenSpans() const { return Spans(m_seen); }

std::vector<std::string> ExocetActivity::GroupNames() const
{
    std::vector<std::string> out;
    for (const auto& [name, bits] : m_active)
        out.push_back(name);
    return out;
}

nlohmann::json ExocetActivity::ToJson() const
{
    char song[16];
    std::snprintf(song, sizeof song, "0x%08x", m_song);
    nlohmann::json groups = nlohmann::json::object();
    for (const auto& [name, bits] : m_active)
        groups[name] = SpansJson(bits);
    return {{"song", song}, {"rows", m_rows}, {"seen", SpansJson(m_seen)}, {"groups", groups}};
}

bool ExocetActivity::FromJson(const nlohmann::json& j, uint32_t digest, int rows)
{
    if (!j.is_object() || !j.contains("song") || !j["song"].is_string() ||
        adm::core::jsonread::ReadOr(j, "rows", -1) != rows)
        return false;
    char want[16];
    std::snprintf(want, sizeof want, "0x%08x", digest);
    if (j["song"].get<std::string>() != want)
        return false;
    ExocetActivity a;
    a.m_song = digest;
    a.m_rows = rows;
    a.m_seen.assign(rows, 0);
    if (!FromSpans(j.value("seen", nlohmann::json::array()), a.m_seen, a.m_seenCount))
        return false;
    if (auto g = j.find("groups"); g != j.end()) {
        if (!g->is_object())
            return false;
        for (const auto& [name, spans] : g->items()) {
            auto& bits = a.m_active[name];
            bits.assign(rows, 0);
            if (!FromSpans(spans, bits, a.m_activeCount[name]))
                return false;
        }
    }
    a.m_revision = m_revision + 1;
    *this = std::move(a);
    return true;
}

} // namespace adm::core
