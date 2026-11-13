#include "DebuggerCIAView.h"
#include <fmt/format.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "../../core/AmigaCustom.h"
#include "../../core/AmigaEmulator.h"
#include "../FontRoles.h"
#include "../IconsFontAwesome6.h"
#include "../TableLayout.h"
#include "EmulatorView.h"
#include "RegisterDocRenderer.h"
#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

// ---------------------------------------------------------------------------

DebuggerCIAView::DebuggerCIAView(EmulatorView* emulatorView) : m_emulatorView(emulatorView) {}

void DebuggerCIAView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void DebuggerCIAView::Draw()
{
    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }

    if (ImGui::Begin(ICON_FA_CLOCK " CIA Registers###CIA Registers##debugger", &m_open, flags)) {
        if (m_turboSuppressed) {
            DrawTurboSuppressedPlaceholder();
        } else {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
                m_showDocs = !m_showDocs;
            }
            DrawContent();
        }
    }
    ImGui::End();
}

void DebuggerCIAView::Clear()
{
    m_regRows.clear();
    // The detail panel addresses one specific register, so a new machine has to invalidate it
    // rather than leave a stale address behind Set and Lock.
    m_selectedKey.clear();
    m_selectedAddr = 0;
    m_detailValue = 0;
    m_detailSync = true;
    m_lockStatus.clear();
    m_lockStatusIsError = false;
}

void DebuggerCIAView::LoadCIARegistersDef()
{
    if (m_ciaRegsDefLoaded)
        return;
    m_ciaRegsDefLoaded = true;
    try {
        std::ifstream f("assets/cia_registers.json");
        if (f.is_open()) {
            m_ciaRegsDef = nlohmann::json::parse(f);
        } else {
            m_ciaRegsDef = nlohmann::json::object();
        }
    } catch (...) {
        m_ciaRegsDef = nlohmann::json::object();
    }
}

void DebuggerCIAView::LoadRegisterDocs()
{
    if (m_registerDocsLoaded)
        return;
    m_registerDocsLoaded = true;
    try {
        std::ifstream f("assets/cia_registers_docs.json");
        if (f.is_open()) {
            m_registerDocs = nlohmann::json::parse(f);
        } else {
            m_registerDocs = nlohmann::json::object();
        }
    } catch (...) {
        m_registerDocs = nlohmann::json::object();
    }
}

