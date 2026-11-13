#pragma once

#include <fmt/core.h>
#include <imgui.h>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "../../core/AdfWrapper.h"
#include "../utils/LegendUtils.h"

namespace adm::ui {

/**
 * @brief Base class for block grid visualizations (BlockMapView, BitmapView, NDOSView).
 *
 * Provides common functionality for rendering blocks in a cylinder/head/sector grid,
 * handling mouse interaction, drag selection, context menus, and the clear block confirmation
 * modal.
 */
class BlockGridViewBase
{
public:
    virtual ~BlockGridViewBase() = default;

    void SetOnBlockSelected(std::function<void(uint32_t)> callback);
    void SetOnBlocksSelected(std::function<void(const std::set<uint32_t>&)> callback);
    void SetOnClearBlock(std::function<void(uint32_t)> callback);
    void SetOnSaveBlocks(std::function<void(const std::set<uint32_t>&)> callback);
    void SetOnInsertBinary(std::function<void(uint32_t)> callback);
    void SetHighlightedBlocks(const std::vector<uint32_t>& blocks);
    void SetSelectedBlocks(const std::set<uint32_t>& blocks);
    void SetCurrentBlock(uint32_t block);
    void ClearSelection();

    const std::set<uint32_t>& GetSelectedBlocks() const { return m_selectedBlocks; }

    void Open();
    void Restore();
    void Close();
    [[nodiscard]] bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    void Draw();

protected:
    bool m_isOpen = true;
    bool m_noFocusOnAppearing = false;
    // Customization points - derived classes must implement these
    virtual const char* GetWindowTitle() const = 0;
    virtual const char* GetEmptyMessage() const = 0;
    virtual bool HasData() const = 0;
    virtual ImU32 GetBlockColor(uint32_t block) const = 0;
    virtual std::string GetBlockStatusText(uint32_t block) const = 0;
    virtual std::vector<utils::LegendItem> GetLegendItems() const = 0;

    // Optional customization - derived classes can override
    virtual void DrawAdditionalTooltipInfo(uint32_t block);

    // Common data members accessible to derived classes
    core::DiskInfo m_info;
    std::set<uint32_t> m_highlightedBlocks;
    std::set<uint32_t> m_selectedBlocks;
    uint32_t m_currentBlock = 0xFFFFFFFF;

    // Helper to reset common state when data changes
    void ResetCommonState();

private:
    // Block position tracking for selection
    struct BlockRect
    {
        uint32_t block;
        ImVec2 min;
        ImVec2 max;
    };

    void DrawGrid();
    void DrawContextMenu();
    void DrawClearConfirmModal();
    void HandleDragSelection(ImDrawList* drawList);
    uint32_t GetBlockAtPosition(const ImVec2& pos) const;
    void UpdateSelectionRange(uint32_t startBlock, uint32_t endBlock);

    std::vector<BlockRect> m_blockRects;

    // Legend built once per instance (items derive from the static color map).
    std::vector<utils::LegendItem> m_legendCache;

    // Drag selection state
    bool m_isDragging = false;
    uint32_t m_dragStartBlock = 0xFFFFFFFF;

    uint32_t m_blockToClear = 0xFFFFFFFF;
    bool m_showClearConfirm = false;
    bool m_clearingSelection = false;
    std::function<void(uint32_t)> m_onBlockSelected;
    std::function<void(const std::set<uint32_t>&)> m_onBlocksSelected;
    std::function<void(uint32_t)> m_onClearBlock;
    std::function<void(const std::set<uint32_t>&)> m_onSaveBlocks;
    std::function<void(uint32_t)> m_onInsertBinary;
};

} // namespace adm::ui
