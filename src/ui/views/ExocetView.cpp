#include "ExocetView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include <fmt/format.h>
#include <imgui_internal.h>

#include "../../core/ExocetSession.h"
#include "../../core/JsonRead.h"
#include "../IconsFontAwesome6.h"
#include "../UiIntrospect.h"
#include "ExocetSong.h"
#include "ExocetUi.h"

namespace adm::ui {

using core::ExocetIssue;
using core::ExocetSession;
using exo::Cell;
using exo::FormatValue;
using exo::Glyph;
using exo::kBookmarkColour;
using exo::SongTime;
using exo::ToolbarFlow;
using exo::TooltipWhenDisabled;
using exo::TypeOf;

namespace {

// The grid's leading columns: Row, Pat (the pattern), Frame, Time, then Part when the demo has a
// director; the channels and the tracks follow
constexpr int kLead = 4;
constexpr int kPatColumn = 1;

// The grid takes its own keys: ImGui's keyboard navigation would keep a second cursor in it,
// moved by the same arrows over other cells, and Enter would activate that one
constexpr ImGuiWindowFlags kOwnKeys = ImGuiWindowFlags_NoNavInputs;

constexpr ImVec4 kPlayTint = {0.20f, 0.75f, 0.35f, 0.30f};
constexpr ImVec4 kSelTint = {0.30f, 0.55f, 1.00f, 0.28f};
// The selected cell's frame, over its tint: the cursor typing goes into
constexpr ImVec4 kSelFrame = {0.45f, 0.70f, 1.00f, 1.00f};
// A key since the .exocet was saved, in its cell's top right corner: added, changed, removed
constexpr ImVec4 kChangeColours[3] = {
    {0.40f, 0.90f, 0.45f, 1.00f}, {1.00f, 0.72f, 0.25f, 1.00f}, {1.00f, 0.35f, 0.35f, 1.00f}};
// The rule along the top of a part's first line, across the grid
constexpr ImU32 kPartRule = IM_COL32(235, 235, 250, 190);
// A row with a breakpoint: its Row, Pat, Frame and Time cells, and a rule like a part's
constexpr ImU32 kBreakTint = IM_COL32(230, 60, 60, 70);
constexpr ImU32 kBreakRule = IM_COL32(230, 60, 60, 230);
// The Part column: the selected cell's part, and the others' alternating washes
constexpr ImU32 kPartMine = IM_COL32(77, 140, 255, 70);
constexpr ImU32 kPartWash[2] = {IM_COL32(255, 255, 255, 20), IM_COL32(255, 255, 255, 8)};
// The selected cell's line, all the way across, so its music shows beside the keys
constexpr ImVec4 kSelRowTint = {0.30f, 0.55f, 1.00f, 0.13f};
constexpr ImVec4 kHitTint = {1.00f, 0.80f, 0.20f, 0.14f};
constexpr ImVec4 kPatternTint = {1.00f, 1.00f, 1.00f, 0.05f};
constexpr ImVec4 kKeyColour = {1.00f, 0.92f, 0.55f, 1.00f};
constexpr ImVec4 kEditColour = {1.00f, 0.60f, 0.20f, 1.00f};
constexpr ImVec4 kBlockColour = {1.00f, 0.40f, 0.35f, 1.00f};
constexpr ImVec4 kWarnColour = {1.00f, 0.78f, 0.30f, 1.00f};
constexpr ImVec4 kInfoColour = {0.60f, 0.75f, 1.00f, 1.00f};
constexpr ImVec4 kOkColour = {0.45f, 0.85f, 0.45f, 1.00f};

ImVec4 LevelColour(const std::string& level)
{
    return level == "block" ? kBlockColour : level == "warn" ? kWarnColour : kInfoColour;
}

// A colour track's 0xRRGGBBAA
ImU32 ColourOf(int32_t v)
{
    const auto c = static_cast<uint32_t>(v);
    return IM_COL32(c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255);
}

// A colour typed into a cell: "#RRGGBBAA", "#RRGGBB", or either without the '#'
bool ParseTypedColour(const char* text, uint32_t* rgba)
{
    const std::string s = text[0] == '#' ? std::string(text) : "#" + std::string(text);
    return core::ExocetParseColor(s, rgba);
}

// A swatch over the start of the last item: the colour at its alpha on the cell, framed
void DrawSwatch(int32_t v)
{
    const ImVec2 a = ImGui::GetItemRectMin();
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 lo(a.x + 1, a.y + 1), hi(a.x + h * 1.5f, a.y + h - 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(lo, hi, ColourOf(v));
    dl->AddRect(lo, hi, IM_COL32(0, 0, 0, 160));
}

// The room DrawSwatch covers, in front of a colour cell's text
constexpr const char* kSwatchRoom = "      ";

// A group bar let go of after a drag was moved, not clicked
bool Dragged()
{
    const float d = ImGui::GetIO().MouseDragThreshold;
    return ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] >= d * d;
}

// The table cell the cursor is at the start of, its padding and all
ImRect CellHere()
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 pad = ImGui::GetStyle().CellPadding;
    return {ImVec2(p.x - pad.x, p.y - pad.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x + pad.x,
                                                     p.y + ImGui::GetTextLineHeight() + pad.y)};
}

// A change since the save: a triangle in the cell's top right corner
void DrawChangeMark(const ImRect& cell, int kind)
{
    const float s = ImGui::GetTextLineHeight() * 0.5f;
    const ImVec2 c(cell.Max.x, cell.Min.y);
    ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(c.x - s, c.y), c, ImVec2(c.x, c.y + s),
                                                  ImGui::GetColorU32(kChangeColours[kind]));
}

// A text's words, split on white space
std::vector<std::string> Words(const std::string& text)
{
    std::vector<std::string> out;
    std::string word;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!word.empty())
                out.push_back(std::move(word));
            word.clear();
        } else {
            word += c;
        }
    }
    if (!word.empty())
        out.push_back(std::move(word));
    return out;
}

// The cells of a range, under the cursor's own
constexpr ImVec4 kRangeTint = {0.30f, 0.55f, 1.00f, 0.18f};

// The music group's bar: no group of the manifest's, so a grey of its own
constexpr ImU32 kMusicColour = IM_COL32(176, 180, 196, 255);
constexpr const char* kMusicName = "Music";

// The Freeze checkbox in a header row: no frame padding, so it keeps the row's height
bool HeaderCheckbox(const char* label, bool* v)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    const bool changed = ImGui::Checkbox(label, v);
    ImGui::PopStyleVar();
    return changed;
}

// The width of the music group's bar folded to one column: its name and Freeze side by side
float MusicBarWidth()
{
    const ImGuiStyle& s = ImGui::GetStyle();
    return ImGui::CalcTextSize(ICON_FA_CHEVRON_RIGHT " Music").x + s.ItemSpacing.x +
           ImGui::GetTextLineHeight() + s.ItemInnerSpacing.x + ImGui::CalcTextSize("Freeze").x;
}

// A folded music column's cell: a mark a channel, lit where it triggers a note, dim where it has
// only a sample or an effect; the filter's sample in the key colour
void DrawMusicMarks(const std::vector<std::array<uint16_t, 4>>& cells, int voices, int sample)
{
    const float h = ImGui::GetTextLineHeight();
    const float w = std::max(4.0f, h * 0.5f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int v = 0; v < voices; ++v) {
        const ImVec2 lo(p.x + v * (w + 3), p.y + 2), hi(lo.x + w, p.y + h - 2);
        const bool has = v < static_cast<int>(cells.size());
        const auto& c = has ? cells[v] : std::array<uint16_t, 4>{};
        if (sample && c[0] == sample)
            dl->AddRectFilled(lo, hi, ImGui::GetColorU32(kKeyColour));
        else if (c[1])
            dl->AddRectFilled(lo, hi, IM_COL32(90, 255, 120, 220));
        else if (c[0] || c[2] || c[3])
            dl->AddRectFilled(lo, hi, IM_COL32(150, 150, 160, 110));
        else
            dl->AddRect(lo, hi, IM_COL32(150, 150, 160, 60));
    }
    ImGui::Dummy(ImVec2(voices * (w + 3), h));
    if (!ImGui::IsItemHovered())
        return;
    std::string tip;
    for (int v = 0; v < voices; ++v)
        tip += fmt::format("{}Ch {}  {}", v ? "\n" : "", v + 1,
                           v < static_cast<int>(cells.size()) ? Cell(cells[v]) : "--- .. ...");
    ImGui::SetTooltip("%s", tip.c_str());
}

} // namespace

ExocetView::ExocetView(core::ExocetSession* session) : m_session(session) {}

std::vector<int> ExocetView::GroupOrder() const
{
    std::vector<int> out;
    const auto* pack = m_session->Pack();
    if (!pack)
        return out;
    const auto& groups = pack->Groups();
    const int n = static_cast<int>(groups.size());
    const std::vector<int> demo = exo::GroupsByFirstRun(*pack, m_session->Activity());
    if (m_groupOrder.empty())
        return demo;
    // as dragged; a group the manifest has gained since goes in at the end, in the demo's order
    for (const auto& name : m_groupOrder)
        for (int g = 0; g < n; ++g)
            if (groups[g].name == name)
                out.push_back(g);
    for (int g : demo)
        if (std::find(out.begin(), out.end(), g) == out.end())
            out.push_back(g);
    return out;
}

void ExocetView::MoveGroup(const std::string& group, const std::string& target)
{
    const auto& groups = m_session->Pack()->Groups();
    std::vector<std::string> order;
    for (int g : GroupOrder())
        order.push_back(groups[g].name);
    const auto from = std::find(order.begin(), order.end(), group);
    const auto to = std::find(order.begin(), order.end(), target);
    if (from == order.end() || to == order.end() || from == to)
        return;
    if (from < to)
        std::rotate(from, from + 1, to + 1);
    else
        std::rotate(to, from, from + 1);
    m_groupOrder = std::move(order);
}

void ExocetView::GroupDragDrop(const std::string& group)
{
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("EXOCET_GROUP", group.c_str(), group.size() + 1);
        ImGui::Text("%s %s", ICON_FA_LAYER_GROUP, group.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* p = ImGui::AcceptDragDropPayload("EXOCET_GROUP"))
            MoveGroup(static_cast<const char*>(p->Data), group);
        ImGui::EndDragDropTarget();
    }
}

std::vector<std::pair<int, std::vector<int>>> ExocetView::GroupedTracks() const
{
    std::vector<std::pair<int, std::vector<int>>> out;
    const auto* pack = m_session->Pack();
    if (!pack)
        return out;
    for (int g : GroupOrder()) {
        std::vector<int> tracks;
        for (const auto& t : pack->Tracks())
            if (t.group == pack->Groups()[g].name)
                tracks.push_back(t.index);
        if (!tracks.empty())
            out.emplace_back(g, std::move(tracks));
    }
    return out;
}

ImU32 ExocetView::GroupColour(const std::string& group, int alpha) const
{
    const auto& groups = m_session->Pack()->Groups();
    int k = 0;
    while (k < static_cast<int>(groups.size()) && groups[k].name != group)
        ++k;
    return exo::GroupColour(k, alpha);
}

void ExocetView::ToggleGroup(const std::string& group)
{
    m_groupModes[group] = GroupOpen(group) ? GroupMode::Folded : GroupMode::Open;
}

ExocetView::GroupMode ExocetView::ModeOf(const std::string& group) const
{
    auto it = m_groupModes.find(group);
    return it == m_groupModes.end() ? GroupMode::Auto : it->second;
}

int ExocetView::FocusRow() const
{
    // paused, the playhead's too: the groups stay as they are while the grid is scrolled to
    // look or key elsewhere, rather than opening and folding under the pointer
    const auto& b = m_session->Block();
    if ((m_follow || m_session->Paused()) && b.present && m_session->ManifestMatches())
        return static_cast<int>(b.row);
    return m_viewRow;
}

std::vector<ExocetView::Part> ExocetView::Parts()
{
    std::vector<Part> out;
    const auto* pack = m_session->Pack();
    const auto* director = pack ? pack->FindTrack(pack->Director()) : nullptr;
    if (!director)
        return out;
    for (const auto& k : m_session->HostKeys(director->index)) // in song order
        out.push_back({k.row, k.sub, FormatValue(*director, k.value)});
    return out;
}

int ExocetView::PartOf(const std::vector<Part>& parts, int row, int tick) const
{
    // an expanded row's line is its tick's; a folded row is in the part a cut inside it starts
    const bool expanded =
        row >= 0 && row < static_cast<int>(m_rowLine.size()) && LineOf(row, 1) != LineOf(row, 0);
    const int sub =
        expanded ? core::ExocetSub(static_cast<uint32_t>(tick), m_session->RowTicks(row)) : 255;
    int at = -1;
    for (int p = 0; p < static_cast<int>(parts.size()); ++p) {
        if (parts[p].row > row || (parts[p].row == row && parts[p].sub > sub))
            break;
        at = p;
    }
    return at;
}

void ExocetView::ShowRow(int row)
{
    if (row < 0)
        return;
    LeaveCell();
    m_isOpen = true;
    m_noFocusOnAppearing = false;
    m_selRow = row;
    m_selTick = 0;
    m_anchorRow = -1;
    m_follow = false;
    m_showRowFrames = 3;
}

void ExocetView::LeaveCell()
{
    // a value typed is keyed, as Enter would; one only put up to edit (a double-click) goes
    if (m_editing && !m_editFresh)
        CommitEdit();
    m_editing = false;
}

std::set<int> ExocetView::TickedRows()
{
    std::set<int> out;
    const auto* pack = m_session->Pack();
    if (!pack)
        return out;
    for (const auto& t : pack->Tracks())
        for (const auto& k : m_session->HostKeys(t.index))
            if (k.sub)
                out.insert(k.row);
    return out;
}

bool ExocetView::Expanded(int row, const std::set<int>& ticked) const
{
    if (m_session->RowTicks(row) < 2)
        return false;
    return m_expanded.count(row) || (ticked.count(row) && !m_collapsed.count(row));
}

void ExocetView::ToggleExpand(int row)
{
    if (Expanded(row, TickedRows())) {
        m_expanded.erase(row);
        m_collapsed.insert(row);
        if (m_selRow == row)
            m_selTick = 0;
    } else {
        m_collapsed.erase(row);
        m_expanded.insert(row);
    }
}

