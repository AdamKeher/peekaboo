#include "FileListView.h"
#include "../IconsFontAwesome6.h"
#include "../TableLayout.h"

#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace adm::ui {

void FileListView::SetRoot(const adm::core::FileNode& root)
{
    m_root = root;
    m_forceSort = true;
    m_lastSelectedSector = -1;
    ClearSelection();
}

void FileListView::SetOnFileDoubleClicked(std::function<void(uint32_t)> callback)
{
    m_onFileDoubleClicked = callback;
}

void FileListView::SetOnOpenFile(std::function<void(uint32_t, const std::string&)> callback)
{
    m_onOpenFile = callback;
}

void FileListView::SetOnViewImage(std::function<void(uint32_t, const std::string&)> callback)
{
    m_onViewImage = callback;
}

void FileListView::SetOnListenAudio(std::function<void(uint32_t, const std::string&)> callback)
{
    m_onListenAudio = callback;
}

void FileListView::SetOnViewHunks(std::function<void(uint32_t, const std::string&)> callback)
{
    m_onViewHunks = callback;
}

void FileListView::SetOnSaveFile(std::function<void(uint32_t, const std::string&)> callback)
{
    m_onSaveFile = callback;
}

void FileListView::SetOnRenameFile(
    std::function<void(uint32_t, uint32_t, const std::string&, const std::string&)> callback)
{
    m_onRenameFile = callback;
}

void FileListView::SetOnToggleWriteProtect(std::function<void(uint32_t, bool)> callback)
{
    m_onToggleWriteProtect = std::move(callback);
}

void FileListView::DrawWriteProtectButton()
{
    // Right-aligned on the panel's first row - the top-right corner of the document,
    // which unlike a real title bar survives being docked into a tab.
    const float btnW = ImGui::GetFrameHeight();
    const float startX = ImGui::GetCursorPosX();
    const float startY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - btnW);

    const ImVec4 locked(0.95f, 0.72f, 0.25f, 1.0f);
    const ImVec4 unlocked(0.55f, 0.55f, 0.55f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, m_writeProtected ? locked : unlocked);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    const bool lockHeld = !m_writeProtectLockReason.empty();
    ImGui::BeginDisabled(lockHeld);
    const bool pressed = ImGui::Button(m_writeProtected ? ICON_FA_LOCK "##WriteProtect"
                                                        : ICON_FA_LOCK_OPEN "##WriteProtect",
                                       ImVec2(btnW, btnW));
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (lockHeld) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", m_writeProtectLockReason.c_str());
    } else {
        ImGui::SetItemTooltip(m_writeProtected
                                  ? "Write-protected - click to allow changes to this image"
                                  : "Writable - click to write-protect this image");
    }
    if (pressed && !lockHeld && m_onToggleWriteProtect)
        m_onToggleWriteProtect(m_documentId, !m_writeProtected);

    // Rewind so the rest of the panel starts on the same row, to the left of the padlock.
    ImGui::SetCursorPos(ImVec2(startX, startY));
}

void FileListView::DrawPartitionSelector()
{
    if (m_partitions.empty())
        return;
    // Choice 0 is the whole image; partition i is choice i + 1.
    const int current = m_selectedPartition + 1;
    const char* preview = current >= 0 && current <= static_cast<int>(m_partitions.size()) - 1
                              ? m_partitions[static_cast<size_t>(current)].label.c_str()
                              : "";
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Partition");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(
        std::max(120.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() * 1.5f));
    // The choice is acted on after EndCombo: the callback replaces m_partitions (via
    // SetPartitions), which would free the element the loop is still reading.
    int pick = -1;
    if (ImGui::BeginCombo("##Partition", preview)) {
        for (size_t i = 0; i < m_partitions.size(); ++i) {
            const bool mountable = m_partitions[i].mountable;
            if (!mountable)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const bool selected = static_cast<int>(i) == current;
            if (ImGui::Selectable(m_partitions[i].label.c_str(), selected) && !selected)
                pick = static_cast<int>(i);
            if (!mountable) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("No filesystem the disk manager can read - it opens as raw "
                                      "blocks of the whole image.");
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Which partition of this hard-disk image the file list, block map and "
                          "hex views show. Block numbers are counted from the partition's start.");
    if (pick >= 0 && m_onSelectPartition)
        m_onSelectPartition(m_documentId, pick - 1);
}

void FileListView::SetOnImportFile(std::function<void(uint32_t)> callback)
{
    m_onImportFile = callback;
}

void FileListView::SetOnRunFile(std::function<void(uint32_t, const std::string&)> callback)
{
    m_onRunFile = callback;
}

void FileListView::SetOnCreateDir(std::function<void(uint32_t, const std::string&)> callback)
{
    m_onCreateDir = std::move(callback);
}

void FileListView::SetOnDeleteFiles(
    std::function<void(const std::vector<std::pair<uint32_t, std::string>>&)> callback)
{
    m_onDeleteFiles = std::move(callback);
}

void FileListView::SetOnSetPermissions(
    std::function<void(const std::vector<uint32_t>&, int32_t)> callback)
{
    m_onSetPermissions = std::move(callback);
}

void FileListView::SetOnScrubFiles(
    std::function<void(const std::vector<std::pair<uint32_t, std::string>>&)> callback)
{
    m_onScrubFiles = std::move(callback);
}

void FileListView::SetOnMoveFile(
    std::function<bool(uint32_t srcParent, const std::string& name, uint32_t destParent)> callback)
{
    m_onMoveFile = std::move(callback);
}

void FileListView::SetOnMoveFiles(
    std::function<void(const std::vector<std::tuple<uint32_t, std::string, uint32_t>>&)> callback)
{
    m_onMoveFiles = std::move(callback);
}

void FileListView::SetOnExportFiles(
    std::function<void(const std::vector<adm::core::FileNode>&)> callback)
{
    m_onExportFiles = std::move(callback);
}

void FileListView::SetOnCrossDocumentDrop(
    std::function<void(uint32_t sourceDocId, const std::vector<uint32_t>& sectors,
                       uint32_t destParentSector, bool isCopy)>
        callback)
{
    m_onCrossDocumentDrop = std::move(callback);
}

void FileListView::SetFilename(const std::string& filename) { m_filename = filename; }

void FileListView::SetDemosceneSource(std::optional<core::DemosceneSource> source)
{
    m_demosceneSource = std::move(source);
}

void FileListView::SetOnClose(std::function<void(uint32_t)> callback)
{
    m_onClose = std::move(callback);
}

namespace {
std::string HumanBytes(int64_t bytes)
{
    if (bytes < 0)
        return "?";
    double b = static_cast<double>(bytes);
    if (b < 1024.0)
        return fmt::format("{} B", bytes);
    if (b < 1024.0 * 1024.0)
        return fmt::format("{:.1f} KB", b / 1024.0);
    return fmt::format("{:.2f} MB", b / (1024.0 * 1024.0));
}
} // namespace

void FileListView::DrawDownloadPlaceholder()
{
    const DownloadStatus& s = *m_downloadStatus;

    ImGui::Spacing();
    ImGui::TextUnformatted(m_filename.empty() ? "Downloading..." : m_filename.c_str());

    if (!s.sourceName.empty()) {
        ImGui::TextDisabled("Source:");
        ImGui::SameLine();
        if (!s.sourceUrl.empty())
            ImGui::TextLinkOpenURL(s.sourceName.c_str(), s.sourceUrl.c_str());
        else
            ImGui::TextDisabled("%s", s.sourceName.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (s.isError) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                           s.errorTitle.empty() ? "Download failed" : s.errorTitle.c_str());
        if (!s.errorMsg.empty())
            ImGui::TextWrapped("%s", s.errorMsg.c_str());
        ImGui::Spacing();
        if (s.showLocate) {
            if (ImGui::Button("Locate...") && m_onLocateDirectory)
                m_onLocateDirectory(m_documentId);
            ImGui::SameLine();
        }
        if (ImGui::Button("Retry") && m_onRetryDownload)
            m_onRetryDownload(m_documentId);
        ImGui::SameLine();
        if (ImGui::Button("Close") && m_onClose)
            m_onClose(m_documentId);
        return;
    }

    // Progress bar (indeterminate when total size is unknown).
    if (s.progress >= 0.0f) {
        char overlay[32];
        snprintf(overlay, sizeof(overlay), "%.0f%%", s.progress * 100.0f);
        ImGui::ProgressBar(s.progress, ImVec2(-1.0f, 0.0f), overlay);
    } else {
        // Animated indeterminate bar: a sweeping fraction.
        float t = static_cast<float>(ImGui::GetTime());
        float frac = 0.5f + 0.5f * sinf(t * 2.0f);
        ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), "Downloading...");
    }

    ImGui::Spacing();

    std::string received = HumanBytes(static_cast<int64_t>(s.bytesReceived));
    if (s.totalBytes > 0)
        ImGui::Text("%s / %s", received.c_str(), HumanBytes(s.totalBytes).c_str());
    else
        ImGui::Text("%s received", received.c_str());

    ImGui::SameLine();
    ImGui::TextDisabled("   %s/s", HumanBytes(static_cast<int64_t>(s.bytesPerSec)).c_str());

    if (!s.statusMsg.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", s.statusMsg.c_str());
    }

    ImGui::Spacing();
    if (ImGui::Button("Cancel") && m_onClose)
        m_onClose(m_documentId);
}

