#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "../core/ScreenProfile.h"
#include "MainLayout.h"

namespace adm::core {
class WorkspaceManager;
} // namespace adm::core

namespace adm::ui {

class BlockEditorView;
class DebuggerView;
class FileListView;
class PanelManager;
class PanelView;
class WorkspaceTransitionManager;

/**
 * @brief The two lists only Application can produce.
 *
 * Which windows exist and what each one's open flag is, and which documents are open along with
 * the view that draws each. Both are censuses of state Application owns.
 */
struct WorkspaceLayoutOps
{
    std::function<std::vector<WindowState>()> windowStates;
    std::function<std::vector<std::pair<uint32_t, FileListView*>>()> documentViews;
    /// The main window's size in ImGui units - the screen a layout borrowed from the other
    /// profile is scaled to fit. Valid before the first frame, unlike the main viewport.
    std::function<ImVec2()> screenSize;
};

/**
 * @brief Saving and restoring a workspace: window visibility, panels, and the ImGui dock tree.
 *
 * Three of the four methods here exist because of how ImGui's docking restores a saved layout,
 * and each carries the reasoning for a specific misbehaviour it corrects - windows re-docking
 * across different frames losing their tab order, panels on hidden tabs never creating their
 * DockSpace host, and the focused window overriding the saved tab selection. The comments in
 * the .cpp are the record of why each is needed; do not simplify them away.
 *
 * The widest collaborator in this decomposition: restoring a layout genuinely touches the panel
 * set, the document list, the hex editor, the debugger panel and the transition manager, because
 * a workspace is a snapshot of all of them.
 *
 * Extracted from Application.
 */
class WorkspaceLayout
{
public:
    /// All are owned by Application and outlive this object.
    void Init(core::WorkspaceManager* workspaceManager, PanelManager* panelManager,
              PanelView* documentsPanel, BlockEditorView* hexEditorView, DebuggerView* debuggerView,
              WorkspaceTransitionManager* transitionManager, ImGuiID documentsPanelDockId,
              WorkspaceLayoutOps ops);

    /// The screen profile whose layouts Save() writes and Load() reads.
    void SetProfile(core::ScreenProfile profile) { m_profile = profile; }
    core::ScreenProfile Profile() const { return m_profile; }

    /// Capture the current layout as @p name's layout for the current profile.
    void Save(const std::string& name);

    /// Restore @p name's layout for the current profile - or, when it has none yet, the other
    /// profile's scaled to this screen. The ImGui half is deferred to ApplyPending() on the next
    /// frame, before any DockSpace is submitted.
    void Load(const std::string& name);

    /// Re-apply the table column store on the next ApplyPending(), for when it changed without
    /// a layout load (a screen profile switch with no workspace selected).
    void ReloadTables() { m_pendingTables = true; }

    /// Apply a layout queued by Load(). Must be called BEFORE ImGui::NewFrame().
    void ApplyPending();

    /// Queue a layout from somewhere other than a saved workspace (a restored .peek session).
    void SetPendingLayout(std::string layout) { m_pendingLayout = std::move(layout); }

    /// Returns true while still priming panel dock hosts, which suppresses tab re-assertion.
    bool PrimePanelDockHosts();

    void ReassertTabSelection();

    /// Snapshot every dock node's selected tab and re-assert it over the next frames. For when
    /// the whole workspace went unsubmitted (emulator fullscreen) and every window is about to
    /// "appear" at once, each taking focus - and with it its node's selected tab.
    void HoldTabSelection();

    /// True while a loaded layout is still settling: windows are mid-re-dock and tab selections
    /// are still being re-pushed, so anything reading the layout is reading it in motion.
    bool IsSettling() const { return m_panelPrimeFrames > 0 || m_tabSelectFrames > 0; }

private:
    core::WorkspaceManager* m_workspaceManager = nullptr;
    PanelManager* m_panelManager = nullptr;
    PanelView* m_documentsPanel = nullptr;
    BlockEditorView* m_hexEditorView = nullptr;
    DebuggerView* m_debuggerView = nullptr;
    WorkspaceTransitionManager* m_transitionManager = nullptr;
    ImGuiID m_documentsPanelDockId = 0;
    WorkspaceLayoutOps m_ops;

    core::ScreenProfile m_profile = core::ScreenProfile::Hires;

    // Deferred workspace layout (applied before DockSpace on next frame)
    std::string m_pendingLayout;
    bool m_pendingTables = false;

    // Captured dock state of each open document at the moment a workspace switch begins.
    // Re-applied in ApplyPending once the new dock tree exists: floating documents are restored
    // to the same position/size, docked documents are re-docked into the Documents panel.
    struct PendingDockRestore
    {
        uint32_t docId = 0;
        bool floating = false;
        float posX = 0.0f, posY = 0.0f;
        float sizeX = 0.0f, sizeY = 0.0f;
    };
    std::vector<PendingDockRestore> m_dockRestore;

    // Saved per-dock-node tab selection captured right after a workspace layout is loaded, plus a
    // countdown of frames over which to re-assert it. ImGui selects the tab of whichever window
    // holds focus (g.NavWindow) when a dock node's tab bar updates, which at runtime (unlike a
    // cold startup, where nothing is focused yet) clobbers the workspace's saved SelectedTabId. We
    // re-assert NextSelectedTabId while the new layout settles so the saved tab wins.
    std::vector<std::pair<uint32_t, uint32_t>> m_tabSelection; // (dockNodeId, tabId)
    int m_tabSelectFrames = 0;

    // Every panel hosts a DockSpace whose node ID equals the panel ID. When a restored layout
    // leaves a panel as a hidden dock tab, that panel never submits its DockSpace, so ImGui never
    // creates the node's host window - and any window docked inside it renders as a large floating
    // window instead of a hidden tab (BeginDocked bails when node->HostWindow is NULL). To fix this
    // we briefly bring each un-hosted panel to the front of its tab bar (one per frame) so it
    // submits its DockSpace and creates the host; the host then persists once the panel is hidden
    // again. Runs for a few frames after a layout is applied, then hands back to tab re-assertion.
    int m_panelPrimeFrames = 0;
};

} // namespace adm::ui
