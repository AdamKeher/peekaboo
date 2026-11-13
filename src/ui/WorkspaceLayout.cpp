#include "WorkspaceLayout.h"

#include <algorithm>
#include <cstring>
#include <map>

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "../core/Logger.h"
#include "../core/WorkspaceManager.h"
#include "PanelManager.h"
#include "TableLayout.h"
#include "WorkspaceTransitionManager.h"
#include "WorkspaceWindowStates.h"
#include "views/BlockEditorView.h"
#include "views/DebuggerView.h"
#include "views/FileListView.h"
#include "views/PanelView.h"

namespace adm::ui {

void WorkspaceLayout::Init(core::WorkspaceManager* workspaceManager, PanelManager* panelManager,
                           PanelView* documentsPanel, BlockEditorView* hexEditorView,
                           DebuggerView* debuggerView,
                           WorkspaceTransitionManager* transitionManager,
                           ImGuiID documentsPanelDockId, WorkspaceLayoutOps ops)
{
    m_workspaceManager = workspaceManager;
    m_panelManager = panelManager;
    m_documentsPanel = documentsPanel;
    m_hexEditorView = hexEditorView;
    m_debuggerView = debuggerView;
    m_transitionManager = transitionManager;
    m_documentsPanelDockId = documentsPanelDockId;
    m_ops = std::move(ops);
}

void WorkspaceLayout::Save(const std::string& name)
{
    core::WorkspaceManager::Layout ws;
    for (const auto& state : m_ops.windowStates())
        ws.windowStates[state.name] = *state.openPtr;
    for (const auto& panel : m_panelManager->Panels()) {
        const bool open = *panel->GetOpenStatePtr();
        ws.panels.push_back({panel->GetName(), panel->GetId(), open ? 1 : 0});
        // under its name too, for an older build - unless a window has that name: the panel's
        // state would overwrite the window's
        if (!ws.windowStates.count(panel->GetName()))
            ws.windowStates[panel->GetName()] = open;
    }
    ws.blockBytesPerLine = m_hexEditorView->GetBytesPerLine();
    for (const auto& [name, open] : m_debuggerView->GetDisplaySectionStates())
        ws.sectionStates["Display/" + name] = open;
    size_t sz = 0;
    const char* iniData = ImGui::SaveIniSettingsToMemory(&sz);
    // Column order, width and visibility belong to the user, not to the workspace: they are
    // stripped here and kept in TableLayoutStore instead, so loading a layout saved months ago
    // does not drag month-old columns back with it. See TableLayout.h.
    ws.imguiLayout = TableLayoutStore::StripTables(std::string_view(iniData, sz));
    GlobalTableLayout().Capture();
    GlobalTableLayout().Save();
    m_workspaceManager->SaveLayout(name, m_profile, ws);
}

void WorkspaceLayout::ApplyPending()
{
    if (m_pendingLayout.empty()) {
        if (m_pendingTables) {
            m_pendingTables = false;
            const std::string& tables = GlobalTableLayout().Ini();
            if (!tables.empty())
                ImGui::LoadIniSettingsFromMemory(tables.c_str(), tables.size());
        }
        return;
    }
    m_pendingTables = false; // the store rides along with the layout below

    // The user's column layout rides along with every layout we load - including a legacy
    // workspace or .peek session that still carries [Table] sections of its own, which ApplyTo
    // drops in favour of the store.
    const std::string layout = GlobalTableLayout().ApplyTo(m_pendingLayout);
    ImGui::LoadIniSettingsFromMemory(layout.c_str(), layout.size());
    m_pendingLayout.clear();

    // Snapshot each dock node's freshly-loaded SelectedTabId now, while it still reflects the saved
    // workspace (before any frame runs and lets the focused window's tab override it). We re-assert
    // these while the layout settles. See ReassertWorkspaceTabSelection / m_tabSelection.
    HoldTabSelection();

    // Give panels a few frames to each create their DockSpace host (see PrimePanelDockHosts). This
    // is capped generously; priming stops early as soon as every panel is hosted.
    m_panelPrimeFrames = 30;

    // After the fresh layout is loaded we must force every window to *re-dock on the same
    // frame*, otherwise both tab order and the selected tab come out wrong. Two reasons:
    //
    //  1) LoadIniSettingsFromMemory -> DockContextBuildAddWindowsToNodes only re-docks windows
    //     that were active last frame (LastFrameActive >= FrameCount-1). On a workspace switch
    //     the incoming windows were mostly NOT active in the previous workspace, so ImGui
    //     pre-docks only the few that overlap and leaves the rest to dock lazily via Begin()
    //     over subsequent frames.
    //  2) DockNodeUpdateTabBar sorts tabs by their persistent DockOrder ONLY among tabs that
    //     first appear on the *same* frame (the contiguous run of ImGuiTabItemFlags_Unsorted).
    //     Windows that dock across different frames are simply appended in submission order,
    //     and the saved SelectedTabId is lost once a stray tab is selected first.
    //
    // So: detach every window from its node here (keeping window->DockId and, crucially, the
    // node's SelectedTabId), and clear each node's Windows list + TabBar. Every still-open
    // window then re-docks together on the next frame via Begin(), letting ImGui apply the
    // saved DockOrder sort and SelectedTabId. (This also subsumes the old split-node sweep that
    // guarded against DockNodeUpdate's "split node must have no TabBar" assert.)
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    for (int n = 0; n < ctx->DockContext.Nodes.Data.Size; n++) {
        ImGuiDockNode* node = (ImGuiDockNode*)ctx->DockContext.Nodes.Data[n].val_p;
        if (!node || (node->Windows.empty() && node->TabBar == nullptr))
            continue;
        for (ImGuiWindow* window : node->Windows) {
            // Keep window->DockId so Begin() re-docks it into this same node next frame.
            window->DockNode = nullptr;
            window->DockIsActive = false;
            window->DockTabIsVisible = false;
            window->DockNodeIsVisible = false;
        }
        node->Windows.clear();
        if (node->TabBar) {
            IM_DELETE(node->TabBar);
            node->TabBar = nullptr;
        }
        // node->SelectedTabId is intentionally left untouched: DockNodeAddWindow() seeds the
        // rebuilt TabBar's selection from it when the windows re-dock next frame.
    }

    // Re-apply document dock state captured in LoadWorkspace, now that the new
    // dock tree exists. Floating documents are restored to the same position and
    // size; docked documents are re-docked into the Documents panel.
    for (const auto& [docId, view] : m_ops.documentViews()) {
        const auto restore =
            std::find_if(m_dockRestore.begin(), m_dockRestore.end(),
                         [id = docId](const PendingDockRestore& r) { return r.docId == id; });
        if (restore == m_dockRestore.end() || !view)
            continue;

        if (restore->floating)
            view->ForceFloat(restore->posX, restore->posY, restore->sizeX, restore->sizeY);
        else
            view->ForceDockId(m_documentsPanelDockId);
    }
    m_dockRestore.clear();
}

bool WorkspaceLayout::PrimePanelDockHosts()
{
    if (m_panelPrimeFrames <= 0)
        return false;
    --m_panelPrimeFrames;

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx)
        return false;

