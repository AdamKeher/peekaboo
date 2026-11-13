#include "ARConsoleTerminal.h"

#include "../ArColorPalette.h"
#include "../FontRoles.h"

#include <imgui_internal.h> // ImGui::SetKeyOwner

#include <algorithm>
#include <cctype>

namespace adm::ui {

namespace {

// Maps semantic colors onto theme-derived values (shared with the disassembly window).
ImU32 SpanColor(core::ArColor color) { return ResolveArColor(color); }

// Opaque ImU32 for a 0xRRGGBB swatch payload.
ImU32 SwatchColor(uint32_t rgb)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
}

constexpr ImGuiKey kOwnedKeys[] = {
    ImGuiKey_UpArrow,
    ImGuiKey_DownArrow,
    ImGuiKey_LeftArrow,
    ImGuiKey_RightArrow,
    ImGuiKey_Enter,
    ImGuiKey_KeypadEnter,
    ImGuiKey_Escape,
    ImGuiKey_Tab,
    ImGuiKey_Home,
    ImGuiKey_End,
    ImGuiKey_PageUp,
    ImGuiKey_PageDown,
    ImGuiKey_Backspace,
    ImGuiKey_Delete,
    ImGuiKey_Insert,
    // Letters used by the emacs/Option editing layers (claimed so they don't
    // trigger ImGui's global nav shortcuts while the console is focused).
    ImGuiKey_A,
    ImGuiKey_B,
    ImGuiKey_D,
    ImGuiKey_E,
    ImGuiKey_F,
    ImGuiKey_H,
    ImGuiKey_K,
    ImGuiKey_N,
    ImGuiKey_P,
    ImGuiKey_U,
    ImGuiKey_W,
};

} // namespace

ARConsoleTerminal::ARConsoleTerminal() { m_lines.push_back({}); }

void ARConsoleTerminal::PutLine(const core::ArLine& line)
{
    TermLine tl;
    tl.text = line.text.substr(0, kCols);
    tl.spans = line.spans;

    const int row = WritePos();
    if (row >= static_cast<int>(m_lines.size())) {
        m_lines.push_back(std::move(tl));
        m_writeRow = static_cast<int>(m_lines.size());
    } else {
        m_lines[static_cast<size_t>(row)] = std::move(tl);
        m_writeRow = row + 1;
    }
    TrimScrollback();
    m_scrollToCursorFrames = 2;
}

void ARConsoleTerminal::PutLine(const std::string& text) { PutLine(core::ArLine(text)); }

void ARConsoleTerminal::SetWritePos(int row)
{
    m_writeRow = std::clamp(row, 0, static_cast<int>(m_lines.size()));
}

void ARConsoleTerminal::SetWritePosToEnd() { m_writeRow = -1; }

int ARConsoleTerminal::WritePos() const
{
    if (m_writeRow < 0)
        return static_cast<int>(m_lines.size());
    return std::min(m_writeRow, static_cast<int>(m_lines.size()));
}

void ARConsoleTerminal::WriteLineAt(int row, const core::ArLine& line)
{
    if (row < 0)
        return;
    while (row >= static_cast<int>(m_lines.size()))
        m_lines.push_back({});
    TermLine tl;
    tl.text = line.text.substr(0, kCols);
    tl.spans = line.spans;
    m_lines[static_cast<size_t>(row)] = std::move(tl);
}

void ARConsoleTerminal::Clear()
{
    m_lines.clear();
    m_lines.push_back({});
    m_cursorRow = 0;
    m_cursorCol = 0;
    m_writeRow = -1;
    m_hasSelection = false;
    m_scrollToCursorFrames = 2;
}

std::string ARConsoleTerminal::GetLineText(int row) const
{
    std::string text = GetLineTextRaw(row);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    return text;
}

std::string ARConsoleTerminal::GetLineTextRaw(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_lines.size()))
        return {};
    return m_lines[static_cast<size_t>(row)].text;
}

void ARConsoleTerminal::SetCursor(int row, int col)
{
    m_cursorRow = row;
    m_cursorCol = col;
    ClampCursor();
    m_scrollToCursorFrames = 2;
}

