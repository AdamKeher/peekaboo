#include "DiskIoView.h"
#include "../TableLayout.h"

#include <fmt/format.h>
#include <algorithm>

#include "../HyperlinkText.h"
#include "../IconsFontAwesome6.h"

namespace adm::ui {

namespace {

// Row tints: writes stand out, because a write is the finding the window exists to surface.
constexpr ImVec4 kWriteTint = {1.0f, 0.35f, 0.25f, 0.16f};
constexpr ImVec4 kReadTint = {0.2f, 0.6f, 1.0f, 0.10f};
constexpr ImVec4 kAbortTint = {1.0f, 0.8f, 0.1f, 0.16f};

constexpr float kDetailHeight = 260.0f;

std::string ShortHash(const std::string& sha1)
{
    return sha1.empty() ? std::string("-") : sha1.substr(0, 12);
}

} // namespace

DiskIoView::DiskIoView(adm::core::DiskProfiler* profiler) : m_profiler(profiler) {}

void DiskIoView::Draw()
{
    if (!m_isOpen)
        return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }

    ImGui::SetNextWindowSize(ImVec2(1000, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_FLOPPY_DISK " Disk I/O###DiskIo", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }

    if (!m_profiler) {
        ImGui::TextDisabled("No profiler attached.");
        ImGui::End();
        return;
    }

    DrawStatsBar(m_profiler->ComputeStats());
    ImGui::Separator();
    DrawTable();

    ImGui::End();
}

void DiskIoView::DrawStatsBar(const adm::core::DiskProfiler::Stats& stats)
{
    if (stats.totalTransfers == 0) {
        ImGui::TextDisabled("No disk transfers recorded. Run the emulator and load something.");
        ImGui::TextDisabled("(Requires a core built from PuAE 1.19.0 or later.)");
        return;
    }

    ImGui::Text("%d transfers | %d read (%llu B)", stats.totalTransfers, stats.reads,
                static_cast<unsigned long long>(stats.bytesRead));
    ImGui::SameLine();

    // The headline answer: did anything go back to the disk?
    if (stats.writes > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "| %d WRITE (%llu B)", stats.writes,
                           static_cast<unsigned long long>(stats.bytesWritten));
    } else {
        ImGui::TextDisabled("| no writes");
    }

    if (m_profiler->DroppedEvents()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.1f, 1.0f), "| ring overflow: log has gaps");
    }

    ImGui::Checkbox("Follow", &m_followTail);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep the newest transfer in view.");
    ImGui::SameLine();
    ImGui::Checkbox("Writes only", &m_writesOnly);
}