void FileListView::SetOnRestoreFile(std::function<bool(uint32_t)> callback)
{
    m_onRestoreFile = std::move(callback);
}

bool FileListView::IsSelected(int32_t sector) const
{
    return m_selectedSectors.find(sector) != m_selectedSectors.end();
}

void FileListView::ToggleSelection(int32_t sector)
{
    if (IsSelected(sector)) {
        m_selectedSectors.erase(sector);
    } else {
        m_selectedSectors.insert(sector);
    }
}

void FileListView::Select(int32_t sector) { m_selectedSectors.insert(sector); }

void FileListView::Deselect(int32_t sector) { m_selectedSectors.erase(sector); }

void FileListView::ClearSelection() { m_selectedSectors.clear(); }

void FileListView::EnsureSelectedForContext(int32_t sector)
{
    if (!IsSelected(sector)) {
        ClearSelection();
        Select(sector);
        m_lastSelectedSector = sector;
    }
}

int32_t FileListView::s_dragHoverDocId = -1;
int FileListView::s_dragHoverFrame = -1;
FileListView::Clipboard FileListView::s_clipboard;

const char* FileListView::DragActionVerb() const
{
    // Hover state is only meaningful for the frame it was recorded (allow one
    // frame of lag for targets in windows drawn after this one). Once the cursor
    // leaves every target it goes stale and we fall back to the move default.
    bool overTarget = s_dragHoverDocId != -1 && (ImGui::GetFrameCount() - s_dragHoverFrame) <= 1;
    bool crossDocument = overTarget && s_dragHoverDocId != static_cast<int32_t>(m_documentId);
    // Cross-document drops copy by default and move with Shift; intra-document drops move.
    bool willCopy = crossDocument && !ImGui::GetIO().KeyShift;
    return willCopy ? "Copy" : "Move";
}

void FileListView::CollectSelectedNodes(const adm::core::FileNode& node,
                                        std::vector<std::pair<uint32_t, std::string>>& outItems)
{
    if (IsSelected(node.startSector)) {
        outItems.emplace_back(node.parentSector, node.name);
    }
    for (const auto& child : node.children) {
        CollectSelectedNodes(child, outItems);
    }
}

void FileListView::SetHighlightedFileHeader(uint32_t head) { m_highlightedHead = head; }

// Helper Methods Implementation
void FileListView::TriggerNewFolder()
{
    m_openNewFolderPopup = true;
    m_newFolderParentSector = 0; // Root (Default, or we could track current dir)
    memset(m_newFolderBuffer, 0, sizeof(m_newFolderBuffer));
    m_newFolderError.clear();
}

void FileListView::TriggerDelete()
{
    m_itemsToDelete.clear();
    CollectSelectedNodes(m_root, m_itemsToDelete);

    if (!m_itemsToDelete.empty()) {
        m_openDeletePopup = true;
        if (m_itemsToDelete.size() == 1) {
            m_deleteConfirmationMessage =
                "Are you sure you want to delete '" + m_itemsToDelete[0].second + "'?";
        } else {
            m_deleteConfirmationMessage = "Are you sure you want to delete " +
                                          std::to_string(m_itemsToDelete.size()) + " items?";
        }
    }
}

void FileListView::TriggerScrub()
{
    m_itemsToScrub.clear();
    std::set<int32_t> seen;
    auto addItem = [&](const adm::core::FileNode& n) {
        // The synthetic [Deleted Files] folder has startSector -1 (no real block);
        // skip it and dedupe in case a folder and one of its children are both selected.
        if (n.startSector >= 0 && seen.insert(n.startSector).second) {
            m_itemsToScrub.emplace_back(n.startSector, n.name);
        }
    };
    auto collectStartSectors = [&](auto&& self, const adm::core::FileNode& n) -> void {
        if (IsSelected(n.startSector)) {
            // Selecting the [Deleted Files] folder scrubs every deleted entry it holds.
            if (n.name == "[Deleted Files]") {
                for (const auto& c : n.children) {
                    addItem(c);
                }
            } else {
                addItem(n);
            }
        }
        for (const auto& c : n.children) {
            self(self, c);
        }
    };
    collectStartSectors(collectStartSectors, m_root);

    if (!m_itemsToScrub.empty()) {
        m_openScrubPopup = true;
        std::string targetName = (m_itemsToScrub.size() == 1)
                                     ? "'" + m_itemsToScrub[0].second + "'"
                                     : std::to_string(m_itemsToScrub.size()) + " items";

        m_scrubConfirmationMessage =
            "Are you sure you want to scrub (securely delete) " + targetName + "?";
    }
}