void ExocetView::BuildLines()
{
    const auto& check = m_session->Check();
    const int nrows = check.valid ? static_cast<int>(check.rows.size()) : 0;
    const std::set<int> ticked = TickedRows();
    m_lines.clear();
    m_rowLine.assign(nrows, 0);
    for (int i = 0; i < nrows; ++i) {
        m_rowLine[i] = static_cast<int>(m_lines.size());
        const int n = Expanded(i, ticked) ? m_session->RowTicks(i) : 1;
        for (int t = 0; t < n; ++t)
            m_lines.emplace_back(i, t);
    }
}

int ExocetView::LineOf(int row, int tick) const
{
    if (row < 0 || row >= static_cast<int>(m_rowLine.size()))
        return 0;
    const int first = m_rowLine[row];
    const int next = row + 1 < static_cast<int>(m_rowLine.size())
                         ? m_rowLine[row + 1]
                         : static_cast<int>(m_lines.size());
    return std::clamp(first + tick, first, std::max(first, next - 1));
}

int ExocetView::TickOfFrac(uint16_t frac, int frames)
{
    // tick t's own fraction is t * 65536 / frames rounded down; this is its inverse
    return std::clamp(static_cast<int>((uint32_t(frac) * frames + 65535) >> 16), 0,
                      std::max(0, frames - 1));
}

bool ExocetView::GroupOpen(const std::string& group) const
{
    switch (ModeOf(group)) {
        case GroupMode::Open:
            return true;
        case GroupMode::Folded:
        case GroupMode::Hidden:
            return false;
        default:
            // open only where the demo has been seen running it: before a first run, and on a
            // row not seen yet, it is folded
            return !m_autoGroups || m_session->Activity().At(group, FocusRow()) ==
                                        core::ExocetActivity::State::Active;
    }
}

std::vector<ExocetView::GridColumn> ExocetView::Columns() const
{
    std::vector<GridColumn> out;
    const auto* pack = m_session->Pack();
    for (const auto& [g, tracks] : GroupedTracks()) {
        const std::string& name = pack->Groups()[g].name;
        if (ModeOf(name) == GroupMode::Hidden)
            continue;
        if (m_activeGroups &&
            m_session->Activity().At(name, FocusRow()) != core::ExocetActivity::State::Active)
            continue;
        if (!GroupOpen(name)) {
            out.push_back({-1, g});
            continue;
        }
        for (int t : tracks)
            out.push_back({t, g});
    }
    return out;
}

std::vector<int> ExocetView::VisibleTracks() const
{
    std::vector<int> out;
    for (const auto& c : Columns())
        if (c.track >= 0)
            out.push_back(c.track);
    return out;
}

void ExocetView::DrawGroupModes(const std::string& group)
{
    static const char* kNames[] = {"Auto", "Open", "Folded", "Hidden"};
    static const char* kTips[] = {
        "Open while the demo has the group enabled at the playhead (or, while it runs and the "
        "grid is not following it, the middle of the grid), folded to one column otherwise - and "
        "until the demo has been seen running it",
        "Always open", "Always one column", "Not in the grid"};
    int mode = static_cast<int>(ModeOf(group));
    ImGui::PushID(group.c_str());
    for (int m = 0; m < 4; ++m) {
        if (m)
            ImGui::SameLine();
        if (ImGui::RadioButton(kNames[m], &mode, m)) {
            if (m == 0)
                m_groupModes.erase(group);
            else
                m_groupModes[group] = static_cast<GroupMode>(m);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", kTips[m]);
    }
    ImGui::PopID();
}

void ExocetView::DrawMusicGroupCell(int k, int n)
{
    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, kMusicColour);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(20, 20, 24, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(255, 255, 255, 60));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(255, 255, 255, 90));
    const bool last = k == n - 1;
    if (k == 0 || !last) {
        // the name on the first; sized to it when Freeze shares the cell, so both take clicks
        const std::string label =
            k ? std::string("##music")
              : std::string(m_musicFolded ? ICON_FA_CHEVRON_RIGHT : ICON_FA_CHEVRON_DOWN) + " " +
                    kMusicName + "##music";
        const ImVec2 size =
            last ? ImVec2(ImGui::CalcTextSize(label.c_str(), nullptr, true).x, 0) : ImVec2(0, 0);
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, size))
            m_musicFolded = !m_musicFolded;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s: the song's channels, read-only.\nClick to %s them.", kMusicName,
                              m_musicFolded ? "open" : "fold");
    }
    if (last) {
        if (k == 0)
            ImGui::SameLine();
        HeaderCheckbox("Freeze", &m_freezeMusic);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Keep the music's columns in view, with Row to Time, as the grid "
                              "scrolls sideways");
    }
    ImGui::PopStyleColor(3);
}

namespace {

// The recorded frames of played rows r0..r1-1: the record is keyed by song frame, so the graphs
// walk only what they draw, not every frame since the boot
std::pair<std::map<uint32_t, core::ExocetSession::Sample>::const_iterator,
          std::map<uint32_t, core::ExocetSession::Sample>::const_iterator>
RecordedRows(const std::map<uint32_t, core::ExocetSession::Sample>& recorded,
             const core::ExocetCheck& check, int r0, int r1)
{
    const int nrows = static_cast<int>(check.rows.size());
    if (r0 < 0 || r0 >= r1 || r0 >= nrows)
        return {recorded.end(), recorded.end()};
    // the check's rows are in frame order, so the end is never before the start
    const uint32_t first = check.rows[static_cast<size_t>(r0)].frame;
    const uint32_t end = r1 < nrows ? check.rows[static_cast<size_t>(r1)].frame : ~0u;
    return {recorded.lower_bound(first), end > first ? recorded.lower_bound(end) : recorded.end()};
}

} // namespace

// The selection is indices into the active pack's tracks and the check's rows. Another
// document's pack, a rebuild with fewer tracks or a shorter song leaves it pointing past them,
// and every key handler indexes with it: the pack changing starts the selection again, and
// anything still past the end is pulled back.
void ExocetView::ClampSelection()
{
    const auto* pack = m_session->Pack();
    const std::string path = pack ? pack->Path() : std::string();
    if (path != m_selPackPath) {
        m_selPackPath = path;
        m_selTrack = m_selRow = m_anchorRow = m_anchorTrack = -1;
        m_selTick = m_anchorTick = 0;
        m_editing = false;
        m_expanded.clear();
        m_collapsed.clear();
    }
    const int ntracks = pack ? static_cast<int>(pack->Tracks().size()) : 0;
    if (m_selTrack >= ntracks) {
        m_selTrack = m_anchorRow = -1;
        m_editing = false;
    }
    const int nrows = static_cast<int>(m_session->Check().rows.size());
    if (m_selRow >= nrows) {
        m_selRow = nrows - 1; // -1 with no rows
        m_selTick = 0;
        m_editing = false;
    }
    if (m_anchorRow >= nrows)
        m_anchorRow = -1;
    if (m_anchorTrack >= ntracks)
        m_anchorTrack = -1;
}

