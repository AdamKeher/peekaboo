#pragma once

#include <fmt/core.h>
#include <imgui.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <set>
#include "../../core/AdfWrapper.h"
#include "../../core/DiskDocument.h"
#include "DmsView.h"
#include "TrackView.h"

namespace adm::ui {

/**
 * @brief Drag-drop payload for moving/copying files between ADF documents.
 */
struct AdfDragPayload
{
    uint32_t sourceDocId;
    uint32_t sectorCount;
    // Followed by sectorCount uint32_t sector values
};

/**
 * @brief Per-frame snapshot of a document's background download, rendered as a
 * placeholder in place of the normal disk view while loading.
 */
struct DownloadStatus
{
    bool isError = false;
    bool showLocate = false;
    float progress = -1.0f; // [0,1]; negative => indeterminate
    size_t bytesReceived = 0;
    int64_t totalBytes = -1; // -1 => unknown
    double bytesPerSec = 0.0;
    std::string statusMsg;
    std::string errorMsg;
    std::string errorTitle;
    std::string sourceName; // e.g. "TOSEC", "demozoo.org"
    std::string sourceUrl;  // optional clickable link
};

/**
 * @brief File browser view displaying the disk's directory structure.
 *
 * Shows a tree view of files and directories with sorting, highlighting,
 * and context menu support.
 */
class FileListView
{
public:
    struct WindowPlacement
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        uint32_t dockId = 0;
        bool valid = false;
    };

    void SetDocumentId(uint32_t id) { m_documentId = id; }
    uint32_t GetDocumentId() const { return m_documentId; }
    bool HasFocus() const { return m_hasFocus; }
    void RequestFocus() { m_requestFocus = true; }
    void SetTargetDockId(uint32_t dockId) { m_targetDockId = dockId; }
    void ForceDockId(uint32_t dockId)
    {
        m_targetDockId = dockId;
        m_forceDock = true;
    }
    void ForceFloat(float x, float y, float w, float h)
    {
        m_targetPosX = x;
        m_targetPosY = y;
        m_targetSizeX = w;
        m_targetSizeY = h;
        m_forceFloat = true;
    }
    void SetTargetPos(float x, float y)
    {
        m_targetPosX = x;
        m_targetPosY = y;
        m_hasTargetPos = true;
    }
    void SetTargetSize(float w, float h)
    {
        m_targetSizeX = w;
        m_targetSizeY = h;
        m_hasTargetSize = true;
    }
    WindowPlacement GetWindowPlacement() const { return m_windowPlacement; }
    void SetWindowPlacement(const WindowPlacement& placement)
    {
        if (!placement.valid)
            return;
        if (placement.dockId != 0)
            ForceDockId(placement.dockId);
        else
            ForceFloat(placement.x, placement.y, placement.width, placement.height);
    }

    void SetRoot(const adm::core::FileNode& root);
    const adm::core::FileNode& GetRoot() const { return m_root; }
    void SetCapabilities(adm::core::DocumentCapabilities capabilities)
    {
        m_capabilities = capabilities;
    }

    // Supplies IPF per-track preservation data. When non-empty, the document
    // window shows a "Track Map" tab alongside "Disk Info".
    void SetIpfTracks(const std::vector<adm::core::IpfTrackInfo>& tracks)
    {
        m_trackView.SetData(tracks);
    }

    // Supplies the DMS archive path. When valid, the document window shows the
    // DMS metadata tabs (General / Track Information / Banner / FILEID.DIZ).
    void SetDmsData(const std::string& path) { m_dmsView.SetData(path); }
    void SetOnFileDoubleClicked(std::function<void(uint32_t)> callback);
    void SetOnOpenFile(std::function<void(uint32_t, const std::string&)> callback);
    void SetOnViewImage(std::function<void(uint32_t, const std::string&)> callback);
    void SetOnListenAudio(std::function<void(uint32_t, const std::string&)> callback);
    void SetOnViewHunks(std::function<void(uint32_t, const std::string&)> callback);
    void SetOnImportFile(std::function<void(uint32_t)> callback);

    // Write protection. The flag itself lives on the document's AdfHandler;
    // Application pushes the current value in every frame, so this is a display
    // mirror and the callback is the only way to change it.
    void SetWriteProtected(bool writeProtected) { m_writeProtected = writeProtected; }
    void SetOnToggleWriteProtect(std::function<void(uint32_t, bool)> callback);
    // Why the padlock cannot be changed right now (a running emulator has the image read-write);
    // empty when it can.
    void SetWriteProtectLockReason(std::string reason)
    {
        m_writeProtectLockReason = std::move(reason);
    }

