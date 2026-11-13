#pragma once

#include <imgui.h>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "../../core/AdfWrapper.h" // For BlockDetails

namespace adm::ui {

/**
 * @brief Renders the structure details table for the Hex Editor.
 */
class BlockDetailsView
{
public:
    using NavigateCallback = std::function<void(uint32_t)>;
    using HoverCallback = std::function<void(int, int)>;

    /**
     * @brief Draws the structure details table.
     * @param blockDef The JSON definition for the current block structure.
     * @param data The raw block data.
     * @param baseOffset The base offset for address calculations (e.g. 0x200 for bootblocks).
     * @param details Analyzed details of the block (used for context like checksum validation).
     */
    void Draw(const nlohmann::json& blockDef, const std::vector<uint8_t>& data, uint32_t baseOffset,
              const core::BlockDetails& details);

    void SetOnNavigate(NavigateCallback cb) { m_onNavigate = cb; }
    void SetOnHover(HoverCallback cb) { m_onHover = cb; }

private:
    void DrawField(const nlohmann::json& field, uint32_t& currentOffset);
    std::string FormatDate(uint32_t days, uint32_t mins, uint32_t ticks);
    void DrawRawValue(const nlohmann::json& field, const std::string& type, uint32_t size,
                      uint32_t offset);
    void DrawProcessedValue(const nlohmann::json& field, const std::string& type, uint32_t size,
                            uint32_t offset);
    void DrawColorPreview(const nlohmann::json& field);
    void RenderBlockLink(uint32_t block);
    void ParseFormat(const std::string& formatStr, std::string& outFmt, int& outWidth);
    uint32_t GetFieldSize(const nlohmann::json& field);

    // Callbacks
    NavigateCallback m_onNavigate;
    HoverCallback m_onHover;

    // Context for current Draw call
    const nlohmann::json* m_currentDef = nullptr;
    const std::vector<uint8_t>* m_currentData = nullptr;
    uint32_t m_currentBaseOffset = 0;
    const core::BlockDetails* m_currentDetails = nullptr;
};

} // namespace adm::ui
