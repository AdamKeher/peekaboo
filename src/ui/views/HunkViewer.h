#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../../core/HunkParser.h"

namespace adm {
namespace ui {

/**
 * @brief Viewer window for Amiga executable hunk structure.
 *
 * Displays detailed information about the hunk structure of Amiga
 * executables, libraries, and object files.
 */
class HunkViewer
{
public:
    HunkViewer();
    ~HunkViewer();

    // Non-copyable
    HunkViewer(const HunkViewer&) = delete;
    HunkViewer& operator=(const HunkViewer&) = delete;

    /**
     * @brief Set the parsed hunk data to display.
     * @param result The parse result from HunkParser.
     * @param filename Original filename for display.
     */
    void SetData(const core::HunkParseResult& result, const std::string& filename);

    /**
     * @brief Set the window title.
     * @param title The title string.
     */
    void SetTitle(const std::string& title);

    /**
     * @brief Open the viewer window.
     */
    void Open();

    /**
     * @brief Draw the viewer window.
     */
    void Draw();

    /**
     * @brief Check if the window is open.
     */
    [[nodiscard]] bool IsOpen() const { return m_isOpen; }

private:
    void DrawSummary();
    void DrawHunkList();
    void DrawHunkDetails(const core::HunkInfo& hunk);
    std::string FormatSize(uint32_t bytes);

    std::string m_title = "Hunk Viewer";
    std::string m_filename;
    bool m_isOpen = false;

    core::HunkParseResult m_parseResult;
    int m_selectedHunk = -1;
};

} // namespace ui
} // namespace adm
