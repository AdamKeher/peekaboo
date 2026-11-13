#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "../../core/ExocetActivity.h"

namespace adm::core {
class ExocetSession;
struct ExocetTrackInfo;
} // namespace adm::core

namespace adm::ui {

// The Exocet Grid: the timing - a Rocket-shaped tracker grid
// over the song's played rows - the music's channels read-only on the left, the production's tracks
// across - with the running demo's playhead, the selected track's details and graph below it,
// the proofing and the issues. The pack and the kit (save, push, inject, check, a new disk) are
// the Exocet window's, ExocetControlView. Everything goes through core::ExocetSession.
class ExocetView
{
public:
    explicit ExocetView(core::ExocetSession* session);

    void Open() { m_isOpen = true; }
    void Restore()
    {
        m_isOpen = true;
        m_noFocusOnAppearing = true;
    }
    void Close() { m_isOpen = false; }
    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    void Draw();
    // Open the grid in front, on played row `row`: selected and scrolled into view, Follow off
    // (the timeline's double-click)
    void ShowRow(int row);

    // Space: pause and resume the emulator.
    std::function<void()> m_onTogglePause;
    // The frame history: the machine frames a seek can land on (false: none), and the seek.
    // A row's Frame or Time clicked inside it rewinds there.
    std::function<bool(uint64_t& first, uint64_t& last)> m_recordedRange;
    std::function<void(uint64_t frame)> m_onSeekFrame;
    // The eyedropper on the emulator's picture (EmulatorView::BeginPixelPick): whether there is
    // a picture to pick from, and the pick - an empty callback cancels the one in progress.
    std::function<bool()> m_canPickScreenColour;
    std::function<void(std::function<void(bool picked, uint32_t rgba)>)> m_pickScreenColour;

private:
    // The machine frame of song frame `songFrame`, when the frame history holds it
    bool SeekableFrame(uint32_t songFrame, uint64_t& emuFrame) const;
    // Frame or Time: its text bright when the frame history holds it, and a click seeks there
    void DrawSeekCell(const std::string& text, uint32_t songFrame);
    // The left edge of the Row cell: a bar on the rows the frame history holds
    void DrawRecordedBar(uint32_t songFrame);
    bool m_haveRange = false; // the frame history's range, this frame
    uint64_t m_rangeFirst = 0, m_rangeLast = 0;

    // a song frame as mm:ss.cc at the session's frame rate
    std::string Time(uint32_t frame) const;
    core::ExocetSession* m_session;
    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;
    // ShowRow: frames left to bring the window to the front and the row into view - a second, as
    // the click's own window takes the focus back at the end of its frame, and a window just
    // opened has no height to scroll in yet
    int m_showRowFrames = 0;

    int m_selRow = -1;
    int m_selTick = 0; // the tick of the selected row: 0 unless the row is expanded
    int m_selTrack = -1;
    // A range of cells: from this end (row, tick, track) to the cursor's, the lines between on
    // the shown tracks between; -1: the cursor's cell alone. Shift with the arrows, or a drag,
    // makes one.
    int m_anchorRow = -1;
    int m_anchorTick = 0;
    int m_anchorTrack = -1;
    bool m_dragging = false;    // the left button went down on a cell: the cursor follows it
    bool m_wideGutter = false;  // a row has a breakpoint and a bookmark: two slots
    int m_bookmarkEditRow = -1; // the bookmark DrawBookmarkPopup edits
    bool m_openBookmarkEdit = false;
    char m_bookmarkName[64] = {};
    int m_pasteCount = 0; // each paste's undo step its own
    bool m_follow = true;
    int m_lastPlayRow = -1;
    int m_scrollToRow = -1;
    int m_scrollToTick = 0;
    // Rows expanded to a line a tick: the ones asked for, and the
    // ones holding a key on a tick past 0 - open by themselves - unless collapsed by hand
    std::set<int> m_expanded, m_collapsed;
    std::string m_selPackPath; // the pack the selection and the expanded rows are of
    // The grid's lines as drawn last: (played row, tick), and each row's first line
    std::vector<std::pair<int, int>> m_lines;
    std::vector<int> m_rowLine;
    char m_edit[32] = {};
    bool m_editing = false;
    bool m_editFresh = false; // the key's value put up to edit: the first character replaces it
    std::string m_editError;
    int m_filterSample = 0; // 0: none
    bool m_showChannels = true;
    // The music's channels are a group of their own: folded to one column (a mark per trigger),
    // and frozen with the lead columns so they stay in view as the tracks scroll sideways
    bool m_musicFolded = false;
    bool m_freezeMusic = false;

    // Groups by activity: Auto opens a group while the demo has it enabled
    // at the focus row - the playhead when following it or paused, else the middle of the grid -
    // and folds it to one column otherwise.
    enum class GroupMode { Auto, Open, Folded, Hidden };
    std::map<std::string, GroupMode> m_groupModes;
    bool m_autoGroups = true;
    // only the groups active at the focus row; the others leave the grid, pinned or not
    bool m_activeGroups = false;
    int m_viewRow = -1;      // the row in the middle of the grid, last frame
    float m_gridScrollY = 0; // the grid's scroll, carried across a change of its columns
    float m_gridScrollX = 0;
    // frames left putting m_gridScrollX back: a new table is 0 wide until it has been drawn,
    // which clamps a scroll set before then to its left edge
    int m_restoreScrollX = 0;
    uint64_t m_gridColumns = 0;
    // the grid's share of the height, dragged by the splitter under it
    float m_gridSplitRatio = 0.58f;
    // The order of groups dragged into, by name; empty: the demo's order (GroupOrder)
    std::vector<std::string> m_groupOrder;
    std::string m_groupMenu; // the group a header's right click is for
    int m_trackMenu = -1;    // and the track, when it was a track's name; -1: the group's
    bool m_openGroupMenu = false;