void ExocetView::Draw()
{
    if (!m_isOpen)
        return;
    ClampSelection();
    ImGuiWindowFlags flags = kOwnKeys;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }
    if (m_showRowFrames > 0) {
        --m_showRowFrames;
        ImGui::SetNextWindowFocus();
        ui::FocusContainingPanels("Exocet");
        m_scrollToRow = m_selRow;
        m_scrollToTick = m_selTick;
    }
    ImGui::SetNextWindowSize(ImVec2(1100, 720), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(1.0f); // a grid over whatever floats behind it is unreadable
    if (!ImGui::Begin(ICON_FA_TABLE_CELLS " Exocet Grid###Exocet", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }

    DrawToolbar();

    // the grid's options sit under it, their line taken off the height the splitter shares
    const float avail = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
    const float splitterH = 4.0f;
    const float minPane = 80.0f;
    const float gridH = std::clamp(avail * m_gridSplitRatio, minPane,
                                   std::max(minPane, avail - splitterH - minPane));
    ImGui::BeginChild("##grid_area", ImVec2(0, gridH), ImGuiChildFlags_None, kOwnKeys);
    DrawGrid();
    ImGui::EndChild();
    DrawGridOptions();

    // drag to share the height between the grid and the detail/proof panes
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
    ImGui::Button("##ExocetGridSplitter", ImVec2(-1, splitterH));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemActive() && avail > 0.0f) {
        const float newTop = gridH + ImGui::GetIO().MouseDelta.y;
        if (newTop >= minPane && avail - splitterH - newTop >= minPane)
            m_gridSplitRatio = newTop / avail;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    if (ImGui::BeginTable("##below", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("proof", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("##detail", ImVec2(0, 0), ImGuiChildFlags_None, kOwnKeys);
        DrawDetail();
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("##proof", ImVec2(0, 0), ImGuiChildFlags_None, kOwnKeys);
        DrawProof();
        DrawIssues();
        ImGui::EndChild();
        ImGui::EndTable();
    }

    // not while a widget has the keys - the colour's RGBA fields, typed into, would key the grid
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive())
        HandleKeys();
    ImGui::End();
}

void ExocetView::DrawToolbar()
{
    const auto& b = m_session->Block();
    ToolbarFlow bar(ImGui::GetFrameHeight());

    // the edits
    bar.Begin("##tb_edit");
    ImGui::BeginDisabled(!m_session->CanUndo());
    if (ImGui::Button(ICON_FA_ROTATE_LEFT "##undo"))
        Undo(false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Undo (Ctrl+Z)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_session->CanRedo());
    if (ImGui::Button(ICON_FA_ROTATE_RIGHT "##redo"))
        Undo(true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Redo (Ctrl+Y, Ctrl+Shift+Z)");
    bar.End();

    // into the running demo, without going to the Exocet window
    bar.Begin("##tb_push");
    exo::PushButton(*m_session);
    bar.End();

    bar.Begin("##tb_sample");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    const auto& samples = m_session->Check().samples;
    std::string current = "Sample: none";
    for (const auto& s : samples)
        if (s.index == m_filterSample)
            current = fmt::format("{:02X} {}", s.index, s.name);
    if (ImGui::BeginCombo("##sample", current.c_str())) {
        if (ImGui::Selectable("none", m_filterSample == 0))
            m_filterSample = 0;
        for (const auto& s : samples)
            if (s.bytes && ImGui::Selectable(fmt::format("{:02X} {}", s.index, s.name).c_str(),
                                             m_filterSample == s.index))
                m_filterSample = s.index;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Highlight every row that triggers a sample");
    ImGui::SameLine();
    {
        const auto* pack = m_session->Pack();
        std::string why;
        if (!m_filterSample)
            why = "Pick a sample first";
        else if (m_selTrack < 0 || m_selRow < 0 || !pack)
            why = "Select the key to copy, on a row that triggers the sample";
        ImGui::BeginDisabled(!why.empty());
        if (ImGui::Button(ICON_FA_TABLE_CELLS " Key every hit")) {
            m_editError.clear();
            const int n = m_session->KeyEveryHit(pack->Tracks()[m_selTrack].name, m_filterSample,
                                                 m_selRow, &m_editError);
            if (n && m_editError.empty())
                m_editError = fmt::format("{} keys set", n);
        }
        ImGui::EndDisabled();
        TooltipWhenDisabled(why.empty() ? "Copy the selected key - and the keys after it, up to "
                                          "the sample's next hit - onto every hit of the sample"
                                        : why);
    }
    bar.End();

    // the live block
    ToolbarFlow status(ImGui::GetTextLineHeight());
    status.Begin("##st_live");
    if (b.present) {
        ImGui::TextColored(m_session->ManifestMatches() ? kOkColour : kBlockColour,
                           ICON_FA_CIRCLE_DOT " Live");
        ImGui::SameLine();
        ImGui::Text("row %u +%.2f  frame %u", b.row, b.frac / 65536.0, b.frame);
        const auto parts = Parts();
        const int part = m_session->ManifestMatches()
                             ? PartOf(parts, b.row, TickOfFrac(b.frac, m_session->RowTicks(b.row)))
                             : -1;
        if (part >= 0) {
            ImGui::SameLine();
            ImGui::Text(" in %s", parts[part].name.c_str());
        }
    } else {
        ImGui::TextDisabled(ICON_FA_CIRCLE_DOT " No Exocet block - the live parts are idle");
    }
    status.End();
    if (b.present) {
        status.Begin("##st_clock");
        ImGui::Text("%s  generation %u/%u", b.clock ? "frame clock" : "music clock", b.generation,
                    b.seen);
        if (b.clock && ImGui::IsItemHovered())
            ImGui::SetTooltip("The fallback frame clock: the music is off, or it is another song");
        status.End();
        if (!m_session->ManifestMatches()) {
            status.Begin("##st_manifest");
            ImGui::TextColored(kBlockColour, "manifest 0x%08X, not the .exocet's", b.manifest);
            status.End();
        }
    }
    status.Message(m_session->LastPush());
    // Ctrl+S saves from here too
    if (!m_session->SaveError().empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kBlockColour, "Save failed: %s", m_session->SaveError().c_str());
        ImGui::PopTextWrapPos();
    }
}

void ExocetView::DrawBreakGutter(int row)
{
    // a debugger's gutter: a click sets or clears the row's breakpoint, a red dot marks one; a
    // right-click sets the row's bookmark, a blue tab with its slot, or edits the one there. A
    // slot holds either; two, the breakpoint's then the bookmark's, while a row has both
    const float h = ImGui::GetTextLineHeight();
    const int slots = m_wideGutter ? 2 : 1;
    ImGui::PushID(row);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##break", ImVec2(h * static_cast<float>(slots), h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool set = m_session->RowBreak(row);
    const auto* mark = m_session->BookmarkAt(row);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        m_session->ToggleRowBreak(row);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        if (mark) {
            m_bookmarkEditRow = row;
            m_openBookmarkEdit = true;
        } else if (std::string error; !m_session->ToggleBookmark(row, &error)) {
            m_editError = error;
        }
        mark = m_session->BookmarkAt(row);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // a key breakpoint (set over MCP or the console's EXO BR KEY) stops here too: a ring
    const int keyBreak = set ? -1 : m_session->KeyBreakTrack(row);
    const bool breakShown = set || keyBreak >= 0;
    const ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
    if (set)
        dl->AddCircleFilled(c, h * 0.32f, IM_COL32(230, 60, 60, 255));
    else if (keyBreak >= 0)
        dl->AddCircle(c, h * 0.32f, IM_COL32(230, 60, 60, 255), 0, 2.0f);
    else if (hovered && !(mark && slots == 1))
        dl->AddCircle(c, h * 0.32f, IM_COL32(230, 60, 60, 160));
    // the bookmark in its own slot when there are two, else where no breakpoint shows
    if (mark && (slots == 2 || !breakShown)) {
        const float x = p.x + (slots == 2 ? h : 0.0f);
        dl->AddRectFilled(ImVec2(x + 1, p.y + 1), ImVec2(x + h - 1, p.y + h - 1), kBookmarkColour,
                          h * 0.2f);
        if (mark->slot) {
            const std::string digit = std::to_string(mark->slot);
            const float size = ImGui::GetFontSize() * 0.85f;
            const ImVec2 t = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, digit.c_str());
            dl->AddText(ImGui::GetFont(), size,
                        ImVec2(x + (h - t.x) * 0.5f, p.y + (h - t.y) * 0.5f),
                        IM_COL32(16, 24, 40, 255), digit.c_str());
        }
    }
    if (hovered) {
        std::string tip;
        if (set)
            tip = "Breakpoint: the emulator pauses as the demo starts this row. Click (or F9) to "
                  "clear it.";
        else if (keyBreak >= 0 && m_session->Pack())
            tip = fmt::format("Key breakpoint on {}: the emulator pauses as the demo starts this "
                              "row. Click (or F9) to break on the row itself.",
                              m_session->Pack()->Tracks()[keyBreak].name);
        else
            tip = "Click (or F9 on the selected row) to pause the emulator as the demo starts "
                  "this row.";
        if (mark)
            tip += fmt::format("\nBookmark{}{}: right-click to name, renumber or remove it.",
                               mark->slot ? fmt::format(" {} (Ctrl+{})", mark->slot, mark->slot)
                                          : std::string(),
                               mark->name.empty() ? std::string() : " \"" + mark->name + "\"");
        else
            tip += "\nRight-click (or M) to bookmark it: Ctrl+Up/Down go between bookmarks, "
                   "Ctrl+1..9 to one.";
        ImGui::SetTooltip("%s", tip.c_str());
    }
    ImGui::PopID();
}

void ExocetView::DrawBookmarkPopup()
{
    if (m_openBookmarkEdit) {
        m_openBookmarkEdit = false;
        if (const auto* mark = m_session->BookmarkAt(m_bookmarkEditRow)) {
            std::snprintf(m_bookmarkName, sizeof m_bookmarkName, "%s", mark->name.c_str());
            ImGui::OpenPopup("##bookmark_edit");
        }
    }
    if (!ImGui::BeginPopup("##bookmark_edit"))
        return;
    const auto* mark = m_session->BookmarkAt(m_bookmarkEditRow);
    if (!mark) { // undone from under it
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const auto& check = m_session->Check();
    ImGui::TextDisabled(
        "Bookmark at %s",
        core::ExocetPlaceLabel(check.rows[m_bookmarkEditRow], check.digests.size() > 1).c_str());
    int slot = mark->slot;
    const std::string name = mark->name;
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const bool enter = ImGui::InputText("Name", m_bookmarkName, sizeof m_bookmarkName,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemDeactivatedAfterEdit() && name != m_bookmarkName)
        m_session->SetBookmark(m_bookmarkEditRow, slot, m_bookmarkName);
    static const char* kSlots[] = {"none", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    if (ImGui::Combo("Slot (Ctrl+n)", &slot, kSlots, IM_ARRAYSIZE(kSlots)))
        m_session->SetBookmark(m_bookmarkEditRow, slot, m_bookmarkName);
    if (ImGui::Button("Remove")) {
        m_session->RemoveBookmark(m_bookmarkEditRow);
        ImGui::CloseCurrentPopup();
    }
    if (enter)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void ExocetView::GoToBookmark(int row)
{
    LeaveCell();
    if (!ImGui::GetIO().KeyShift) {
        m_anchorRow = -1;
    } else if (m_anchorRow < 0) {
        m_anchorRow = m_selRow;
        m_anchorTick = m_selTick;
        m_anchorTrack = m_selTrack;
    }
    m_selRow = row;
    m_selTick = 0;
    m_scrollToRow = row;
    m_scrollToTick = 0;
    m_follow = false;
    if (const auto* mark = m_session->BookmarkAt(row)) {
        const auto& check = m_session->Check();
        m_editError = fmt::format(
            "Bookmark{}{} at {}", mark->slot ? fmt::format(" {}", mark->slot) : std::string(),
            mark->name.empty() ? std::string() : " \"" + mark->name + "\"",
            core::ExocetPlaceLabel(check.rows[row], check.digests.size() > 1));
    }
}

bool ExocetView::SeekableFrame(uint32_t songFrame, uint64_t& emuFrame) const
{
    return m_haveRange && m_session->EmuFrameOf(songFrame, emuFrame) && emuFrame >= m_rangeFirst &&
           emuFrame <= m_rangeLast;
}

void ExocetView::DrawRecordedBar(uint32_t songFrame)
{
    // in the cell's left padding, the full height of the line, so the held rows read as one bar
    uint64_t emu = 0;
    if (!SeekableFrame(songFrame, emu))
        return;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 pad = ImGui::GetStyle().CellPadding;
    const float x = p.x - pad.x * 0.5f - 1;
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(x, p.y - pad.y), ImVec2(x + 2, p.y + ImGui::GetTextLineHeight() + pad.y),
        ImGui::GetColorU32(kInfoColour));
}

void ExocetView::DrawSeekCell(const std::string& text, uint32_t songFrame)
{
    uint64_t emu = 0;
    if (!SeekableFrame(songFrame, emu)) {
        ImGui::TextDisabled("%s", text.c_str());
        return;
    }
    ImGui::TextColored(kInfoColour, "%s", text.c_str());
    if (!ImGui::IsItemHovered())
        return;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetTooltip("In the frame history: click to rewind to it (machine frame %llu)",
                      static_cast<unsigned long long>(emu));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && m_onSeekFrame)
        m_onSeekFrame(emu);
}

void ExocetView::DrawGridOptions()
{
    ImGui::Checkbox("Follow", &m_follow);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scroll the grid with the playhead");
    ImGui::SameLine();
    ImGui::Checkbox("Channels", &m_showChannels);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show the song's channels beside the tracks");
    ImGui::SameLine();
    ImGui::Checkbox("Auto groups", &m_autoGroups);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show a group's tracks only while the demo has the group enabled at "
                          "the playhead (or, while it runs and the grid is not following it, the "
                          "middle of the grid); fold it to one column otherwise. Paused, the "
                          "groups stay as they are at the playhead however the grid is scrolled. "
                          "Per group: right-click its header.");
    ImGui::SameLine();
    ImGui::Checkbox("Active groups", &m_activeGroups);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show only the groups the demo has enabled at the playhead (or, while "
                          "it runs and the grid is not following it, the middle of the grid); "
                          "the others leave the grid, pinned or not");
    ImGui::SameLine();
    const size_t breaks = m_session->BreakCount();
    ImGui::BeginDisabled(breaks == 0);
    if (ImGui::Button(fmt::format("Clear breakpoints ({})", breaks).c_str()))
        m_session->ClearBreaks();
    ImGui::EndDisabled();
    TooltipWhenDisabled(breaks ? "" : "No breakpoints: click a row's left edge to set one");

    // what this edit has changed, as the cells' corners mark it
    ImGui::SameLine();
    const auto n = m_session->ChangeCounts();
    ImGui::BeginGroup();
    if (n[0] + n[1] + n[2] == 0) {
        ImGui::TextDisabled("No changes since the save");
    } else {
        static const char* kKinds[] = {"new", "changed", "removed"};
        ImGui::TextUnformatted("Since the save:");
        for (int k = 0; k < 3; ++k) {
            ImGui::SameLine();
            if (n[k])
                ImGui::TextColored(kChangeColours[k], "%d %s", n[k], kKinds[k]);
            else
                ImGui::TextDisabled("0 %s", kKinds[k]);
        }
    }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keys added, changed and removed since the .exocet was last saved: a "
                          "corner of their cells marks them, green, amber and red. Saving "
                          "(Ctrl+S) clears the marks; undoing back to the save does too.");
}

void ExocetView::DrawGrid()
{
    auto* pack = m_session->Pack();
    const auto& check = m_session->Check();
    const auto& b = m_session->Block();
    const std::vector<GridColumn> columns = Columns();
    // the song's channels, and the music group's columns: one a channel, or one folded
    const int voices = m_showChannels ? check.channels : 0;
    const int channels = voices && m_musicFolded ? 1 : voices;
    const auto& activity = m_session->Activity();
    const int nrows = check.valid ? static_cast<int>(check.rows.size()) : 0;
    // the parts, by the director's cuts: a column naming them, a rule where each starts
    const auto* director = pack ? pack->FindTrack(pack->Director()) : nullptr;
    const int lead = kLead + (director ? 1 : 0);

    BuildLines();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        m_dragging = false;
    const bool range = HasRange();
    const auto [rangeLo, rangeHi] = RangeLines();
    const std::vector<int> rangeTracks = RangeTracks();
    const std::vector<Part> parts = Parts();
    const int selPart = m_selTrack >= 0 && m_selRow >= 0 && m_selRow < nrows
                            ? PartOf(parts, m_selRow, m_selTick)
                            : -1;
    m_haveRange = m_recordedRange && m_recordedRange(m_rangeFirst, m_rangeLast);
    if (m_follow && b.present && m_session->ManifestMatches() &&
        static_cast<int>(b.row) != m_lastPlayRow) {
        m_lastPlayRow = b.row;
        m_scrollToRow = b.row;
        m_scrollToTick = 0;
    }
    // the playhead's line: its row's, or the tick's in an expanded row
    int playLine = -1;
    if (b.present && m_session->ManifestMatches() && static_cast<int>(b.row) < nrows)
        playLine = LineOf(b.row, TickOfFrac(b.frac, m_session->RowTicks(b.row)));
    const int nlines = static_cast<int>(m_lines.size());

    const bool live = b.present && m_session->ManifestMatches();
    const int cols = lead + channels + std::max<int>(1, static_cast<int>(columns.size()));
    // A table per set of columns, so a column's width is never another's; the scroll carried
    // the gutter holds a breakpoint or a bookmark; two slots wide while a row has both
    m_wideGutter = false;
    for (const auto& [row, mark] : m_session->BookmarkRows())
        m_wideGutter |= m_session->RowBreak(row);
    uint64_t sig = 1469598103934665603ull ^ static_cast<uint64_t>(channels) ^
                   (static_cast<uint64_t>(lead) << 32) ^ (m_musicFolded ? 1ull << 40 : 0) ^
                   (m_wideGutter ? 1ull << 41 : 0);
    for (const auto& c : columns)
        sig = (sig ^ static_cast<uint64_t>((c.track + 1) * 4096 + c.group + 1)) * 1099511628211ull;
    if (sig != m_gridColumns) {
        if (m_gridColumns) {
            ImGui::SetNextWindowScroll(ImVec2(0, m_gridScrollY));
            m_restoreScrollX = 3; // a group folded or opened: the grid stays where it was
        }
        m_gridColumns = sig;
    }
    ImGui::BeginDisabled(!check.valid);
    if (ImGui::BeginTable(fmt::format("##exocet_grid_{:x}", sig).c_str(), cols,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings,
                          ImVec2(0, 0))) {
        // the table's own scrolling window is the one focused while the grid is clicked in
        ImGui::GetCurrentWindow()->Flags |= kOwnKeys;
        ImGui::TableSetupScrollFreeze(lead + (m_freezeMusic ? channels : 0), 2);
        const float pad = ImGui::GetStyle().CellPadding.x * 2 + 2;
        // the breakpoint and bookmark gutter, then position:row
        ImGui::TableSetupColumn("Row", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetTextLineHeight() * (m_wideGutter ? 2 : 1) + 2 +
                                    ImGui::CalcTextSize("00:00.0").x + pad);
        ImGui::TableSetupColumn(
            "Pat", ImGuiTableColumnFlags_WidthFixed,
            std::max(ImGui::CalcTextSize("Pat").x, ImGui::CalcTextSize("00").x) + pad);
        ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("00000").x + pad);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("00:00.00").x + pad);
        if (director) {
            float w = ImGui::CalcTextSize("Part").x;
            for (const auto& p : parts)
                w = std::max(w, ImGui::CalcTextSize(p.name.c_str()).x);
            ImGui::TableSetupColumn("Part", ImGuiTableColumnFlags_WidthFixed,
                                    std::min(w, ImGui::CalcTextSize("MMMMMMMMMMMM").x) +
                                        ImGui::GetFontSize() * 0.5f + pad);
        }
        if (channels && m_musicFolded) {
            const std::string label = fmt::format("{} ch", voices);
            ImGui::TableSetupColumn(
                label.c_str(), ImGuiTableColumnFlags_WidthFixed,
                std::max(ImGui::CalcTextSize(label.c_str()).x, MusicBarWidth()) + pad);
        } else {
            for (int c = 0; c < channels; ++c)
                ImGui::TableSetupColumn(fmt::format("Ch {}", c + 1).c_str(),
                                        ImGuiTableColumnFlags_WidthFixed,
                                        ImGui::CalcTextSize("C-3 05 A0F").x + pad);
        }
        if (columns.empty())
            ImGui::TableSetupColumn(pack ? "(no tracks shown)" : "(no .exocet)",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("(no tracks shown)").x + pad);
        for (const auto& col : columns) {
            if (col.track >= 0) {
                const char* name = pack->Tracks()[col.track].name.c_str();
                ImGui::TableSetupColumn(
                    name, ImGuiTableColumnFlags_WidthFixed,
                    std::max(ImGui::CalcTextSize("-0000.000 ~").x, ImGui::CalcTextSize(name).x) +
                        pad);
            } else {
                // a folded group: its name above (the group row), how many tracks below
                const auto& name = pack->Groups()[col.group].name;
                int n = 0;
                for (const auto& t : pack->Tracks())
                    n += t.group == name;
                const std::string label = fmt::format("{} track{}", n, n == 1 ? "" : "s");
                const std::string above = std::string(ICON_FA_CHEVRON_RIGHT " ") + name + " *";
                ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed,
                                        std::max(ImGui::CalcTextSize(label.c_str()).x,
                                                 ImGui::CalcTextSize(above.c_str()).x) +
                                            pad);
            }
        }

        // The group row: each group's columns under one bar in its colour, its name on the
        // first, a click anywhere on it folding or opening the group
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < cols; ++c) {
            if (!ImGui::TableSetColumnIndex(c))
                continue;
            const int k = c - lead - channels;
            ImGui::PushID(c);
            if (c >= lead && k < 0) {
                DrawMusicGroupCell(c - lead, channels);
                ImGui::PopID();
                continue;
            }
            if (k < 0 || k >= static_cast<int>(columns.size())) {
                ImGui::TableHeader("##group_row");
                ImGui::PopID();
                continue;
            }
            const auto& col = columns[k];
            const auto& group = pack->Groups()[col.group];
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, GroupColour(group.name));
            const bool first = k == 0 || columns[k - 1].group != col.group;
            std::string label = "##group";
            if (first) {
                label = std::string(col.track >= 0 ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT) +
                        " " + group.name + (ModeOf(group.name) != GroupMode::Auto ? " *" : "") +
                        "##group";
            }
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(20, 20, 24, 255));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(255, 255, 255, 60));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(255, 255, 255, 90));
            if (ImGui::Selectable(label.c_str(), false) && !Dragged())
                ToggleGroup(group.name);
            ImGui::PopStyleColor(3);
            GroupDragDrop(group.name);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                m_groupMenu = group.name;
                m_trackMenu = -1;
                m_openGroupMenu = true;
            }
            if (ImGui::IsItemHovered()) {
                const int n = activity.ActiveRows(group.name);
                const std::string when =
                    !activity.SeenRows()
                        ? "Not seen running yet."
                        : fmt::format("Enabled on {} of the {} rows seen.", n, activity.SeenRows());
                const char* mode = ModeOf(group.name) == GroupMode::Auto
                                       ? "Auto: open while the demo runs it."
                                       : "* pinned: right-click, Auto to follow the demo again.";
                ImGui::SetTooltip("%s: %s\n%s\n%s\nClick to %s it, drag to move it.",
                                  group.name.c_str(), group.text.c_str(), when.c_str(), mode,
                                  col.track >= 0 ? "fold" : "open");
            }
            ImGui::PopID();
        }

        // the tracks' names, by hand, so a track's description is the tooltip of its header only
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < cols; ++c) {
            if (!ImGui::TableSetColumnIndex(c))
                continue;
            ImGui::TableHeader(ImGui::TableGetColumnName(c));
            if (m_musicFolded && c == lead && channels) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                       (kMusicColour & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 150));
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    m_musicFolded = false;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The song's %d channels, folded: a mark a channel, lit where "
                                      "it triggers a note. Click to open them.",
                                      voices);
            } else if (c >= lead && c < lead + channels && c - lead < 4) {
                // the channel's light: a trigger flashes it, the level holds a glow under it
                const auto& l = m_session->Lights()[c - lead];
                const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                const float x = r1.x - (r1.y - r0.y) * 0.5f - 2;
                const float y = (r0.y + r1.y) * 0.5f;
                const float rad = (r1.y - r0.y) * 0.3f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddCircleFilled(ImVec2(x, y), rad, IM_COL32(40, 50, 40, 255));
                const float a = std::max(l.trigger, l.level * 0.6f);
                if (a > 0.02f)
                    dl->AddCircleFilled(ImVec2(x, y), rad,
                                        IM_COL32(90, 255, 120, static_cast<int>(255 * a)));
            }
            if (c == kPatColumn && ImGui::IsItemHovered())
                ImGui::SetTooltip("The pattern each row's position plays, by the module's numbers: "
                                  "bright where the position starts, -- in the kit's lead. The "
                                  "Exocet Music window has the whole order list.");
            if (director && c == kLead && ImGui::IsItemHovered())
                ImGui::SetTooltip("The demo's parts, by the director's cuts (%s): each from its "
                                  "key to the next, a rule across the grid where it starts. The "
                                  "selected cell's part is lit.",
                                  director->name.c_str());
            const int k = c - lead - channels;
            if (k < 0 || k >= static_cast<int>(columns.size()))
                continue;
            const auto& col = columns[k];
            const auto& group = pack->Groups()[col.group];
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, GroupColour(group.name, 150));
            if (col.track < 0 && ImGui::IsItemClicked(ImGuiMouseButton_Left))
                ToggleGroup(group.name);
            // on the release, as the table opens its own header menu (column sizing): ours,
            // opened after the table ends, then takes its place
            if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                m_groupMenu = group.name;
                m_trackMenu = col.track;
                m_openGroupMenu = true;
            }
            if (!ImGui::IsItemHovered())
                continue;
            const int n = activity.ActiveRows(group.name);
            const std::string when =
                !activity.SeenRows()
                    ? "Not seen running yet."
                    : fmt::format("Enabled on {} of the {} rows seen.", n, activity.SeenRows());
            if (col.track < 0) {
                ImGui::SetTooltip("%s: %s\n%s\nFolded: click to open it, right-click for the "
                                  "group's options.",
                                  group.name.c_str(), group.text.c_str(), when.c_str());
            } else {
                const auto& t = pack->Tracks()[col.track];
                ImGui::SetTooltip("%s (%s, %g..%g, default %g)\n%s\n\n%s: %s\n%s Right-click "
                                  "for the group's options, or to clear the track.",
                                  t.name.c_str(), t.type.c_str(), t.min, t.max, t.def,
                                  t.text.c_str(), group.name.c_str(), group.text.c_str(),
                                  when.c_str());
            }
        }

        const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2;
        if (m_scrollToRow >= 0 && nrows) {
            const float top = ImGui::GetScrollY();
            const float view = ImGui::GetWindowHeight() - rowH * 2;
            const float y = LineOf(m_scrollToRow, m_scrollToTick) * rowH;
            if (y < top || y > top + view)
                ImGui::SetScrollY(std::max(0.0f, y - view * 0.4f));
            m_scrollToRow = -1;
        }

        m_gridScrollY = ImGui::GetScrollY();
        if (m_restoreScrollX > 0) {
            --m_restoreScrollX;
            ImGui::SetScrollX(m_gridScrollX); // clamped to the new width once it is known
        } else {
            m_gridScrollX = ImGui::GetScrollX();
        }
        if (nlines)
            m_viewRow = m_lines[std::clamp(static_cast<int>((ImGui::GetScrollY() +
                                                             ImGui::GetWindowHeight() * 0.5f) /
                                                            rowH) -
                                               2,
                                           0, nlines - 1)]
                            .first;

        // the first line clear of the frozen header rows: where a part scrolled into is named
        const int topLine =
            std::clamp(static_cast<int>(std::ceil(ImGui::GetScrollY() / rowH - 0.25f)), 0,
                       std::max(0, nlines - 1));

        ImGuiListClipper clipper;
        clipper.Begin(nlines, rowH);
        while (clipper.Step()) {
            for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
                // a line is a row, or a tick of an expanded row past its first
                const int i = m_lines[line].first, tk = m_lines[line].second;
                const auto& r = check.rows[i];
                const int ticks = m_session->RowTicks(i);
                const bool open = line + 1 < nlines && m_lines[line + 1].first == i;
                const int part = PartOf(parts, i, tk);
                const bool partStarts =
                    part >= 0 && (line == 0 || PartOf(parts, m_lines[line - 1].first,
                                                      m_lines[line - 1].second) != part);
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
                // the row's breakpoint, on its line (a tick's line is under it)
                const bool brk = !tk && m_session->RowBreak(i);
                auto breakTint = [&] {
                    if (brk)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, kBreakTint);
                };
                bool hit = false;
                if (m_filterSample && !tk)
                    for (const auto& c : r.cells)
                        hit |= c[0] == m_filterSample;
                if (live && playLine == line)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                           ImGui::GetColorU32(kPlayTint));
                else if (hit)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(kHitTint));
                else if (r.row == 0 && !tk)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                           ImGui::GetColorU32(kPatternTint));
                // the cursor's line, over whichever of those it has
                if (m_selTrack >= 0 && m_selRow == i && m_selTick == tk)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                           ImGui::GetColorU32(kSelRowTint));

                ImGui::TableNextColumn();
                if (tk) {
                    // a tick: its number under the row's, its frame and time
                    DrawRecordedBar(r.frame + tk);
                    ImGui::Dummy(ImVec2(ImGui::GetTextLineHeight(), 1));
                    ImGui::SameLine(0, 2);
                    ImGui::TextDisabled("   .%d", tk);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("tick %d of %d of played row %d, frame %u", tk, ticks, i,
                                          r.frame + tk);
                    ImGui::TableNextColumn(); // the pattern is on the row's line
                    ImGui::TableNextColumn();
                    DrawSeekCell(std::to_string(r.frame + tk), r.frame + tk);
                    ImGui::TableNextColumn();
                    DrawSeekCell(Time(r.frame + tk), r.frame + tk);
                } else {
                    breakTint();
                    DrawRecordedBar(r.frame);
                    DrawBreakGutter(i);
                    ImGui::SameLine(0, 2);
                    const std::string label =
                        core::ExocetPlaceLabel(r, m_session->Check().digests.size() > 1);
                    // a folded row holding keys on its ticks shows it in the key colour
                    bool tickedFolded = false;
                    if (!open && ticks > 1)
                        for (const auto& t : pack->Tracks())
                            for (const auto& k : m_session->HostKeys(t.index))
                                tickedFolded |= k.row == i && k.sub;
                    if (tickedFolded)
                        ImGui::TextColored(kKeyColour, "%s", label.c_str());
                    else if (r.row % 4 == 0)
                        ImGui::TextUnformatted(label.c_str());
                    else
                        ImGui::TextDisabled("%s", label.c_str());
                    if (ticks > 1 && ImGui::IsItemClicked(ImGuiMouseButton_Left))
                        ToggleExpand(i);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "song %u position %d (pattern %u) row %u%s\nplayed row %d, "
                            "frame %u, %u frames%s",
                            r.song, r.pos, r.pattern, r.row,
                            r.pass ? fmt::format(", pass {}", r.pass).c_str() : "", i, r.frame,
                            r.frames,
                            ticks < 2      ? ""
                            : open         ? "\nclick (or T) to fold its ticks"
                            : tickedFolded ? "\nkeys on its ticks: click (or T) to show them"
                                           : "\nclick (or T) for a line a tick");
                    // the pattern its position plays, bright where the position starts
                    ImGui::TableNextColumn();
                    breakTint();
                    const bool posStarts = i == 0 || check.rows[i - 1].song != r.song ||
                                           check.rows[i - 1].pos != r.pos;
                    if (r.pos < 0)
                        ImGui::TextDisabled("--");
                    else if (posStarts)
                        ImGui::Text("%u", r.pattern);
                    else
                        ImGui::TextDisabled("%u", r.pattern);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "%s", exo::PositionTip(check, r.song, r.pos, m_session->Fps()).c_str());
                    ImGui::TableNextColumn();
                    breakTint();
                    DrawSeekCell(std::to_string(r.frame), r.frame);
                    ImGui::TableNextColumn();
                    breakTint();
                    DrawSeekCell(Time(r.frame), r.frame);
                }
                // the part: named on its first line, and on the top line so the part scrolled
                // into is always named; lit down its length where the selected cell is
                if (director) {
                    ImGui::TableNextColumn();
                    if (part >= 0) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                               part == selPart ? kPartMine : kPartWash[part % 2]);
                        if (partStarts || line == topLine) {
                            const Part& p = parts[part];
                            if (partStarts)
                                ImGui::TextUnformatted(p.name.c_str());
                            else
                                ImGui::TextDisabled("%s", p.name.c_str());
                            if (ImGui::IsItemHovered()) {
                                const bool songs = check.digests.size() > 1;
                                const std::string to =
                                    part + 1 < static_cast<int>(parts.size())
                                        ? core::ExocetPlaceLabel(check.rows[parts[part + 1].row],
                                                                 songs)
                                        : std::string("the song's end");
                                ImGui::SetTooltip(
                                    "%s: from its cut at %s to %s", p.name.c_str(),
                                    core::ExocetPlaceLabel(check.rows[p.row], songs).c_str(),
                                    to.c_str());
                            }
                        }
                    }
                }
                if (m_musicFolded && channels) {
                    ImGui::TableNextColumn();
                    if (!tk)
                        DrawMusicMarks(r.cells, voices, m_filterSample);
                }
                for (int c = 0; c < (m_musicFolded ? 0 : channels); ++c) {
                    ImGui::TableNextColumn();
                    if (tk) // a tick's line: the row's music is on the row's
                        continue;
                    if (c < static_cast<int>(r.cells.size()) &&
                        (r.cells[c][0] || r.cells[c][1] || r.cells[c][2] || r.cells[c][3])) {
                        if (m_filterSample && r.cells[c][0] == m_filterSample)
                            ImGui::TextColored(kKeyColour, "%s", Cell(r.cells[c]).c_str());
                        else
                            ImGui::TextUnformatted(Cell(r.cells[c]).c_str());
                    } else {
                        ImGui::TextDisabled("--- .. ...");
                    }
                }
                const uint8_t sub = core::ExocetSub(static_cast<uint32_t>(tk), ticks);
                const uint16_t frac = core::ExocetTickFrac(static_cast<uint32_t>(tk), ticks);
                if (columns.empty())
                    ImGui::TableNextColumn();
                for (const auto& col : columns) {
                    ImGui::TableNextColumn();
                    const std::string& groupName = pack->Groups()[col.group].name;
                    const auto state = activity.At(groupName, i);
                    if (col.track < 0) {
                        DrawFoldedCell(col.group, i, tk, state);
                        continue;
                    }
                    const int t = col.track;
                    const auto& info = pack->Tracks()[t];
                    const bool selected = m_selRow == i && m_selTick == tk && m_selTrack == t;
                    const ImRect cellRect = CellHere();
                    if (selected)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                               ImGui::GetColorU32(kSelTint));
                    else if (range && line >= rangeLo && line <= rangeHi &&
                             std::find(rangeTracks.begin(), rangeTracks.end(), t) !=
                                 rangeTracks.end())
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                               ImGui::GetColorU32(kRangeTint));
                    else if (state == core::ExocetActivity::State::Inactive)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(0, 0, 0, 70));
                    else
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                               GroupColour(groupName, tk ? 12 : 22));
                    const auto& keys = m_session->HostKeys(t);
                    const auto k =
                        std::find_if(keys.begin(), keys.end(), [&](const core::ExocetKey& x) {
                            return x.row == i && x.sub == sub;
                        });
                    std::string text;
                    ImVec4 colour = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                    const bool isColour = TypeOf(info) == core::ExocetType::Color;
                    bool swatch = isColour;
                    int32_t swatchValue = 0;
                    if (selected && m_editing) {
                        text = std::string(m_edit) + "_";
                        colour = kEditColour;
                        uint32_t typed = 0;
                        swatch = isColour && ParseTypedColour(m_edit, &typed);
                        swatchValue = static_cast<int32_t>(typed);
                    } else if (k != keys.end()) {
                        text = FormatValue(info, k->value) + " " + Glyph(k->interp);
                        colour = kKeyColour;
                        swatchValue = k->value;
                    } else if (TypeOf(info) == core::ExocetType::Event) {
                        text = ".";
                    } else {
                        swatchValue = m_session->HostValue(t, i, frac);
                        text = FormatValue(info, swatchValue);
                    }
                    if (isColour)
                        text = kSwatchRoom + text;
                    ImGui::PushStyleColor(ImGuiCol_Text, colour);
                    ImGui::PushID(i * 256 + t);
                    ImGui::PushID(tk);
                    // No AllowOverlap: it defers hover by a frame, and a click that arrives with
                    // the pointer (an injected one, or a quick real one) then misses the cell.
                    // clicked off the cell being typed into, the value is keyed there first
                    // a press puts the cursor here - with Shift, a range from where it was - and
                    // a drag takes it along, down and across the tracks, the range from the press
                    ImGui::Selectable(text.c_str(), false);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        const bool extend =
                            ImGui::GetIO().KeyShift && m_selTrack >= 0 && m_selRow >= 0;
                        if (!selected) {
                            m_editError.clear();
                            LeaveCell();
                        }
                        if (!extend) {
                            m_anchorRow = i;
                            m_anchorTick = tk;
                            m_anchorTrack = t;
                        } else if (m_anchorRow < 0) {
                            m_anchorRow = m_selRow;
                            m_anchorTick = m_selTick;
                            m_anchorTrack = m_selTrack;
                        }
                        m_selRow = i;
                        m_selTick = tk;
                        m_selTrack = t;
                        m_dragging = true;
                    } else if (m_dragging && !selected &&
                               ImGui::IsItemHovered(
                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                        LeaveCell();
                        m_selRow = i;
                        m_selTick = tk;
                        m_selTrack = t;
                    }
                    // double-click keys the cell (or edits its key); right-click clears a key
                    if (ImGui::IsItemHovered() &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        KeyCell(i, tk, t);
                    if (k != keys.end() && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        m_editError.clear();
                        if (!selected)
                            LeaveCell();
                        m_editing = false;
                        m_anchorRow = -1;
                        m_selRow = i;
                        m_selTick = tk;
                        m_selTrack = t;
                        m_session->DeleteKey(info.name, i, tk, {}, &m_editError);
                    }
                    if (swatch)
                        DrawSwatch(swatchValue);
                    if (selected)
                        ImGui::GetWindowDrawList()->AddRect(
                            ImVec2(cellRect.Min.x + 1, cellRect.Min.y + 1),
                            ImVec2(cellRect.Max.x - 1, cellRect.Max.y - 1),
                            ImGui::GetColorU32(kSelFrame), 0.0f, 0, 2.0f);
                    // a key since the save: its corner, and what it was
                    const auto& changes = m_session->Changes(t);
                    if (const auto ch = changes.find({i, sub}); ch != changes.end()) {
                        const auto& c = ch->second;
                        DrawChangeMark(cellRect, c.kind);
                        if (ImGui::IsItemHovered()) {
                            const std::string was =
                                FormatValue(info, core::ExocetToBin(TypeOf(info), c.was)) + " " +
                                Glyph(static_cast<uint8_t>(
                                    std::max(0, core::ExocetInterpFromName(c.wasInterp))));
                            if (c.kind == ExocetSession::KeyChange::Added)
                                ImGui::SetTooltip("Keyed since the save");
                            else if (c.kind == ExocetSession::KeyChange::Changed)
                                ImGui::SetTooltip("Changed since the save: it was %s", was.c_str());
                            else
                                ImGui::SetTooltip("Removed since the save: a key of %s was here",
                                                  was.c_str());
                        }
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                    ImGui::PopStyleColor();
                }
                // a part's first line, and a breakpoint's row: a rule along its top, all the way
                // across - cell by cell, as the table clips each column to itself; the
                // breakpoint's red over the part's white where a row has both
                if (partStarts || brk)
                    for (int c = 0; c < cols; ++c)
                        if (ImGui::TableSetColumnIndex(c)) {
                            const ImRect cell = CellHere();
                            ImGui::GetWindowDrawList()->AddRectFilled(
                                cell.Min, ImVec2(cell.Max.x, cell.Min.y + 2.0f),
                                brk ? kBreakRule : kPartRule);
                        }
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    DrawBookmarkPopup();

    if (m_openGroupMenu) {
        ImGui::OpenPopup("##group_menu");
        m_openGroupMenu = false;
    }
    if (ImGui::BeginPopup("##group_menu")) {
        ImGui::TextUnformatted(m_groupMenu.c_str());
        DrawGroupModes(m_groupMenu);
        ImGui::Separator();
        // a hidden group has no header to right-click, so it comes back from here
        std::string show;
        for (const auto& [name, mode] : m_groupModes)
            if (mode == GroupMode::Hidden && ImGui::MenuItem(("Show " + name).c_str()))
                show = name;
        if (!show.empty())
            m_groupModes.erase(show);
        if (ImGui::MenuItem("Demo's order", nullptr, false, !m_groupOrder.empty()))
            m_groupOrder.clear();
        if (ImGui::MenuItem("Forget activity", nullptr, false, activity.SeenRows() > 0))
            m_session->ForgetActivity();
        // a track's name: every key on it, one undo step
        if (pack && m_trackMenu >= 0 && m_trackMenu < static_cast<int>(pack->Tracks().size())) {
            const auto& t = pack->Tracks()[m_trackMenu];
            const size_t n = m_session->HostKeys(m_trackMenu).size();
            const std::string why = m_session->EditBlockedReason();
            ImGui::Separator();
            if (ImGui::MenuItem(
                    fmt::format("Clear {} ({} key{})", t.name, n, n == 1 ? "" : "s").c_str(),
                    nullptr, false, n > 0 && why.empty())) {
                m_editing = false;
                m_editError.clear();
                m_session->ClearTrack(t.name, &m_editError);
            }
            if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", why.c_str());
        }
        ImGui::EndPopup();
    }
}

void ExocetView::DrawFoldedCell(int group, int row, int tick, core::ExocetActivity::State state)
{
    // a folded group is the part's own timeline: tinted where the demo had it enabled, and the
    // number of its tracks keyed on the row (the tick, in an expanded row)
    const auto* pack = m_session->Pack();
    const std::string& name = pack->Groups()[group].name;
    const ImRect cellRect = CellHere();
    ImGui::TableSetBgColor(
        ImGuiTableBgTarget_CellBg,
        GroupColour(name, state == core::ExocetActivity::State::Active ? 80 : 14));
    const uint8_t sub = core::ExocetSub(static_cast<uint32_t>(tick), m_session->RowTicks(row));
    auto here = [&](const core::ExocetKey& k) { return k.row == row && k.sub == sub; };
    int keyed = 0;
    int changed = -1; // a change since the save on one of its tracks here: its kind, the first
    for (const auto& t : pack->Tracks()) {
        if (t.group != name)
            continue;
        const auto& keys = m_session->HostKeys(t.index);
        keyed += std::any_of(keys.begin(), keys.end(), here);
        const auto& changes = m_session->Changes(t.index);
        if (const auto c = changes.find({row, sub}); changed < 0 && c != changes.end())
            changed = c->second.kind;
    }
    ImGui::PushID(row * 256 + 255 - group);
    ImGui::PushID(tick);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          keyed ? kKeyColour : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (ImGui::Selectable(keyed ? fmt::format("{}", keyed).c_str() : ".", false)) {
        // open it on this row, on its first track keyed here (else its first)
        m_editError.clear();
        LeaveCell();
        m_groupModes[name] = GroupMode::Open;
        m_anchorRow = -1;
        m_selRow = row;
        m_selTick = tick;
        int first = -1, keyedHere = -1;
        for (const auto& t : pack->Tracks()) {
            if (t.group != name)
                continue;
            if (first < 0)
                first = t.index;
            const auto& keys = m_session->HostKeys(t.index);
            if (std::any_of(keys.begin(), keys.end(), here)) {
                keyedHere = t.index;
                break;
            }
        }
        m_selTrack = keyedHere >= 0 ? keyedHere : first;
    }
    ImGui::PopStyleColor();
    if (changed >= 0)
        DrawChangeMark(cellRect, changed);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "%s: %s\n%d of its tracks keyed here.%s Click to open the group.", name.c_str(),
            state == core::ExocetActivity::State::Active     ? "enabled on this row"
            : state == core::ExocetActivity::State::Inactive ? "not enabled on this row"
                                                             : "row not seen running",
            keyed, changed >= 0 ? " Keys here changed since the save." : "");
    ImGui::PopID();
    ImGui::PopID();
}

