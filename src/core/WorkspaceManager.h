#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "LaunchOptions.h"
#include "ScreenProfile.h"

namespace adm::core {

/**
 * @brief Named workspaces, each holding one window layout per screen profile.
 *
 * The workspace list is shared by both profiles: deleting or renaming a workspace acts on both,
 * while saving writes only the layout of the profile in use. A workspace with no layout for a
 * profile yet is shown scaled from the other one (see WorkspaceLayout::Load) until it is saved.
 *
 * On disk the Hires layout keeps the top-level fields it always had, so an older build still
 * reads the file; the Lores layout is an optional "lores" object beside them. An entry with no
 * "imguiLayout" key has no Hires layout.
 */
class WorkspaceManager
{
public:
    static constexpr const char* CONFIG_FILENAME = "workspaces.json";

    struct Layout
    {
        std::map<std::string, bool> windowStates; // window name -> visible
        // Collapsible-section open/closed state, keyed "Window/Section" (e.g. "Display/Overlays").
        // Kept out of ImGui's own ini so a workspace restores a panel exactly as it was left.
        std::map<std::string, bool> sectionStates;
        std::string imguiLayout;   // imgui.ini-format string
        int blockBytesPerLine = 0; // 0 = use the user preference (legacy workspace)

        struct PanelDef
        {
            std::string name;
            uint32_t id = 0;
            // Visible: kept with the panel, as a panel's name can be a window's too (a panel
            // called "Exocet" beside the Exocet Grid, whose key is "Exocet"). -1: not saved, so
            // windowStates under the panel's name says (a workspace saved before this).
            int open = -1;
        };
        std::vector<PanelDef> panels;
    };

    struct Workspace
    {
        std::string name;
        std::map<ScreenProfile, Layout> layouts;
    };

    WorkspaceManager() { Load(); }

    const std::vector<Workspace>& GetWorkspaces() const { return m_workspaces; }

    /// Store @p layout as the @p profile layout of @p name, creating the workspace if it is new.
    /// The other profile's layout is left as it was.
    void SaveLayout(const std::string& name, ScreenProfile profile, const Layout& layout)
    {
        auto it = std::find_if(m_workspaces.begin(), m_workspaces.end(),
                               [&](const Workspace& w) { return w.name == name; });
        if (it == m_workspaces.end()) {
            m_workspaces.push_back({name, {}});
            it = std::prev(m_workspaces.end());
        }
        it->layouts[profile] = layout;
        Save();
    }

    const Workspace* FindWorkspace(const std::string& name) const
    {
        auto it = std::find_if(m_workspaces.begin(), m_workspaces.end(),
                               [&](const Workspace& w) { return w.name == name; });
        return (it != m_workspaces.end()) ? &(*it) : nullptr;
    }

    const Layout* FindLayout(const std::string& name, ScreenProfile profile) const
    {
        const Workspace* ws = FindWorkspace(name);
        if (!ws)
            return nullptr;
        auto it = ws->layouts.find(profile);
        return it != ws->layouts.end() ? &it->second : nullptr;
    }

    bool HasLayout(const std::string& name, ScreenProfile profile) const
    {
        return FindLayout(name, profile) != nullptr;
    }

    void DeleteWorkspace(const std::string& name)
    {
        m_workspaces.erase(std::remove_if(m_workspaces.begin(), m_workspaces.end(),
                                          [&](const Workspace& w) { return w.name == name; }),
                           m_workspaces.end());
        Save();
    }

    void RenameWorkspace(const std::string& oldName, const std::string& newName)
    {
        if (oldName == newName || newName.empty())
            return;
        auto it = std::find_if(m_workspaces.begin(), m_workspaces.end(),
                               [&](const Workspace& w) { return w.name == oldName; });
        if (it != m_workspaces.end()) {
            it->name = newName;
            Save();
        }
    }