void FileListView::TriggerRename()
{
    std::vector<std::pair<uint32_t, std::string>> items;
    CollectSelectedNodes(m_root, items);
    if (items.size() == 1) {
        m_openRenamePopup = true;
        auto findNode = [&](auto&& self,
                            const adm::core::FileNode& n) -> const adm::core::FileNode* {
            if (n.startSector == *m_selectedSectors.begin())
                return &n;
            for (const auto& c : n.children) {
                auto res = self(self, c);
                if (res)
                    return res;
            }
            return nullptr;
        };

        const auto* node = findNode(findNode, m_root);
        if (node) {
            m_renameOldName = node->name;
            m_renameParentSector = node->parentSector;
            m_renameStartSector = node->startSector;
            memset(m_renameBuffer, 0, sizeof(m_renameBuffer));
            strncpy(m_renameBuffer, node->name.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameError.clear();
        }
    }
}

void FileListView::TriggerPermissions()
{
    m_permissionsSectors.clear();
    for (int32_t sector : m_selectedSectors) {
        m_permissionsSectors.push_back(static_cast<uint32_t>(sector));
    }
    if (!m_permissionsSectors.empty()) {
        m_openPermissionsPopup = true;
        // Find the first node to get its permissions
        auto findNodeBySector = [&](auto&& self, const adm::core::FileNode& n,
                                    uint32_t targetSector) -> const adm::core::FileNode* {
            if (static_cast<uint32_t>(n.startSector) == targetSector)
                return &n;
            for (const auto& c : n.children) {
                auto res = self(self, c, targetSector);
                if (res)
                    return res;
            }
            return nullptr;
        };
        const auto* firstNode = findNodeBySector(findNodeBySector, m_root, m_permissionsSectors[0]);
        if (firstNode) {
            if (m_permissionsSectors.size() == 1) {
                m_permissionsFileName = firstNode->name;
            } else {
                m_permissionsFileName = std::to_string(m_permissionsSectors.size()) + " items";
            }
            // Set individual flags from first file's access value
            m_permHidden = (firstNode->access & (1 << 7)) != 0;
            m_permScript = (firstNode->access & (1 << 6)) != 0;
            m_permPure = (firstNode->access & (1 << 5)) != 0;
            m_permArchived = (firstNode->access & (1 << 4)) != 0;
            m_permRead = (firstNode->access & (1 << 3)) == 0;
            m_permWrite = (firstNode->access & (1 << 2)) == 0;
            m_permExecute = (firstNode->access & (1 << 1)) == 0;
            m_permDelete = (firstNode->access & (1 << 0)) == 0;
        }
    }
}

void FileListView::TriggerRestore()
{
    for (int32_t sector : m_selectedSectors) {
        if (m_onRestoreFile) {
            m_onRestoreFile(static_cast<uint32_t>(sector));
        }
    }
    ClearSelection();
}

void FileListView::TriggerExport()
{
    std::vector<adm::core::FileNode> nodesToExport;
    auto collectNodes = [&](auto&& self, const adm::core::FileNode& n) -> void {
        if (IsSelected(n.startSector)) {
            nodesToExport.push_back(n);
        } else {
            for (const auto& c : n.children) {
                self(self, c);
            }
        }
    };
    collectNodes(collectNodes, m_root);

    if (!nodesToExport.empty() && m_onExportFiles) {
        m_onExportFiles(nodesToExport);
    }
}

void FileListView::CollectTopLevelSelectedSectors(const adm::core::FileNode& node,
                                                  std::vector<uint32_t>& out) const
{
    // A selected folder represents its whole subtree; don't descend past a
    // selected node. Skip the synthetic [Deleted Files] container (sector < 0)
    // and deleted entries, which can't be meaningfully copied.
    if (IsSelected(node.startSector) && node.startSector >= 0 && !node.isDeleted) {
        out.push_back(static_cast<uint32_t>(node.startSector));
        return;
    }
    for (const auto& child : node.children) {
        CollectTopLevelSelectedSectors(child, out);
    }
}

void FileListView::TriggerCopy()
{
    std::vector<uint32_t> sectors;
    CollectTopLevelSelectedSectors(m_root, sectors);
    if (sectors.empty())
        return;
    s_clipboard.docId = m_documentId;
    s_clipboard.sectors = std::move(sectors);
}

void FileListView::TriggerPaste(uint32_t destParentSector)
{
    if (s_clipboard.sectors.empty() || !m_onPasteFiles)
        return;
    m_onPasteFiles(s_clipboard.docId, s_clipboard.sectors, destParentSector);
}

bool FileListView::CanCopy() const
{
    // At least one real (non-deleted) item must be selected.
    std::vector<uint32_t> sectors;
    CollectTopLevelSelectedSectors(m_root, sectors);
    return !sectors.empty();
}

bool FileListView::CanPaste() const { return !s_clipboard.sectors.empty(); }

void FileListView::SetOnPasteFiles(
    std::function<void(uint32_t, const std::vector<uint32_t>&, uint32_t)> callback)
{
    m_onPasteFiles = std::move(callback);
}

bool FileListView::CanCreateNewFolder() const
{
    // Only enabled if a disk is loaded (root has children or name is not empty if root folder is
    // empty)
    return m_capabilities.createDirectory && !m_root.name.empty();
}

bool FileListView::CanDelete() const
{
    if (!m_capabilities.deleteEntries)
        return false;
    if (m_selectedSectors.empty())
        return false;

    // Check if any selected item is NOT deleted
    auto findNodeBySector = [&](auto&& self, const adm::core::FileNode& n,
                                int32_t targetSector) -> const adm::core::FileNode* {
        if (n.startSector == targetSector)
            return &n;
        for (const auto& c : n.children) {
            auto res = self(self, c, targetSector);
            if (res)
                return res;
        }
        return nullptr;
    };

    for (int32_t sector : m_selectedSectors) {
        const auto* node = findNodeBySector(findNodeBySector, m_root, sector);
        if (node && !node->isDeleted) {
            return true;
        }
    }
    return false;
}

bool FileListView::CanScrub() const
{
    return m_capabilities.rawBlocks && m_capabilities.writeBlocks && !m_selectedSectors.empty();
}

bool FileListView::CanRename() const
{
    return m_capabilities.rename && m_selectedSectors.size() == 1;
}

bool FileListView::CanSetPermissions() const
{
    return m_capabilities.rawBlocks && m_capabilities.writeBlocks && !m_selectedSectors.empty();
}

bool FileListView::CanRestore() const
{
    if (!m_capabilities.rawBlocks || !m_capabilities.writeBlocks)
        return false;
    if (m_selectedSectors.empty())
        return false;

    // Check if any selected item IS a deleted file
    auto findNodeBySector = [&](auto&& self, const adm::core::FileNode& n,
                                int32_t targetSector) -> const adm::core::FileNode* {
        if (n.startSector == targetSector)
            return &n;
        for (const auto& c : n.children) {
            auto res = self(self, c, targetSector);
            if (res)
                return res;
        }
        return nullptr;
    };
    for (int32_t sector : m_selectedSectors) {
        const auto* node = findNodeBySector(findNodeBySector, m_root, sector);
        if (node && node->isDeleted && !node->isDir) {
            return true;
        }
    }
    return false;
}

bool FileListView::CanExport() const { return !m_selectedSectors.empty(); }

void FileListView::SelectRange(int32_t startSector, int32_t endSector, bool clearFirst)
{
    if (m_visibleSectors.empty())
        return;

    auto itStart = std::find(m_visibleSectors.begin(), m_visibleSectors.end(), startSector);
    auto itEnd = std::find(m_visibleSectors.begin(), m_visibleSectors.end(), endSector);

    if (itStart == m_visibleSectors.end() || itEnd == m_visibleSectors.end())
        return;

    if (std::distance(m_visibleSectors.begin(), itStart) >
        std::distance(m_visibleSectors.begin(), itEnd)) {
        std::swap(itStart, itEnd);
    }

    if (clearFirst) {
        ClearSelection();
    }
    for (auto it = itStart; it <= itEnd; ++it) {
        Select(*it);
    }
}

void FileListView::CollectVisibleNodes(const adm::core::FileNode& /*node*/,
                                       std::vector<int32_t>& /*outNodes*/)
{
    // Not used in current implementation (using m_visibleSectors frame accumulation)
}

void FileListView::HandleItemSelection(int32_t sector, bool isCheckbox, bool isTreeNodeToggledOpen)
{
    // If the expand/collapse arrow was clicked, don't change selection
    if (isTreeNodeToggledOpen) {
        return;
    }

    // Checkbox clicked: toggle selection (like Ctrl-click) but don't update anchor
    if (isCheckbox) {
        ToggleSelection(sector);
        // Note: anchor (m_lastSelectedSector) is NOT updated on checkbox click
        return;
    }

    // Row/name clicked: handle based on modifier keys
    bool ctrlOrCmd = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
    bool shift = ImGui::GetIO().KeyShift;

    if (shift && m_lastSelectedSector != -1) {
        // Shift-click: Select range from anchor to this item
        // Ctrl+Shift adds range to existing selection, Shift alone replaces
        SelectRange(m_lastSelectedSector, sector, !ctrlOrCmd);
        // Note: anchor is NOT updated on Shift-click
    } else if (ctrlOrCmd) {
        // Ctrl/Cmd-click: Toggle selection, update anchor
        ToggleSelection(sector);
        m_lastSelectedSector = sector;
    } else {
        // Plain click: If clicking on an already-selected item, keep selection
        // (allows dragging multiple files). Otherwise clear and select just this one.
        if (!IsSelected(sector)) {
            ClearSelection();
            Select(sector);
        }
        m_lastSelectedSector = sector;
    }
}

void FileListView::Draw(const adm::core::DiskInfo& diskInfo, bool hasUnsavedChanges, float dpiScale)
{
    m_isDropTargetWindow = false; // reset each frame; set to true below if hovered
    bool contextChanged = (m_lastHighlightedHead != m_highlightedHead);

    ImGuiWindowFlags windowFlags = 0;
    if (hasUnsavedChanges) {
        windowFlags |= ImGuiWindowFlags_UnsavedDocument;
    }

    std::string title;
    if (m_documentId > 0) {
        std::string displayName = m_filename.empty() ? "File Browser" : m_filename;
        title = fmt::format("{}###FileListView_{}", displayName, m_documentId);
    } else {
        title = m_filename.empty() ? "File Browser" : m_filename + "###File Browser";
    }

    bool open = true;
    bool* pOpen = (m_documentId > 0) ? &open : nullptr;

    if (m_requestFocus) {
        ImGui::SetNextWindowFocus();
        m_requestFocus = false;
    }

    if (m_forceFloat) {
        // Undock and pin to the captured position/size so a workspace switch
        // doesn't fling the floating window to a random spot.
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(m_targetPosX, m_targetPosY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(m_targetSizeX, m_targetSizeY), ImGuiCond_Always);
        m_forceFloat = false;
    } else if (m_targetDockId != 0) {
        ImGui::SetNextWindowDockID(m_targetDockId,
                                   m_forceDock ? ImGuiCond_Always : ImGuiCond_Appearing);
        m_targetDockId = 0;
        m_forceDock = false;
    } else if (m_hasTargetPos && m_hasTargetSize) {
        ImGui::SetNextWindowPos(ImVec2(m_targetPosX, m_targetPosY), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(m_targetSizeX, m_targetSizeY), ImGuiCond_Appearing);
        m_hasTargetPos = false;
        m_hasTargetSize = false;
    }

    const bool windowVisible = ImGui::Begin(title.c_str(), pOpen, windowFlags);
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    m_windowPlacement = {
        windowPos.x, windowPos.y, windowSize.x, windowSize.y, ImGui::GetWindowDockID(), true};
    if (windowVisible) {
        m_hasFocus = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

        // While an ADF file drag is in progress, record which document's window the
        // cursor is over. The drag source uses this in DragActionVerb() to show
        // "Move" for an intra-document drop or "Copy" for a cross-document drop. Checking the
        // whole window (incl. child table) is more reliable than per-item drop targets.
        // isDropTargetWindow gates the background BeginDragDropTargetCustom so only
        // the hovered document shows its window-level yellow border — not the source.
        bool isDropTargetWindow = false;
        if (const ImGuiPayload* dragPayload = ImGui::GetDragDropPayload()) {
            if (dragPayload->IsDataType("DOCUMENT_ENTRIES_MOVE") &&
                ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                       ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                s_dragHoverDocId = static_cast<int32_t>(m_documentId);
                s_dragHoverFrame = ImGui::GetFrameCount();
                isDropTargetWindow = true;
                m_isDropTargetWindow = true;
            }
        }

        // Download placeholder: show progress / error instead of the disk view.
        if (m_downloadStatus) {
            DrawDownloadPlaceholder();
            ImGui::End();
            if (!open && m_onClose) {
                m_onClose(m_documentId);
            }
            return;
        }

        // IPF documents present the disk view and the per-track map as two tabs
        // contained within this document window; other formats render directly.
        const bool useDocTabs = m_trackView.HasData() || m_dmsView.HasData();
        bool drawDiskInfo = true;
        if (useDocTabs) {
            ImGui::BeginTabBar("##DocTabs");
            drawDiskInfo = ImGui::BeginTabItem("Disk Info");
        }
        if (drawDiskInfo) {
            // Disk Info Panel

            DrawWriteProtectButton();
            DrawPartitionSelector();

            if (!diskInfo.name.empty()) {
                if (m_demosceneSource) {
                    ImGui::Text("Volume: %s", diskInfo.name.c_str());
                    ImGui::SameLine();
                    const auto& src = *m_demosceneSource;
                    std::string siteName;
                    std::string url;
                    if (src.source == "tosec") {
                        ImGui::Text("(source: TOSEC)");
                    } else {
                        if (src.source == "demozoo") {
                            siteName = "demozoo.org";
                            url = fmt::format("https://demozoo.org/productions/{}/", src.releaseId);
                        } else if (src.source == "pouet") {
                            siteName = "pouet.net";
                            url = fmt::format("https://www.pouet.net/prod.php?which={}",
                                              src.releaseId);
                        } else if (src.source == "janeway") {
                            siteName = "janeway.exotica.org.uk";
                            url = fmt::format("https://janeway.exotica.org.uk/release.php?id={}",
                                              src.releaseId);
                        }
                        if (!siteName.empty()) {
                            std::string linkLabel = fmt::format("#{}", src.releaseId);
                            ImGui::Text("(source: %s -", siteName.c_str());
                            ImGui::SameLine();
                            ImGui::TextLinkOpenURL(linkLabel.c_str(), url.c_str());
                            ImGui::SameLine();
                            ImGui::Text(")");
                        }
                    }
                } else {
                    ImGui::Text("Volume: %s", diskInfo.name.c_str());
                }
                ImGui::Text("File System: %s", diskInfo.type.c_str());
                if (!diskInfo.runtimeStatus.empty()) {
                    const ImVec4 statusColor =
                        diskInfo.hostActivity == 2   ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                        : diskInfo.hostActivity == 1 ? ImVec4(0.3f, 1.0f, 0.45f, 1.0f)
                                                     : ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
                    ImGui::TextColored(statusColor, "%s", diskInfo.runtimeStatus.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("R: %llu  W: %llu",
                                        static_cast<unsigned long long>(diskInfo.hostReads),
                                        static_cast<unsigned long long>(diskInfo.hostWrites));
                }
                if (!diskInfo.warning.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f), "%s",
                                       diskInfo.warning.c_str());
                ImGui::Text("Size: %.2f KB  Free: %.2f KB", diskInfo.sizeBytes / 1024.0f,
                            diskInfo.freeBytes / 1024.0f);

                char buf[32];
                snprintf(buf, 32, "%.1f%% Used", diskInfo.usedPercentage * 100.0f);
                ImGui::ProgressBar(diskInfo.usedPercentage, ImVec2(-1.0f, 0.0f), buf);

                if (diskInfo.isReadOnly) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Read Only");
                }

                ImGui::Separator();

                // Toolbar
                ImVec2 btnSize(32.0f * dpiScale, 32.0f * dpiScale);

                if (ImGui::Button(ICON_FA_UPLOAD "##Import", btnSize)) {
                    if (m_onImportFile) {
                        m_onImportFile(0); // Import to Root
                    }
                }
                ImGui::SetItemTooltip("Import Files...");

                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_FOLDER_PLUS "##NewFolder", btnSize)) {
                    TriggerNewFolder();
                }
                ImGui::SetItemTooltip("New Folder...");

                ImGui::SameLine();
                ImGui::BeginDisabled(!CanDelete());
                if (ImGui::Button(ICON_FA_TRASH "##Delete", btnSize)) {
                    TriggerDelete();
                }
                ImGui::SetItemTooltip("Delete");
                ImGui::EndDisabled();

                ImGui::SameLine();
                // Scrub Button
                ImGui::BeginDisabled(!CanScrub());
                if (ImGui::Button(ICON_FA_ERASER "##Scrub", btnSize)) {
                    TriggerScrub();
                }
                ImGui::SetItemTooltip("Scrub");
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(!CanRename());
                if (ImGui::Button(ICON_FA_PEN "##Rename", btnSize)) {
                    TriggerRename();
                }
                ImGui::SetItemTooltip("Rename");
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(!CanSetPermissions());
                if (ImGui::Button(ICON_FA_KEY "##Permissions", btnSize)) {
                    TriggerPermissions();
                }
                ImGui::SetItemTooltip("Permissions");
                ImGui::EndDisabled();

                // Restore button - only enabled when deleted files are selected
                ImGui::SameLine();
                ImGui::BeginDisabled(!CanRestore());
                if (ImGui::Button(ICON_FA_ROTATE_LEFT "##Restore", btnSize)) {
                    TriggerRestore();
                }
                ImGui::SetItemTooltip("Restore");
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(!CanExport());
                if (ImGui::Button(ICON_FA_FILE_EXPORT "##Export", btnSize)) {
                    TriggerExport();
                }
                ImGui::SetItemTooltip("Export...");
                ImGui::EndDisabled();

                ImGui::Spacing();
                ImGui::Separator();
            }

            if (ImGui::BeginChild("##FileTableRegion")) {
                if (m_root.children.empty() && m_root.name.empty()) {
                    ImGui::TextDisabled("No disk loaded or empty directory.");
                } else {
                    // BeginTableEx with a fixed ID, not BeginTable: the window here is named
                    // per document (###FileListView_<id>), so a plain BeginTable would hash a
                    // different table ID for every disk opened - each with its own settings
                    // entry, none of them carrying over to the next disk, and the store growing
                    // a dead entry per document for ever. That is the staleness NoSavedSettings
                    // was added to paper over. One ID instead means one File Browser column
                    // layout, shared by every open document and saved once.
                    if (ImGui::BeginTableEx("FileTree", StableTableId("adm.FileListView.FileTree"),
                                            6,
                                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                ImGuiTableFlags_NoBordersInBody |
                                                ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
                                                ImGuiTableFlags_ScrollX | kAdjustableColumns)) {
                        ImGui::TableSetupScrollFreeze(0, 1); // Freeze header row
                        ImGui::TableSetupColumn("Name",
                                                ImGuiTableColumnFlags_DefaultSort |
                                                    ImGuiTableColumnFlags_PreferSortDescending |
                                                    ImGuiTableColumnFlags_NoHide |
                                                    ImGuiTableColumnFlags_NoReorder |
                                                    ImGuiTableColumnFlags_WidthFixed,
                                                300.0f * dpiScale);
                        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed,
                                                100.0f * dpiScale);
                        ImGui::TableSetupColumn("DataType", ImGuiTableColumnFlags_WidthFixed,
                                                200.0f * dpiScale);
                        ImGui::TableSetupColumn(
                            "Flags",
                            ImGuiTableColumnFlags_WidthFixed |
                                (m_capabilities.rawBlocks ? 0 : ImGuiTableColumnFlags_DefaultHide),
                            110.0f * dpiScale);
                        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed,
                                                175.0f * dpiScale);
                        ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthFixed,
                                                300.0f * dpiScale);
                        ImGui::TableHeadersRow();

                        ImGuiTableSortSpecs* sorts_specs = ImGui::TableGetSortSpecs();
                        // Only sort when the specs changed (header click) or the tree changed
                        // (m_forceSort) — SortRecursive over the whole tree every frame moves
                        // entire child subtrees per comparison swap. The comparison is against
                        // this view's own copy because the specs are shared; see m_appliedSort.
                        if (sorts_specs) {
                            std::vector<AppliedSort> current;
                            current.reserve(
                                static_cast<size_t>(std::max(0, sorts_specs->SpecsCount)));
                            for (int n = 0; n < sorts_specs->SpecsCount; n++)
                                current.push_back({sorts_specs->Specs[n].ColumnIndex,
                                                   sorts_specs->Specs[n].SortDirection});
                            if (m_forceSort || current != m_appliedSort) {
                                SortRecursive(m_root.children, sorts_specs);
                                m_appliedSort = std::move(current);
                            }
                            sorts_specs->SpecsDirty = false;
                        }
                        m_forceSort = false;

                        m_visibleSectors.clear();

                        DrawNode(m_root, contextChanged);

                        if (ImGui::BeginPopupContextWindow("##BackgroundContextMenu",
                                                           ImGuiPopupFlags_MouseButtonRight |
                                                               ImGuiPopupFlags_NoOpenOverItems)) {
                            bool singleSelection = m_selectedSectors.size() == 1;
                            const adm::core::FileNode* selectedNode = nullptr;
                            if (singleSelection) {
                                selectedNode =
                                    FindNode(static_cast<uint32_t>(*m_selectedSectors.begin()));
                            }

                            // Restore
                            if (CanRestore()) {
                                if (ImGui::MenuItem("Restore")) {
                                    TriggerRestore();
                                }
                                ImGui::Separator();
                            }

                            // Open File
                            bool canOpen = singleSelection && selectedNode && !selectedNode->isDir;
                            if (ImGui::MenuItem("Open File", nullptr, false, canOpen)) {
                                if (m_onOpenFile && selectedNode) {
                                    m_onOpenFile(selectedNode->startSector, selectedNode->name);
                                }
                            }

                            // Dynamic Type items
                            if (selectedNode) {
                                if (selectedNode->dataTypeGroup == "Image") {
                                    if (ImGui::MenuItem("View")) {
                                        if (m_onViewImage) {
                                            m_onViewImage(selectedNode->startSector,
                                                          selectedNode->name);
                                        }
                                    }
                                }
                                if (selectedNode->dataTypeGroup == "Audio" ||
                                    selectedNode->dataTypeGroup == "Tracker Music") {
                                    if (ImGui::MenuItem("Listen")) {
                                        if (m_onListenAudio) {
                                            m_onListenAudio(selectedNode->startSector,
                                                            selectedNode->name);
                                        }
                                    }
                                }
                                if (selectedNode->dataTypeGroup == "Executable") {
                                    if (ImGui::MenuItem("Run")) {
                                        if (m_onRunFile) {
                                            m_onRunFile(selectedNode->startSector,
                                                        selectedNode->name);
                                        }
                                    }
                                    if (ImGui::MenuItem("View Hunks")) {
                                        if (m_onViewHunks) {
                                            m_onViewHunks(selectedNode->startSector,
                                                          selectedNode->name);
                                        }
                                    }
                                }
                            }

                            // Save File As
                            bool canSave = singleSelection && selectedNode && !selectedNode->isDir;
                            if (ImGui::MenuItem("Save File As...", nullptr, false, canSave)) {
                                if (m_onSaveFile && selectedNode) {
                                    m_onSaveFile(selectedNode->startSector, selectedNode->name);
                                }
                            }

                            // Export
                            if (ImGui::MenuItem("Export to Folder...", nullptr, false,
                                                CanExport())) {
                                TriggerExport();
                            }

                            ImGui::Separator();

                            // Copy / Paste (paste into root)
                            if (ImGui::MenuItem("Copy", nullptr, false, CanCopy())) {
                                TriggerCopy();
                            }
                            if (ImGui::MenuItem("Paste", nullptr, false, CanPaste())) {
                                TriggerPaste(0);
                            }

                            ImGui::Separator();

                            // Rename
                            if (ImGui::MenuItem("Rename", nullptr, false, CanRename())) {
                                TriggerRename();
                            }

                            // Delete
                            if (ImGui::MenuItem("Delete", nullptr, false, CanDelete())) {
                                TriggerDelete();
                            }

                            ImGui::Separator();

                            // Scrub
                            if (ImGui::MenuItem("Scrub", nullptr, false, CanScrub())) {
                                TriggerScrub();
                            }

                            ImGui::Separator();

                            // Permissions
                            if (ImGui::MenuItem("Set Permissions...", nullptr, false,
                                                CanSetPermissions())) {
                                TriggerPermissions();
                            }

                            ImGui::Separator();

                            // Import
                            if (ImGui::MenuItem("Import Files Here", nullptr, false,
                                                m_capabilities.importFiles)) {
                                if (m_onImportFile) {
                                    m_onImportFile(0); // Import to Root
                                }
                            }

                            // New Folder
                            if (ImGui::MenuItem("New Folder Here", nullptr, false,
                                                m_capabilities.createDirectory)) {
                                TriggerNewFolder();
                            }

                            ImGui::EndPopup();
                        }

                        ImGui::EndTable();

                        // Background drop target: catches drops on empty space → root directory.
                        // Runs after all item-level targets so items take priority.
                        // Only activate for the window the cursor is actually over; without
                        // the isDropTargetWindow guard both documents' full-content rects can
                        // both return true from BeginDragDropTargetCustom simultaneously,
                        // causing the yellow border to flicker between source and target.
                        ImGuiWindow* childWin = ImGui::GetCurrentWindow();
                        if (isDropTargetWindow &&
                            ImGui::BeginDragDropTargetCustom(childWin->InnerRect,
                                                             childWin->GetID("##RootDropBg"))) {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload("DOCUMENT_ENTRIES_MOVE")) {
                                const uint32_t* data = (const uint32_t*)payload->Data;
                                int totalCount = payload->DataSize / sizeof(uint32_t);
                                if (totalCount >= 2) {
                                    uint32_t sourceDocId = data[0];
                                    const uint32_t* sectors = data + 1;
                                    int sectorCount = totalCount - 1;
                                    if (sourceDocId != m_documentId && m_onCrossDocumentDrop) {
                                        bool isCopy = !ImGui::GetIO().KeyShift;
                                        std::vector<uint32_t> sectorVec(sectors,
                                                                        sectors + sectorCount);
                                        m_onCrossDocumentDrop(sourceDocId, sectorVec, 0, isCopy);
                                    } else {
                                        std::vector<std::tuple<uint32_t, std::string, uint32_t>>
                                            moves;
                                        for (int i = 0; i < sectorCount; i++) {
                                            const auto* srcNode = FindNode(sectors[i]);
                                            if (srcNode && srcNode->parentSector != 0) {
                                                moves.emplace_back(srcNode->parentSector,
                                                                   srcNode->name, 0);
                                            }
                                        }
                                        if (!moves.empty()) {
                                            if (m_onMoveFiles) {
                                                m_onMoveFiles(moves);
                                            } else if (m_onMoveFile) {
                                                for (const auto& move : moves) {
                                                    m_onMoveFile(std::get<0>(move),
                                                                 std::get<1>(move),
                                                                 std::get<2>(move));
                                                }
                                            }
                                            ClearSelection();
                                        }
                                    }
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                    }
                }
            }
            ImGui::EndChild();
            if (useDocTabs)
                ImGui::EndTabItem(); // close the "Disk Info" tab item
        } // end: if (drawDiskInfo)
        if (useDocTabs) {
            if (m_trackView.HasData() && ImGui::BeginTabItem("Track Map")) {
                m_trackView.DrawContent();
                ImGui::EndTabItem();
            }
            m_dmsView.DrawTabItems(); // emits the DMS tabs; no-op when not a DMS image
            ImGui::EndTabBar();
        }
    } else {
        m_hasFocus = false;
    }
    ImGui::End();

    if (!open && m_onClose) {
        m_onClose(m_documentId);
    }

    if (contextChanged) {
        m_lastHighlightedHead = m_highlightedHead;
    }
}