void ExocetView::HandleKeys()
{
    auto* pack = m_session->Pack();
    if (!pack || !m_session->Check().valid)
        return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        m_session->Save();
        return;
    }
    if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Z) || ImGui::IsKeyPressed(ImGuiKey_Y))) {
        Undo(ImGui::IsKeyPressed(ImGuiKey_Y) || io.KeyShift);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !m_editing) {
        if (m_onTogglePause)
            m_onTogglePause();
        return;
    }
    const int nrows = static_cast<int>(m_session->Check().rows.size());
    const auto tracks = VisibleTracks();
    if (tracks.empty())
        return;
    // a selection whose group has folded stays - the detail below still shows it - until an
    // arrow moves it to a track that is shown
    if (nrows == 0)
        return;
    if (m_selTrack < 0)
        m_selTrack = tracks.front();
    if (m_selRow < 0) // the playhead's row, when the block is this build's
        m_selRow = m_session->ManifestMatches()
                       ? std::clamp(static_cast<int>(m_session->Block().row), 0, nrows - 1)
                       : 0;
    if (m_selTick >= m_session->RowTicks(m_selRow) ||
        LineOf(m_selRow, m_selTick) - LineOf(m_selRow, 0) != m_selTick)
        m_selTick = 0; // the row folded, or is shorter than the tick
    // moving off a cell keys what was typed into it (LeaveCell), so a value can be typed over,
    // the cursor moved and the next typed, Enter or not
    // Shift holds the range's other end where the cursor was; without it, the range goes
    auto extend = [&] {
        if (!io.KeyShift)
            m_anchorRow = -1;
        else if (m_anchorRow < 0) {
            m_anchorRow = m_selRow;
            m_anchorTick = m_selTick;
            m_anchorTrack = m_selTrack;
        }
    };
    auto move = [&](int d) {
        LeaveCell();
        extend();
        m_selRow = std::clamp(m_selRow + d, 0, nrows - 1);
        m_selTick = 0;
        m_scrollToRow = m_selRow;
        m_scrollToTick = 0;
        m_follow = false;
    };
    // up and down go a line: a tick in an expanded row
    auto step = [&](int d) {
        if (m_lines.empty()) {
            move(d);
            return;
        }
        LeaveCell();
        extend();
        const int line =
            std::clamp(LineOf(m_selRow, m_selTick) + d, 0, static_cast<int>(m_lines.size()) - 1);
        m_selRow = m_lines[line].first;
        m_selTick = m_lines[line].second;
        m_scrollToRow = m_selRow;
        m_scrollToTick = m_selTick;
        m_follow = false;
    };
    // Ctrl+Up/Down: the bookmark before or after the cursor's row; Ctrl+1..9: the one in that
    // slot. With Shift, the range taken there
    if (io.KeyCtrl &&
        (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))) {
        const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
        const auto& marks = m_session->BookmarkRows();
        int to = -1;
        if (up) {
            auto it = marks.lower_bound(m_selRow);
            if (it != marks.begin())
                to = std::prev(it)->first;
        } else if (auto it = marks.upper_bound(m_selRow); it != marks.end()) {
            to = it->first;
        }
        if (to >= 0)
            GoToBookmark(to);
        else
            m_editError = marks.empty() ? "No bookmarks: M bookmarks the selected row"
                          : up          ? "No bookmark above"
                                        : "No bookmark below";
        return;
    }
    if (io.KeyCtrl)
        for (int slot = 1; slot <= 9; ++slot)
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + slot), false)) {
                const int to = m_session->BookmarkSlotRow(slot);
                if (to >= 0)
                    GoToBookmark(to);
                else
                    m_editError = fmt::format("No bookmark {}", slot);
                return;
            }
    // M: the selected row's bookmark, on or off
    if (ImGui::IsKeyPressed(ImGuiKey_M, false) && !io.KeyCtrl && !m_editing) {
        std::string error;
        if (!m_session->ToggleBookmark(m_selRow, &error))
            m_editError = error;
        else if (const auto* mark = m_session->BookmarkAt(m_selRow))
            m_editError = mark->slot ? fmt::format("Bookmark {} set: Ctrl+{} comes back here",
                                                   mark->slot, mark->slot)
                                     : std::string("Bookmark set (slots 1-9 are all taken)");
        else
            m_editError = "Bookmark removed";
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        step(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        step(1);
    // T shows the selected row's ticks, a line each, or folds them
    if (ImGui::IsKeyPressed(ImGuiKey_T, false) && !io.KeyCtrl && !m_editing) {
        ToggleExpand(m_selRow);
        BuildLines();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
        move(-64);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
        move(64);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
        LeaveCell();
        extend(); // with Shift, the range takes in the tracks it moves across
        auto it = std::find(tracks.begin(), tracks.end(), m_selTrack);
        if (it != tracks.end()) {
            int k = static_cast<int>(it - tracks.begin()) + (left ? -1 : 1);
            m_selTrack = tracks[std::clamp(k, 0, static_cast<int>(tracks.size()) - 1)];
        } else {
            // from a folded group's track: the nearest shown one that way, in the grid's order
            std::vector<int> order;
            for (const auto& [g, ts] : GroupedTracks())
                order.insert(order.end(), ts.begin(), ts.end());
            const auto at = std::find(order.begin(), order.end(), m_selTrack) - order.begin();
            auto pos = [&](int t) {
                return std::find(order.begin(), order.end(), t) - order.begin();
            };
            int best = left ? tracks.front() : tracks.back();
            for (int t : tracks)
                if (left ? (pos(t) < at && pos(t) >= pos(best))
                         : (pos(t) > at && pos(t) <= pos(best)))
                    best = t;
            m_selTrack = best;
        }
    }

    const auto& info = pack->Tracks()[m_selTrack];
    const auto& r = m_session->Check().rows[m_selRow];
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        m_session->ToggleRowBreak(m_selRow);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_editing) {
        m_editError.clear();
        if (HasRange())
            DeleteRange();
        else
            m_session->DeleteKey(info.name, m_selRow, m_selTick, {}, &m_editError);
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        CopyCell();
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        PasteCell();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (!m_editing)
            m_anchorRow = -1;
        m_editing = false;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        if (m_editing)
            CommitEdit();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && m_editing) {
        m_editFresh = false; // editing the key's value, not replacing it
        const size_t n = std::strlen(m_edit);
        if (n)
            m_edit[n - 1] = 0;
        return;
    }
    // I cycles the interpolation of the key here: read as a key, not a character, so it works
    // whatever the keyboard layout sends for it (and when a key arrives without a character)
    if (ImGui::IsKeyPressed(ImGuiKey_I, false) && !io.KeyCtrl && !m_editing) {
        if (const auto* k = pack->FindKey(info.name, r.song, r.pos, r.row, r.pass, m_selTick)) {
            static const char* kOrder[] = {"step", "linear", "smooth", "ramp"};
            const int at = std::max(0, core::ExocetInterpFromName(k->interp));
            const double v = k->value;
            m_session->SetKey(info.name, m_selRow, v, kOrder[(at + 1) % 4], &m_editError, {},
                              m_selTick);
        }
    }
    const bool hex = TypeOf(info) == core::ExocetType::Color;
    for (ImWchar c : io.InputQueueCharacters) {
        const bool hexDigit = (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '#';
        if ((c >= '0' && c <= '9') || (!hex && (c == '-' || c == '.')) || (hex && hexDigit)) {
            if (!m_editing || m_editFresh) {
                m_editing = true;
                m_editFresh = false;
                m_edit[0] = 0;
                m_editError.clear();
            }
            const size_t n = std::strlen(m_edit);
            if (n + 1 < sizeof m_edit) {
                m_edit[n] = static_cast<char>(hex ? std::toupper(static_cast<int>(c)) : c);
                m_edit[n + 1] = 0;
            }
        }
    }
}

void ExocetView::KeyCell(int row, int tick, int track)
{
    auto* pack = m_session->Pack();
    if (!pack || row < 0 || row >= static_cast<int>(m_session->Check().rows.size()))
        return;
    const auto& info = pack->Tracks()[track];
    const auto& r = m_session->Check().rows[row];
    const auto type = TypeOf(info);
    m_selRow = row;
    m_selTick = tick;
    m_selTrack = track;
    m_editError.clear();
    const auto* k = pack->FindKey(info.name, r.song, r.pos, r.row, r.pass, tick);
    if (k || type == core::ExocetType::Event) {
        // a key: its value, to edit or (the first character typed) to replace; an event
        // track has no value between its keys to key, so it is typed
        const std::string text = k ? ValueText(info, k->value) : std::string();
        std::snprintf(m_edit, sizeof m_edit, "%s", text.c_str());
        m_editing = true;
        m_editFresh = true;
        return;
    }
    // what the keys give here, as the track takes it (exocet.bin holds fix as 16.16)
    const int32_t v = m_session->HostValue(
        track, row, core::ExocetTickFrac(static_cast<uint32_t>(tick), m_session->RowTicks(row)));
    const double value = type == core::ExocetType::Fix     ? v / 65536.0
                         : type == core::ExocetType::Color ? static_cast<uint32_t>(v)
                                                           : v;
    m_editing = false;
    m_session->SetKey(info.name, row, value, "step", &m_editError, {}, tick);
}

std::string ExocetView::ValueText(const core::ExocetTrackInfo& info, double value)
{
    const auto type = TypeOf(info);
    if (type == core::ExocetType::Color)
        return core::ExocetColorText(static_cast<uint32_t>(value));
    if (type == core::ExocetType::Fix)
        return fmt::format("{:g}", value);
    return std::to_string(static_cast<int64_t>(value));
}

void ExocetView::CopyCell()
{
    auto* pack = m_session->Pack();
    if (!pack || m_selTrack < 0 || m_selRow < 0)
        return;
    if (HasRange() && !m_editing) {
        CopyRange();
        return;
    }
    const auto& info = pack->Tracks()[m_selTrack];
    const auto& r = m_session->Check().rows[m_selRow];
    std::string text;
    if (m_editing) {
        text = m_edit; // what is typed, as it is
    } else if (const auto* k = pack->FindKey(info.name, r.song, r.pos, r.row, r.pass, m_selTick)) {
        text = ValueText(info, k->value) + " " + k->interp;
    } else if (TypeOf(info) == core::ExocetType::Event) {
        m_editError = "nothing to copy: no event here";
        return;
    } else {
        // the value the keys give here, as the cell shows it
        const int32_t v = m_session->HostValue(
            m_selTrack, m_selRow,
            core::ExocetTickFrac(static_cast<uint32_t>(m_selTick), m_session->RowTicks(m_selRow)));
        const auto type = TypeOf(info);
        text = ValueText(info, type == core::ExocetType::Fix     ? v / 65536.0
                               : type == core::ExocetType::Color ? static_cast<uint32_t>(v)
                                                                 : v);
    }
    ImGui::SetClipboardText(text.c_str());
    m_editError.clear();
}

bool ExocetView::ParseValue(const core::ExocetTrackInfo& info, const std::string& text, double* v,
                            std::string* error)
{
    if (TypeOf(info) == core::ExocetType::Color) {
        uint32_t rgba = 0;
        if (!ParseTypedColour(text.c_str(), &rgba)) {
            *error = "\"" + text + "\" is not a colour, #RRGGBBAA or #RRGGBB";
            return false;
        }
        *v = rgba;
        return true;
    }
    char* end = nullptr;
    *v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end || !std::isfinite(*v)) {
        *error = "\"" + text + "\" is not a number";
        return false;
    }
    return true;
}