void ARConsoleTerminal::MoveCursorToNewLine(bool forceNew)
{
    // Reuse the last line if it is already blank (unless forced).
    if (forceNew || m_lines.empty() || !GetLineText(static_cast<int>(m_lines.size()) - 1).empty())
        m_lines.push_back({});
    TrimScrollback();
    m_cursorRow = static_cast<int>(m_lines.size()) - 1;
    m_cursorCol = 0;
    m_scrollToCursorFrames = 2;
}

void ARConsoleTerminal::SetCursorToLastLine(int col)
{
    m_cursorRow = static_cast<int>(m_lines.size()) - 1;
    m_cursorCol = col;
    ClampCursor();
    m_scrollToCursorFrames = 2;
}

void ARConsoleTerminal::CursorToClearedLine(int row)
{
    if (row < 0)
        row = 0;
    while (row >= static_cast<int>(m_lines.size()))
        m_lines.push_back({});
    m_lines[static_cast<size_t>(row)] = {};
    TrimScrollback();
    SetCursor(row, 0);
}

void ARConsoleTerminal::SelectAll()
{
    if (m_lines.empty())
        return;
    m_selAnchorRow = 0;
    m_selAnchorCol = 0;
    m_selEndRow = static_cast<int>(m_lines.size()) - 1;
    m_selEndCol = kCols - 1;
    m_hasSelection = true;
}

bool ARConsoleTerminal::SelectionRange(int row, int& colStart, int& colEnd) const
{
    if (!m_hasSelection)
        return false;
    int r0 = m_selAnchorRow, c0 = m_selAnchorCol;
    int r1 = m_selEndRow, c1 = m_selEndCol;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }
    if (row < r0 || row > r1)
        return false;
    colStart = (row == r0) ? c0 : 0;
    colEnd = (row == r1) ? c1 : kCols - 1;
    return true;
}

std::string ARConsoleTerminal::GetSelectedText() const
{
    std::string out;
    if (!m_hasSelection)
        return out;
    const int first = std::min(m_selAnchorRow, m_selEndRow);
    const int last = std::max(m_selAnchorRow, m_selEndRow);
    for (int row = first; row <= last; ++row) {
        int a = 0, b = 0;
        if (!SelectionRange(row, a, b))
            continue;
        const std::string text = GetLineTextRaw(row);
        std::string piece;
        if (a < static_cast<int>(text.size())) {
            const int end = std::min(b, static_cast<int>(text.size()) - 1);
            piece = text.substr(static_cast<size_t>(a), static_cast<size_t>(end - a + 1));
        }
        while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t'))
            piece.pop_back();
        out += piece;
        if (row != last)
            out += '\n';
    }
    return out;
}

void ARConsoleTerminal::CopySelectionToClipboard()
{
    if (!m_hasSelection)
        return;
    ImGui::SetClipboardText(GetSelectedText().c_str());
}

void ARConsoleTerminal::PasteFromClipboard()
{
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip)
        return;
    ClearSelection();
    for (const char* p = clip; *p; ++p) {
        const char c = *p;
        if (c == '\r')
            continue;
        if (c == '\n') {
            ++m_cursorRow;
            m_cursorCol = 0;
            EnsureCursorLine();
        } else if (c == '\t') {
            m_cursorCol = std::min(kCols - 1, ((m_cursorCol / 8) + 1) * 8);
        } else if (c >= 32 && static_cast<unsigned char>(c) < 127) {
            TypeChar(c);
        }
    }
    TrimScrollback();
    m_scrollToCursorFrames = 2;
}

void ARConsoleTerminal::EnsureCursorLine()
{
    while (m_cursorRow >= static_cast<int>(m_lines.size()))
        m_lines.push_back({});
}

void ARConsoleTerminal::TrimScrollback()
{
    if (static_cast<int>(m_lines.size()) <= kMaxScrollback)
        return;
    const int drop = static_cast<int>(m_lines.size()) - kMaxScrollback;
    m_lines.erase(m_lines.begin(), m_lines.begin() + drop);
    m_cursorRow = std::max(0, m_cursorRow - drop);
    if (m_writeRow > 0)
        m_writeRow = std::max(0, m_writeRow - drop);
    if (m_hasSelection) {
        m_selAnchorRow -= drop;
        m_selEndRow -= drop;
        if (m_selAnchorRow < 0 && m_selEndRow < 0) {
            m_hasSelection = false;
        } else {
            m_selAnchorRow = std::max(0, m_selAnchorRow);
            m_selEndRow = std::max(0, m_selEndRow);
        }
    }
}

