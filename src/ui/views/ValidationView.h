#pragma once

#include <functional>
#include <string>

#include "../../core/DiskValidator.h"

namespace adm {
namespace ui {

/**
 * @brief Window displaying disk validation results.
 */
class ValidationView
{
public:
    ValidationView();
    ~ValidationView();

    // Non-copyable
    ValidationView(const ValidationView&) = delete;
    ValidationView& operator=(const ValidationView&) = delete;

    /**
     * @brief Set the validation results to display.
     */
    void SetResults(const core::ValidationResult& result);

    /**
     * @brief Open the validation window.
     */
    void Open();

    /**
     * @brief Close the validation window.
     */
    void Close();

    /**
     * @brief Draw the validation window.
     */
    void Draw();

    /**
     * @brief Check if the window is open.
     */
    [[nodiscard]] bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    /**
     * @brief Set callback for when user clicks on a block number.
     */
    void SetOnBlockClicked(std::function<void(uint32_t)> callback) { m_onBlockClicked = callback; }

    /**
     * @brief Set callback for when user clicks the Validate button.
     */
    void SetOnValidate(std::function<void()> callback) { m_onValidate = callback; }

    /**
     * @brief Tell the window whether a mounted disk is available to validate. Drives the
     *        Validate button's enabled state; pushed every frame by the caller.
     */
    void SetDocumentLoaded(bool loaded) { m_documentLoaded = loaded; }

private:
    void DrawValidateButton();
    void DrawSummary();
    void DrawIssueList();
    void DrawIssueDetails(const core::ValidationIssue& issue);

    core::ValidationResult m_result;
    bool m_isOpen = false;
    bool m_documentLoaded = false;
    int m_selectedIssue = -1;

    // Filter state
    bool m_showSuccess = true;
    bool m_showInfo = true;
    bool m_showWarnings = true;
    bool m_showErrors = true;

    std::function<void(uint32_t)> m_onBlockClicked;
    std::function<void()> m_onValidate;
};

} // namespace ui
} // namespace adm
