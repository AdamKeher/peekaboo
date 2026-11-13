#include "DebuggerCustomView.h"
#include "../FontRoles.h"
#include "../TableLayout.h"

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include "../../core/AmigaCustom.h"
#include "../../core/AmigaEmulator.h"
#include "../../core/ArMemoryAccess.h"
#include "../AddressLink.h"
#include "../IconsFontAwesome6.h"
#include "../UiIntrospect.h"
#include "EmulatorView.h"
#include "RegisterDocRenderer.h"
#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

// ---------------------------------------------------------------------------

DebuggerCustomView::DebuggerCustomView(EmulatorView* emulatorView) : m_emulatorView(emulatorView) {}

void DebuggerCustomView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void DebuggerCustomView::FocusContainingPanels()
{
    ui::FocusContainingPanels("Custom Registers##debugger");
}

void DebuggerCustomView::Draw()
{
    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        FocusContainingPanels();
        --m_focusFrames;
    }

    if (ImGui::Begin(ICON_FA_SLIDERS " Custom Registers###Custom Registers##debugger", &m_open,
                     flags)) {
        if (m_turboSuppressed) {
            DrawTurboSuppressedPlaceholder();
        } else {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
                m_showCustomDocs = !m_showCustomDocs;
            }
            DrawContent();
        }
    }
    ImGui::End();
}

void DebuggerCustomView::Clear()
{
    m_customRegRows.clear();
    m_rowIndex.clear();
    m_rowIndexDirty = true;
}

void DebuggerCustomView::NavigateToRegister(uint32_t offset, bool hasValue, uint32_t value)
{
    LoadCustomRegistersDef();
    m_open = true;
    m_customRegFilter[0] = '\0';
    const uint32_t masked = offset & 0x1FEu;
    char hex[16];
    snprintf(hex, sizeof(hex), "%03X", masked);
    if (m_customRegsDef.contains(hex)) {
        auto& def = m_customRegsDef[hex];
        if (def.contains("doc_name") && def["doc_name"].is_string())
            m_selectedDocName = def["doc_name"].get<std::string>();
        if (def.contains("name") && def["name"].is_string())
            m_selectedRegisterName = def["name"].get<std::string>();
    }
    m_selectedOffset = static_cast<int>(masked);
    m_pendingNavigateOffset = static_cast<int>(masked);
    m_focusFrames = 4;
    m_hasPendingDecoderValue = hasValue;
    m_pendingDecoderValue = value;
}

void DebuggerCustomView::LoadCustomRegistersDef()
{
    if (m_customRegsDefLoaded)
        return;
    m_customRegsDefLoaded = true;
    try {
        std::ifstream f("assets/custom_registers.json");
        if (f.is_open()) {
            m_customRegsDef = nlohmann::json::parse(f);
        } else {
            m_customRegsDef = nlohmann::json::object();
        }
    } catch (...) {
        m_customRegsDef = nlohmann::json::object();
    }
}

void DebuggerCustomView::LoadRegisterDocs()
{
    if (m_registerDocsLoaded)
        return;
    m_registerDocsLoaded = true;
    try {
        std::ifstream f("assets/custom_registers_docs.json");
        if (f.is_open()) {
            m_registerDocs = nlohmann::json::parse(f);
        } else {
            m_registerDocs = nlohmann::json::object();
        }
    } catch (...) {
        m_registerDocs = nlohmann::json::object();
    }
}