void ARConsoleTerminal::ClampCursor()
{
    m_cursorRow = std::clamp(m_cursorRow, 0, std::max(0, static_cast<int>(m_lines.size()) - 1));
    m_cursorCol = std::clamp(m_cursorCol, 0, kCols - 1);
}

void ARConsoleTerminal::EraseSpanAt(TermLine& line, int col)
{
    // Split/trim any span covering col so the edited char renders in Default.
    std::vector<core::ArSpan> result;
    result.reserve(line.spans.size() + 1);
    for (const auto& s : line.spans) {
        if (col < s.col || col >= s.col + s.len) {
            result.push_back(s);
            continue;
        }
        if (col > s.col)
            result.push_back({s.col, col - s.col, s.color});
        if (col + 1 < s.col + s.len)
            result.push_back({col + 1, s.col + s.len - col - 1, s.color});
    }
    line.spans = std::move(result);
    line.spansSorted = false;
}

void ARConsoleTerminal::TypeChar(char c)
{
    EnsureCursorLine();
    TermLine& line = m_lines[static_cast<size_t>(m_cursorRow)];
    if (static_cast<int>(line.text.size()) < m_cursorCol)
        line.text.resize(static_cast<size_t>(m_cursorCol), ' ');

    if (m_insertMode) {
        line.text.insert(line.text.begin() + m_cursorCol, c);
        if (static_cast<int>(line.text.size()) > kCols)
            line.text.resize(kCols);
        line.spans.clear(); // spans are stale after a shift
    } else {
        if (m_cursorCol < static_cast<int>(line.text.size()))
            line.text[static_cast<size_t>(m_cursorCol)] = c;
        else
            line.text.push_back(c);
        EraseSpanAt(line, m_cursorCol);
    }

    if (m_cursorCol < kCols - 1)
        ++m_cursorCol;
    m_scrollToCursorFrames = 2;
}

int ARConsoleTerminal::PrevWordCol() const
{
    const std::string text = GetLineTextRaw(m_cursorRow);
    auto isWord = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    int col = std::min(m_cursorCol, static_cast<int>(text.size()));
    while (col > 0 && !isWord(text[static_cast<size_t>(col - 1)]))
        --col;
    while (col > 0 && isWord(text[static_cast<size_t>(col - 1)]))
        --col;
    return col;
}

int ARConsoleTerminal::NextWordCol() const
{
    const std::string text = GetLineTextRaw(m_cursorRow);
    const int len = static_cast<int>(text.size());
    auto isWord = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    int col = std::min(m_cursorCol, len);
    while (col < len && isWord(text[static_cast<size_t>(col)]))
        ++col;
    while (col < len && !isWord(text[static_cast<size_t>(col)]))
        ++col;
    return std::min(col, kCols - 1);
}

void ARConsoleTerminal::KillToLineEnd()
{
    EnsureCursorLine();
    TermLine& line = m_lines[static_cast<size_t>(m_cursorRow)];
    if (m_cursorCol < static_cast<int>(line.text.size())) {
        line.text.resize(static_cast<size_t>(m_cursorCol));
        line.spans.clear();
        m_scrollToCursorFrames = 2;
    }
}

void ARConsoleTerminal::KillToLineStart()
{
    EnsureCursorLine();
    TermLine& line = m_lines[static_cast<size_t>(m_cursorRow)];
    const int cut = std::min(m_cursorCol, static_cast<int>(line.text.size()));
    if (cut > 0) {
        line.text.erase(line.text.begin(), line.text.begin() + cut);
        line.spans.clear();
    }
    m_cursorCol = 0;
    m_scrollToCursorFrames = 2;
}

void ARConsoleTerminal::DeleteWordBack()
{
    EnsureCursorLine();
    const int target = PrevWordCol();
    if (target >= m_cursorCol)
        return;
    TermLine& line = m_lines[static_cast<size_t>(m_cursorRow)];
    const int end = std::min(m_cursorCol, static_cast<int>(line.text.size()));
    if (target < end) {
        line.text.erase(line.text.begin() + target, line.text.begin() + end);
        line.spans.clear();
    }
    m_cursorCol = target;
    m_scrollToCursorFrames = 2;
}

