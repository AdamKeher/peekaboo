#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "../../core/DiskIdentifier.h"
#include "../../core/DownloadManager.h"
#include "../../core/TosecManager.h"
#include "../TextureManager.h"
#include "IdentifyResultPanel.h"

namespace adm::ui {

class TosecBrowserView
{
public:
    explicit TosecBrowserView(TextureManager* textureMgr = nullptr);
    ~TosecBrowserView();

    void Open();
    void Close();
    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    // Called when the user clicks Download; the application opens a placeholder
    // document and runs the download in the background (progress shown in the tab).
    void SetOnStartDownload(std::function<void(core::DownloadRequest)> cb)
    {
        m_onStartDownload = std::move(cb);
    }

    // Open the browser and select the entry matching the given MD5, clearing filters first
    void SelectEntry(const std::string& md5);

    // Force the detail panel to re-check the cache for the currently selected entry on the next
    // draw (call this after external identification completes for the same title group).
    void RefreshPanel() { m_panelLastMd5.clear(); }

    void Draw();

private:
    void BuildFilterOptions();
    void ApplyFilters();
    void DrawFilterPanel();
    void DrawResultsTable();
    void DrawDetailPanel();
    static std::string ToLowerStr(const std::string& s);
    static int ParseTotalDisks(const std::vector<std::string>& media);
    static std::string SanitizeFilename(const std::string& name);

    bool m_isOpen = false;

    // ── Filter state ──────────────────────────────────────────────────────────
    char m_searchBuf[256]{};
    char m_publisherBuf[128]{};
    char m_yearMinBuf[8]{};
    char m_yearMaxBuf[8]{};
    int m_categoryIdx = 0;
    int m_languageIdx = 0;
    int m_regionIdx = 0;
    int m_systemIdx = 0;
    int m_videoIdx = 0;
    int m_diskCountIdx = 0; // 0=Any, 1=1 disk, 2=2, 3=3, 4=4, 5=5+
    bool m_filterVerifiedOnly = false;
    bool m_filterDemoOnly = false;
    bool m_filterHasDownload = false;
    bool m_filterCracked = false;
    bool m_filterTrained = false;
    bool m_filterHacked = false;
    bool m_filterModified = false;
    bool m_hideBadDumps = true;
    bool m_hideVirus = true;
    bool m_hideOverdumps = false;
    bool m_hideAlternate = false;

    // ── Scene group filter ────────────────────────────────────────────────────
    char m_groupSearchBuf[128]{};
    int m_groupTypeIdx = 0; // 0=All, 1=Crack, 2=Trainer, 3=Hack, 4=Mod
    std::set<std::string> m_selectedGroups;
    std::set<std::string> m_activeGroups; // groups present in pre-group filtered results

    // ── Filter options (unique values from all entries) ───────────────────────
    std::vector<std::string> m_categories;
    std::vector<std::string> m_languages;
    std::vector<std::string> m_regions;
    std::vector<std::string> m_systems;
    std::vector<std::string> m_videos;
    std::vector<std::string> m_allGroups;
    std::vector<std::string> m_crackGroups;
    std::vector<std::string> m_trainerGroups;
    std::vector<std::string> m_hackGroups;
    std::vector<std::string> m_modGroups;
    bool m_optionsBuilt = false;

    // ── Filtered results ──────────────────────────────────────────────────────
    std::vector<const core::TosecEntry*> m_filtered;

    // Lowercased text per entry, aligned with TosecManager::GetAllEntries() order. Built once
    // per database load so filter keystrokes don't lowercase four fields per entry.
    struct LoweredEntry
    {
        std::string searchBlob; // title '\n' romName '\n' publisher '\n' releaseGroup
        std::string publisher;
    };
    std::vector<LoweredEntry> m_lowerCache;
    bool m_filterDirty = true;
    int m_totalEntries = 0;

    // ── Sort state ────────────────────────────────────────────────────────────
    int m_sortColumn = 0;
    ImGuiSortDirection m_sortDirection = ImGuiSortDirection_Ascending;
    bool m_needsSort = true;

    // ── Table selection ───────────────────────────────────────────────────────
    int m_selectedIdx = -1;
    const core::TosecEntry* m_selectedEntry = nullptr;

    // ── Download ──────────────────────────────────────────────────────────────
    std::function<void(core::DownloadRequest)> m_onStartDownload;

    // Pending navigation request from SelectEntry()
    std::string m_pendingSelectMD5;
    bool m_scrollToSelected = false;

    // ── Disk identification panel (cache lookup) ───────────────────────────────
    std::unique_ptr<IdentifyResultPanel> m_identifyPanel;
    std::string m_panelLastMd5;

    // ── Layout ────────────────────────────────────────────────────────────────
    float m_tableSplitRatio = 0.55f; // fraction of right panel given to results table
};

} // namespace adm::ui
