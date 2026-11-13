#pragma once

#include <imgui.h>
#include <functional>
#include <string>
#include <vector>

#include "../../core/ArConsoleTypes.h"

namespace adm::ui {

// Character-grid terminal with free cursor movement and type-over editing,
// modeled on the Amiga Action Replay console. The whole scrollback is
// editable; Return hands the line under the cursor to the owner.
class ARConsoleTerminal
{
public:
    static constexpr int kCols = 160;
    static constexpr int kMaxScrollback = 5000;

    ARConsoleTerminal();

    // Output goes to the write position: lines overwrite existing buffer
    // content (AR screen semantics) and append once past the end.
    void PutLine(const core::ArLine& line);
    void PutLine(const std::string& text);
    void SetWritePos(int row);
    void SetWritePosToEnd();
    int WritePos() const; // effective row the next PutLine targets
    // Replaces one line in place without moving the write position.
    void WriteLineAt(int row, const core::ArLine& line);
    void Clear();

    int LineCount() const { return static_cast<int>(m_lines.size()); }
    // Line text with trailing whitespace trimmed (spans ignored).
    std::string GetLineText(int row) const;
    // Raw line text — trailing spaces preserved (typed-over trailing spaces in
    // memory-dump ASCII tails are significant).
    std::string GetLineTextRaw(int row) const;

    int CursorRow() const { return m_cursorRow; }
    int CursorCol() const { return m_cursorCol; }
    void SetCursor(int row, int col);
    // Moves the cursor to a fresh blank line at the bottom. By default an
    // already-blank last line is reused; forceNew always appends one.
    void MoveCursorToNewLine(bool forceNew = false);
    // Clears the given row (creating it if needed) and puts the cursor there.
    void CursorToClearedLine(int row);
    // Places the cursor on the last line at the given column.
    void SetCursorToLastLine(int col);

    bool IsFocused() const { return m_focused; }
    // Type-over (false, default) vs insert mode; toggled with the Insert key.
    bool IsInsertMode() const { return m_insertMode; }
    void ToggleInsertMode() { m_insertMode = !m_insertMode; }

    // Mouse drag-selection + system clipboard
    bool HasSelection() const { return m_hasSelection; }
    void ClearSelection() { m_hasSelection = false; }
    void SelectAll();
    std::string GetSelectedText() const;
    void CopySelectionToClipboard();
    // Types clipboard text at the cursor; newlines move to the next line
    // without executing anything.
    void PasteFromClipboard();

    // Drawn in the Console font role (Preferences > Appearance).
    // parentFocused: the caller should pass IsWindowFocused(ChildWindows) from the
    // parent window context before entering BeginChild, so focus is correctly detected
    // when the window is docked or the child has NoNavFocus-like interaction quirks.
    void Draw(bool parentFocused = false);

    std::function<void(int row, const std::string& line)> onReturn;
    std::function<void()> onEscape;
    // Fired when an address-colored hex token is clicked (memory hyperlink).
    std::function<void(uint32_t addr)> onAddressClicked;
    // Fired when an address hyperlink is Cmd/Alt+clicked or right-clicked
    // (request the owner to open an actions context menu for this address).
    std::function<void(uint32_t addr)> onAddressContextMenu;
    // Fired when a register-colored span is clicked (custom register hyperlink).
    std::function<void(uint32_t offset)> onCustomRegClicked;

private:
    struct TermLine
    {
        std::string text;
        std::vector<core::ArSpan> spans;
        // Draw sorts spans by column lazily; every mutation of `spans` must reset this.
        bool spansSorted = false;
    };

    void EnsureCursorLine();
    void TrimScrollback();
    void TypeChar(char c);
    void EraseSpanAt(TermLine& line, int col);
    void ClampCursor();
    void HandleKeyboard(int visibleRows);

    // Word boundaries on the current row's text (word char = alphanumeric).
    int PrevWordCol() const;
    int NextWordCol() const;
    // Line/word editing (clear the row's spans + scroll to cursor like TypeChar).
    void KillToLineEnd();
    void KillToLineStart();
    void DeleteWordBack();
    void DeleteWordForward();
    void DeleteForwardChar();
    // Selected column span on a row (inclusive); false if the row is outside
    // the selection.
    bool SelectionRange(int row, int& colStart, int& colEnd) const;

    std::vector<TermLine> m_lines;
    int m_cursorRow = 0;
    int m_cursorCol = 0;
    bool m_insertMode = false; // AR default: overwrite
    bool m_focused = false;
    bool m_prevFocused = false;   // focus state saved at end of previous frame
    bool m_grantingFocus = false; // click is granting focus (don't move cursor)
    int m_writeRow = -1;          // -1 = append at end

    // Drag-selection (anchor = press cell, end = current drag cell; inclusive)
    bool m_hasSelection = false;
    bool m_dragging = false;
    int m_selAnchorRow = 0;
    int m_selAnchorCol = 0;
    int m_selEndRow = 0;
    int m_selEndCol = 0;
    // Counts down over frames: newly appended lines only enter ImGui's scroll
    // range on the next layout pass, so a single-frame scroll gets clamped short.
    int m_scrollToCursorFrames = 0;
};

} // namespace adm::ui