    std::vector<PanelView*> panels;
    if (m_documentsPanel)
        panels.push_back(m_documentsPanel);
    for (auto& p : m_panelManager->Panels())
        panels.push_back(p.get());

    // A panel is "hosted" once its DockSpace node (ID == panel ID) has a host window. Bring one
    // un-hosted panel per containing tab bar to the front this frame so it can submit its DockSpace
    // next; re-checking each frame lets every panel take its turn until all are hosted.
    std::vector<ImGuiID> touchedContainers;
    bool anyUnhosted = false;
    for (PanelView* p : panels) {
        ImGuiDockNode* dsNode =
            ImGui::DockContextFindNodeByID(ctx, static_cast<ImGuiID>(p->GetId()));
        if (dsNode && dsNode->HostWindow != nullptr)
            continue; // already hosted
        anyUnhosted = true;

        // Match the panel window by its persistent ID (the part after "###" in PanelView's title).
        std::string idName = fmt::format("###{}##panel_{:08X}", p->GetName(), p->GetId());
        ImGuiWindow* pw = ImGui::FindWindowByName(idName.c_str());
        if (!pw || pw->DockNode == nullptr || pw->DockNode->TabBar == nullptr)
            continue; // panel window / tab bar not built yet — retry next frame

        ImGuiDockNode* container = pw->DockNode;
        if (std::find(touchedContainers.begin(), touchedContainers.end(), container->ID) !=
            touchedContainers.end())
            continue; // already promoted a panel in this tab bar this frame
        touchedContainers.push_back(container->ID);
        container->SelectedTabId = pw->TabId;
        container->TabBar->NextSelectedTabId = pw->TabId;
    }

