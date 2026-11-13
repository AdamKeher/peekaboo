#include "ExocetControlView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "../../core/ExocetBlock.h"
#include "../../core/ExocetEval.h"
#include "../../core/ExocetSession.h"
#include "../IconsFontAwesome6.h"
#include "ExocetSong.h"
#include "ExocetTimeline.h"
#include "ExocetUi.h"

namespace adm::ui {

using core::ExocetSession;
using exo::ToolbarFlow;
using exo::TooltipWhenDisabled;

namespace {

constexpr ImVec4 kWarnColour = {1.00f, 0.78f, 0.30f, 1.00f};
constexpr ImVec4 kBlockColour = {1.00f, 0.40f, 0.35f, 1.00f};

} // namespace

void ExocetControlView::Draw()
{
    if (!m_isOpen)
        return;
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }
    ImGui::SetNextWindowSize(ImVec2(760, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_SCREWDRIVER_WRENCH " Exocet###ExocetControl", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }
    DrawToolbar();
    DrawTrustBanner();
    DrawMessages();
    ImGui::Separator();
    // the actions stay put; what the file holds scrolls under them when the window is short
    ImGui::BeginChild("##info", ImVec2(0, 0), ImGuiChildFlags_None);
    DrawInfo();
    ImGui::EndChild();
    ImGui::End();
}

void ExocetControlView::DrawToolbar()
{
    auto* pack = m_session->Pack();
    ToolbarFlow bar(ImGui::GetFrameHeight());

    // the pack
    bar.Begin("##tb_pack");
    ImGui::AlignTextToFramePadding();
    if (pack) {
        const std::string name = std::filesystem::path(pack->Path()).filename().string();
        ImGui::TextUnformatted((name + (pack->Dirty() ? " *" : "")).c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", pack->Path().c_str());
    } else {
        ImGui::TextDisabled("%s", m_session->PackError().empty() ? "No .exocet beside the disk"
                                                                 : m_session->PackError().c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!pack || !pack->Dirty());
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save"))
        m_session->Save();
    ImGui::EndDisabled();
    TooltipWhenDisabled(!pack ? "No .exocet to save"
                              : (pack->Dirty() ? "" : "No changes (Ctrl+S in the grid)"));
    bar.End();

    // what the kit makes, into the machine and onto the disk
    bar.Begin("##tb_kit");
    exo::PushButton(*m_session);

    ImGui::SameLine();
    const std::string injectWhy = m_session->InjectBlockedReason();
    ImGui::BeginDisabled(!injectWhy.empty());
    if (ImGui::Button(ICON_FA_VOLUME_HIGH " Inject"))
        m_session->Inject();
    ImGui::EndDisabled();
    TooltipWhenDisabled(injectWhy.empty()
                            ? "Put the module as saved into the running demo at the row it is on, "
                              "with the keys re-fitted to it; clears the frame history"
                            : injectWhy);

    ImGui::SameLine();
    const std::string writeWhy = m_session->WriteBlockedReason();
    ImGui::BeginDisabled(!writeWhy.empty());
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Write ADF"))
        m_session->WriteAdf();
    ImGui::EndDisabled();
    TooltipWhenDisabled(writeWhy.empty() ? "A new disk from this one with the keys and the module "
                                           "as they are, by the kit - over the pack's disk, the "
                                           "old one kept as .bak; the .exocet is saved with it"
                                         : writeWhy);
    if (m_session->WroteDisk()) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLAY " Boot the new disk"))
            m_session->BootWrittenDisk();
    }
    bar.End();

    bar.Begin("##tb_check");
    ImGui::BeginDisabled(m_session->GetKitState() != ExocetSession::KitState::Ready ||
                         m_session->Busy());
    if (ImGui::Button(ICON_FA_REPEAT " Check"))
        m_session->RequestCheck();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (m_session->Busy())
        ImGui::TextColored(kWarnColour, "%s...", m_session->BusyWhat().c_str());
    else if (m_session->Check().valid)
        ImGui::TextDisabled("checked in %.1f s", m_session->LastCheckSeconds());
    else
        ImGui::TextDisabled("not checked");
    bar.End();

