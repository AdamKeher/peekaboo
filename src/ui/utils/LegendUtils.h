#pragma once

#include <imgui.h>
#include <string>
#include <vector>

namespace adm::ui::utils {

struct LegendItem
{
    std::string label;
    ImU32 color;
};

inline float DrawLegend(const std::vector<LegendItem>& items, float startX, float startY,
                        float indent, float scale)
{
    if (items.empty())
        return startY;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float currentX = startX + indent;
    float currentY = startY + 5.0f * scale;
    const float colorBoxSize = 12.0f * scale;

    float maxTextWidth = 0.0f;
    for (const auto& item : items) {
        float textWidth = ImGui::CalcTextSize(item.label.c_str()).x;
        if (textWidth > maxTextWidth)
            maxTextWidth = textWidth;
    }

    const float itemWidth = colorBoxSize + 5.0f * scale + maxTextWidth + 30.0f * scale;
    float availableWidth = ImGui::GetContentRegionAvail().x - indent;
    if (availableWidth < itemWidth)
        availableWidth = itemWidth;

    int columns = std::max(1, static_cast<int>(availableWidth / itemWidth));
    int rows = (static_cast<int>(items.size()) + columns - 1) / columns;

    drawList->AddText({currentX, currentY}, ImGui::GetColorU32(ImGuiCol_TextDisabled), "Key:");
    currentY += 18.0f * scale;

    int idx = 0;
    for (const auto& item : items) {
        int col = idx / rows;
        int row = idx % rows;

        float x = currentX + col * itemWidth;
        float y = currentY + row * 18.0f * scale;

        ImVec2 boxMin = {x, y};
        ImVec2 boxMax = {x + colorBoxSize, y + colorBoxSize};

        drawList->AddRectFilled(boxMin, boxMax, item.color);
        drawList->AddRect(boxMin, boxMax, IM_COL32(200, 200, 200, 100));

        drawList->AddText({x + colorBoxSize + 5.0f * scale, y - 1.0f * scale},
                          ImGui::GetColorU32(ImGuiCol_Text), item.label.c_str());
        idx++;
    }

    return currentY + rows * 18.0f * scale;
}

} // namespace adm::ui::utils
