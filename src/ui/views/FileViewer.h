#pragma once

#include <imgui.h>
#include <cstdint>
#include <string>
#include <vector>
#include "../../vendor/imgui_text_edit/TextEditor.h"
#include "imgui_hex.h"

namespace adm {
namespace ui {

/**
 * @brief File content viewer with font selection.
 *
 * Displays the content of Amiga files with customizable fonts.
 */
class FileViewer
{
public:
    FileViewer();

    /**
     * @brief Set the content to display.
     * @param content The file content as a string.
     */
    void SetContent(const std::string& content); // Keep for compatibility if needed, or redirect
    void SetContent(const std::vector<uint8_t>& data);

    /**
     * @brief Set file metadata for saving.
     * @param headerSector The file's header sector.
     * @param parentSector The file's parent directory sector.
     * @param fileName The file name.
     */
    void SetFileMetadata(uint32_t headerSector, uint32_t parentSector, const std::string& fileName);

    /**
     * @brief Set callback for saving file content.
     *Callback returns new header sector on success, or 0 on failure.
     */
    void SetOnSaveCallback(
        std::function<uint32_t(uint32_t, uint32_t, const std::string&, const std::vector<uint8_t>&)>
            callback);

    /**
     * @brief Set the window title.
     * @param title The title string.
     */
    void SetTitle(const std::string& title);

    /**
     * @brief Set the current font by name.
     * @param fontName The name of the font to use.
     */
    void SetCurrentFont(const std::string& fontName);

    /**
     * @brief Get the current font name.
     * @return The name of the currently selected font.
     */
    std::string GetCurrentFontName() const;

    /**
     * @brief Add a font to the available fonts.
     * @param name The display name for the font.
     * @param font The ImGui font pointer (nullptr for default).
     */
    void AddFont(const std::string& name, ImFont* font);

    /**
     * @brief Clear all registered fonts.
     */
    void ClearFonts();

    /**
     * @brief Open the viewer window.
     */
    void Open();

    /**
     * @brief Draw the viewer window.
     */
    /**
     * @brief Draw the viewer window.
     */
    void Draw();

    /**
     * @brief Check if the window is open.
     * @return true if open, false otherwise.
     */
    bool IsOpen() const { return m_isOpen; }

private:
    // Hex View state
    ImGuiHexEditorState m_hexState;
    bool m_isBinary = false;   // Auto-detected type
    bool m_justOpened = false; // Flag to trigger default tab selection

    // Text Editor
    TextEditor m_editor;

    // File Metadata
    uint32_t m_headerSector = 0;
    uint32_t m_parentSector = 0;
    std::string m_fileName;
    bool m_showUnsavedChangesDialog = false;

    // Save Callback: (headerSector, parentSector, fileName, data) -> success
    std::function<uint32_t(uint32_t, uint32_t, const std::string&, const std::vector<uint8_t>&)>
        m_onSave;

    // Legacy/Font support members
    std::string m_title = "File Viewer";
    std::vector<ImFont*> m_fonts;
    std::vector<std::string> m_fontNames;
    int m_currentFontIdx = 0;
    bool m_isOpen = false;
    std::vector<uint8_t> m_rawData;

    bool IsBinary(const std::vector<uint8_t>& data);
};

} // namespace ui
} // namespace adm