    // Hard-disk images: the partitions the document can show, and which one it shows (-1 = the
    // whole image as raw blocks). An empty list hides the selector - floppies have no partitions.
    struct PartitionChoice
    {
        std::string label;
        bool mountable = true;
    };
    void SetPartitions(std::vector<PartitionChoice> partitions, int selected)
    {
        m_partitions = std::move(partitions);
        m_selectedPartition = selected;
    }
    void SetOnSelectPartition(std::function<void(uint32_t, int)> callback)
    {
        m_onSelectPartition = std::move(callback);
    }
    void SetOnRunFile(std::function<void(uint32_t, const std::string&)> callback);
    void SetOnCreateDir(std::function<void(uint32_t, const std::string&)> callback);
    // void SetOnDeleteFile(std::function<void(uint32_t, const std::string&)> callback); //
    // Deprecated
    void SetOnDeleteFiles(
        std::function<void(const std::vector<std::pair<uint32_t, std::string>>&)> callback);
    void SetOnSetPermissions(std::function<void(const std::vector<uint32_t>&, int32_t)> callback);
    void SetOnRestoreFile(std::function<bool(uint32_t)> callback);

    void SetHighlightedFileHeader(uint32_t sector);

    void Draw(const adm::core::DiskInfo& diskInfo, bool hasUnsavedChanges, float dpiScale = 1.0f);
    void DrawDialogs();

    // Edit Menu Action Triggers
    void TriggerNewFolder();
    void TriggerDelete();
    void TriggerScrub();
    void TriggerRename();
    void TriggerPermissions();
    void TriggerRestore();
    void TriggerExport();
    void TriggerCopy();
    void TriggerPaste(uint32_t destParentSector);

    // Edit Menu State Checks
    bool CanCreateNewFolder() const;
    bool CanDelete() const;
    bool CanScrub() const;
    bool CanRename() const;
    bool CanSetPermissions() const;
    bool CanRestore() const;
    bool CanExport() const;
    bool CanCopy() const;
    bool CanPaste() const;

    // Cross-document copy/paste clipboard (shared across all file browser windows).
    void
    SetOnPasteFiles(std::function<void(uint32_t sourceDocId, const std::vector<uint32_t>& sectors,
                                       uint32_t destParentSector)>
                        callback);

    void SetOnSaveFile(std::function<void(uint32_t, const std::string&)> callback);
    void SetOnRenameFile(
        std::function<void(uint32_t, uint32_t, const std::string&, const std::string&)> callback);

private:
    void SortRecursive(std::vector<adm::core::FileNode>& nodes, ImGuiTableSortSpecs* sorts_specs);
    void DrawNode(const adm::core::FileNode& node, bool contextChanged);
    bool ContainsBlock(const adm::core::FileNode& node, uint32_t head);
    void DrawRecursive(const adm::core::FileNode& node, bool contextChanged);
    // Emit the ImGui drag-drop source (payload + tooltip) for a node. Call inside the
    // item's scope; the caller decides whether the node is draggable.
    void DrawDragDropSource(const adm::core::FileNode& node);
    std::string FormatFlags(int32_t access);

    const adm::core::FileNode* FindNode(uint32_t sector) const;
    const adm::core::FileNode* FindNodeRecursive(const adm::core::FileNode& node,
                                                 uint32_t sector) const;

    // Selection Helpers
    bool IsSelected(int32_t sector) const;
    void ToggleSelection(int32_t sector);
    void Select(int32_t sector);
    void Deselect(int32_t sector);
    void ClearSelection();
    void CollectSelectedNodes(const adm::core::FileNode& node,
                              std::vector<std::pair<uint32_t, std::string>>& outItems);

    /**
     * @brief Unified selection handler for all selection interactions.
     * @param sector The sector of the clicked item
     * @param isCheckbox True if checkbox was clicked (additive toggle, no anchor update)
     * @param isTreeNodeToggledOpen True if expand/collapse arrow was clicked (no selection change)
     */
    void HandleItemSelection(int32_t sector, bool isCheckbox, bool isTreeNodeToggledOpen);

    /**
     * @brief Ensure a context-menu target participates in the current selection.
     *
     * If the right-clicked item is not part of the selection, the selection is
     * replaced with just that item. If it is already selected (possibly among
     * several items), the selection is left untouched so the chosen action
     * applies to every selected item.
     */
    void EnsureSelectedForContext(int32_t sector);