void FileListView::DrawDialogs()
{
    // Handle Rename Popup
    if (m_openRenamePopup) {
        ImGui::OpenPopup("Rename File");
        m_openRenamePopup = false;
    }

    if (ImGui::BeginPopupModal("Rename File", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter new name for: %s", m_renameOldName.c_str());

        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();

        bool enterPressed = ImGui::InputText("New Name", m_renameBuffer, sizeof(m_renameBuffer),
                                             ImGuiInputTextFlags_EnterReturnsTrue);

        // Validation
        std::string newName(m_renameBuffer);
        m_renameError.clear();
        bool isValid = true;

        if (newName.empty()) {
            m_renameError = "Name cannot be empty.";
            isValid = false;
        } else if (newName.length() > 30) {
            m_renameError = "Name is too long (max 30 chars).";
            isValid = false;
        } else {
            if (newName.find('/') != std::string::npos || newName.find(':') != std::string::npos) {
                m_renameError = "Name cannot contain '/' or ':'.";
                isValid = false;
            }
        }

        if (enterPressed && isValid) {
            if (m_onRenameFile) {
                m_onRenameFile(m_renameParentSector, m_renameStartSector, m_renameOldName, newName);
            }
            ImGui::CloseCurrentPopup();
            m_renameError.clear();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
            m_renameError.clear();
        }

        if (!m_renameError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_renameError.c_str());
        }

        ImGui::BeginDisabled(!isValid);
        if (ImGui::Button("Rename", ImVec2(120, 0))) {
            if (m_onRenameFile) {
                m_onRenameFile(m_renameParentSector, m_renameStartSector, m_renameOldName, newName);
            }
            ImGui::CloseCurrentPopup();
            m_renameError.clear();
        }
        ImGui::EndDisabled();

        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            m_renameError.clear();
        }
        ImGui::EndPopup();
    }

    // Handle New Folder Popup
    if (m_openNewFolderPopup) {
        ImGui::OpenPopup("New Folder");
        m_openNewFolderPopup = false;
    }

    if (ImGui::BeginPopupModal("New Folder", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter name for new directory:");

        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();

        bool enterPressed = ImGui::InputText("Name", m_newFolderBuffer, sizeof(m_newFolderBuffer),
                                             ImGuiInputTextFlags_EnterReturnsTrue);

        // Validation
        std::string newName(m_newFolderBuffer);
        m_newFolderError.clear();
        bool isValid = true;

        if (newName.empty()) {
            m_newFolderError = "Name cannot be empty.";
            isValid = false;
        } else if (newName.length() > 30) {
            m_newFolderError = "Name is too long (max 30 chars).";
            isValid = false;
        } else {
            if (newName.find('/') != std::string::npos || newName.find(':') != std::string::npos) {
                m_newFolderError = "Name cannot contain '/' or ':'.";
                isValid = false;
            }
        }

        if (enterPressed && isValid) {
            if (m_onCreateDir) {
                m_onCreateDir(m_newFolderParentSector, newName);
            }
            ImGui::CloseCurrentPopup();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }

        if (!m_newFolderError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_newFolderError.c_str());
        }

        ImGui::BeginDisabled(!isValid);
        if (ImGui::Button("Create", ImVec2(120, 0))) {
            if (m_onCreateDir) {
                m_onCreateDir(m_newFolderParentSector, newName);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Handle Delete Popup
    if (m_openDeletePopup) {
        ImGui::OpenPopup("Delete File(s)?");
        m_openDeletePopup = false;
    }

    if (ImGui::BeginPopupModal("Delete File(s)?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", m_deleteConfirmationMessage.c_str());
        if (m_itemsToDelete.size() > 1) {
            ImGui::TextDisabled("(%zu files selected)", m_itemsToDelete.size());
        }
        ImGui::Separator();

        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            if (m_onDeleteFiles) {
                m_onDeleteFiles(m_itemsToDelete);
            }
            ClearSelection();
            ImGui::CloseCurrentPopup();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::Button("Delete", ImVec2(120, 0))) {
            if (m_onDeleteFiles) {
                m_onDeleteFiles(m_itemsToDelete);
            }
            ClearSelection();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Handle Permissions Popup
    if (m_openPermissionsPopup) {
        ImGui::OpenPopup("Set Permissions");
        m_openPermissionsPopup = false;
    }

    if (ImGui::BeginPopupModal("Set Permissions", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Permissions for: %s", m_permissionsFileName.c_str());
        ImGui::Separator();

        // Standard flags (non-inverted)
        ImGui::Text("Attributes:");
        ImGui::Checkbox("Hidden (H)", &m_permHidden);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("File is hidden from directory listings");
        }
        ImGui::Checkbox("Script (S)", &m_permScript);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("File is a script (executable by shell)");
        }
        ImGui::Checkbox("Pure (P)", &m_permPure);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pure/reentrant code (can be made resident)");
        }
        ImGui::Checkbox("Archived (A)", &m_permArchived);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("File has been archived/backed up");
        }

        ImGui::Separator();

        // Protection flags (inverted in storage)
        ImGui::Text("Protection:");
        ImGui::Checkbox("Read (R)", &m_permRead);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("File can be read");
        }
        ImGui::Checkbox("Write (W)", &m_permWrite);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("File can be written/modified");
        }
        ImGui::Checkbox("Execute (E)", &m_permExecute);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("File can be executed");
        }
        ImGui::Checkbox("Delete (D)", &m_permDelete);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("File can be deleted");
        }

        ImGui::Separator();

        // Calculate new access value
        int32_t newAccess = 0;
        // Standard flags (set bit = enabled)
        if (m_permHidden)
            newAccess |= (1 << 7);
        if (m_permScript)
            newAccess |= (1 << 6);
        if (m_permPure)
            newAccess |= (1 << 5);
        if (m_permArchived)
            newAccess |= (1 << 4);
        // Protection flags (inverted: clear bit = enabled)
        if (!m_permRead)
            newAccess |= (1 << 3);
        if (!m_permWrite)
            newAccess |= (1 << 2);
        if (!m_permExecute)
            newAccess |= (1 << 1);
        if (!m_permDelete)
            newAccess |= (1 << 0);

        // Show preview
        ImGui::Text("Flags: %s", FormatFlags(newAccess).c_str());

        ImGui::Separator();

        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            if (m_onSetPermissions && !m_permissionsSectors.empty()) {
                m_onSetPermissions(m_permissionsSectors, newAccess);
            }
            ImGui::CloseCurrentPopup();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::Button("Apply", ImVec2(120, 0))) {
            if (m_onSetPermissions && !m_permissionsSectors.empty()) {
                m_onSetPermissions(m_permissionsSectors, newAccess);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Handle Scrub Popup
    if (m_openScrubPopup) {
        ImGui::OpenPopup("Scrub File(s)?");
        m_openScrubPopup = false;
    }

    ImGui::SetNextWindowSize(ImVec2(600, 0));
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Scrub File(s)?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", m_scrubConfirmationMessage.c_str());

        if (m_itemsToScrub.size() > 1) {
            ImGui::TextDisabled("(%zu files selected)", m_itemsToScrub.size());
        }
        ImGui::Separator();

        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            if (m_onScrubFiles) {
                m_onScrubFiles(m_itemsToScrub);
            }
            ClearSelection();
            ImGui::CloseCurrentPopup();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::Button("Scrub", ImVec2(120, 0))) {
            if (m_onScrubFiles) {
                m_onScrubFiles(m_itemsToScrub);
            }
            ClearSelection();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void FileListView::SortRecursive(std::vector<adm::core::FileNode>& nodes,
                                 ImGuiTableSortSpecs* sorts_specs)
{
    if (nodes.empty())
        return;

    std::sort(
        nodes.begin(), nodes.end(),
        [&](const adm::core::FileNode& a, const adm::core::FileNode& b) {
            // Always keep [Deleted Files] at the bottom
            if (a.name == "[Deleted Files]")
                return false;
            if (b.name == "[Deleted Files]")
                return true;

            for (int n = 0; n < sorts_specs->SpecsCount; n++) {
                const ImGuiTableColumnSortSpecs* sort_spec = &sorts_specs->Specs[n];
                int delta = 0;

                if (sort_spec->ColumnIndex == 0) { // Name
                    std::string nameA = a.name;
                    std::string nameB = b.name;
                    std::transform(nameA.begin(), nameA.end(), nameA.begin(), [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                    });
                    std::transform(nameB.begin(), nameB.end(), nameB.begin(), [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                    });
                    delta = nameA.compare(nameB);
                } else if (sort_spec->ColumnIndex == 1) { // Size
                    delta = (a.size - b.size);
                } else if (sort_spec->ColumnIndex == 2) { // DataType
                    delta = a.dataType.compare(b.dataType);
                } else if (sort_spec->ColumnIndex == 4) { // Date
                    if (a.modified.year != b.modified.year)
                        delta = a.modified.year - b.modified.year;
                    else if (a.modified.month != b.modified.month)
                        delta = a.modified.month - b.modified.month;
                    else if (a.modified.day != b.modified.day)
                        delta = a.modified.day - b.modified.day;
                    else if (a.modified.hour != b.modified.hour)
                        delta = a.modified.hour - b.modified.hour;
                    else if (a.modified.min != b.modified.min)
                        delta = a.modified.min - b.modified.min;
                    else
                        delta = a.modified.sec - b.modified.sec;
                }

                if (delta > 0)
                    return (sort_spec->SortDirection == ImGuiSortDirection_Ascending);
                if (delta < 0)
                    return (sort_spec->SortDirection == ImGuiSortDirection_Descending);
            }
            return a.name < b.name;
        });

    for (auto& node : nodes) {
        SortRecursive(node.children, sorts_specs);
    }
}

void FileListView::DrawNode(const adm::core::FileNode& node, bool contextChanged)
{
    if (node.name == "Root" && node.children.size() > 0) {
        for (const auto& child : node.children) {
            DrawRecursive(child, contextChanged);
        }
    } else if (node.children.size() > 0) {
        DrawRecursive(node, contextChanged);
    }
}

bool FileListView::ContainsBlock(const adm::core::FileNode& node, uint32_t head)
{
    if (head == 0)
        return false;
    for (const auto& child : node.children) {
        if ((uint32_t)child.startSector == head)
            return true;
        if (child.isDir && ContainsBlock(child, head))
            return true;
    }
    return false;
}

void FileListView::DrawDragDropSource(const adm::core::FileNode& node)
{
    if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
        return;

    if (!IsSelected(node.startSector)) {
        ClearSelection();
        Select(node.startSector);
    }

    // Build payload with document ID + sector list
    std::vector<uint32_t> payloadData;
    payloadData.push_back(m_documentId);
    for (int32_t s : m_selectedSectors) {
        payloadData.push_back(static_cast<uint32_t>(s));
    }

    ImGui::SetDragDropPayload("DOCUMENT_ENTRIES_MOVE", payloadData.data(),
                              payloadData.size() * sizeof(uint32_t));

    size_t sectorCount = payloadData.size() - 1;
    const char* verb = DragActionVerb();
    if (sectorCount == 1) {
        ImGui::Text("%s: %s", verb, node.name.c_str());
    } else {
        ImGui::Text("%s %zu items", verb, sectorCount);
    }
    ImGui::EndDragDropSource();
}

void FileListView::DrawRecursive(const adm::core::FileNode& node, bool contextChanged)
{
    m_visibleSectors.push_back(node.startSector);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    bool isSelected = IsSelected(node.startSector);

    // Checkbox column behavior simulation (since we don't have a dedicated column in TableSetup)
    // We can put a checkbox before the tree node.

    // Indent based on depth (Tree does this automatically, but Checkbox needs manual)
    // Actually, putting Checkbox inside Tree node label is tricky.
    // Better: Put Checkbox, then SameLine, then TreeNode.
    // BUT TreeNode spans full width usually.
    // Let's try: render Checkbox, same line, render Tree/Leaf.

    ImGui::PushID(node.startSector);
    bool check = isSelected;
    bool checkboxClicked = ImGui::Checkbox("##select", &check);
    bool checkboxHovered = ImGui::IsItemHovered();

    // Lambda to handle selection when clicking on data columns
    auto CheckColumnSelection = [&]() {
        if (ImGui::IsItemClicked()) {
            HandleItemSelection(node.startSector, false, false);
        }
    };
    ImGui::PopID();
    ImGui::SameLine();
    bool isHighlighted =
        (m_highlightedHead != 0 && (uint32_t)node.startSector == m_highlightedHead);

    if (node.isDir && m_highlightedHead != 0 && ContainsBlock(node, m_highlightedHead)) {
        if (contextChanged) {
            ImGui::SetNextItemOpen(true);
        }
    }

    if (isHighlighted) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(50, 220, 50, 255));
        if (contextChanged) {
            ImGui::SetScrollHereY();
        }
    } else if (node.isDeleted) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 80, 80, 255));
    }

    bool isFolder = node.isDir;
    int flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_SpanAllColumns;
    if (isSelected)
        flags |= ImGuiTreeNodeFlags_Selected;

    if (isFolder) {
        bool open = ImGui::TreeNodeEx(node.name.c_str(), flags);

        if (node.name != "[Deleted Files]" && !node.isDeleted) {
            DrawDragDropSource(node);
        }

        // Drop target for folders (can receive files/folders).
        // Guard on m_isDropTargetWindow: the ScrollX table clip rect can extend
        // beyond the visible window, causing HoveredRect to be set for items in the
        // source document when the cursor is at the same Y in the target document.
        if (node.name != "[Deleted Files]" && !node.isDeleted) {
            if (m_isDropTargetWindow && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("DOCUMENT_ENTRIES_MOVE")) {
                    const uint32_t* data = (const uint32_t*)payload->Data;
                    int totalCount = payload->DataSize / sizeof(uint32_t);
                    if (totalCount >= 2) {
                        uint32_t sourceDocId = data[0];
                        const uint32_t* sectors = data + 1;
                        int sectorCount = totalCount - 1;

                        if (sourceDocId != m_documentId && m_onCrossDocumentDrop) {
                            // Cross-document: copy by default, move when Shift is held
                            bool isCopy = !ImGui::GetIO().KeyShift;
                            std::vector<uint32_t> sectorVec(sectors, sectors + sectorCount);
                            m_onCrossDocumentDrop(sourceDocId, sectorVec, node.startSector, isCopy);
                        } else {
                            // Intra-disk move
                            std::vector<std::tuple<uint32_t, std::string, uint32_t>> moves;
                            for (int i = 0; i < sectorCount; i++) {
                                const auto* srcNode = FindNode(sectors[i]);
                                if (srcNode) {
                                    int32_t dest = (int32_t)node.startSector;
                                    if (srcNode->parentSector != dest) {
                                        if (!ContainsBlock(*srcNode, dest) &&
                                            (int32_t)srcNode->startSector != dest) {
                                            moves.emplace_back(srcNode->parentSector, srcNode->name,
                                                               dest);
                                        }
                                    }
                                }
                            }
                            if (!moves.empty()) {
                                if (m_onMoveFiles) {
                                    m_onMoveFiles(moves);
                                } else if (m_onMoveFile) {
                                    for (const auto& move : moves) {
                                        m_onMoveFile(std::get<0>(move), std::get<1>(move),
                                                     std::get<2>(move));
                                    }
                                }
                                ClearSelection();
                            }
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        // Handle checkbox click (processed after TreeNode)
        if (checkboxClicked) {
            HandleItemSelection(node.startSector, true, false);
        }

        // Selection Logic for folder row/name click
        if (ImGui::IsItemClicked() && !checkboxHovered) {
            bool treeNodeToggled = ImGui::IsItemToggledOpen();
            HandleItemSelection(node.startSector, false, treeNodeToggled);
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (m_onFileDoubleClicked) {
                m_onFileDoubleClicked((uint32_t)node.startSector);
            }
        }

        if (ImGui::BeginPopupContextItem()) {
            // Right-clicking an unselected item targets just that item; right-clicking
            // within an existing (multi-)selection keeps the whole selection.
            EnsureSelectedForContext(node.startSector);
            if (ImGui::MenuItem("Import Files Here", nullptr, false, m_capabilities.importFiles)) {
                if (m_onImportFile) {
                    m_onImportFile((uint32_t)node.startSector);
                }
            }
            if (ImGui::MenuItem("New Folder Here", nullptr, false,
                                m_capabilities.createDirectory)) {
                m_openNewFolderPopup = true;
                m_newFolderParentSector = (uint32_t)node.startSector;
                memset(m_newFolderBuffer, 0, sizeof(m_newFolderBuffer));
                m_newFolderError.clear();
            }
            if (ImGui::MenuItem("Export to Folder...")) {
                TriggerExport();
            }
            ImGui::Separator();
            // Copy the selection; paste lands inside this folder.
            if (ImGui::MenuItem("Copy", nullptr, false, CanCopy())) {
                TriggerCopy();
            }
            if (ImGui::MenuItem("Paste", nullptr, false, CanPaste() && node.startSector >= 0)) {
                TriggerPaste((uint32_t)node.startSector);
            }
            ImGui::Separator();
            ImGui::BeginDisabled(!CanRename());
            if (ImGui::MenuItem("Rename")) {
                m_openRenamePopup = true;
                m_renameOldName = node.name;
                m_renameParentSector = node.parentSector;
                m_renameStartSector = node.startSector;
                memset(m_renameBuffer, 0, sizeof(m_renameBuffer));
                strncpy(m_renameBuffer, node.name.c_str(), sizeof(m_renameBuffer) - 1);
                m_renameError.clear();
            }
            ImGui::EndDisabled();
            if (ImGui::MenuItem("Delete", nullptr, false, CanDelete())) {
                TriggerDelete();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Scrub", nullptr, false, CanScrub())) {
                TriggerScrub();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Set Permissions...", nullptr, false, CanSetPermissions())) {
                TriggerPermissions();
            }
            ImGui::EndPopup();
        }

        ImGui::TableNextColumn();
        if (node.name == "[Deleted Files]") {
            ImGui::Spacing();
            ImGui::TableNextColumn();
            ImGui::Spacing();
            ImGui::TableNextColumn();
            ImGui::Spacing();
            ImGui::TableNextColumn();
            ImGui::Spacing();
            ImGui::TableNextColumn();
            ImGui::Spacing();
        } else {
            ImGui::Text("-");
            CheckColumnSelection();

            ImGui::TableNextColumn();
            ImGui::Spacing(); // DataType (empty for directories)
            CheckColumnSelection();

            ImGui::TableNextColumn();
            ImGui::Text("%s", FormatFlags(node.access).c_str());
            CheckColumnSelection();

            ImGui::TableNextColumn();
            ImGui::Text("%04d-%02d-%02d %02d:%02d:%02d", node.modified.year, node.modified.month,
                        node.modified.day, node.modified.hour, node.modified.min,
                        node.modified.sec);
            CheckColumnSelection();

            ImGui::TableNextColumn();
            ImGui::Text("%s", node.comment.c_str());
            CheckColumnSelection();
        }

        if (open) {
            for (const auto& child : node.children) {
                DrawRecursive(child, contextChanged);
            }
            ImGui::TreePop();
        }
    } else {
        ImGui::TreeNodeEx(node.name.c_str(),
                          ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | flags);

        // Drag source for files
        if (!node.isDeleted) {
            DrawDragDropSource(node);
        }

        // Drop target for files (moves to parent directory)
        if (!node.isDeleted) {
            if (m_isDropTargetWindow && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("DOCUMENT_ENTRIES_MOVE")) {
                    const uint32_t* data = (const uint32_t*)payload->Data;
                    int totalCount = payload->DataSize / sizeof(uint32_t);
                    if (totalCount >= 2) {
                        uint32_t sourceDocId = data[0];
                        const uint32_t* sectors = data + 1;
                        int sectorCount = totalCount - 1;

                        if (sourceDocId != m_documentId && m_onCrossDocumentDrop) {
                            // Cross-document: copy by default, move when Shift is held
                            bool isCopy = !ImGui::GetIO().KeyShift;
                            std::vector<uint32_t> sectorVec(sectors, sectors + sectorCount);
                            m_onCrossDocumentDrop(sourceDocId, sectorVec, node.parentSector,
                                                  isCopy);
                        } else {
                            // Intra-disk move
                            std::vector<std::tuple<uint32_t, std::string, uint32_t>> moves;
                            for (int i = 0; i < sectorCount; i++) {
                                const auto* srcNode = FindNode(sectors[i]);
                                if (srcNode) {
                                    int32_t dest = node.parentSector;
                                    if (srcNode->parentSector != dest) {
                                        if (!ContainsBlock(*srcNode, dest) &&
                                            (int32_t)srcNode->startSector != dest) {
                                            moves.emplace_back(srcNode->parentSector, srcNode->name,
                                                               dest);
                                        }
                                    }
                                }
                            }
                            if (!moves.empty()) {
                                if (m_onMoveFiles) {
                                    m_onMoveFiles(moves);
                                } else if (m_onMoveFile) {
                                    for (const auto& move : moves) {
                                        m_onMoveFile(std::get<0>(move), std::get<1>(move),
                                                     std::get<2>(move));
                                    }
                                }
                                ClearSelection();
                            }
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        // Handle checkbox click (processed after TreeNode)
        if (checkboxClicked) {
            HandleItemSelection(node.startSector, true, false);
        }

        // Selection Logic for file row/name click
        if (ImGui::IsItemClicked() && !checkboxHovered) {
            HandleItemSelection(node.startSector, false, false);
        }
    }

    if (!isFolder) {
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (m_onFileDoubleClicked) {
                m_onFileDoubleClicked((uint32_t)node.startSector);
            }
        }

        if (ImGui::BeginPopupContextItem()) {
            // Right-clicking an unselected item targets just that item; right-clicking
            // within an existing (multi-)selection keeps the whole selection.
            EnsureSelectedForContext(node.startSector);
            if (node.isDeleted) {
                // Deleted file - show restore option
                if (ImGui::MenuItem("Restore")) {
                    TriggerRestore();
                }
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Open File")) {
                if (m_onOpenFile) {
                    m_onOpenFile((uint32_t)node.startSector, node.name);
                }
            }
            // Show "View" option for image files
            if (node.dataTypeGroup == "Image") {
                if (ImGui::MenuItem("View")) {
                    if (m_onViewImage) {
                        m_onViewImage((uint32_t)node.startSector, node.name);
                    }
                }
            }
            // Show "Listen" option for audio/tracker files
            if (node.dataTypeGroup == "Audio" || node.dataTypeGroup == "Tracker Music") {
                if (ImGui::MenuItem("Listen")) {
                    if (m_onListenAudio) {
                        m_onListenAudio((uint32_t)node.startSector, node.name);
                    }
                }
            }
            // Show "View Hunks" option for Amiga executables
            if (node.dataTypeGroup == "Executable") {
                if (ImGui::MenuItem("Run")) {
                    if (m_onRunFile) {
                        m_onRunFile((uint32_t)node.startSector, node.name);
                    }
                }
                if (ImGui::MenuItem("View Hunks")) {
                    if (m_onViewHunks) {
                        m_onViewHunks((uint32_t)node.startSector, node.name);
                    }
                }
            }
            if (ImGui::MenuItem("Save File As...")) {
                if (m_onSaveFile) {
                    m_onSaveFile((uint32_t)node.startSector, node.name);
                }
            }
            if (ImGui::MenuItem("Export to Folder...")) {
                TriggerExport();
            }
            ImGui::Separator();
            // Copy the selection; paste lands in this file's parent directory.
            if (ImGui::MenuItem("Copy", nullptr, false, CanCopy())) {
                TriggerCopy();
            }
            if (ImGui::MenuItem("Paste", nullptr, false, CanPaste())) {
                TriggerPaste(node.parentSector);
            }
            ImGui::Separator();
            ImGui::BeginDisabled(node.isDeleted || !CanRename());
            if (ImGui::MenuItem("Rename")) {
                m_openRenamePopup = true;
                m_renameOldName = node.name;
                m_renameParentSector = node.parentSector;
                m_renameStartSector = node.startSector;
                memset(m_renameBuffer, 0, sizeof(m_renameBuffer));
                strncpy(m_renameBuffer, node.name.c_str(), sizeof(m_renameBuffer) - 1);
                m_renameError.clear();
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(node.isDeleted || !m_capabilities.deleteEntries);
            if (ImGui::MenuItem("Delete", nullptr, false, CanDelete())) {
                TriggerDelete();
            }
            ImGui::EndDisabled();

            ImGui::Separator();
            if (ImGui::MenuItem("Scrub", nullptr, false, CanScrub())) {
                TriggerScrub();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Set Permissions...", nullptr, false, CanSetPermissions())) {
                TriggerPermissions();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Import Files Here", nullptr, false, m_capabilities.importFiles)) {
                if (m_onImportFile) {
                    m_onImportFile(node.parentSector);
                }
            }
            if (ImGui::MenuItem("New Folder Here", nullptr, false,
                                m_capabilities.createDirectory)) {
                m_openNewFolderPopup = true;
                m_newFolderParentSector = node.parentSector;
                memset(m_newFolderBuffer, 0, sizeof(m_newFolderBuffer));
                m_newFolderError.clear();
            }
            ImGui::EndPopup();
        }
    }

    // Only render file column data for non-directory nodes
    if (!isFolder) {
        ImGui::TableNextColumn();
        ImGui::Text("%d B", node.size);
        CheckColumnSelection();

        ImGui::TableNextColumn();
        if (!node.dataType.empty()) {
            ImGui::Text("%s", node.dataType.c_str());
        } else {
            ImGui::TextDisabled("-");
        }
        CheckColumnSelection();

        ImGui::TableNextColumn();
        ImGui::Text("%s", FormatFlags(node.access).c_str());
        CheckColumnSelection();

        ImGui::TableNextColumn();
        ImGui::Text("%s", node.comment.c_str());
        CheckColumnSelection();

        ImGui::TableNextColumn();
        ImGui::Text("%04d-%02d-%02d %02d:%02d:%02d", node.modified.year, node.modified.month,
                    node.modified.day, node.modified.hour, node.modified.min, node.modified.sec);
        CheckColumnSelection();
    }

    if (isHighlighted || node.isDeleted) {
        ImGui::PopStyleColor();
    }
}

// ... but WAIT, we need to handle the popup drawing outside of DrawRecursive loop to avoid
// potential issues? No, DrawRecursive is called within a Table. Popping up a modal from within
// a Table might be weird if not done carefully. Standard ImGui practice: set a flag, then open
// specific popup outside the loop/table. So let's handle the actual OpenPopup in the main Draw
// method.

std::string FileListView::FormatFlags(int32_t access)
{
    std::string flags = "--------";
    if (access & (1 << 7))
        flags[0] = 'H';
    if (access & (1 << 6))
        flags[1] = 'S';
    if (access & (1 << 5))
        flags[2] = 'P';
    if (access & (1 << 4))
        flags[3] = 'A';
    if (!(access & (1 << 3)))
        flags[4] = 'R';
    if (!(access & (1 << 2)))
        flags[5] = 'W';
    if (!(access & (1 << 1)))
        flags[6] = 'E';
    if (!(access & (1 << 0)))
        flags[7] = 'D';

    return flags;
}

const adm::core::FileNode* FileListView::FindNode(uint32_t sector) const
{
    // Special check for root (though usually root has sektors, sector 0 is often root parent, or
    // root dir starts at specific block)
    if (m_root.startSector == (int32_t)sector)
        return &m_root;
    return FindNodeRecursive(m_root, sector);
}

const adm::core::FileNode* FileListView::FindNodeRecursive(const adm::core::FileNode& node,
                                                           uint32_t sector) const
{
    if ((uint32_t)node.startSector == sector) {
        return &node;
    }
    for (const auto& child : node.children) {
        if ((uint32_t)child.startSector == sector) {
            return &child;
        }
        const auto* found = FindNodeRecursive(child, sector);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

} // namespace adm::ui
