#pragma once

#include "BlockEditorBase.h"

#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../../core/AdfWrapper.h"
#include "../../core/DefinitionManager.h"
#include "../../core/DiskSearcher.h"
#include "BlockDetailsView.h"

namespace adm::ui {

/**
 * @brief Hex editor view for displaying and analyzing raw block data.
 *
 * Provides a hex dump view with structure highlighting, block navigation,
 * and interactive field inspection based on JSON definitions.
 */
class BlockEditorView : public BlockEditorBase
{
    struct ByteHighlight
    {
        ImU32 bg = IM_COL32(0, 0, 0, 0);
        ImU32 fg = IM_COL32(255, 255, 255, 255);
        bool active = false;
    };

public:
    BlockEditorView();

    void SetOnReadBlock(std::function<std::vector<uint8_t>(uint32_t)> callback);
    void SetOnAnalyzeBlock(
        std::function<core::BlockDetails(uint32_t, std::span<const uint8_t>)> callback);
    void SetOnBlockChanged(std::function<void(uint32_t)> callback);
    void
    SetOnWriteBlock(std::function<bool(uint32_t, uint32_t, const std::vector<uint8_t>&)> callback);
    void SetOnRestoreFile(std::function<bool(uint32_t)> callback);
    void SetIsBlockModified(std::function<bool(uint32_t)> callback);
    void SetOnRevertBlock(std::function<bool(uint32_t)> callback);
    void SetIsBlockCached(std::function<bool(uint32_t)> callback);
    void SetOnReadOriginalBlock(std::function<std::vector<uint8_t>(uint32_t)> callback);
    void SetOnHoverRange(std::function<void(int, int)> callback);

    // Mirrors the document's write protection. The hex editor is the one write
    // path the DocumentCapabilities gating does not cover, because it edits
    // bytes rather than entries.
    void SetReadOnly(bool readOnly) { m_hexState.ReadOnly = readOnly; }
    [[nodiscard]] bool IsReadOnly() const { return m_hexState.ReadOnly; }

    enum class SearchType { Hex, ASCII }; // Moved to public

    // Search functionality
    void ShowFindDialog();
    void FindNext();
    void FindPrevious();
    void UpdateBlockState();
    void SetIsFFS(bool isFFS);
    void SetDiskSearcher(core::DiskSearcher* searcher);
    // New methods for Advanced Find integration
    void SetSearchContext(const std::vector<uint8_t>& pattern, bool caseSensitive,
                          SearchType type); // Sets internal search state
    void HighlightSearchResult(uint32_t block, size_t offset,
                               size_t length); // Highlights a specific result

    // Internal helper for WriteCallback
    void OnWrite(int offset, void* buf, int size);

    void Draw() override;

private:
    // One sector of the track view. The current block draws from m_data/m_hexState instead;
    // its entry only holds the block number.
    struct TrackSector
    {
        uint32_t block = 0;
        std::string typeLabel;
        std::vector<uint8_t> data;
        ImGuiHexEditorState state;
        std::vector<FieldInfo> fields;
        const ImU32* textColor = nullptr;
        uint32_t blockSize = 512;
    };

    bool ShowsBootBlock() const override;
    // The track view draws every sector alike, with field colours but no field outlines
    bool DrawsFieldBorders() const override { return !m_showTrack || !TrackViewAvailable(); }
    // A floppy's track (11 or 22 sectors). A hardfile's geometry is a guess of thousands of
    // sectors a "track" - read and drawn a hex editor each, every frame - so it has no Track view.
    static constexpr uint32_t kMaxTrackSectors = 22;
    bool TrackViewAvailable() const { return m_sectors <= kMaxTrackSectors; }
    std::string DefinitionFor(uint32_t block, const core::BlockDetails& details,
                              const std::vector<uint32_t>& bitmapBlocks) const;
    void LoadTrack();
    void DrawBlockData(const ImVec2& size);
    float HexContentWidth() const;
    std::string SectorHeader(uint32_t block, const std::string& typeLabel) const;

    void RenderBlockLink(uint32_t block);
    void RenderProtection(uint8_t access);
    void UpdateHighlights();