bool ExocetView::HasRange() const
{
    return m_anchorRow >= 0 && m_selTrack >= 0 && m_selRow >= 0 &&
           (LineOf(m_anchorRow, m_anchorTick) != LineOf(m_selRow, m_selTick) ||
            (m_anchorTrack >= 0 && m_anchorTrack != m_selTrack));
}

std::vector<int> ExocetView::RangeTracks() const
{
    const std::vector<int> shown = VisibleTracks();
    const auto anchor = std::find(shown.begin(), shown.end(), m_anchorTrack);
    const auto cur = std::find(shown.begin(), shown.end(), m_selTrack);
    // the anchor's track folded away: the cursor's track alone
    if (!HasRange() || anchor == shown.end() || cur == shown.end())
        return {m_selTrack};
    return std::vector<int>(std::min(anchor, cur), std::max(anchor, cur) + 1);
}

std::pair<int, int> ExocetView::RangeLines() const
{
    const int cur = LineOf(m_selRow, m_selTick);
    if (!HasRange())
        return {cur, cur};
    const int anchor = LineOf(m_anchorRow, m_anchorTick);
    return {std::min(cur, anchor), std::max(cur, anchor)};
}

void ExocetView::CopyRange()
{
    const auto* pack = m_session->Pack();
    const auto& check = m_session->Check();
    const auto [lo, hi] = RangeLines();
    if (m_lines.empty() || hi >= static_cast<int>(m_lines.size()))
        return;
    const int first = m_lines[lo].first, last = m_lines[hi].first;
    // an end on a row shown a line a tick starts (or stops) at its tick; on a row on one line,
    // the range has all of the row
    auto expanded = [&](int row) { return LineOf(row, 1) != LineOf(row, 0); };
    const int fromTick = expanded(first) ? m_lines[lo].second : 0;
    const int toTick = expanded(last) ? m_lines[hi].second : 255;
    // One track: "exocet <type> <rows> <from tick> <to tick>", then a line a key: "<row> <tick>
    // <value> <interp>", the row counted from the range's first. Several: "exocet block
    // <tracks> <rows> <from tick> <to tick>", then each track's "track <type> <name>" and its
    // keys under it.
    const std::vector<int> tracks = RangeTracks();
    const int rows = last - first + 1;
    std::string text =
        tracks.size() == 1
            ? fmt::format("exocet {} {} {} {}\n", pack->Tracks()[tracks[0]].type, rows, fromTick,
                          toTick)
            : fmt::format("exocet block {} {} {} {}\n", tracks.size(), rows, fromTick, toTick);
    int n = 0;
    for (const int t : tracks) {
        const auto& info = pack->Tracks()[t];
        if (tracks.size() > 1)
            text += fmt::format("track {} {}\n", info.type, info.name);
        for (const auto& k : pack->Keys(info.name)) {
            const int row = check.FindRow(k.song, k.pos, k.row, k.pass);
            if (row < first || row > last)
                continue;
            const int line = LineOf(row, k.tick);
            if (line < lo || line > hi)
                continue;
            text += fmt::format("{} {} {} {}\n", row - first, k.tick, ValueText(info, k.value),
                                k.interp);
            ++n;
        }
    }
    ImGui::SetClipboardText(text.c_str());
    m_editError = tracks.size() == 1
                      ? fmt::format("Copied {} rows of {}: {} key{}", rows,
                                    pack->Tracks()[tracks[0]].name, n, n == 1 ? "" : "s")
                      : fmt::format("Copied {} rows of {} tracks: {} key{}", rows, tracks.size(), n,
                                    n == 1 ? "" : "s");
}