    /**
     * @brief The verb ("Move"/"Copy") describing what the active drag will do.
     *
     * Reflects the drop target currently under the cursor: an intra-disk drop
     * always moves, while a cross-document drop copies by default (move with Shift).
     * Shared across all FileListView instances via the static hover state below.
     */
    const char* DragActionVerb() const;

    // Document id of the drop target currently hovered during a drag (-1 = none),
    // plus the ImGui frame it was recorded on so stale values self-expire.
    static int32_t s_dragHoverDocId;
    static int s_dragHoverFrame;

    // Copy/paste clipboard shared across every file browser window. Holds the
    // source document and the top-level item sectors that were copied.
    struct Clipboard
    {
        uint32_t docId = 0;
        std::vector<uint32_t> sectors;
    };
    static Clipboard s_clipboard;

    // Collect the "highest" selected items (a selected folder stands in for its
    // whole subtree, so its children are not listed separately).
    void CollectTopLevelSelectedSectors(const adm::core::FileNode& node,
                                        std::vector<uint32_t>& out) const;

    std::function<void(uint32_t sourceDocId, const std::vector<uint32_t>& sectors,
                       uint32_t destParentSector)>
        m_onPasteFiles;

    uint32_t m_documentId = 0;
    uint32_t m_targetDockId = 0;
    bool m_forceDock = false;
    bool m_forceFloat = false;
    bool m_isDropTargetWindow = false; // true when cursor is over this window during a drag
    bool m_hasTargetPos = false;
    bool m_hasTargetSize = false;
    float m_targetPosX = 0.0f;
    float m_targetPosY = 0.0f;
    float m_targetSizeX = 0.0f;
    float m_targetSizeY = 0.0f;
    bool m_hasFocus = false;
    bool m_requestFocus = false;
    WindowPlacement m_windowPlacement;

    adm::core::FileNode m_root;
    adm::core::DocumentCapabilities m_capabilities{true, true, true, true, true,
                                                   true, true, true, true, true};
    std::function<void(uint32_t)> m_onFileDoubleClicked;
    std::function<void(uint32_t, const std::string&)> m_onOpenFile;
    std::function<void(uint32_t, const std::string&)> m_onViewImage;
    std::function<void(uint32_t, const std::string&)> m_onListenAudio;
    std::function<void(uint32_t, const std::string&)> m_onViewHunks;
    std::function<void(uint32_t, const std::string&)> m_onSaveFile;
    std::function<void(uint32_t, uint32_t, const std::string&, const std::string&)> m_onRenameFile;
    bool m_writeProtected = true;
    std::function<void(uint32_t, bool)> m_onToggleWriteProtect;
    std::string m_writeProtectLockReason;
    std::vector<PartitionChoice> m_partitions;
    int m_selectedPartition = 0;
    std::function<void(uint32_t, int)> m_onSelectPartition;
    void DrawPartitionSelector();
    std::function<void(uint32_t)> m_onImportFile;
    std::function<void(uint32_t, const std::string&)> m_onRunFile;
    std::function<void(uint32_t, const std::string&)> m_onCreateDir;
    // std::function<void(uint32_t, const std::string&)> m_onDeleteFile;
    std::function<void(const std::vector<std::pair<uint32_t, std::string>>&)> m_onDeleteFiles;
    std::function<void(const std::vector<uint32_t>&, int32_t)> m_onSetPermissions;
    std::function<bool(uint32_t)> m_onRestoreFile;

    std::set<int32_t> m_selectedSectors;
    int32_t m_lastSelectedSector = -1; // Anchor for Shift-click selection
    std::vector<int32_t> m_visibleSectors;
    uint32_t m_highlightedHead = 0;
    uint32_t m_lastHighlightedHead = 0;
    bool m_forceSort = true;

    // Every File Browser shares one table ID so that they share one column layout (see the
    // BeginTableEx call in Draw). That also makes them share one ImGuiTableSortSpecs, whose
    // SpecsDirty flag is consumed by whichever document draws first - the others would never
    // see the header click. Each view therefore remembers the spec it last applied and sorts
    // when its own copy is stale, rather than trusting the shared flag.
    struct AppliedSort
    {
        int column = -1;
        int direction = 0;
        bool operator==(const AppliedSort&) const = default;
    };
    std::vector<AppliedSort> m_appliedSort;