void DebuggerCIAView::DrawContent()
{
    LoadCIARegistersDef();
    LoadRegisterDocs();

    bool running = m_emulatorView && m_emulatorView->IsRunning();
    adm::core::CIAState ciaRegs{};
    bool regsValid = false;
    if (running) {
        regsValid = m_emulatorView->GetEmulator()->GetCIARegisters(ciaRegs);
        if (!regsValid) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            const char* msg = "CIA Registers unavailable (rebuild PuAE with adm_get_cia_state)";
            ImVec2 textSize = ImGui::CalcTextSize(msg);
            ImGui::SetCursorPos(
                ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
            ImGui::TextDisabled("%s", msg);
            return;
        }
    }

    ImGui::Checkbox("Show Details", &m_showDocs);
    ImGui::Spacing();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float splitterThickness = 4.0f;
    float topH = avail.y;
    float bottomH = 0.0f;

    if (m_showDocs) {
        topH = avail.y * m_splitRatio;
        bottomH = avail.y - topH - splitterThickness;
        if (topH < 50.0f)
            topH = 50.0f;
        if (bottomH < 50.0f)
            bottomH = 50.0f;
    }

    ImGui::BeginChild("##CIARegsTop", ImVec2(0, topH), false, ImGuiWindowFlags_None);
    {
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputTextWithHint("##CIARegFilter", "Filter registers...", m_regFilter,
                                 sizeof(m_regFilter));
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            m_regFilter[0] = '\0';
        }
        ImGui::Spacing();

        ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                     kAdjustableColumns | ImGuiTableFlags_Sortable;

        PushFontRole(FontRole::Code);
        if (ImGui::BeginTable("CIARegsTable", 6, tableFlags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                "Address", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort,
                80.0f, 0);
            ImGui::TableSetupColumn("Chip", ImGuiTableColumnFlags_WidthFixed, 45.0f, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 60.0f, 2);
            ImGui::TableSetupColumn(
                "Value", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 55.0f, 3);
            ImGui::TableSetupColumn("R/W", ImGuiTableColumnFlags_WidthFixed, 30.0f, 4);
            ImGui::TableSetupColumn(
                "Description", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort,
                0.0f, 5);
            ImGui::TableHeadersRow();

            // Build register rows from CIA struct + JSON defs. Only rebuild (and pay for the JSON
            // lookups + allocations) when the emulator advances a frame, or when we have no rows
            // yet; while paused the cached rows are reused untouched.
            uint64_t emuFrame = m_emulatorView ? m_emulatorView->GetEmulatorFrameCount() : 0;
            bool rebuildRows = m_regRows.empty() || (running && emuFrame != m_lastRowsFrame);
            if (running)
                m_lastRowsFrame = emuFrame;

            if (rebuildRows) {
                m_regRows.clear();

                auto toLower = [](std::string s) {
                    std::transform(s.begin(), s.end(), s.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    return s;
                };

                auto AddReg = [&](const char* key, uint8_t value) {
                    RegisterRow row;
                    row.key = key;
                    row.value = value;
                    if (m_ciaRegsDef.contains(key)) {
                        auto& def = m_ciaRegsDef[key];
                        if (def.contains("name") && def["name"].is_string())
                            row.name = def["name"].get<std::string>();
                        if (def.contains("address") && def["address"].is_string())
                            row.address = def["address"].get<std::string>();
                        if (def.contains("chip") && def["chip"].is_string())
                            row.chip = def["chip"].get<std::string>();
                        if (def.contains("rw") && def["rw"].is_string())
                            row.rw = def["rw"].get<std::string>();
                        if (def.contains("description") && def["description"].is_string())
                            row.description = def["description"].get<std::string>();
                        if (def.contains("doc_name") && def["doc_name"].is_string())
                            row.docName = def["doc_name"].get<std::string>();
                    } else {
                        row.address = key;
                    }
                    row.keyLower = toLower(row.key);
                    row.nameLower = toLower(row.name);
                    row.chipLower = toLower(row.chip);
                    m_regRows.push_back(row);
                };

                // CIAA registers
                AddReg("BFE001", ciaRegs.a.pra);
                AddReg("BFE101", ciaRegs.a.prb);
                AddReg("BFE201", ciaRegs.a.dra);
                AddReg("BFE301", ciaRegs.a.drb);
                AddReg("BFE401", ciaRegs.a.talo);
                AddReg("BFE501", ciaRegs.a.tahi);
                AddReg("BFE601", ciaRegs.a.tblo);
                AddReg("BFE701", ciaRegs.a.tbhi);
                AddReg("BFE801", ciaRegs.a.todlo);
                AddReg("BFE901", ciaRegs.a.todmid);
                AddReg("BFEA01", ciaRegs.a.todhi);
                AddReg("BFEC01", ciaRegs.a.sdr);
                AddReg("BFED01", ciaRegs.a.icr);
                AddReg("BFEE01", ciaRegs.a.cra);
                AddReg("BFEF01", ciaRegs.a.crb);

                // CIAB registers
                AddReg("BFD000", ciaRegs.b.pra);
                AddReg("BFD100", ciaRegs.b.prb);
                AddReg("BFD200", ciaRegs.b.dra);
                AddReg("BFD300", ciaRegs.b.drb);
                AddReg("BFD400", ciaRegs.b.talo);
                AddReg("BFD500", ciaRegs.b.tahi);
                AddReg("BFD600", ciaRegs.b.tblo);
                AddReg("BFD700", ciaRegs.b.tbhi);
                AddReg("BFD800", ciaRegs.b.todlo);
                AddReg("BFD900", ciaRegs.b.todmid);
                AddReg("BFDA00", ciaRegs.b.todhi);
                AddReg("BFDC00", ciaRegs.b.sdr);
                AddReg("BFDD00", ciaRegs.b.icr);
                AddReg("BFDE00", ciaRegs.b.cra);
                AddReg("BFDF00", ciaRegs.b.crb);
            } // if (rebuildRows)

            // Rows are persistent across frames now, so re-apply the sort only when the user
            // changes it (SpecsDirty) or when a rebuild reset the vector to insertion order.
            if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
                const ImGuiTableColumnSortSpecs* spec =
                    sortSpecs->Specs ? &sortSpecs->Specs[0] : nullptr;
                if (spec && (sortSpecs->SpecsDirty || rebuildRows)) {
                    auto compare = [spec](const RegisterRow& a, const RegisterRow& b) {
                        int delta = 0;
                        if (spec->ColumnIndex == 0)
                            delta = a.key.compare(b.key);
                        else if (spec->ColumnIndex == 1)
                            delta = a.chip.compare(b.chip);
                        else if (spec->ColumnIndex == 2)
                            delta = a.name.compare(b.name);
                        else if (spec->ColumnIndex == 4)
                            delta = a.rw.compare(b.rw);
                        if (delta > 0)
                            return spec->SortDirection == ImGuiSortDirection_Ascending ? false
                                                                                       : true;
                        if (delta < 0)
                            return spec->SortDirection == ImGuiSortDirection_Ascending ? true
                                                                                       : false;
                        return a.key < b.key;
                    };
                    std::stable_sort(m_regRows.begin(), m_regRows.end(), compare);
                }
                sortSpecs->SpecsDirty = false;
            }

            std::string filterStr = m_regFilter;
            std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            for (const auto& row : m_regRows) {
                if (!filterStr.empty()) {
                    // Lowercased fields are precomputed at build time (row.*Lower).
                    if (row.nameLower.find(filterStr) == std::string::npos &&
                        row.keyLower.find(filterStr) == std::string::npos &&
                        row.chipLower.find(filterStr) == std::string::npos) {
                        continue;
                    }
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                bool isSelected = !row.docName.empty() && row.docName == m_selectedDocName;
                char selectableId[64];
                snprintf(selectableId, sizeof(selectableId), "##selrow_%s", row.key.c_str());
                if (ImGui::Selectable(selectableId, isSelected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    // The docs are shared between the two chips' copies of a register, so the
                    // doc group and the exact register are tracked separately: Set and Lock act
                    // on one address, and a register with no docs is still selectable.
                    if (!row.docName.empty()) {
                        m_selectedDocName = row.docName;
                        m_selectedRegisterName = row.name;
                    }
                    m_selectedKey = row.key;
                    m_selectedAddr =
                        static_cast<uint32_t>(std::strtoul(row.key.c_str(), nullptr, 16));
                    m_detailSync = true;
                }
                if (row.key == m_selectedKey && m_detailSync && regsValid)
                    m_detailValue = row.value;
                ImGui::SameLine();

                ImGui::Text("%s", row.address.c_str());

                ImGui::TableSetColumnIndex(1);
                // Color-code chip label
                if (row.chip == "ciaa") {
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "CIAA");
                } else if (row.chip == "ciab") {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "CIAB");
                } else {
                    ImGui::Text("%s", row.chip.c_str());
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", row.name.c_str());

                ImGui::TableSetColumnIndex(3);
                if (!regsValid) {
                    ImGui::TextDisabled("--");
                } else {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "$%02X", row.value);
                }
                DrawLockGlyph(
                    m_lockUi.At(static_cast<uint32_t>(std::strtoul(row.key.c_str(), nullptr, 16))));

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s", row.rw.c_str());

                ImGui::TableSetColumnIndex(5);
                ImGui::TextWrapped("%s", row.description.c_str());
            }

            ImGui::EndTable();
        }
        PopFontRole();
    }
    ImGui::EndChild();

    if (m_showDocs) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Button("##CIASplitter", ImVec2(-1, splitterThickness));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemActive()) {
            float delta = ImGui::GetIO().MouseDelta.y;
            float newTop = topH + delta;
            float newBottom = bottomH - delta;
            if (newTop >= 50.0f && newBottom >= 50.0f) {
                m_splitRatio = newTop / avail.y;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

        ImGui::BeginChild("##CIARegsBottom", ImVec2(0, bottomH), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        DrawDetailPanel();
        DrawDocPanel();
        ImGui::EndChild();
    }
}

void DebuggerCIAView::DrawDetailPanel()
{
    if (m_selectedKey.empty())
        return;

    ImGui::Text("%s  $%06X",
                m_selectedRegisterName.empty() ? m_selectedKey.c_str()
                                               : m_selectedRegisterName.c_str(),
                m_selectedAddr);

    char buf[8];
    snprintf(buf, sizeof(buf), "%02X", m_detailValue & 0xFFu);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(40.0f);
    if (ImGui::InputText("##ciadetval", buf, sizeof(buf),
                         ImGuiInputTextFlags_CharsHexadecimal |
                             ImGuiInputTextFlags_AutoSelectAll)) {
        m_detailValue = static_cast<uint32_t>(std::strtoul(buf, nullptr, 16)) & 0xFFu;
        m_detailSync = false; // an edited value must not be overwritten by the live one
    }
    ImGui::SameLine();
    ImGui::Checkbox("Sync", &m_detailSync);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Follow the selected register's live value.\nTurn off to hold a value "
                          "here while the register changes.");

    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    // Read once: if a lock is already holding this register, Set means "hold this instead",
    // not "poke it and let the lock put the old value straight back".
    const core::LockEntry* existing = m_lockUi.Valid() ? m_lockUi.At(m_selectedAddr) : nullptr;

    ImGui::SameLine();
    ImGui::BeginDisabled(!emu);
    if (ImGui::Button("Set##cia")) {
        std::string err;
        const bool wrote =
            emu && emu->WriteCIARegister(m_selectedAddr, static_cast<uint8_t>(m_detailValue));
        if (existing && existing->mode == core::LockMode::Set) {
            if (m_lockUi.locks->SetValue(existing->id, m_detailValue & 0xFFu, 1, err)) {
                m_lockStatus = fmt::format("Now holding ${:02X}.", m_detailValue & 0xFFu);
                m_lockStatusIsError = false;
            } else {
                m_lockStatus = err;
                m_lockStatusIsError = true;
            }
        } else if (wrote) {
            m_lockStatus = "Written.";
            m_lockStatusIsError = false;
        } else {
            m_lockStatus = "Write failed - no core loaded?";
            m_lockStatusIsError = true;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(existing && existing->mode == core::LockMode::Set
                              ? "Write this value and hold it: the lock below is updated to\n"
                                "force this value from now on."
                              : "Write this value to the register once.");
    }

    if (m_lockUi.Valid()) {
        bool locked = existing != nullptr;
        ImGui::SameLine();
        if (ImGui::Checkbox("Lock##cia", &locked)) {
            std::string err;
            if (locked) {
                if (AddSetLock(m_lockUi, m_selectedAddr, 1, m_detailValue & 0xFFu,
                               fmt::format("{} = ${:02X}",
                                           m_selectedRegisterName.empty() ? m_selectedKey
                                                                          : m_selectedRegisterName,
                                           m_detailValue & 0xFFu),
                               err)) {
                    m_lockStatus = fmt::format("Locked ${:06X}.", m_selectedAddr);
                    m_lockStatusIsError = false;
                } else {
                    m_lockStatus = err;
                    m_lockStatusIsError = true;
                }
            } else {
                RemoveLocksAt(m_lockUi, m_selectedAddr);
                m_lockStatus = fmt::format("Unlocked ${:06X}.", m_selectedAddr);
                m_lockStatusIsError = false;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Force this value: every CPU write to the register is rewritten\n"
                              "to it. Same store as the Trainer window and the PR console\n"
                              "command, so it shows up in both.");
        }
    }

    if (!m_lockStatus.empty()) {
        if (m_lockStatusIsError)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", m_lockStatus.c_str());
        else
            ImGui::TextDisabled("%s", m_lockStatus.c_str());
    }

    // Worth saying plainly: unlike a RAM variable, most of these registers are driven by the
    // hardware itself. Holding a timer latch or a port-input register does not stop the chip
    // updating what it reports, and the CIA only ever sees CPU writes here anyway.
    ImGui::TextDisabled(ICON_FA_CIRCLE_INFO
                        " Most CIA registers are hardware-driven (timers, TOD, port inputs), so "
                        "forcing one rarely behaves like locking a RAM variable.");
    ImGui::Separator();
}

void DebuggerCIAView::DrawDocPanel()
{
    if (m_selectedDocName.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "Select a register above to view documentation";
        ImVec2 textSize = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
        return;
    }

    if (!m_registerDocs.contains(m_selectedDocName)) {
        ImGui::TextDisabled("No documentation found for '%s'", m_selectedDocName.c_str());
        return;
    }

    m_docPanel.Draw(m_registerDocs, m_selectedDocName);

    ImGui::Dummy(ImVec2(0, 20.0f));
}

} // namespace adm::ui
