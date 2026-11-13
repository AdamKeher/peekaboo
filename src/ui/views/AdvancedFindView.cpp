#include "AdvancedFindView.h"
#include <fmt/core.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <chrono>
#include "../IconsFontAwesome6.h"
#include "../TableLayout.h"

namespace adm::ui {

AdvancedFindView::AdvancedFindView() {}

void AdvancedFindView::SetDiskSearcher(core::DiskSearcher* searcher) { m_diskSearcher = searcher; }

void AdvancedFindView::SetMemorySearcher(core::MemorySearcher* searcher)
{
    m_memorySearcher = searcher;
}

void AdvancedFindView::SetFocus() { m_focusFrames = 2; }

void AdvancedFindView::SetOnNavigate(
    std::function<void(const core::SearchResult&, const std::vector<uint8_t>& pattern,
                       bool caseSensitive, core::DiskSearcher::SearchType type)>
        callback)
{
    m_onNavigate = callback;
}

void AdvancedFindView::SetOnNavigateMemory(
    std::function<void(const core::MemorySearchResult&)> callback)
{
    m_onNavigateMemory = callback;
}

void AdvancedFindView::SetOnSaveData(
    std::function<void(const std::string& filename, const core::SearchResult& result)> callback)
{
    m_onSaveData = callback;
}

void AdvancedFindView::SetOnListen(
    std::function<void(const std::string& filename, const core::SearchResult& result)> callback)
{
    m_onListen = callback;
}

void AdvancedFindView::SetOnSaveMemoryData(
    std::function<void(const std::string& filename, const core::MemorySearchResult& result)>
        callback)
{
    m_onSaveMemoryData = callback;
}

void AdvancedFindView::SetOnListenMemory(
    std::function<void(const std::string& filename, const core::MemorySearchResult& result)>
        callback)
{
    m_onListenMemory = callback;
}

void AdvancedFindView::SetDocumentId(uint32_t docId) { m_currentDocId = docId; }

void AdvancedFindView::Clear()
{
    m_allResults.clear();
    m_allStatusMessages.clear();
    m_allMemoryResults.clear();
    m_allMemoryStatusMessages.clear();
    m_searchPending = false;
}

void AdvancedFindView::PerformSearch()
{
    // Validate input and build pattern
    std::vector<uint8_t> pattern;
    bool isMod = (m_searchType == core::DiskSearcher::SearchType::MOD);

    if (!isMod) {
        if (m_searchType == core::DiskSearcher::SearchType::ASCII) {
            if (m_searchAscii.empty()) {
                if (m_searchTarget == SearchTarget::ADF)
                    m_allStatusMessages[m_currentDocId][m_searchType] = "Please enter search text.";
                else
                    m_allMemoryStatusMessages[m_searchType] = "Please enter search text.";
                return;
            }
            pattern.assign(m_searchAscii.begin(), m_searchAscii.end());
        } else {
            if (m_searchHex.empty()) {
                if (m_searchTarget == SearchTarget::ADF)
                    m_allStatusMessages[m_currentDocId][m_searchType] = "Please enter hex string.";
                else
                    m_allMemoryStatusMessages[m_searchType] = "Please enter hex string.";
                return;
            }
            pattern = core::DiskSearcher::ParseHex(m_searchHex);
            if (pattern.empty()) {
                if (m_searchTarget == SearchTarget::ADF)
                    m_allStatusMessages[m_currentDocId][m_searchType] = "Invalid hex string.";
                else
                    m_allMemoryStatusMessages[m_searchType] = "Invalid hex string.";
                return;
            }
        }
    }

    // Queue the scan; RunPendingSearch() executes it next frame so the spinner shows.
    m_pending.target = m_searchTarget;
    m_pending.type = m_searchType;
    m_pending.pattern = std::move(pattern);
    m_pending.caseSensitive = m_isCaseSensitive;
    m_pending.docId = m_currentDocId;
    m_searchPending = true;

    if (m_searchTarget == SearchTarget::ADF)
        m_allStatusMessages[m_currentDocId][m_searchType] = "Searching...";
    else
        m_allMemoryStatusMessages[m_searchType] = "Searching...";
}

void AdvancedFindView::RunPendingSearch()
{
    // Collect a finished worker scan first.
    if (m_searchRunning) {
        if (m_searchFuture.valid() &&
            m_searchFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            SearchOutcome out = m_searchFuture.get();
            m_searchRunning = false;
            const PendingSearch& p = m_runningParams;
            if (p.target == SearchTarget::ADF) {
                auto& results = m_allResults[p.docId][p.type];
                results = std::move(out.disk);
                m_allStatusMessages[p.docId][p.type] =
                    fmt::format("Found {} matches.", results.size());
            } else {
                auto& results = m_allMemoryResults[p.type];
                results = std::move(out.mem);
                m_allMemoryStatusMessages[p.type] =
                    fmt::format("Found {} matches.", results.size());
            }
        }
        return; // one scan at a time
    }

    if (!m_searchPending)
        return;
    m_searchPending = false;
    const PendingSearch& p = m_pending;

    if (p.target == SearchTarget::ADF) {
        if (!m_diskSearcher)
            return;
        if (p.type == core::DiskSearcher::SearchType::MOD) {
            // The filesystem-walking MOD search reads the live volume — synchronous.
            auto& results = m_allResults[p.docId][p.type];
            results = m_diskSearcher->FindAll(p.pattern, p.caseSensitive, p.type);
            m_allStatusMessages[p.docId][p.type] = fmt::format("Found {} matches.", results.size());
            return;
        }
        // Snapshot on the UI thread (adflib callbacks aren't thread-safe), scan on a worker.
        auto blocks =
            std::make_shared<std::vector<std::vector<uint8_t>>>(m_diskSearcher->SnapshotBlocks());
        const uint32_t totalBlocks = m_diskSearcher->TotalBlocks();
        m_runningParams = p;
        m_searchRunning = true;
        m_searchFuture = std::async(std::launch::async, [blocks, totalBlocks, p]() {
            core::DiskSearcher local;
            local.SetDiskInfo(totalBlocks, [blocks](uint32_t b) {
                return b < blocks->size() ? (*blocks)[b] : std::vector<uint8_t>{};
            });
            SearchOutcome out;
            out.disk = local.FindAll(p.pattern, p.caseSensitive, p.type);
            return out;
        });
    } else {
        if (!m_memorySearcher || !m_memorySearcher->HasRegions()) {
            m_allMemoryStatusMessages[p.type] = "No emulator memory available.";
            return;
        }
        // Copy the region bytes so the worker never dereferences live emulator memory —
        // the core could be stopped or reloaded mid-scan.
        auto buffers = std::make_shared<std::vector<std::vector<uint8_t>>>();
        std::vector<core::MemorySearcher::Region> snapRegions;
        for (const auto& r : m_memorySearcher->Regions()) {
            if (!r.data || r.size == 0)
                continue;
            buffers->emplace_back(r.data, r.data + r.size);
            snapRegions.push_back({buffers->back().data(), r.baseAddress, r.size, r.name});
        }
        m_runningParams = p;
        m_searchRunning = true;
        m_searchFuture =
            std::async(std::launch::async, [buffers, snapRegions = std::move(snapRegions), p]() {
                core::MemorySearcher local;
                local.SetRegions(snapRegions);
                SearchOutcome out;
                out.mem = local.FindAll(p.pattern, p.caseSensitive, p.type);
                return out;
            });
    }
}

void AdvancedFindView::Draw(bool* p_open)
{
    if (!p_open || !*p_open)
        return;

    // Launch a queued scan / collect a finished one. ASCII/Hex scans run on a worker thread
    // over a snapshot; only the snapshot copy itself happens here on the UI thread.
    RunPendingSearch();

    ImGui::SetNextWindowSize(ImVec2(660, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(ICON_FA_MAGNIFYING_GLASS " Advanced Find###Advanced Find", p_open)) {
        // ── Search Target ─────────────────────────────────────────────────────────
        bool emuRunning = (m_memorySearcher && m_memorySearcher->HasRegions());
        bool switch_to_adf = false;

        if (!emuRunning && m_searchTarget == SearchTarget::Memory) {
            m_searchTarget = SearchTarget::ADF;
            switch_to_adf = true;
        }

        if (ImGui::BeginTabBar("SearchTargetTabs")) {
            ImGuiTabItemFlags adfFlags = switch_to_adf ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("ADF Disk", nullptr, adfFlags)) {
                m_searchTarget = SearchTarget::ADF;
                ImGui::EndTabItem();
            }

            if (!emuRunning)
                ImGui::BeginDisabled();
            if (ImGui::BeginTabItem("Emulator Memory")) {
                m_searchTarget = SearchTarget::Memory;
                ImGui::EndTabItem();
            }
            if (!emuRunning)
                ImGui::EndDisabled();

            ImGui::EndTabBar();
        }

        ImGui::Separator();

        // ── Search Type ───────────────────────────────────────────────────────────
        ImGui::Text("Search Type:");
        ImGui::SameLine();
        int searchType = (int)m_searchType;
        if (ImGui::RadioButton("ASCII", &searchType, (int)core::DiskSearcher::SearchType::ASCII))
            m_searchType = core::DiskSearcher::SearchType::ASCII;
        ImGui::SameLine();
        if (ImGui::RadioButton("Hex", &searchType, (int)core::DiskSearcher::SearchType::Hex))
            m_searchType = core::DiskSearcher::SearchType::Hex;
        ImGui::SameLine();
        if (ImGui::RadioButton("MOD File", &searchType, (int)core::DiskSearcher::SearchType::MOD))
            m_searchType = core::DiskSearcher::SearchType::MOD;

        ImGuiInputTextFlags inputFlags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
        bool triggerSearch = false;

        bool isMod = (m_searchType == core::DiskSearcher::SearchType::MOD);

        // Force focus for a few frames when window first opens
        if (m_focusFrames > 0) {
            ImGui::SetWindowFocus();
            if (!isMod)
                ImGui::SetKeyboardFocusHere(0);
            m_focusFrames--;
        }

        ImGui::BeginDisabled(isMod);
        if (m_searchType == core::DiskSearcher::SearchType::ASCII || isMod) {
            if (ImGui::InputText("Text##ascii", &m_searchAscii, inputFlags))
                triggerSearch = true;
        } else {
            if (ImGui::InputText("Hex##hex", &m_searchHex, inputFlags))
                triggerSearch = true;
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(m_searchType == core::DiskSearcher::SearchType::Hex || isMod);
        ImGui::Checkbox("Case Sensitive", &m_isCaseSensitive);
        ImGui::EndDisabled();

        ImGui::Separator();

        ImGui::BeginDisabled(m_searchRunning); // one scan at a time
        if ((ImGui::Button(ICON_FA_MAGNIFYING_GLASS " Find All") || triggerSearch) &&
            !m_searchRunning)
            PerformSearch();
        ImGui::EndDisabled();

        ImGui::SameLine();
        const std::string& statusMsg = (m_searchTarget == SearchTarget::ADF)
                                           ? m_allStatusMessages[m_currentDocId][m_searchType]
                                           : m_allMemoryStatusMessages[m_searchType];
        ImGui::Text("%s", statusMsg.c_str());

        ImGui::Separator();

        // ── Results Table ─────────────────────────────────────────────────────────
        if (m_searchTarget == SearchTarget::ADF) {
            // 6-column ADF results table
            if (ImGui::BeginTable("ResultsTable", 6,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | kAdjustableColumns)) {
                ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("End Block", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableHeadersRow();

                const auto& currentResults = m_allResults[m_currentDocId][m_searchType];
                // Clipped: "Find All" on a full disk returns thousands of rows, and every row
                // built several fmt::format temporaries. PushID(row) replaces the
                // pointer-suffixed labels.
                ImGuiListClipper resultsClipper;
                resultsClipper.Begin(static_cast<int>(currentResults.size()));
                while (resultsClipper.Step())
                    for (int ri = resultsClipper.DisplayStart; ri < resultsClipper.DisplayEnd;
                         ++ri) {
                        const auto& result = currentResults[static_cast<size_t>(ri)];
                        ImGui::PushID(ri);
                        ImGui::TableNextRow();

                        auto DrawCell = [&](int col, const std::string& text) {
                            ImGui::TableSetColumnIndex(col);
                            if (col == 0) {
                                std::string label = fmt::format("{}##sel", text);
                                if (ImGui::Selectable(label.c_str(), false,
                                                      ImGuiSelectableFlags_SpanAllColumns |
                                                          ImGuiSelectableFlags_AllowOverlap)) {
                                    if (m_onNavigate) {
                                        std::vector<uint8_t> pattern;
                                        if (m_searchType == core::DiskSearcher::SearchType::ASCII)
                                            pattern.assign(m_searchAscii.begin(),
                                                           m_searchAscii.end());
                                        else if (m_searchType ==
                                                 core::DiskSearcher::SearchType::Hex)
                                            pattern = core::DiskSearcher::ParseHex(m_searchHex);
                                        m_onNavigate(result, pattern, m_isCaseSensitive,
                                                     m_searchType);
                                    }
                                }
                            } else {
                                ImGui::TextUnformatted(text.c_str());
                            }
                        };

                        uint64_t startGlobal = (uint64_t)result.block * 512 + result.offset;
                        uint64_t endGlobal = startGlobal + result.length - 1;
                        uint32_t endBlock = (uint32_t)(endGlobal / 512);

                        std::string sizeStr;
                        if (result.length > 1024)
                            sizeStr = fmt::format("{:.1f} KB", result.length / 1024.0f);
                        else
                            sizeStr = fmt::format("{} B", result.length);

                        DrawCell(0, std::to_string(result.block));
                        DrawCell(1, std::to_string(endBlock));
                        DrawCell(2, fmt::format("0x{:X}", result.offset));
                        DrawCell(3, sizeStr);
                        DrawCell(4, result.context);

                        ImGui::TableSetColumnIndex(5);
                        if (m_searchType == core::DiskSearcher::SearchType::MOD) {
                            if (ImGui::Button(ICON_FA_FLOPPY_DISK "##save")) {
                                if (m_onSaveData) {
                                    std::string filename;
                                    if (result.fromFilesystem) {
                                        filename = result.context;
                                        std::replace(filename.begin(), filename.end(), '/', '_');
                                        std::replace(filename.begin(), filename.end(), '\\', '_');
                                    } else {
                                        filename =
                                            result.context.empty() ? "module" : result.context;
                                        std::replace(filename.begin(), filename.end(), '/', '_');
                                        std::replace(filename.begin(), filename.end(), '\\', '_');
                                        filename += ".mod";
                                    }
                                    m_onSaveData(filename, result);
                                }
                            }
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Save");

                            ImGui::SameLine();
                            if (ImGui::Button(ICON_FA_PLAY "##listen")) {
                                if (m_onListen) {
                                    std::string filename =
                                        result.context.empty() ? "module.mod" : result.context;
                                    m_onListen(filename, result);
                                }
                            }
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Listen");
                        }
                        ImGui::PopID();
                    }

                ImGui::EndTable();
            }
        } else {
            // Memory results table: 5 columns for MOD (adds Action column), 4 for others
            bool isMemMod = (m_searchType == core::DiskSearcher::SearchType::MOD);
            int numCols = isMemMod ? 5 : 4;
            if (ImGui::BeginTable("MemResultsTable", numCols,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | kAdjustableColumns)) {
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthStretch);
                if (isMemMod)
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableHeadersRow();

                const auto& memResults = m_allMemoryResults[m_searchType];
                // Clipped for the same reason as the ADF table above.
                ImGuiListClipper memClipper;
                memClipper.Begin(static_cast<int>(memResults.size()));
                while (memClipper.Step())
                    for (int ri = memClipper.DisplayStart; ri < memClipper.DisplayEnd; ++ri) {
                        const auto& result = memResults[static_cast<size_t>(ri)];
                        ImGui::PushID(ri);
                        ImGui::TableNextRow();

                        auto DrawMemCell = [&](int col, const std::string& text) {
                            ImGui::TableSetColumnIndex(col);
                            if (col == 0) {
                                std::string label = fmt::format("{}##msel", text);
                                if (ImGui::Selectable(label.c_str(), false,
                                                      ImGuiSelectableFlags_SpanAllColumns |
                                                          ImGuiSelectableFlags_AllowOverlap)) {
                                    if (m_onNavigateMemory)
                                        m_onNavigateMemory(result);
                                }
                            } else {
                                ImGui::TextUnformatted(text.c_str());
                            }
                        };

                        std::string lenStr;
                        if (result.length > 1024)
                            lenStr = fmt::format("{:.1f} KB", result.length / 1024.0f);
                        else
                            lenStr = fmt::format("{} B", result.length);

                        DrawMemCell(0, fmt::format("${:06X}", result.address));
                        DrawMemCell(1, lenStr);
                        DrawMemCell(2, result.regionName);
                        DrawMemCell(3, result.context);

                        if (isMemMod) {
                            ImGui::TableSetColumnIndex(4);
                            if (ImGui::Button(ICON_FA_FLOPPY_DISK "##msave")) {
                                if (m_onSaveMemoryData) {
                                    std::string filename =
                                        result.context.empty() ? "module" : result.context;
                                    std::replace(filename.begin(), filename.end(), '/', '_');
                                    std::replace(filename.begin(), filename.end(), '\\', '_');
                                    filename += ".mod";
                                    m_onSaveMemoryData(filename, result);
                                }
                            }
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Save");
                            ImGui::SameLine();
                            if (ImGui::Button(ICON_FA_PLAY "##mlisten")) {
                                if (m_onListenMemory) {
                                    std::string filename =
                                        result.context.empty() ? "module.mod" : result.context;
                                    m_onListenMemory(filename, result);
                                }
                            }
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Listen");
                        }
                        ImGui::PopID();
                    }

                ImGui::EndTable();
            }
        }
    }
    ImGui::End();
}

} // namespace adm::ui