void ARConsoleTerminal::DeleteWordForward()
{
    EnsureCursorLine();
    const int target = NextWordCol();
    TermLine& line = m_lines[static_cast<size_t>(m_cursorRow)];
    const int start = std::min(m_cursorCol, static_cast<int>(line.text.size()));
    const int end = std::min(target, static_cast<int>(line.text.size()));
    if (start < end) {
        line.text.erase(line.text.begin() + start, line.text.begin() + end);
        line.spans.clear();
        m_scrollToCursorFrames = 2;
    }
}

void ARConsoleTerminal::DeleteForwardChar()
{
    EnsureCursorLine();
    TermLine& line = m_lines[static_cast<size_t>(m_cursorRow)];
    if (m_cursorCol < static_cast<int>(line.text.size())) {
        line.text.erase(line.text.begin() + m_cursorCol);
        line.spans.clear();
        m_scrollToCursorFrames = 2;
    }
}

void ARConsoleTerminal::HandleKeyboard(int visibleRows)
{
    ImGuiIO& io = ImGui::GetIO();

    // Platform-native modifier layout. On macOS Cmd owns the clipboard, leaving
    // Ctrl free for an emacs/readline editing layer; on Windows/Linux Ctrl owns
    // the clipboard and word ops use Ctrl+Arrow. AltGr reports Ctrl+Alt, so a
    // held Alt with Ctrl means the user is typing a character, not a shortcut.
    const bool mac = io.ConfigMacOSXBehaviors;
    const bool ctrl = io.KeyCtrl && !io.KeyAlt;      // bare Ctrl (no AltGr)
    const bool alt = io.KeyAlt && !io.KeyCtrl;       // Option on macOS
    const bool clipboard = mac ? io.KeySuper : ctrl; // Cmd on mac, Ctrl elsewhere
    const bool emacs = mac && ctrl && !io.KeySuper;  // mac-only emacs layer

    if (clipboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false))
            CopySelectionToClipboard();
        if (ImGui::IsKeyPressed(ImGuiKey_V, false))
            PasteFromClipboard();
        if (ImGui::IsKeyPressed(ImGuiKey_A, false))
            SelectAll();
    }

    const int lastRow = static_cast<int>(m_lines.size()) - 1;

    // macOS emacs/readline editing layer (Ctrl + letter).
    if (emacs) {
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            m_cursorCol = 0;
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            m_cursorCol = std::min(kCols - 1, static_cast<int>(GetLineText(m_cursorRow).size()));
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_B, false)) {
            m_cursorCol = std::max(0, m_cursorCol - 1);
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            m_cursorCol = std::min(kCols - 1, m_cursorCol + 1);
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_P)) {
            m_cursorRow = std::max(0, m_cursorRow - 1);
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_N)) {
            m_cursorRow = std::min(lastRow, m_cursorRow + 1);
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D))
            DeleteForwardChar();
        if (ImGui::IsKeyPressed(ImGuiKey_H)) {
            m_cursorCol = std::max(0, m_cursorCol - 1);
            DeleteForwardChar();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_K, false))
            KillToLineEnd();
        if (ImGui::IsKeyPressed(ImGuiKey_U, false))
            KillToLineStart();
        if (ImGui::IsKeyPressed(ImGuiKey_W, false))
            DeleteWordBack();
    }
    // macOS Option layer: word movement (Option+B/F and Option+Arrow both jump
    // to the first character of the previous/next word) + word delete.
    if (mac && alt) {
        if (ImGui::IsKeyPressed(ImGuiKey_B) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            m_cursorCol = PrevWordCol();
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            m_cursorCol = NextWordCol();
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D))
            DeleteWordForward();
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace))
            DeleteWordBack();
    }
    // macOS Cmd layer: line/buffer extents.
    if (mac && io.KeySuper) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            m_cursorCol = 0;
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            m_cursorCol = std::min(kCols - 1, static_cast<int>(GetLineText(m_cursorRow).size()));
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            m_cursorRow = 0;
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            m_cursorRow = lastRow;
            m_scrollToCursorFrames = 2;
        }
    }
    // Windows/Linux: Ctrl+Arrow word movement, Ctrl+Home/End buffer extents,
    // Ctrl+Backspace/Delete word delete.
    if (!mac && ctrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            m_cursorCol = PrevWordCol();
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            m_cursorCol = NextWordCol();
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
            m_cursorRow = 0;
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_End)) {
            m_cursorRow = lastRow;
            m_scrollToCursorFrames = 2;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace))
            DeleteWordBack();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))
            DeleteWordForward();
    }

    // Typed characters. Suppress while a Ctrl/Cmd shortcut is active, and (mac
    // only) while Option is held so Option combos act as commands, not glyphs.
    const bool typingBlocked = io.KeyCtrl || io.KeySuper || (mac && io.KeyAlt);
    if (!typingBlocked) {
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            ImWchar wc = io.InputQueueCharacters[i];
            if (wc >= 32 && wc < 127) {
                ClearSelection();
                TypeChar(static_cast<char>(wc));
            }
        }
    }

    // Plain (unmodified) navigation/editing keys. Gated so they don't double-fire
    // alongside the Ctrl/Cmd/Option word and extent bindings handled above.
    const bool bareKey = !io.KeyCtrl && !io.KeySuper && !io.KeyAlt;

    if (bareKey && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        m_cursorRow = std::max(0, m_cursorRow - 1);
        m_scrollToCursorFrames = 2;
    }
    if (bareKey && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        m_cursorRow = std::min(lastRow, m_cursorRow + 1);
        m_scrollToCursorFrames = 2;
    }
    if (bareKey && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        m_cursorCol = std::max(0, m_cursorCol - 1);
        m_scrollToCursorFrames = 2;
    }
    if (bareKey && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        m_cursorCol = std::min(kCols - 1, m_cursorCol + 1);
        m_scrollToCursorFrames = 2;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
        m_cursorRow = std::max(0, m_cursorRow - std::max(1, visibleRows - 1));
        m_scrollToCursorFrames = 2;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
        m_cursorRow = std::min(lastRow, m_cursorRow + std::max(1, visibleRows - 1));
        m_scrollToCursorFrames = 2;
    }
    if (bareKey && ImGui::IsKeyPressed(ImGuiKey_Home)) {
        m_cursorCol = 0;
        m_scrollToCursorFrames = 2;
    }
    if (bareKey && ImGui::IsKeyPressed(ImGuiKey_End)) {
        m_cursorCol = std::min(kCols - 1, static_cast<int>(GetLineText(m_cursorRow).size()));
        m_scrollToCursorFrames = 2;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Insert, false))
        m_insertMode = !m_insertMode;

    if (bareKey && ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        EnsureCursorLine();
        if (m_cursorCol > 0) {
            --m_cursorCol;
            TermLine& line = m_lines[static_cast<size_t>(m_cursorRow)];
            if (m_insertMode) {
                if (m_cursorCol < static_cast<int>(line.text.size())) {
                    line.text.erase(line.text.begin() + m_cursorCol);
                    line.spans.clear();
                }
            } else if (m_cursorCol < static_cast<int>(line.text.size())) {
                line.text[static_cast<size_t>(m_cursorCol)] = ' ';
                EraseSpanAt(line, m_cursorCol);
            }
            m_scrollToCursorFrames = 2;
        }
    }
    if (bareKey && ImGui::IsKeyPressed(ImGuiKey_Delete))
        DeleteForwardChar();
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        const int next = ((m_cursorCol / 8) + 1) * 8;
        m_cursorCol = std::min(kCols - 1, next);
        m_scrollToCursorFrames = 2;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        const int row = m_cursorRow;
        if (onReturn)
            onReturn(row, GetLineTextRaw(row));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (m_hasSelection)
            ClearSelection();
        else if (onEscape)
            onEscape();
    }
}

