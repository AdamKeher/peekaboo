#include "UiIntrospect.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include <fmt/core.h>
#include <imgui.h>
#include <imgui_internal.h>

namespace adm::ui {

using core::UiDockNodeInfo;
using core::UiDockRequest;
using core::UiLayoutSnapshot;
using core::UiPanelInfo;
using core::UiWindowInfo;

namespace {

// A PanelView's Begin() label is "{icon}{name}###{name}##panel_{id:08X}", so this is its identity.
std::string PanelIdTail(const UiPanelEntry& p)
{
    return fmt::format("{}##panel_{:08X}", p.name, p.id);
}

const std::string& LabelOf(const UiRegistryEntry& e)
{
    return e.imguiId.empty() ? e.name : e.imguiId;
}

ImGuiWindow* Find(const std::string& id)
{
    return id.empty() ? nullptr : ImGui::FindWindowByName(id.c_str());
}

// Walk from a docked window up through the nested DockSpaces, returning the name of the outermost
// PanelView that contains it, or empty when it is not inside one.
//
// The climb cannot stop at the first parent it finds: the main dockspace also has a host window
// (DockSpaceOverViewport's "WindowOverViewport_########"), and that host has a parent too, so a
// walk that simply took the last parent reported every top-level window as living in a panel called
// "WindowOverViewport_11111111". Only windows the caller told us are panels are recorded.
std::string OutermostPanelName(ImGuiWindow* w,
                               const std::unordered_map<ImGuiWindow*, std::string>& panelNames)
{
    std::string found;
    int guard = 0;
    while (w && w->DockNode && guard++ < 8) {
        ImGuiDockNode* root = w->DockNode;
        while (root->ParentNode)
            root = root->ParentNode;
        // The DockSpace host is a child window "{panel}/DockSpace_XXXX"; its parent is the panel.
        ImGuiWindow* host = root->HostWindow;
        if (!host || !host->ParentWindow)
            break;
        ImGuiWindow* panel = host->ParentWindow;
        if (panel == w)
            break;
        auto it = panelNames.find(panel);
        if (it != panelNames.end())
            found = it->second;
        w = panel;
    }
    return found;
}

bool IsFocused(ImGuiWindow* w)
{
    ImGuiContext& g = *GImGui;
    if (!w || !g.NavWindow)
        return false;
    return g.NavWindow == w || g.NavWindow->RootWindow == w;
}

// Fill the live-ImGui half of a row. Leaves everything false when the window has never been
// submitted, which is the honest answer for a view that has not been opened this session.
void FillLiveState(UiWindowInfo& info, ImGuiWindow* w,
                   const std::unordered_map<ImGuiWindow*, std::string>& panelNames)
{
    if (!w)
        return;
    info.exists = true;
    info.visible = w->WasActive;
    info.collapsed = w->Collapsed;
    info.focused = IsFocused(w);
    info.x = w->Pos.x;
    info.y = w->Pos.y;
    info.w = w->Size.x;
    info.h = w->Size.y;

    if (ImGuiDockNode* node = w->DockNode) {
        info.docked = true;
        info.dockNodeId = node->ID;
        info.isSelectedTab = node->SelectedTabId == w->TabId;
        info.hostPanel = OutermostPanelName(w, panelNames);
    }
}

void CollectNodes(ImGuiDockNode* node, std::vector<UiDockNodeInfo>& out)
{
    if (!node)
        return;

    UiDockNodeInfo info;
    info.id = node->ID;
    info.parentId = node->ParentNode ? node->ParentNode->ID : 0;
    info.isLeaf = node->IsLeafNode();
    info.isCentral = node->IsCentralNode();
    info.hasHostWindow = node->HostWindow != nullptr;
    info.x = node->Pos.x;
    info.y = node->Pos.y;
    info.w = node->Size.x;
    info.h = node->Size.y;
    if (info.isLeaf)
        info.splitAxis = "none";
    else
        info.splitAxis = node->SplitAxis == ImGuiAxis_X   ? "x"
                         : node->SplitAxis == ImGuiAxis_Y ? "y"
                                                          : "none";

    for (int i = 0; i < node->Windows.Size; i++) {
        ImGuiWindow* w = node->Windows[i];
        if (!w)
            continue;
        std::string id = ImGuiIdTail(w->Name);
        info.windows.push_back(id);
        if (node->SelectedTabId != 0 && w->TabId == node->SelectedTabId)
            info.selectedTab = id;
    }

    for (int c = 0; c < 2; c++)
        if (node->ChildNodes[c])
            info.childIds.push_back(node->ChildNodes[c]->ID);

    out.push_back(std::move(info));

    for (int c = 0; c < 2; c++)
        CollectNodes(node->ChildNodes[c], out);
}

ImGuiDir ToImGuiDir(UiDockRequest::Direction d)
{
    switch (d) {
        case UiDockRequest::Direction::Left:
            return ImGuiDir_Left;
        case UiDockRequest::Direction::Right:
            return ImGuiDir_Right;
        case UiDockRequest::Direction::Up:
            return ImGuiDir_Up;
        case UiDockRequest::Direction::Down:
            return ImGuiDir_Down;
        default:
            return ImGuiDir_None;
    }
}

// Resolve a caller-supplied window key to its ImGui identity. Accepts the View-menu name or the
// imguiId itself, so both `"CIA"` and `"CIA Registers##debugger"` work.
bool ResolveWindowId(const std::string& key, const UiSources& src, std::string& outId,
                     std::string& outError)
{
    for (const auto& e : src.windows) {
        if (e.name == key || e.imguiId == key) {
            if (!e.isWindow) {
                outError = "'" + key + "' is a draw-list overlay, not a window";
                return false;
            }
            outId = LabelOf(e);
            return true;
        }
    }
    for (const auto& p : src.panels) {
        if (p.name == key) {
            outId = PanelIdTail(p);
            return true;
        }
    }
    // Not in the registry - it may still be a live window (a document, a file viewer).
    if (ImGui::FindWindowByName(key.c_str())) {
        outId = key;
        return true;
    }
    outError = "unknown window: '" + key + "'";
    return false;
}

} // namespace

std::string ImGuiIdTail(const char* label)
{
    if (!label)
        return {};
    const char* hash = nullptr;
    for (const char* p = label; *p; ++p)
        if (p[0] == '#' && p[1] == '#' && p[2] == '#')
            hash = p + 3;
    return hash ? std::string(hash) : std::string(label);
}

bool HasLiveUiContext() { return ImGui::GetCurrentContext() != nullptr; }

void FocusContainingPanels(const char* imguiId)
{
    // When a window is docked inside a PanelView's nested DockSpace, focusing it only selects its
    // tab in the *inner* node. The outer node(s) won't switch to the host panel's tab, because
    // DockNodeUpdateTabBar only auto-selects when g.NavWindow's root window is in that node - and a
    // nested docked window's root is itself (the inner node), never the outer one. (A real click
    // works only because the mouse is physically over the panel too.) So walk up the dock tree and
    // manually select each host panel's tab in its node. These won't be overridden, since the
    // focused window's root is not in those outer nodes.
    ImGuiWindow* w = ImGui::FindWindowByName(imguiId);
    int guard = 0;
    while (w && w->DockNode && guard++ < 8) {
        ImGuiDockNode* root = w->DockNode;
        while (root->ParentNode)
            root = root->ParentNode;
        ImGuiWindow* host = root->HostWindow;
        if (!host || !host->ParentWindow)
            break;
        ImGuiWindow* panel = host->ParentWindow;
        if (panel == w)
            break;
        if (ImGuiDockNode* pn = panel->DockNode) {
            if (pn->TabBar)
                pn->TabBar->NextSelectedTabId = panel->TabId;
            pn->SelectedTabId = panel->TabId;
        }
        w = panel;
    }
}

bool FindWindowRect(const char* imguiId, float& x, float& y, float& w, float& h)
{
    ImGuiWindow* win = ImGui::FindWindowByName(imguiId);
    if (!win)
        return false;
    x = win->Pos.x;
    y = win->Pos.y;
    w = win->Size.x;
    h = win->Size.y;
    return true;
}

UiLayoutSnapshot BuildLayoutSnapshot(const UiSources& src)
{
    UiLayoutSnapshot snap;
    if (!HasLiveUiContext())
        return snap;
    snap.available = true;
    snap.mainDockId = src.mainDockId;
    snap.layoutSettling = src.layoutSettling;

    // Panel window -> panel name, so every docked window can name the panel hosting it.
    std::unordered_map<ImGuiWindow*, std::string> panelNames;
    for (const auto& p : src.panels) {
        const std::string id = PanelIdTail(p);
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(static_cast<ImGuiID>(p.id));
        UiPanelInfo info;
        info.name = p.name;
        info.id = p.id;
        info.open = p.openPtr ? *p.openPtr : true;
        info.permanent = p.permanent;
        info.hosted = node && node->HostWindow != nullptr;
        snap.panels.push_back(std::move(info));
        if (ImGuiWindow* pw = Find(id))
            panelNames[pw] = p.name;
    }

    // Track which live windows the registry and panels account for, so the leftovers can be
    // reported too - a document's FileListView floating in the wrong place is exactly the kind of
    // thing this snapshot exists to reveal, and it is in no registry.
    std::unordered_map<ImGuiWindow*, bool> accounted;

    for (const auto& e : src.windows) {
        UiWindowInfo info;
        info.name = e.name;
        info.imguiId = LabelOf(e);
        info.group = e.group;
        info.registered = true;
        info.isWindow = e.isWindow;
        info.open = e.openPtr ? *e.openPtr : false;
        if (e.isWindow) {
            ImGuiWindow* w = Find(info.imguiId);
            FillLiveState(info, w, panelNames);
            if (w)
                accounted[w] = true;
        }
        snap.windows.push_back(std::move(info));
    }

    for (const auto& p : src.panels) {
        UiWindowInfo info;
        info.name = p.name;
        info.imguiId = PanelIdTail(p);
        info.registered = true;
        info.isPanel = true;
        info.open = p.openPtr ? *p.openPtr : true;
        ImGuiWindow* w = Find(info.imguiId);
        FillLiveState(info, w, panelNames);
        if (w)
            accounted[w] = true;
        snap.windows.push_back(std::move(info));
    }

    ImGuiContext& g = *GImGui;
    for (int i = 0; i < g.Windows.Size; i++) {
        ImGuiWindow* w = g.Windows[i];
        if (!w || accounted.count(w))
            continue;
        // Skip ImGui's own plumbing: dockspace hosts, tooltips, popups and menu overlays are not
        // things a caller can address.
        if (w->Flags & ImGuiWindowFlags_ChildWindow)
            continue;
        if (!w->WasActive)
            continue;
        if (w->Name && (std::strstr(w->Name, "/DockSpace_") || std::strncmp(w->Name, "##", 2) == 0))
            continue;

        UiWindowInfo info;
        info.name = ImGuiIdTail(w->Name);
        info.imguiId = info.name;
        info.registered = false;
        FillLiveState(info, w, panelNames);
        info.open = info.exists;
        snap.windows.push_back(std::move(info));
    }

    CollectNodes(ImGui::DockBuilderGetNode(static_cast<ImGuiID>(src.mainDockId)), snap.nodes);
    for (const auto& p : src.panels)
        CollectNodes(ImGui::DockBuilderGetNode(static_cast<ImGuiID>(p.id)), snap.nodes);

    return snap;
}

bool ApplyDockRequest(const UiDockRequest& req, const UiSources& src, std::string& outError)
{
    if (!HasLiveUiContext()) {
        outError = "no ImGui context";
        return false;
    }
    // Docking into a layout that is still settling loses the change: ApplyPendingWorkspaceLayout
    // detaches every window from every node, and ReassertWorkspaceTabSelection re-pushes saved tab
    // selections for 60 frames. Refuse rather than silently snap back.
    if (src.layoutSettling) {
        outError = "a workspace layout is still settling; retry in a moment";
        return false;
    }

    std::string windowId;
    if (!ResolveWindowId(req.window, src, windowId, outError))
        return false;

    if (req.target == UiDockRequest::Target::Float) {
        ImGui::DockBuilderDockWindow(windowId.c_str(), 0);
        if (req.w > 0 && req.h > 0)
            ImGui::SetWindowSize(windowId.c_str(), ImVec2(req.w, req.h), ImGuiCond_Always);
        if (req.x != 0 || req.y != 0)
            ImGui::SetWindowPos(windowId.c_str(), ImVec2(req.x, req.y), ImGuiCond_Always);
        return true;
    }

    ImGuiID nodeId = 0;
    switch (req.target) {
        case UiDockRequest::Target::Main:
            nodeId = static_cast<ImGuiID>(src.mainDockId);
            break;
        case UiDockRequest::Target::Panel: {
            const UiPanelEntry* panel = nullptr;
            for (const auto& p : src.panels)
                if (p.name == req.targetName)
                    panel = &p;
            if (!panel) {
                outError = "unknown panel: '" + req.targetName + "'";
                return false;
            }
            ImGuiDockNode* node = ImGui::DockBuilderGetNode(static_cast<ImGuiID>(panel->id));
            // A panel that has never submitted its DockSpace has no host window, and anything
            // docked into it renders as a stray floating window - the exact failure
            // PrimePanelDockHosts repairs. Refuse with an actionable message instead of creating
            // it.
            if (!node || !node->HostWindow) {
                outError =
                    "panel '" + req.targetName +
                    "' has no DockSpace host yet (it is a hidden tab); open or focus it first";
                return false;
            }
            nodeId = static_cast<ImGuiID>(panel->id);
            break;
        }
        case UiDockRequest::Target::Window: {
            std::string otherId;
            if (!ResolveWindowId(req.targetName, src, otherId, outError))
                return false;
            ImGuiWindow* other = ImGui::FindWindowByName(otherId.c_str());
            if (!other || !other->DockNode) {
                outError = "target window '" + req.targetName +
                           "' is not docked; dock it first or use target 'main'/'panel'";
                return false;
            }
            nodeId = other->DockNode->ID;
            break;
        }
        default:
            break;
    }

    if (!ImGui::DockBuilderGetNode(nodeId)) {
        outError = "dock node does not exist yet";
        return false;
    }

    if (req.direction == UiDockRequest::Direction::Tab) {
        ImGui::DockBuilderDockWindow(windowId.c_str(), nodeId);
    } else {
        ImGuiID atDir = 0, opposite = 0;
        const float ratio = std::clamp(req.ratio, 0.05f, 0.95f);
        ImGuiID split = ImGui::DockBuilderSplitNode(nodeId, ToImGuiDir(req.direction), ratio,
                                                    &atDir, &opposite);
        (void)split;
        ImGui::DockBuilderDockWindow(windowId.c_str(), atDir);
    }

    ImGuiDockNode* node = ImGui::DockBuilderGetNode(nodeId);
    ImGuiDockNode* root = node;
    while (root && root->ParentNode)
        root = root->ParentNode;
    ImGui::DockBuilderFinish(root ? root->ID : nodeId);
    return true;
}

std::string SaveImGuiIni()
{
    if (!HasLiveUiContext())
        return {};
    size_t size = 0;
    const char* data = ImGui::SaveIniSettingsToMemory(&size);
    return data ? std::string(data, size) : std::string{};
}

} // namespace adm::ui