void DebuggerCustomView::DrawContent()
{
    LoadCustomRegistersDef();
    LoadRegisterDocs();

    bool running = m_emulatorView && m_emulatorView->IsRunning();
    adm::core::Custom customRegs{};
    bool regsValid = false;
    if (running) {
        if (!m_emulatorView->GetEmulator()->GetCustomRegisters(customRegs)) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            const char* msg = "Custom Registers unavailable";
            ImVec2 textSize = ImGui::CalcTextSize(msg);
            ImGui::SetCursorPos(
                ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
            ImGui::TextDisabled("%s", msg);
            return;
        }
        regsValid = true;
    }

    ImGui::Checkbox("Show Documentation", &m_showCustomDocs);
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::Checkbox("Show Decoder", &m_showDecoder);
    ImGui::Spacing();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float splitterThickness = 4.0f;
    float topH = avail.y;
    float bottomH = 0.0f;

    if (m_showCustomDocs) {
        topH = avail.y * m_customSplitRatio;
        bottomH = avail.y - topH - splitterThickness;
        if (topH < 50.0f)
            topH = 50.0f;
        if (bottomH < 50.0f)
            bottomH = 50.0f;
    }

    ImGui::BeginChild("##CustomRegsTop", ImVec2(0, topH), false, ImGuiWindowFlags_None);
    {
        // Vertical split: register table on the left, the bit-field decoder on the right.
        // When the decoder is hidden the table fills the whole width.
        const float vsplitThickness = 4.0f;
        float regionW = ImGui::GetContentRegionAvail().x;
        float regionH = ImGui::GetContentRegionAvail().y;
        float leftW = regionW;
        if (m_showDecoder) {
            leftW = regionW * m_vertSplitRatio;
            if (leftW < 150.0f)
                leftW = 150.0f;
            if (leftW > regionW - 150.0f - vsplitThickness)
                leftW = regionW - 150.0f - vsplitThickness;
        }

        // Captured during the row loop: the selected register's geometry + live value, fed to the
        // decoder panel after the table is built.
        bool haveSel = false;
        std::string selDocName;
        std::string selRegName;
        uint32_t selValue = 0;
        int selBits = 16;
        bool selValueValid = false;

        ImGui::BeginChild("##RegList", ImVec2(leftW, regionH), false, ImGuiWindowFlags_None);
        ImGui::SetNextItemWidth(leftW - 70.0f);
        ImGui::InputTextWithHint("##CustomRegFilter", "Filter registers...", m_customRegFilter,
                                 sizeof(m_customRegFilter));
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            m_customRegFilter[0] = '\0';
        }
        ImGui::Spacing();

        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                kAdjustableColumns | ImGuiTableFlags_Sortable;

        PushFontRole(FontRole::Code);
        if (ImGui::BeginTable("CustomRegsTable", 6, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                "Address", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort,
                70.0f, 0);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 80.0f, 1);
            ImGui::TableSetupColumn("Value (Hex)",
                                    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                    100.0f, 2);
            ImGui::TableSetupColumn("R/W", ImGuiTableColumnFlags_WidthFixed, 30.0f, 3);
            ImGui::TableSetupColumn("Chipset", ImGuiTableColumnFlags_WidthFixed, 50.0f, 4);
            ImGui::TableSetupColumn(
                "Description", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort,
                0.0f, 5);
            ImGui::TableHeadersRow();

            // Only rebuild the rows (and pay for the JSON lookups + string allocations) when the
            // emulator has advanced a frame, or when we have no rows yet (e.g. emulator stopped, so
            // the table still shows register names with "--------" values). While paused on the
            // same frame the cached rows are reused untouched.
            uint64_t emuFrame = m_emulatorView ? m_emulatorView->GetEmulatorFrameCount() : 0;
            bool rebuildRows = m_customRegRows.empty() || (running && emuFrame != m_lastRowsFrame);
            if (running)
                m_lastRowsFrame = emuFrame;

            if (rebuildRows) {
                // Metadata (names, rw, descriptions, doc links — ~7 JSON lookups and several
                // string allocations per register) is built ONCE; later rebuilds only refresh
                // the live values, looked up by register offset because the table sort can
                // reorder the rows.
                const bool buildMeta = m_customRegRows.empty();
                if (!buildMeta && m_rowIndexDirty) {
                    m_rowIndex.clear();
                    for (size_t i = 0; i < m_customRegRows.size(); ++i)
                        m_rowIndex.emplace(m_customRegRows[i].offset, i);
                    m_rowIndexDirty = false;
                }

                auto GetRegDef = [&](int offset) -> const nlohmann::json* {
                    char hexOffset[16];
                    snprintf(hexOffset, sizeof(hexOffset), "%03X", offset);
                    if (m_customRegsDef.contains(hexOffset))
                        return &m_customRegsDef[hexOffset];
                    return nullptr;
                };

                auto AddRow16 = [&](const char* name, int offset, uint16_t value) {
                    if (!buildMeta) {
                        auto it = m_rowIndex.find(offset);
                        if (it != m_rowIndex.end()) {
                            auto& existing = m_customRegRows[it->second];
                            existing.value = value;
                            existing.displayValue = value;
                        }
                        return;
                    }
                    RegisterRow row;
                    row.name = name;
                    row.offset = offset;
                    row.value = value;
                    row.displayValue = value;
                    row.bitSize = 16;
                    row.isColor = false;
                    const auto* def = GetRegDef(offset);
                    if (def) {
                        if (def->contains("rw") && (*def)["rw"].is_string())
                            row.rw = (*def)["rw"].get<std::string>();
                        if (def->contains("description") && (*def)["description"].is_string())
                            row.description = (*def)["description"].get<std::string>();
                        if (def->contains("is_color") && (*def)["is_color"].is_boolean())
                            row.isColor = (*def)["is_color"].get<bool>();
                        if (def->contains("is_address") && (*def)["is_address"].is_boolean())
                            row.isAddress = (*def)["is_address"].get<bool>();
                        if (def->contains("address") && (*def)["address"].is_string())
                            row.address = (*def)["address"].get<std::string>();
                        if (def->contains("chipset") && (*def)["chipset"].is_string())
                            row.chipset = (*def)["chipset"].get<std::string>();
                        if (def->contains("doc_name") && (*def)["doc_name"].is_string())
                            row.docName = (*def)["doc_name"].get<std::string>();
                    }
                    row.nameLower = row.name;
                    std::transform(row.nameLower.begin(), row.nameLower.end(),
                                   row.nameLower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    m_customRegRows.push_back(row);
                };

                auto AddRow32 = [&](const char* name, int offset, uint32_t value) {
                    if (!buildMeta) {
                        auto it = m_rowIndex.find(offset);
                        if (it != m_rowIndex.end()) {
                            auto& existing = m_customRegRows[it->second];
                            existing.value = value;
                            existing.displayValue =
                                existing.isAddress ? adm::core::SwapPtrWords(value) : value;
                        }
                        return;
                    }
                    RegisterRow row;
                    row.name = name;
                    row.offset = offset;
                    row.value = value;
                    row.bitSize = 32;
                    row.isColor = false;
                    const auto* def = GetRegDef(offset);
                    if (def) {
                        if (def->contains("rw") && (*def)["rw"].is_string())
                            row.rw = (*def)["rw"].get<std::string>();
                        if (def->contains("description") && (*def)["description"].is_string())
                            row.description = (*def)["description"].get<std::string>();
                        if (def->contains("is_address") && (*def)["is_address"].is_boolean())
                            row.isAddress = (*def)["is_address"].get<bool>();
                        if (def->contains("address") && (*def)["address"].is_string())
                            row.address = (*def)["address"].get<std::string>();
                        if (def->contains("chipset") && (*def)["chipset"].is_string())
                            row.chipset = (*def)["chipset"].get<std::string>();
                        if (def->contains("doc_name") && (*def)["doc_name"].is_string())
                            row.docName = (*def)["doc_name"].get<std::string>();
                    }
                    // Pointer registers (BLTxPT, COPxLC, BPLxPT, SPRxPT, AUDxLC, DSKPT) are stored
                    // word-swapped and shown as the real address. Non-pointer 32-bit values such as
                    // VPOS32 are two distinct registers, so they must NOT be swapped.
                    row.displayValue = row.isAddress ? adm::core::SwapPtrWords(value) : value;
                    row.nameLower = row.name;
                    std::transform(row.nameLower.begin(), row.nameLower.end(),
                                   row.nameLower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    m_customRegRows.push_back(row);
                };

                AddRow16("BLTDDAT", 0x000, customRegs.bltddat);
                AddRow16("DMACONR", 0x002, customRegs.dmaconr);
                AddRow32("VPOS32", 0x004, customRegs.vpos32);
                AddRow16("DSKDATR", 0x008, customRegs.dskdatr);
                AddRow16("JOY0DAT", 0x00A, customRegs.joy0dat);
                AddRow16("JOY1DAT", 0x00C, customRegs.joy1dat);
                AddRow16("CLXDAT", 0x00E, customRegs.clxdat);
                AddRow16("ADKCONR", 0x010, customRegs.adkconr);
                AddRow16("POT0DAT", 0x012, customRegs.pot0dat);
                AddRow16("POT1DAT", 0x014, customRegs.pot1dat);
                AddRow16("POTINP", 0x016, customRegs.potinp);
                AddRow16("SERDATR", 0x018, customRegs.serdatr);
                AddRow16("DSKBYTR", 0x01A, customRegs.dskbytr);
                AddRow16("INTENAR", 0x01C, customRegs.intenar);
                AddRow16("INTREQR", 0x01E, customRegs.intreqr);
                AddRow32("DSKPT", 0x020, customRegs.dskpt);
                AddRow16("DSKLEN", 0x024, customRegs.dsklen);
                AddRow16("DSKDAT", 0x026, customRegs.dskdat);
                AddRow16("REFPTR", 0x028, customRegs.refptr);
                AddRow16("VPOSW", 0x02A, customRegs.vposw);
                AddRow16("VHPOSW", 0x02C, customRegs.vhposw);
                AddRow16("COPCON", 0x02E, customRegs.copcon);
                AddRow16("SERDAT", 0x030, customRegs.serdat);
                AddRow16("SERPER", 0x032, customRegs.serper);
                AddRow16("POTGO", 0x034, customRegs.potgo);
                AddRow16("JOYTEST", 0x036, customRegs.joytest);
                AddRow16("STREQU", 0x038, customRegs.strequ);
                AddRow16("STRVBL", 0x03A, customRegs.strvbl);
                AddRow16("STRHOR", 0x03C, customRegs.strhor);
                AddRow16("STRLONG", 0x03E, customRegs.strlong);
                AddRow16("BLTCON0", 0x040, customRegs.bltcon0);
                AddRow16("BLTCON1", 0x042, customRegs.bltcon1);
                AddRow16("BLTAFWM", 0x044, customRegs.bltafwm);
                AddRow16("BLTALWM", 0x046, customRegs.bltalwm);
                AddRow32("BLTCPT", 0x048, customRegs.bltcpt);
                AddRow32("BLTBPT", 0x04C, customRegs.bltbpt);
                AddRow32("BLTAPT", 0x050, customRegs.bltapt);
                AddRow32("BLTDPT", 0x054, customRegs.bltdpt);
                AddRow16("BLTSIZE", 0x058, customRegs.bltsize);
                AddRow16("BLTCON0L", 0x05A, customRegs.bltcon0l);
                AddRow16("BLTSIZV", 0x05C, customRegs.bltsizv);
                AddRow16("BLTSIZH", 0x05E, customRegs.bltsizh);
                AddRow16("BLTCMOD", 0x060, customRegs.bltcmod);
                AddRow16("BLTBMOD", 0x062, customRegs.bltbmod);
                AddRow16("BLTAMOD", 0x064, customRegs.bltamod);
                AddRow16("BLTDMOD", 0x066, customRegs.bltdmod);
                AddRow16("BLTCDAT", 0x070, customRegs.bltcdat);
                AddRow16("BLTBDAT", 0x072, customRegs.bltbdat);
                AddRow16("BLTADAT", 0x074, customRegs.bltadat);
                AddRow16("DENISEID", 0x07C, customRegs.deniseid);
                AddRow16("DSKSYNC", 0x07E, customRegs.dsksync);
                AddRow32("COP1LC", 0x080, customRegs.cop1lc);
                AddRow32("COP2LC", 0x084, customRegs.cop2lc);
                AddRow16("COPJMP1", 0x088, customRegs.copjmp1);
                AddRow16("COPJMP2", 0x08A, customRegs.copjmp2);
                AddRow16("COPINS", 0x08C, customRegs.copins);
                AddRow16("DIWSTRT", 0x08E, customRegs.diwstrt);
                AddRow16("DIWSTOP", 0x090, customRegs.diwstop);
                AddRow16("DDFSTRT", 0x092, customRegs.ddfstrt);
                AddRow16("DDFSTOP", 0x094, customRegs.ddfstop);
                AddRow16("DMACON", 0x096, customRegs.dmacon);
                AddRow16("CLXCON", 0x098, customRegs.clxcon);
                AddRow16("INTENA", 0x09A, customRegs.intena);
                AddRow16("INTREQ", 0x09C, customRegs.intreq);
                AddRow16("ADKCON", 0x09E, customRegs.adkcon);

                for (int i = 0; i < 4; i++) {
                    char nameBuf[32];
                    int off = 0x0A0 + i * 16;
                    snprintf(nameBuf, sizeof(nameBuf), "AUD%dLC", i);
                    AddRow32(nameBuf, off, customRegs.aud[i].ac_ptr);
                    snprintf(nameBuf, sizeof(nameBuf), "AUD%dLEN", i);
                    AddRow16(nameBuf, off + 4, customRegs.aud[i].ac_len);
                    snprintf(nameBuf, sizeof(nameBuf), "AUD%dPER", i);
                    AddRow16(nameBuf, off + 6, customRegs.aud[i].ac_per);
                    snprintf(nameBuf, sizeof(nameBuf), "AUD%dVOL", i);
                    AddRow16(nameBuf, off + 8, customRegs.aud[i].ac_vol);
                    snprintf(nameBuf, sizeof(nameBuf), "AUD%dDAT", i);
                    AddRow16(nameBuf, off + 10, customRegs.aud[i].ac_dat);
                }

                for (int i = 0; i < 8; i++) {
                    char nameBuf[32];
                    snprintf(nameBuf, sizeof(nameBuf), "BPL%dPT", i + 1);
                    AddRow32(nameBuf, 0x0E0 + i * 4, customRegs.bplpt[i]);
                }

                AddRow16("BPLCON0", 0x100, customRegs.bplcon0);
                AddRow16("BPLCON1", 0x102, customRegs.bplcon1);
                AddRow16("BPLCON2", 0x104, customRegs.bplcon2);
                AddRow16("BPLCON3", 0x106, customRegs.bplcon3);
                AddRow16("BPL1MOD", 0x108, customRegs.bpl1mod);
                AddRow16("BPL2MOD", 0x10A, customRegs.bpl2mod);
                AddRow16("BPLCON4", 0x10C, customRegs.bplcon4);
                AddRow16("CLXCON2", 0x10E, customRegs.clxcon2);

                for (int i = 0; i < 8; i++) {
                    char nameBuf[32];
                    snprintf(nameBuf, sizeof(nameBuf), "BPL%dDAT", i + 1);
                    AddRow16(nameBuf, 0x110 + i * 2, customRegs.bpldat[i]);
                }

                for (int i = 0; i < 8; i++) {
                    char nameBuf[32];
                    snprintf(nameBuf, sizeof(nameBuf), "SPR%dPT", i);
                    AddRow32(nameBuf, 0x120 + i * 4, customRegs.sprpt[i]);
                }

                for (int i = 0; i < 8; i++) {
                    char nameBuf[32];
                    int off = 0x140 + i * 8;
                    snprintf(nameBuf, sizeof(nameBuf), "SPR%dPOS", i);
                    AddRow16(nameBuf, off, customRegs.spr[i].pos);
                    snprintf(nameBuf, sizeof(nameBuf), "SPR%dCTL", i);
                    AddRow16(nameBuf, off + 2, customRegs.spr[i].ctl);
                    snprintf(nameBuf, sizeof(nameBuf), "SPR%dDATA", i);
                    AddRow16(nameBuf, off + 4, customRegs.spr[i].dataa);
                    snprintf(nameBuf, sizeof(nameBuf), "SPR%dDATB", i);
                    AddRow16(nameBuf, off + 6, customRegs.spr[i].datab);
                }

                for (int i = 0; i < 32; i++) {
                    char nameBuf[32];
                    snprintf(nameBuf, sizeof(nameBuf), "COLOR%02d", i);
                    AddRow16(nameBuf, 0x180 + i * 2, customRegs.color[i]);
                }

                AddRow16("HTOTAL", 0x1C0, customRegs.htotal);
                AddRow16("HSSTOP", 0x1C2, customRegs.hsstop);
                AddRow16("HBSTRT", 0x1C4, customRegs.hbstrt);
                AddRow16("HBSTOP", 0x1C6, customRegs.hbstop);
                AddRow16("VTOTAL", 0x1C8, customRegs.vtotal);
                AddRow16("VSSTOP", 0x1CA, customRegs.vsstop);
                AddRow16("VBSTRT", 0x1CC, customRegs.vbstrt);
                AddRow16("VBSTOP", 0x1CE, customRegs.vbstop);
                AddRow16("SPRHSTRT", 0x1D0, customRegs.sprhstrt);
                AddRow16("SPRHSTOP", 0x1D2, customRegs.sprhstop);
                AddRow16("BPLHSTRT", 0x1D4, customRegs.bplhstrt);
                AddRow16("BPLHSTOP", 0x1D6, customRegs.bplhstop);
                AddRow16("HHPOSW", 0x1D8, customRegs.hhposw);
                AddRow16("HHPOSR", 0x1DA, customRegs.hhposr);
                AddRow16("BEAMCON0", 0x1DC, customRegs.beamcon0);
                AddRow16("HSSTRT", 0x1DE, customRegs.hsstrt);
                AddRow16("VSSTRT", 0x1E0, customRegs.vsstrt);
                AddRow16("HCENTER", 0x1E2, customRegs.hcenter);
                AddRow16("DIWHIGH", 0x1E4, customRegs.diwhigh);
                AddRow16("FMODE", 0x1FC, customRegs.fmode);
            } // if (rebuildRows)

            // The rows are now persistent across frames, so the sort only needs re-applying when
            // the user changes it (SpecsDirty) or when a rebuild just reset the vector to insertion
            // order. Re-sorting every frame would otherwise be wasted work while paused.
            if (ImGuiTableSortSpecs* sorts_specs = ImGui::TableGetSortSpecs()) {
                const ImGuiTableColumnSortSpecs* sort_spec =
                    sorts_specs->Specs ? &sorts_specs->Specs[0] : nullptr;
                if (sort_spec && (sorts_specs->SpecsDirty || rebuildRows)) {
                    auto compare = [sort_spec](const RegisterRow& a, const RegisterRow& b) {
                        int delta = 0;
                        if (sort_spec->ColumnIndex == 0)
                            delta = a.offset - b.offset;
                        else if (sort_spec->ColumnIndex == 1)
                            delta = a.name.compare(b.name);
                        else if (sort_spec->ColumnIndex == 3)
                            delta = a.rw.compare(b.rw);
                        else if (sort_spec->ColumnIndex == 4)
                            delta = a.chipset.compare(b.chipset);
                        if (delta > 0)
                            return sort_spec->SortDirection == ImGuiSortDirection_Ascending ? false
                                                                                            : true;
                        if (delta < 0)
                            return sort_spec->SortDirection == ImGuiSortDirection_Ascending ? true
                                                                                            : false;
                        return a.offset < b.offset;
                    };
                    std::stable_sort(m_customRegRows.begin(), m_customRegRows.end(), compare);
                    m_rowIndexDirty = true; // value updates look rows up by offset
                }
                sorts_specs->SpecsDirty = false;
            }

            std::string filterStr = m_customRegFilter;
            std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            for (const auto& row : m_customRegRows) {
                if (!filterStr.empty()) {
                    char hexOffset[16];
                    snprintf(hexOffset, sizeof(hexOffset), "%03X", row.offset);
                    // row.nameLower is precomputed at build time; only the (cheap) hex match needs
                    // a per-row buffer.
                    if (row.nameLower.find(filterStr) == std::string::npos &&
                        std::string_view(hexOffset).find(filterStr) == std::string_view::npos) {
                        continue;
                    }
                }

                ImGui::TableNextRow();
                if (m_pendingNavigateOffset == row.offset) {
                    ImGui::SetScrollHereY(0.5f);
                    m_pendingNavigateOffset = -1;
                }
                ImGui::TableSetColumnIndex(0);

                // doc_name was resolved once at row-build time (row.docName); no per-frame JSON.
                const std::string& docName = row.docName;
                // The exact register clicked is the primary selection; other registers that share
                // its documentation group are tinted so it's clear they're documented together.
                bool isSelected = row.offset == m_selectedOffset;
                bool inGroup = !isSelected && !docName.empty() && docName == m_selectedDocName;
                if (inGroup)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 85, 75, 110));

                char selectableId[64];
                snprintf(selectableId, sizeof(selectableId), "##selrow_%s_%03X", row.name.c_str(),
                         row.offset);
                if (ImGui::Selectable(selectableId, isSelected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    m_selectedOffset = row.offset;
                    m_selectedDocName = docName;
                    m_selectedRegisterName = row.name;
                }

                // Remember the selected register so the decoder panel can show it. Capture the
                // geometry even when the emulator is stopped so the field table still renders.
                if (row.offset == m_selectedOffset) {
                    selDocName = docName;
                    selRegName = row.name;
                    selBits = row.bitSize;
                    haveSel = true;
                    if (regsValid) {
                        selValue = row.displayValue;
                        selValueValid = true;
                    }
                }

                ImGui::SameLine();

                ImGui::Text("%s", row.address.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", row.name.c_str());

                ImGui::TableSetColumnIndex(2);
                if (!regsValid) {
                    ImGui::TextDisabled("--------");
                } else {
                    if (row.isColor) {
                        float r = ((row.value >> 8) & 0xF) / 15.0f;
                        float g = ((row.value >> 4) & 0xF) / 15.0f;
                        float b = ((row.value >> 0) & 0xF) / 15.0f;
                        ImVec4 color(r, g, b, 1.0f);
                        ImGui::ColorButton(
                            "##Color", color,
                            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker |
                                ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoAlpha,
                            ImVec2(16, 16));
                        ImGui::SameLine();
                    }
                    ImVec4 textColor = row.bitSize == 16 ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                                                         : ImVec4(0.4f, 1.0f, 0.8f, 1.0f);
                    char valLabel[32];
                    if (row.bitSize == 16) {
                        snprintf(valLabel, sizeof(valLabel), "$%04X", row.displayValue);
                    } else {
                        snprintf(valLabel, sizeof(valLabel), "$%08X", row.displayValue);
                    }

                    if (row.isAddress && m_addrCtx && m_addrCtx->valid()) {
                        char suffix[64];
                        snprintf(suffix, sizeof(suffix), "%s_%03X", row.name.c_str(), row.offset);
                        DrawAddressLink(suffix, row.displayValue,
                                        row.bitSize == 16 ? "$%04X" : "$%08X", *m_addrCtx);
                    } else {
                        ImGui::TextColored(textColor, "%s", valLabel);
                    }
                    DrawLockGlyph(m_lockUi.At(core::ArMemoryAccess::kCustomBase +
                                              static_cast<uint32_t>(row.offset)));
                }

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", row.rw.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s", row.chipset.c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextWrapped("%s", row.description.c_str());
            }

            ImGui::EndTable();
        }
        PopFontRole();
        ImGui::EndChild(); // ##RegList

        if (haveSel)
            UpdateDecoder(selDocName, selRegName, selValue, selBits, selValueValid);

        // Seed the decoder with a navigation-supplied value (e.g. a copperlist MOVE's word2). Done
        // after UpdateDecoder so it overrides the live/reseeded value; m_lastSyncValue (set by
        // UpdateDecoder to the live value) keeps the override until the register actually changes.
        if (m_hasPendingDecoderValue && haveSel) {
            uint32_t mask = (m_decoderBits == 32) ? 0xFFFFFFFFu : 0xFFFFu;
            m_decoderValue = m_pendingDecoderValue & mask;
            m_hasPendingDecoderValue = false;
        }

        if (m_showDecoder) {
            // Vertical splitter between the register table and the decoder panel.
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            ImGui::Button("##CustomVSplitter", ImVec2(vsplitThickness, regionH));
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemActive()) {
                float dx = ImGui::GetIO().MouseDelta.x;
                float newLeft = leftW + dx;
                if (newLeft >= 150.0f && (regionW - newLeft - vsplitThickness) >= 150.0f)
                    m_vertSplitRatio = newLeft / regionW;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            ImGui::SameLine(0.0f, 0.0f);
            ImGui::BeginChild("##DecoderPanel", ImVec2(0, regionH), false, ImGuiWindowFlags_None);
            DrawDecoderPanel();
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();

    if (m_showCustomDocs) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Button("##CustomSplitter", ImVec2(-1, splitterThickness));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemActive()) {
            float delta = ImGui::GetIO().MouseDelta.y;
            float newTop = topH + delta;
            float newBottom = bottomH - delta;
            if (newTop >= 50.0f && newBottom >= 50.0f) {
                m_customSplitRatio = newTop / avail.y;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

        ImGui::BeginChild("##CustomRegsBottom", ImVec2(0, bottomH), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        DrawDocPanel();
        ImGui::EndChild();
    }
}

void DebuggerCustomView::DrawDocPanel()
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

// ---------------------------------------------------------------------------
// Embedded bit-field decoder / value calculator
// ---------------------------------------------------------------------------

void DebuggerCustomView::LoadBitfieldDefs()
{
    if (m_bitfieldDefsLoaded)
        return;
    m_bitfieldDefsLoaded = true;
    try {
        std::ifstream f("assets/custom_registers_bitfields.json");
        if (f.is_open())
            m_bitfieldDefs = nlohmann::json::parse(f);
        else
            m_bitfieldDefs = nlohmann::json::object();
    } catch (...) {
        m_bitfieldDefs = nlohmann::json::object();
    }
}

const std::vector<DebuggerCustomView::DecoderField>& DebuggerCustomView::ActiveFields() const
{
    static const std::vector<DecoderField> kEmpty;
    if (m_decoderMode >= 0 && m_decoderMode < static_cast<int>(m_decoderModeFields.size()))
        return m_decoderModeFields[m_decoderMode];
    return kEmpty;
}

void DebuggerCustomView::SetDecoderFields(const std::string& regName, const std::string& docName)
{
    LoadBitfieldDefs();
    m_decoderModes.clear();
    m_decoderModeFields.clear();
    m_decoderMode = 0;

    // Prefer a register-name-specific entry (e.g. BLTCON0/BLTCON1, which share a doc group but
    // have distinct layouts); otherwise fall back to the shared doc_name entry.
    const nlohmann::json* def = nullptr;
    if (!regName.empty() && m_bitfieldDefs.contains(regName))
        def = &m_bitfieldDefs[regName];
    else if (!docName.empty() && m_bitfieldDefs.contains(docName))
        def = &m_bitfieldDefs[docName];
    if (!def)
        return;

    auto parseFields = [](const nlohmann::json& arr) {
        std::vector<DecoderField> out;
        for (const auto& jf : arr) {
            DecoderField fld;
            fld.hi = jf.value("hi", 0);
            fld.lo = jf.value("lo", 0);
            fld.name = jf.value("name", std::string());
            fld.desc = jf.value("desc", std::string());
            out.push_back(std::move(fld));
        }
        return out;
    };

    if (def->contains("variants") && (*def)["variants"].is_array()) {
        for (const auto& v : (*def)["variants"]) {
            m_decoderModes.push_back(v.value("mode", std::string()));
            m_decoderModeFields.push_back(
                parseFields(v.contains("fields") ? v["fields"] : nlohmann::json::array()));
        }
    } else if (def->contains("fields") && (*def)["fields"].is_array()) {
        m_decoderModes.push_back(std::string());
        m_decoderModeFields.push_back(parseFields((*def)["fields"]));
    }
}

void DebuggerCustomView::UpdateDecoder(const std::string& docName, const std::string& regName,
                                       uint32_t value, int bits, bool valueValid)
{
    if (regName != m_decoderRegName) {
        // The specific register changed (possibly within a shared doc group such as DMACON vs
        // DMACONR, or BLTCON0 vs BLTCON1) — reload its fields and reseed its value.
        m_decoderDocName = docName;
        m_decoderRegName = regName;
        m_decoderBits = (bits == 32) ? 32 : 16;
        SetDecoderFields(regName, docName);
        if (valueValid)
            m_decoderValue = value;
        m_lastSyncValue = valueValid ? static_cast<int64_t>(value) : -1;
    } else if (valueValid && m_decoderSync) {
        // Following the live value: update only when it actually changes, so edits made while
        // the emulator is paused survive frame-to-frame.
        if (static_cast<int64_t>(value) != m_lastSyncValue)
            m_decoderValue = value;
        m_lastSyncValue = static_cast<int64_t>(value);
    }
}

uint32_t DebuggerCustomView::FieldValue(const DecoderField& f) const
{
    int width = f.hi - f.lo + 1;
    uint32_t mask = (width >= 32) ? 0xFFFFFFFFu : (((1u << width) - 1u) << f.lo);
    return (m_decoderValue & mask) >> f.lo;
}

void DebuggerCustomView::SetFieldValue(const DecoderField& f, uint32_t v)
{
    int width = f.hi - f.lo + 1;
    uint32_t mask = (width >= 32) ? 0xFFFFFFFFu : (((1u << width) - 1u) << f.lo);
    m_decoderValue = (m_decoderValue & ~mask) | ((v << f.lo) & mask);
}

uint32_t DebuggerCustomView::DecoderAddress() const
{
    if (m_selectedOffset < 0)
        return 0;
    return core::ArMemoryAccess::kCustomBase + static_cast<uint32_t>(m_selectedOffset);
}

bool DebuggerCustomView::WriteDecoderValueToChipset()
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu || m_selectedOffset < 0)
        return false;
    const uint32_t base = DecoderAddress();
    if (m_decoderBits != 32)
        return emu->WriteCustomRegister(base, static_cast<uint16_t>(m_decoderValue & 0xFFFFu));

    // A 32-bit row is a pointer pair: the high word (PTH) lives at the row's own offset and the
    // low word (PTL) two bytes on. m_decoderValue holds the *decoded* pointer (the row's
    // displayValue went through core::SwapPtrWords), so it splits the obvious way round.
    const bool hi = emu->WriteCustomRegister(base, static_cast<uint16_t>(m_decoderValue >> 16));
    const bool lo =
        emu->WriteCustomRegister(base + 2, static_cast<uint16_t>(m_decoderValue & 0xFFFFu));
    return hi && lo;
}

void DebuggerCustomView::DrawDecoderLockControls()
{
    if (!m_lockUi.Valid() || m_selectedOffset < 0)
        return;

    const uint32_t addr = DecoderAddress();
    const bool haveEmu = m_emulatorView && m_emulatorView->GetEmulator();

    // Read once: Set needs to know whether a lock is already holding this register, because
    // then "set" means "change what is being held", not "poke it once and watch the lock put
    // the old value straight back".
    const core::LockEntry* existing = m_lockUi.At(addr);
    const uint8_t width = m_decoderBits == 32 ? 4 : 2;

    ImGui::SameLine();
    ImGui::BeginDisabled(!haveEmu);
    if (ImGui::Button("Set")) {
        std::string err;
        const bool wrote = WriteDecoderValueToChipset();
        if (existing && existing->mode == core::LockMode::Set) {
            // Retarget the lock so it holds the new value from here on. Without this the write
            // above lands once and the next CPU or copper write is forced back to the old
            // value - which reads as the field resetting itself.
            if (m_lockUi.locks->SetValue(existing->id, m_decoderValue, width, err)) {
                m_lockStatus = fmt::format("Now holding ${:0{}X}.", m_decoderValue, width * 2);
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

    // The lock covers the register's own address. A 32-bit pointer row is two registers, so its
    // lock spans the full 4 bytes starting at the high word - which is exactly what a 32-bit Set
    // lock protects.
    bool locked = existing != nullptr;
    ImGui::SameLine();
    if (ImGui::Checkbox("Lock", &locked)) {
        std::string err;
        if (locked) {
            if (AddSetLock(
                    m_lockUi, addr, width, m_decoderValue,
                    fmt::format("{} = ${:0{}X}", m_decoderRegName, m_decoderValue, width * 2),
                    err)) {
                m_lockStatus = fmt::format("Locked ${:08X}.", addr);
                m_lockStatusIsError = false;
            } else {
                m_lockStatus = err;
                m_lockStatusIsError = true;
            }
        } else {
            RemoveLocksAt(m_lockUi, addr);
            m_lockStatus = fmt::format("Unlocked ${:08X}.", addr);
            m_lockStatusIsError = false;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Force this value: every write to the register is rewritten to it,\n"
                          "whether it comes from the CPU or from a copper MOVE - which is what\n"
                          "makes it useful on a copper-driven register such as BPLCON1.\n\n"
                          "It is the same store the Trainer window and the PR console command\n"
                          "use, so a lock made here shows up in both.\n\n"
                          "Copper timing is untouched: the list still runs, its DMA cycles are\n"
                          "still consumed and its PC still advances. Only the write is caught.");
    }

    if (!m_lockStatus.empty()) {
        if (m_lockStatusIsError)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", m_lockStatus.c_str());
        else
            ImGui::TextDisabled("%s", m_lockStatus.c_str());
    }
}

void DebuggerCustomView::DrawDecoderPanel()
{
    if (m_decoderRegName.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "Select a register";
        ImVec2 textSize = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
        return;
    }

    // Header: register name, editable hex value, and the live-value Sync toggle.
    ImGui::TextUnformatted(m_decoderRegName.c_str());
    ImGui::SameLine();
    char buf[16];
    if (m_decoderBits == 32)
        snprintf(buf, sizeof(buf), "%08X", m_decoderValue);
    else
        snprintf(buf, sizeof(buf), "%04X", m_decoderValue & 0xFFFFu);
    ImGui::SetNextItemWidth(m_decoderBits == 32 ? 80.0f : 56.0f);
    if (ImGui::InputText("##decval", buf, sizeof(buf),
                         ImGuiInputTextFlags_CharsHexadecimal |
                             ImGuiInputTextFlags_AutoSelectAll)) {
        uint32_t v = static_cast<uint32_t>(std::strtoul(buf, nullptr, 16));
        m_decoderValue = (m_decoderBits == 32) ? v : (v & 0xFFFFu);
        // Stop following the live value, or the next frame overwrites what is being typed.
        // A copper-driven register changes every frame, so without this the field is
        // effectively uneditable while the machine runs.
        m_decoderSync = false;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Sync", &m_decoderSync);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Follow the selected register's live value.\nTurn off to freeze the "
                          "value and use the panel as a calculator.");
    DrawDecoderLockControls();

    // Mode selector for registers with multiple variants (e.g. blitter Area / Line mode).
    if (m_decoderModes.size() > 1) {
        for (int i = 0; i < static_cast<int>(m_decoderModes.size()); ++i) {
            if (i > 0)
                ImGui::SameLine();
            ImGui::RadioButton(m_decoderModes[i].c_str(), &m_decoderMode, i);
        }
    }

    ImGui::Separator();

    const std::vector<DecoderField>& fields = ActiveFields();
    if (fields.empty()) {
        ImGui::TextDisabled("No field breakdown available.");
        return;
    }

    static const ImVec4 kSet = {0.30f, 0.85f, 0.30f, 1.0f};
    static const ImVec4 kClear = {0.55f, 0.55f, 0.55f, 1.0f};

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable;
    PushFontRole(FontRole::Code);
    if (ImGui::BeginTable("##decfields", 4, flags)) {
        ImGui::TableSetupColumn("Bits", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Val", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("##info", ImGuiTableColumnFlags_WidthFixed, 18.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < fields.size(); ++i) {
            const DecoderField& f = fields[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            ImGui::TableSetColumnIndex(0);
            if (f.hi == f.lo)
                ImGui::Text("%d", f.hi);
            else
                ImGui::Text("%d-%d", f.hi, f.lo);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(f.name.c_str());

            // Value column: single-bit fields toggle 0/1, multi-bit fields edit a number.
            ImGui::TableSetColumnIndex(2);
            if (f.hi == f.lo) {
                bool set = (m_decoderValue >> f.lo) & 1u;
                ImGui::PushStyleColor(ImGuiCol_Text, set ? kSet : kClear);
                if (ImGui::Selectable(set ? "1" : "0", false, ImGuiSelectableFlags_None,
                                      ImVec2(16.0f, 0.0f)))
                    m_decoderValue ^= (1u << f.lo);
                ImGui::PopStyleColor();
            } else {
                int width = f.hi - f.lo + 1;
                int maxv = (width >= 31) ? 0x7FFFFFFF : ((1 << width) - 1);
                int v = static_cast<int>(FieldValue(f));
                ImGui::SetNextItemWidth(36.0f);
                if (ImGui::DragInt("##v", &v, 0.1f, 0, maxv, "%d"))
                    SetFieldValue(f, static_cast<uint32_t>(v));
            }

            // Info column: (i) with the field's description as a wrapped tooltip.
            ImGui::TableSetColumnIndex(3);
            if (!f.desc.empty()) {
                ImGui::TextDisabled("(i)");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
                    ImGui::TextUnformatted(f.desc.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    PopFontRole();
}

} // namespace adm::ui
