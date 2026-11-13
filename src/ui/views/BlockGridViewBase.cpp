#include "BlockGridViewBase.h"
#include <imgui.h>
#include "../utils/LegendUtils.h"

#include <algorithm>

namespace adm::ui {

void BlockGridViewBase::SetOnBlockSelected(std::function<void(uint32_t)> callback)
{
    m_onBlockSelected = callback;
}

void BlockGridViewBase::SetOnBlocksSelected(std::function<void(const std::set<uint32_t>&)> callback)
{
    m_onBlocksSelected = callback;
}

void BlockGridViewBase::SetOnClearBlock(std::function<void(uint32_t)> callback)
{
    m_onClearBlock = callback;
}

void BlockGridViewBase::SetOnSaveBlocks(std::function<void(const std::set<uint32_t>&)> callback)
{
    m_onSaveBlocks = callback;
}

void BlockGridViewBase::SetOnInsertBinary(std::function<void(uint32_t)> callback)
{
    m_onInsertBinary = callback;
}

void BlockGridViewBase::SetHighlightedBlocks(const std::vector<uint32_t>& blocks)
{
    m_highlightedBlocks.clear();
    m_highlightedBlocks.insert(blocks.begin(), blocks.end());
}

void BlockGridViewBase::SetSelectedBlocks(const std::set<uint32_t>& blocks)
{
    m_selectedBlocks = blocks;
}

void BlockGridViewBase::SetCurrentBlock(uint32_t block) { m_currentBlock = block; }

void BlockGridViewBase::ClearSelection()
{
    m_selectedBlocks.clear();
    if (m_onBlocksSelected) {
        m_onBlocksSelected(m_selectedBlocks);
    }
}

void BlockGridViewBase::ResetCommonState()
{
    m_highlightedBlocks.clear();
    m_selectedBlocks.clear();
    m_currentBlock = 0xFFFFFFFF;
    m_isDragging = false;
}

void BlockGridViewBase::Open() { m_isOpen = true; }

void BlockGridViewBase::Restore()
{
    m_isOpen = true;
    m_noFocusOnAppearing = true;
}

void BlockGridViewBase::Close() { m_isOpen = false; }

void BlockGridViewBase::Draw()
{
    if (!m_isOpen)
        return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }

    if (ImGui::Begin(GetWindowTitle(), &m_isOpen, flags)) {
        if (!HasData()) {
            ImGui::TextDisabled("%s", GetEmptyMessage());
        } else {
            DrawGrid();
        }
    }
    ImGui::End();
}

void BlockGridViewBase::DrawAdditionalTooltipInfo(uint32_t /*block*/)
{
    // Default: no additional info. Derived classes can override.
}