    if (!anyUnhosted) {
        m_panelPrimeFrames = 0; // every panel hosted — stop priming, restore saved tab selection
        return false;
    }
    return true; // still priming this frame; skip tab re-assertion so we don't fight our selection
}

void WorkspaceLayout::HoldTabSelection()
{
    m_tabSelection.clear();
    ImGuiContext* c = ImGui::GetCurrentContext();
    for (int n = 0; n < c->DockContext.Nodes.Data.Size; n++) {
        ImGuiDockNode* node = (ImGuiDockNode*)c->DockContext.Nodes.Data[n].val_p;
        if (node && node->SelectedTabId != 0)
            m_tabSelection.emplace_back(node->ID, node->SelectedTabId);
    }
    m_tabSelectFrames = 60;
}

void WorkspaceLayout::ReassertTabSelection()
{
    if (m_tabSelectFrames <= 0)
        return;

    // Bail out the instant the user interacts, so a deliberate tab click is never fought.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_tabSelectFrames = 0;
        m_tabSelection.clear();
        return;
    }

    // Keep the correction window open for the whole workspace transition plus a settle buffer.
    // The layout can take a while to settle after the transition ends, and a docked window that
    // only "appears" then (e.g. one on a hidden tab) will grab focus-on-appear and flip its dock
    // node's selected tab away from what the workspace saved. Topping the counter up while the
    // transition is active makes the buffer adapt to the machine's frame rate.
    if (m_transitionManager->IsTransitioning() && m_tabSelectFrames < 60)
        m_tabSelectFrames = 60;
    --m_tabSelectFrames;

    // Runs before this frame's dock-node updates (DockSpaceOverViewport / panel DockSpaces). Only
    // touch nodes that have actually drifted from the saved selection, so we don't cause needless
    // churn on nodes that are already correct.
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    bool drifted = false;
    for (const auto& [nodeId, tabId] : m_tabSelection) {
        ImGuiDockNode* node = ImGui::DockContextFindNodeByID(ctx, nodeId);
        if (!node || !node->TabBar || node->TabBar->SelectedTabId == tabId)
            continue;
        if (ImGui::TabBarFindTabByID(node->TabBar, tabId) == nullptr)
            continue; // target tab not present yet (windows still re-docking) — try again next
                      // frame
        node->SelectedTabId = tabId;
        node->TabBar->NextSelectedTabId = tabId;
        drifted = true;
    }

    // A node only drifts when some window grabbed focus and DockNodeUpdateTabBar's "select the
    // focused window's tab" rule (imgui_internal, ~DockNodeUpdateTabBar) overrode our selection.
    // Setting NextSelectedTabId re-selects the right tab for rendering but does NOT move
    // g.NavWindow off the culprit, so it would just win again next frame. Dropping focus entirely
    // reproduces the cold-startup condition (NavWindow == NULL), where every node honours its saved
    // SelectedTabId. The culprit window has already finished appearing, so it won't re-grab; the
    // user's first click (handled above) restores normal focus behaviour.
    // Not while a modal is up: focusing nothing closes every open popup, the modal included.
    if (drifted && ImGui::GetTopMostPopupModal() == nullptr)
        ImGui::FocusWindow(nullptr);

    if (m_tabSelectFrames == 0)
        m_tabSelection.clear();
}

