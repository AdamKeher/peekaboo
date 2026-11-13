#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "views/PanelView.h"

namespace adm::ui {

/**
 * @brief The user's dockable panels: the list itself, and the new/rename dialogs over it.
 *
 * A panel is a named dockspace the user creates to group windows in. This owns them outright -
 * creation, deletion, renaming, and the two modal dialogs that drive those from the UI - so
 * Application no longer carries the list, two 128-byte name buffers and two dialog flags.
 *
 * Panels are handed out by reference for iteration (drawing, workspace save/load, dock
 * priming); everything that *changes* the set goes through a method here, so the id-uniqueness
 * invariant and the rename/delete callback wiring have exactly one home.
 *
 * Extracted from Application.
 */
class PanelManager
{
public:
    PanelManager() = default;

    PanelManager(const PanelManager&) = delete;
    PanelManager& operator=(const PanelManager&) = delete;

    /// Create a panel with a fresh unique id, wired up so its own close/rename affordances
    /// come back here. Returns the new panel's id.
    uint32_t Create(const std::string& name);

    /// Recreate a panel with an id that already exists - workspace and session restore, which
    /// must preserve ids because saved layouts refer to them. Returns false if the id is
    /// already taken.
    bool Restore(const std::string& name, uint32_t id);

    void Delete(uint32_t id);
    void Rename(uint32_t id, const std::string& newName);

    /// Throw every panel away. Workspace load rebuilds the whole set.
    void Clear() { m_panels.clear(); }

    /// Open the "New Panel" dialog on the next DrawModals().
    void RequestNew();
    /// Open the "Rename Panel" dialog for `id`, pre-filled with its current name.
    void RequestRename(uint32_t id);

    /// Draws both dialogs. Call once per frame; each is a no-op unless requested.
    void DrawModals();

    const std::vector<std::unique_ptr<PanelView>>& Panels() const { return m_panels; }
    std::vector<std::unique_ptr<PanelView>>& Panels() { return m_panels; }

    /// The panel with this id, or null.
    PanelView* Find(uint32_t id);

private:
    /// An id no live panel is using. Random rather than sequential because saved workspaces
    /// hold ids, and a counter reset on restart would collide with them.
    uint32_t GenerateId() const;

    std::vector<std::unique_ptr<PanelView>> m_panels;

    bool m_showNewDialog = false;
    char m_newNameBuf[128] = {};

    bool m_showRenameDialog = false;
    uint32_t m_renameId = 0;
    char m_renameNameBuf[128] = {};
};

} // namespace adm::ui
