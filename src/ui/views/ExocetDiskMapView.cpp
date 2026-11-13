#include "ExocetDiskMapView.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "../../core/ExocetSession.h"
#include "../IconsFontAwesome6.h"

namespace adm::ui {

namespace {

constexpr ImVec4 kWarnColour = {1.00f, 0.78f, 0.30f, 1.00f};
constexpr ImVec4 kBlockColour = {1.00f, 0.40f, 0.35f, 1.00f};
constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;

// 12,345
std::string Bytes(uint64_t n)
{
    std::string d = std::to_string(n);
    for (int i = static_cast<int>(d.size()) - 3; i > 0; i -= 3)
        d.insert(static_cast<size_t>(i), ",");
    return d;
}

std::string Ratio(uint64_t packed, uint64_t unpacked)
{
    if (unpacked == 0 || packed == unpacked)
        return packed == unpacked ? "raw" : "";
    return fmt::format("{:.0f}%", 100.0 * double(packed) / double(unpacked));
}

void Tooltip(const std::string& text)
{
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text.c_str());
}

void Cell(const std::string& s) { ImGui::TextUnformatted(s.c_str()); }

// a number, right-aligned in its column
void CellRight(const std::string& s)
{
    const float w = ImGui::CalcTextSize(s.c_str()).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    ImGui::TextUnformatted(s.c_str());
}

// a packer as the tables write it
std::string PackerName(const std::string& p)
{
    if (p == "zx0")
        return "ZX0";
    if (p == "lz4")
        return "LZ4";
    return p;
}

// a packer cell: marked when the disk is not what the map says
void PackerCell(const std::string& packer, bool ok, const std::string& why)
{
    if (ok)
        Cell(PackerName(packer));
    else
        ImGui::TextColored(kWarnColour, "%s", PackerName(packer).c_str());
    Tooltip(why);
}

} // namespace

// cylinder.head, as the Disk I/O window and the drive show a place
std::string ExocetDiskMapView::SectorPlace(uint32_t sector) const
{
    const uint32_t track = sector / m_spec.sectorsPerTrack;
    return fmt::format("{}.{}", track / 2, track % 2);
}

void ExocetDiskMapView::Refresh(bool force)
{
    namespace fs = std::filesystem;
    core::ExocetPack* pack = m_session->Pack();
    const std::string packPath = pack ? pack->Path() : std::string();
    const uint64_t packRevision = pack ? pack->Revision() : 0;
    const std::string diskPath = m_session->DiskPath();
    std::error_code ec;
    fs::file_time_type diskTime{};
    uintmax_t diskSize = 0;
    if (!diskPath.empty()) {
        diskTime = fs::last_write_time(diskPath, ec);
        diskSize = ec ? 0 : fs::file_size(diskPath, ec);
    }
    if (!force && packPath == m_packPath && packRevision == m_packRevision &&
        diskPath == m_diskPath && diskTime == m_diskTime && diskSize == m_diskSize)
        return;
    m_packPath = packPath;
    m_packRevision = packRevision;
    m_diskPath = diskPath;
    m_diskTime = diskTime;
    m_diskSize = diskSize;
    core::ExocetDiskMapLoad load = core::LoadExocetDiskMap(pack, m_session->PackError(), diskPath);
    m_haveSpec = load.haveSpec;
    if (load.haveSpec)
        m_spec = std::move(load.spec);
    m_map = std::move(load.map);
    m_status = std::move(load.status);
}

