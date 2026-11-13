#include "BlockDetailsView.h"
#include <fmt/core.h>
#include <algorithm>
#include "../ColorManager.h"
#include "../TableLayout.h"

namespace adm::ui {

void BlockDetailsView::Draw(const nlohmann::json& blockDef, const std::vector<uint8_t>& data,
                            uint32_t baseOffset, const core::BlockDetails& details)
{
    m_currentDef = &blockDef;
    m_currentData = &data;
    m_currentBaseOffset = baseOffset;
    m_currentDetails = &details;

    if (blockDef.empty()) {
        ImGui::TextDisabled("No structure definition for this block.");
        return;
    }

    static ImGuiTableFlags flags = ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
                                   kAdjustableColumns | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##structure_table", 8, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1); // Freeze header row
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide);
        ImGui::TableSetupColumn("Raw");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Start");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Color");
        ImGui::TableSetupColumn("Comment");
        ImGui::TableHeadersRow();

        std::string rootName = blockDef.value("name", "Structure");
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool open = ImGui::TreeNodeEx(rootName.c_str(), ImGuiTreeNodeFlags_DefaultOpen |
                                                            ImGuiTreeNodeFlags_SpanFullWidth);

        for (int i = 0; i < 7; ++i)
            ImGui::TableNextColumn();

        if (open) {
            uint32_t currentOffset = 0;
            if (blockDef.contains("fields")) {
                for (auto& field : blockDef["fields"]) {
                    DrawField(field, currentOffset);
                }
            }
            ImGui::TreePop();
        }
        ImGui::EndTable();
    }
}

void BlockDetailsView::ParseFormat(const std::string& formatStr, std::string& outFmt, int& outWidth)
{
    outFmt = "hex"; // Default
    outWidth = -1;

    size_t colonPos = formatStr.find(':');
    if (colonPos != std::string::npos) {
        outFmt = formatStr.substr(0, colonPos);
        std::string widthStr = formatStr.substr(colonPos + 1);
        try {
            outWidth = std::stoi(widthStr);
        } catch (...) {
            outWidth = -1;
        }
    } else {
        if (!formatStr.empty())
            outFmt = formatStr;
    }
}

void BlockDetailsView::DrawRawValue(const nlohmann::json& field, const std::string& type,
                                    uint32_t size, uint32_t offset)
{
    long long rawLocalOffset = (long long)offset - m_currentBaseOffset;

    if (rawLocalOffset >= 0 && (size_t)rawLocalOffset + size <= m_currentData->size()) {
        uint32_t localOffset = (uint32_t)rawLocalOffset;
        uint32_t val = 0;
        const auto& data = *m_currentData;

        if (type == "ulong" || type == "long") {
            val = (data[localOffset] << 24) | (data[localOffset + 1] << 16) |
                  (data[localOffset + 2] << 8) | data[localOffset + 3];
        } else if (type == "ushort" || type == "short") {
            val = (data[localOffset] << 8) | data[localOffset + 1];
        } else if (type == "uchar" || type == "char") {
            if (size == 1)
                val = data[localOffset];
        }

        std::string fmtType = "default";
        int fmtWidth = -1;
        if (field.contains("format")) {
            ParseFormat(field["format"], fmtType, fmtWidth);
        }

        if (type == "ulong" || type == "long") {
            if (fmtType == "hex" || (fmtType == "default" && type == "ulong")) {
                int width = (fmtWidth != -1) ? fmtWidth : 8;
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%0*X", width, val);
                ImGui::Text("%s", buf);
            } else {
                if (type == "long")
                    ImGui::Text("%d", static_cast<int32_t>(val));
                else
                    ImGui::Text("%u", val);
            }
        } else if (type == "ushort" || type == "short") {
            if (fmtType == "hex" ||
                fmtType == "default") { // Default for short/ushort isn't strictly hex/dec, depends
                                        // on context, but legacy was hex check
                if (fmtType == "hex" || field.value("format", "") == "hex") {
                    int width = (fmtWidth != -1) ? fmtWidth : 4;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "0x%0*X", width, val);
                    ImGui::Text("%s", buf);
                } else if (type == "short") {
                    ImGui::Text("%d", static_cast<int16_t>(val));
                } else {
                    ImGui::Text("%u", val);
                }
            } else {
                // Explicit non-hex format requested (e.g. decimal)
                if (type == "short")
                    ImGui::Text("%d", static_cast<int16_t>(val));
                else
                    ImGui::Text("%u", val);
            }
        } else if (type == "uchar" || type == "char") {
            if (size > 1) {
                // String handling
                std::string s;
                for (uint32_t i = 0; i < size; ++i) {
                    if (data[localOffset + i] >= 32 && data[localOffset + i] < 127)
                        s += (char)data[localOffset + i];
                    else
                        s += ".";
                }
                ImGui::Text("\"%s\"", s.c_str());
            } else {
                if (fmtType == "hex" || (fmtType == "default" && type == "uchar")) {
                    int width = (fmtWidth != -1) ? fmtWidth : 2;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "0x%0*X", width, data[localOffset]);
                    ImGui::Text("%s", buf);
                } else {
                    ImGui::Text("%d", data[localOffset]);
                }
            }
        } else {
            ImGui::Text("Unknown");
        }
    } else {
        ImGui::TextDisabled("OOB");
    }
}

