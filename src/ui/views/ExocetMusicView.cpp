#include "ExocetMusicView.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include <fmt/format.h>

#include "../../core/ExocetSession.h"
#include "../../core/JsonRead.h"
#include "../IconsFontAwesome6.h"
#include "ExocetSong.h"
#include "ExocetUi.h"

namespace adm::ui {

using core::ExocetSession;
using exo::TooltipWhenDisabled;

namespace {

constexpr ImVec4 kWarnColour = {1.00f, 0.78f, 0.30f, 1.00f};
constexpr ImVec4 kBlockColour = {1.00f, 0.40f, 0.35f, 1.00f};
constexpr ImVec4 kGoodColour = {0.55f, 0.85f, 0.55f, 1.00f};

// 12,345: the kit's figures are whole bytes
std::string Bytes(uint32_t n)
{
    std::string d = std::to_string(n);
    for (int i = static_cast<int>(d.size()) - 3; i > 0; i -= 3)
        d.insert(static_cast<size_t>(i), ",");
    return d;
}

// How close a 4-bit sample comes, against the bar auto uses: over it, near it, or far under
ImVec4 DbColour(double db, double bar)
{
    if (db >= bar)
        return kGoodColour;
    if (db >= bar - 10.0)
        return kWarnColour;
    return kBlockColour;
}

const char* kSettings[] = {"auto", "8bit", "4bit"};
const char* kSettingLabels[] = {"Auto", "8-bit", "4-bit"};

int SettingIndex(const std::string& s)
{
    for (int i = 0; i < 3; ++i)
        if (s == kSettings[i])
            return i;
    return 0;
}

} // namespace

void ExocetMusicView::Draw()
{
    if (!m_isOpen)
        return;
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }
    ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (!ImGui::Begin(ICON_FA_SLIDERS " Exocet Music###ExocetMusic", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }

    core::ExocetPack* pack = m_session->Pack();
    const core::ExocetCheck& check = m_session->Check();
    const nlohmann::json& music = check.music;
    const double bar = pack ? pack->AutoDb() : core::ExocetPack::kAutoDb;

    exo::ToolbarFlow tb(ImGui::GetFrameHeight());

    // the pack: saved with the keys
    tb.Begin("##mu_pack");
    ImGui::AlignTextToFramePadding();
    if (pack) {
        const std::string name = std::filesystem::path(pack->Path()).filename().string();
        ImGui::TextUnformatted((name + (pack->Dirty() ? " *" : "")).c_str());
    } else {
        ImGui::TextDisabled("No .exocet beside the disk");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!pack || !pack->Dirty());
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save"))
        m_session->Save();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_session->CanUndo());
    if (ImGui::Button(ICON_FA_ROTATE_LEFT))
        m_session->Undo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Undo - a sample's setting or the bar, as a key edit");
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_session->CanRedo());
    if (ImGui::Button(ICON_FA_ROTATE_RIGHT))
        m_session->Redo();
    ImGui::EndDisabled();
    tb.End();

    // the bar auto takes 4-bit over
    tb.Begin("##mu_bar");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Auto: 4-bit over");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    float db = static_cast<float>(bar);
    ImGui::BeginDisabled(!pack);
    if (ImGui::SliderFloat("##autodb", &db, 10.0f, 50.0f, "%.1f dB"))
        m_session->SetAutoDb(db, "autodb");
    if (ImGui::IsItemDeactivatedAfterEdit() && pack)
        pack->BreakMerge();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "A sample left on Auto goes 4-bit when its 4-bit version measures at least this close "
            "to the 8-bit one (signal to noise), and is smaller. %.0f dB is the kit's default: "
            "about where the 4-bit grit is under a sample's own 8-bit noise in a busy mix. "
            "Ctrl+click to type a value.",
            core::ExocetPack::kAutoDb);
    ImGui::SameLine();
    if (ImGui::Button("Default"))
        m_session->SetAutoDb(core::ExocetPack::kAutoDb);
    ImGui::EndDisabled();
    tb.End();

    // listening
    tb.Begin("##mu_listen");
    const std::string why = m_session->AuditionBlockedReason();
    ImGui::BeginDisabled(!pack);
    if (ImGui::Button(ICON_FA_PLAY " Module"))
        m_session->ListenOriginal();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The module as it is, every sample 8-bit, in Peekaboo's player");
    ImGui::SameLine();
    ImGui::BeginDisabled(!why.empty());
    if (ImGui::Button(ICON_FA_VOLUME_HIGH " As the Amiga plays it"))
        m_session->Audition(0);
    ImGui::EndDisabled();
    TooltipWhenDisabled(why.empty() ? "The module with every 4-bit sample as it decodes - the "
                                      "codecs the kit chooses now - in Peekaboo's player"
                                    : why);
    tb.End();
    tb.Message(m_session->LastAudition());

    ImGui::SeparatorText("Song");
    DrawSong();
    ImGui::SeparatorText("Samples");

    if (!check.valid || check.samples.empty()) {
        ImGui::TextDisabled("%s", m_session->Busy() ? "The kit is checking..."
                                                    : "The kit's check reports the samples: "
                                                      "open the Exocet Control window to run it.");
        ImGui::End();
        return;
    }

    // what it comes to
    uint32_t all8 = 0, now = 0, saved = 0;
    int fourBit = 0, used = 0;
    bool assessed = false;
    for (const auto& s : check.samples) {
        if (!s.assessed || !s.used)
            continue;
        assessed = true;
        ++used;
        all8 += s.packed;
        const bool four = s.codec == "4bit";
        now += four ? s.delta4 : s.packed;
        if (four) {
            ++fourBit;
            saved += s.packed > s.delta4 ? s.packed - s.delta4 : 0;
        }
    }
    if (!assessed) {
        ImGui::TextColored(kWarnColour,
                           "This kit reports no codecs: it is older than 4-bit samples. Rebuild "
                           "the disk to get a new one.");
        ImGui::End();
        return;
    }
    ImGui::Text("%d of %d samples 4-bit: %s bytes of samples, %s saved", fourBit, used,
                Bytes(now).c_str(), Bytes(saved).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("All 8-bit the samples would be %s bytes packed. The first 4-bit sample "
                          "also costs the decoder's 512-byte tables in fast RAM.",
                          Bytes(all8).c_str());
    if (music.is_object()) {
        ImGui::SameLine();
        ImGui::TextDisabled("music.bin %s, fast RAM %s with the swapper's tables",
                            Bytes(core::jsonread::ReadOr(music, "bytes", 0u)).c_str(),
                            Bytes(core::jsonread::ReadOr(music, "fast", 0u)).c_str());
        if (auto f = music.find("forced"); f != music.end() && f->is_array() && !f->empty()) {
            ImGui::TextColored(kWarnColour, "%zu sample(s) made 4-bit to fit: %s", f->size(),
                               core::jsonread::ReadOr(music, "forced_why", "").c_str());
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Samples the song never plays", &m_showUnused);

    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##samples", 10, tf, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Sample", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Bytes");
        ImGui::TableSetupColumn("8-bit");
        ImGui::TableSetupColumn("4-bit");
        ImGui::TableSetupColumn("dB");
        ImGui::TableSetupColumn("Worst");
        ImGui::TableSetupColumn("Setting");
        ImGui::TableSetupColumn("Packed as");
        ImGui::TableSetupColumn("Listen");
        ImGui::TableHeadersRow();
        for (const auto& s : check.samples) {
            if (!s.assessed || (!s.used && !m_showUnused))
                continue;
            ImGui::PushID(s.index);
            ImGui::TableNextRow();
            ImGui::BeginDisabled(!s.used);
            ImGui::TableNextColumn();
            ImGui::Text("%2d", s.index);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.name.empty() ? "(no name)" : s.name.c_str());
            if (s.loop && ImGui::IsItemHovered())
                ImGui::SetTooltip("Looped: its dB counts the loop four times, the attack once");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Bytes(s.bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Bytes(s.packed).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Packed 8-bit, as the disk's packer has it");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Bytes(s.delta4).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("As 4-bit deltas: half the bytes, and 18 for its step table");
            ImGui::TableNextColumn();
            ImGui::TextColored(DbColour(s.db, bar), "%.1f", s.db);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "How close the 4-bit sample comes to the 8-bit one: signal to noise. Higher is "
                    "closer; the bar is %.1f. Loud, bright and sharp sounds (hats, snares, "
                    "distorted leads) measure lowest: the 4-bit steps cannot follow their edges.",
                    bar);
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.worst);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The largest single error, in levels of 256");
            ImGui::TableNextColumn();
            int cur = SettingIndex(pack ? pack->SampleSetting(s.index) : s.setting);
            ImGui::SetNextItemWidth(80);
            if (ImGui::Combo("##set", &cur, kSettingLabels, 3))
                m_session->SetSampleSetting(s.index, kSettings[cur]);
            ImGui::TableNextColumn();
            const bool four = s.codec == "4bit";
            const bool pending = pack && pack->SampleSetting(s.index) != s.setting;
            const bool forced = s.why.find("to fit") != std::string::npos;
            if (pending)
                ImGui::TextDisabled("%s ...", four ? "4-bit" : "8-bit");
            else if (forced)
                ImGui::TextColored(kWarnColour, "4-bit !");
            else if (four)
                ImGui::TextColored(kGoodColour, "4-bit");
            else
                ImGui::TextUnformatted("8-bit");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  pending ? "The kit is checking the new setting." : s.why.c_str());
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!why.empty());
            if (ImGui::SmallButton(ICON_FA_PLAY))
                m_session->Audition(s.index);
            ImGui::EndDisabled();
            TooltipWhenDisabled(why.empty() ? "This sample at C-3: 8-bit, then 4-bit, twice" : why);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// The song as the Exocet window's Song card has it, a fact after another across the window, then
// its order list with the demo's position lit
void ExocetMusicView::DrawSong()
{
    const float gap = ImGui::GetFontSize() * 1.5f;
    bool first = true;
    for (const auto& f : exo::SongFacts(*m_session)) {
        const float w = ImGui::CalcTextSize(f.label.c_str()).x + ImGui::GetStyle().ItemSpacing.x +
                        ImGui::CalcTextSize(f.value.c_str()).x;
        if (!first) {
            ImGui::SameLine(0, gap);
            if (ImGui::GetContentRegionAvail().x < w)
                ImGui::NewLine();
        }
        first = false;
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", f.label.c_str());
        ImGui::SameLine();
        if (f.colour)
            ImGui::TextColored(*f.colour, "%s", f.value.c_str());
        else
            ImGui::TextUnformatted(f.value.c_str());
        ImGui::EndGroup();
        if (!f.tip.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", f.tip.c_str());
    }
    const auto& b = m_session->Block();
    const int playRow = b.present && m_session->ManifestMatches() ? static_cast<int>(b.row) : -1;
    exo::DrawOrderList(m_session->Check(), playRow, m_session->Fps());
}

} // namespace adm::ui
