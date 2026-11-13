#include "ChangeMapView.h"
#include "../ColorManager.h"

namespace adm::ui {

void ChangeMapView::SetData(const core::DiskInfo& info, const std::vector<bool>& modified)
{
    m_info = info;
    m_modified = modified;
    ResetCommonState();
}

const char* ChangeMapView::GetWindowTitle() const { return "Change Map"; }

const char* ChangeMapView::GetEmptyMessage() const { return "No volume mounted."; }

bool ChangeMapView::HasData() const { return !m_modified.empty(); }

ImU32 ChangeMapView::GetBlockColor(uint32_t block) const
{
    if (block >= m_modified.size()) {
        return IM_COL32(64, 64, 64, 255);
    }
    bool isModified = m_modified[block];
    // Orange/red for modified, dark gray for unmodified
    return isModified ? IM_COL32(255, 100, 50, 255) : IM_COL32(60, 60, 60, 255);
}

std::string ChangeMapView::GetBlockStatusText(uint32_t block) const
{
    if (block >= m_modified.size()) {
        return "Status: Unknown";
    }
    return fmt::format("Status: {}", m_modified[block] ? "Modified" : "Unchanged");
}

std::vector<utils::LegendItem> ChangeMapView::GetLegendItems() const
{
    return {{"Unchanged", IM_COL32(60, 60, 60, 255)}, {"Modified", IM_COL32(255, 100, 50, 255)}};
}

} // namespace adm::ui
