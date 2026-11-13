#pragma once

#include "../../core/BlockAnalyzer.h"
#include "../../core/BootBlockLibrary.h"
#include "../../core/BrainFile.h"
#include "BlockEditorBase.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

struct GLFWwindow;

namespace adm::ui {

/**
 * @brief Boot Block Viewer window.
 *
 * Displays bootblock data with 32-byte-wide hex view, brainfile identification
 * information, and structure details including checksum validation.
 */
class BootBlockView : public BlockEditorBase
{
public:
    BootBlockView();

    /**
     * @brief Set the bootblock data (both sectors).
     * @param sector0 First 512 bytes (block 0)
     * @param sector1 Second 512 bytes (block 1)
     */
    void SetData(const std::vector<uint8_t>& sector0, const std::vector<uint8_t>& sector1);

    /**
     * @brief Set the brainfile identification info.
     */
    void SetBootBlockInfo(const core::BootBlockInfo& info);

    /**
     * @brief Set callback for block navigation.
     */
    void SetOnNavigate(std::function<void(uint32_t)> callback);

    /**
     * @brief Provide the boot block library to browse (not owned).
     */
    void SetLibrary(core::BootBlockLibrary* library)
    {
        m_library = library;
        m_metaDirty = true;
    }

    /**
     * @brief Supply brainfile metadata for a library entry (by name), used by the
     *        Type / Kickstart / Machine filters. Returns an info with identified=false
     *        when the entry is unknown.
     */
    void SetMetadataProvider(std::function<core::BootBlockInfo(const std::string&)> cb)
    {
        m_metadataProvider = std::move(cb);
        m_metaDirty = true;
    }

    /**
     * @brief Called after a boot block is successfully added to the library, so the
     *        host can record it in the brainfile. Args: (name, sector0, sector1).
     */
    void SetOnLibraryEntryAdded(std::function<void(const std::string&, const std::vector<uint8_t>&,
                                                   const std::vector<uint8_t>&)>
                                    cb)
    {
        m_onLibraryEntryAdded = std::move(cb);
    }

    /**
     * @brief Set the callback used to identify a library boot block's two sectors.
     */
    void SetIdentifyCallback(
        std::function<core::BootBlockInfo(const std::vector<uint8_t>&, const std::vector<uint8_t>&)>
            callback)
    {
        m_identify = std::move(callback);
    }

    /**
     * @brief Set the callback that installs the given boot block onto the loaded disk.
     */
    void SetOnInstallBootBlock(
        std::function<void(const std::vector<uint8_t>&, const std::vector<uint8_t>&)> callback)
    {
        m_onInstallBootBlock = std::move(callback);
    }

    /**
     * @brief Inform the view whether a disk document is currently loaded.
     */
    void SetDocumentLoaded(bool loaded) { m_documentLoaded = loaded; }

    // ── Live preview (emulator tab) ──────────────────────────────────────────
    struct PreviewFrame
    {
        ImTextureID texture = 0;
        float aspect = 4.0f / 3.0f;
        bool valid = false;
    };

    void SetOnStartPreview(
        std::function<void(const std::vector<uint8_t>&, const std::vector<uint8_t>&)> cb)
    {
        m_onStartPreview = std::move(cb);
    }
    void SetOnStopPreview(std::function<void()> cb) { m_onStopPreview = std::move(cb); }
    void SetOnSelectPreviewModel(std::function<void(int)> cb)
    {
        m_onSelectPreviewModel = std::move(cb);
    }
    void SetPreviewFrameProvider(std::function<PreviewFrame()> cb)
    {
        m_getPreviewFrame = std::move(cb);
    }
    void SetPreviewModels(std::vector<std::string> names) { m_previewModels = std::move(names); }
    void SetWindow(GLFWwindow* window) { m_window = window; }
    // Called when the preview should send a key event. Args: (down, retrokey_as_int).
    void SetPreviewKeyCallback(std::function<void(bool, int)> cb)
    {
        m_previewSendKey = std::move(cb);
    }
    // Called each frame while mouse is captured. Args: (dx, dy, left, right, middle).
    void SetPreviewMouseCallback(std::function<void(int16_t, int16_t, bool, bool, bool)> cb)
    {
        m_previewSetMouse = std::move(cb);
    }

    // State queried by Application to drive the preview emulator each frame.
    bool IsPreviewRunning() const { return m_previewRunning; }
    bool IsPreviewTabActive() const { return m_previewTabActive; }
    bool IsPreviewFocused() const { return m_previewFocused; }
    void NotifyPreviewStopped() { m_previewRunning = false; }
    void ReleasePreviewMouse();

    /**
     * @brief Open the viewer window.
     */
    void Open();