    // Range Selection Helpers
    void SelectRange(int32_t startSector, int32_t endSector, bool clearFirst = true);
    void CollectVisibleNodes(const adm::core::FileNode& node, std::vector<int32_t>& outNodes);

    // Rename state
    bool m_openRenamePopup = false;
    std::string m_renameOldName;
    uint32_t m_renameParentSector = 0;
    uint32_t m_renameStartSector = 0;
    char m_renameBuffer[108]; // Max filename length in AmigaDOS is usually 30, but FFS can hold
                              // more, standard buffer size safe
    std::string m_renameError;

    // New Folder state
    bool m_openNewFolderPopup = false;
    uint32_t m_newFolderParentSector = 0;
    char m_newFolderBuffer[32];
    std::string m_newFolderError;

    // Delete Popup State
    bool m_openDeletePopup = false;
    // uint32_t m_deleteParentSector = 0;
    // std::string m_deleteName;
    std::vector<std::pair<uint32_t, std::string>> m_itemsToDelete;
    std::string m_deleteConfirmationMessage;

    // Permissions Popup State
    bool m_openPermissionsPopup = false;
    std::vector<uint32_t> m_permissionsSectors;
    std::string m_permissionsFileName; // Display name (single file or "X items")
    // Individual permission flags for UI
    bool m_permHidden = false;
    bool m_permScript = false;
    bool m_permPure = false;
    bool m_permArchived = false;
    bool m_permRead = false;
    bool m_permWrite = false;
    bool m_permExecute = false;
    bool m_permDelete = false;

    // Scrub Popup State
    bool m_openScrubPopup = false;
    std::string m_scrubConfirmationMessage;
    std::vector<std::pair<uint32_t, std::string>> m_itemsToScrub;

    std::function<void(const std::vector<std::pair<uint32_t, std::string>>&)> m_onScrubFiles;
    std::function<bool(uint32_t, const std::string&, uint32_t)> m_onMoveFile;
    std::function<void(const std::vector<std::tuple<uint32_t, std::string, uint32_t>>&)>
        m_onMoveFiles;
    std::function<void(const std::vector<adm::core::FileNode>&)> m_onExportFiles;
    std::function<void(uint32_t, const std::vector<uint32_t>&, uint32_t, bool)>
        m_onCrossDocumentDrop;

public:
    void SetOnScrubFiles(
        std::function<void(const std::vector<std::pair<uint32_t, std::string>>&)> callback);
    void SetOnMoveFile(
        std::function<bool(uint32_t srcParent, const std::string& name, uint32_t destParent)>
            callback);
    void SetOnMoveFiles(
        std::function<void(const std::vector<std::tuple<uint32_t, std::string, uint32_t>>&)>
            callback);
    void SetOnExportFiles(std::function<void(const std::vector<adm::core::FileNode>&)> callback);
    void SetOnCrossDocumentDrop(
        std::function<void(uint32_t sourceDocId, const std::vector<uint32_t>& entries,
                           uint32_t destParent, bool isCopy)>
            callback);
    void SetFilename(const std::string& filename);
    void SetOnClose(std::function<void(uint32_t)> callback);
    void SetDemosceneSource(std::optional<core::DemosceneSource> source);

    // Download placeholder: when set, Draw renders progress/error UI instead of the
    // disk view. Pass std::nullopt once the document is mounted (normal view).
    void SetDownloadStatus(std::optional<DownloadStatus> status)
    {
        m_downloadStatus = std::move(status);
    }
    void SetOnRetryDownload(std::function<void(uint32_t)> callback)
    {
        m_onRetryDownload = std::move(callback);
    }
    void SetOnLocateDirectory(std::function<void(uint32_t)> callback)
    {
        m_onLocateDirectory = std::move(callback);
    }

private:
    void DrawDownloadPlaceholder();
    void DrawWriteProtectButton();

    std::string m_filename;
    std::optional<core::DemosceneSource> m_demosceneSource;
    std::function<void(uint32_t)> m_onClose;
    std::optional<DownloadStatus> m_downloadStatus;
    std::function<void(uint32_t)> m_onRetryDownload;
    std::function<void(uint32_t)> m_onLocateDirectory;

    // Embedded IPF track map (rendered as the "Track Map" document tab).
    TrackView m_trackView;
    // Embedded DMS metadata (rendered as extra document tabs for .dms images).
    DmsView m_dmsView;
};

} // namespace adm::ui
