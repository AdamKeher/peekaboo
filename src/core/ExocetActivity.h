#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace adm::core {

// Which of a production's groups the demo had enabled on each played row:
// what the running demo did, recorded from the block every frame, so the
// Exocet window can show a part's tracks only while that part runs. Kept by group name, for one
// song (its digest and its played rows); a row the demo never played is Unknown.
class ExocetActivity
{
public:
    enum class State { Unknown, Active, Inactive };

    // For a song of `rows` played rows: what is recorded stays when it is the same song, and is
    // dropped when it is not.
    void SetSong(uint32_t digest, int rows);
    uint32_t Song() const { return m_song; }
    int Rows() const { return m_rows; }

    // Row `row` played with `enabled` (bit g: `groups[g]` evaluated).
    void Mark(int row, uint32_t enabled, const std::vector<std::string>& groups);
    void Clear();

    State At(const std::string& group, int row) const;
    bool Seen(int row) const;
    int SeenRows() const { return m_seenCount; }
    // Rows `group` was recorded active on, and those rows as [first, last] spans.
    int ActiveRows(const std::string& group) const;
    std::vector<std::pair<int, int>> ActiveSpans(const std::string& group) const;
    std::vector<std::pair<int, int>> SeenSpans() const;
    std::vector<std::string> GroupNames() const;

    // Bumped by every change, for the saver.
    uint64_t Revision() const { return m_revision; }

    // {"song": "0x...", "rows": n, "seen": [[a, b], ...], "groups": {"lst": [[a, b], ...]}}
    nlohmann::json ToJson() const;
    // False (and nothing taken) for a record of another song or a malformed one.
    bool FromJson(const nlohmann::json& j, uint32_t digest, int rows);

private:
    uint32_t m_song = 0;
    int m_rows = 0;
    std::vector<uint8_t> m_seen;
    int m_seenCount = 0;
    std::map<std::string, std::vector<uint8_t>> m_active;
    std::map<std::string, int> m_activeCount;
    uint64_t m_revision = 0;
};

} // namespace adm::core