void BlockDetailsView::DrawProcessedValue(const nlohmann::json& field, const std::string& type,
                                          uint32_t size, uint32_t offset)
{
    long long rawLocalOffset = (long long)offset - m_currentBaseOffset;

    if (rawLocalOffset >= 0 && (size_t)rawLocalOffset + size <= m_currentData->size()) {
        uint32_t localOffset = (uint32_t)rawLocalOffset;
        uint32_t val = 0;
        const auto& data = *m_currentData;

        if (type == "ulong" || type == "long") {
            val = (data[localOffset] << 24) | (data[localOffset + 1] << 16) |
                  (data[localOffset + 2] << 8) | data[localOffset + 3];
        } else if (type == "ushort" || type == "short") {
            val = (data[localOffset] << 8) | data[localOffset + 1];
        } else if (type == "uchar" || type == "char") {
            if (size == 1)
                val = data[localOffset];
        }

        if (field.value("blocktarget", "") == "true") {
            RenderBlockLink(val);
            return;
        }

        if (field.contains("lookup") && m_currentDef->contains("lookups")) {
            std::string lookupName = field["lookup"];
            if ((*m_currentDef)["lookups"].contains(lookupName)) {
                auto& lookupDef = (*m_currentDef)["lookups"][lookupName];
                std::string lookupType = lookupDef.value("type", "index");
                auto& values = lookupDef["values"];

                if (lookupType == "index") {
                    std::string valStr;
                    if (type == "long" || type == "short") {
                        int32_t signedVal = static_cast<int32_t>(val);
                        valStr = std::to_string(signedVal);
                    } else {
                        valStr = std::to_string(val);
                    }
                    if (values.contains(valStr)) {
                        ImGui::Text("%s", values[valStr].get<std::string>().c_str());
                        return;
                    }
                } else if (lookupType == "bitfield") {
                    std::string result = "";
                    for (auto& [bitStr, name] : values.items()) {
                        uint32_t bit = std::stoul(bitStr);
                        if (val & bit) {
                            if (!result.empty())
                                result += " | ";
                            result += name.get<std::string>();
                        }
                    }
                    if (!result.empty()) {
                        ImGui::Text("%s", result.c_str());
                        return;
                    }
                } else if (lookupType == "flag_string") {
                    std::string result = "";
                    for (const auto& entry : values) {
                        uint32_t mask = entry.value("mask", 0);
                        std::string ch = entry.value("char", "?");
                        bool inverted = entry.value("inverted", false);
                        bool ignoreIfUnset = entry.value("ignoreIfUnset", false);

                        bool isSet = (val & mask) != 0;
                        if ((inverted && !isSet) || (!inverted && isSet)) {
                            result += ch;
                        } else if (!ignoreIfUnset) {
                            result += "-";
                        }
                    }
                    ImGui::Text("%s", result.c_str());
                    return;
                }
            }
        }

        if (field.contains("checksumType") && m_currentDetails) {
            std::string checksumType = field["checksumType"];
            bool performCheck = (checksumType == "bootblock" && m_currentDetails->isBootBlock) ||
                                (checksumType == "standard");

            if (performCheck) {
                if (m_currentDetails->isChecksumValid) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[OK]");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[ERROR]");
                }
                return;
            }
        }
    } else {
        ImGui::TextDisabled("OOB");
    }
}

