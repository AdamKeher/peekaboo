#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <imgui.h>

#include "../core/LaunchOptions.h"
#include "../core/ScreenProfile.h"

namespace adm::ui {

/**
 * @brief The table flags every data grid in the app is given.
 *
 * These do two jobs, not one. They are what lets the user drag a column border, drag a header to
 * reorder, and right-click the header row to hide a column - and they are also the only thing
 * that makes ImGui record the table at all: BeginTable() forces ImGuiTableFlags_NoSavedSettings
 * onto any table carrying none of Resizable/Reorderable/Hideable/Sortable (imgui_tables.cpp,
 * "Adjust flags: disable saved settings if there's nothing to save"). A table without them can
 * never persist its columns, however the layout is stored.
 *
 * Reference and decorative tables - a truth table, a key/value property list, a tooltip - are
 * deliberately left out: their column set is the drawing, not a view onto rows.
 */
inline constexpr ImGuiTableFlags kAdjustableColumns =
    ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;

/**
 * @brief An ImGui table ID that does not depend on which window drew the table.
 *
 * BeginTable() hashes its string against the current ID stack, whose seed is the window ID, so a
 * table drawn in a per-document window gets a different ID for every document - and therefore a
 * different, unshareable settings entry that accumulates in the store forever. Pass this to
 * ImGui::BeginTableEx() instead when one column layout should serve every instance of a view.
 */
ImGuiID StableTableId(const char* key);

/**
 * @brief Column order, width and visibility, persisted for the user rather than per workspace.
 *
 * ImGui keeps table column state in its ini text alongside window and dock state, and Peekaboo
 * sets io.IniFilename = nullptr - layouts are owned by WorkspaceManager. That left column layout
 * reachable only through an explicit workspace save, so columns resized in an ordinary session
 * were gone at the next launch.
 *
 * Columns are treated as a property of the user, not of a layout: this store is the single
 * source of truth for them. WorkspaceLayout strips [Table] sections out of what it saves into
 * workspaces.json and re-applies this store's copy over every layout it loads, so switching
 * workspaces (or restoring a .peek session) leaves your columns alone.
 */
class TableLayoutStore
{
public:
    static constexpr const char* CONFIG_FILENAME = "table_layout.ini";
    static constexpr const char* LORES_FILENAME = "table_layout_lores.ini";

    /// Column widths are pixels and follow the text size, so each screen profile keeps its own
    /// file. Hires keeps the original name, so an existing store stays the Hires one.
    static std::filesystem::path FileFor(core::ScreenProfile profile)
    {
        return core::ProfilePath(profile == core::ScreenProfile::Lores ? LORES_FILENAME
                                                                       : CONFIG_FILENAME);
    }

    /// The file Load() and Save() use when given none. Set before the first Load().
    void SetFile(std::filesystem::path path) { m_file = std::move(path); }
    const std::filesystem::path& File() const { return m_file; }

    /// Save the live columns to the current file and make @p path current, loading it. A file
    /// that does not exist yet leaves the columns as they are, to carry over into it.
    /// @return true when @p path held a stored layout that is now loaded.
    bool SwitchFile(const std::filesystem::path& path);

    /// Read the file. Missing or unreadable is not an error - the store is simply empty.
    /// The path is a parameter so a test can point at its own file rather than the user's.
    void Load() { Load(m_file); }
    void Load(const std::filesystem::path& path);

    /// Write the file. No-op while empty, so a failed launch cannot truncate a good store.
    void Save() const { Save(m_file); }
    void Save(const std::filesystem::path& path) const;

    /// Take the [Table] sections out of the live ImGui context. Call while the context exists.
    void Capture();

    /// The stored ini text: zero or more [Table] sections, or empty.
    const std::string& Ini() const { return m_ini; }

    /// Append the store to @p layout so a LoadIniSettingsFromMemory of it restores columns too.
    /// Any [Table] sections already in @p layout are dropped first: two copies of one table would
    /// otherwise merge flag-by-flag rather than one cleanly replacing the other.
    std::string ApplyTo(std::string_view layout) const;

    /// Forget every stored column layout of the current file (screen profile) and reset every
    /// live table to its defaults.
    void ResetAll();

    /// The [Table] sections of an ImGui ini, concatenated.
    static std::string ExtractTables(std::string_view ini);

    /// @p ini with its [Table] sections removed.
    static std::string StripTables(std::string_view ini);

private:
    std::string m_ini;
    std::filesystem::path m_file = core::ProfilePath(CONFIG_FILENAME);
};

/// The process-wide store. Column layout is user-level configuration with exactly one instance,
/// reached from main() at startup and shutdown, from WorkspaceLayout, and from the View menu.
TableLayoutStore& GlobalTableLayout();

} // namespace adm::ui
