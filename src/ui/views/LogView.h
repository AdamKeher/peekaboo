#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "../../core/Logger.h"
#include "../../core/UiLayoutSnapshot.h"

namespace adm {
namespace ui {

class LogView
{
public:
    LogView();
    ~LogView();

    // Non-copyable
    LogView(const LogView&) = delete;
    LogView& operator=(const LogView&) = delete;

    void Open();
    void Restore();
    void Close();
    void Draw();

    [[nodiscard]] bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    void AddEntry(core::LogLevel level, const std::string& message);
    void Clear();

    // The most recent `limit` entries, oldest first, optionally restricted to one level. This is
    // the store behind the MCP get_ui_log tool - Logger itself only fans out to a callback and
    // keeps no history, so this view is the only place Peekaboo's own log is retained.
    core::UiLogSnapshot Snapshot(int limit, const std::string& levelFilter);

private:
    struct LogEntry
    {
        core::LogLevel level;
        std::string message;
    };

    // Bounded: an unbounded log grows for the whole session and every frame paid to walk it.
    // Oldest entries are dropped past the cap (the MCP snapshot tool already truncates anyway).
    static constexpr size_t kMaxEntries = 10000;
    std::deque<LogEntry> m_entries;
    uint64_t m_version = 0; // bumped by AddEntry/Clear; invalidates m_filteredIndices

    // Rebuilt (indices only) when the log or the filter toggles change; lets Draw use an
    // ImGuiListClipper so the mutex is held for O(entries) index work + O(visible) text, not
    // a full-text render of everything — Logger callers on other threads block on this lock.
    std::vector<uint32_t> m_filteredIndices;
    uint64_t m_filteredVersion = ~0ull;
    uint32_t m_filteredKey = ~0u;

    std::mutex m_mutex;
    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;
    bool m_autoScroll = true;
    bool m_showInfo = true;
    bool m_showWarnings = true;
    bool m_showErrors = true;
    bool m_showDebug = true;
};

} // namespace ui
} // namespace adm