void BlockDetailsView::DrawColorPreview(const nlohmann::json& field)
{
    if (field.contains("color")) {
        auto colorObj = field["color"];
        ImU32 bg = IM_COL32(0, 0, 0, 0);
        ImU32 fg = IM_COL32(255, 255, 255, 255);

        if (colorObj.contains("background")) {
            std::string bgName = colorObj["background"];
            bg = ColorManager::Instance().GetColor(bgName);
        }
        if (colorObj.contains("foreground")) {
            std::string fgName = colorObj["foreground"];
            fg = ColorManager::Instance().GetColor(fgName);
        }

        if ((bg & 0xFF000000) != 0) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, bg);
        }
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(fg), "Text");
    }
}

void BlockDetailsView::RenderBlockLink(uint32_t block)
{
    if (block == 0) {
        ImGui::TextDisabled("None");
        return;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%u", block);

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(100, 180, 255, 255));
    ImGui::Text("%s", buf);
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::BeginTooltip();
        ImGui::Text("Click to go to block %u", block);
        ImGui::EndTooltip();
    }

    if (ImGui::IsItemClicked()) {
        if (m_onNavigate) {
            m_onNavigate(block);
        }
    }
}

void BlockDetailsView::DrawField(const nlohmann::json& field, uint32_t& currentOffset)
{
    ImGui::PushID(currentOffset);
    std::string name = field.value("name", "Unknown");
    std::string type = field.value("type", "uchar");
    std::string comment = field.value("comment", "");
    uint32_t count = field.value("size", 0);
    uint32_t elemSize = 1;

    bool isUnused = (name == "unused");
    if (isUnused) {
        ImGui::PushStyleColor(ImGuiCol_Text, ColorManager::Instance().GetColor("Unused"));
    }

    if (type == "ulong" || type == "long")
        elemSize = 4;
    else if (type == "ushort" || type == "short")
        elemSize = 2;

    uint32_t baseOff = m_currentBaseOffset;

    if (type == "struct") {
        // Calculate total size of struct using recursive helper
        uint32_t structSize = GetFieldSize(field);

        long long localStart = (long long)currentOffset - baseOff;
        long long localEnd = (long long)currentOffset + structSize - 1 - baseOff;
        int hoverFrom = (int)std::max(0LL, localStart);
        int hoverTo = (int)std::min(511LL, localEnd);

        ImGui::TableNextRow();
        float cellPaddingY = ImGui::GetStyle().CellPadding.y;
        float rowMinY = ImGui::GetCursorScreenPos().y - cellPaddingY;

        std::string treeLabel = name;
        if (field.contains("struct_type")) {
            std::string sType = field["struct_type"];
            // Display struct_type, but use name as ID to avoid collisions if multiple same
            // struct_types exist
            treeLabel = sType + "###" + name;
        }

        ImGui::TableNextColumn();
        bool open = ImGui::TreeNodeEx(treeLabel.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);

        ImGui::TableNextColumn();
        ImGui::Text("%s", name.c_str()); // Raw column: display Struct Name

        ImGui::TableNextColumn();
        // Value Column: Handle "date" struct type
        if (field.value("struct_type", "") == "date" && field.contains("fields")) {
            // We need to read days, mins, ticks.
            // Assuming they are the first 3 fields in that order as per definition:
            // days (long/ulong), mins (long/ulong), ticks (long/ulong)
            uint32_t days = 0, mins = 0, ticks = 0;

            // Read ahead without advancing currentOffset permanently
            uint32_t tempOffset = currentOffset;
            const auto& fields = field["fields"];

            auto readValue = [&](const nlohmann::json& f, uint32_t& val) {
                std::string fType = f.value("type", "long");
                uint32_t fSize = 1; // Assuming scalar for date parts
                if (fType == "ulong" || fType == "long")
                    fSize = 4;

                long long rawOff = (long long)tempOffset - m_currentBaseOffset;
                if (rawOff >= 0 && (size_t)rawOff + fSize <= m_currentData->size()) {
                    const auto& data = *m_currentData;
                    if (fSize == 4) {
                        val = (data[rawOff] << 24) | (data[rawOff + 1] << 16) |
                              (data[rawOff + 2] << 8) | data[rawOff + 3];
                    }
                }
                tempOffset += fSize;
            };

            if (fields.size() >= 3) {
                readValue(fields[0], days);  // days
                readValue(fields[1], mins);  // mins
                readValue(fields[2], ticks); // ticks

                ImGui::Text("%s", FormatDate(days, mins, ticks).c_str());
            } else {
                ImGui::TextDisabled("Invalid Date Struct");
            }
        } else {
            ImGui::TextDisabled("-");
        }

        ImGui::TableNextColumn();
        ImGui::Text("%u", currentOffset); // Start
        ImGui::TableNextColumn();
        ImGui::Text("%u", structSize); // Size
        ImGui::TableNextColumn();
        ImGui::Text("struct"); // Type
        ImGui::TableNextColumn();
        // Color preview for struct? Maybe not needed or use first field's color.
        DrawColorPreview(field);
        ImGui::TableNextColumn();
        ImGui::Text("%s", comment.c_str()); // Comment

        // Highlight
        float rowHeight = ImGui::GetTextLineHeight() + (cellPaddingY * 2.0f);
        float rowMaxY = rowMinY + rowHeight;
        ImVec2 mousePos = ImGui::GetMousePos();
        bool isRowHovered = mousePos.y >= rowMinY && mousePos.y < rowMaxY &&
                            ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (isRowHovered && hoverFrom <= hoverTo && m_onHover) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(255, 100, 100, 80));
            m_onHover(hoverFrom, hoverTo);
        }

        if (open) {
            if (field.contains("fields")) {
                for (const auto& child : field["fields"]) {
                    DrawField(child, currentOffset);
                }
            }
            ImGui::TreePop();
        } else {
            // If collapsed, we still need to advance offset
            currentOffset += structSize;
        }

    } else {
        // Standard Field / Array Logic
        uint32_t totalSize = (count > 0 ? count : 1) * elemSize;
        bool isArray = (count > 1 && type != "char" && type != "uchar");

        long long localStart = (long long)currentOffset - baseOff;
        long long localEnd = (long long)currentOffset + totalSize - 1 - baseOff;
        int hoverFrom = (int)std::max(0LL, localStart);
        int hoverTo = (int)std::min(511LL, localEnd);

        ImGui::TableNextRow();
        float cellPaddingY = ImGui::GetStyle().CellPadding.y;
        float rowMinY = ImGui::GetCursorScreenPos().y - cellPaddingY;
        float rowHeight = ImGui::GetTextLineHeight() + (cellPaddingY * 2.0f);

        ImGui::TableNextColumn();

        if (isArray) {
            bool fieldOpen = ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);

            ImGui::TableNextColumn();
            ImGui::TextDisabled("(Array)");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            ImGui::Text("%u", currentOffset);
            ImGui::TableNextColumn();
            ImGui::Text("%u", totalSize);
            ImGui::TableNextColumn();
            ImGui::Text("%s[%u]", type.c_str(), count);
            ImGui::TableNextColumn();
            DrawColorPreview(field);
            ImGui::TableNextColumn();
            ImGui::Text("%s", comment.c_str());

            // Highlight logic ...
            float rowMaxY = rowMinY + rowHeight;
            ImVec2 mousePos = ImGui::GetMousePos();
            bool isRowHovered =
                mousePos.y >= rowMinY && mousePos.y < rowMaxY &&
                ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            if (isRowHovered && hoverFrom <= hoverTo && m_onHover) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(255, 100, 100, 80));
                m_onHover(hoverFrom, hoverTo);
            }

            if (fieldOpen) {
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t itemOffset = currentOffset + (i * elemSize);
                    long long itemLocalStart = (long long)itemOffset - baseOff;
                    long long itemLocalEnd = (long long)itemOffset + elemSize - 1 - baseOff;
                    int itemHoverFrom = (int)std::max(0LL, itemLocalStart);
                    int itemHoverTo = (int)std::min(511LL, itemLocalEnd);

                    ImGui::TableNextRow();
                    float itemRowMinY = ImGui::GetCursorScreenPos().y - cellPaddingY;

                    ImGui::TableNextColumn();
                    ImGui::Indent();
                    ImGui::Text("[%u]", i);
                    ImGui::Unindent();

                    ImGui::TableNextColumn();
                    DrawRawValue(field, type, elemSize, itemOffset);
                    ImGui::TableNextColumn();
                    DrawProcessedValue(field, type, elemSize, itemOffset);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", itemOffset);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", elemSize);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", type.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("-"); // No comment for array items?

                    // Item Highlight
                    float itemRowMaxY = itemRowMinY + rowHeight;
                    ImVec2 itemMousePos = ImGui::GetMousePos();
                    bool isItemRowHovered =
                        itemMousePos.y >= itemRowMinY && itemMousePos.y < itemRowMaxY &&
                        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
                    if (isItemRowHovered && itemHoverFrom <= itemHoverTo && m_onHover) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                               IM_COL32(255, 100, 100, 80));
                        m_onHover(itemHoverFrom, itemHoverTo);
                    }
                }
                ImGui::TreePop();
            }
        } else {
            ImGui::Indent();
            ImGui::Text("%s", name.c_str());

            ImGui::TableNextColumn();
            DrawRawValue(field, type, totalSize, currentOffset);
            ImGui::TableNextColumn();
            DrawProcessedValue(field, type, totalSize, currentOffset);
            ImGui::TableNextColumn();
            ImGui::Text("%u", currentOffset);
            ImGui::TableNextColumn();
            ImGui::Text("%u", totalSize);
            ImGui::TableNextColumn();
            ImGui::Text("%s", type.c_str());
            ImGui::TableNextColumn();
            DrawColorPreview(field);
            ImGui::TableNextColumn();
            ImGui::Text("%s", comment.c_str());

            // Highlight
            float rowMaxY = rowMinY + rowHeight;
            ImVec2 mousePos = ImGui::GetMousePos();
            bool isRowHovered =
                mousePos.y >= rowMinY && mousePos.y < rowMaxY &&
                ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            if (isRowHovered && hoverFrom <= hoverTo && m_onHover) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(255, 100, 100, 80));
                m_onHover(hoverFrom, hoverTo);
            }
            ImGui::Unindent();
        }

        currentOffset += totalSize;
    }

    if (isUnused) {
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}

