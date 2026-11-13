#include "TosecBrowserView.h"
#include "../TableLayout.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

#include <fmt/format.h>
#include <imgui.h>

#include "../../core/DiskIdentifier.h"
#include "../../core/Logger.h"
#include "../IconsFontAwesome6.h"
#include "../utils/ImGuiWidgets.h"
#include "IdentifyResultPanel.h"

namespace adm::ui {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string TosecBrowserView::ToLowerStr(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

int TosecBrowserView::ParseTotalDisks(const std::vector<std::string>& media)
{
    for (const auto& m : media) {
        auto pos = m.find(" of ");
        if (pos != std::string::npos) {
            try {
                return std::stoi(m.substr(pos + 4));
            } catch (...) {
                // Unparseable number in TOSEC text — fall back to the default.
            }
        }
    }
    return media.empty() ? 1 : static_cast<int>(media.size());
}

std::string TosecBrowserView::SanitizeFilename(const std::string& name)
{
    std::string out;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            out += '_';
        else
            out += c;
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

TosecBrowserView::TosecBrowserView(TextureManager* textureMgr)
    : m_identifyPanel(std::make_unique<IdentifyResultPanel>(textureMgr))
{
}

TosecBrowserView::~TosecBrowserView() = default;

void TosecBrowserView::Open() { m_isOpen = true; }
void TosecBrowserView::Close() { m_isOpen = false; }

void TosecBrowserView::SelectEntry(const std::string& md5)
{
    // Clear all filters so the entry is guaranteed to be visible
    m_searchBuf[0] = '\0';
    m_publisherBuf[0] = '\0';
    m_yearMinBuf[0] = '\0';
    m_yearMaxBuf[0] = '\0';
    m_categoryIdx = 0;
    m_languageIdx = 0;
    m_regionIdx = 0;
    m_systemIdx = 0;
    m_videoIdx = 0;
    m_diskCountIdx = 0;
    m_filterVerifiedOnly = false;
    m_filterDemoOnly = false;
    m_filterHasDownload = false;
    m_filterCracked = false;
    m_filterTrained = false;
    m_filterHacked = false;
    m_filterModified = false;
    m_hideBadDumps = false;
    m_hideVirus = false;
    m_hideOverdumps = false;
    m_hideAlternate = false;
    m_groupSearchBuf[0] = '\0';
    m_groupTypeIdx = 0;
    m_selectedGroups.clear();

    m_pendingSelectMD5 = md5;
    m_filterDirty = true;
    Open();
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter option collection (runs once when DB becomes available)
// ─────────────────────────────────────────────────────────────────────────────

void TosecBrowserView::BuildFilterOptions()
{
    if (m_optionsBuilt || !core::TosecManager::Instance().IsLoaded())
        return;

    std::set<std::string> cats, langs, regions, systems, videos;
    std::set<std::string> crackGrps, trainerGrps, hackGrps, modGrps;
    for (const auto* e : core::TosecManager::Instance().GetAllEntries()) {
        if (!e->category.empty())
            cats.insert(e->category);
        if (!e->language.empty())
            langs.insert(e->language);
        if (!e->region.empty())
            regions.insert(e->region);
        if (!e->system.empty())
            systems.insert(e->system);
        if (!e->video.empty())
            videos.insert(e->video);
        if (e->cracked && !e->crackGroup.empty())
            crackGrps.insert(e->crackGroup);
        if (e->trained && !e->trainerGroup.empty())
            trainerGrps.insert(e->trainerGroup);
        if (e->hacked && !e->hackGroup.empty())
            hackGrps.insert(e->hackGroup);
        if (e->modified && !e->modGroup.empty())
            modGrps.insert(e->modGroup);
    }

    auto makeList = [](const std::set<std::string>& s, const char* allLabel) {
        std::vector<std::string> v;
        v.push_back(allLabel);
        for (const auto& x : s)
            v.push_back(x);
        return v;
    };
    auto makeVec = [](const std::set<std::string>& s) {
        return std::vector<std::string>(s.begin(), s.end());
    };
    m_categories = makeList(cats, "All Categories");
    m_languages = makeList(langs, "All Languages");
    m_regions = makeList(regions, "All Regions");
    m_systems = makeList(systems, "All Systems");
    m_videos = makeList(videos, "All Video Modes");

    m_crackGroups = makeVec(crackGrps);
    m_trainerGroups = makeVec(trainerGrps);
    m_hackGroups = makeVec(hackGrps);
    m_modGroups = makeVec(modGrps);

    // allGroups = sorted union of all scene group types
    std::set<std::string> allGrps;
    for (const auto* e : core::TosecManager::Instance().GetAllEntries()) {
        if (!e->releaseGroup.empty())
            allGrps.insert(e->releaseGroup);
    }
    allGrps.insert(crackGrps.begin(), crackGrps.end());
    allGrps.insert(trainerGrps.begin(), trainerGrps.end());
    allGrps.insert(hackGrps.begin(), hackGrps.end());
    allGrps.insert(modGrps.begin(), modGrps.end());
    m_allGroups = makeVec(allGrps);

    m_optionsBuilt = true;
    m_filterDirty = true; // rebuild filtered list with fresh options
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter application
// ─────────────────────────────────────────────────────────────────────────────

void TosecBrowserView::ApplyFilters()
{
    if (!m_filterDirty || !core::TosecManager::Instance().IsLoaded())
        return;
    m_filterDirty = false;

    // Reset selection when filters change
    m_selectedIdx = -1;
    m_selectedEntry = nullptr;

    auto allEntries = core::TosecManager::Instance().GetAllEntries();
    m_totalEntries = static_cast<int>(allEntries.size());

    // Lowercase text is precomputed once per database load: ToLowerStr on four fields per
    // entry over the whole database used to run on every filter keystroke. The '\n' joiner
    // keeps a search string from matching across field boundaries.
    if (m_lowerCache.size() != allEntries.size()) {
        m_lowerCache.clear();
        m_lowerCache.reserve(allEntries.size());
        for (const auto* e : allEntries) {
            LoweredEntry low;
            low.searchBlob = ToLowerStr(e->gameTitle) + '\n' + ToLowerStr(e->romName) + '\n' +
                             ToLowerStr(e->publisher) + '\n' + ToLowerStr(e->releaseGroup);
            low.publisher = ToLowerStr(e->publisher);
            m_lowerCache.push_back(std::move(low));
        }
    }

    m_filtered.clear();
    m_filtered.reserve(static_cast<size_t>(m_totalEntries));

    // Pre-parse text filters
    std::string searchLow = ToLowerStr(std::string(m_searchBuf));
    std::string pubLow = ToLowerStr(std::string(m_publisherBuf));

    int yearMin = 0, yearMax = 9999;
    {
        std::string s(m_yearMinBuf);
        if (!s.empty())
            try {
                yearMin = std::stoi(s);
            } catch (...) {
                // Unparseable number in TOSEC text — fall back to the default.
            }
    }
    {
        std::string s(m_yearMaxBuf);
        if (!s.empty())
            try {
                yearMax = std::stoi(s);
            } catch (...) {
                // Unparseable number in TOSEC text — fall back to the default.
            }
    }

    // Combo selections → value string (index 0 = "All" → empty string means no filter)
    const std::string& catFilter = (m_categoryIdx > 0 && m_categoryIdx < (int)m_categories.size())
                                       ? m_categories[m_categoryIdx]
                                       : "";
    const std::string& langFilter = (m_languageIdx > 0 && m_languageIdx < (int)m_languages.size())
                                        ? m_languages[m_languageIdx]
                                        : "";
    const std::string& regFilter =
        (m_regionIdx > 0 && m_regionIdx < (int)m_regions.size()) ? m_regions[m_regionIdx] : "";
    const std::string& sysFilter =
        (m_systemIdx > 0 && m_systemIdx < (int)m_systems.size()) ? m_systems[m_systemIdx] : "";
    const std::string& vidFilter =
        (m_videoIdx > 0 && m_videoIdx < (int)m_videos.size()) ? m_videos[m_videoIdx] : "";

    // ── Pass 1: apply all NON-group filters, collect active groups ───────────
    m_activeGroups.clear();
    std::vector<const core::TosecEntry*> preGroupFiltered;
    preGroupFiltered.reserve(static_cast<size_t>(m_totalEntries));

    for (size_t ei = 0; ei < allEntries.size(); ++ei) {
        const auto* e = allEntries[ei];
        const LoweredEntry& low = m_lowerCache[ei];
        // ── Full-text search (title, romName, publisher, group) ──────────────
        if (!searchLow.empty() && low.searchBlob.find(searchLow) == std::string::npos)
            continue;

        // ── Category ─────────────────────────────────────────────────────────
        if (!catFilter.empty() && e->category != catFilter)
            continue;

        // ── Publisher text filter ─────────────────────────────────────────────
        if (!pubLow.empty() && low.publisher.find(pubLow) == std::string::npos)
            continue;

        // ── Year range ────────────────────────────────────────────────────────
        if (!e->year.empty() && e->year != "????" && e->year.size() >= 4) {
            try {
                int y = std::stoi(e->year.substr(0, 4));
                if (y < yearMin || y > yearMax)
                    continue;
            } catch (...) {
                // Unparseable number in TOSEC text — fall back to the default.
            }
        }

        // ── Language ──────────────────────────────────────────────────────────
        if (!langFilter.empty() && e->language != langFilter)
            continue;

        // ── Region ────────────────────────────────────────────────────────────
        if (!regFilter.empty() && e->region != regFilter)
            continue;

        // ── System ────────────────────────────────────────────────────────────
        if (!sysFilter.empty() && e->system != sysFilter)
            continue;

        // ── Video standard ────────────────────────────────────────────────────
        if (!vidFilter.empty() && e->video != vidFilter)
            continue;

        // ── Disk count ────────────────────────────────────────────────────────
        if (m_diskCountIdx > 0) {
            int total = ParseTotalDisks(e->media);
            if (m_diskCountIdx <= 4) {
                if (total != m_diskCountIdx)
                    continue;
            } else {
                if (total < 5)
                    continue;
            }
        }

        // ── Positive filters (entry MUST have these) ─────────────────────────
        if (m_filterVerifiedOnly && !e->verified)
            continue;
        if (m_filterDemoOnly && e->demo.empty())
            continue;
        if (m_filterHasDownload && e->downloadUrl.empty())
            continue;
        if (m_filterCracked && !e->cracked)
            continue;
        if (m_filterTrained && !e->trained)
            continue;
        if (m_filterHacked && !e->hacked)
            continue;
        if (m_filterModified && !e->modified)
            continue;

        // ── Hide filters ─────────────────────────────────────────────────────
        if (m_hideBadDumps && e->badDump)
            continue;
        if (m_hideVirus && e->virusInfected)
            continue;
        if (m_hideOverdumps && e->overdump)
            continue;
        if (m_hideAlternate && e->alternateDump)
            continue;

        // Collect groups present in this pre-group result set
        switch (m_groupTypeIdx) {
            case 0: // All types
                if (!e->crackGroup.empty())
                    m_activeGroups.insert(e->crackGroup);
                if (!e->trainerGroup.empty())
                    m_activeGroups.insert(e->trainerGroup);
                if (!e->hackGroup.empty())
                    m_activeGroups.insert(e->hackGroup);
                if (!e->modGroup.empty())
                    m_activeGroups.insert(e->modGroup);
                if (!e->releaseGroup.empty())
                    m_activeGroups.insert(e->releaseGroup);
                break;
            case 1:
                if (!e->crackGroup.empty())
                    m_activeGroups.insert(e->crackGroup);
                break;
            case 2:
                if (!e->trainerGroup.empty())
                    m_activeGroups.insert(e->trainerGroup);
                break;
            case 3:
                if (!e->hackGroup.empty())
                    m_activeGroups.insert(e->hackGroup);
                break;
            case 4:
                if (!e->modGroup.empty())
                    m_activeGroups.insert(e->modGroup);
                break;
        }

        preGroupFiltered.push_back(e);
    }

    // ── Pass 2: apply scene group filter ─────────────────────────────────────
    for (const auto* e : preGroupFiltered) {
        if (!m_selectedGroups.empty()) {
            bool match = false;
            switch (m_groupTypeIdx) {
                case 0: // All types
                    match = m_selectedGroups.count(e->crackGroup) ||
                            m_selectedGroups.count(e->trainerGroup) ||
                            m_selectedGroups.count(e->hackGroup) ||
                            m_selectedGroups.count(e->modGroup) ||
                            m_selectedGroups.count(e->releaseGroup);
                    break;
                case 1:
                    match = m_selectedGroups.count(e->crackGroup) > 0;
                    break;
                case 2:
                    match = m_selectedGroups.count(e->trainerGroup) > 0;
                    break;
                case 3:
                    match = m_selectedGroups.count(e->hackGroup) > 0;
                    break;
                case 4:
                    match = m_selectedGroups.count(e->modGroup) > 0;
                    break;
            }
            if (!match)
                continue;
        }
        m_filtered.push_back(e);
    }

    // Resolve a pending SelectEntry() request
    if (!m_pendingSelectMD5.empty()) {
        for (int i = 0; i < static_cast<int>(m_filtered.size()); ++i) {
            if (m_filtered[i]->md5 == m_pendingSelectMD5) {
                m_selectedIdx = i;
                m_selectedEntry = m_filtered[i];
                m_scrollToSelected = true;
                break;
            }
        }
        m_pendingSelectMD5.clear();
    }

    // Signal that the sort needs to be re-applied (Item 5)
    m_needsSort = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter panel (left sidebar)
// ─────────────────────────────────────────────────────────────────────────────

static bool ComboFilter(const char* label, int& idx, const std::vector<std::string>& items)
{
    return ComboFromVector(label, idx, items, ImGuiComboFlags_HeightLarge);
}

void TosecBrowserView::DrawFilterPanel()
{
    bool dirty = false;

    ImGui::SeparatorText("Search");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##search", "Title / ROM name / Publisher...", m_searchBuf,
                                 sizeof(m_searchBuf)))
        dirty = true;

    ImGui::Spacing();
    ImGui::SeparatorText("Classification");

    ImGui::SetNextItemWidth(-1.0f);
    if (ComboFilter("##cat", m_categoryIdx, m_categories))
        dirty = true;
    ImGui::SetItemTooltip("Category");

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##pub", "Publisher...", m_publisherBuf, sizeof(m_publisherBuf)))
        dirty = true;
    ImGui::SetItemTooltip("Publisher");

    float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::SetNextItemWidth(halfW);
    if (ImGui::InputTextWithHint("##ymin", "Year min", m_yearMinBuf, sizeof(m_yearMinBuf)))
        dirty = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##ymax", "Year max", m_yearMaxBuf, sizeof(m_yearMaxBuf)))
        dirty = true;

    ImGui::Spacing();
    ImGui::SeparatorText("Technical");

    ImGui::SetNextItemWidth(-1.0f);
    if (ComboFilter("##lang", m_languageIdx, m_languages))
        dirty = true;
    ImGui::SetItemTooltip("Language");

    ImGui::SetNextItemWidth(-1.0f);
    if (ComboFilter("##reg", m_regionIdx, m_regions))
        dirty = true;
    ImGui::SetItemTooltip("Region");

    ImGui::SetNextItemWidth(-1.0f);
    if (ComboFilter("##sys", m_systemIdx, m_systems))
        dirty = true;
    ImGui::SetItemTooltip("System / Hardware");

    ImGui::SetNextItemWidth(-1.0f);
    if (ComboFilter("##vid", m_videoIdx, m_videos))
        dirty = true;
    ImGui::SetItemTooltip("Video standard (PAL / NTSC)");

    // Disk count
    static const char* diskItems[] = {"Any", "1 disk", "2 disks", "3 disks", "4 disks", "5+ disks"};
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##disks", diskItems[m_diskCountIdx])) {
        for (int i = 0; i < 6; ++i) {
            bool sel = (m_diskCountIdx == i);
            if (ImGui::Selectable(diskItems[i], sel)) {
                m_diskCountIdx = i;
                dirty = true;
            }
            if (sel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Number of disks");

    ImGui::Spacing();
    ImGui::SeparatorText("Integrity");

    if (ImGui::Checkbox("Verified [!] only", &m_filterVerifiedOnly))
        dirty = true;
    if (ImGui::Checkbox("Has download URL", &m_filterHasDownload))
        dirty = true;
    if (ImGui::Checkbox("Demo only", &m_filterDemoOnly))
        dirty = true;

    ImGui::Spacing();
    ImGui::SeparatorText("Scene Modifications");
    ImGui::TextDisabled("(Entry must have ALL checked)");
    ImGui::Spacing();

    if (ImGui::Checkbox("Cracked [cr]", &m_filterCracked))
        dirty = true;
    if (ImGui::Checkbox("Trained [t]", &m_filterTrained))
        dirty = true;
    if (ImGui::Checkbox("Hacked [h]", &m_filterHacked))
        dirty = true;
    if (ImGui::Checkbox("Modified [m]", &m_filterModified))
        dirty = true;

    ImGui::Spacing();
    ImGui::SeparatorText("Scene Groups");

    // Type selector – clears selection when changed
    static const char* groupTypeLabels[] = {"All Groups", "Crack [cr]", "Trainer [t]", "Hack [h]",
                                            "Mod [m]"};
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##gtype", groupTypeLabels[m_groupTypeIdx])) {
        for (int i = 0; i < 5; ++i) {
            bool sel = (m_groupTypeIdx == i);
            if (ImGui::Selectable(groupTypeLabels[i], sel)) {
                if (m_groupTypeIdx != i) {
                    m_groupTypeIdx = i;
                    m_selectedGroups.clear();
                    dirty = true;
                }
            }
            if (sel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Search box
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##gsearch", "Filter groups...", m_groupSearchBuf,
                             sizeof(m_groupSearchBuf));

    // Build the display list (current type, filtered by search text)
    const std::vector<std::string>* srcList = &m_allGroups;
    switch (m_groupTypeIdx) {
        case 1:
            srcList = &m_crackGroups;
            break;
        case 2:
            srcList = &m_trainerGroups;
            break;
        case 3:
            srcList = &m_hackGroups;
            break;
        case 4:
            srcList = &m_modGroups;
            break;
        default:
            break;
    }
    std::string groupSearchLow = ToLowerStr(std::string(m_groupSearchBuf));

    float listH = 8.0f * ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginListBox("##groups", ImVec2(-1.0f, listH))) {
        for (const auto& grp : *srcList) {
            // Skip groups not present in the pre-group filtered results
            if (!m_activeGroups.empty() && m_activeGroups.count(grp) == 0)
                continue;
            if (!groupSearchLow.empty() &&
                ToLowerStr(grp).find(groupSearchLow) == std::string::npos)
                continue;
            bool sel = m_selectedGroups.count(grp) > 0;
            if (ImGui::Selectable(grp.c_str(), sel)) {
                if (sel)
                    m_selectedGroups.erase(grp);
                else
                    m_selectedGroups.insert(grp);
                dirty = true;
            }
        }
        ImGui::EndListBox();
    }

    // Selection count + clear button
    {
        int n = static_cast<int>(m_selectedGroups.size());
        if (n > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%d selected", n);
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##grp")) {
                m_selectedGroups.clear();
                dirty = true;
            }
        } else {
            ImGui::TextDisabled("None selected (show all)");
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Hide");

    if (ImGui::Checkbox("Bad dumps [b]", &m_hideBadDumps))
        dirty = true;
    if (ImGui::Checkbox("Virus infected [v]", &m_hideVirus))
        dirty = true;
    if (ImGui::Checkbox("Overdumps [o]", &m_hideOverdumps))
        dirty = true;
    if (ImGui::Checkbox("Alternate dumps [a]", &m_hideAlternate))
        dirty = true;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Reset Filters", ImVec2(-1.0f, 0.0f))) {
        m_searchBuf[0] = '\0';
        m_publisherBuf[0] = '\0';
        m_yearMinBuf[0] = '\0';
        m_yearMaxBuf[0] = '\0';
        m_categoryIdx = m_languageIdx = m_regionIdx = m_systemIdx = m_videoIdx = 0;
        m_diskCountIdx = 0;
        m_filterVerifiedOnly = m_filterDemoOnly = m_filterHasDownload = false;
        m_filterCracked = m_filterTrained = m_filterHacked = m_filterModified = false;
        m_hideBadDumps = true;
        m_hideVirus = true;
        m_hideOverdumps = m_hideAlternate = false;
        m_groupTypeIdx = 0;
        m_groupSearchBuf[0] = '\0';
        m_selectedGroups.clear();
        dirty = true;
    }

    ImGui::Spacing();

    // Result count
    if (core::TosecManager::Instance().IsLoaded()) {
        ImGui::Text("%d / %d", static_cast<int>(m_filtered.size()), m_totalEntries);
    }

    if (dirty)
        m_filterDirty = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Results table
// ─────────────────────────────────────────────────────────────────────────────

static void DrawFlagBadges(const core::TosecEntry& e)
{
    bool first = true;
    auto badge = [&](const char* label, ImVec4 col) {
        if (!first)
            ImGui::SameLine(0.0f, 3.0f);
        ImGui::TextColored(col, "%s", label);
        first = false;
    };

    if (e.verified)
        badge("[!]", {0.40f, 1.00f, 0.40f, 1.0f});
    if (!e.demo.empty())
        badge("[demo]", {0.60f, 0.90f, 1.00f, 1.0f});
    if (e.cracked)
        badge("[cr]", {1.00f, 0.60f, 0.20f, 1.0f});
    if (e.trained)
        badge("[t]", {0.80f, 0.50f, 1.00f, 1.0f});
    if (e.hacked)
        badge("[h]", {1.00f, 0.40f, 0.60f, 1.0f});
    if (e.modified)
        badge("[m]", {0.90f, 0.70f, 0.30f, 1.0f});
    if (e.fixed)
        badge("[f]", {0.50f, 0.80f, 1.00f, 1.0f});
    if (e.alternateDump)
        badge("[a]", {0.80f, 0.80f, 0.40f, 1.0f});
    if (e.badDump)
        badge("[b]", {1.00f, 0.30f, 0.30f, 1.0f});
    if (e.overdump)
        badge("[o]", {0.80f, 0.55f, 0.30f, 1.0f});
    if (e.virusInfected)
        badge("[v]", {1.00f, 0.10f, 0.10f, 1.0f});
}

static void DrawGroupCell(const core::TosecEntry& e)
{
    bool first = true;
    auto show = [&](const char* prefix, const std::string& group, ImVec4 col) {
        if (group.empty())
            return;
        if (!first)
            ImGui::SameLine(0.0f, 6.0f);
        if (prefix && *prefix)
            ImGui::TextColored(col, "%s %s", prefix, group.c_str());
        else
            ImGui::TextColored(col, "%s", group.c_str());
        first = false;
    };
    show("", e.crackGroup, {1.00f, 0.60f, 0.20f, 1.0f});
    show("", e.trainerGroup, {0.80f, 0.50f, 1.00f, 1.0f});
    show("", e.hackGroup, {1.00f, 0.40f, 0.60f, 1.0f});
    show("", e.modGroup, {0.90f, 0.70f, 0.30f, 1.0f});
    show("", e.releaseGroup, {0.60f, 0.90f, 1.00f, 1.0f});
}

static std::string ParseDiskLabel(const std::vector<std::string>& media)
{
    for (const auto& m : media) {
        auto pos = m.find(" of ");
        if (pos != std::string::npos) {
            std::string beforeOf = m.substr(0, pos);
            std::string afterOf = m.substr(pos + 4);
            size_t spPos = beforeOf.rfind(' ');
            std::string diskNumStr =
                (spPos != std::string::npos) ? beforeOf.substr(spPos + 1) : beforeOf;
            try {
                int diskNum = std::stoi(diskNumStr);
                int total = std::stoi(afterOf);
                if (total > 1)
                    return fmt::format(" ({}/{})", diskNum, total);
            } catch (...) {
                // Unparseable number in TOSEC text — fall back to the default.
            }
        }
    }
    return {};
}

void TosecBrowserView::DrawResultsTable()
{
    if (!core::TosecManager::Instance().IsLoaded()) {
        const char* spinChars = "|/-\\";
        int frame = static_cast<int>(ImGui::GetTime() * 8.0f) % 4;
        ImGui::Text("%c Loading TOSEC database...", spinChars[frame]);
        return;
    }

    static constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable | kAdjustableColumns |
        ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##tosec_table", 8, kTableFlags))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(
        "Title", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 2.5f, 0);
    ImGui::TableSetupColumn("Year", ImGuiTableColumnFlags_WidthStretch, 0.3f, 1);
    ImGui::TableSetupColumn("Publisher", ImGuiTableColumnFlags_WidthStretch, 1.0f, 2);
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 0.9f, 3);
    ImGui::TableSetupColumn("Language", ImGuiTableColumnFlags_WidthStretch, 0.4f, 4);
    ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthStretch, 0.4f, 5);
    ImGui::TableSetupColumn(
        "Groups", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 1.2f, 7);
    ImGui::TableSetupColumn(
        "Flags", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 0.9f, 6);
    ImGui::TableHeadersRow();

    // Sorting (Item 5: retain sort when filters change)
    if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
        if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
            const auto& spec = sortSpecs->Specs[0];
            m_sortColumn = spec.ColumnUserID;
            m_sortDirection = spec.SortDirection;
            m_needsSort = true;
            sortSpecs->SpecsDirty = false;
        }
        if (m_needsSort) {
            bool asc = (m_sortDirection == ImGuiSortDirection_Ascending);
            std::sort(m_filtered.begin(), m_filtered.end(),
                      [&](const core::TosecEntry* a, const core::TosecEntry* b) {
                          int c = 0;
                          switch (m_sortColumn) {
                              case 0:
                                  c = a->gameTitle.compare(b->gameTitle);
                                  break;
                              case 1:
                                  c = a->year.compare(b->year);
                                  break;
                              case 2:
                                  c = a->publisher.compare(b->publisher);
                                  break;
                              case 3:
                                  c = a->category.compare(b->category);
                                  break;
                              case 4:
                                  c = a->language.compare(b->language);
                                  break;
                              case 5:
                                  c = a->region.compare(b->region);
                                  break;
                              default:
                                  c = 0;
                          }
                          return asc ? c < 0 : c > 0;
                      });
            m_needsSort = false;

            // Re-find the selected entry's index after the sort so scroll-to works correctly
            if (m_selectedEntry) {
                m_selectedIdx = -1;
                for (int i = 0; i < static_cast<int>(m_filtered.size()); ++i) {
                    if (m_filtered[i] == m_selectedEntry) {
                        m_selectedIdx = i;
                        break;
                    }
                }
            }
        }
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_filtered.size()));
    if (m_scrollToSelected && m_selectedIdx >= 0)
        clipper.IncludeItemByIndex(m_selectedIdx);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto* e = m_filtered[i];
            ImGui::TableNextRow();

            if (m_scrollToSelected && i == m_selectedIdx) {
                ImGui::SetScrollHereY(0.3f);
                m_scrollToSelected = false;
            }

            ImGui::PushID(i);
            bool selected = (m_selectedIdx == i);

            // Red background tint for virus-infected entries
            if (e->virusInfected) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       ImGui::GetColorU32(ImVec4(0.5f, 0.0f, 0.0f, 0.35f)));
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                       ImGui::GetColorU32(ImVec4(0.6f, 0.0f, 0.0f, 0.35f)));
            }

            // Col 0: Title (the selectable spans all columns)
            if (ImGui::TableSetColumnIndex(0)) {
                std::string displayTitle =
                    (e->gameTitle.empty() ? e->romName : e->gameTitle) + ParseDiskLabel(e->media);
                if (ImGui::Selectable(displayTitle.c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    m_selectedIdx = i;
                    m_selectedEntry = e;
                }
            }
            if (ImGui::TableSetColumnIndex(1))
                ImGui::TextUnformatted(e->year.c_str());
            if (ImGui::TableSetColumnIndex(2))
                ImGui::TextUnformatted(e->publisher.c_str());
            if (ImGui::TableSetColumnIndex(3))
                ImGui::TextUnformatted(e->category.c_str());
            if (ImGui::TableSetColumnIndex(4))
                ImGui::TextUnformatted(e->language.c_str());
            if (ImGui::TableSetColumnIndex(5))
                ImGui::TextUnformatted(e->region.c_str());
            if (ImGui::TableSetColumnIndex(6))
                DrawGroupCell(*e);
            if (ImGui::TableSetColumnIndex(7))
                DrawFlagBadges(*e);

            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

// ─────────────────────────────────────────────────────────────────────────────
// Detail panel
// ─────────────────────────────────────────────────────────────────────────────

static void DetailRow(const char* label, const std::string& value)
{
    if (value.empty())
        return;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s", value.c_str());
}

static void DetailRowBool(const char* label, bool value)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(value ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "%s", value ? "Yes" : "No");
}

