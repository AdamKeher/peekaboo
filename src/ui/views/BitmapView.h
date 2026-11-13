#pragma once

#include <vector>

#include "BlockGridViewBase.h"

namespace adm::ui {

/**
 * @brief Disk bitmap visualization showing used vs free blocks from the filesystem.
 */
class BitmapView : public BlockGridViewBase
{
public:
    void SetData(const core::DiskInfo& info, const std::vector<bool>& usage);

protected:
    const char* GetWindowTitle() const override;
    const char* GetEmptyMessage() const override;
    bool HasData() const override;
    ImU32 GetBlockColor(uint32_t block) const override;
    std::string GetBlockStatusText(uint32_t block) const override;
    std::vector<utils::LegendItem> GetLegendItems() const override;

private:
    std::vector<bool> m_usage;
};

} // namespace adm::ui