void WorkspaceLayout::Load(const std::string& name)
{
    const auto* ws = m_workspaceManager->FindLayout(name, m_profile);
    core::WorkspaceManager::Layout scaled;
    if (!ws) {
        // Nothing saved for this screen yet: borrow the other profile's layout, stretched from
        // the window it was saved in to this one. Only in memory - it becomes this profile's
        // own the first time the user saves it.
        const auto* other =
            m_workspaceManager->FindLayout(name, core::OtherScreenProfile(m_profile));
        if (!other)
            return;
        scaled = *other;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        const ImVec2 screen = m_ops.screenSize ? m_ops.screenSize() : ImVec2(0.0f, 0.0f);
        if (core::FindMainDockSpace(scaled.imguiLayout, x, y, w, h) && screen.x > 0.0f &&
            screen.y > 0.0f) {
            // The main dock space runs from under the menu bar to the window's bottom-right
            // corner, so its far edge is the saving window's size.
            const float sx = screen.x / (x + w);
            const float sy = screen.y / (y + h);
            scaled.imguiLayout = core::ScaleImGuiIni(scaled.imguiLayout, sx, sy);
            core::Logger::Info("Workspace: '{}' has no {} layout; scaling the {} one by "
                               "{:.2f} x {:.2f}",
                               name, core::ScreenProfileLabel(m_profile),
                               core::ScreenProfileLabel(core::OtherScreenProfile(m_profile)), sx,
                               sy);
        }
        ws = &scaled;
    }

    // Recreate panels with their original IDs before applying the imgui.ini so that
    // ImGui can rebind the saved DockSpace nodes to the correct panel windows.
    m_panelManager->Clear();
    for (const auto& def : ws->panels) {
        if (!m_panelManager->Restore(def.name, def.id))
            continue;
        bool* open = m_panelManager->Find(def.id)->GetOpenStatePtr();
        if (def.open >= 0) {
            *open = def.open != 0;
        } else if (auto state = ws->windowStates.find(def.name); state != ws->windowStates.end()) {
            *open = state->second;
        }
    }

    ApplyWorkspaceWindowStates(ws->windowStates, m_ops.windowStates());
    {
        std::map<std::string, bool> displaySections;
        const std::string prefix = "Display/";
        for (const auto& [key, open] : ws->sectionStates) {
            if (key.rfind(prefix, 0) == 0)
                displaySections[key.substr(prefix.size())] = open;
        }
        // Only override when the workspace actually carries section state, so a workspace saved
        // before this existed leaves the panel at its defaults instead of collapsing everything.
        if (!displaySections.empty())
            m_debuggerView->SetDisplaySectionStates(displaySections);
    }
    if (ws->blockBytesPerLine == 16 || ws->blockBytesPerLine == 24 || ws->blockBytesPerLine == 32)
        m_hexEditorView->SetBytesPerLine(ws->blockBytesPerLine);
    m_dockRestore.clear();
    if (ws->imguiLayout.empty())
        return;
    m_pendingLayout = ws->imguiLayout;

    // Capture each open document's current dock state. The new workspace's saved
    // layout has no entry for these FileListViews, so after
    // LoadIniSettingsFromMemory replaces the dock node tree they would float to a
    // random location. We re-apply in ApplyPendingWorkspaceLayout once the new
    // tree exists:
    //   - floating -> restore to the same position and size
    //   - docked   -> re-dock into the Documents panel
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx)
        return;

    for (const auto& [docId, view] : m_ops.documentViews()) {
        (void)view;
        std::string windowId = fmt::format("###FileListView_{}", docId);
        ImGuiWindow* docWin = nullptr;
        for (int i = 0; i < ctx->Windows.Size; i++) {
            ImGuiWindow* win = ctx->Windows[i];
            if (strstr(win->Name, windowId.c_str())) {
                docWin = win;
                break;
            }
        }
        if (!docWin)
            continue;

        PendingDockRestore restore;
        restore.docId = docId;
        restore.floating = (docWin->DockId == 0); // not in any dock node
        restore.posX = docWin->Pos.x;
        restore.posY = docWin->Pos.y;
        restore.sizeX = docWin->Size.x;
        restore.sizeY = docWin->Size.y;
        m_dockRestore.push_back(restore);
    }
}

} // namespace adm::ui
