#include "HunkViewer.h"
#include "../FontRoles.h"

#include <imgui.h>

#include <algorithm>

namespace adm {
namespace ui {

HunkViewer::HunkViewer() = default;
HunkViewer::~HunkViewer() = default;

void HunkViewer::SetData(const core::HunkParseResult& result, const std::string& filename)
{
    m_parseResult = result;
    m_filename = filename;
    m_selectedHunk = -1;
}

void HunkViewer::SetTitle(const std::string& title) { m_title = title; }

void HunkViewer::Open() { m_isOpen = true; }

void HunkViewer::Draw()
{
    if (!m_isOpen) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(550, 450), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(m_title.c_str(), &m_isOpen, ImGuiWindowFlags_NoSavedSettings)) {
        if (!m_parseResult.success) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Parse Error:");
            ImGui::TextWrapped("%s", m_parseResult.errorMessage.c_str());
        } else {
            PushFontRole(FontRole::Code);
            // Draw summary section
            DrawSummary();

            ImGui::Separator();

            // Split view: hunk list on left, details on right
            ImVec2 availSize = ImGui::GetContentRegionAvail();

            // Hunk list (left side)
            if (ImGui::BeginChild("##HunkList", ImVec2(availSize.x * 0.45f, 0), true)) {
                DrawHunkList();
            }
            ImGui::EndChild();

            ImGui::SameLine();

            // Hunk details (right side)
            if (ImGui::BeginChild("##HunkDetails", ImVec2(0, 0), true)) {
                if (m_selectedHunk >= 0 &&
                    m_selectedHunk < static_cast<int>(m_parseResult.hunks.size())) {
                    DrawHunkDetails(m_parseResult.hunks[m_selectedHunk]);
                } else {
                    ImGui::TextDisabled("Select a hunk to view details");
                }
            }
            ImGui::EndChild();
            PopFontRole();
        }
    }
    ImGui::End();
}

void HunkViewer::DrawSummary()
{
    // File type and name
    ImGui::Text("File: %s", m_filename.c_str());

    // Type badge with color
    ImGui::SameLine();
    ImVec4 typeColor;
    switch (m_parseResult.executableType) {
        case core::ExecutableType::Executable:
            typeColor = ImVec4(0.4f, 0.8f, 0.4f, 1.0f); // Green
            break;
        case core::ExecutableType::Library:
            typeColor = ImVec4(0.4f, 0.6f, 1.0f, 1.0f); // Blue
            break;
        case core::ExecutableType::Object:
            typeColor = ImVec4(1.0f, 0.8f, 0.4f, 1.0f); // Orange
            break;
        default:
            typeColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f); // Gray
    }
    ImGui::TextColored(typeColor, "[%s]", m_parseResult.executableTypeName.c_str());

    // Statistics in a compact table
    if (ImGui::BeginTable("##Stats", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextColumn();
        ImGui::Text("Hunks: %u", static_cast<unsigned>(m_parseResult.hunks.size()));

        ImGui::TableNextColumn();
        ImGui::Text("Code: %s", FormatSize(m_parseResult.totalCodeSize).c_str());

        ImGui::TableNextColumn();
        ImGui::Text("Data: %s", FormatSize(m_parseResult.totalDataSize).c_str());

        ImGui::TableNextColumn();
        ImGui::Text("BSS: %s", FormatSize(m_parseResult.totalBSSSize).c_str());

        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        // Memory requirements
        if (m_parseResult.hasChipMem || m_parseResult.hasFastMem) {
            std::string memReq;
            if (m_parseResult.hasChipMem)
                memReq += "CHIP ";
            if (m_parseResult.hasFastMem)
                memReq += "FAST";
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "Mem: %s", memReq.c_str());
        } else {
            ImGui::TextDisabled("Mem: ANY");
        }

        ImGui::TableNextColumn();
        ImGui::Text("Relocs: %u", m_parseResult.totalRelocations);

        ImGui::TableNextColumn();
        ImGui::Text("Symbols: %u", m_parseResult.totalSymbols);

        ImGui::EndTable();
    }
}

