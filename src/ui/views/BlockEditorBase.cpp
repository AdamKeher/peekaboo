#include "BlockEditorBase.h"
#include <imgui.h>
#include "../../core/DefinitionManager.h"
#include "../ColorManager.h"
#include "../FontRoles.h"

#include <algorithm>
#include <iomanip>
#include <iostream>

namespace adm::ui {

BlockEditorBase::BlockEditorBase() { InitHexState(); }

void BlockEditorBase::InitHexState()
{
    m_hexState.BytesPerLine = 16;
    m_hexState.AddressChars = 8;
    m_hexState.LowercaseBytes = true;
    m_hexState.ReadOnly = true;
    m_hexState.UserData = this;
}

void BlockEditorBase::SetData(const std::vector<uint8_t>& data)
{
    m_data = data;
    m_hexState.Bytes = m_data.data();
    m_hexState.MaxBytes = static_cast<int>(m_data.size());
}

void BlockEditorBase::AddToHistory(uint32_t block)
{
    // If not at end of history, truncate forward history
    if (m_historyIndex < (int)m_blockHistory.size() - 1) {
        m_blockHistory.resize(m_historyIndex + 1);
    }

    // Add new block if different from current end
    if (m_blockHistory.empty() || m_blockHistory.back() != block) {
        m_blockHistory.push_back(block);
        m_historyIndex = (int)m_blockHistory.size() - 1;
    }
}

void BlockEditorBase::NavigateToHistoryIndex(int index)
{
    if (index >= 0 && index < (int)m_blockHistory.size()) {
        m_historyIndex = index;
        GoToBlock(m_blockHistory[index]);
    }
}

void BlockEditorBase::GoToBlock(uint32_t block)
{
    m_currentBlock = block;
    // Default implementation just sets current block.
    // Derived classes should override to load data.
}

void BlockEditorBase::DrawToolbar()
{
    bool hasData = !m_data.empty();

    // History navigation state
    bool canGoFirst = m_historyIndex > 0;
    bool canGoPrev = m_historyIndex > 0;
    bool canGoNext = m_historyIndex < (int)m_blockHistory.size() - 1;
    bool canGoLast = m_historyIndex < (int)m_blockHistory.size() - 1;

    // History navigation: First | Prev | [Current Block] | Next | Last
    ImGui::BeginDisabled(!canGoFirst);
    if (ImGui::Button("\xef\x84\x80")) { // Angles Left
        NavigateToHistoryIndex(0);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("First (Block %u)", m_blockHistory.empty() ? 0 : m_blockHistory[0]);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!canGoPrev);
    if (ImGui::Button("\xef\x84\x84")) { // Angle Left
        NavigateToHistoryIndex(m_historyIndex - 1);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (canGoPrev) {
            ImGui::SetTooltip("Previous (Block %u)", m_blockHistory[m_historyIndex - 1]);
        } else {
            ImGui::SetTooltip("No previous block (History)");
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!hasData);
    if (ImGui::Button("\xef\x81\xa8")) { // Minus
        if (m_currentBlock > 0) {
            GoToBlock(m_currentBlock - 1);
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Text("Block #");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);

    // Use deferred input: only apply when Enter pressed or focus lost
    if (!m_blockInputActive) {
        m_blockInputValue = m_currentBlock;
    }

    ImGui::BeginDisabled(!hasData);
    ImGui::InputInt("##block_input", &m_blockInputValue, 0, 0);

    // Check for Enter key press while the input is active
    bool enterPressed = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter);
    if (enterPressed) {
        if (m_blockInputValue >= 0) {
            GoToBlock((uint32_t)m_blockInputValue);
        }
        m_blockInputActive = false;
    } else if (ImGui::IsItemActive()) {
        m_blockInputActive = true;
    } else if (m_blockInputActive && ImGui::IsItemDeactivatedAfterEdit()) {
        // Focus lost after editing
        if (m_blockInputValue >= 0) {
            GoToBlock((uint32_t)m_blockInputValue);
        }
        m_blockInputActive = false;
    } else if (!ImGui::IsItemActive()) {
        m_blockInputActive = false;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!hasData);
    if (ImGui::Button("\xef\x81\xa7")) { // Plus
        GoToBlock(m_currentBlock + 1);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!canGoNext);
    if (ImGui::Button("\xef\x84\x85")) { // Angle Right
        NavigateToHistoryIndex(m_historyIndex + 1);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (canGoNext) {
            ImGui::SetTooltip("Next (Block %u)", m_blockHistory[m_historyIndex + 1]);
        } else {
            ImGui::SetTooltip("No next block (History)");
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!canGoLast);
    if (ImGui::Button("\xef\x84\x81")) { // Angles Right
        NavigateToHistoryIndex((int)m_blockHistory.size() - 1);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Last (Block %u)", m_blockHistory.empty() ? 0 : m_blockHistory.back());
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!hasData);
    if (ImGui::Button("\xef\x80\x9e")) { // Rotate Right (Refresh)
        GoToBlock(m_currentBlock);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Refresh Current Block");
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    ImGui::BeginDisabled(!hasData);
    if (ImGui::Button("Boot")) {
        GoToBlock(0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Root")) {
        // m_rootBlock is set by Application with fallback logic
        uint32_t targetRoot = m_rootBlock > 0 ? m_rootBlock : 880;
        GoToBlock(targetRoot);
    }
    ImGui::EndDisabled();
}

void BlockEditorBase::SetBlockSize(uint32_t size) { m_blockSize = size; }

void BlockEditorBase::SetGeometry(uint32_t heads, uint32_t sectors)
{
    m_heads = heads;
    m_sectors = sectors;
}

void BlockEditorBase::SetDiskLayout(uint32_t totalBlocks, uint32_t rootBlock)
{
    m_totalBlocks = totalBlocks;
    m_rootBlock = rootBlock;
}

void BlockEditorBase::SetDefinitionManager(core::DefinitionManager* manager)
{
    m_definitionManager = manager;
}

void BlockEditorBase::SetHoverRange(int from, int to)
{
    m_hasHoverRange = true;
    m_hoverRangeFrom = from;
    m_hoverRangeTo = to;
}

void BlockEditorBase::SetBlockDefinition(const nlohmann::json& def, const std::string& defFile)
{
    m_blockDef = def;
    m_currentDefinitionFile = defFile;
    if (!m_blockDef.empty()) {
        ApplyHighlights();
    } else {
        ClearHighlights();
    }
}

void BlockEditorBase::ClearHighlights()
{
    m_hexState.HighlightRanges.clear();
    m_staticRangeCount = 0;
}

void BlockEditorBase::LoadDefinition(const std::string& path)
{
    if (m_definitionManager) {
        m_blockDef = m_definitionManager->GetDefinition(path);
    } else {
        m_blockDef = nlohmann::json();
    }

    if (!m_blockDef.empty()) {
        m_currentDefinitionFile = path;
        ApplyHighlights();
    } else {
        m_currentDefinitionFile = "";
        ClearHighlights();
    }
}

void BlockEditorBase::ApplyHighlights()
{
    m_hexState.HighlightRanges.clear();
    BuildHighlightRanges(m_blockDef, GetViewBaseOffset(), DrawsFieldBorders(),
                         m_hexState.HighlightRanges, m_fieldRanges);
    m_staticRangeCount = m_hexState.HighlightRanges.Size;
}

void BlockEditorBase::BuildHighlightRanges(const nlohmann::json& def, uint32_t baseOffset,
                                           bool border,
                                           ImVector<ImGuiHexEditorHighlightRange>& ranges,
                                           std::vector<FieldInfo>& fields)
{
    ranges.clear();
    fields.clear();

    if (!def.contains("fields"))
        return;

    uint32_t offset = 0;
    for (const auto& field : def["fields"]) {
        uint32_t totalSize = GetFieldSize(field);

        long long start = (long long)offset - baseOffset;
        long long end = (long long)offset + totalSize - 1 - baseOffset;

        int validStart = (int)std::max(0LL, start);
        int validEnd = (int)std::min(511LL, end);

        if (validStart <= validEnd) {
            ImGuiHexEditorHighlightRange range;
            range.From = validStart;
            range.To = validEnd;
            range.BorderColor = IM_COL32(255, 255, 255, 255);

            ImU32 bg = IM_COL32(0, 0, 0, 0);
            if (field.contains("color")) {
                const auto& colorObj = field["color"];
                if (colorObj.contains("background")) {
                    std::string bgName = colorObj["background"];
                    bg = ColorManager::Instance().GetColor(bgName);
                }
            }
            range.Color = bg;

            range.Flags = ImGuiHexEditorHighlightFlags_FullSized |
                          ImGuiHexEditorHighlightFlags_Ascii |
                          ImGuiHexEditorHighlightFlags_TextAutomaticContrast;
            if (border) {
                range.Flags |= ImGuiHexEditorHighlightFlags_Border |
                               ImGuiHexEditorHighlightFlags_OverrideBorderColor;
            }
            ranges.push_back(range);

            // Store field info for tooltip
            if (field.contains("name")) {
                FieldInfo info;
                info.from = validStart;
                info.to = validEnd;
                info.name = field["name"].get<std::string>();
                fields.push_back(info);
            }
        }

        offset += totalSize;
    }
}

void BlockEditorBase::ApplyHoverHighlight()
{
    // Remove any dynamic highlights (keep only static field highlights)
    while (m_hexState.HighlightRanges.Size > m_staticRangeCount) {
        m_hexState.HighlightRanges.pop_back();
    }

    if (m_hasHoverRange) {
        ImGuiHexEditorHighlightRange hoverRange;
        hoverRange.From = m_hoverRangeFrom;
        hoverRange.To = m_hoverRangeTo;
        hoverRange.Color = IM_COL32(0, 0, 0, 0);
        hoverRange.BorderColor = IM_COL32(255, 0, 0, 255);
        hoverRange.Flags = ImGuiHexEditorHighlightFlags_Border |
                           ImGuiHexEditorHighlightFlags_OverrideBorderColor |
                           ImGuiHexEditorHighlightFlags_Ascii;
        m_hexState.HighlightRanges.push_back(hoverRange);
    }

    m_hasHoverRange = false;
}

uint32_t BlockEditorBase::GetViewBaseOffset() const
{
    if (m_currentDefinitionFile == "definitions/bootblock.json" && m_currentBlock == 1) {
        return 0x200;
    }
    return 0;
}

uint32_t BlockEditorBase::GetFieldSize(const nlohmann::json& field)
{
    std::string type = field.value("type", "uchar");
    uint32_t count = field.value("size", 0);
    uint32_t elemSize = 1;

    if (type == "ulong" || type == "long") {
        elemSize = 4;
    } else if (type == "ushort" || type == "short") {
        elemSize = 2;
    } else if (type == "struct") {
        uint32_t structBytes = 0;
        if (field.contains("fields")) {
            for (const auto& child : field["fields"]) {
                structBytes += GetFieldSize(child);
            }
        }
        elemSize = structBytes;
    }

    return (count > 0 ? count : 1) * elemSize;
}

void BlockEditorBase::DrawHexEditor(const ImVec2& size, const char* editorId,
                                    ImGuiWindowFlags windowFlags)
{
    // Capture default text color before pushing mint green for addresses
    m_defaultTextColor = ImGui::GetColorU32(ImGuiCol_Text);

    // Push mint green for address text
    ImGui::PushStyleColor(ImGuiCol_Text, ColorManager::Instance().GetColor("MintGreen"));
    PushFontRole(FontRole::Memory);
    ImGui::BeginHexEditor(editorId, &m_hexState, size, 0, windowFlags);

    ImGui::EndHexEditor();
    PopFontRole();
    ImGui::PopStyleColor();

    DrawAddDialog();

    // Show tooltip for field under mouse cursor using HoveredByte from the hex editor
    int hoveredByte = m_hexState.HoveredByte;
    if (hoveredByte >= 0 && !m_fieldRanges.empty()) {
        for (const auto& field : m_fieldRanges) {
            if (hoveredByte >= field.from && hoveredByte <= field.to) {
                ImGui::SetTooltip("%s", field.name.c_str());
                break;
            }
        }
    }
}

void BlockEditorBase::SetBootBlockInfo(const core::BootBlockInfo& info) { m_bootBlockInfo = info; }

float BlockEditorBase::GetBottomHeight() const
{
    float rows = (ShowsBootBlock() && m_bootBlockInfo.identified) ? 2.0f : 1.0f;
    // FrameHeightWithSpacing accounts for button heights and vertical spacing
    return ImGui::GetFrameHeightWithSpacing() * rows + 4.0f;
}

void BlockEditorBase::DrawBootBlockInfo()
{
    // If the bootblock is on screen, show class info and CRC32 right-aligned
    if (ShowsBootBlock()) {
        const auto& info = m_bootBlockInfo;

        // Prepare text strings
        std::string classText = info.identified ? info.className : "Unknown Bootblock";
        std::string infoText;
        if (info.identified) {
            infoText = " | " + std::string(info.bootable ? "Bootable" : "Non-Bootable") + " | " +
                       info.kickstart + " | CRC32: " + info.crc32;
        } else if (!info.crc32.empty()) {
            infoText = " | CRC32: " + info.crc32;
        }

        // Calculate widths
        float classWidth = ImGui::CalcTextSize(classText.c_str()).x;
        float infoWidth = ImGui::CalcTextSize(infoText.c_str()).x;
        float addButtonWidth = 40.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;

        float totalWidth = classWidth + infoWidth;
        if (!info.identified) {
            totalWidth += addButtonWidth + spacing;
        }

        // Determine start position (Right Aligned)
        float windowWidth = ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX();
        float startX = windowWidth - totalWidth;

        // Draw elements from left to right starting at startX
        ImGui::SameLine(startX);

        // 1. Add Button (if unknown)
        if (!info.identified) {
            if (ImGui::Button("Add", ImVec2(0, 0))) {
                m_showAddDialog = true;
                m_newBootBlockInfo = info;
                // Initialize dialog fields
                m_newBootBlockInfo.name = "";
                m_newBootBlockInfo.classCode = "s"; // Default to Standard
                m_newBootBlockInfo.className = "Standard";
                m_newBootBlockInfo.notes = "";
                m_newBootBlockInfo.kickstart = "1.2+";
                m_newBootBlockInfo.bootable = true;
                m_selectedClassIndex = 0; // "s"
            }
            ImGui::SameLine();
        }

        // 2. Class Name
        if (info.identified) {
            ImGui::PushStyleColor(ImGuiCol_Text, core::BrainFile::GetClassColor(info.classCode));
        }
        ImGui::TextUnformatted(classText.c_str());
        if (info.identified) {
            ImGui::PopStyleColor();
        }

        // 3. Info Text
        if (!infoText.empty()) {
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(infoText.c_str());
        }

        // Line 2: Name (only if identified)
        if (info.identified && !info.name.empty()) {
            float nameWidth = ImGui::CalcTextSize(info.name.c_str()).x;
            ImGui::SetCursorPosX(windowWidth - nameWidth);
            ImGui::TextUnformatted(info.name.c_str());
        }
    }
}

void BlockEditorBase::DrawAddDialog()
{
    if (!m_showAddDialog)
        return;

    ImGui::OpenPopup("Add to Brainfile");

    // Center the modal
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Add to Brainfile", &m_showAddDialog,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Add current bootblock to database?");
        ImGui::Spacing();

        // CRC32 (ReadOnly)
        ImGui::Text("CRC32:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", m_newBootBlockInfo.crc32.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Name
        char nameBuf[128];
        strncpy(nameBuf, m_newBootBlockInfo.name.c_str(), sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = 0;
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
            m_newBootBlockInfo.name = nameBuf;
        }

        // Class
        // Map common classes to dropdown
        const char* classNames[] = {"Standard (s)", "Game (ga)",     "Intro (i)",  "Demo (ds)",
                                    "Virus (v)",    "Depacker (dp)", "Loader (bl)"};
        const char* classCodes[] = {"s", "ga", "i", "ds", "v", "dp", "bl"};

        if (ImGui::Combo("Class", &m_selectedClassIndex, classNames, IM_ARRAYSIZE(classNames))) {
            m_newBootBlockInfo.classCode = classCodes[m_selectedClassIndex];
            m_newBootBlockInfo.className =
                core::BrainFile::GetClassName(m_newBootBlockInfo.classCode);
        }

        // Kickstart
        char ksBuf[64];
        strncpy(ksBuf, m_newBootBlockInfo.kickstart.c_str(), sizeof(ksBuf));
        ksBuf[sizeof(ksBuf) - 1] = 0;
        if (ImGui::InputText("Kickstart", ksBuf, sizeof(ksBuf))) {
            m_newBootBlockInfo.kickstart = ksBuf;
        }

        // Notes
        char notesBuf[256];
        strncpy(notesBuf, m_newBootBlockInfo.notes.c_str(), sizeof(notesBuf));
        notesBuf[sizeof(notesBuf) - 1] = 0;
        if (ImGui::InputText("Notes", notesBuf, sizeof(notesBuf))) {
            m_newBootBlockInfo.notes = notesBuf;
        }

        // Bootable
        ImGui::Checkbox("Bootable", &m_newBootBlockInfo.bootable);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Save", ImVec2(120, 0))) {
            if (m_onSaveBootBlock) {
                m_onSaveBootBlock(m_newBootBlockInfo);
            }
            m_showAddDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_showAddDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace adm::ui