void ExocetView::DeleteRange()
{
    auto* pack = m_session->Pack();
    const auto& check = m_session->Check();
    const auto [lo, hi] = RangeLines();
    if (!pack || m_lines.empty() || hi >= static_cast<int>(m_lines.size()))
        return;
    const int first = m_lines[lo].first, last = m_lines[hi].first;
    const std::vector<int> tracks = RangeTracks();
    // the keys under it as CopyRange takes them, gathered before any goes
    struct Place
    {
        std::string track;
        int row, tick;
    };
    std::vector<Place> doomed;
    for (const int t : tracks) {
        const auto& name = pack->Tracks()[t].name;
        for (const auto& k : pack->Keys(name)) {
            const int row = check.FindRow(k.song, k.pos, k.row, k.pass);
            if (row < first || row > last)
                continue;
            const int line = LineOf(row, k.tick);
            if (line >= lo && line <= hi)
                doomed.push_back({name, row, k.tick});
        }
    }
    if (doomed.empty()) {
        m_editError = "nothing to delete: no keys in the range";
        return;
    }
    const std::string reason = m_session->EditBlockedReason();
    if (!reason.empty()) {
        m_editError = reason;
        return;
    }
    // one undo step, its own: the paste's merge counter keeps it apart from the last
    const std::string merge = fmt::format("delete {}", ++m_pasteCount);
    pack->BreakMerge();
    int n = 0;
    for (const auto& p : doomed)
        n += m_session->DeleteKey(p.track, p.row, p.tick, merge) ? 1 : 0;
    pack->BreakMerge();
    m_editError = tracks.size() == 1 ? fmt::format("Deleted {} key{} from {}", n, n == 1 ? "" : "s",
                                                   pack->Tracks()[tracks[0]].name)
                                     : fmt::format("Deleted {} key{} from {} tracks", n,
                                                   n == 1 ? "" : "s", tracks.size());
}

void ExocetView::PasteRange(const std::string& text)
{
    auto* pack = m_session->Pack();
    const auto& check = m_session->Check();
    const int nrows = static_cast<int>(check.rows.size());
    auto refuse = [&](const std::string& why) { m_editError = "can't paste: " + why; };
    if (const std::string why = m_session->EditBlockedReason(); !why.empty())
        return refuse(why);
    std::vector<std::vector<std::string>> lines;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t nl = std::min(text.find('\n', at), text.size());
        if (auto w = Words(text.substr(at, nl - at)); !w.empty())
            lines.push_back(std::move(w));
        at = nl + 1;
    }
    auto whole = [](const std::string& s, int* out) {
        char* end = nullptr;
        const long v = std::strtol(s.c_str(), &end, 10);
        *out = static_cast<int>(v);
        return end != s.c_str() && !*end;
    };
    // the header: one track's ("exocet <type> ...") or a block of several ("exocet block <n> ...")
    int rows = 0, fromTick = 0, toTick = 0, ntracks = 1;
    const auto& head = lines.front();
    const bool block = head.size() == 6 && head[1] == "block";
    const size_t h = block ? 3 : 2; // where <rows> is
    if (head.size() != h + 3 || (block && (!whole(head[2], &ntracks) || ntracks < 1)) ||
        !whole(head[h], &rows) || !whole(head[h + 1], &fromTick) || !whole(head[h + 2], &toTick) ||
        rows < 1 || fromTick < 0 || toTick > 255)
        return refuse("the clipboard's first line is not \"exocet <type> <rows> <from tick> "
                      "<to tick>\" or \"exocet block <tracks> <rows> <from tick> <to tick>\"");
    const int first = m_selRow;
    if (rows > nrows - first)
        return refuse(fmt::format("the {} rows copied run past the song's end", rows));
    // the copied tracks go onto the cursor's and the ones shown to its right, in the grid's order
    const std::vector<int> shown = VisibleTracks();
    const auto cursor = std::find(shown.begin(), shown.end(), m_selTrack);
    if (cursor == shown.end())
        return refuse("the cursor's track is in a folded group");
    if (ntracks > shown.end() - cursor)
        return refuse(
            fmt::format("the {} tracks copied run past the grid's last open track", ntracks));
    const std::vector<int> targets(cursor, cursor + ntracks);
    if (!block && head[1] != pack->Tracks()[targets[0]].type)
        return refuse(fmt::format("the keys are a {} track's, {} is a {} track", head[1],
                                  pack->Tracks()[targets[0]].name,
                                  pack->Tracks()[targets[0]].type));
    // every line checked before anything is keyed: the paste is all of it or none
    struct Paste
    {
        int track, row, tick;
        double value;
        std::string interp;
    };
    std::vector<Paste> keys;
    int column = block ? -1 : 0; // the copied track the lines are under
    for (size_t l = 1; l < lines.size(); ++l) {
        const auto& w = lines[l];
        if (block && !w.empty() && w[0] == "track") {
            if (w.size() != 3 || ++column >= ntracks)
                return refuse(fmt::format("line {}: more tracks than the {} the first line says",
                                          l + 1, ntracks));
            const auto& info = pack->Tracks()[targets[column]];
            if (w[1] != info.type)
                return refuse(fmt::format("{} is a {} track, {} is a {} track", w[2], w[1],
                                          info.name, info.type));
            continue;
        }
        if (column < 0)
            return refuse(fmt::format("line {} is a key before the first \"track\" line", l + 1));
        const auto& info = pack->Tracks()[targets[column]];
        int off = 0, tick = 0;
        double v = 0;
        std::string e;
        if (w.size() != 4 || !whole(w[0], &off) || !whole(w[1], &tick) || off < 0 || off >= rows ||
            tick < 0 || tick > 255)
            return refuse(fmt::format("line {} is not \"<row> <tick> <value> <interp>\"", l + 1));
        if (!ParseValue(info, w[2], &v, &e))
            return refuse(fmt::format("line {}: {}", l + 1, e));
        if (core::ExocetInterpFromName(w[3]) < 0)
            return refuse(
                fmt::format("line {}: \"{}\" is not step, linear, smooth or ramp", l + 1, w[3]));
        const int row = first + off;
        // a row here shorter than the one copied: its last tick
        keys.push_back(
            {targets[column], row, std::min(tick, m_session->RowTicks(row) - 1), v, w[3]});
    }
    if (block && column != ntracks - 1)
        return refuse(fmt::format("the first line says {} tracks, {} follow", ntracks, column + 1));
    // the keys under the range, laid from the cursor's row and track, go first; then the copied
    // ones - as one undo step, taken back whole if one is refused
    const std::string merge = fmt::format("paste {}", ++m_pasteCount);
    m_editing = false;
    pack->BreakMerge();
    const core::ExocetPack::Checkpoint checkpoint = pack->MakeCheckpoint();
    for (const int t : targets) {
        const std::string name = pack->Tracks()[t].name;
        for (const auto& k : pack->Keys(name)) {
            const int row = check.FindRow(k.song, k.pos, k.row, k.pass);
            if (row < first || row >= first + rows)
                continue;
            if ((row == first && k.tick < fromTick) || (row == first + rows - 1 && k.tick > toTick))
                continue;
            m_session->DeleteKey(name, row, k.tick, merge);
        }
    }
    std::string error;
    for (const auto& k : keys) {
        if (m_session->SetKey(pack->Tracks()[k.track].name, k.row, k.value, k.interp, &error, merge,
                              k.tick))
            continue;
        pack->Rollback(checkpoint); // whole, and nothing left to redo
        return refuse(
            fmt::format("{} on {} at {}", error, pack->Tracks()[k.track].name,
                        core::ExocetPlaceLabel(check.rows[k.row], check.digests.size() > 1)));
    }
    pack->BreakMerge();
    // what was pasted, selected
    m_anchorRow = first + rows - 1;
    m_anchorTick = std::min(toTick, m_session->RowTicks(m_anchorRow) - 1);
    m_anchorTrack = targets.back();
    m_selTick = std::min(fromTick, m_session->RowTicks(first) - 1);
    m_editError = ntracks == 1 ? fmt::format("Pasted {} rows onto {}: {} key{}", rows,
                                             pack->Tracks()[targets[0]].name, keys.size(),
                                             keys.size() == 1 ? "" : "s")
                               : fmt::format("Pasted {} rows onto {} tracks from {}: {} key{}",
                                             rows, ntracks, pack->Tracks()[targets[0]].name,
                                             keys.size(), keys.size() == 1 ? "" : "s");
}