void HunkViewer::DrawHunkList()
{
    ImGui::Text("Hunks (%zu)", m_parseResult.hunks.size());
    ImGui::Separator();

    for (size_t i = 0; i < m_parseResult.hunks.size(); ++i) {
        const auto& hunk = m_parseResult.hunks[i];

        ImGui::PushID(static_cast<int>(i));

        // Color based on hunk type
        ImVec4 color;
        switch (hunk.type) {
            case core::HunkType::Code:
                color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f); // Green
                break;
            case core::HunkType::Data:
                color = ImVec4(0.4f, 0.8f, 1.0f, 1.0f); // Cyan
                break;
            case core::HunkType::BSS:
                color = ImVec4(1.0f, 0.8f, 0.4f, 1.0f); // Orange
                break;
            case core::HunkType::Reloc32:
            case core::HunkType::Reloc16:
            case core::HunkType::Reloc8:
                color = ImVec4(0.8f, 0.6f, 1.0f, 1.0f); // Purple
                break;
            case core::HunkType::Symbol:
            case core::HunkType::Debug:
                color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f); // Gray
                break;
            default:
                color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // White
        }

        // Selectable item
        bool isSelected = (m_selectedHunk == static_cast<int>(i));
        char label[64];
        snprintf(label, sizeof(label), "%2d: %s", hunk.index, hunk.typeName.c_str());

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        if (ImGui::Selectable(label, isSelected)) {
            m_selectedHunk = static_cast<int>(i);
        }
        ImGui::PopStyleColor();

        // Show size on same line if significant
        if (hunk.size > 0) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
            ImGui::TextDisabled("%s", FormatSize(hunk.size).c_str());
        }

        ImGui::PopID();
    }
}

void HunkViewer::DrawHunkDetails(const core::HunkInfo& hunk)
{
    // Header
    ImGui::Text("Hunk #%d: %s", hunk.index, hunk.typeName.c_str());
    ImGui::Separator();

    // Basic info
    if (ImGui::BeginTable("##HunkInfo", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Value");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Type:");
        ImGui::TableNextColumn();
        ImGui::Text("%s", hunk.typeName.c_str());

        if (hunk.size > 0) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Size:");
            ImGui::TableNextColumn();
            ImGui::Text("%s (%u bytes)", FormatSize(hunk.size).c_str(), hunk.size);
        }

        if (hunk.allocSize > 0 && hunk.allocSize != hunk.size) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Alloc Size:");
            ImGui::TableNextColumn();
            ImGui::Text("%s (%u bytes)", FormatSize(hunk.allocSize).c_str(), hunk.allocSize);
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("File Offset:");
        ImGui::TableNextColumn();
        ImGui::Text("0x%08X", hunk.dataOffset);

        if (hunk.memoryFlags != 0) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Memory:");
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s",
                               core::HunkParser::GetMemoryFlagsString(hunk.memoryFlags).c_str());
        }

        ImGui::EndTable();
    }

    // Symbols section
    if (!hunk.symbols.empty()) {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Symbols", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginChild("##Symbols", ImVec2(0, 100), true)) {
                for (const auto& sym : hunk.symbols) {
                    ImGui::BulletText("%s", sym.c_str());
                }
            }
            ImGui::EndChild();
        }
    }

    // Relocations section
    if (!hunk.relocations.empty()) {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Relocations")) {
            ImGui::Text("%zu relocation(s)", hunk.relocations.size());
            if (ImGui::BeginChild("##Relocs", ImVec2(0, 80), true)) {
                // Show first few relocations
                size_t showCount = std::min(hunk.relocations.size(), static_cast<size_t>(20));
                for (size_t i = 0; i < showCount; ++i) {
                    ImGui::Text("  0x%08X", hunk.relocations[i]);
                }
                if (hunk.relocations.size() > showCount) {
                    ImGui::TextDisabled("  ... and %zu more", hunk.relocations.size() - showCount);
                }
            }
            ImGui::EndChild();
        }
    }

    // Type-specific information
    ImGui::Spacing();
    switch (hunk.type) {
        case core::HunkType::Code:
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Executable code segment");
            break;
        case core::HunkType::Data:
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Initialized data segment");
            break;
        case core::HunkType::BSS:
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                               "Uninitialized data (zeroed at load)");
            break;
        case core::HunkType::Reloc32:
            ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "32-bit relocation table");
            break;
        case core::HunkType::Symbol:
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Symbol table for debugging");
            break;
        case core::HunkType::Debug:
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Debug information");
            break;
        default:
            break;
    }
}

std::string HunkViewer::FormatSize(uint32_t bytes)
{
    char buffer[32];
    if (bytes >= 1024 * 1024) {
        snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / (1024.0f * 1024.0f));
    } else if (bytes >= 1024) {
        snprintf(buffer, sizeof(buffer), "%.1f KB", bytes / 1024.0f);
    } else {
        snprintf(buffer, sizeof(buffer), "%u B", bytes);
    }
    return buffer;
}

} // namespace ui
} // namespace adm