    static nlohmann::json ToJson(const std::vector<Workspace>& workspaces)
    {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& ws : workspaces) {
            nlohmann::json entry;
            entry["name"] = ws.name;
            if (auto hires = ws.layouts.find(ScreenProfile::Hires); hires != ws.layouts.end())
                WriteLayout(entry, hires->second);
            if (auto lores = ws.layouts.find(ScreenProfile::Lores); lores != ws.layouts.end()) {
                nlohmann::json lj;
                WriteLayout(lj, lores->second);
                entry[ScreenProfileKey(ScreenProfile::Lores)] = lj;
            }
            j.push_back(entry);
        }
        return j;
    }

    /// Unreadable entries are skipped; a document that is not an array yields nothing.
    static std::vector<Workspace> FromJson(const nlohmann::json& j)
    {
        std::vector<Workspace> workspaces;
        if (!j.is_array())
            return workspaces;
        for (const auto& entry : j) {
            if (!entry.is_object())
                continue;
            Workspace ws;
            if (entry.contains("name") && entry["name"].is_string())
                ws.name = entry["name"];
            if (ws.name.empty())
                continue;
            if (entry.contains("imguiLayout"))
                ws.layouts[ScreenProfile::Hires] = ReadLayout(entry);
            const char* loresKey = ScreenProfileKey(ScreenProfile::Lores);
            if (entry.contains(loresKey) && entry[loresKey].is_object())
                ws.layouts[ScreenProfile::Lores] = ReadLayout(entry[loresKey]);
            workspaces.push_back(std::move(ws));
        }
        return workspaces;
    }

private:
    std::vector<Workspace> m_workspaces;

    static void WriteLayout(nlohmann::json& entry, const Layout& layout)
    {
        entry["windowStates"] = layout.windowStates;
        entry["sectionStates"] = layout.sectionStates;
        entry["imguiLayout"] = layout.imguiLayout;
        entry["blockBytesPerLine"] = layout.blockBytesPerLine;
        nlohmann::json panelsArr = nlohmann::json::array();
        for (const auto& p : layout.panels) {
            nlohmann::json pj = {{"name", p.name}, {"id", p.id}};
            if (p.open >= 0)
                pj["open"] = p.open != 0;
            panelsArr.push_back(pj);
        }
        entry["panels"] = panelsArr;
    }

    static Layout ReadLayout(const nlohmann::json& entry)
    {
        Layout layout;
        if (entry.contains("windowStates") && entry["windowStates"].is_object()) {
            for (auto& [key, val] : entry["windowStates"].items()) {
                if (val.is_boolean())
                    layout.windowStates[key] = val.get<bool>();
            }
        }
        if (entry.contains("sectionStates") && entry["sectionStates"].is_object()) {
            for (auto& [key, val] : entry["sectionStates"].items()) {
                if (val.is_boolean())
                    layout.sectionStates[key] = val.get<bool>();
            }
        }
        if (entry.contains("imguiLayout") && entry["imguiLayout"].is_string())
            layout.imguiLayout = entry["imguiLayout"];
        if (entry.contains("blockBytesPerLine") && entry["blockBytesPerLine"].is_number_integer())
            layout.blockBytesPerLine = entry["blockBytesPerLine"].get<int>();
        if (entry.contains("panels") && entry["panels"].is_array()) {
            for (const auto& pd : entry["panels"]) {
                Layout::PanelDef def;
                if (pd.contains("name") && pd["name"].is_string())
                    def.name = pd["name"];
                if (pd.contains("id") && pd["id"].is_number_unsigned())
                    def.id = pd["id"].get<uint32_t>();
                if (pd.contains("open") && pd["open"].is_boolean())
                    def.open = pd["open"].get<bool>() ? 1 : 0;
                if (!def.name.empty() && def.id != 0)
                    layout.panels.push_back(def);
            }
        }
        return layout;
    }

    void Save()
    {
        std::ofstream out(ProfilePath(CONFIG_FILENAME));
        if (out.is_open()) {
            out << ToJson(m_workspaces).dump(4);
        }
    }

    void Load()
    {
        const auto path = ProfilePath(CONFIG_FILENAME);
        if (!std::filesystem::exists(path))
            return;
        std::ifstream in(path);
        if (!in.is_open())
            return;
        try {
            nlohmann::json j;
            in >> j;
            m_workspaces = FromJson(j);
        } catch (...) {
            m_workspaces.clear();
        }
    }
};

} // namespace adm::core