void ARConsoleTerminal::Draw(bool parentFocused)
{
    ScopedFontRole font(FontRole::Console);

    const ImVec2 cell = ImGui::CalcTextSize("M");
    const float cellW = cell.x;
    const float cellH = ImGui::GetTextLineHeight();

    ImGui::BeginChild("##ar_terminal", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);

    const int lineCount = static_cast<int>(m_lines.size());
    const ImVec2 contentSize(kCols * cellW, lineCount * cellH);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float availH = ImGui::GetContentRegionAvail().y;

    // Full-content button: covers at least the visible window area so clicks below
    // the buffer still register, allowing cursor placement on the last line.
    ImGui::InvisibleButton("##ar_grid", ImVec2(std::max(contentSize.x, 1.0f),
                                               std::max({contentSize.y, availH, 1.0f})));

    // Address hyperlink under the mouse: a hex token (4-8 digits) inside an
    // Address-colored span. AsmAddress covers disassembly operand addresses
    // (branch/jump targets, absolute addressing) so those become clickable too.
    struct
    {
        int row = -1;
        int col = 0;
        int len = 0;
        uint32_t value = 0;
    } link;
    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const int hr = static_cast<int>((mouse.y - origin.y) / cellH);
        const int hc = static_cast<int>((mouse.x - origin.x) / cellW);
        if (hr >= 0 && hr < lineCount && hc >= 0) {
            const TermLine& hovLine = m_lines[static_cast<size_t>(hr)];
            const std::string& t = hovLine.text;
            auto isHexAt = [&t](int c) {
                return c >= 0 && c < static_cast<int>(t.size()) &&
                       std::isxdigit(static_cast<unsigned char>(t[static_cast<size_t>(c)]));
            };
            for (const auto& s : hovLine.spans) {
                if ((s.color != core::ArColor::Address && s.color != core::ArColor::AsmAddress) ||
                    hc < s.col || hc >= s.col + s.len)
                    continue;
                if (!isHexAt(hc))
                    break;
                int lo = hc, hi = hc;
                while (lo - 1 >= s.col && isHexAt(lo - 1))
                    --lo;
                while (hi + 1 < s.col + s.len && isHexAt(hi + 1))
                    ++hi;
                const int n = hi - lo + 1;
                if (n >= 4 && n <= 8) {
                    link.row = hr;
                    link.col = lo;
                    link.len = n;
                    link.value = static_cast<uint32_t>(std::stoul(
                        t.substr(static_cast<size_t>(lo), static_cast<size_t>(n)), nullptr, 16));
                }
                break;
            }
        }
    }

    // Register hyperlink under the mouse: any Register-colored span covering the cursor column.
    struct
    {
        int row = -1;
        int col = 0;
        int len = 0;
        uint32_t payload = 0;
    } linkReg;
    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const int hr = static_cast<int>((mouse.y - origin.y) / cellH);
        const int hc = static_cast<int>((mouse.x - origin.x) / cellW);
        if (hr >= 0 && hr < lineCount && hc >= 0) {
            const TermLine& hovLine = m_lines[static_cast<size_t>(hr)];
            for (const auto& s : hovLine.spans) {
                if (s.color != core::ArColor::Register || hc < s.col || hc >= s.col + s.len)
                    continue;
                linkReg.row = hr;
                linkReg.col = s.col;
                linkReg.len = s.len;
                linkReg.payload = s.payload;
                break;
            }
        }
    }

    // Mouse: press anchors a potential drag-selection; dragging extends it;
    // release without a drag is a plain click (address link or cursor move).
    auto mouseCell = [&](int& row, int& col) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        row = std::clamp(static_cast<int>((mouse.y - origin.y) / cellH), 0, lineCount - 1);
        col = std::clamp(static_cast<int>((mouse.x - origin.x) / cellW), 0, kCols - 1);
    };
    if (ImGui::IsItemActivated()) {
        if (!m_prevFocused) {
            // Click is granting focus — don't move cursor or anchor a selection.
            m_grantingFocus = true;
            m_dragging = false;
        } else {
            mouseCell(m_selAnchorRow, m_selAnchorCol);
            m_hasSelection = false;
            m_dragging = false;
            m_grantingFocus = false;
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
        if (!m_grantingFocus) {
            m_dragging = true;
            m_hasSelection = true;
            mouseCell(m_selEndRow, m_selEndCol);
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (m_grantingFocus) {
            m_grantingFocus = false;
        } else if (!m_dragging) {
            if (link.row >= 0) {
                // Cmd (macOS) / Alt (elsewhere) + click opens the actions menu;
                // a plain click jumps to the address in the Memory window.
                ImGuiIO& io = ImGui::GetIO();
                const bool actionMod = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyAlt;
                if (actionMod && onAddressContextMenu)
                    onAddressContextMenu(link.value);
                else if (onAddressClicked)
                    onAddressClicked(link.value);
            } else if (linkReg.row >= 0) {
                if (onCustomRegClicked)
                    onCustomRegClicked(linkReg.payload);
            } else {
                // Compute raw click position — below-buffer clicks land on last line col 0.
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const int rawRow = static_cast<int>((mouse.y - origin.y) / cellH);
                const int rawCol = static_cast<int>((mouse.x - origin.x) / cellW);
                const int lastRow = static_cast<int>(m_lines.size()) - 1;
                if (rawRow > lastRow) {
                    m_cursorRow = lastRow;
                    m_cursorCol = 0;
                } else {
                    m_cursorRow = std::clamp(rawRow, 0, lastRow);
                    m_cursorCol = std::clamp(rawCol, 0, kCols - 1);
                }
                ClampCursor();
            }
        }
    }

    // Right-click on an address routes to the actions menu; right-click on plain
    // text opens the Copy/Paste menu below.
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        if (link.row >= 0 && onAddressContextMenu)
            onAddressContextMenu(link.value);
        else
            ImGui::OpenPopup("##ar_ctx");
    }
    if (ImGui::BeginPopup("##ar_ctx")) {
        const bool mac = ImGui::GetIO().ConfigMacOSXBehaviors;
        if (ImGui::MenuItem("Copy", mac ? "Cmd+C" : "Ctrl+C", false, m_hasSelection))
            CopySelectionToClipboard();
        if (ImGui::MenuItem("Paste", mac ? "Cmd+V" : "Ctrl+V"))
            PasteFromClipboard();
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", mac ? "Cmd+A" : "Ctrl+A"))
            SelectAll();
        ImGui::EndPopup();
    }

    // parentFocused: caller checked focus from the outer window context (more reliable with
    // docking) IsWindowFocused(): direct check — true when this child is the nav window (e.g.
    // clicked directly)
    m_focused = parentFocused || ImGui::IsWindowFocused();
    m_prevFocused = m_focused;

    const float viewH = ImGui::GetWindowHeight();
    const int visibleRows = std::max(1, static_cast<int>(viewH / cellH));

    if (m_focused) {
        ImGui::SetNextFrameWantCaptureKeyboard(true);
        const ImGuiID ownerId = ImGui::GetItemID();
        for (ImGuiKey key : kOwnedKeys)
            ImGui::SetKeyOwner(key, ownerId);
        HandleKeyboard(visibleRows);
    }

    // Keep the cursor visible after edits/appends
    if (m_scrollToCursorFrames > 0) {
        --m_scrollToCursorFrames;
        const float curY = m_cursorRow * cellH;
        const float scrollY = ImGui::GetScrollY();
        if (curY < scrollY)
            ImGui::SetScrollY(curY);
        else if (curY + cellH > scrollY + viewH)
            ImGui::SetScrollY(curY + cellH - viewH);
    }

    // Visible row range
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const float scrollY = ImGui::GetScrollY();
    int firstRow = std::max(0, static_cast<int>(scrollY / cellH));
    int lastRow = std::min(lineCount - 1, static_cast<int>((scrollY + viewH) / cellH) + 1);
    (void)winPos;

    const ImU32 defaultColor = SpanColor(core::ArColor::Default);
    const ImU32 selectionBg = ImGui::GetColorU32(ImGuiCol_TextSelectedBg, 0.9f);

    for (int row = firstRow; row <= lastRow; ++row) {
        TermLine& line = m_lines[static_cast<size_t>(row)];
        const float y = origin.y + row * cellH;

        int selStart = 0, selEnd = 0;
        if (SelectionRange(row, selStart, selEnd)) {
            dl->AddRectFilled(ImVec2(origin.x + selStart * cellW, y),
                              ImVec2(origin.x + (selEnd + 1) * cellW, y + cellH), selectionBg);
        }

        if (line.text.empty())
            continue;

        if (line.spans.empty()) {
            dl->AddText(ImVec2(origin.x, y), defaultColor, line.text.c_str(),
                        line.text.c_str() + line.text.size());
            continue;
        }

        // Draw per-span runs; gaps between spans render in Default. Spans are sorted in place
        // once per mutation — copying + sorting per visible line per frame was pure waste,
        // since spans never change after the line is written.
        int col = 0;
        if (!line.spansSorted) {
            std::sort(line.spans.begin(), line.spans.end(),
                      [](const core::ArSpan& a, const core::ArSpan& b) { return a.col < b.col; });
            line.spansSorted = true;
        }
        const std::vector<core::ArSpan>& spans = line.spans;
        const int len = static_cast<int>(line.text.size());
        for (const auto& s : spans) {
            int start = std::clamp(s.col, 0, len);
            int end = std::clamp(s.col + s.len, 0, len);
            if (col < start) {
                dl->AddText(ImVec2(origin.x + col * cellW, y), defaultColor,
                            line.text.c_str() + col, line.text.c_str() + start);
            }
            if (start < end) {
                if (s.color == core::ArColor::Swatch) {
                    // Filled colour block from the span's RGB payload (inset 1px).
                    dl->AddRectFilled(ImVec2(origin.x + start * cellW + 1.0f, y + 1.0f),
                                      ImVec2(origin.x + end * cellW - 1.0f, y + cellH - 1.0f),
                                      SwatchColor(s.payload));
                } else {
                    dl->AddText(ImVec2(origin.x + start * cellW, y), SpanColor(s.color),
                                line.text.c_str() + start, line.text.c_str() + end);
                }
            }
            col = std::max(col, end);
        }
        if (col < len) {
            dl->AddText(ImVec2(origin.x + col * cellW, y), defaultColor, line.text.c_str() + col,
                        line.text.c_str() + len);
        }
    }

    // Hovered address hyperlink: underline + hand cursor
    if (link.row >= 0) {
        const float x0 = origin.x + link.col * cellW;
        const float x1 = x0 + link.len * cellW;
        const float y = origin.y + (link.row + 1) * cellH - 1.0f;
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), SpanColor(core::ArColor::Address));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    // Hovered register hyperlink: underline + hand cursor
    if (linkReg.row >= 0 && link.row < 0) {
        const float x0 = origin.x + linkReg.col * cellW;
        const float x1 = x0 + linkReg.len * cellW;
        const float y = origin.y + (linkReg.row + 1) * cellH - 1.0f;
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), SpanColor(core::ArColor::Register));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // Cursor: full block in overwrite mode, thin bar in insert mode. Blinks
    // while focused, hollow box when not.
    {
        const float x = origin.x + m_cursorCol * cellW;
        const float y = origin.y + m_cursorRow * cellH;
        const ImU32 cursorColor = SpanColor(core::ArColor::Default);
        const bool blinkOn =
            !m_focused || fmodf(static_cast<float>(ImGui::GetTime()), 1.06f) < 0.53f;

        if (m_focused && blinkOn) {
            if (m_insertMode) {
                // Thin vertical bar at the cell's left edge; glyph stays readable.
                const float barW = std::max(1.0f, cellW * 0.15f);
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + barW, y + cellH), cursorColor);
            } else {
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cellW, y + cellH), cursorColor);
                // Redraw the glyph under the cursor in the background color
                const std::string& text = m_lines[static_cast<size_t>(m_cursorRow)].text;
                if (m_cursorCol < static_cast<int>(text.size())) {
                    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_ChildBg, 1.0f);
                    const ImU32 inverse =
                        (bg & IM_COL32_A_MASK) ? bg : ImGui::GetColorU32(ImGuiCol_WindowBg, 1.0f);
                    dl->AddText(ImVec2(x, y), inverse, text.c_str() + m_cursorCol,
                                text.c_str() + m_cursorCol + 1);
                }
            }
        } else if (!m_focused) {
            dl->AddRect(ImVec2(x, y), ImVec2(x + cellW, y + cellH), cursorColor);
        }
    }

    ImGui::EndChild();
}

} // namespace adm::ui
