#include "LogView.h"
#include <imgui.h>
#include <imgui_internal.h>
#include "../FontRoles.h"
#include "../IconsFontAwesome6.h"

namespace adm {
namespace ui {

LogView::LogView() = default;
LogView::~LogView() = default;

void LogView::Open() { m_isOpen = true; }

void LogView::Restore()
{
    m_isOpen = true;
    m_noFocusOnAppearing = true;
}

void LogView::Close() { m_isOpen = false; }

void LogView::AddEntry(core::LogLevel level, const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_entries.size() >= kMaxEntries)
        m_entries.pop_front();
    m_entries.push_back({level, message});
    ++m_version;
}

void LogView::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    ++m_version;
}

void LogView::Draw()
{
    if (!m_isOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(600, 300), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }

    if (!ImGui::Begin(ICON_FA_FILE_LINES " Log###Log", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }

    // Toolbar row
    if (ImGui::Button("Clear")) {
        Clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    // Filter toggles with colored labels
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
    ImGui::Checkbox("Info", &m_showInfo);
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
    ImGui::Checkbox("Warn", &m_showWarnings);
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
    ImGui::Checkbox("Error", &m_showErrors);
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
    ImGui::Checkbox("Debug", &m_showDebug);
    ImGui::PopStyleColor();

    ImGui::Separator();

    // Log content
    PushFontRole(FontRole::Console);
    if (ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        // The lock also guards m_filteredIndices' referents. The clipper keeps the held-lock
        // work at O(entries) integer filtering + O(visible rows) text — the previous
        // render-everything loop blocked Logger callers on other threads for the whole child.
        std::lock_guard<std::mutex> lock(m_mutex);

        const uint32_t filterKey = (m_showInfo ? 1u : 0u) | (m_showWarnings ? 2u : 0u) |
                                   (m_showErrors ? 4u : 0u) | (m_showDebug ? 8u : 0u);
        if (m_version != m_filteredVersion || filterKey != m_filteredKey) {
            m_filteredVersion = m_version;
            m_filteredKey = filterKey;
            m_filteredIndices.clear();
            for (size_t i = 0; i < m_entries.size(); ++i) {
                switch (m_entries[i].level) {
                    case core::LogLevel::Info:
                        if (!m_showInfo)
                            continue;
                        break;
                    case core::LogLevel::Warning:
                        if (!m_showWarnings)
                            continue;
                        break;
                    case core::LogLevel::Error:
                        if (!m_showErrors)
                            continue;
                        break;
                    case core::LogLevel::Debug:
                        if (!m_showDebug)
                            continue;
                        break;
                }
                m_filteredIndices.push_back(static_cast<uint32_t>(i));
            }
        }

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_filteredIndices.size()));
        while (clipper.Step()) {
            for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
                const auto& entry = m_entries[m_filteredIndices[static_cast<size_t>(n)]];
                ImVec4 color;
                const char* prefix;
                switch (entry.level) {
                    case core::LogLevel::Info:
                        color = ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
                        prefix = "[INFO]  ";
                        break;
                    case core::LogLevel::Warning:
                        color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
                        prefix = "[WARN]  ";
                        break;
                    case core::LogLevel::Error:
                        color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                        prefix = "[ERROR] ";
                        break;
                    case core::LogLevel::Debug:
                    default:
                        color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                        prefix = "[DEBUG] ";
                        break;
                }

                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(prefix);
                ImGui::SameLine();
                ImGui::TextUnformatted(entry.message.c_str());
                ImGui::PopStyleColor();
            }
        }

        ImGui::PopStyleVar();

        if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    PopFontRole();

    ImGui::End();
}

core::UiLogSnapshot LogView::Snapshot(int limit, const std::string& levelFilter)
{
    auto levelName = [](core::LogLevel l) -> const char* {
        switch (l) {
            case core::LogLevel::Warning:
                return "Warning";
            case core::LogLevel::Error:
                return "Error";
            case core::LogLevel::Debug:
                return "Debug";
            case core::LogLevel::Info:
            default:
                return "Info";
        }
    };

    std::lock_guard<std::mutex> lock(m_mutex);
    core::UiLogSnapshot snap;
    snap.available = true;

    std::vector<const LogEntry*> matched;
    matched.reserve(m_entries.size());
    for (const auto& e : m_entries)
        if (levelFilter.empty() || levelFilter == levelName(e.level))
            matched.push_back(&e);

    snap.total = static_cast<int>(matched.size());
    size_t first = 0;
    if (limit > 0 && matched.size() > static_cast<size_t>(limit)) {
        first = matched.size() - static_cast<size_t>(limit);
        snap.truncated = true;
    }
    for (size_t i = first; i < matched.size(); ++i)
        snap.entries.push_back({levelName(matched[i]->level), matched[i]->message});
    return snap;
}

} // namespace ui
} // namespace adm
