#include "TrackView.h"

#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>

namespace adm::ui {

namespace {

// Cell colours by track classification
// (priority: protection > weak > standard > partial > empty).
constexpr ImU32 kColProtection = IM_COL32(225, 70, 70, 255); // named copy-protection format
constexpr ImU32 kColWeak = IM_COL32(220, 70, 200, 255);      // weak/fuzzy bits present
constexpr ImU32 kColStandard = IM_COL32(70, 180, 90, 255);   // standard 11-sector AmigaDOS
constexpr ImU32 kColPartial = IM_COL32(210, 170, 60, 255);   // some sectors, non-standard
constexpr ImU32 kColEmpty = IM_COL32(70, 70, 80, 255);       // unformatted / custom / no sectors

ImU32 ClassifyColor(const core::IpfTrackInfo& t)
{
    if (t.isProtection)
        return kColProtection;
    if (t.weakCount > 0)
        return kColWeak;
    if (t.isStandard)
        return kColStandard;
    if (t.sectorCount > 0)
        return kColPartial;
    return kColEmpty;
}

const char* DensityName(uint32_t type)
{
    switch (type) {
        case 1:
            return "unformatted";
        case 2:
            return "auto density";
        case 3:
            return "variable density";
        default:
            return "n/a";
    }
}

} // namespace

void TrackView::SetData(const std::vector<core::IpfTrackInfo>& tracks)
{
    m_tracks = tracks;
    m_maxCylinder = 0;
    m_maxHead = 0;
    m_standardCount = 0;
    m_weakCount = 0;
    m_protectionCount = 0;
    for (const auto& t : m_tracks) {
        m_maxCylinder = std::max(m_maxCylinder, t.cylinder);
        m_maxHead = std::max(m_maxHead, t.head);
        if (t.isStandard)
            ++m_standardCount;
        if (t.weakCount > 0)
            ++m_weakCount;
        if (t.isProtection)
            ++m_protectionCount;
    }
}

void TrackView::DrawContent()
{
    if (m_tracks.empty()) {
        ImGui::TextUnformatted("No IPF track data.");
        return;
    }
    DrawSummary();
    ImGui::Separator();
    DrawGrid();
}

void TrackView::DrawSummary()
{
    ImGui::Text("Tracks: %zu", m_tracks.size());
    ImGui::Text("Standard AmigaDOS: %u", m_standardCount);
    ImGui::Text("With weak/fuzzy bits: %u", m_weakCount);
    ImGui::Text("Copy-protection tracks: %u", m_protectionCount);

    // Inline legend.
    auto swatch = [](ImU32 col, const char* label) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFontSize();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + h, p.y + h), col);
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
    };
    swatch(kColStandard, "Standard");
    ImGui::SameLine();
    swatch(kColProtection, "Protection");
    ImGui::SameLine();
    swatch(kColWeak, "Weak bits");
    ImGui::SameLine();
    swatch(kColPartial, "Partial");
    ImGui::SameLine();
    swatch(kColEmpty, "Custom/empty");
}

void TrackView::DrawGrid()
{
    const float scale = ImGui::GetFontSize() / 16.0f;
    const float cell = 14.0f * scale;                        // size of one (cyl,head) cell
    const float pad = 3.0f * scale;                          // gap between cells
    const float gutterW = 26.0f * scale;                     // left gutter for H0/H1 labels
    const float tickH = ImGui::GetFontSize() + 2.0f * scale; // cylinder tick label row
    const float bandGap = 10.0f * scale;                     // vertical gap between wrapped bands

    const uint32_t numCyl = m_maxCylinder + 1;
    const uint32_t heads = m_maxHead + 1;

    // Index tracks by (cyl, head) for O(1) cell lookup.
    std::vector<const core::IpfTrackInfo*> grid(numCyl * heads, nullptr);
    for (const auto& t : m_tracks) {
        if (t.head < heads && t.cylinder <= m_maxCylinder)
            grid[t.cylinder * heads + t.head] = &t;
    }

    // Everything is drawn inside the scrolling child so the child clip rect keeps
    // the grid from bleeding over the summary/legend above it.
    ImGui::BeginChild("TrackGrid", ImVec2(0, 0), false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // Cylinders are clustered into groups of 5 (with a gap between groups); whole
    // groups wrap into stacked "bands" so a group is never split across a row.
    // The number of groups across auto-fits the available width.
    constexpr uint32_t kGroupSize = 5;
    const float colStride = cell + pad;
    const float groupGap = 8.0f * scale;
    const float groupStride = kGroupSize * colStride + groupGap;
    const float avail = ImGui::GetContentRegionAvail().x;
    const uint32_t groupsPerBand =
        std::max(1u, static_cast<uint32_t>((avail - gutterW) / groupStride));
    const uint32_t colsPerBand = groupsPerBand * kGroupSize;
    const uint32_t bands = (numCyl + colsPerBand - 1) / colsPerBand;
    const float bandH = tickH + heads * (cell + pad) + bandGap;

    for (uint32_t cyl = 0; cyl < numCyl; ++cyl) {
        const uint32_t group = cyl / kGroupSize;      // which group of 5
        const uint32_t posInGroup = cyl % kGroupSize; // 0..4 within the group
        const uint32_t band = group / groupsPerBand;
        const uint32_t groupInBand = group % groupsPerBand;
        const float x = origin.x + gutterW + groupInBand * groupStride + posInGroup * colStride;
        const float bandTop = origin.y + band * bandH;

        // Label the first cylinder of every group of 5.
        if (posInGroup == 0) {
            dl->AddText(ImVec2(x, bandTop), IM_COL32(150, 150, 160, 255),
                        fmt::format("{}", cyl).c_str());
        }

        for (uint32_t h = 0; h < heads; ++h) {
            // Head labels in the left gutter, once per band.
            if (groupInBand == 0 && posInGroup == 0) {
                dl->AddText(ImVec2(origin.x, bandTop + tickH + h * (cell + pad)),
                            IM_COL32(180, 180, 190, 255), fmt::format("H{}", h).c_str());
            }

            const core::IpfTrackInfo* t = grid[cyl * heads + h];
            ImVec2 rmin(x, bandTop + tickH + h * (cell + pad));
            ImVec2 rmax(rmin.x + cell, rmin.y + cell);

            ImU32 col32 = t ? ClassifyColor(*t) : IM_COL32(40, 40, 46, 255);
            dl->AddRectFilled(rmin, rmax, col32);

            if (ImGui::IsMouseHoveringRect(rmin, rmax)) {
                dl->AddRect(rmin, rmax, IM_COL32(255, 255, 255, 255));
                ImGui::BeginTooltip();
                ImGui::Text("Cyl %u  Head %u", cyl, h);
                if (t) {
                    if (!t->formatName.empty())
                        ImGui::Text("Format: %s", t->formatName.c_str());
                    else
                        ImGui::TextUnformatted("Format: unidentified");
                    ImGui::Text("Track length: %u bits", t->mfmLengthBits);
                    ImGui::Text("Sectors decoded: %u", t->sectorCount);
                    ImGui::Text("Weak/fuzzy areas: %u", t->weakCount);
                    ImGui::Text("Density: %s", DensityName(t->trackType));
                    ImGui::Text("Standard AmigaDOS: %s", t->isStandard ? "yes" : "no");
                } else {
                    ImGui::TextUnformatted("(no data for this track)");
                }
                ImGui::EndTooltip();
            }
        }
    }

    // Reserve layout space so the child's vertical scrollbar is sized correctly.
    ImGui::Dummy(ImVec2(avail, bands * bandH));
    ImGui::EndChild();
}

} // namespace adm::ui