void ExocetDiskMapView::Draw()
{
    if (!m_isOpen)
        return;
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }
    ImGui::SetNextWindowSize(ImVec2(820, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (!ImGui::Begin(ICON_FA_BOX_ARCHIVE " Exocet Disk Map###ExocetDiskMap", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }
    Refresh(false);

    if (ImGui::SmallButton(ICON_FA_ARROW_ROTATE_RIGHT " Reload"))
        Refresh(true);
    Tooltip("Read the disk image and the .exocet's map again");
    ImGui::SameLine();
    const std::string diskName = m_diskPath.empty()
                                     ? std::string("no disk")
                                     : std::filesystem::path(m_diskPath).filename().string();
    if (m_haveSpec && !m_spec.name.empty())
        ImGui::Text("%s  -  %s", diskName.c_str(), m_spec.name.c_str());
    else
        ImGui::TextUnformatted(diskName.c_str());
    Tooltip(m_haveSpec ? m_spec.text : m_diskPath);

    if (!m_map.ok) {
        ImGui::TextColored(kBlockColour, "%s", m_status.c_str());
        ImGui::End();
        return;
    }
    const uint32_t freeSectors =
        m_spec.sectors > m_map.usedEnd ? m_spec.sectors - m_map.usedEnd : 0;
    ImGui::SameLine();
    ImGui::TextDisabled("%zu blocks, %u of %u sectors used, %u free (%s bytes)",
                        m_map.entries.size(), m_map.usedEnd, m_spec.sectors, freeSectors,
                        Bytes(uint64_t(freeSectors) * m_spec.sectorBytes).c_str());
    for (const auto& w : m_map.warnings)
        ImGui::TextColored(kWarnColour, "%s", w.c_str());

    if (ImGui::CollapsingHeader("Regions", ImGuiTreeNodeFlags_DefaultOpen))
        DrawRegions();
    if (ImGui::CollapsingHeader("Directory header", ImGuiTreeNodeFlags_DefaultOpen))
        DrawHeader();
    if (ImGui::CollapsingHeader("Kinds", ImGuiTreeNodeFlags_DefaultOpen))
        DrawKinds();
    if (ImGui::CollapsingHeader("Runs", ImGuiTreeNodeFlags_DefaultOpen))
        DrawRuns();
    if (ImGui::CollapsingHeader("Entries"))
        DrawEntries();
    ImGui::End();
}

void ExocetDiskMapView::DrawRegions()
{
    if (!ImGui::BeginTable("regions", 6, kTableFlags))
        return;
    ImGui::TableSetupColumn("Region");
    ImGui::TableSetupColumn("Sectors");
    ImGui::TableSetupColumn("Count");
    ImGui::TableSetupColumn("Bytes");
    ImGui::TableSetupColumn("Cyl.Head");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& r : m_map.regions) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (r.fromEntries)
            ImGui::TextDisabled("%s", r.name.c_str());
        else
            Cell(r.name);
        ImGui::TableNextColumn();
        Cell(r.count ? fmt::format("{}-{}", r.first, r.first + r.count - 1) : "-");
        ImGui::TableNextColumn();
        CellRight(std::to_string(r.count));
        ImGui::TableNextColumn();
        CellRight(Bytes(uint64_t(r.count) * m_spec.sectorBytes));
        ImGui::TableNextColumn();
        Cell(r.count ? SectorPlace(r.first) + " - " + SectorPlace(r.first + r.count - 1) : "");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", r.text.c_str());
    }
    ImGui::EndTable();
}

void ExocetDiskMapView::DrawHeader()
{
    ImGui::TextDisabled("Sector %u, %u sectors; header %zu bytes, then %zu-byte entries",
                        m_spec.dirSector, m_spec.dirSectors, m_spec.headerBytes, m_spec.entryBytes);
    if (!ImGui::BeginTable("header", 6, kTableFlags))
        return;
    ImGui::TableSetupColumn("Offset");
    ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("Field");
    ImGui::TableSetupColumn("Value");
    ImGui::TableSetupColumn("Hex");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& v : m_map.header) {
        const auto& f = *v.field;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        CellRight(std::to_string(f.offset));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", f.type.c_str());
        ImGui::TableNextColumn();
        Cell(f.name);
        ImGui::TableNextColumn();
        if (f.isText)
            Cell(v.text);
        else
            CellRight(f.unit == "bytes" ? Bytes(v.value) : std::to_string(v.value));
        ImGui::TableNextColumn();
        if (!f.isText)
            ImGui::TextDisabled("$%0*X", static_cast<int>(f.size * 2), v.value);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", f.text.c_str());
    }
    ImGui::EndTable();
}

void ExocetDiskMapView::DrawKinds()
{
    if (!ImGui::BeginTable("kinds", 9, kTableFlags))
        return;
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Packer");
    ImGui::TableSetupColumn("Blocks");
    ImGui::TableSetupColumn("Sectors");
    ImGui::TableSetupColumn("Packed");
    ImGui::TableSetupColumn("Unpacked");
    ImGui::TableSetupColumn("Ratio");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    uint64_t blocks = 0, sectors = 0, packed = 0, unpacked = 0;
    for (const auto& k : m_map.kinds) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool selected = m_selectedKind == k.kind;
        if (ImGui::Selectable(std::to_string(k.kind).c_str(), selected,
                              ImGuiSelectableFlags_SpanAllColumns))
            m_selectedKind = selected ? -1 : k.kind;
        Tooltip("Mark this kind's runs and entries");
        ImGui::TableNextColumn();
        Cell(k.name);
        ImGui::TableNextColumn();
        {
            // the map's packer; what the disk has instead, when it differs
            const bool ok = k.packer.empty() || k.found.empty() || k.found == k.packer;
            const std::string shown = k.packer.empty() ? k.found : k.packer;
            PackerCell(ok ? shown : shown + " (disk: " + PackerName(k.found) + ")", ok,
                       ok ? m_spec.slicesText
                          : "The map says " + PackerName(k.packer) +
                                "; its blocks on the disk are " + PackerName(k.found));
        }
        ImGui::TableNextColumn();
        CellRight(std::to_string(k.blocks));
        ImGui::TableNextColumn();
        CellRight(std::to_string(k.sectors));
        ImGui::TableNextColumn();
        CellRight(Bytes(k.packed));
        ImGui::TableNextColumn();
        CellRight(Bytes(k.unpacked));
        ImGui::TableNextColumn();
        CellRight(Ratio(k.packed, k.unpacked));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", k.text.c_str());
        blocks += k.blocks;
        sectors += k.sectors;
        packed += k.packed;
        unpacked += k.unpacked;
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("all");
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    CellRight(std::to_string(blocks));
    ImGui::TableNextColumn();
    CellRight(std::to_string(sectors));
    ImGui::TableNextColumn();
    CellRight(Bytes(packed));
    ImGui::TableNextColumn();
    CellRight(Bytes(unpacked));
    ImGui::TableNextColumn();
    CellRight(Ratio(packed, unpacked));
    ImGui::EndTable();
}