void DiskIoView::DrawTable()
{
    const auto& records = m_profiler->GetRecords();

    const bool hasSelection =
        m_selectedIdx >= 0 && m_selectedIdx < static_cast<int>(records.size());
    const float detailH = hasSelection ? kDetailHeight : 0.0f;
    float tableH = ImGui::GetContentRegionAvail().y - detailH;
    if (tableH < 80.0f)
        tableH = 80.0f;

    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                           ImGuiTableFlags_BordersV | kAdjustableColumns |
                                           ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginTable("##diskio", 13, tableFlags, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Seq", ImGuiTableColumnFlags_WidthFixed, 46.0f);
        ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Trig", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed, 66.0f);
        ImGui::TableSetupColumn("Drv", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("Cyl/Sd", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("DSKPT", ImGuiTableColumnFlags_WidthFixed, 66.0f);
        ImGui::TableSetupColumn("DSKLEN", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("Sectors", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("SHA-1", ImGuiTableColumnFlags_WidthFixed, 106.0f);
        ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(records.size()); ++i) {
            const adm::core::DiskRecord& r = records[i];
            const bool isWrite = r.raw.direction == 3;
            if (m_writesOnly && !isWrite)
                continue;

            ImGui::TableNextRow();
            const ImVec4 tint = r.raw.aborted ? kAbortTint : (isWrite ? kWriteTint : kReadTint);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(tint));

            ImGui::TableSetColumnIndex(0);
            const bool selected = (i == m_selectedIdx);
            // AllowOverlap so the hyperlinked PC/DSKPT cells stay clickable on the selected row.
            if (ImGui::Selectable(fmt::format("{}##r{}", r.raw.seq, i).c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap)) {
                m_selectedIdx = selected ? -1 : i;
                m_followTail = false;
                if (m_onJumpToLRef && r.lRef)
                    m_onJumpToLRef(r.lRef);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", r.frame);

            ImGui::TableSetColumnIndex(2);
            if (isWrite)
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "write");
            else
                ImGui::TextUnformatted("read");

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(r.raw.fromCopper ? "copper" : "cpu");

            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(kHyperlinkColor, "%06X", r.raw.pc);
            {
                bool clicked = false;
                HoverableLink(&clicked);
                if (clicked && m_onJumpToDisasm)
                    m_onJumpToDisasm(r.raw.pc);
            }

            ImGui::TableSetColumnIndex(5);
            if (r.raw.drive == 0xFF)
                ImGui::TextDisabled("-");
            else
                ImGui::Text("df%u", r.raw.drive);

            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%u/%u", r.raw.cyl, r.raw.side);

            ImGui::TableSetColumnIndex(7);
            ImGui::TextColored(kHyperlinkColor, "$%06X", r.raw.dskptStart);
            {
                bool clicked = false;
                HoverableLink(&clicked);
                if (clicked && m_onJumpToMemory)
                    m_onJumpToMemory(r.raw.dskptStart, r.raw.payloadLen ? r.raw.payloadLen : 16u);
            }

            ImGui::TableSetColumnIndex(8);
            ImGui::Text("$%04X", r.raw.dsklen);

            ImGui::TableSetColumnIndex(9);
            ImGui::Text("%u", r.raw.payloadLen);

            ImGui::TableSetColumnIndex(10);
            if (r.sectors.empty())
                ImGui::TextDisabled("-");
            else if (r.standardTrack)
                ImGui::Text("%d ok", static_cast<int>(r.sectors.size()));
            else
                ImGui::Text("%d/%d", r.sectorsChecksumOk, static_cast<int>(r.sectors.size()));

            ImGui::TableSetColumnIndex(11);
            ImGui::TextUnformatted(ShortHash(r.sha1).c_str());
            if (!r.sha1.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", r.sha1.c_str());

            ImGui::TableSetColumnIndex(12);
            std::string flags;
            if (r.raw.aborted)
                flags += "ABORTED ";
            if (r.raw.turbo)
                flags += "turbo ";
            if (isWrite)
                flags += r.persistedToFile ? "persisted " : "not-persisted ";
            if (r.payloadDropped)
                flags += "payload-evicted ";
            if (!flags.empty())
                ImGui::TextUnformatted(flags.c_str());
        }

        // Keep the newest transfer visible while the program is still loading.
        if (m_followTail && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndTable();
    }

    if (m_selectedIdx >= 0 && m_selectedIdx < static_cast<int>(records.size())) {
        ImGui::Separator();
        DrawDetail(records[m_selectedIdx]);
    }
}

void DiskIoView::DrawDetail(const adm::core::DiskRecord& rec)
{
    if (!ImGui::BeginChild("##diskdetail", ImVec2(0, 0), ImGuiChildFlags_None))
        return ImGui::EndChild();

    const bool isWrite = rec.raw.direction == 3;
    ImGui::Text("Transfer #%u — %s, df%u cyl %u side %u (track %u), %u bytes", rec.raw.seq,
                isWrite ? "WRITE" : "read", rec.raw.drive, rec.raw.cyl, rec.raw.side,
                rec.raw.cyl * 2u + rec.raw.side, rec.raw.payloadLen);
    ImGui::Text("Armed by PC $%06X (%s) at cycle %u; DSKPT $%06X..$%06X, DSKLEN $%04X (%u words)",
                rec.raw.pc, rec.raw.fromCopper ? "copper" : "cpu", rec.raw.startCycle,
                rec.raw.dskptStart, rec.raw.dskptEnd, rec.raw.dsklen, rec.raw.words);
    if (!rec.sha1.empty())
        ImGui::Text("SHA-1 %s", rec.sha1.c_str());

    if (ImGui::BeginTabBar("##diskdetailtabs")) {
        if (ImGui::BeginTabItem("Sectors")) {
            if (rec.sectors.empty()) {
                ImGui::TextDisabled(
                    "No standard AmigaDOS sectors decoded from this transfer's MFM data.");
                ImGui::TextDisabled(
                    "That is expected for custom trackloaders and copy-protected formats.");
            } else if (ImGui::BeginTable("##secs", 4,
                                         ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                             ImGuiTableFlags_ScrollY |
                                             ImGuiTableFlags_SizingFixedFit | kAdjustableColumns)) {
                ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("Sector", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                ImGui::TableSetupColumn("Checksum", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("SHA-1 (decoded 512 B)",
                                        ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < rec.sectors.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%u", rec.sectors[i].trackNumber);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%u", rec.sectors[i].sectorNumber);
                    ImGui::TableSetColumnIndex(2);
                    if (rec.sectors[i].checksumOk)
                        ImGui::TextUnformatted("ok");
                    else
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "BAD");
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(i < rec.sectorHashes.size() ? rec.sectorHashes[i].c_str()
                                                                       : "-");
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Data")) {
            if (rec.payload.empty()) {
                ImGui::TextDisabled(rec.payloadDropped
                                        ? "Payload evicted — only the most recent transfers keep "
                                          "their raw bytes."
                                        : "No payload captured for this transfer.");
            } else {
                // Raw MFM as it crossed the bus, 16 bytes per line.
                ImGui::TextDisabled("Raw MFM payload, %zu bytes", rec.payload.size());
                if (ImGui::BeginChild("##hex", ImVec2(0, 0))) {
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>((rec.payload.size() + 15) / 16));
                    while (clipper.Step()) {
                        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
                            const size_t base = static_cast<size_t>(line) * 16;
                            std::string hex;
                            for (size_t k = 0; k < 16 && base + k < rec.payload.size(); ++k)
                                hex += fmt::format("{:02X} ", rec.payload[base + k]);
                            ImGui::Text("%06zX  %s", base, hex.c_str());
                        }
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::EndChild();
}

} // namespace adm::ui
