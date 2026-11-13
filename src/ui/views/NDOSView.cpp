#include "NDOSView.h"
#include "../ColorManager.h"

namespace adm::ui {

NDOSView::NDOSView() { LoadColors(); }

void NDOSView::SetData(const core::DiskInfo& info, const std::vector<bool>& zeroFilled)
{
    m_info = info;
    m_zeroFilled = zeroFilled;
    ResetCommonState();
}

void NDOSView::LoadColors()
{
    m_colorEmpty = ColorManager::Instance().GetBlockColor("NDOS_Empty");
    m_colorUsed = ColorManager::Instance().GetBlockColor("NDOS_Used");
}

const char* NDOSView::GetWindowTitle() const { return "NDOS Map"; }

const char* NDOSView::GetEmptyMessage() const { return "No volume mounted."; }

bool NDOSView::HasData() const { return !m_zeroFilled.empty(); }

ImU32 NDOSView::GetBlockColor(uint32_t block) const
{
    if (block >= m_zeroFilled.size()) {
        return IM_COL32(64, 64, 64, 255);
    }
    return m_zeroFilled[block] ? m_colorEmpty : m_colorUsed;
}

std::string NDOSView::GetBlockStatusText(uint32_t block) const
{
    if (block >= m_zeroFilled.size()) {
        return "Status: Unknown";
    }
    return fmt::format("Status: {}", m_zeroFilled[block] ? "Empty (00)" : "Used (Data)");
}

std::vector<utils::LegendItem> NDOSView::GetLegendItems() const
{
    return {{"Free (All 0s)", m_colorEmpty}, {"Used (Data)", m_colorUsed}};
}

} // namespace adm::ui