    struct GridColumn
    {
        int track = -1; // a track's column, or
        int group = -1; // a folded group's (the pack's group index)
    };

    void DrawToolbar();
    // Follow, Channels, Auto groups, Active groups: under the grid, as they are the grid's
    void DrawGridOptions();
    void DrawGrid();
    void DrawDetail();
    void DrawGraph(int track);
    void DrawColourGraph(int track, ImVec2 p0, ImVec2 size, int r0, int r1);
    void DrawColourEdit(const core::ExocetTrackInfo& t);
    // The eyedropper's button: a pick for the selected cell, or cancels the one in progress
    void DrawScreenPickButton(const core::ExocetTrackInfo& t);
    // Key a colour cell with `rgb` (0xRRGGBB--), keeping the cell's alpha and its key's
    // interpolation (step for a new one)
    void KeyPickedColour(const std::string& track, int row, int tick, uint32_t rgb);
    bool m_pickingColour = false;
    float m_mixerOriginal[4] = {}; // the colour when the mixer opened: its "Original" swatch
    void DrawProof();
    void DrawIssues();
    void HandleKeys();
    // Keeps the selection inside the active pack's tracks and the check's rows (EXO-03)
    void ClampSelection();
    void CommitEdit();
    // The selection is moving off the cell: a value typed into it is keyed, as Enter would
    void LeaveCell();
    // The director's cuts as the grid's parts, each from its key to the next, in song order
    struct Part
    {
        int row = 0; // the cut key's played row
        int sub = 0; // and its place in the row, 256ths
        std::string name;
    };
    std::vector<Part> Parts();
    // The part a line - (row, tick) - is in, an index into `parts`; -1 before the first cut
    int PartOf(const std::vector<Part>& parts, int row, int tick) const;
    // Double-click: key the cell at the value it shows, or put its key's value up to edit.
    void KeyCell(int row, int tick, int track);
    // The Row cell's gutter: the row's breakpoint, set and cleared by a click, and its bookmark,
    // set by a right-click (or M) and edited by one; a slot a row, two while a row has both.
    void DrawBreakGutter(int row);
    // The bookmark editor a right-click on one opens: its name, its slot, its removal
    void DrawBookmarkPopup();
    // The cursor to a bookmarked row - with Shift, the range taken there - and why not
    void GoToBookmark(int row);
    // Ctrl+C: the selected cell as "<value> <interp>" (a key) or "<value>" (between keys); a
    // range as its keys, by row from its first, a block a track (CopyRange's format).
    // Ctrl+V: keys the selected cell from that, checked against the track's type first; a range
    // goes in from the cursor's row down and its track rightwards, all of it or none.
    void CopyCell();
    void PasteCell();
    bool HasRange() const;
    // The range's first and last lines, the cursor's alone without one
    std::pair<int, int> RangeLines() const;
    // The range's tracks, in the grid's order: the cursor's alone without one
    std::vector<int> RangeTracks() const;
    void CopyRange();
    void PasteRange(const std::string& text);
    // Delete on a range: every key under it, across its tracks, one undo step
    void DeleteRange();
    // A value typed or pasted into a cell of `info`: false with `error` set when it is not one
    static bool ParseValue(const core::ExocetTrackInfo& info, const std::string& text, double* v,
                           std::string* error);
    // A value as a cell is typed: #RRGGBBAA, a fix's decimal, else a whole number
    static std::string ValueText(const core::ExocetTrackInfo& info, double value);
    // Ctrl+Z, Ctrl+Y and the toolbar's buttons.
    void Undo(bool redo);
    void DrawGroupModes(const std::string& group);
    // The music group's bar over its column `k` of `n`: its name on the first, Freeze on the last
    void DrawMusicGroupCell(int k, int n);
    void DrawFoldedCell(int group, int row, int tick, core::ExocetActivity::State state);
    // The pack's group indices in the grid's order: as dragged, else the demo's - the director
    // first, then by the first row each was recorded enabled on, the ones never seen last.
    std::vector<int> GroupOrder() const;
    // Put `group` where `target` is, the ones between shifting over.
    void MoveGroup(const std::string& group, const std::string& target);
    // Drag and drop a group by the last item: a group bar.
    void GroupDragDrop(const std::string& group);
    // Groups in the grid's order, each with its tracks in manifest order.
    std::vector<std::pair<int, std::vector<int>>> GroupedTracks() const;
    GroupMode ModeOf(const std::string& group) const;
    // The group's own colour, by its place in the manifest; `alpha` 0..255
    ImU32 GroupColour(const std::string& group, int alpha = 255) const;
    // Open it if it is folded, fold it if it is open: pinned, until set back to Auto
    void ToggleGroup(const std::string& group);
    bool GroupOpen(const std::string& group) const;
    int FocusRow() const;
    // Rows holding a key on a tick past 0, on any track
    std::set<int> TickedRows();
    bool Expanded(int row, const std::set<int>& ticked) const;
    void ToggleExpand(int row);
    // The grid's lines, from the rows and which are expanded
    void BuildLines();
    // The line of (row, tick), or of the row when it is not expanded
    int LineOf(int row, int tick) const;
    // The tick of a row a fraction falls on (the tick whose own fraction it is, or past)
    static int TickOfFrac(uint16_t frac, int frames);
    std::vector<GridColumn> Columns() const;
    std::vector<int> VisibleTracks() const;
};

} // namespace adm::ui