void ExocetView::PasteCell()
{
    auto* pack = m_session->Pack();
    if (!pack || m_selTrack < 0 || m_selRow < 0)
        return;
    m_editing = false;
    const auto& info = pack->Tracks()[m_selTrack];
    const char* clip = ImGui::GetClipboardText();
    const std::string text = clip ? clip : "";
    // "<value>" or "<value> <interp>", as Ctrl+C puts a cell; a range starts "exocet"
    const std::vector<std::string> words = Words(text);
    if (!words.empty() && words[0] == "exocet") {
        PasteRange(text);
        return;
    }
    m_anchorRow = -1;
    if (words.empty() || words.size() > 2) {
        m_editError = "can't paste: the clipboard is not a value, or a value and an "
                      "interpolation";
        return;
    }
    double v = 0;
    std::string why;
    if (!ParseValue(info, words[0], &v, &why)) {
        m_editError = "can't paste: " + why;
        return;
    }
    const auto& r = m_session->Check().rows[m_selRow];
    std::string interp = "step";
    if (words.size() == 2)
        interp = words[1];
    else if (const auto* k = pack->FindKey(info.name, r.song, r.pos, r.row, r.pass, m_selTick))
        interp = k->interp;
    if (core::ExocetInterpFromName(interp) < 0) {
        m_editError = "can't paste: \"" + interp + "\" is not step, linear, smooth or ramp";
        return;
    }
    // the track's range, whole numbers, an event's entries: the session refuses with why
    m_editError.clear();
    if (!m_session->SetKey(info.name, m_selRow, v, interp, &m_editError, {}, m_selTick))
        m_editError = "can't paste: " + m_editError;
}

void ExocetView::Undo(bool redo)
{
    m_editing = false; // a value half typed goes with the step it would have been on
    m_editError.clear();
    if (redo ? m_session->Redo() : m_session->Undo()) {
        m_scrollToRow = m_selRow; // the selection stays; bring it back into view
        m_scrollToTick = m_selTick;
    }
}

void ExocetView::CommitEdit()
{
    m_editing = false;
    auto* pack = m_session->Pack();
    if (!pack || m_selTrack < 0 || m_selRow < 0)
        return;
    const auto& info = pack->Tracks()[m_selTrack];
    double v = 0;
    if (TypeOf(info) == core::ExocetType::Color) {
        uint32_t rgba = 0;
        if (!ParseTypedColour(m_edit, &rgba)) {
            m_editError = "not a colour: #RRGGBBAA or #RRGGBB";
            return;
        }
        v = rgba;
    } else {
        char* end = nullptr;
        v = std::strtod(m_edit, &end);
        if (end == m_edit) {
            m_editError = "not a number";
            return;
        }
    }
    const auto& r = m_session->Check().rows[m_selRow];
    std::string interp = "step";
    if (const auto* k = pack->FindKey(info.name, r.song, r.pos, r.row, r.pass, m_selTick))
        interp = k->interp;
    m_editError.clear();
    m_session->SetKey(info.name, m_selRow, v, interp, &m_editError, {}, m_selTick);
}

void ExocetView::DrawDetail()
{
    auto* pack = m_session->Pack();
    if (!pack || m_selTrack < 0 || m_selTrack >= static_cast<int>(pack->Tracks().size())) {
        ImGui::TextDisabled("Select a cell and type over its value: Enter, or moving off the cell, "
                            "keys it, Esc drops it. I cycles the "
                            "interpolation, Del removes a key (a range: all its keys), Space "
                            "pauses, PgUp/PgDn move a pattern, "
                            "T shows a row's ticks (click the row too), Ctrl+C and Ctrl+V copy "
                            "and paste a cell or a range (Shift+arrows, or drag), "
                            "M bookmarks the row (or right-click the gutter), Ctrl+Up/Down go "
                            "between bookmarks and Ctrl+1..9 to one, Ctrl+S saves. "
                            "Double-click keys a cell or edits its key; right-click clears a key.");
        return;
    }
    const auto& t = pack->Tracks()[m_selTrack];
    const auto& b = m_session->Block();
    const bool isColour = TypeOf(t) == core::ExocetType::Color;
    ImGui::TextColored(kKeyColour, "%s", t.name.c_str());
    ImGui::SameLine();
    if (isColour)
        ImGui::TextDisabled("color, default %s, %zu keys",
                            core::ExocetColorText(static_cast<uint32_t>(t.def)).c_str(),
                            m_session->HostKeys(t.index).size());
    else
        ImGui::TextDisabled("%s, %g..%g, default %g, %zu keys", t.type.c_str(), t.min, t.max, t.def,
                            m_session->HostKeys(t.index).size());
    if (!t.text.empty())
        ImGui::TextWrapped("%s", t.text.c_str());
    if (b.present && m_session->ManifestMatches() && t.index < static_cast<int>(b.values.size())) {
        const int32_t host = m_session->HostValue(t.index, b.row, b.frac);
        const int32_t live = b.values[t.index];
        ImGui::Text("Live %s", FormatValue(t, live).c_str());
        ImGui::SameLine();
        ImGui::TextColored(host == live || TypeOf(t) == core::ExocetType::Event ? kOkColour
                                                                                : kWarnColour,
                           "keys %s", FormatValue(t, host).c_str());
    } else {
        ImGui::TextDisabled("Live -");
    }
    if (m_selRow >= 0 && m_selRow < static_cast<int>(m_session->Check().rows.size())) {
        const auto& r = m_session->Check().rows[m_selRow];
        const auto parts = Parts();
        const int part = PartOf(parts, m_selRow, m_selTick);
        ImGui::SameLine();
        ImGui::TextDisabled(
            "| at %s%s", core::ExocetPlaceLabel(r, m_session->Check().digests.size() > 1).c_str(),
            m_selTick ? fmt::format(" tick {}", m_selTick).c_str() : "");
        if (part >= 0) {
            ImGui::SameLine();
            ImGui::TextUnformatted(("in " + parts[part].name).c_str());
        }
    }
    if (isColour)
        DrawColourEdit(t);
    if (!m_editError.empty())
        ImGui::TextColored(kBlockColour, "%s", m_editError.c_str());
    DrawGraph(t.index);
}

void ExocetView::DrawColourEdit(const core::ExocetTrackInfo& t)
{
    // The selected row's colour: its key's, or what the keys give there - changing it keys it,
    // with the key's interpolation kept (step for a new one), as typing a value does
    auto* pack = m_session->Pack();
    const auto& check = m_session->Check();
    const bool row = m_selRow >= 0 && m_selRow < static_cast<int>(check.rows.size());
    uint32_t rgba = static_cast<uint32_t>(core::ExocetToBin(core::ExocetType::Color, t.def));
    const core::ExocetKeyInfo* key = nullptr;
    if (row) {
        const auto& r = check.rows[m_selRow];
        key = pack->FindKey(t.name, r.song, r.pos, r.row, r.pass, m_selTick);
        rgba = key ? static_cast<uint32_t>(key->value)
                   : static_cast<uint32_t>(
                         m_session->HostValue(t.index, m_selRow,
                                              core::ExocetTickFrac(static_cast<uint32_t>(m_selTick),
                                                                   m_session->RowTicks(m_selRow))));
    }
    float c[4] = {(rgba >> 24) / 255.0f, ((rgba >> 16) & 255) / 255.0f,
                  ((rgba >> 8) & 255) / 255.0f, (rgba & 255) / 255.0f};
    const std::string why = row ? m_session->EditBlockedReason() : "Select a row to key";
    ImGui::BeginDisabled(!why.empty());
    const std::string interp = key ? key->interp : "step";
    auto apply = [&] {
        auto byte = [](float f) {
            return static_cast<uint32_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        const uint32_t v = byte(c[0]) << 24 | byte(c[1]) << 16 | byte(c[2]) << 8 | byte(c[3]);
        if (v != rgba) {
            m_editError.clear();
            m_editing = false;
            m_session->SetKey(t.name, m_selRow, v, interp, &m_editError,
                              fmt::format("colour {} {} {}", t.name, m_selRow, m_selTick),
                              m_selTick);
        }
    };
    ImGui::SetNextItemWidth(std::min(360.0f, ImGui::GetContentRegionAvail().x));
    // RGBA as 0..255 each; right-click for the options. The swatch after them opens the mixer
    // (with hex), which has the eyedropper in it too.
    const ImGuiColorEditFlags flags = ImGuiColorEditFlags_AlphaBar |
                                      ImGuiColorEditFlags_AlphaPreviewHalf |
                                      ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_Uint8;
    if (ImGui::ColorEdit4("##colour", c, flags | ImGuiColorEditFlags_NoSmallPreview))
        apply();
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::ColorButton("##colour_swatch", ImVec4(c[0], c[1], c[2], c[3]),
                           ImGuiColorEditFlags_AlphaPreviewHalf)) {
        // the colour as it was when the mixer opened, beside the current one (a click on it
        // puts it back), as ImGui's own popup shows
        std::copy(c, c + 4, m_mixerOriginal);
        ImGui::OpenPopup("##colour_mixer");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Open the mixer");
    if (ImGui::BeginPopup("##colour_mixer")) {
        if (ImGui::ColorPicker4("##mixer", c,
                                ImGuiColorEditFlags_AlphaBar |
                                    ImGuiColorEditFlags_AlphaPreviewHalf |
                                    ImGuiColorEditFlags_Uint8,
                                m_mixerOriginal))
            apply();
        DrawScreenPickButton(t);
        ImGui::SameLine();
        ImGui::TextDisabled("Pick from the emulator's screen");
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    DrawScreenPickButton(t);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(key ? "Key" : "Here");
    // a drag through the picker is one undo step; the next press of the mouse starts another
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && pack)
        pack->BreakMerge();
    ImGui::EndDisabled();
    TooltipWhenDisabled(why);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", key ? "the key on this row" : "not keyed here: a change keys it");
}

void ExocetView::DrawScreenPickButton(const core::ExocetTrackInfo& t)
{
    if (m_pickingColour) {
        // lit while it waits for the click on the picture; a click here calls it off
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(ICON_FA_EYE_DROPPER "##pick_screen") && m_pickScreenColour)
            m_pickScreenColour(nullptr);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click the emulator's picture to take its colour.\nEsc, a right "
                              "click on the picture, or this button cancels.");
        return;
    }
    const bool can = m_pickScreenColour && m_canPickScreenColour && m_canPickScreenColour();
    ImGui::BeginDisabled(!can);
    if (ImGui::Button(ICON_FA_EYE_DROPPER "##pick_screen")) {
        // the cell it was started on is the one keyed, wherever the cursor is by the click
        const std::string track = t.name;
        const int row = m_selRow, tick = m_selTick;
        m_pickingColour = true;
        m_pickScreenColour([this, track, row, tick](bool picked, uint32_t rgba) {
            m_pickingColour = false;
            if (picked)
                KeyPickedColour(track, row, tick, rgba);
        });
        ImGui::CloseCurrentPopup(); // the mixer, when it is in it: it would hold the hover
    }
    ImGui::EndDisabled();
    if (can && ImGui::IsItemHovered())
        ImGui::SetTooltip("Pick the colour off the emulator's picture (the alpha stays)");
    TooltipWhenDisabled(can ? "" : "Start the emulator to pick a colour off its picture");
}

