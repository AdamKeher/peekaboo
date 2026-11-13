#pragma once

#include <vector>

#include "BlockGridViewBase.h"

namespace adm::ui {

/**
 * @brief Change map visualization showing modified vs unmodified blocks.
 */
class ChangeMapView : public BlockGridViewBase
{
public:
    void SetData(const core::DiskInfo& info, const std::vector<bool>& modified);

protected:
    const char* GetWindowTitle() const override;
    const char* GetEmptyMessage() const override;
    bool HasData() const override;
    ImU32 GetBlockColor(uint32_t block) const override;
    std::string GetBlockStatusText(uint32_t block) const override;
    std::vector<utils::LegendItem> GetLegendItems() const override;

private:
    std::vector<bool> m_modified;
};

} // namespace adm::ui
