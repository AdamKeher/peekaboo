#include "ExocetMemoryView.h"
#include "../FontRoles.h"

#include <algorithm>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "../../core/ExocetSession.h"
#include "../IconsFontAwesome6.h"

namespace adm::ui {

namespace {

constexpr ImVec4 kWarnColour = {1.00f, 0.78f, 0.30f, 1.00f};
constexpr ImVec4 kBlockColour = {1.00f, 0.40f, 0.35f, 1.00f};

void TooltipWhenDisabled(const std::string& why)
{
    if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", why.c_str());
}

// A program's colour, as the demo's block gives it; grey for one it does not name
ImU32 OwnerColour(const core::ExocetMemoryState& m, char owner)
{
    const core::ExocetOwner* o = m.Owner(owner);
    return o ? IM_COL32(o->r, o->g, o->b, 255) : IM_COL32(170, 170, 170, 255);
}

constexpr ImU32 kUntrackedColour = IM_COL32(95, 95, 100, 255);

// 12,345: the figures the crass docs quote are whole bytes
std::string Bytes(uint32_t n)
{
    std::string d = std::to_string(n);
    for (int i = static_cast<int>(d.size()) - 3; i > 0; i -= 3)
        d.insert(static_cast<size_t>(i), ",");
    return d;
}

// Where an address is on an A500-shaped machine: the Memory column and its sort
const char* MemoryKind(uint32_t addr)
{
    return addr >= 0xC00000 && addr < 0xD80000 ? "slow" : addr < 0x200000 ? "chip" : "fast";
}

void Swatch(ImU32 col)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float sz = ImGui::GetTextLineHeight() * 0.75f;
    const float y = p.y + (ImGui::GetTextLineHeight() - sz) * 0.5f;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + sz, y + sz), col);
    ImGui::Dummy(ImVec2(sz, ImGui::GetTextLineHeight()));
    ImGui::SameLine();
}

} // namespace

