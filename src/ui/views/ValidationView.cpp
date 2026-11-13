#include "ValidationView.h"

#include <imgui.h>

#include "../IconsFontAwesome6.h"

namespace adm {
namespace ui {

ValidationView::ValidationView() = default;
ValidationView::~ValidationView() = default;

void ValidationView::SetResults(const core::ValidationResult& result)
{
    m_result = result;
    m_selectedIssue = -1;
}

void ValidationView::Open() { m_isOpen = true; }

void ValidationView::Close() { m_isOpen = false; }

void ValidationView::Draw()
{
    if (!m_isOpen) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);

    // The "###" tail is the window's stable identity - saved dock layouts still name it by the
    // old title, so only the visible half was renamed.
    if (ImGui::Begin(ICON_FA_CHECK_DOUBLE " Disk Validator###Disk Validation Results", &m_isOpen)) {
        // Before a run the whole interface is drawn as usual and disabled, not replaced by an
        // explanation - the layout is what tells the user what this window does, and it must
        // not jump when results arrive. Only the Validate button stays live.
        DrawSummary();
        ImGui::Separator();
        ImGui::BeginDisabled(!m_result.completed);
        DrawIssueList();
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void ValidationView::DrawValidateButton()
{
    ImGui::BeginDisabled(!m_documentLoaded);
    if (ImGui::Button("Validate")) {
        if (m_onValidate) {
            m_onValidate();
        }
    }
    ImGui::EndDisabled();
    // A disabled item is not "hovered" by default, so the tooltip explaining why needs the
    // AllowWhenDisabled flag - without it the greyed button says nothing at all.
    if (!m_documentLoaded && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Load a disk image to validate it.");
    }
}

void ValidationView::DrawSummary()
{
    const bool hasResults = m_result.completed;

    // Summary header. The un-run state keeps the same two lines so the panel does not
    // reflow the moment a result lands.
    ImGui::BeginDisabled(!hasResults);
    if (!hasResults) {
        ImGui::TextUnformatted("Not validated yet");
        ImGui::TextUnformatted("0 passed, 0 error(s), 0 warning(s), 0 info");
    } else if (m_result.IsClean()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Disk is healthy!");
        ImGui::Text("%d checks passed, %d info", m_result.successCount, m_result.infoCount);
    } else if (m_result.HasErrors()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Disk has errors!");
        ImGui::Text("%d passed, %d error(s), %d warning(s), %d info", m_result.successCount,
                    m_result.errorCount, m_result.warningCount, m_result.infoCount);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Disk has warnings");
        ImGui::Text("%d passed, %d warning(s), %d info", m_result.successCount,
                    m_result.warningCount, m_result.infoCount);
    }

    ImGui::Spacing();

    // Filter checkboxes
    ImGui::Text("Show:");
    ImGui::SameLine();
    ImGui::Checkbox("OK", &m_showSuccess);
    ImGui::SameLine();
    ImGui::Checkbox("Errors", &m_showErrors);
    ImGui::SameLine();
    ImGui::Checkbox("Warnings", &m_showWarnings);
    ImGui::SameLine();
    ImGui::Checkbox("Info", &m_showInfo);
    ImGui::EndDisabled();

    ImGui::SameLine();
    float button_width =
        ImGui::CalcTextSize("Validate").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - button_width -
                         ImGui::GetStyle().WindowPadding.x);
    DrawValidateButton();
}

void ValidationView::DrawIssueList()
{
    ImVec2 availSize = ImGui::GetContentRegionAvail();

    // Issue list (left side)
    if (ImGui::BeginChild("##IssueList", ImVec2(availSize.x * 0.5f, 0), true)) {
        ImGui::Text("Issues (%zu)", m_result.issues.size());
        ImGui::Separator();

        int visibleIndex = 0;
        for (size_t i = 0; i < m_result.issues.size(); ++i) {
            const auto& issue = m_result.issues[i];

            // Apply filter
            bool show = false;
            switch (issue.severity) {
                case core::ValidationSeverity::Success:
                    show = m_showSuccess;
                    break;
                case core::ValidationSeverity::Error:
                    show = m_showErrors;
                    break;
                case core::ValidationSeverity::Warning:
                    show = m_showWarnings;
                    break;
                case core::ValidationSeverity::Info:
                    show = m_showInfo;
                    break;
            }

            if (!show) {
                continue;
            }

            ImGui::PushID(static_cast<int>(i));

            // Color based on severity
            ImVec4 color;
            const char* icon;
            switch (issue.severity) {
                case core::ValidationSeverity::Success:
                    color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
                    icon = "[OK]";
                    break;
                case core::ValidationSeverity::Error:
                    color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                    icon = "[E]";
                    break;
                case core::ValidationSeverity::Warning:
                    color = ImVec4(1.0f, 0.8f, 0.4f, 1.0f);
                    icon = "[W]";
                    break;
                case core::ValidationSeverity::Info:
                default:
                    color = ImVec4(0.6f, 0.8f, 1.0f, 1.0f);
                    icon = "[I]";
                    break;
            }

            bool isSelected = (m_selectedIssue == static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Text, color);

            char label[256];
            snprintf(label, sizeof(label), "%s %s", icon, issue.message.c_str());

            if (ImGui::Selectable(label, isSelected)) {
                m_selectedIssue = static_cast<int>(i);
            }

            ImGui::PopStyleColor();
            ImGui::PopID();

            visibleIndex++;
        }

        if (visibleIndex == 0) {
            ImGui::TextDisabled("No issues match the current filter.");
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Issue details (right side)
    if (ImGui::BeginChild("##IssueDetails", ImVec2(0, 0), true)) {
        if (m_selectedIssue >= 0 && m_selectedIssue < static_cast<int>(m_result.issues.size())) {
            DrawIssueDetails(m_result.issues[m_selectedIssue]);
        } else {
            ImGui::TextDisabled("Select an issue to view details");
        }
    }
    ImGui::EndChild();
}

void ValidationView::DrawIssueDetails(const core::ValidationIssue& issue)
{
    // Severity and category header
    ImVec4 color;
    switch (issue.severity) {
        case core::ValidationSeverity::Success:
            color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
            break;
        case core::ValidationSeverity::Error:
            color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
            break;
        case core::ValidationSeverity::Warning:
            color = ImVec4(1.0f, 0.8f, 0.4f, 1.0f);
            break;
        case core::ValidationSeverity::Info:
        default:
            color = ImVec4(0.6f, 0.8f, 1.0f, 1.0f);
            break;
    }

    ImGui::TextColored(color, "%s", core::DiskValidator::GetSeverityName(issue.severity).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", core::DiskValidator::GetCategoryName(issue.category).c_str());

    ImGui::Separator();

    // Message
    ImGui::Text("Issue:");
    ImGui::TextWrapped("%s", issue.message.c_str());

    ImGui::Spacing();

    // Block number (clickable).
    // blockNumber == 0 is used both for the real boot block (category BootBlock)
    // and as a "no specific block" sentinel for summary rows; only the former is
    // navigable, so allow block 0 through when the issue is about the boot block.
    bool hasBlock = issue.blockNumber > 0 || issue.category == core::ValidationCategory::BootBlock;
    if (hasBlock) {
        ImGui::Text("Block:");
        ImGui::SameLine();
        char blockLabel[32];
        snprintf(blockLabel, sizeof(blockLabel), "%u", issue.blockNumber);
        if (ImGui::SmallButton(blockLabel)) {
            if (m_onBlockClicked) {
                m_onBlockClicked(issue.blockNumber);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to navigate to this block in the Hex Editor");
        }
    }

    ImGui::Spacing();

    // Details
    if (!issue.details.empty()) {
        ImGui::Text("Details:");
        ImGui::TextWrapped("%s", issue.details.c_str());
    }

    ImGui::Spacing();

    // Repairable indicator
    if (issue.isRepairable) {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "This issue may be repairable");
        // Note: Repair functionality can be added later
    }
}

} // namespace ui
} // namespace adm
