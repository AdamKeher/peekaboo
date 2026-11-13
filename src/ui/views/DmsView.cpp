/*
 * Copyright (c) 2026 Admin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "DmsView.h"
#include "../../core/Logger.h"
#include "../TableLayout.h"

#include <fmt/format.h>
#include <imgui.h>
#include <algorithm>

namespace adm::ui {

static const char* modes[] = {"NOCOMP", "SIMPLE", "QUICK ", "MEDIUM", "DEEP  ", "HEAVY1", "HEAVY2"};

void DmsView::SetData(const std::string& path)
{
    m_currentPath = path;
    m_info = adm::core::DmsParser::Parse(path);
    if (!m_info.isValid)
        adm::core::Logger::Error("Failed to parse DMS file: {}", path);
}

void DmsView::DrawTabItems()
{
    if (!m_info.isValid)
        return;

    if (ImGui::BeginTabItem("General")) {
        DrawGeneralTab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Track Information")) {
        DrawTrackInfoTab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Banner")) {
        DrawBannerTab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("FILEID.DIZ")) {
        DrawFileIdDizTab();
        ImGui::EndTabItem();
    }
}

void DmsView::DrawGeneralTab()
{
    if (ImGui::BeginTable("Details", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 300.0f);
        ImGui::TableSetupColumn("Value");
        // ImGui::TableHeadersRow();

        auto Row = [](const char* label, std::string value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", value.c_str());
        };

        Row("File", m_info.filename);

        uint16_t cv = m_info.dmsVersion;
        std::string ver = fmt::format("{}.{:02d} {}", cv / 100, cv % 100,
                                      m_info.registered ? "Registered" : "Evaluation");
        Row("Created with DMS version", ver);

        // ctime appends newline, remove it
        std::string dateStr = std::ctime(&m_info.creationDate);
        if (!dateStr.empty() && dateStr.back() == '\n')
            dateStr.pop_back();
        Row("Creation date", dateStr);

        Row("Lowest track in archive", std::to_string(m_info.lowTrack));
        Row("Highest track in archive", std::to_string(m_info.highTrack));
        Row("Packed data size", std::to_string(m_info.packedSize));
        Row("Unpacked data size", std::to_string(m_info.unpackedSize));

        std::string diskTypeStr = "Unknown";
        switch (m_info.diskType) {
            case 0:
            case 1:
                diskTypeStr = "AmigaOS 1.0 OFS";
                break;
            case 2:
                diskTypeStr = "AmigaOS 2.0 FFS";
                break;
            case 3:
                diskTypeStr = "AmigaOS 3.0 OFS / International";
                break;
            case 4:
                diskTypeStr = "AmigaOS 3.0 FFS / International";
                break;
            case 5:
                diskTypeStr = "AmigaOS 3.0 OFS / Dir Cache";
                break;
            case 6:
                diskTypeStr = "AmigaOS 3.0 FFS / Dir Cache";
                break;
            case 7:
                diskTypeStr = "FMS Amiga System File";
                break;
        }
        Row("Disk type of archive", diskTypeStr);

        std::string cmodeStr =
            (m_info.compressionMode <= 6) ? modes[m_info.compressionMode] : "Unknown";
        Row("Compression mode used", cmodeStr);

        std::string genInfo;
        if ((m_info.generalInfo == 0) || (m_info.generalInfo == 0x80))
            genInfo = "None";
        else {
            if (m_info.generalInfo & 1)
                genInfo += "NoZero ";
            if (m_info.generalInfo & 2)
                genInfo += "Encrypted ";
            if (m_info.generalInfo & 4)
                genInfo += "Appends ";
            if (m_info.generalInfo & 8)
                genInfo += "Banner ";
            if (m_info.generalInfo & 16)
                genInfo += "HD ";
            if (m_info.generalInfo & 32)
                genInfo += "MS-DOS ";
            if (m_info.generalInfo & 64)
                genInfo += "DMS_DEV_Fixed ";
            if (m_info.generalInfo & 256)
                genInfo += "FILEID.DIZ";
        }
        Row("General info", genInfo);
        Row("Info Header CRC", fmt::format("{:04X}", m_info.headerCrc));

        ImGui::EndTable();
    }
}

void DmsView::DrawTrackInfoTab()
{
    if (ImGui::BeginTable("Tracks", 8,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | kAdjustableColumns)) {
        ImGui::TableSetupColumn("Track");
        ImGui::TableSetupColumn("Plength");
        ImGui::TableSetupColumn("Ulength");
        ImGui::TableSetupColumn("Cmode");
        ImGui::TableSetupColumn("USUM");
        ImGui::TableSetupColumn("HCRC");
        ImGui::TableSetupColumn("DCRC");
        ImGui::TableSetupColumn("Cflag");
        ImGui::TableHeadersRow();

        for (const auto& track : m_info.tracks) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", track.number);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", track.packedLength);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", track.unpackedLength);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%s", (track.cmode <= 6) ? modes[track.cmode] : "Unknown");
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%04X", track.usum);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%04X", track.hcrc);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%04X", track.dcrc);
            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%d", track.flags);
        }

        ImGui::EndTable();
    }
}

void DmsView::DrawBannerTab()
{
    if (m_info.banner.empty()) {
        ImGui::Text("No Banner present.");
    } else {
        // Use monospaced font if available?
        // ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[1]); // Assuming 1 is Mono, but unsafe
        ImGui::InputTextMultiline("##banner", const_cast<char*>(m_info.banner.c_str()),
                                  m_info.banner.size(), ImVec2(-FLT_MIN, -FLT_MIN),
                                  ImGuiInputTextFlags_ReadOnly);
        // ImGui::PopFont();
    }
}

void DmsView::DrawFileIdDizTab()
{
    if (m_info.fileIdDiz.empty()) {
        ImGui::Text("No FILEID.DIZ present.");
    } else {
        ImGui::InputTextMultiline("##fileid", const_cast<char*>(m_info.fileIdDiz.c_str()),
                                  m_info.fileIdDiz.size(), ImVec2(-FLT_MIN, -FLT_MIN),
                                  ImGuiInputTextFlags_ReadOnly);
    }
}

} // namespace adm::ui
