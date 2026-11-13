#include "ExocetTimeline.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <string>

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "../../core/ExocetBlock.h"
#include "../../core/ExocetEval.h"
#include "../../core/ExocetSession.h"
#include "ExocetUi.h"

namespace adm::ui::exo {

namespace {

// A part's phase (engine/exocet.h EXO_PHASE_*), its colour and name on the timeline
ImU32 PhaseColour(int phase, int alpha = 255)
{
    static const ImU32 kColours[] = {
        IM_COL32(120, 120, 120, 255), // none
        IM_COL32(96, 150, 230, 255),  // load
        IM_COL32(176, 120, 226, 255), // unpack
        IM_COL32(232, 170, 70, 255),  // prep
        IM_COL32(110, 200, 110, 255), // run
        IM_COL32(90, 150, 140, 255),  // hold
    };
    const ImU32 c = kColours[phase >= 0 && phase <= 5 ? phase : 0];
    return (c & ~IM_COL32_A_MASK) |
           (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << IM_COL32_A_SHIFT);
}

const char* PhaseName(int phase)
{
    static const char* kNames[] = {"-", "load", "unpack", "prep", "run", "hold"};
    return kNames[phase >= 0 && phase <= 5 ? phase : 0];
}

// A key's interpolation (core ExocetInterp), its colour and name under a group's lane
ImU32 InterpColour(int interp, int alpha = 255)
{
    static const ImU32 kColours[] = {
        IM_COL32(150, 160, 175, 255), // step: set, and held
        IM_COL32(90, 200, 220, 255),  // linear
        IM_COL32(220, 120, 190, 255), // smooth
        IM_COL32(235, 205, 80, 255),  // ramp
    };
    const ImU32 c = kColours[interp >= 0 && interp <= 3 ? interp : 0];
    return (c & ~IM_COL32_A_MASK) |
           (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << IM_COL32_A_SHIFT);
}

const char* InterpName(int interp) { return core::ExocetInterpName(static_cast<uint8_t>(interp)); }

// What runs across: the song's time or its frames - one scale, the labels apart - or its played
// rows, each the same width whatever its speed
enum Axis { kAxisTime, kAxisRow, kAxisFrame };
const char* const kAxisNames[] = {"Time", "Row", "Frame"};

} // namespace

void DrawTimeline(core::ExocetSession& session, const core::ExocetBlockState& b, bool fill,
                  const std::function<void(int row)>& onShowRow,
                  const std::function<bool(uint64_t& first, uint64_t& last)>& recordedRange,
                  const std::function<void(uint64_t frame)>& onSeekFrame,
                  const std::function<bool(uint64_t frame)>& drawPreview)
{
    const auto* pack = session.Pack();
    const auto& check = session.Check();
    const auto& act = session.Activity();
    const auto& shown = session.PartsShown();
    const auto& loaded = session.PartsLoaded();
    const auto& names = session.PartNames();
    const int fps = session.Fps();
    auto Time = [fps](uint32_t frame) { return SongTime(frame, fps); };
    if (!pack || !check.valid || check.rows.empty() || !check.frames) {
        ImGui::TextDisabled("The song's rows are not known yet: check the song");
        return;
    }
    const auto& groups = pack->Groups();
    const auto& tracks = pack->Tracks();
    const auto& rows = check.rows;
    const int rowCount = static_cast<int>(rows.size());
    // the song, and past its end as far as the demo has been seen running
    double total = static_cast<double>(check.frames);
    for (const auto* runs : {&shown, &loaded})
        if (!runs->empty())
            total = std::max(total, static_cast<double>(runs->back().to));

    // the x axis's choice, top right, as the frame scrubber has its unit; kept by the window
    const float keyY = ImGui::GetCursorScreenPos().y;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const ImGuiID axisId = ImGui::GetID("##timeline_axis");
    int axis = std::clamp(ImGui::GetStateStorage()->GetInt(axisId, kAxisTime), 0, 2);

    // the phases' key
    for (int ph = 1; ph <= core::kExocetPhases; ++ph) {
        const float h = ImGui::GetTextLineHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + h, p.y + h), PhaseColour(ph),
                                                  h * 0.2f);
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine();
        ImGui::TextUnformatted(PhaseName(ph));
        ImGui::SameLine(0, ImGui::GetFontSize());
    }
    ImGui::TextDisabled(
        "the upper half of a part's lane: on the screen; the lower: the disk for it");
    // and the interpolations', the lower half of a group's lane
    for (int in = 0; in <= 3; ++in) {
        const float h = ImGui::GetTextLineHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + h, p.y + h), InterpColour(in),
                                                  h * 0.2f);
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine();
        ImGui::TextUnformatted(InterpName(in));
        ImGui::SameLine(0, ImGui::GetFontSize());
    }
    ImGui::TextDisabled("the lower half of a group's lane: its keys, to the next by interpolation");
    {
        const ImVec2 after = ImGui::GetCursorScreenPos();
        const float comboW = ImGui::CalcTextSize("Frame").x + ImGui::GetFrameHeight() * 2.0f;
        const float labelW = ImGui::CalcTextSize("X axis").x + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorScreenPos(ImVec2(right - comboW - labelW, keyY));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("X axis");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::Combo("##axis", &axis, kAxisNames, IM_ARRAYSIZE(kAxisNames)))
            ImGui::GetStateStorage()->SetInt(axisId, axis);
        ImGui::SetCursorScreenPos(
            ImVec2(after.x, std::max(after.y, keyY + ImGui::GetFrameHeightWithSpacing())));
    }

    // a lane a part, then a lane a group; labels on the left, the time across the rest
    const float lineH = ImGui::GetTextLineHeight();
    float labelW = ImGui::CalcTextSize("director").x;
    for (const auto& g : groups)
        labelW = std::max(labelW, ImGui::CalcTextSize(g.name.c_str()).x);
    for (const auto& n : names)
        labelW = std::max(labelW, ImGui::CalcTextSize(n.c_str()).x);
    labelW += lineH + 12.0f;
    const int partLanes = static_cast<int>(names.size());
    const int lanes = partLanes + static_cast<int>(groups.size());
    const float gap = partLanes ? lineH * 0.6f : 0.0f; // between the parts and the groups
    const float axisH = lineH + 8.0f;
    // A lane a line high, or filling the room left (a line kept for the note under it)
    float laneH = lineH + 6.0f;
    if (fill && lanes > 0) {
        const float room = ImGui::GetContentRegionAvail().y - gap - axisH -
                           ImGui::GetTextLineHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y;
        laneH = std::clamp(room / static_cast<float>(lanes), laneH, lineH * 2.5f);
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, labelW + 100.0f);
    const float barX0 = origin.x + labelW, barX1 = origin.x + width - 4.0f;
    const float height = laneH * static_cast<float>(lanes) + gap + axisH;
    ImGui::InvisibleButton("##timeline", ImVec2(width, height));
    const ImGuiID timelineId = ImGui::GetItemID();
    const bool itemPressed = ImGui::IsItemActivated(), itemActive = ImGui::IsItemActive();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // what lies under the mouse is only what shows: the lanes scrolled off are left of the bar
    const bool hovered =
        ImGui::IsItemHovered() && mouse.x >= barX0 - 4.0f && mouse.x < barX1 + 4.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Ctrl+wheel zooms the song across, about the mouse, and a scroll bar under the axis (or
    // Shift+wheel) moves along it; kept by the window. The video's timeline is the whole song
    const ImGuiID zoomId = ImGui::GetID("##timeline_zoom");
    const ImGuiID scrollId = ImGui::GetID("##timeline_scroll");
    const float viewW = barX1 - barX0;
    float zoom =
        fill ? 1.0f : std::clamp(ImGui::GetStateStorage()->GetFloat(zoomId, 1.0f), 1.0f, 256.0f);
    float scroll = ImGui::GetStateStorage()->GetFloat(scrollId, 0.0f);
    if (hovered && !fill) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && io.MouseWheel != 0.0f) {
            const float at = (mouse.x - barX0 + scroll) / (viewW * zoom);
            zoom = std::clamp(zoom * std::pow(1.25f, io.MouseWheel), 1.0f, 256.0f);
            scroll = at * viewW * zoom - (mouse.x - barX0);
            ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, timelineId);
        } else if (zoom > 1.0f &&
                   (io.MouseWheelH != 0.0f || (io.KeyShift && io.MouseWheel != 0.0f))) {
            const float wheel = io.MouseWheelH != 0.0f ? -io.MouseWheelH : -io.MouseWheel;
            scroll += wheel * viewW * 0.1f;
            ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, timelineId);
            ImGui::SetKeyOwner(ImGuiKey_MouseWheelX, timelineId);
        }
    }
    const float contentW = viewW * zoom;
    scroll = std::clamp(scroll, 0.0f, contentW - viewW);
    ImGui::GetStateStorage()->SetFloat(zoomId, zoom);
    ImGui::GetStateStorage()->SetFloat(scrollId, scroll);
    // the lanes and the axis drawn inside the bar; unzoomed, the first label may overhang it
    const ImVec2 outerMin = dl->GetClipRectMin(), outerMax = dl->GetClipRectMax();
    dl->PushClipRect(ImVec2(zoom > 1.0f ? barX0 : origin.x, origin.y),
                     ImVec2(barX1 + 4.0f, origin.y + height), true);

    // A song frame's place across, in the axis's units: frames, or played rows - a row's own
    // frames spread over it, and past the song's end rows of its mean length
    const double meanRow = static_cast<double>(check.frames) / rowCount;
    auto unitOf = [&](double f) -> double {
        if (axis != kAxisRow)
            return f;
        if (f >= check.frames)
            return rowCount + (f - check.frames) / meanRow;
        const int r = std::max(0, check.RowAtFrame(static_cast<uint32_t>(std::max(0.0, f))));
        return r + (f - rows[r].frame) / std::max<double>(1, rows[r].frames);
    };
    const double totalUnits = unitOf(total);
    auto xOf = [&](double f) {
        return barX0 - scroll + static_cast<float>(unitOf(f) / totalUnits) * contentW;
    };
    // and back: the song frame under x
    auto frameAt = [&](float x) -> double {
        const double u = (x - barX0 + scroll) / contentW * totalUnits;
        if (axis != kAxisRow)
            return u;
        if (u >= rowCount)
            return check.frames + (u - rowCount) * meanRow;
        const int r = std::clamp(static_cast<int>(u), 0, rowCount - 1);
        return rows[r].frame + (u - r) * rows[r].frames;
    };
    auto frameOfRow = [&](int r) -> double {
        return r >= rowCount ? static_cast<double>(check.frames) : rows[std::max(0, r)].frame;
    };
    // a key's song frame: its row's, and its sub in 256ths of the row
    auto keyFrame = [&](const core::ExocetKey& k) -> double {
        if (k.row >= rowCount)
            return static_cast<double>(check.frames);
        return rows[k.row].frame + rows[k.row].frames * (k.sub / 256.0);
    };
    auto laneTop = [&](int lane) {
        return origin.y + laneH * static_cast<float>(lane) + (lane >= partLanes ? gap : 0.0f);
    };
    const float axisY = laneTop(lanes);

    // The frame history: the song frames whose machine frame it still holds. The one of them
    // nearest `f`, where a drag of the playhead goes
    uint64_t histFirst = 0, histLast = 0;
    const bool history = recordedRange && recordedRange(histFirst, histLast);
    auto nearestHeld = [&](double f, uint32_t& song, uint64_t& emu) {
        if (!history)
            return false;
        const auto& emuFrames = session.EmuFrames();
        auto held = [&](auto it) { return it->second >= histFirst && it->second <= histLast; };
        const auto at = emuFrames.lower_bound(static_cast<uint32_t>(std::max(0.0, std::round(f))));
        auto after = at, before = at;
        while (after != emuFrames.end() && !held(after))
            ++after;
        while (before != emuFrames.begin() && !held(std::prev(before)))
            --before;
        if (before != emuFrames.begin() &&
            (after == emuFrames.end() || f - std::prev(before)->first <= after->first - f))
            after = std::prev(before);
        else if (after == emuFrames.end())
            return false;
        song = after->first;
        emu = after->second;
        return true;
    };
    // and the machine frame of one within a frame or two of `f`, as the band bridges them: where
    // a tooltip shows the screen
    auto heldAt = [&](double f, uint64_t& emu) {
        const long at = std::lround(f);
        for (long d : {0, -1, 1, -2, 2})
            if (history && at + d >= 0 && session.EmuFrameOf(static_cast<uint32_t>(at + d), emu) &&
                emu >= histFirst && emu <= histLast)
                return true;
        return false;
    };

    // The playhead, dragged through the frame history: a press on it takes it, and it goes to
    // the held song frame nearest the mouse, the machine sought there as it moves
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID dragId = ImGui::GetID("##timeline_drag");
    const ImGuiID dragDxId = ImGui::GetID("##timeline_drag_dx"); // the press's way off the line
    const ImGuiID dragAtId = ImGui::GetID("##timeline_drag_at"); // the song frame last sought
    uint32_t headSong = 0;
    uint64_t headEmu = 0;
    const bool seekable = onSeekFrame && b.present && nearestHeld(b.frame, headSong, headEmu);
    const float headX = xOf(b.frame);
    const bool onHead = seekable && hovered &&
                        std::abs(mouse.x - headX) <= std::max(4.0f, lineH * 0.35f) &&
                        mouse.y < axisY + 4.0f;
    if (itemPressed && onHead) {
        store->SetBool(dragId, true);
        store->SetFloat(dragDxId, mouse.x - headX);
        store->SetInt(dragAtId, static_cast<int>(b.frame));
    } else if (!itemActive) {
        store->SetBool(dragId, false);
    }
    const bool dragging = itemActive && store->GetBool(dragId);
    double headFrame = b.frame;
    std::string dragTip;
    if (dragging) {
        uint32_t song = 0;
        uint64_t emu = 0;
        if (nearestHeld(frameAt(mouse.x - store->GetFloat(dragDxId)), song, emu)) {
            headFrame = song;
            if (static_cast<int>(song) != store->GetInt(dragAtId)) {
                store->SetInt(dragAtId, static_cast<int>(song));
                onSeekFrame(emu);
            }
            const int r = song < check.frames ? check.RowAtFrame(song) : -1;
            dragTip = fmt::format(
                "{}, frame {}{} - machine frame {}", Time(song), song,
                r >= 0 ? fmt::format(", row {} (position {}, row {})", r, rows[r].pos, rows[r].row)
                       : std::string(", after the song"),
                emu);
        }
    }
    if (dragging || onHead)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    const ImU32 laneBg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 seenBg = ImGui::GetColorU32(ImGuiCol_FrameBgHovered, 0.35f);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    std::string tip;
    auto label = [&](float y0, ImU32 swatch, const std::string& name) {
        const float ty = y0 + (laneH - lineH) * 0.5f - 1.0f;
        // out of the bar's clip, into the window's
        dl->PushClipRect(ImMax(outerMin, ImVec2(origin.x, y0)),
                         ImMin(outerMax, ImVec2(barX0, y0 + laneH)));
        dl->AddRectFilled(ImVec2(origin.x, ty + 1), ImVec2(origin.x + lineH - 2, ty + lineH - 1),
                          swatch, lineH * 0.2f);
        dl->AddText(ImVec2(origin.x + lineH + 4, ty), text, name.c_str());
        dl->PopClipRect();
    };

    // the parts: what was on the screen, and under it what the disk was doing for it
    for (int p = 0; p < partLanes; ++p) {
        const float y0 = laneTop(p), y1 = y0 + laneH - 2.0f, ym = (y0 + y1) * 0.5f;
        label(y0, PhaseColour(4), names[p]);
        dl->AddRectFilled(ImVec2(barX0, y0), ImVec2(barX1, y1), laneBg);
        for (const auto* runs : {&shown, &loaded}) {
            const bool disk = runs == &loaded;
            for (const auto& r : *runs) {
                if (r.part != p + 1)
                    continue;
                const ImVec2 a(xOf(r.from), disk ? ym + 1 : y0 + 1);
                const ImVec2 z(std::max(xOf(r.to), a.x + 1.5f), disk ? y1 - 1 : ym);
                dl->AddRectFilled(a, z, PhaseColour(r.phase, disk ? 200 : 235), 2.0f);
                if (hovered && mouse.x >= a.x && mouse.x < z.x && mouse.y >= a.y && mouse.y < z.y)
                    tip = fmt::format("{} {}{} - {} to {}, {:.1f} s", names[p], PhaseName(r.phase),
                                      disk ? " (the disk)" : "", Time(r.from), Time(r.to),
                                      (r.to - r.from) / static_cast<double>(fps));
            }
        }
    }

    // the groups: the rows the demo ran each on, and its events' keys (the cues, the cuts), in
    // the order they first ran, as the grid has them
    const std::vector<int> groupOrder = GroupsByFirstRun(*pack, act);
    for (size_t lane = 0; lane < groupOrder.size(); ++lane) {
        const int i = groupOrder[lane];
        const auto& g = groups[i];
        const float y0 = laneTop(partLanes + static_cast<int>(lane)), y1 = y0 + laneH - 2.0f;
        label(y0, GroupColour(i), g.name);
        dl->AddRectFilled(ImVec2(barX0, y0), ImVec2(barX1, y1), laneBg);
        for (const auto& [a, z] : act.SeenSpans())
            dl->AddRectFilled(ImVec2(xOf(frameOfRow(a)), y0), ImVec2(xOf(frameOfRow(z + 1)), y1),
                              seenBg);
        // a group with value tracks splits its lane as a part's does: the rows it ran on above,
        // its keys' work below
        bool keyed = false;
        for (const auto& t : tracks)
            keyed |= t.group == g.name && t.type != "event";
        const float ym = keyed ? (y0 + y1) * 0.5f : y1 - 2;
        for (const auto& [a, z] : act.ActiveSpans(g.name)) {
            const ImVec2 p0(xOf(frameOfRow(a)), y0 + 2);
            const ImVec2 p1(std::max(xOf(frameOfRow(z + 1)), p0.x + 2.0f), ym);
            dl->AddRectFilled(p0, p1, GroupColour(i, 220), 3.0f);
            if (hovered && mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= y0 && mouse.y < ym)
                tip = fmt::format("{}: rows {}-{}, {} to {}\n{}", g.name, a, z,
                                  Time(static_cast<uint32_t>(frameOfRow(a))),
                                  Time(static_cast<uint32_t>(frameOfRow(z + 1))), g.text);
        }
        // the keys: each one's span to the next in its interpolation's colour, a tick on each
        for (const auto& t : tracks) {
            if (!keyed || t.group != g.name || t.type == "event")
                continue;
            const auto& keys = session.HostKeys(t.index);
            for (size_t k = 0; k < keys.size() && keys[k].row < rowCount; ++k) {
                const float x = xOf(keyFrame(keys[k]));
                if (k + 1 < keys.size()) {
                    const ImVec2 p0(x, ym + 1), p1(xOf(keyFrame(keys[k + 1])), y1 - 1);
                    dl->AddRectFilled(p0, ImVec2(std::max(p1.x, x + 1.0f), p1.y),
                                      InterpColour(keys[k].interp, 150));
                    if (hovered && mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y &&
                        mouse.y < p1.y)
                        tip = fmt::format("{} {} - rows {} to {}, {} to {}", t.name,
                                          InterpName(keys[k].interp), keys[k].row, keys[k + 1].row,
                                          Time(static_cast<uint32_t>(keyFrame(keys[k]))),
                                          Time(static_cast<uint32_t>(keyFrame(keys[k + 1]))));
                }
                dl->AddLine(ImVec2(x, ym + 1), ImVec2(x, y1 - 1), InterpColour(keys[k].interp));
            }
        }
        for (const auto& t : tracks) {
            if (t.group != g.name || t.type != "event")
                continue;
            for (const auto& k : session.HostKeys(t.index)) {
                if (k.row >= rowCount)
                    continue;
                const double f = keyFrame(k);
                const float x = xOf(f);
                dl->AddTriangleFilled(ImVec2(x - 4, y0), ImVec2(x + 4, y0), ImVec2(x, y0 + 7),
                                      text);
                dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), text);
                if (hovered && std::abs(mouse.x - x) <= 4.0f && mouse.y >= y0 && mouse.y < y1) {
                    std::string name = fmt::format("{}", k.value);
                    for (const auto& e : t.entries)
                        if (e.value == k.value)
                            name = e.name;
                    tip = fmt::format("{} {} - row {}, {}", t.name, name, k.row,
                                      Time(static_cast<uint32_t>(f)));
                }
            }
        }
    }

    // the axis across the bottom, a tick as often as its labels fit: every 10, 15, 30, 60 or 120
    // seconds; a round number of frames; a power of two of rows, up to the song's last. And the
    // song's end where the demo runs past it
    auto tick = [&](double f, const std::string& t) {
        const float x = xOf(f);
        if (x < barX0 - 100.0f || x > barX1 + 100.0f) // scrolled off
            return;
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, axisY + 3),
                    (dim & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 64));
        dl->AddText(ImVec2(x - ImGui::CalcTextSize(t.c_str()).x * 0.5f, axisY + 4), dim, t.c_str());
    };
    // the first of `steps` (in the axis's units) at least `labelPx` apart, else the last; the
    // `fine` ones before them only once zoomed in
    auto pickStep = [&](std::initializer_list<int> fine, std::initializer_list<int> steps,
                        float labelPx) {
        int step = *(steps.end() - 1);
        auto fits = [&](int s) { return contentW * s / totalUnits >= labelPx; };
        if (zoom > 1.0f)
            for (int s : fine)
                if (fits(s))
                    return s;
        for (int s : steps)
            if (fits(s)) {
                step = s;
                break;
            }
        return step;
    };
    if (axis == kAxisTime) {
        const double seconds = total / static_cast<double>(fps);
        const int step =
            pickStep({fps, 2 * fps, 5 * fps}, {10 * fps, 15 * fps, 30 * fps, 60 * fps, 120 * fps},
                     ImGui::CalcTextSize("00:00").x + 12.0f) /
            fps;
        for (int s = 0; s <= static_cast<int>(seconds); s += step)
            tick(s * static_cast<double>(fps), fmt::format("{}:{:02}", s / 60, s % 60));
    } else if (axis == kAxisFrame) {
        const int step =
            pickStep({1, 5, 10, 25, 50}, {100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000},
                     ImGui::CalcTextSize("000000").x + 12.0f);
        for (int f = 0; f <= static_cast<int>(total); f += step)
            tick(f, std::to_string(f));
    } else {
        const int step = pickStep({1, 2, 4, 8}, {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192},
                                  ImGui::CalcTextSize("00000").x + 12.0f);
        for (int r = 0; r < rowCount; r += step)
            tick(rows[r].frame, std::to_string(r));
    }
    if (total > check.frames) {
        const float x = xOf(check.frames);
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, axisY), IM_COL32(230, 200, 90, 200), 1.5f);
        if (hovered && std::abs(mouse.x - x) <= 3.0f)
            tip =
                fmt::format("the song ends, {}: the timeline's keys stop here", Time(check.frames));
    }
    // the frame history: the song frames whose machine frame it still holds - the ones a grid
    // row's Frame or Time rewinds to, and the playhead drags through - as a band along the top of
    // the time axis, in the grid's colour for them
    if (history) {
        const ImU32 band = IM_COL32(153, 191, 255, 200);
        const float hy0 = axisY + 1.0f, hy1 = axisY + 4.0f;
        auto span = [&](uint32_t from, uint32_t to) {
            const float x0 = xOf(from), x1 = std::max(xOf(to + 1.0), x0 + 1.0f);
            dl->AddRectFilled(ImVec2(x0, hy0), ImVec2(x1, hy1), band);
            dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, axisY), IM_COL32(153, 191, 255, 14));
            if (hovered && mouse.x >= x0 && mouse.x < x1 && mouse.y >= hy0 - 2.0f &&
                mouse.y < hy1 + 3.0f)
                tip = fmt::format("The frame history holds {} to {} ({:.1f} s): {}a row's Frame "
                                  "or Time in the grid rewinds there",
                                  Time(from), Time(to + 1),
                                  (to + 1 - from) / static_cast<double>(fps),
                                  onSeekFrame ? "drag the playhead into it, or " : "");
        };
        bool open = false;
        uint32_t from = 0, last = 0;
        for (const auto& [song, emu] : session.EmuFrames()) {
            const bool held = emu >= histFirst && emu <= histLast;
            if (held && open && song <= last + 2) { // a missed frame or two bridged
                last = song;
                continue;
            }
            if (open)
                span(from, last);
            open = held;
            from = last = song;
        }
        if (open)
            span(from, last);
    }
    // the bookmarks: a tab with its slot along the top, a faint line down through the lanes; a
    // click on the tab shows its row in the grid
    for (const auto& [row, mark] : session.BookmarkRows()) {
        if (row >= rowCount)
            continue;
        const float x = xOf(rows[row].frame), s = lineH * 0.9f;
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, axisY),
                    (kBookmarkColour & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 110));
        const ImVec2 t0(x, origin.y), t1(x + s, origin.y + s);
        dl->AddRectFilled(t0, t1, kBookmarkColour, s * 0.2f, ImDrawFlags_RoundCornersRight);
        if (mark.slot) {
            const std::string digit = std::to_string(mark.slot);
            const float size = ImGui::GetFontSize() * 0.8f;
            const ImVec2 d = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, digit.c_str());
            dl->AddText(ImGui::GetFont(), size,
                        ImVec2(x + (s - d.x) * 0.5f, origin.y + (s - d.y) * 0.5f),
                        IM_COL32(16, 24, 40, 255), digit.c_str());
        }
        if (hovered && mouse.x >= t0.x - 1 && mouse.x < t1.x + 1 && mouse.y >= t0.y &&
            mouse.y < t1.y) {
            tip =
                fmt::format("Bookmark{}{} - row {}, {}{}",
                            mark.slot ? fmt::format(" {}", mark.slot) : std::string(),
                            mark.name.empty() ? std::string() : " \"" + mark.name + "\"", row,
                            Time(rows[row].frame), onShowRow ? "\nclick: show it in the grid" : "");
            if (onShowRow && !onHead && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                onShowRow(row);
        }
    }
    // the grid's breakpoints: a row's a dot on the axis, a key breakpoint's a ring at each of its
    // track's keys, a line up through the lanes for either
    {
        const ImU32 red = IM_COL32(230, 60, 60, 255);
        const float r = lineH * 0.3f, cy = axisY - r - 1.0f;
        auto mark = [&](int row, bool ring, const std::string& what) {
            if (row < 0 || row >= rowCount)
                return;
            const float x = xOf(rows[row].frame);
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, cy), IM_COL32(230, 60, 60, 110));
            if (ring)
                dl->AddCircle(ImVec2(x, cy), r, red, 0, 2.0f);
            else
                dl->AddCircleFilled(ImVec2(x, cy), r, red);
            if (hovered && std::abs(mouse.x - x) <= r + 1.0f && std::abs(mouse.y - cy) <= r + 2.0f)
                tip = fmt::format("{} - row {}, {}", what, row, Time(rows[row].frame));
        };
        const std::set<int> keyBreaks = session.KeyBreaks(); // resolved by name: once
        for (const auto& t : tracks)
            if (keyBreaks.count(t.index))
                for (const auto& k : session.HostKeys(t.index))
                    mark(k.row, true, "Key breakpoint on " + t.name);
        for (const auto& [song, pos, row, pass] : session.RowBreakPlaces())
            mark(check.FindRow(song, pos, row, pass), false, "Breakpoint");
    }
    // the playhead - while it is dragged, where it is going - with a grip on top when the frame
    // history has somewhere to drag it
    if (b.present) {
        const ImU32 red = IM_COL32(255, 80, 80, 255);
        const float x = xOf(headFrame);
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, axisY), red, 2.0f);
        if (seekable) {
            const float s = lineH * 0.4f;
            dl->AddTriangleFilled(ImVec2(x - s, origin.y), ImVec2(x + s, origin.y),
                                  ImVec2(x, origin.y + s * 1.4f), red);
        }
    }
    dl->PopClipRect();
    // zoomed in: the scroll bar along the song, under the axis
    if (zoom > 1.0f) {
        const float barH = ImGui::GetStyle().ScrollbarSize;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImS64 pos = static_cast<ImS64>(scroll);
        if (ImGui::ScrollbarEx(ImRect(barX0, at.y, barX1, at.y + barH),
                               ImGui::GetID("##timeline_scrollbar"), ImGuiAxis_X, &pos,
                               static_cast<ImS64>(viewW), static_cast<ImS64>(contentW)))
            ImGui::GetStateStorage()->SetFloat(scrollId, static_cast<float>(pos));
        ImGui::Dummy(ImVec2(width, barH));
    }
    if (hovered) {
        if (onHead && !dragging)
            tip = fmt::format("The playhead, {}: drag it through the frame history", Time(b.frame));
        if (tip.empty() && mouse.x >= barX0 && mouse.x < barX1) {
            const double f = frameAt(mouse.x);
            if (f < check.frames) {
                const int r = check.RowAtFrame(static_cast<uint32_t>(f));
                if (r >= 0)
                    tip = fmt::format("row {} (position {}, row {}), {}, frame {}{}", r,
                                      rows[r].pos, rows[r].row, Time(static_cast<uint32_t>(f)),
                                      static_cast<uint32_t>(f),
                                      onShowRow ? "\ndouble-click: show it in the grid" : "");
                if (!fill)
                    tip += "\nCtrl+wheel: zoom; Shift+wheel: scroll";
            } else {
                tip = fmt::format("{}, frame {}, after the song", Time(static_cast<uint32_t>(f)),
                                  static_cast<uint32_t>(f));
            }
        }
        // a double-click anywhere across the song: the grid, on the row under the mouse
        if (onShowRow && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && mouse.x >= barX0 &&
            mouse.x < barX1) {
            const double f = frameAt(mouse.x);
            if (f < check.frames) {
                const int r = check.RowAtFrame(static_cast<uint32_t>(f));
                if (r >= 0)
                    onShowRow(r);
            }
        }
    }
    if (dragging)
        tip = dragTip;
    if ((hovered || dragging) && !tip.empty()) {
        // over the frame history, the screen of the frame under the mouse on the tip's left
        uint64_t emu = 0;
        ImGui::BeginTooltip();
        if (drawPreview && heldAt(dragging ? headFrame : frameAt(mouse.x), emu) && drawPreview(emu))
            ImGui::SameLine();
        ImGui::TextUnformatted(tip.c_str());
        ImGui::EndTooltip();
    }
    if (!partLanes && b.present && b.version < 3)
        ImGui::TextDisabled("This build's block has no parts (version 3): only the groups show");
    else if (!act.SeenRows() && shown.empty())
        ImGui::TextDisabled("Run the demo to fill it in: the parts are recorded as it plays");
}

} // namespace adm::ui::exo
