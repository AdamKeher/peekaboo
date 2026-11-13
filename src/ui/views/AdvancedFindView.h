#pragma once

#include <functional>
#include <future>
#include <map>
#include <string>
#include <vector>
#include "../../core/DiskSearcher.h"
#include "../../core/MemorySearcher.h"

namespace adm::ui {

class AdvancedFindView
{
public:
    enum class SearchTarget { ADF, Memory };

    AdvancedFindView();

    void Draw(bool* p_open);
    void SetFocus();

    void SetDiskSearcher(core::DiskSearcher* searcher);
    void SetMemorySearcher(core::MemorySearcher* searcher);

    void
    SetOnNavigate(std::function<void(const core::SearchResult&, const std::vector<uint8_t>& pattern,
                                     bool caseSensitive, core::DiskSearcher::SearchType type)>
                      callback);

    void SetOnNavigateMemory(std::function<void(const core::MemorySearchResult&)> callback);

    void
    SetOnSaveData(std::function<void(const std::string& filename, const core::SearchResult& result)>
                      callback);

    void
    SetOnListen(std::function<void(const std::string& filename, const core::SearchResult& result)>
                    callback);

    // Callbacks for memory MOD results (save to file / listen in audio player)
    void SetOnSaveMemoryData(
        std::function<void(const std::string& filename, const core::MemorySearchResult& result)>
            callback);
    void SetOnListenMemory(
        std::function<void(const std::string& filename, const core::MemorySearchResult& result)>
            callback);

    void SetDocumentId(uint32_t docId);

    void Clear();

private:
    core::DiskSearcher* m_diskSearcher = nullptr;
    core::MemorySearcher* m_memorySearcher = nullptr;

    std::function<void(const core::SearchResult&, const std::vector<uint8_t>& pattern,
                       bool caseSensitive, core::DiskSearcher::SearchType type)>
        m_onNavigate;
    std::function<void(const core::MemorySearchResult&)> m_onNavigateMemory;
    std::function<void(const std::string& filename, const core::SearchResult& result)> m_onSaveData;
    std::function<void(const std::string& filename, const core::SearchResult& result)> m_onListen;
    std::function<void(const std::string& filename, const core::MemorySearchResult& result)>
        m_onSaveMemoryData;
    std::function<void(const std::string& filename, const core::MemorySearchResult& result)>
        m_onListenMemory;

    // Search target
    SearchTarget m_searchTarget = SearchTarget::ADF;

    // Search State
    core::DiskSearcher::SearchType m_searchType = core::DiskSearcher::SearchType::ASCII;
    std::string m_searchAscii;
    std::string m_searchHex;
    bool m_isCaseSensitive = false;
    uint32_t m_currentDocId = 0;

    // ADF results per document
    // docId -> { SearchType -> Results }
    std::map<uint32_t, std::map<core::DiskSearcher::SearchType, std::vector<core::SearchResult>>>
        m_allResults;
    std::map<uint32_t, std::map<core::DiskSearcher::SearchType, std::string>> m_allStatusMessages;

    // Memory results (separate set, global to emulator)
    std::map<core::DiskSearcher::SearchType, std::vector<core::MemorySearchResult>>
        m_allMemoryResults;
    std::map<core::DiskSearcher::SearchType, std::string> m_allMemoryStatusMessages;

    int m_focusFrames = 0;

    // Deferred search: PerformSearch() validates and queues; RunPendingSearch() (start of
    // Draw) launches it. ASCII/Hex scans snapshot the disk blocks / memory regions on the UI
    // thread (the adflib callbacks are not safe to run concurrently), then scan the snapshot
    // on a worker thread and deliver through m_searchFuture — a full-disk or full-RAM scan
    // used to stall the UI for its whole duration. Filesystem-walking MOD searches still run
    // synchronously: they read the live volume.
    bool m_searchPending = false;
    struct PendingSearch
    {
        SearchTarget target = SearchTarget::ADF;
        core::DiskSearcher::SearchType type = core::DiskSearcher::SearchType::ASCII;
        std::vector<uint8_t> pattern;
        bool caseSensitive = false;
        uint32_t docId = 0;
    } m_pending;

    struct SearchOutcome
    {
        std::vector<core::SearchResult> disk;
        std::vector<core::MemorySearchResult> mem;
    };
    std::future<SearchOutcome> m_searchFuture;
    bool m_searchRunning = false;  // a worker scan is in flight; Find is disabled meanwhile
    PendingSearch m_runningParams; // routes the worker's outcome to the right result slot

    void PerformSearch();
    void RunPendingSearch();
};

} // namespace adm::ui
