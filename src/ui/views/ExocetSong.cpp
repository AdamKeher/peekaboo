#include "ExocetSong.h"

#include <algorithm>
#include <filesystem>

#include <fmt/format.h>

#include "../../core/ExocetSession.h"
#include "ExocetUi.h"

namespace adm::ui::exo {

namespace {

constexpr ImVec4 kOkColour = {0.45f, 0.85f, 0.45f, 1.00f};
constexpr ImVec4 kBlockColour = {1.00f, 0.40f, 0.35f, 1.00f};

// The order row's cells: a position, the playing one, and the others that play its pattern
constexpr ImU32 kOrderCell = IM_COL32(40, 40, 50, 255);
constexpr ImU32 kOrderPlaying = IM_COL32(45, 140, 70, 255);
constexpr ImU32 kOrderAgain = IM_COL32(35, 80, 48, 255);

std::string FileName(const std::string& path)
{
    return path.empty() ? "-" : std::filesystem::path(path).filename().string();
}

} // namespace

std::string PositionTip(const core::ExocetCheck& check, size_t song, int pos, int fps)
{
    if (pos < 0)
        return "The kit's lead: silent rows in front of the song, in patterns of its own";
    if (song >= check.orders.size() || pos >= static_cast<int>(check.orders[song].size()))
        return {};
    const std::vector<int>& order = check.orders[song];
    std::string tip = fmt::format("Position {:02}: pattern {}", pos, order[pos]);
    const auto first = std::find_if(check.rows.begin(), check.rows.end(),
                                    [&](const auto& r) { return r.song == song && r.pos == pos; });
    if (first == check.rows.end())
        tip += ", which the song never plays";
    else
        tip += fmt::format(", from {} (frame {})", SongTime(first->frame, fps), first->frame);
    std::string others;
    for (size_t p = 0; p < order.size(); ++p)
        if (order[p] == order[pos] && static_cast<int>(p) != pos)
            others += fmt::format("{}{:02}", others.empty() ? "" : ", ", p);
    if (!others.empty())
        tip += fmt::format("\nPattern {} is at {} too: keys are on positions, so each has its own",
                           order[pos], others);
    return tip;
}

std::vector<SongFact> SongFacts(const core::ExocetSession& session)
{
    const auto* pack = session.Pack();
    const auto& check = session.Check();
    std::vector<SongFact> out;
    auto add = [&out](std::string label, std::string value, std::string tip = {},
                      std::optional<ImVec4> colour = std::nullopt) {
        out.push_back({std::move(label), std::move(value), std::move(tip), colour});
    };
    const int songs = pack ? pack->SongCount() : 0;
    if (songs == 0)
        add("Module", "-");
    for (int n = 0; n < songs; ++n)
        add(songs > 1 ? fmt::format("Song {}", n) : "Module", FileName(pack->ModulePath(n)),
            pack->ModulePath(n));
    if (!check.valid) {
        for (const char* label : {"Length", "Rows", "Positions", "Samples"})
            add(label, "-", pack ? "The kit's check has not given the song yet" : "");
    } else {
        add("Length", SongTime(check.frames, session.Fps()),
            fmt::format("{} frames", check.frames));
        add("Rows", fmt::format("{} played", check.rows.size()),
            "Every row the song plays, in order, a repeated pattern as often as it plays");
        add("Positions", fmt::format("{}, {} channels", check.order.size(), check.channels));
        int used = 0;
        for (const auto& smp : check.samples)
            used += smp.bytes != 0;
        add("Samples", std::to_string(used));
    }
    bool same = true;
    for (int n = 0; n < songs && n < static_cast<int>(check.orders.size()); ++n) {
        const auto order = pack->SongOrder(n);
        same &= order.empty() || order == check.orders[static_cast<size_t>(n)];
    }
    if (!pack || !check.valid)
        add("Keys placed", "-");
    else if (same)
        add("Keys placed", "on these songs", {}, kOkColour);
    else
        add("Keys placed", "on another order list",
            "The module's order list is not the one the keys were placed against; the kit has "
            "moved them, but the file still holds the old places",
            kBlockColour);
    return out;
}

// A list longer than the room scrolls, the playing position a third of the way in; << and >> say
// there is more either side
void DrawOrderList(const core::ExocetCheck& check, int playRow, int fps)
{
    const core::ExocetRow* now = playRow >= 0 && playRow < static_cast<int>(check.rows.size())
                                     ? &check.rows[static_cast<size_t>(playRow)]
                                     : nullptr;
    // with no playhead, the song the samples are of: the one the Music window is about
    const size_t song = now                    ? now->song
                        : check.musicSong >= 0 ? static_cast<size_t>(check.musicSong)
                                               : 0;
    static const std::vector<int> kNone;
    const std::vector<int>& order = song < check.orders.size() ? check.orders[song] : kNone;
    const int count = static_cast<int>(order.size());
    const int pos = now ? now->pos : -1; // negative in the song's lead
    const int pattern = pos >= 0 && pos < count ? order[pos] : -1;
    const bool songs = check.orders.size() > 1;

    const ImGuiStyle& st = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = ImGui::GetTextLineHeight() + st.CellPadding.y * 2.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float right = origin.x + ImGui::GetContentRegionAvail().x;
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_TableBorderLight);

