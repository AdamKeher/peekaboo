#include "BlockMapView.h"
#include "../ColorManager.h"

namespace adm::ui {

BlockMapView::BlockMapView() { LoadColors(); }

void BlockMapView::SetData(const core::DiskInfo& info, const std::vector<std::string>& blockTypes)
{
    m_info = info;
    m_blockTypes = blockTypes;
    ResetCommonState();
}

void BlockMapView::LoadColors() { m_colorMap = ColorManager::Instance().GetBlockColors(); }

const char* BlockMapView::GetWindowTitle() const { return "Block Type Map"; }

const char* BlockMapView::GetEmptyMessage() const { return "No volume mounted."; }

bool BlockMapView::HasData() const { return !m_blockTypes.empty(); }

ImU32 BlockMapView::GetBlockColor(uint32_t block) const
{
    if (block >= m_blockTypes.size()) {
        return IM_COL32(64, 64, 64, 255);
    }
    return ColorManager::Instance().GetBlockColor(m_blockTypes[block]);
}

std::string BlockMapView::GetBlockStatusText(uint32_t block) const
{
    if (block >= m_blockTypes.size()) {
        return "Type: Unknown";
    }
    return fmt::format("Type: {}", m_blockTypes[block]);
}

void BlockMapView::DrawAdditionalTooltipInfo(uint32_t /*block*/)
{
    // BlockMapView adds a separator before the type info for consistency with original
    // This is handled in GetBlockStatusText instead
}

std::vector<utils::LegendItem> BlockMapView::GetLegendItems() const
{
    std::vector<utils::LegendItem> items;
    // Explicitly ordered keys
    std::vector<std::string> keys = {"Free",        "T_DATA",  "T_HEADER",   "T_LIST",
                                     "T_DIRC",      "ST_ROOT", "ST_USERDIR", "ST_FILE",
                                     "ST_LFILE",    "ST_LDIR", "ST_LSOFT",   "BootBlock",
                                     "BitmapBlock", "Bad",     "Unknown"};

    for (const auto& key : keys) {
        if (m_colorMap.count(key)) {
            items.push_back({key, m_colorMap.at(key)});
        }
    }

    return items;
}

} // namespace adm::ui