void BlockGridViewBase::DrawGrid()
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Calculate scale based on current font size (assuming base is 16.0f)
    const float scale = ImGui::GetFontSize() / 16.0f;

    const float squareSize = 10.0f * scale;
    const float padding = 2.0f * scale;
    const float headSpacing = 12.0f * scale;
    const float labelWidth = 40.0f * scale;

    ImVec2 startPos = ImGui::GetCursorScreenPos();
    float currentY = startPos.y;

    float availableWidth = ImGui::GetContentRegionAvail().x;
    uint32_t numBlocksPerRow = m_info.heads * m_info.sectors;

    float squareWidth = squareSize;
    if (numBlocksPerRow > 0) {
        float requiredNonBlockWidth = labelWidth +
                                      (m_info.heads > 1 ? (m_info.heads - 1) * headSpacing : 0) +
                                      (numBlocksPerRow > 1 ? (numBlocksPerRow - 1) * padding : 0);
        float totalBlockWidthAvail = availableWidth - requiredNonBlockWidth;
        float dynamicSquareWidth = totalBlockWidthAvail / numBlocksPerRow;
        squareWidth = std::max(squareSize, dynamicSquareWidth);
    }
    float squareHeight = squareSize;

    // Clear and rebuild block positions
    m_blockRects.clear();
    m_blockRects.reserve(m_info.cylinders * m_info.heads * m_info.sectors);

    ImVec2 gridEnd = startPos;

    // Vertical culling window: rows scrolled out of view still record their rects (drag
    // selection needs them) but skip every draw call, color lookup, and hover test — a
    // hardfile-sized grid used to submit one AddRectFilled per block per frame regardless.
    const float visMinY = ImGui::GetWindowPos().y - squareSize;
    const float visMaxY = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y + squareSize;
    // Hover/click must not act through another window drawn on top of this one:
    // IsMouseHoveringRect deliberately ignores z-order.
    const bool windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

    for (uint32_t cyl = 0; cyl < m_info.cylinders; ++cyl) {
        const bool rowVisible = currentY + squareHeight >= visMinY && currentY <= visMaxY;
        if (rowVisible) {
            std::string cylLabel = fmt::format("C{:02d}", cyl);
            drawList->AddText({startPos.x, currentY}, ImGui::GetColorU32(ImGuiCol_Text),
                              cylLabel.c_str());
        }

        float currentX = startPos.x + labelWidth;

        for (uint32_t head = 0; head < m_info.heads; head++) {
            if (head > 0) {
                currentX += headSpacing;
            }

            for (uint32_t sec = 0; sec < m_info.sectors; sec++) {
                uint32_t block = (cyl * m_info.heads + head) * m_info.sectors + sec;

                ImVec2 rectMin = {currentX, currentY};
                ImVec2 rectMax = {currentX + squareWidth, currentY + squareHeight};

                // Store block position for selection
                m_blockRects.push_back({block, rectMin, rectMax});

                // Track grid bounds
                if (rectMax.x > gridEnd.x)
                    gridEnd.x = rectMax.x;
                if (rectMax.y > gridEnd.y)
                    gridEnd.y = rectMax.y;

                if (!rowVisible) {
                    currentX += squareWidth + padding;
                    continue;
                }

                ImU32 color = GetBlockColor(block);
                bool isHighlighted = m_highlightedBlocks.count(block) > 0;
                bool isSelected = m_selectedBlocks.count(block) > 0;

                drawList->AddRectFilled(rectMin, rectMax, color);

                // Draw selection indicator (cyan border)
                if (isSelected) {
                    drawList->AddRect(rectMin, rectMax, IM_COL32(0, 255, 255, 255), 0.0f, 0,
                                      2.0f * scale);
                }
                // Draw highlight indicator (yellow border) - only if not selected
                else if (isHighlighted) {
                    drawList->AddRect(rectMin, rectMax, IM_COL32(255, 255, 0, 255), 0.0f, 0,
                                      2.0f * scale);
                }

                if (block == m_currentBlock) {
                    drawList->AddCircleFilled(
                        {rectMin.x + squareWidth * 0.5f, rectMin.y + squareHeight * 0.5f},
                        squareHeight * 0.35f, IM_COL32(255, 255, 255, 255));
                }

                // Only show tooltip when this window is actually hovered (not covered by
                // another), not dragging, and no context menu is open
                if (windowHovered && !m_isDragging &&
                    !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                    ImGui::IsMouseHoveringRect(rectMin, rectMax)) {
                    drawList->AddRect(rectMin, rectMax, IM_COL32(255, 255, 255, 255), 0.0f, 0,
                                      1.0f * scale);

                    ImGui::BeginTooltip();
                    ImGui::Text("Block: %u", block);
                    ImGui::Text("Cyl: %u, Head: %u, Sec: %u", cyl, head, sec);
                    std::string statusText = GetBlockStatusText(block);
                    if (!statusText.empty()) {
                        ImGui::Text("%s", statusText.c_str());
                    }
                    DrawAdditionalTooltipInfo(block);
                    if (isHighlighted) {
                        ImGui::TextColored({1.0f, 1.0f, 0.0f, 1.0f}, "Part of selected file");
                    }
                    if (isSelected) {
                        ImGui::TextColored({0.0f, 1.0f, 1.0f, 1.0f}, "Selected");
                    }
                    ImGui::EndTooltip();

                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (m_onBlockSelected) {
                            m_onBlockSelected(block);
                        }
                    }

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        m_currentBlock = block;
                        if (m_onBlockSelected) {
                            m_onBlockSelected(block);
                        }
                        ImGui::OpenPopup("BlockGridContextMenu");
                    }
                }
                currentX += squareWidth + padding;
            }
        }
        // currentX += headSpacing; // Removed as it was ineffective at end of loop
        currentY += squareSize + padding;
    }

    // Handle drag selection
    HandleDragSelection(drawList);

    DrawContextMenu();
    DrawClearConfirmModal();

    currentY += 5.0f * scale;

    // Draw legend (items derive from the static color map — build once per instance)
    if (m_legendCache.empty())
        m_legendCache = GetLegendItems();
    float finalY = utils::DrawLegend(m_legendCache, startPos.x, currentY, labelWidth, scale);

    ImGui::SetCursorScreenPos({startPos.x, finalY + 10.0f * scale});
    ImGui::Dummy({0, 0});
}

