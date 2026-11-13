#include "BitmapView.h"
#include "../ColorManager.h"

namespace adm::ui {

void BitmapView::SetData(const core::DiskInfo& info, const std::vector<bool>& usage)
{
    m_info = info;
    m_usage = usage;
    ResetCommonState();
}

const char* BitmapView::GetWindowTitle() const { return "Disk Bitmap"; }

const char* BitmapView::GetEmptyMessage() const
{
    return "No volume mounted or bitmap unavailable.";
}

bool BitmapView::HasData() const { return !m_usage.empty(); }

ImU32 BitmapView::GetBlockColor(uint32_t block) const
{
    if (block >= m_usage.size()) {
        return IM_COL32(64, 64, 64, 255);
    }
    bool isUsed = m_usage[block];
    return isUsed ? ColorManager::Instance().GetBlockColor("Bitmap_Used")
                  : ColorManager::Instance().GetBlockColor("Bitmap_Free");
}

std::string BitmapView::GetBlockStatusText(uint32_t block) const
{
    if (block >= m_usage.size()) {
        return "Status: Unknown";
    }
    return fmt::format("Status: {}", m_usage[block] ? "Used" : "Free");
}

std::vector<utils::LegendItem> BitmapView::GetLegendItems() const
{
    return {{"Free", ColorManager::Instance().GetBlockColor("Bitmap_Free")},
            {"Used", ColorManager::Instance().GetBlockColor("Bitmap_Used")}};
}

} // namespace adm::ui