void ExocetView::KeyPickedColour(const std::string& track, int row, int tick, uint32_t rgb)
{
    auto* pack = m_session->Pack();
    const auto* info = pack ? pack->FindTrack(track) : nullptr;
    const auto& check = m_session->Check();
    if (!info || row < 0 || row >= static_cast<int>(check.rows.size()))
        return;
    const auto& r = check.rows[row];
    const auto* key = pack->FindKey(track, r.song, r.pos, r.row, r.pass, tick);
    const uint32_t was =
        key ? static_cast<uint32_t>(key->value)
            : static_cast<uint32_t>(m_session->HostValue(
                  info->index, row,
                  core::ExocetTickFrac(static_cast<uint32_t>(tick), m_session->RowTicks(row))));
    const std::string interp = key ? key->interp : "step";
    m_editError.clear();
    m_editing = false;
    pack->BreakMerge(); // its own undo step, not the mixer's last drag
    m_session->SetKey(track, row, (rgb & 0xFFFFFF00u) | (was & 0xFFu), interp, &m_editError, {},
                      tick);
}

void ExocetView::DrawGraph(int track)
{
    auto* pack = m_session->Pack();
    const auto& check = m_session->Check();
    if (!pack || !check.valid || check.rows.empty())
        return;
    const auto& t = pack->Tracks()[track];
    const auto type = TypeOf(t);
    const auto& b = m_session->Block();
    const int center = m_selRow >= 0 ? m_selRow : static_cast<int>(b.row);
    const int span = 96;
    const int r0 = std::max(0, center - span / 2);
    const int r1 = std::min(static_cast<int>(check.rows.size()), r0 + span);

    const ImVec2 size(ImGui::GetContentRegionAvail().x,
                      std::max(90.0f, ImGui::GetContentRegionAvail().y - 4));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##graph", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(20, 22, 28, 255));

    if (type == core::ExocetType::Color) {
        DrawColourGraph(track, p0, size, r0, r1);
        return;
    }
    double lo = core::ExocetToBin(type, t.min), hi = core::ExocetToBin(type, t.max);
    if (type == core::ExocetType::Event) {
        // an event is 0 between its keys and one of its entries on them
        lo = 0;
        hi = 1;
        for (const auto& e : t.entries)
            hi = std::max<double>(hi, e.value);
    }
    const double range = hi > lo ? hi - lo : 1;
    auto X = [&](double row) { return p0.x + float((row - r0) / std::max(1, r1 - r0) * size.x); };
    auto Y = [&](double v) { return p0.y + size.y - 4 - float((v - lo) / range * (size.y - 8)); };

    // pattern starts
    for (int r = r0; r < r1; ++r)
        if (check.rows[r].row == 0)
            dl->AddLine(ImVec2(X(r), p0.y), ImVec2(X(r), p0.y + size.y),
                        IM_COL32(255, 255, 255, 25));

    // the keys' curve (the host evaluator), four points a row
    ImVec2 prev;
    for (int r = r0; r < r1; ++r)
        for (int q = 0; q < 4; ++q) {
            const uint16_t frac = static_cast<uint16_t>(q << 14);
            const ImVec2 pt(X(r + q / 4.0), Y(m_session->HostValue(track, r, frac)));
            if (r != r0 || q)
                dl->AddLine(prev, pt, IM_COL32(255, 220, 120, 200), 1.5f);
            prev = pt;
        }
    for (const auto& k : m_session->HostKeys(track))
        if (k.row >= r0 && k.row < r1)
            dl->AddCircleFilled(ImVec2(X(k.row + k.sub / 256.0), Y(k.value)), 3.0f,
                                IM_COL32(255, 235, 150, 255));

    // what the demo actually had, recorded every frame; red where it is not what the keys say
    int diverged = 0;
    const auto [recFrom, recTo] = RecordedRows(m_session->Recorded(), check, r0, r1);
    for (auto it = recFrom; it != recTo; ++it) {
        const auto& s = it->second;
        if (s.row < r0 || s.row >= r1 || track >= static_cast<int>(s.values.size()))
            continue;
        const double row = s.row + s.frac / 65536.0;
        const bool same = type == core::ExocetType::Event ||
                          m_session->HostValue(track, s.row, s.frac) == s.values[track];
        diverged += same ? 0 : 1;
        dl->AddCircleFilled(ImVec2(X(row), Y(s.values[track])), same ? 1.5f : 2.5f,
                            same ? IM_COL32(110, 200, 255, 200) : IM_COL32(255, 80, 70, 255));
    }
    if (b.present && m_session->ManifestMatches() && b.row >= uint32_t(r0) &&
        b.row < uint32_t(r1)) {
        const float x = X(b.row + b.frac / 65536.0);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + size.y), IM_COL32(90, 220, 110, 200), 1.5f);
    }
    dl->AddText(ImVec2(p0.x + 4, p0.y + 2), IM_COL32(200, 200, 200, 160),
                fmt::format("rows {}-{}   keys (yellow), recorded (blue){}", r0, r1 - 1,
                            diverged ? fmt::format(", {} differ (red)", diverged) : "")
                    .c_str());
}

void ExocetView::DrawColourGraph(int track, ImVec2 p0, ImVec2 size, int r0, int r1)
{
    // the colour along the rows, as a strip over the checks its alpha shows through; under it
    // each channel 0..255 (red, green, blue, and alpha in grey), with the keys marked
    const auto& b = m_session->Block();
    const auto& check = m_session->Check();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float strip = std::min(28.0f, size.y * 0.3f);
    auto X = [&](double row) { return p0.x + float((row - r0) / std::max(1, r1 - r0) * size.x); };
    const float top = p0.y + 16, low = p0.y + size.y - 4;
    const float g0 = top + strip + 6;
    auto Y = [&](int v) { return low - (v / 255.0f) * (low - g0); };

    const float cell = strip / 2;
    for (float x = p0.x; x < p0.x + size.x; x += cell)
        for (int k = 0; k < 2; ++k) {
            const bool dark = (static_cast<int>((x - p0.x) / cell) + k) & 1;
            dl->AddRectFilled(ImVec2(x, top + k * cell),
                              ImVec2(std::min(x + cell, p0.x + size.x), top + (k + 1) * cell),
                              dark ? IM_COL32(70, 70, 70, 255) : IM_COL32(110, 110, 110, 255));
        }
    for (int r = r0; r < r1; ++r)
        for (int q = 0; q < 4; ++q) {
            const uint16_t frac = static_cast<uint16_t>(q << 14);
            const float x0 = X(r + q / 4.0), x1 = X(r + (q + 1) / 4.0) + 1;
            dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + strip),
                              ColourOf(m_session->HostValue(track, r, frac)));
        }

    for (int r = r0; r < r1; ++r)
        if (check.rows[r].row == 0)
            dl->AddLine(ImVec2(X(r), g0), ImVec2(X(r), low), IM_COL32(255, 255, 255, 25));
    static const ImU32 kChannel[4] = {IM_COL32(255, 90, 80, 220), IM_COL32(90, 220, 110, 220),
                                      IM_COL32(100, 150, 255, 220), IM_COL32(200, 200, 200, 160)};
    for (int ch = 0; ch < 4; ++ch) {
        const int sh = 24 - 8 * ch;
        ImVec2 prev;
        for (int r = r0; r < r1; ++r)
            for (int q = 0; q < 4; ++q) {
                const uint16_t frac = static_cast<uint16_t>(q << 14);
                const auto v = static_cast<uint32_t>(m_session->HostValue(track, r, frac));
                const ImVec2 pt(X(r + q / 4.0), Y((v >> sh) & 255));
                if (r != r0 || q)
                    dl->AddLine(prev, pt, kChannel[ch], 1.5f);
                prev = pt;
            }
    }
    for (const auto& k : m_session->HostKeys(track))
        if (k.row >= r0 && k.row < r1) {
            const float x = X(k.row + k.sub / 256.0);
            dl->AddLine(ImVec2(x, top - 2), ImVec2(x, top + strip + 2),
                        IM_COL32(255, 235, 150, 255), 2.0f);
        }

    // what the demo actually had: a tick under the strip, red where the keys say otherwise
    int diverged = 0;
    const auto [recFrom, recTo] = RecordedRows(m_session->Recorded(), check, r0, r1);
    for (auto it = recFrom; it != recTo; ++it) {
        const auto& s = it->second;
        if (s.row < r0 || s.row >= r1 || track >= static_cast<int>(s.values.size()))
            continue;
        const bool same = m_session->HostValue(track, s.row, s.frac) == s.values[track];
        diverged += same ? 0 : 1;
        const float x = X(s.row + s.frac / 65536.0);
        dl->AddRectFilled(ImVec2(x - 1, top + strip + 1), ImVec2(x + 1, top + strip + 5),
                          same ? ColourOf(s.values[track] | 0xFF) : IM_COL32(255, 80, 70, 255));
    }
    if (b.present && m_session->ManifestMatches() && b.row >= uint32_t(r0) &&
        b.row < uint32_t(r1)) {
        const float x = X(b.row + b.frac / 65536.0);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + size.y), IM_COL32(90, 220, 110, 200), 1.5f);
    }
    dl->AddText(ImVec2(p0.x + 4, p0.y + 1), IM_COL32(200, 200, 200, 160),
                fmt::format("rows {}-{}   the colour, then R G B A 0..255; keys (yellow), recorded "
                            "(under the strip){}",
                            r0, r1 - 1, diverged ? fmt::format(", {} differ (red)", diverged) : "")
                    .c_str());
}

void ExocetView::DrawProof()
{
    const auto cuts = m_session->Cuts();
    if (ImGui::CollapsingHeader("Cuts", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (cuts.empty()) {
            ImGui::TextDisabled(m_session->Check().valid
                                    ? "No director.cut track."
                                    : "No rows yet: the kit's check gives them.");
        } else if (ImGui::BeginTable("##cuts", 6,
                                     ImGuiTableFlags_BordersInnerH |
                                         ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Cut");
            ImGui::TableSetupColumn("Key");
            ImGui::TableSetupColumn("Key frame");
            ImGui::TableSetupColumn("Ready");
            ImGui::TableSetupColumn("Cut at");
            ImGui::TableSetupColumn("Margin");
            ImGui::TableHeadersRow();
            const auto& rows = m_session->Check().rows;
            for (const auto& c : cuts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                // anywhere on the row takes the grid to the cut's key
                ImGui::BeginDisabled(c.keyRow < 0);
                if (ImGui::Selectable(fmt::format("{}##cut{}", c.name, c.cut).c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns) &&
                    c.keyRow >= 0) {
                    m_selRow = c.keyRow;
                    m_selTick = 0;
                    m_scrollToRow = c.keyRow;
                    m_scrollToTick = 0;
                    m_follow = false;
                }
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                if (c.keyRow >= 0) {
                    const auto& r = rows[c.keyRow];
                    ImGui::Text("%02u:%02u", r.pos, r.row);
                } else {
                    ImGui::TextDisabled("no key");
                }
                ImGui::TableNextColumn();
                if (c.keyRow >= 0)
                    ImGui::Text("%u", c.keyFrame);
                ImGui::TableNextColumn();
                c.readyFrame >= 0 ? ImGui::Text("%d", c.readyFrame) : ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                c.cutFrame >= 0 ? ImGui::Text("%d", c.cutFrame) : ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (c.keyRow < 0 || c.readyFrame < 0)
                    ImGui::TextDisabled("-");
                else if (c.late > 0)
                    ImGui::TextColored(kBlockColour, "%d late", c.late);
                else
                    ImGui::TextColored(c.margin < 25 ? kWarnColour : kOkColour, "%d ahead",
                                       c.margin);
            }
            ImGui::EndTable();
        }
    }
    const auto events = m_session->Events();
    int on = 0, late = 0, missed = 0, pending = 0;
    for (const auto& e : events)
        (e.state == ExocetSession::EventProof::OnTime   ? on
         : e.state == ExocetSession::EventProof::Late   ? late
         : e.state == ExocetSession::EventProof::Missed ? missed
                                                        : pending)++;
    ImGui::Text("Events: ");
    ImGui::SameLine();
    ImGui::TextColored(kOkColour, "%d on time", on);
    ImGui::SameLine();
    ImGui::TextColored(late ? kWarnColour : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                       "%d late", late);
    ImGui::SameLine();
    ImGui::TextColored(missed ? kWarnColour : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                       "%d never fired", missed);
    ImGui::SameLine();
    ImGui::TextDisabled("%d to come, %zu frames recorded", pending, m_session->Recorded().size());
}

void ExocetView::DrawIssues()
{
    const auto issues = m_session->Issues();
    const std::string head = fmt::format("Issues ({})###issues", issues.size());
    if (!ImGui::CollapsingHeader(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        return;
    if (issues.empty()) {
        ImGui::TextDisabled("None.");
        return;
    }
    int n = 0;
    for (const char* level : {"block", "warn", "info"}) {
        for (const auto& i : issues) {
            if (i.level != level)
                continue;
            ImGui::PushID(n++);
            ImGui::TextColored(LevelColour(i.level), "%s",
                               i.level == "block"  ? ICON_FA_CIRCLE_XMARK
                               : i.level == "warn" ? ICON_FA_TRIANGLE_EXCLAMATION
                                                   : ICON_FA_CIRCLE_INFO);
            ImGui::SameLine();
            const std::string title = i.title + (i.peekaboo ? "  (Peekaboo)" : "");
            if (ImGui::Selectable(title.c_str())) {
                // anchors: a song place moves the grid there, a track selects it
                if (i.at.is_object()) {
                    const int row =
                        m_session->Check().FindRow(core::jsonread::ReadOr(i.at, "song", 0),
                                                   core::jsonread::ReadOr(i.at, "pos", 0),
                                                   core::jsonread::ReadOr(i.at, "row", 0),
                                                   core::jsonread::ReadOr(i.at, "pass", 0));
                    if (row >= 0) {
                        m_selRow = row;
                        m_selTick = 0;
                        m_scrollToRow = row;
                        m_scrollToTick = 0;
                        m_follow = false;
                    }
                }
                if (i.subject.is_object()) {
                    if (i.subject.contains("track") && m_session->Pack())
                        if (const auto* t = m_session->Pack()->FindTrack(
                                core::jsonread::ReadOr(i.subject, "track", "")))
                            m_selTrack = t->index;
                    if (i.subject.contains("sample"))
                        m_filterSample = core::jsonread::ReadOr(i.subject, "sample", 0);
                }
            }
            if (ImGui::IsItemHovered()) {
                std::string blocks;
                for (const auto& a : i.blocks)
                    blocks += (blocks.empty() ? "" : ", ") + a;
                ImGui::SetTooltip("%s\n\n%s%s", i.text.c_str(), i.code.c_str(),
                                  blocks.empty() ? "" : (" - stops " + blocks).c_str());
            }
            ImGui::PopID();
        }
    }
}

std::string ExocetView::Time(uint32_t frame) const
{
    return exo::SongTime(frame, m_session->Fps());
}

} // namespace adm::ui