void ExocetDiskMapView::DrawRuns()
{
    ImGui::TextDisabled("The blocks as they lie on the disk: entries of one kind, sector after "
                        "sector, as one row");
    if (!ImGui::BeginTable("runs", 10, kTableFlags))
        return;
    ImGui::TableSetupColumn("Entries");
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Packer");
    ImGui::TableSetupColumn("Sectors");
    ImGui::TableSetupColumn("Count");
    ImGui::TableSetupColumn("Cyl.Head");
    ImGui::TableSetupColumn("Blocks");
    ImGui::TableSetupColumn("Packed");
    ImGui::TableSetupColumn("Unpacked");
    ImGui::TableSetupColumn("Ratio");
    ImGui::TableHeadersRow();
    for (const auto& r : m_map.runs) {
        ImGui::TableNextRow();
        if (r.kind == m_selectedKind)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                   ImGui::GetColorU32(ImGuiCol_HeaderActive, 0.45f));
        ImGui::TableNextColumn();
        Cell(r.firstEntry == r.lastEntry ? std::to_string(r.firstEntry)
                                         : fmt::format("{}-{}", r.firstEntry, r.lastEntry));
        ImGui::TableNextColumn();
        Cell(r.kindName);
        if (const auto* k = m_spec.FindKind(r.kind))
            Tooltip(k->text);
        ImGui::TableNextColumn();
        PackerCell(r.packer, r.packerOk,
                   r.packerOk ? std::string() : "Not the packer the map gives this kind");
        ImGui::TableNextColumn();
        Cell(fmt::format("{}-{}", r.firstSector, r.endSector - 1));
        ImGui::TableNextColumn();
        CellRight(std::to_string(r.endSector - r.firstSector));
        ImGui::TableNextColumn();
        Cell(SectorPlace(r.firstSector) + " - " + SectorPlace(r.endSector - 1));
        ImGui::TableNextColumn();
        CellRight(std::to_string(r.blocks));
        ImGui::TableNextColumn();
        CellRight(Bytes(r.packed));
        ImGui::TableNextColumn();
        CellRight(Bytes(r.unpacked));
        ImGui::TableNextColumn();
        CellRight(Ratio(r.packed, r.unpacked));
    }
    ImGui::EndTable();
}

void ExocetDiskMapView::DrawEntries()
{
    const float rows = static_cast<float>(std::min<size_t>(m_map.entries.size() + 1, 24));
    const ImVec2 size(0, ImGui::GetTextLineHeightWithSpacing() * (rows + 0.5f));
    if (!ImGui::BeginTable("entries", 11, kTableFlags | ImGuiTableFlags_ScrollY, size))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Packer");
    ImGui::TableSetupColumn("Slices");
    ImGui::TableSetupColumn("Sector");
    ImGui::TableSetupColumn("Sectors");
    ImGui::TableSetupColumn("Cyl.Head");
    ImGui::TableSetupColumn("Packed");
    ImGui::TableSetupColumn("Unpacked");
    ImGui::TableSetupColumn("Ratio");
    ImGui::TableSetupColumn("Stream offset");
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_map.entries.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& e = m_map.entries[static_cast<size_t>(i)];
            ImGui::TableNextRow();
            if (e.kind == m_selectedKind)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                       ImGui::GetColorU32(ImGuiCol_HeaderActive, 0.45f));
            ImGui::TableNextColumn();
            CellRight(std::to_string(e.index));
            ImGui::TableNextColumn();
            if (e.fits)
                Cell(e.kindName);
            else
                ImGui::TextColored(kWarnColour, "%s", e.kindName.c_str());
            ImGui::TableNextColumn();
            PackerCell(e.packer, e.packerOk,
                       e.packerOk ? std::string() : "Not the packer the map gives this kind");
            ImGui::TableNextColumn();
            if (e.slices)
                CellRight(std::to_string(e.slices));
            ImGui::TableNextColumn();
            CellRight(std::to_string(e.sector));
            ImGui::TableNextColumn();
            CellRight(std::to_string(e.sectors));
            ImGui::TableNextColumn();
            Cell(SectorPlace(e.sector));
            ImGui::TableNextColumn();
            CellRight(Bytes(e.packed));
            ImGui::TableNextColumn();
            CellRight(Bytes(e.unpacked));
            ImGui::TableNextColumn();
            CellRight(Ratio(e.packed, e.unpacked));
            ImGui::TableNextColumn();
            CellRight(Bytes(e.streamOffset));
        }
    }
    ImGui::EndTable();
}

} // namespace adm::ui
