#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "BlockGridViewBase.h"

namespace adm::ui {

/**
 * @brief Block type map visualization showing each block's type with color coding.
 */
class BlockMapView : public BlockGridViewBase
{
public:
    BlockMapView();

    void SetData(const core::DiskInfo& info, const std::vector<std::string>& blockTypes);

protected:
    const char* GetWindowTitle() const override;
    const char* GetEmptyMessage() const override;
    bool HasData() const override;
    ImU32 GetBlockColor(uint32_t block) const override;
    std::string GetBlockStatusText(uint32_t block) const override;
    std::vector<utils::LegendItem> GetLegendItems() const override;
    void DrawAdditionalTooltipInfo(uint32_t block) override;

private:
    void LoadColors();

    std::vector<std::string> m_blockTypes;
    std::map<std::string, ImU32> m_colorMap;
};

} // namespace adm::ui