    // "Pos 07", "Song 1  Pos 07", "Pos L1" in the lead; its width fixed, so the cells never move
    std::string where = songs ? fmt::format("Song {}  ", song) : std::string();
    where += !now      ? std::string("Pos --")
             : pos < 0 ? fmt::format("Pos L{}", -pos)
                       : fmt::format("Pos {:02}", pos);
    const float labelW =
        ImGui::CalcTextSize(songs ? "Song 0  Pos 000" : "Pos 000").x + st.ItemSpacing.x * 2.0f;
    const float textY = origin.y + st.CellPadding.y;
    dl->AddText(ImVec2(origin.x, textY), now ? text : dim, where.c_str());
    if (!count) {
        ImGui::Dummy(ImVec2(right - origin.x, rowH));
        return;
    }

    // A cell as wide as the widest pattern number, two digits at least
    const int widest = std::max(10, *std::max_element(order.begin(), order.end()));
    const float cellW =
        ImGui::CalcTextSize(std::to_string(widest).c_str()).x + st.CellPadding.x * 2.0f;
    const float gap = 2.0f;
    const float step = cellW + gap;
    const float arrowW = ImGui::CalcTextSize("<<").x + st.ItemSpacing.x * 2.0f;
    const float x0 = origin.x + labelW;
    int fit = static_cast<int>((right - x0 + gap) / step);
    int first = 0;
    const bool scrolls = count > fit;
    if (scrolls) {
        fit = std::max(1, static_cast<int>((right - x0 - arrowW * 2.0f + gap) / step));
        first = std::clamp(std::max(pos, 0) - fit / 3, 0, count - fit);
    }
    const int last = std::min(count, first + fit);

    const bool hovering = ImGui::IsWindowHovered();
    int hovered = -1;
    float x = x0 + (scrolls ? arrowW : 0.0f);
    if (first > 0)
        dl->AddText(ImVec2(x0, textY), dim, "<<");
    for (int i = first; i < last; ++i) {
        const ImVec2 a(x, origin.y), b(x + cellW, origin.y + rowH);
        const bool playing = i == pos;
        const bool again = !playing && order[i] == pattern;
        dl->AddRectFilled(a, b, playing ? kOrderPlaying : again ? kOrderAgain : kOrderCell);
        if (playing)
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
        else
            dl->AddRect(a, b, border);
        const std::string s = std::to_string(order[i]);
        const float w = ImGui::CalcTextSize(s.c_str()).x;
        dl->AddText(ImVec2(a.x + (cellW - w) * 0.5f, textY), text, s.c_str());
        if (hovering && ImGui::IsMouseHoveringRect(a, b))
            hovered = i;
        x += step;
    }
    if (last < count)
        dl->AddText(ImVec2(x - gap + st.ItemSpacing.x, textY), dim, ">>");
    ImGui::Dummy(ImVec2(right - origin.x, rowH));
    if (hovered >= 0)
        ImGui::SetTooltip("%s", PositionTip(check, song, hovered, fps).c_str());
}

} // namespace adm::ui::exo
