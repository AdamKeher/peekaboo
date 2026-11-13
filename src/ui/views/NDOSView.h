#pragma once

#include <vector>

#include "BlockGridViewBase.h"

namespace adm::ui {

/**
 * @brief NDOS Map visualization showing empty vs used blocks based on zero-fill status.
 */
class NDOSView : public BlockGridViewBase
{
public:
    NDOSView();

    void SetData(const core::DiskInfo& info, const std::vector<bool>& zeroFilled);

protected:
    const char* GetWindowTitle() const override;
    const char* GetEmptyMessage() const override;
    bool HasData() const override;
    ImU32 GetBlockColor(uint32_t block) const override;
    std::string GetBlockStatusText(uint32_t block) const override;
    std::vector<utils::LegendItem> GetLegendItems() const override;

private:
    void LoadColors();

    std::vector<bool> m_zeroFilled;
    ImU32 m_colorEmpty = IM_COL32(64, 64, 64, 255);
    ImU32 m_colorUsed = IM_COL32(51, 102, 255, 255);
};

} // namespace adm::ui