    /**
     * @brief Close the viewer window.
     */
    void Close();

    bool* GetOpenStatePtr() { return &m_isOpen; }

    /**
     * @brief Draw the viewer window.
     */
    void Draw();

private:
    enum class Source { CurrentDisk, Library };

    void DrawBrainfileInfo();
    void DrawStructureDetails();
    void DrawPreviewTab();
    void HandlePreviewInput();
    void DrawRootblockLink(uint32_t block);
    std::string GetFilesystemName(uint8_t flags) const;

    // Source list / selection
    void DrawSourceList();
    void DrawFilters();
    void DrawLibraryToolbar();

    // Filtering / metadata
    void EnsureMetadata();
    bool PassesFilters(const std::string& name) const;
    const core::BootBlockInfo* MetaFor(const std::string& name) const;
    void ApplyPreviewModelForMachine(const std::string& kickstart);
    void SelectCurrentDisk();
    void SelectLibraryEntry(const std::string& name);
    void DisplaySectors(const std::vector<uint8_t>& sector0, const std::vector<uint8_t>& sector1);

    // Library dialogs
    void DrawLibraryAddDialog();
    void DrawRenameDialog();
    void DrawDeleteConfirm();
    void DrawInstallConfirm();

    // Displayed (selected source) data
    std::vector<uint8_t> m_sector0;
    std::vector<uint8_t> m_sector1;
    core::BootBlockInfo m_info;

    // Current disk data (source of truth for the "Current Disk" row)
    std::vector<uint8_t> m_currentS0;
    std::vector<uint8_t> m_currentS1;
    core::BootBlockInfo m_currentInfo;
    bool m_documentLoaded = false;

    Source m_selectedSource = Source::CurrentDisk;
    std::string m_selectedLibraryName;

    bool m_isOpen = false;
    std::function<void(uint32_t)> m_onNavigate;

    // Width of the left source-list panel; adjustable via the splitter gripper.
    float m_leftWidth = 220.0f;

    // Library integration
    core::BootBlockLibrary* m_library = nullptr;
    std::function<core::BootBlockInfo(const std::vector<uint8_t>&, const std::vector<uint8_t>&)>
        m_identify;
    std::function<void(const std::vector<uint8_t>&, const std::vector<uint8_t>&)>
        m_onInstallBootBlock;
    std::function<core::BootBlockInfo(const std::string&)> m_metadataProvider;
    std::function<void(const std::string&, const std::vector<uint8_t>&,
                       const std::vector<uint8_t>&)>
        m_onLibraryEntryAdded;

    // Per-entry brainfile metadata cache + distinct filter option lists. Rebuilt
    // lazily (m_metaDirty) when the library or provider changes.
    std::unordered_map<std::string, core::BootBlockInfo> m_metaCache;
    std::vector<std::string> m_classOptions;   // index 0 == "All"
    std::vector<std::string> m_ksOptions;      // index 0 == "All"
    std::vector<std::string> m_machineOptions; // index 0 == "All"
    bool m_metaDirty = true;
    int m_filterClassIdx = 0;
    int m_filterKsIdx = 0;
    int m_filterMachineIdx = 0;
    char m_searchBuf[128] = {0};

    // Live preview state
    std::function<void(const std::vector<uint8_t>&, const std::vector<uint8_t>&)> m_onStartPreview;
    std::function<void()> m_onStopPreview;
    std::function<void(int)> m_onSelectPreviewModel;
    std::function<PreviewFrame()> m_getPreviewFrame;
    std::vector<std::string> m_previewModels;
    int m_previewModelIdx = 1; // default A500 (512 chip + 512 slow)
    bool m_previewRunning = false;
    bool m_previewTabActive = false;
    bool m_previewFocused = false;
    // Bumped whenever the selected boot block changes; the Preview tab auto-(re)loads when it
    // differs from the version currently being previewed, so flicking entries previews each one.
    int m_selectionVersion = 0;
    int m_previewedVersion = -1;

    // Input capture for the preview display area.
    GLFWwindow* m_window = nullptr;
    bool m_previewMouseCaptured = false;
    bool m_previewSuppressCaptureClick = false;
    std::unordered_set<int> m_previewPrevKeys;
    std::function<void(bool, int)> m_previewSendKey;
    std::function<void(int16_t, int16_t, bool, bool, bool)> m_previewSetMouse;

    // Dialog state
    bool m_showLibAddDialog = false;
    char m_addNameBuf[128] = {0};
    bool m_showRenameDialog = false;
    std::string m_renameOldName;
    char m_renameBuf[128] = {0};
    bool m_showDeleteConfirm = false;
    std::string m_deleteName;
    bool m_showInstallConfirm = false;
};

} // namespace adm::ui