// The disk's memory (core/CrassMemMap.h): a bar a region - its free chunks, the table's
// allocations coloured by the program that made them, and memory in use that no entry holds -
// then the allocations as a table. A disk without a map shows the same, empty and disabled.
void ExocetMemoryView::Draw()
{
    if (!m_isOpen)
        return;
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (!ImGui::Begin(ICON_FA_MEMORY " Exocet Memory###ExocetMemory", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }
    DrawContents();
    ImGui::End();
}

void ExocetMemoryView::DrawContents(bool table)
{
    const core::ExocetMemoryState* mem = m_session->Memory();
    const bool present = mem && mem->present;
    static const core::ExocetMemoryState kNone = [] {
        core::ExocetMemoryState m;
        core::CrassMemRegion chip, fast;
        chip.chip = true;
        m.regions = {chip, fast};
        return m;
    }();
    const core::ExocetMemoryState& m = present ? *mem : kNone;
    const auto& entries = m.table.entries;
    const std::string why =
        present ? std::string()
                : "No memory map: the running program was not built with one (ENGINE_MEMMAP, a "
                  "diagnostic build; Exocet block version 2), or has not got that far.";

    ImGui::BeginDisabled(!present);
    ImGui::Text("Table %u of %u", m.table.count, m.table.capacity);
    TooltipWhenDisabled(why);
    ImGui::SameLine();
    ImGui::TextDisabled("most %u, %u changes", m.table.peak, m.table.changes);
    if (m.table.dropped) {
        ImGui::SameLine();
        ImGui::TextColored(kWarnColour, "%u not recorded: the table was full", m.table.dropped);
    }
    if (m.table.changing) {
        ImGui::SameLine();
        ImGui::TextColored(kWarnColour, "changing");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The machine stopped while the program was changing the table: it "
                              "is shown as it stood, and one entry may be half written.");
    }

    // a bar a region
    uint32_t hovered = 0;
    const float barH = ImGui::GetFrameHeight();
    for (const auto& r : m.regions) {
        const uint32_t span = r.upper > r.lower ? r.upper - r.lower : 0;
        ImGui::TextUnformatted(r.chip ? "Chip" : "Fast");
        ImGui::SameLine();
        if (present) {
            ImGui::TextDisabled("$%06X-$%06X", r.lower, r.upper);
            ImGui::SameLine();
            ImGui::Text("%s free, largest %s", Bytes(r.free).c_str(), Bytes(r.largestFree).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%d pieces, %s untracked", r.freeChunks,
                                Bytes(r.untracked).c_str());
            if (r.overlaps) {
                ImGui::SameLine();
                ImGui::TextColored(kBlockColour, "%d over free memory", r.overlaps);
            }
        } else {
            ImGui::TextDisabled("- free, largest -");
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = std::max(40.0f, ImGui::GetContentRegionAvail().x);
        ImGui::PushID(static_cast<int>(&r - m.regions.data()));
        ImGui::InvisibleButton("##bar", ImVec2(w, barH));
        ImGui::PopID();
        TooltipWhenDisabled(why);
        const bool barHovered = ImGui::IsItemHovered();
        const bool barClicked = ImGui::IsItemClicked();
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + barH), ImGui::GetColorU32(ImGuiCol_FrameBg));
        const auto xOf = [&](uint32_t a) {
            return span ? p0.x + w * static_cast<float>(static_cast<double>(a - r.lower) / span)
                        : p0.x;
        };
        const float mouseX = ImGui::GetIO().MousePos.x;
        bool tipped = false;
        for (const auto& seg : r.segments) {
            const float x0 = xOf(seg.start);
            const float x1 = std::max(xOf(seg.end), x0 + 1.0f);
            const bool under = barHovered && mouseX >= x0 && mouseX < x1 && !tipped;
            if (seg.kind == core::CrassMemSegment::Kind::Free) {
                if (under) {
                    ImGui::SetTooltip("Free: %s bytes at $%06X", Bytes(seg.end - seg.start).c_str(),
                                      seg.start);
                    tipped = true;
                }
                continue;
            }
            ImU32 col = kUntrackedColour;
            const core::CrassMemMapEntry* e = nullptr;
            if (seg.kind == core::CrassMemSegment::Kind::Allocated && seg.entry >= 0) {
                e = &entries[static_cast<size_t>(seg.entry)];
                col = OwnerColour(m, e->owner);
            }
            // a pixel between neighbours, where there is room for one, so blocks of one
            // program do not run together
            const float x1d = x1 - x0 >= 3.0f ? x1 - 1.0f : x1;
            dl->AddRectFilled(ImVec2(x0, p0.y + 2), ImVec2(x1d, p0.y + barH - 2), col);
            if (e && (e->addr == m_selected || e->addr == m_hovered))
                dl->AddRect(ImVec2(x0 - 1, p0.y), ImVec2(x1 + 1, p0.y + barH),
                            IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
            if (!under)
                continue;
            tipped = true;
            if (e) {
                hovered = e->addr;
                ImGui::SetTooltip("%s\n%s, $%06X, %s bytes%s", e->Label().c_str(),
                                  m.OwnerName(e->owner).c_str(), e->addr, Bytes(e->size).c_str(),
                                  e->Chip() ? ", chip asked for" : "");
                if (barClicked)
                    m_selected = e->addr;
            } else {
                ImGui::SetTooltip("In use, not in the table: %s bytes at $%06X\n(exec, the OS, "
                                  "trackdisk, or a block made before the map was)",
                                  Bytes(seg.end - seg.start).c_str(), seg.start);
            }
        }
    }

    // the legend
    for (const auto& o : m.owners) {
        Swatch(OwnerColour(m, o.owner));
        ImGui::TextUnformatted(m.OwnerName(o.owner).c_str());
        ImGui::SameLine();
    }
    Swatch(kUntrackedColour);
    ImGui::TextUnformatted("untracked");
    if (!table) {
        ImGui::EndDisabled();
        m_hovered = hovered;
        return;
    }

    // the allocations
    const ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                               ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
    PushFontRole(FontRole::Memory);
    if (ImGui::BeginTable("##memory", 5, tf, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Program", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("scroller").x + ImGui::GetTextLineHeight() +
                                    ImGui::GetStyle().ItemSpacing.x);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("Memory");
        ImGui::TableHeadersRow();

        std::vector<size_t> order(entries.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        if (const ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount) {
            const ImGuiTableColumnSortSpecs spec = ss->Specs[0];
            const bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                const auto& x = entries[a];
                const auto& y = entries[b];
                int c = 0;
                switch (spec.ColumnIndex) {
                    case 0:
                        c = (x.owner > y.owner) - (x.owner < y.owner);
                        break;
                    case 1:
                        c = x.Label().compare(y.Label());
                        break;
                    case 3:
                        c = (x.size > y.size) - (x.size < y.size);
                        break;
                    case 4:
                        c = std::string(MemoryKind(x.addr)).compare(MemoryKind(y.addr));
                        break;
                    default:
                        c = (x.addr > y.addr) - (x.addr < y.addr);
                }
                return asc ? c < 0 : c > 0;
            });
        }
        for (size_t i : order) {
            const auto& e = entries[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Swatch(OwnerColour(m, e.owner));
            const bool sel = e.addr == m_selected;
            if (ImGui::Selectable(fmt::format("{}##m{}", m.OwnerName(e.owner), i).c_str(), sel,
                                  ImGuiSelectableFlags_SpanAllColumns))
                m_selected = sel ? 0 : e.addr;
            if (ImGui::IsItemHovered())
                hovered = e.addr;
            ImGui::TableNextColumn();
            if (e.Named())
                ImGui::TextUnformatted(e.Label().c_str());
            else
                ImGui::TextDisabled("%s", e.Label().c_str());
            ImGui::TableNextColumn();
            ImGui::Text("$%06X", e.addr);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Bytes(e.size).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(MemoryKind(e.addr));
        }
        ImGui::EndTable();
    }
    PopFontRole();
    TooltipWhenDisabled(why);
    ImGui::EndDisabled();
    m_hovered = hovered;
}

} // namespace adm::ui