void BlockGridViewBase::HandleDragSelection(ImDrawList* drawList)
{
    ImVec2 mousePos = ImGui::GetMousePos();

    // Start dragging on left mouse button press — only when this window is hovered, so a
    // click on a window stacked above the grid can't start a selection underneath it.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        uint32_t blockUnderMouse = GetBlockAtPosition(mousePos);
        if (blockUnderMouse != 0xFFFFFFFF) {
            m_isDragging = true;
            m_dragStartBlock = blockUnderMouse;
            m_selectedBlocks.clear();
            m_selectedBlocks.insert(blockUnderMouse);
        }
    }

    // While dragging
    if (m_isDragging) {
        uint32_t currentBlock = GetBlockAtPosition(mousePos);

        // Update selection to be contiguous range from start to current
        if (currentBlock != 0xFFFFFFFF) {
            UpdateSelectionRange(m_dragStartBlock, currentBlock);
        }

        // Draw visual indicator on selected blocks
        for (const auto& br : m_blockRects) {
            if (m_selectedBlocks.count(br.block) > 0) {
                drawList->AddRectFilled(br.min, br.max, IM_COL32(0, 120, 215, 80));
            }
        }

        // End dragging on mouse release
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            m_isDragging = false;

            // Notify callback of final selection
            if (m_onBlocksSelected && !m_selectedBlocks.empty()) {
                m_onBlocksSelected(m_selectedBlocks);
            }
        }
    }
}

uint32_t BlockGridViewBase::GetBlockAtPosition(const ImVec2& pos) const
{
    for (const auto& br : m_blockRects) {
        if (pos.x >= br.min.x && pos.x <= br.max.x && pos.y >= br.min.y && pos.y <= br.max.y) {
            return br.block;
        }
    }
    return 0xFFFFFFFF;
}

void BlockGridViewBase::UpdateSelectionRange(uint32_t startBlock, uint32_t endBlock)
{
    m_selectedBlocks.clear();

    // Select all blocks in the contiguous range
    uint32_t minBlock = std::min(startBlock, endBlock);
    uint32_t maxBlock = std::max(startBlock, endBlock);

    for (uint32_t block = minBlock; block <= maxBlock; ++block) {
        m_selectedBlocks.insert(block);
    }
}

void BlockGridViewBase::DrawContextMenu()
{
    if (ImGui::BeginPopup("BlockGridContextMenu")) {
        bool hasSelection = !m_selectedBlocks.empty();

        if (ImGui::MenuItem("Clear Block")) {
            m_blockToClear = m_currentBlock;
            m_clearingSelection = false;
            ImGui::CloseCurrentPopup();
            m_showClearConfirm = true;
        }

        if (ImGui::MenuItem("Clear Selection", nullptr, false, hasSelection)) {
            m_clearingSelection = true;
            ImGui::CloseCurrentPopup();
            m_showClearConfirm = true;
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Insert Binary File...")) {
            if (m_onInsertBinary) {
                // Use first selected block if there's a selection, otherwise current block
                uint32_t startBlock = hasSelection ? *m_selectedBlocks.begin() : m_currentBlock;
                m_onInsertBinary(startBlock);
            }
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::MenuItem("Save Blocks As...", nullptr, false, hasSelection)) {
            if (m_onSaveBlocks) {
                m_onSaveBlocks(m_selectedBlocks);
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Deselect All", nullptr, false, hasSelection)) {
            ClearSelection();
        }

        ImGui::Separator();

        if (hasSelection) {
            ImGui::TextDisabled("%zu blocks selected", m_selectedBlocks.size());
        } else {
            ImGui::TextDisabled("No selection");
        }

        ImGui::EndPopup();
    }

    if (m_showClearConfirm) {
        ImGui::OpenPopup("Confirm Clear Block");
        m_showClearConfirm = false;
    }
}

void BlockGridViewBase::DrawClearConfirmModal()
{
    // Always center this modal
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Confirm Clear Block", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_clearingSelection) {
            ImGui::Text("Are you sure you want to clear %zu blocks?", m_selectedBlocks.size());
        } else {
            ImGui::Text("Are you sure you want to clear block %u?", m_blockToClear);
        }
        ImGui::Separator();

        if (ImGui::Button("Clear", ImVec2(120, 0))) {
            if (m_onClearBlock) {
                if (m_clearingSelection) {
                    // Copy selection since OnClearBlock may trigger view refresh which resets it
                    std::set<uint32_t> blocksToClear = m_selectedBlocks;
                    for (uint32_t block : blocksToClear) {
                        m_onClearBlock(block);
                    }
                } else if (m_blockToClear != 0xFFFFFFFF) {
                    m_onClearBlock(m_blockToClear);
                }
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
}

} // namespace adm::ui
