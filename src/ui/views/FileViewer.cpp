#include "FileViewer.h"
#include <algorithm>
#include <cstdio>
#include "../../core/Logger.h"

namespace adm {
namespace ui {

FileViewer::FileViewer()
{
    m_hexState.BytesPerLine = 16;
    m_hexState.AddressChars = 6;
    m_hexState.LowercaseBytes = true;

    m_hexState.GetAddressNameCallback = [](ImGuiHexEditorState* state, int offset, char* buf,
                                           int size) -> bool {
        snprintf(buf, size, "%04x", offset);
        return true;
    };

    m_editor.SetReadOnly(true);
    m_editor.SetColorizerEnable(false);
    m_editor.SetShowWhitespaces(false);

    // Set a single color palette (bright text on dark background)
    auto palette = TextEditor::GetDarkPalette();
    palette[(int)TextEditor::PaletteIndex::Default] = 0xffe0e0e0; // Light Grey
    m_editor.SetPalette(palette);
}

void FileViewer::SetContent(const std::string& content)
{
    m_rawData.assign(content.begin(), content.end());
    m_isBinary = IsBinary(m_rawData);

    // Sanitize for TextEditor: the UTF8CharLength() function interprets high
    // bytes (0x80+) as multi-byte UTF-8 lead bytes, causing glyph index
    // overflows and out-of-bounds crashes on binary data.
    std::string sanitized = content;
    for (char& c : sanitized) {
        auto uc = static_cast<unsigned char>(c);
        if (uc == 0 || (uc < 32 && uc != '\n' && uc != '\r' && uc != '\t') || uc > 126) {
            c = '.';
        }
    }
    m_editor.SetText(sanitized);

    // Update hex state
    m_hexState.Bytes = m_rawData.data();
    m_hexState.MaxBytes = (int)m_rawData.size();
}

void FileViewer::SetContent(const std::vector<uint8_t>& data)
{
    m_rawData = data;
    m_isBinary = IsBinary(m_rawData);

    // Sanitize for TextEditor: replace non-ASCII and non-printable characters
    // with '.' to prevent out-of-bounds crashes. The TextEditor's UTF8CharLength()
    // interprets high bytes (0x80+) as multi-byte UTF-8 lead bytes, causing the
    // glyph index to overshoot the line vector.
    std::string textContent(data.begin(), data.end());
    for (char& c : textContent) {
        auto uc = static_cast<unsigned char>(c);
        if (uc == 0 || (uc < 32 && uc != '\n' && uc != '\r' && uc != '\t') || uc > 126) {
            c = '.';
        }
    }
    m_editor.SetText(textContent);

    m_hexState.Bytes = m_rawData.data();
    m_hexState.MaxBytes = (int)m_rawData.size();
    // Reset/Default formatting - handled in constructor now
}

void FileViewer::SetFileMetadata(uint32_t headerSector, uint32_t parentSector,
                                 const std::string& fileName)
{
    m_headerSector = headerSector;
    m_parentSector = parentSector;
    m_fileName = fileName;

    // Enable editing if we have valid metadata (and likely valid callback soon)
    m_editor.SetReadOnly(false);
}

void FileViewer::SetOnSaveCallback(
    std::function<uint32_t(uint32_t, uint32_t, const std::string&, const std::vector<uint8_t>&)>
        callback)
{
    m_onSave = callback;
}

bool FileViewer::IsBinary(const std::vector<uint8_t>& data)
{
    // Simple heuristic: if it contains null bytes or many non-printable chars, it's binary.
    // Allow some non-printable (newlines, tabs).
    if (data.empty())
        return false;

    size_t nonPrintable = 0;
    for (uint8_t b : data) {
        if (b == 0)
            return true; // Null byte usually means binary
        if ((b < 32 && b != '\n' && b != '\r' && b != '\t') || b > 126) {
            nonPrintable++;
        }
    }

    // If more than 30% is non-printable, treat as binary
    return (nonPrintable > (data.size() * 0.3));
}

void FileViewer::SetTitle(const std::string& title) { m_title = title; }

void FileViewer::SetCurrentFont(const std::string& fontName)
{
    for (size_t i = 0; i < m_fontNames.size(); ++i) {
        if (m_fontNames[i] == fontName) {
            m_currentFontIdx = static_cast<int>(i);
            return;
        }
    }
}

std::string FileViewer::GetCurrentFontName() const
{
    if (m_currentFontIdx >= 0 && static_cast<size_t>(m_currentFontIdx) < m_fontNames.size()) {
        return m_fontNames[m_currentFontIdx];
    }
    return "Default";
}

void FileViewer::AddFont(const std::string& name, ImFont* font)
{
    m_fontNames.push_back(name);
    m_fonts.push_back(font);
}

void FileViewer::ClearFonts()
{
    m_fontNames.clear();
    m_fonts.clear();
    m_currentFontIdx = 0;
}

void FileViewer::Open()
{
    m_isOpen = true;
    m_justOpened = true;
}

void FileViewer::Draw()
{
    if (!m_isOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar;

    // Check if modified (using CanUndo as proxy for "modified since load/save")
    if (!m_editor.IsReadOnly() && m_editor.CanUndo()) {
        windowFlags |= ImGuiWindowFlags_UnsavedDocument;
    }

    bool wasOpen = m_isOpen;
    // NoSavedSettings prevents this window from being saved to imgui.ini
    if (ImGui::Begin(m_title.c_str(), &m_isOpen, windowFlags)) {
        // Handle window close request via X button
        if (wasOpen && !m_isOpen) {
            if (!m_editor.IsReadOnly() && m_editor.CanUndo()) {
                m_isOpen = true; // Cancel close
                m_showUnsavedChangesDialog = true;
            }
        }

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save", "Ctrl+S", false, !m_editor.IsReadOnly() && m_onSave)) {
                    if (m_onSave) {
                        std::string text = m_editor.GetText();
                        // Convert string to vector<uint8_t> (simple cast usually fine for
                        // ASCII/UTF-8)
                        std::vector<uint8_t> data(text.begin(), text.end());

                        uint32_t newHeader =
                            m_onSave(m_headerSector, m_parentSector, m_fileName, data);
                        if (newHeader != 0) {
                            m_headerSector = newHeader;
                            // Reset dirty state by resetting text (clears undo buffer)
                            // Preserve cursor and scroll if possible?
                            auto cursor = m_editor.GetCursorPosition();
                            m_editor.SetText(text);
                            m_editor.SetCursorPosition(cursor);
                            // SetText forces scroll to top, so maybe we can't easily fix that
                            // without modifying TextEditor. But correctness > scroll position for
                            // now.
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Close", "Ctrl+W")) {
                    if (!m_editor.IsReadOnly() && m_editor.CanUndo()) {
                        m_showUnsavedChangesDialog = true;
                    } else {
                        m_isOpen = false;
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit")) {
                bool ro = m_editor.IsReadOnly();
                if (ImGui::MenuItem("Undo", "Ctrl+Z", nullptr, !ro && m_editor.CanUndo()))
                    m_editor.Undo();
                if (ImGui::MenuItem("Redo", "Ctrl+Y", nullptr, !ro && m_editor.CanRedo()))
                    m_editor.Redo();

                ImGui::Separator();

                if (ImGui::MenuItem("Cut", "Ctrl+X", nullptr, !ro && m_editor.HasSelection()))
                    m_editor.Cut();
                if (ImGui::MenuItem("Copy", "Ctrl+C", nullptr, m_editor.HasSelection()))
                    m_editor.Copy();
                if (ImGui::MenuItem("Paste", "Ctrl+V", nullptr,
                                    !ro && ImGui::GetClipboardText() != nullptr))
                    m_editor.Paste();
                if (ImGui::MenuItem("Delete", "Del", nullptr, !ro && m_editor.HasSelection()))
                    m_editor.Delete();

                ImGui::Separator();

                if (ImGui::MenuItem("Select All", "Ctrl+A", nullptr))
                    m_editor.SelectAll();

                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Handle Unsaved Changes Dialog
        if (m_showUnsavedChangesDialog) {
            ImGui::OpenPopup("Unsaved Changes");
            m_showUnsavedChangesDialog = false; // Reset trigger
        }

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("You have unsaved changes.");
            ImGui::Text("Do you want to save before closing?");
            ImGui::Separator();

            // Handle Escape key to close dialog
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ImGui::CloseCurrentPopup();
            }

            if (ImGui::Button("Save", ImVec2(120, 0))) {
                bool saveSuccess = false;
                if (m_onSave) {
                    std::string text = m_editor.GetText();
                    std::vector<uint8_t> data(text.begin(), text.end());

                    uint32_t newHeader = m_onSave(m_headerSector, m_parentSector, m_fileName, data);
                    if (newHeader != 0) {
                        m_headerSector = newHeader;
                        saveSuccess = true;
                    }
                }

                if (saveSuccess) {
                    m_isOpen = false; // Close window after save
                    ImGui::CloseCurrentPopup();
                } else {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save", ImVec2(120, 0))) {
                m_isOpen = false; // Close window without saving
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Auto-select tab on first open or content change if we tracked it,
        // but ImGuiTabBarFlags_None preserves selection.
        // We can force selection if we want with ImGuiTabBarFlags_Reorderable etc.
        // For now, let's just let user switch.
        // To auto-select, we'd need to set a flag "SetTabNextFrame".

        ImGuiTabBarFlags tabFlags = ImGuiTabBarFlags_None;
        if (ImGui::BeginTabBar("##viewertabs", tabFlags)) {
            // Determine default open tab logic could be here if using specific flags,
            // but standard ImGui tab bar remembers last selection per ID.
            // Since ID is likely constant ("##viewertabs"), it remembers last used.
            // To force default based on content type, we might need unique ID per file
            // implementation, OR handle it manually. Let's rely on user preference implicitly for
            // now, OR: If we want "smart" selection, we could use SetTabItemClosed to reset? No.

            // Actually, we can check `m_isBinary` and use `ImGuiTabItemFlags_SetSelected`
            // on the first frame it's opened. But `Draw` is called every frame.

            // Let's just render both.

            ImGuiTabItemFlags textFlags = ImGuiTabItemFlags_None;
            ImGuiTabItemFlags hexFlags = ImGuiTabItemFlags_None;

            if (m_justOpened) {
                if (m_isBinary) {
                    hexFlags |= ImGuiTabItemFlags_SetSelected;
                } else {
                    textFlags |= ImGuiTabItemFlags_SetSelected;
                }
                m_justOpened = false;
            }

            if (ImGui::BeginTabItem("Text", nullptr, textFlags)) {
                ImFont* font = (static_cast<size_t>(m_currentFontIdx) < m_fonts.size())
                                   ? m_fonts[m_currentFontIdx]
                                   : nullptr;
                if (font)
                    ImGui::PushFont(font);

                m_editor.Render("TextEditor");

                if (font)
                    ImGui::PopFont();

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Hex", nullptr, hexFlags)) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                ImGui::BeginHexEditor("##hexview", &m_hexState, avail);
                ImGui::EndHexEditor();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace ui
} // namespace adm