std::string BlockDetailsView::FormatDate(uint32_t days, uint32_t mins, uint32_t ticks)
{
    // Amiga Epoch: Jan 1, 1978
    std::tm timeinfo = {};
    timeinfo.tm_year = 78; // 1978
    timeinfo.tm_mon = 0;   // Jan
    timeinfo.tm_mday = 1;

    std::time_t timestamp = std::mktime(&timeinfo);

    // Add days
    timestamp += (std::time_t)days * 86400;

    // Add minutes
    timestamp += (std::time_t)mins * 60;

    // Ticks (1/50 sec) - add seconds part
    timestamp += (std::time_t)(ticks / 50);

    // Convert back to tm
    std::tm* result = std::localtime(&timestamp);
    if (!result)
        return "Invalid Date";

    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", result);
    return std::string(buffer);
}

uint32_t BlockDetailsView::GetFieldSize(const nlohmann::json& field)
{
    std::string type = field.value("type", "uchar");
    uint32_t count = field.value("size", 0);
    uint32_t elemSize = 1;

    if (type == "ulong" || type == "long") {
        elemSize = 4;
    } else if (type == "ushort" || type == "short") {
        elemSize = 2;
    } else if (type == "struct") {
        // Recursive size calculation for structs
        uint32_t structBytes = 0;
        if (field.contains("fields")) {
            for (const auto& child : field["fields"]) {
                structBytes += GetFieldSize(child);
            }
        }
        elemSize = structBytes;
    }

    // specific handling for array of structs vs single struct is implicit:
    // if type is struct, elemSize is the full struct size.
    // 'count' is typically 0 (missing) for single items, so we take max(1, count).
    return (count > 0 ? count : 1) * elemSize;
}

} // namespace adm::ui
