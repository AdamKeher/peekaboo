#pragma once

#include <imgui.h>
#include "imgui_hex.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "../../core/BrainFile.h"

namespace adm {
namespace core {
class DefinitionManager;
}
} // namespace adm

namespace adm::ui {

/**
 * @brief Base class for hex editor views.
 *
 * Provides common hex display functionality shared between the main
 * hex editor and the original data comparison view.
 */
class BlockEditorBase
{
public:
    BlockEditorBase();
    virtual ~BlockEditorBase() = default;

    void SetData(const std::vector<uint8_t>& data);
    void SetBlockSize(uint32_t size);
    void SetGeometry(uint32_t heads, uint32_t sectors);
    void SetDiskLayout(uint32_t totalBlocks, uint32_t rootBlock);
    void SetDefinitionManager(core::DefinitionManager* manager);
    void SetBootBlockInfo(const core::BootBlockInfo& info);

    [[nodiscard]] uint32_t GetCurrentBlock() const { return m_currentBlock; }
    [[nodiscard]] uint32_t GetBlockSize() const { return m_blockSize; }
    [[nodiscard]] const std::vector<uint8_t>& GetData() const { return m_data; }
    [[nodiscard]] const nlohmann::json& GetBlockDefinition() const { return m_blockDef; }
    [[nodiscard]] const std::string& GetCurrentDefinitionFile() const
    {
        return m_currentDefinitionFile;
    }

    // Highlighting
    void SetHoverRange(int from, int to);
    void SetBlockDefinition(const nlohmann::json& def, const std::string& defFile);
    void ClearHighlights();

    void SetOnSaveBootBlock(std::function<void(const core::BootBlockInfo&)> callback)
    {
        m_onSaveBootBlock = std::move(callback);
    }

    virtual void Draw() = 0;

    // Toolbar
    virtual void DrawToolbar();
    virtual void GoToBlock(uint32_t block);

protected:
    void DrawHexEditor(const ImVec2& size, const char* editorId = "##hex_editor",
                       ImGuiWindowFlags windowFlags = 0);
    void InitHexState();
    virtual void ApplyHighlights();
    void ApplyHoverHighlight();
    // True when block 0 or 1 is on screen, so the bootblock identification is shown.
    [[nodiscard]] virtual bool ShowsBootBlock() const { return m_currentBlock <= 1; }
    // True when the current block's fields get the white outline.
    [[nodiscard]] virtual bool DrawsFieldBorders() const { return true; }
    void DrawBootBlockInfo();
    void DrawAddDialog();
    float GetBottomHeight() const;
    uint32_t GetViewBaseOffset() const;
    uint32_t GetFieldSize(const nlohmann::json& field);
    void LoadDefinition(const std::string& path);

    // Data
    std::vector<uint8_t> m_data;

    // State
    uint32_t m_currentBlock = 0;
    ImGuiHexEditorState m_hexState;
    uint32_t m_blockSize = 512;
    uint32_t m_heads = 0;
    uint32_t m_sectors = 0;
    uint32_t m_totalBlocks = 0;
    uint32_t m_rootBlock = 0;

    // Definition and highlighting
    core::DefinitionManager* m_definitionManager = nullptr;
    nlohmann::json m_blockDef;
    std::string m_currentDefinitionFile;
    int m_staticRangeCount = 0;
    bool m_hasHoverRange = false;
    int m_hoverRangeFrom = 0;
    int m_hoverRangeTo = 0;

    // Bootblock identification info
    core::BootBlockInfo m_bootBlockInfo;

    // Add Bootblock Dialog State
    bool m_showAddDialog = false;
    core::BootBlockInfo m_newBootBlockInfo;
    std::function<void(const core::BootBlockInfo&)> m_onSaveBootBlock;
    int m_selectedClassIndex = 0;

    // Field info for tooltips
    struct FieldInfo
    {
        int from;
        int to;
        std::string name;
    };
    std::vector<FieldInfo> m_fieldRanges;

    // Field highlight ranges for one block laid out by `def`, starting `baseOffset` bytes into
    // the structure. `border` draws the white outline around each field.
    void BuildHighlightRanges(const nlohmann::json& def, uint32_t baseOffset, bool border,
                              ImVector<ImGuiHexEditorHighlightRange>& ranges,
                              std::vector<FieldInfo>& fields);

    // Colors
    ImU32 m_defaultTextColor = IM_COL32(255, 255, 255, 255);

    // Toolbar State (moved from derived classes)
    std::vector<uint32_t> m_blockHistory;
    int m_historyIndex = -1;
    int m_blockInputValue = 0;
    bool m_blockInputActive = false;

    // Helper to add block to history (called by derived classes when navigation occurs)
    void AddToHistory(uint32_t block);
    void NavigateToHistoryIndex(int index);

    // Virtual hook for navigation (so derived classes can handle side effects)
    virtual void OnNavigate(uint32_t block) {}
};

} // namespace adm::ui