void TosecBrowserView::DrawDetailPanel()
{
    if (!m_selectedEntry) {
        float avail = ImGui::GetContentRegionAvail().y;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail * 0.40f);
        const char* msg = "Select a row to view details";
        float w = ImGui::CalcTextSize(msg).x;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
        ImGui::TextDisabled("%s", msg);
        return;
    }

    const auto& e = *m_selectedEntry;

    // Refresh identify panel whenever the selected entry changes (fires for both clicks and
    // programmatic selection via SelectEntry / Find in TOSEC Browser)
    if (m_identifyPanel && e.md5 != m_panelLastMd5) {
        m_panelLastMd5 = e.md5;
        core::IdentifyResult cacheResult;
        core::DiskIdentifier cacheIdentifier;
        // Prefer titleGroupId-based cache (covers all versions of a title);
        // fall back to SHA1 for non-TOSEC entries.
        bool found = (!e.titleGroupId.empty() &&
                      cacheIdentifier.TryLoadCacheByTitleGroup(e.titleGroupId, cacheResult));
        if (!found)
            found = (!e.sha1.empty() && cacheIdentifier.TryLoadCacheBySHA1(e.sha1, cacheResult));
        if (found)
            m_identifyPanel->SetResult(cacheResult);
        else
            m_identifyPanel->ClearResult();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6, 3));

    if (ImGui::BeginTabBar("##detail_tabs")) {
        // ── Identity tab ─────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Identity")) {
            if (ImGui::BeginTable("##id_tbl", 2,
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 240.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                DetailRow("ROM Filename", e.romName);
                DetailRow("Title", e.gameTitle);
                DetailRow("Version", e.version);
                DetailRow("Demo", e.demo);
                DetailRow("Year", e.year);
                DetailRow("Publisher", e.publisher);
                DetailRow("Release Group", e.releaseGroup);
                DetailRow("Category", e.category);
                DetailRow("Language", e.language);
                DetailRow("System", e.system);
                DetailRow("Video", e.video);
                DetailRow("Region", e.region);

                if (!e.media.empty()) {
                    std::string mediaStr;
                    for (size_t i = 0; i < e.media.size(); ++i) {
                        if (i > 0)
                            mediaStr += ", ";
                        mediaStr += e.media[i];
                    }
                    DetailRow("Media", mediaStr);
                }

                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // ── Checksums tab ────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Checksums")) {
            if (ImGui::BeginTable("##cs_tbl", 2,
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 240.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                DetailRow("Size", e.size);
                DetailRow("CRC32", e.crc);
                DetailRow("MD5", e.md5);
                DetailRow("SHA1", e.sha1);

                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // ── Dump status tab ──────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Dump Status")) {
            if (ImGui::BeginTable("##ds_tbl", 2,
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Flag", ImGuiTableColumnFlags_WidthFixed, 240.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                DetailRowBool("Verified [!]", e.verified);
                DetailRowBool("Alternate Dump [a]", e.alternateDump);
                DetailRowBool("Bad Dump [b]", e.badDump);
                DetailRowBool("Overdump [o]", e.overdump);
                DetailRowBool("Virus Infected [v]", e.virusInfected);
                if (e.virusInfected && !e.virusName.empty())
                    DetailRow("  Virus Name", e.virusName);
                DetailRowBool("Fixed [f]", e.fixed);

                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // ── Scene tab ────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Scene")) {
            if (ImGui::BeginTable("##sc_tbl", 2,
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 240.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                // Cracked
                DetailRowBool("Cracked [cr]", e.cracked);
                if (e.cracked)
                    DetailRow("  Crack Group", e.crackGroup);

                // Trained
                DetailRowBool("Trained [t]", e.trained);
                if (e.trained) {
                    if (e.trainerCount > 0) {
                        std::string cnt = std::to_string(e.trainerCount) + " trainers";
                        DetailRow("  Trainer Count", cnt);
                    }
                    DetailRow("  Trainer Language", e.trainerLanguage);
                    DetailRow("  Trainer Group", e.trainerGroup);
                }

                // Hacked
                DetailRowBool("Hacked [h]", e.hacked);
                if (e.hacked) {
                    DetailRow("  Hack Type", e.hackType);
                    DetailRow("  Hack Group", e.hackGroup);
                }

                // Modified
                DetailRowBool("Modified [m]", e.modified);
                if (e.modified) {
                    DetailRow("  Mod Type", e.modType);
                    DetailRow("  Mod Group", e.modGroup);
                }

                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // ── ScreenScraper tab (from cache) ───────────────────────────────────
        if (m_identifyPanel && m_identifyPanel->HasScreenScraper()) {
            if (ImGui::BeginTabItem("ScreenScraper")) {
                m_identifyPanel->DrawScreenScraper();
                ImGui::EndTabItem();
            }
        }

        // ── Pouet tab (from cache) ────────────────────────────────────────────
        if (m_identifyPanel && m_identifyPanel->HasPouet()) {
            if (ImGui::BeginTabItem("Pouet")) {
                m_identifyPanel->DrawPouet();
                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    ImGui::PopStyleVar();

    // ── Download section ──────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Download");
    ImGui::Spacing();

    if (e.downloadUrl.empty()) {
        ImGui::TextDisabled("No download URL available for this entry.");
    } else {
        // Show URL as link
        ImGui::TextLinkOpenURL("Download URL", e.downloadUrl.c_str());
        ImGui::Spacing();

        // The download itself runs in the Documents panel as a placeholder document
        // with its own progress bar; here we just kick it off.
        if (ImGui::Button("  Download  ") && m_onStartDownload) {
            core::DownloadRequest req;
            req.url = e.downloadUrl;

            // Filename: extension from the URL, stem from the ROM name.
            std::string romName = SanitizeFilename(e.romName);
            const std::string& url = e.downloadUrl;
            auto qpos = url.find('?');
            std::string urlNoQuery = url.substr(0, qpos);
            auto slashPos = urlNoQuery.rfind('/');
            std::string urlFile =
                (slashPos != std::string::npos) ? urlNoQuery.substr(slashPos + 1) : urlNoQuery;
            auto dotPos = urlFile.rfind('.');
            std::string ext = (dotPos != std::string::npos) ? urlFile.substr(dotPos) : "";
            auto stemEnd = romName.rfind('.');
            std::string stem =
                (stemEnd != std::string::npos) ? romName.substr(0, stemEnd) : romName;
            req.filename =
                stem +
                (ext.empty() ? (stemEnd != std::string::npos ? romName.substr(stemEnd) : "") : ext);

            m_onStartDownload(std::move(req));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main Draw
// ─────────────────────────────────────────────────────────────────────────────

void TosecBrowserView::Draw()
{
    if (!m_isOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(1200, 750), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(ICON_FA_DATABASE " TOSEC Browser###TOSEC Browser", &m_isOpen)) {
        ImGui::End();
        return;
    }

    BuildFilterOptions();
    ApplyFilters();

    // ── Left: filter panel ────────────────────────────────────────────────────
    constexpr float kFilterW = 270.0f;
    if (ImGui::BeginChild("##filter_panel", ImVec2(kFilterW, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) {
        DrawFilterPanel();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ── Right: table + detail ─────────────────────────────────────────────────
    if (ImGui::BeginChild("##right_panel", ImVec2(0, 0))) {
        constexpr float kSplitterH = 5.0f;
        float availH = ImGui::GetContentRegionAvail().y;
        float tableH = std::max(availH * m_tableSplitRatio, 60.0f);
        float detailH = std::max(availH - tableH - kSplitterH, 80.0f);

        // Table
        if (ImGui::BeginChild("##table_area", ImVec2(0, tableH))) {
            DrawResultsTable();
        }
        ImGui::EndChild();

        // Horizontal splitter
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.6f, 0.6f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.6f, 0.40f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::Button("##tosec_splitter", ImVec2(-1.0f, kSplitterH));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);

        if (ImGui::IsItemActive()) {
            m_tableSplitRatio += ImGui::GetIO().MouseDelta.y / availH;
            m_tableSplitRatio = std::clamp(m_tableSplitRatio, 0.15f, 0.85f);
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

        // Detail
        if (ImGui::BeginChild("##detail_area", ImVec2(0, detailH), ImGuiChildFlags_Borders)) {
            DrawDetailPanel();
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace adm::ui