    // Callbacks (data and hex state are inherited from HexViewBase)
    std::function<std::vector<uint8_t>(uint32_t)> m_readBlockCallback;
    std::function<core::BlockDetails(uint32_t, std::span<const uint8_t>)> m_analyzeCallback;
    std::function<void(uint32_t)> m_onBlockChanged;
    std::function<bool(uint32_t, uint32_t, const std::vector<uint8_t>&)> m_writeBlockCallback;
    std::function<bool(uint32_t)> m_onRestoreFile;
    std::function<bool(uint32_t)> m_isBlockModified;
    std::function<bool(uint32_t)> m_onRevertBlock;
    std::function<bool(uint32_t)> m_isBlockCached;
    std::function<std::vector<uint8_t>(uint32_t)> m_readOriginalBlockCallback;
    std::function<void(int, int)> m_onHoverRange;

    // State (base state inherited from HexViewBase)
    core::BlockDetails m_lastDetails;
    std::vector<uint8_t> m_originalData; // Original block data for diff highlighting

    // Definition tracking (definition itself is in base class)
    std::string m_loadedDefinitionName;
    std::vector<ByteHighlight> m_byteHighlights;
    int m_lastHighlightedBlock = -1;

    // Additional highlighting state
    std::vector<uint32_t> m_bitmapBlocks;

    // Sub-views
    BlockDetailsView m_structureView;

    bool m_isFFS = false;

    // Track view: every sector of the current block's track and side, stacked
    bool m_showTrack = false;
    std::vector<TrackSector> m_trackSectors;
    int m_scrollToCurrentFrames = 0;

    // Navigation history
    // m_blockHistory, m_historyIndex, etc. are now in HexViewBase
    // void NavigateToHistoryIndex(int index); // Use base implementation
    void LoadBlock(uint32_t block); // Internal: loads block without adding to history

public:
    // Focus tracking (fires once when the Hex View window gains focus)
    void SetOnFocused(std::function<void()> callback) { m_onFocused = std::move(callback); }

    // Override base navigation hook
    void OnNavigate(uint32_t block) override;

    // Override base GoToBlock
    void GoToBlock(uint32_t block) override;

    // Force navigation even if block is same as current (used for reload)
    void GoToBlockForce(uint32_t block);

    // Search State
    // SearchType moved to public

    core::DiskSearcher* m_diskSearcher = nullptr;
    bool m_findDialogOpen = false;
    bool m_focusSearchInput = false;
    SearchType m_lastSearchType = SearchType::ASCII;
    bool m_isCaseSensitive = false;
    std::string m_searchAscii;
    std::string m_searchHex;
    // m_searchBytes removed, local to PerformSearch now

    // Deferred navigation (to avoid modifying data while StructureDetailsView is drawing)
    bool m_pendingNavigate = false;
    uint32_t m_pendingNavigateBlock = 0;

    // Search Result State
    bool m_hasSearchResult = false;
    uint32_t m_searchResultBlock = 0;
    size_t m_searchResultOffset = 0;
    size_t m_searchResultLength = 0;

    void PerformSearch(bool reverse); // Interface to m_diskSearcher
    // MatchesAt removed

    void Open();
    void Restore();
    void Close();
    [[nodiscard]] bool IsOpen() const { return m_isBlockOpen; }

    // Focus tracking
    std::function<void()> m_onFocused;
    bool m_wasFocused = false;

    // Window open state
    bool m_isBlockOpen = true;
    bool m_isBlockDetailsOpen = true;
    bool m_noFocusOnAppearing = false;

public:
    bool* GetOpenStatePtr() { return &m_isBlockOpen; }
    bool* GetBlockDetailsOpenStatePtr() { return &m_isBlockDetailsOpen; }
    bool GetShowTrack() const { return m_showTrack; }
    void SetShowTrack(bool show)
    {
        m_showTrack = show;
        if (!m_blockDef.is_null())
            ApplyHighlights();
        LoadTrack();
    }
    int GetBytesPerLine() const { return m_hexState.BytesPerLine; }
    void SetBytesPerLine(int bytesPerLine)
    {
        if (bytesPerLine == 16 || bytesPerLine == 24 || bytesPerLine == 32)
            m_hexState.BytesPerLine = bytesPerLine;
    }
};

} // namespace adm::ui