    // other windows, and what the kit runs with
    bar.Begin("##tb_more");
    if (ImGui::Button(ICON_FA_SLIDERS " Music") && m_onOpenMusic)
        m_onOpenMusic();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The samples: which go onto the disk 8-bit and which 4-bit, at half "
                          "the size - and listening to them both ways");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_MEMORY " Memory") && m_onOpenMemory)
        m_onOpenMemory();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Exocet Memory: every allocation the disk's programs made, over exec's "
                          "free memory");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_SLIDERS " Tools")) {
        if (m_getTools) {
            const auto [py, tools] = m_getTools();
            std::snprintf(m_python, sizeof m_python, "%s", py.c_str());
            std::snprintf(m_tools, sizeof m_tools, "%s", tools.c_str());
        }
        ImGui::OpenPopup("##tools");
    }
    if (ImGui::BeginPopup("##tools")) {
        const auto& v = m_session->Tools();
        ImGui::TextUnformatted("The Python and the build tools the kit runs with.");
        ImGui::SetNextItemWidth(420);
        ImGui::InputTextWithHint("Python", "python (on PATH)", m_python, sizeof m_python);
        ImGui::SetNextItemWidth(420);
        ImGui::InputTextWithHint("Tools", "EXOCET_TOOLS: the folder with salvador", m_tools,
                                 sizeof m_tools);
        ImGui::TextDisabled(
            "Found: Python %s, lz4 %s, salvador %s", v.python.empty() ? "-" : v.python.c_str(),
            v.lz4.empty() ? "-" : v.lz4.c_str(), v.salvador.empty() ? "-" : v.salvador.c_str());
        if (ImGui::Button("Apply") && m_onToolsChanged) {
            m_onToolsChanged(m_python, m_tools);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    bar.End();
}

void ExocetControlView::DrawTrustBanner()
{
    using KS = ExocetSession::KitState;
    const auto state = m_session->GetKitState();
    if (state == KS::Untrusted) {
        const auto& kit = m_session->Kit();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.25f, 0.05f, 0.45f));
        // sized to what it says: the button must never be clipped off the bottom
        ImGui::BeginChild("##trust", ImVec2(0, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::TextWrapped("This pack's kit is code, and it has not run on this machine: "
                           "production \"%s\", folder %s, hash %s. The rows, a push and a new "
                           "disk all come from it.",
                           kit.production.c_str(), kit.dir.c_str(), m_session->KitHash().c_str());
        if (ImGui::Button(ICON_FA_LOCK " Trust this kit and run it"))
            m_session->TrustKit();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else if (state == KS::Missing) {
        ImGui::TextColored(kWarnColour, "The kit is missing: %s", m_session->KitError().c_str());
    }
}

void ExocetControlView::DrawMessages()
{
    // what the last push, inject, write and save did
    ToolbarFlow status(ImGui::GetTextLineHeight());
    status.Message(m_session->LastPush());
    status.Message(m_session->LastInject());
    status.Message(m_session->LastWrite());
    if (!m_session->SaveError().empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kBlockColour, "Save failed: %s", m_session->SaveError().c_str());
        ImGui::PopTextWrapPos();
    }
}

void ExocetControlView::Recount()
{
    const auto* pack = m_session->Pack();
    const auto& check = m_session->Check();
    if (pack == m_statsPack && pack && m_statsRevision == pack->Revision() &&
        m_statsCheck == m_session->CheckGeneration())
        return;
    m_statsPack = pack;
    m_statsRevision = pack ? pack->Revision() : ~0ull;
    m_statsCheck = m_session->CheckGeneration();
    m_stats = Stats{};
    if (!pack)
        return;
    Stats& s = m_stats;
    const auto& groups = pack->Groups();
    s.groups.resize(groups.size());
    s.tracks = static_cast<int>(pack->Tracks().size());
    for (const auto& t : pack->Tracks()) {
        auto type = std::find_if(s.types.begin(), s.types.end(),
                                 [&](const auto& p) { return p.first == t.type; });
        if (type == s.types.end())
            s.types.push_back({t.type, 1});
        else
            ++type->second;
        const auto keys = pack->Keys(t.name);
        const auto g = std::find_if(groups.begin(), groups.end(),
                                    [&](const auto& x) { return x.name == t.group; });
        Stats::Group* group = g == groups.end() ? nullptr : &s.groups[g - groups.begin()];
        if (group) {
            ++group->tracks;
            group->keyed += !keys.empty();
            group->keys += static_cast<int>(keys.size());
        }
        s.keyedTracks += !keys.empty();
        s.keys += static_cast<int>(keys.size());
        for (const auto& k : keys) {
            const int i = core::ExocetInterpFromName(k.interp);
            ++s.interps[std::clamp(i, 0, 3)];
            if (!check.valid)
                continue;
            const int row = check.FindRow(k.song, k.pos, k.row, k.pass);
            if (row < 0) {
                ++s.offSong;
                continue;
            }
            const int64_t f = check.rows[row].frame;
            s.firstFrame = s.firstFrame < 0 ? f : std::min(s.firstFrame, f);
            s.lastFrame = std::max(s.lastFrame, f);
        }
    }
    for (const auto& name : pack->KeyedTracks())
        if (!pack->FindTrack(name))
            s.orphans.push_back(name);
}

namespace {

// A card: its title ruled across the column, then a row per fact, the label dim
bool BeginCard(const char* title)
{
    ImGui::SeparatorText(title);
    if (!ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingFixedFit))
        return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void Fact(const char* label, const std::string& value, const std::string& tip = {},
          const ImVec4* colour = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    if (colour)
        ImGui::TextColored(*colour, "%s", value.c_str());
    else
        ImGui::TextUnformatted(value.c_str());
    if (!tip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tip.c_str());
}

std::string FileName(const std::string& path)
{
    return path.empty() ? "-" : std::filesystem::path(path).filename().string();
}

std::string Plural(int n, const char* one, const char* many)
{
    return fmt::format("{} {}", n, n == 1 ? one : many);
}

constexpr ImVec4 kOkColour = {0.45f, 0.85f, 0.45f, 1.00f};

} // namespace

void ExocetControlView::DrawInfo()
{
    Recount();
    // without a pack every card is still there, greyed, with nothing in it
    ImGui::BeginDisabled(!m_session->Pack());
    // the cards across in as many columns as fit, a column each when the window is narrow
    const float cardW = ImGui::GetFontSize() * 19.0f;
    const int columns =
        std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / cardW), 1, 3);
    if (ImGui::BeginTable("##cards", columns, ImGuiTableFlags_SizingStretchSame)) {
        using Draw = void (ExocetControlView::*)();
        for (Draw d : {&ExocetControlView::DrawFileCard, &ExocetControlView::DrawSongCard,
                       &ExocetControlView::DrawKeysCard, &ExocetControlView::DrawKitCard,
                       &ExocetControlView::DrawLiveCard}) {
            ImGui::TableNextColumn();
            (this->*d)();
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Timeline");
    uint64_t first = 0, last = 0;
    if (m_recordedRange && m_recordedRange(first, last))
        m_preview.SetRange(first, last);
    exo::DrawTimeline(*m_session, m_session->Block(), false, m_onShowRow, m_recordedRange,
                      m_onSeekFrame, [this](uint64_t frame) {
                          return m_preview.Draw(frame, m_getFrameThumbnail,
                                                ImGui::GetFontSize() * 10.0f);
                      });
    DrawGroups();
    ImGui::EndDisabled();
}

void ExocetControlView::DrawFileCard()
{
    const auto* pack = m_session->Pack();
    if (!BeginCard("File"))
        return;
    Fact("File", pack ? FileName(pack->Path()) : "-", pack ? pack->Path() : "");
    Fact("Production", pack && !pack->Production().empty() ? pack->Production() : "-");
    if (pack && pack->Dirty())
        Fact("State", "unsaved changes", {}, &kWarnColour);
    else
        Fact("State", pack ? "saved" : "-");
    Fact("Undo", pack ? Plural(static_cast<int>(pack->UndoSteps()), "step", "steps") : "-");
    int n[3] = {};
    for (const auto& i : m_session->Issues())
        ++n[i.level == "block" ? 0 : i.level == "warn" ? 1 : 2];
    const std::string issues =
        pack ? fmt::format("{} block, {} warn, {} info", n[0], n[1], n[2]) : "-";
    if (n[0])
        Fact("Issues", issues, "Listed in the Exocet Grid", &kBlockColour);
    else
        Fact("Issues", issues, pack ? "Listed in the Exocet Grid" : "");
    ImGui::EndTable();
}

void ExocetControlView::DrawSongCard()
{
    if (!BeginCard("Song"))
        return;
    for (const auto& f : exo::SongFacts(*m_session))
        Fact(f.label.c_str(), f.value, f.tip, f.colour ? &*f.colour : nullptr);
    ImGui::EndTable();
}

void ExocetControlView::DrawKeysCard()
{
    const auto* pack = m_session->Pack();
    const Stats& s = m_stats;
    if (!BeginCard("Keys"))
        return;
    Fact("Keys", pack ? std::to_string(s.keys) : "-");
    Fact("Tracks", pack ? fmt::format("{} keyed of {}", s.keyedTracks, s.tracks) : "-");
    std::string types;
    for (const auto& [type, n] : s.types)
        types += (types.empty() ? "" : ", ") + fmt::format("{} {}", n, type);
    Fact("Types", types.empty() ? "-" : types);
    Fact("Curves", pack ? fmt::format("{} step, {} linear, {} smooth, {} ramp", s.interps[0],
                                      s.interps[1], s.interps[2], s.interps[3])
                        : "-");
    Fact("Span",
         s.firstFrame < 0 ? std::string("-")
                          : Time(static_cast<uint32_t>(s.firstFrame)) + " to " +
                                Time(static_cast<uint32_t>(s.lastFrame)),
         "From the first key to the last, where the song plays them");
    if (s.offSong)
        Fact("Off the song", Plural(s.offSong, "key", "keys"),
             "On a place the song does not play: the kit leaves them out", &kWarnColour);
    if (!s.orphans.empty()) {
        std::string names;
        for (const auto& n : s.orphans)
            names += (names.empty() ? "" : "\n") + n;
        Fact("Orphaned", Plural(static_cast<int>(s.orphans.size()), "track", "tracks"),
             "Keyed, but the manifest no longer has them:\n" + names, &kWarnColour);
    }
    ImGui::EndTable();
}

void ExocetControlView::DrawKitCard()
{
    using KS = ExocetSession::KitState;
    const auto state = m_session->GetKitState();
    const auto& kit = m_session->Kit();
    if (!BeginCard("Kit"))
        return;
    switch (state) {
        case KS::Ready:
            Fact("State", "trusted", {}, &kOkColour);
            break;
        case KS::Untrusted:
            Fact("State", "not trusted", "Trust it above to run it", &kWarnColour);
            break;
        case KS::Missing:
            Fact("State", "missing", m_session->KitError(), &kBlockColour);
            break;
        default:
            Fact("State", m_session->Pack() ? "none named" : "-");
            break;
    }
    const bool have = state != KS::None && state != KS::Missing;
    Fact("Folder", have ? FileName(kit.dir) : "-", have ? kit.dir : "");
    const std::string hash = m_session->KitHash();
    Fact("Hash", have && !hash.empty() ? hash.substr(0, 12) : "-", have ? hash : "");
    const auto& v = m_session->Tools();
    auto ver = [](const std::string& s) { return s.empty() ? std::string("-") : s; };
    Fact("Python", ver(v.python));
    Fact("lz4, salvador", ver(v.lz4) + ", " + ver(v.salvador));
    Fact("Last check",
         m_session->Check().valid ? fmt::format("{:.1f} s", m_session->LastCheckSeconds()) : "-");
    ImGui::EndTable();
}

void ExocetControlView::DrawLiveCard()
{
    const auto& b = m_session->Block();
    const auto& act = m_session->Activity();
    if (!BeginCard("Live"))
        return;
    if (!b.present) {
        Fact("Block", "none", "No Exocet block in the running machine: the live parts are idle");
    } else if (m_session->ManifestMatches()) {
        Fact("Block", "this build", {}, &kOkColour);
    } else {
        Fact("Block", fmt::format("manifest 0x{:08X}", b.manifest),
             "The running demo was built from another manifest than the .exocet's", &kBlockColour);
    }
    Fact("Playhead",
         b.present ? fmt::format("row {}, {}", b.row, Time(b.frame)) : std::string("-"));
    Fact("Clock", b.present ? (b.clock ? "frame (fallback)" : "music") : "-",
         b.present && b.clock ? "The music is off, or it is another song" : "");
    Fact("Generation", b.present ? fmt::format("{} loaded of {}", b.seen, b.generation) : "-",
         "Bumped by every push; the demo loads it on its next field");
    Fact("Seen", act.Rows() ? fmt::format("{} of {} rows", act.SeenRows(), act.Rows()) : "-",
         "The rows the demo has been seen playing, which the groups' activity is from");
    ImGui::EndTable();
}

void ExocetControlView::DrawGroups()
{
    const auto* pack = m_session->Pack();
    ImGui::SeparatorText("Groups");
    const auto& groups = pack ? pack->Groups() : std::vector<core::ExocetGroupInfo>{};
    const auto& act = m_session->Activity();
    // the description only where there is room for it
    const bool wide = ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 40.0f;
    if (!ImGui::BeginTable("##groups", wide ? 6 : 5,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_SizingFixedFit))
        return;
    ImGui::TableSetupColumn("Group");
    ImGui::TableSetupColumn("Tracks");
    ImGui::TableSetupColumn("Keyed");
    ImGui::TableSetupColumn("Keys");
    ImGui::TableSetupColumn("Active",
                            wide ? ImGuiTableColumnFlags_None : ImGuiTableColumnFlags_WidthStretch);
    if (wide)
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    const float h = ImGui::GetTextLineHeight();
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        const Stats::Group st = i < m_stats.groups.size() ? m_stats.groups[i] : Stats::Group{};
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + h, p.y + h),
                                                  exo::GroupColour(static_cast<int>(i)), h * 0.2f);
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine();
        ImGui::TextUnformatted(g.name.c_str());
        if (!wide && !g.text.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", g.text.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%d", st.tracks);
        ImGui::TableNextColumn();
        ImGui::Text("%d", st.keyed);
        ImGui::TableNextColumn();
        ImGui::Text("%d", st.keys);
        ImGui::TableNextColumn();
        if (!act.SeenRows())
            ImGui::TextDisabled("not seen");
        else if (const int n = act.ActiveRows(g.name))
            ImGui::Text("%d rows", n);
        else
            ImGui::TextDisabled("never");
        if (wide) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", g.text.c_str());
        }
    }
    if (groups.empty()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
}

std::string ExocetControlView::Time(uint32_t frame) const
{
    return exo::SongTime(frame, m_session->Fps());
}

} // namespace adm::ui
