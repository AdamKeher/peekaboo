#include "DebuggerCPUView.h"
#include <fmt/format.h>
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include "../../core/AmigaEmulator.h"
#include "../../core/debug/SnapshotBuilders.h"
#include "../AddressLink.h"
#include "../FontRoles.h"
#include "../HyperlinkText.h"
#include "../IconsFontAwesome6.h"
#include "EmulatorView.h"
#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

void DebuggerCPUView::DrawRegLink(const char* id, uint32_t value, int hexDigits, bool interactive)
{
    char text[16];
    snprintf(text, sizeof(text), "%0*X", hexDigits, value);

    if (!interactive || !m_addrCtx || !m_addrCtx->valid()) {
        ImGui::TextUnformatted(text);
        return;
    }

    // Back the link with a real (invisible) item rather than a bare HoverableLink: clicking it
    // registers an ImGui ActiveId, which suppresses ImGui's EndFrame "focus the clicked window"
    // logic so the Memory window's SetNextWindowFocus request survives. (HoverableLink draws
    // nothing interactive, leaving HoveredId == 0 and letting the CPU window grab focus back.)
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::CalcTextSize(text);
    char btnId[24];
    snprintf(btnId, sizeof(btnId), "##reglink_%s", id);
    bool clicked = ImGui::InvisibleButton(btnId, size);
    bool hovered = ImGui::IsItemHovered();

    ImU32 col = ImGui::ColorConvertFloat4ToU32(kHyperlinkColor);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(pos, col, text);
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddLine({pos.x, pos.y + size.y - 1.0f}, {pos.x + size.x, pos.y + size.y - 1.0f}, col);
    }
    if (clicked)
        RunAddressDefaultAction(value, *m_addrCtx, AddressLinkHost::Window, AddressDefault::Memory);

    // Right-click → shared address-actions menu (breakpoint/watchpoint/label/…).
    char popupId[32];
    snprintf(popupId, sizeof(popupId), "##reglinkctx_%s", id);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup(popupId);
    if (ImGui::BeginPopup(popupId)) {
        DrawAddressMenuBody(value, *m_addrCtx, AddressLinkHost::Window, AddressDefault::Memory);
        ImGui::EndPopup();
    }
}

DebuggerCPUView::DebuggerCPUView(EmulatorView* emulatorView) : m_emulatorView(emulatorView) {}

void DebuggerCPUView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void DebuggerCPUView::Draw()
{
    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(640, 400), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }

    if (ImGui::Begin(ICON_FA_MICROCHIP " CPU###CPU##debugger", &m_open, flags)) {
        if (m_turboSuppressed) {
            DrawTurboSuppressedPlaceholder();
        } else {
            bool running = m_emulatorView && m_emulatorView->IsRunning();
            bool paused = m_emulatorView && m_emulatorView->IsPaused();

            ImGui::BeginDisabled(!running);
            if (ImGui::Button(paused ? ICON_FA_PLAY " Resume" : ICON_FA_PAUSE " Break")) {
                m_emulatorView->TogglePause();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (!running)
                    ImGui::SetTooltip("Emulator must be running to break/resume");
                else
                    ImGui::SetTooltip(paused ? "Resume execution" : "Break execution");
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!paused || !running);
            if (ImGui::Button(ICON_FA_FORWARD_STEP " Step Frame")) {
                m_emulatorView->StepFrame();
                if (m_onStepFrame)
                    m_onStepFrame();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (!running)
                    ImGui::SetTooltip("Emulator must be running to step");
                else if (!paused)
                    ImGui::SetTooltip("Emulator must be paused to step");
                else
                    ImGui::SetTooltip("Advance emulator by one frame (stops at breakpoints)");
            }

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FORWARD_FAST " End Frame")) {
                m_emulatorView->EndFrame();
                if (m_onStepFrame)
                    m_onStepFrame();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (!running)
                    ImGui::SetTooltip("Emulator must be running to end the frame");
                else if (!paused)
                    ImGui::SetTooltip("Emulator must be paused to end the frame");
                else
                    ImGui::SetTooltip("Complete the current (mid-execution) frame to the next "
                                      "boundary, stepping past breakpoints");
            }

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ANGLE_RIGHT " Step")) {
                m_emulatorView->StepInstruction();
                if (m_onStepFrame)
                    m_onStepFrame();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (!running)
                    ImGui::SetTooltip("Emulator must be running to step");
                else if (!paused)
                    ImGui::SetTooltip("Emulator must be paused to step");
                else
                    ImGui::SetTooltip("Single Step Instruction");
            }

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ANGLES_RIGHT " Step Over")) {
                m_emulatorView->StepOver();
                if (m_onStepFrame)
                    m_onStepFrame();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (!running)
                    ImGui::SetTooltip("Emulator must be running to step");
                else if (!paused)
                    ImGui::SetTooltip("Emulator must be paused to step");
                else
                    ImGui::SetTooltip("Step Over Subroutine");
            }
            ImGui::EndDisabled();

            if (running) {
                ImGui::SameLine(0, 16);
                auto* emu = m_emulatorView->GetEmulator();
                bool traceEnabled = emu ? emu->IsCpuTraceEnabled() : false;
                if (ImGui::Checkbox("CPU Trace", &traceEnabled)) {
                    if (emu)
                        emu->SetCpuTraceEnabled(traceEnabled);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Record every M68K instruction per frame\n"
                                      "Enables Trace mode in the Disassembly view\n"
                                      "~14k-17k entries/frame on A500 (~3.7 MB)");
                }
            }

            ImGui::Separator();

            PushFontRole(FontRole::Code);
            DrawContent();
            PopFontRole();
        }
    }
    ImGui::End();
}

void DebuggerCPUView::Clear()
{
    m_traceMode = false;
    m_traceEntry = {};
}

void DebuggerCPUView::SetTraceEntry(bool active, const adm::AmigaEmulator::CpuTraceEntry& entry)
{
    m_traceMode = active;
    m_traceEntry = entry;
}

namespace {

constexpr ImVec4 kDataColor{0.4f, 0.8f, 1.0f, 1.0f};
constexpr ImVec4 kAddrColor{1.0f, 0.8f, 0.4f, 1.0f};
constexpr ImVec4 kSpecialColor{1.0f, 0.4f, 0.4f, 1.0f};
constexpr ImVec4 kControlColor{0.75f, 0.6f, 1.0f, 1.0f};
constexpr ImVec4 kFpuColor{0.5f, 1.0f, 0.7f, 1.0f};
constexpr ImVec4 kFlagSetColor{0.2f, 1.0f, 0.2f, 1.0f};

// Width of the name column: fits "FPIAR:" / "MMUSR:".
constexpr float kExtNameWidth = 66.0f;

ImVec4 GroupColor(adm::core::CpuRegGroup group)
{
    switch (group) {
        case adm::core::CpuRegGroup::Stack:
            return kAddrColor;
        case adm::core::CpuRegGroup::Fpu:
            return kFpuColor;
        default:
            return kControlColor;
    }
}

const char* Dashes(int hexDigits)
{
    switch (hexDigits) {
        case 4:
            return "----";
        case 16:
            return "--------:--------";
        case 24:
            return "----------------------";
        default:
            return "--------";
    }
}

} // namespace

void DebuggerCPUView::DrawContent()
{
    bool running = m_emulatorView && m_emulatorView->IsRunning();
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;

    Panel p;
    // The machine, for greying what it lacks even when there are no values to show.
    if (m_haveLastExt) {
        p.cpuModel = m_lastExt.cpuModel;
        p.fpuModel = m_lastExt.fpuModel;
    }
    p.addressBits = p.cpuModel < 68020 ? 24 : 32;
    if (running && emu) {
        if (auto config = emu->GetMachineConfig()) {
            p.cpuModel = config->cpuModel;
            p.fpuModel = config->fpuModel;
            p.addressBits = adm::core::AddressBits(*config);
        }
    }

    // Trace mode: show captured state from a selected instruction
    if (m_traceMode) {
        uint32_t traceLRef = uint32_t(m_traceEntry.vpos) * 228u + uint32_t(m_traceEntry.hpos);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "TRACE @ L%07X - [$%08X]", traceLRef,
                           m_traceEntry.pc);
        ImGui::Separator();
        p.d = m_traceEntry.d;
        p.a = m_traceEntry.a;
        p.pc = m_traceEntry.pc;
        p.sr = m_traceEntry.sr;
        p.valid = true;
        p.interactive = true;
        p.extNote = "Not captured in the CPU trace";
        DrawPanel(p);
        return;
    }

    // Live mode
    adm::AmigaEmulator::CpuState state{};
    bool stateValid = false;
    // While the CPU is free-running (Play / TA / step-over) the live registers churn
    // through unreadable values, so hold the last paused snapshot (greyed) and snap
    // back to live the moment execution stops.
    bool paused = m_emulatorView && m_emulatorView->IsPaused();
    bool freeze = running && !paused;
    if (running) {
        if (freeze && m_haveLastState) {
            state = m_lastState;
            stateValid = true;
        } else if (!m_emulatorView->GetCpuState(state)) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            const char* msg = "CPU State unavailable (core does not export it)";
            ImVec2 textSize = ImGui::CalcTextSize(msg);
            ImGui::SetCursorPos(
                ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
            ImGui::TextDisabled("%s", msg);
            return;
        } else {
            stateValid = true;
            if (!freeze) {
                m_lastState = state;
                m_haveLastState = true;
            }
        }

        if (!freeze) {
            adm::core::CpuExtendedState ext;
            if (m_emulatorView->GetCpuExtState(ext)) {
                m_lastExt = std::move(ext);
                m_haveLastExt = true;
            } else {
                m_haveLastExt = false;
            }
        }
        if (m_haveLastExt) {
            p.ext = &m_lastExt;
            p.cpuModel = m_lastExt.cpuModel;
            p.fpuModel = m_lastExt.fpuModel;
        } else if (emu && !emu->HasCpuExtState()) {
            p.extNote = "The loaded core predates 1.102.0 and doesn't export these registers";
        } else {
            p.extNote = "Registers unavailable";
        }
    } else {
        m_haveLastState = false;
        p.extNote = "Emulator not running";
    }

    if (freeze) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), ICON_FA_PLAY " Running...");
        ImGui::Separator();
    }
    ImGui::BeginDisabled(freeze);
    p.d = state.d;
    p.a = state.a;
    p.pc = state.pc;
    p.sr = state.sr;
    p.valid = stateValid;
    p.interactive = !freeze;
    DrawPanel(p);
    ImGui::EndDisabled();
}

void DebuggerCPUView::DrawPanel(const Panel& p)
{
    adm::core::debug::CpuRawState raw;
    std::copy(p.d, p.d + 8, raw.d.begin());
    std::copy(p.a, p.a + 8, raw.a.begin());
    raw.pc = p.pc;
    raw.sr = p.sr;
    const auto snapshot = adm::core::debug::CpuRegisterSnapshotBuilder::Build(raw, false);

    DrawModelStrip(p);
    ImGui::Separator();

    if (ImGui::BeginTable("CpuRegsTable", 2, ImGuiTableFlags_None)) {
        ImGui::TableNextColumn();
        ImGui::Text("Data Registers");
        ImGui::Separator();
        for (int i = 0; i < 8; ++i) {
            ImGui::TextColored(kDataColor, "D%d:", i);
            ImGui::SameLine(50.0f);
            if (p.valid)
                ImGui::Text("%08X", snapshot.d[i]);
            else
                ImGui::TextDisabled("--------");
        }

        ImGui::TableNextColumn();
        ImGui::Text("Address Registers");
        ImGui::Separator();
        for (int i = 0; i < 8; ++i) {
            ImGui::TextColored(kAddrColor, "A%d:", i);
            ImGui::SameLine(50.0f);
            if (p.valid) {
                char id[8];
                snprintf(id, sizeof(id), "A%d", i);
                DrawRegLink(id, snapshot.a[i], 8, p.interactive);
            } else {
                ImGui::TextDisabled("--------");
            }
            if (i == 7 && p.valid) {
                // A7 is whichever stack SR.S/SR.M select.
                const char* stack =
                    !snapshot.supervisor ? "USP"
                    : (p.sr & (1 << 12)) != 0 && p.cpuModel >= 68020 && p.cpuModel <= 68040
                        ? "MSP"
                        : adm::core::IspName(p.cpuModel);
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", stack);
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    DrawStatus(p);
    ImGui::Spacing();

    DrawStack(p);
    DrawExtGroup(p, adm::core::CpuRegGroup::System, "System & Cache");
    DrawExtGroup(p, adm::core::CpuRegGroup::Mmu, "MMU");
    DrawFpu(p);
}

void DebuggerCPUView::DrawModelStrip(const Panel& p)
{
    std::string fpu = adm::core::FpuName(p.fpuModel, p.cpuModel);
    std::string mmu = p.ext ? adm::core::CpuMmuName(*p.ext) : std::string("?");
    ImGui::TextColored(kSpecialColor, "%d", p.cpuModel);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("FPU %s", fpu.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("MMU %s", mmu.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("%d-bit address", p.addressBits);
}

void DebuggerCPUView::DrawStatus(const Panel& p)
{
    if (!ImGui::BeginTable("CpuSpecialTable", 2, ImGuiTableFlags_None))
        return;
    ImGui::TableNextColumn();

    ImGui::Text("Special Purpose");
    ImGui::Separator();
    ImGui::TextColored(kSpecialColor, "PC:");
    ImGui::SameLine(50.0f);
    if (p.valid)
        DrawRegLink("PC", p.pc, 8, p.interactive);
    else
        ImGui::TextDisabled("--------");

    ImGui::TextColored(kSpecialColor, "SR:");
    ImGui::SameLine(50.0f);
    if (p.valid)
        DrawRegLink("SR", p.sr, 4, p.interactive);
    else
        ImGui::TextDisabled("----");

    ImGui::TableNextColumn();

    ImGui::Text("Status Flags");
    ImGui::Separator();

    const bool t0m = p.cpuModel >= 68020 && p.cpuModel <= 68040;
    // present=false: the CPU has no such bit; drawn faint with a tooltip saying which do.
    auto DrawFlag = [&](const char* name, int bit, bool present, const char* tip) {
        const bool set = p.valid && (p.sr & (1 << bit)) != 0;
        if (!present)
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 0.35f), "%s", name);
        else if (set)
            ImGui::TextColored(kFlagSetColor, "%s", name);
        else
            ImGui::TextDisabled("%s", name);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (present)
                ImGui::SetTooltip("%s", tip);
            else
                ImGui::SetTooltip("%s\nNot on a %d (68020 / 68030 / 68040)", tip, p.cpuModel);
        }
    };

    DrawFlag("T1", 15, true, "Trace on any instruction");
    ImGui::SameLine();
    DrawFlag("T0", 14, t0m, "Trace on change of flow");
    ImGui::SameLine();
    DrawFlag("S", 13, true, "Supervisor");
    ImGui::SameLine();
    DrawFlag("M", 12, t0m, "Master stack (MSP) in use");
    ImGui::SameLine();
    ImGui::Text("  ");
    ImGui::SameLine();
    DrawFlag("X", 4, true, "Extend");
    ImGui::SameLine();
    DrawFlag("N", 3, true, "Negative");
    ImGui::SameLine();
    DrawFlag("Z", 2, true, "Zero");
    ImGui::SameLine();
    DrawFlag("V", 1, true, "Overflow");
    ImGui::SameLine();
    DrawFlag("C", 0, true, "Carry");

    if (p.valid)
        ImGui::Text("IPL %d", (p.sr >> 8) & 7);
    else
        ImGui::TextDisabled("IPL -");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(
            "Interrupt priority mask (SR bits 10-8): levels at or below are held off");
    ImGui::SameLine();
    const bool stopped = p.ext && p.ext->stopped;
    if (stopped)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "STOP");
    else
        ImGui::TextDisabled("STOP");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("CPU halted by a STOP instruction, waiting for an interrupt");

    ImGui::EndTable();
}

void DebuggerCPUView::ExtTooltip(const Panel& p, const adm::core::CpuRegDef& def, bool present)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        return;
    const bool onCpu = adm::core::CpuHasRegister(def, p.cpuModel, p.fpuModel);
    if (!onCpu)
        ImGui::SetTooltip("%s\n%s", def.description,
                          adm::core::NotOnCpuText(def, p.cpuModel).c_str());
    else if (!present && p.extNote)
        ImGui::SetTooltip("%s\n%s", def.description, p.extNote);
    else
        ImGui::SetTooltip("%s", def.description);
}

void DebuggerCPUView::DrawExtValue(const Panel& p, const adm::core::CpuRegDef& def)
{
    const bool onCpu = adm::core::CpuHasRegister(def, p.cpuModel, p.fpuModel);
    std::optional<adm::core::CpuRegReading> reading;
    if (p.ext)
        reading = adm::core::ReadCpuRegister(*p.ext, def);

    const char* label =
        def.id == adm::core::CpuRegId::Isp ? adm::core::IspName(p.cpuModel) : def.name;
    if (onCpu)
        ImGui::TextColored(GroupColor(def.group), "%s:", label);
    else
        ImGui::TextDisabled("%s:", label);
    ExtTooltip(p, def, reading.has_value());
    ImGui::SameLine(kExtNameWidth);

    if (!reading) {
        int digits = 8;
        if (p.cpuModel == 68030 &&
            (def.id == adm::core::CpuRegId::Crp || def.id == adm::core::CpuRegId::Srp))
            digits = 16;
        ImGui::TextDisabled("%s", Dashes(digits));
        ExtTooltip(p, def, false);
        return;
    }

    char id[16];
    snprintf(id, sizeof(id), "X%s", def.name);
    if (reading->hexDigits == 16) {
        // 64-bit root pointer: descriptor word, then the table address (the link).
        ImGui::Text("%08X:", uint32_t(reading->value >> 32));
        ImGui::SameLine(0, 0);
        DrawRegLink(id, uint32_t(reading->value), 8, p.interactive);
    } else if (def.isAddress) {
        DrawRegLink(id, uint32_t(reading->value), reading->hexDigits, p.interactive);
    } else {
        ImGui::TextUnformatted(adm::core::FormatCpuRegister(*reading).c_str());
        ExtTooltip(p, def, true);
    }
}

void DebuggerCPUView::DrawStack(const Panel& p)
{
    char header[64];
    snprintf(header, sizeof(header), "Stack Pointers%s%s###cpu_stack", p.ext ? "  - A7 is " : "",
             p.ext ? adm::core::ActiveStackName(*p.ext) : "");
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
        return;
    if (!ImGui::BeginTable("CpuStackTable", 3, ImGuiTableFlags_None))
        return;
    const char* active = p.ext ? adm::core::ActiveStackName(*p.ext) : "";
    for (const auto& def : adm::core::CpuRegisterCatalog()) {
        if (def.group != adm::core::CpuRegGroup::Stack)
            continue;
        ImGui::TableNextColumn();
        DrawExtValue(p, def);
        const char* label =
            def.id == adm::core::CpuRegId::Isp ? adm::core::IspName(p.cpuModel) : def.name;
        if (p.ext && strcmp(label, active) == 0) {
            ImGui::SameLine();
            ImGui::TextColored(kFlagSetColor, "A7");
        }
    }
    ImGui::EndTable();
}

void DebuggerCPUView::DrawExtGroup(const Panel& p, adm::core::CpuRegGroup group, const char* title)
{
    // How the group stands on this machine, shown in the header so a folded one still says it.
    bool anyOnCpu = false;
    for (const auto& def : adm::core::CpuRegisterCatalog())
        if (def.group == group && adm::core::CpuHasRegister(def, p.cpuModel, p.fpuModel))
            anyOnCpu = true;

    std::string note;
    if (group == adm::core::CpuRegGroup::Mmu) {
        if (!anyOnCpu)
            note = fmt::format("none on a {}", p.cpuModel);
        else if (p.ext && p.ext->mmuModel == 0)
            note = fmt::format("{} - translation off", p.cpuModel);
        else if (p.ext && p.ext->mmuEc)
            note = fmt::format("{} - EC, no translation", adm::core::CpuMmuName(*p.ext));
        else
            note = std::to_string(p.cpuModel);
    } else if (!anyOnCpu) {
        note = fmt::format("none on a {}", p.cpuModel);
    }

    std::string header = note.empty() ? std::string(title) : fmt::format("{}  - {}", title, note);
    header += fmt::format("###cpu_group_{}", static_cast<int>(group));
    if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        return;

    char tableId[32];
    snprintf(tableId, sizeof(tableId), "CpuExtTable%d", static_cast<int>(group));
    if (!ImGui::BeginTable(tableId, 3, ImGuiTableFlags_None))
        return;
    for (const auto& def : adm::core::CpuRegisterCatalog()) {
        if (def.group != group)
            continue;
        ImGui::TableNextColumn();
        DrawExtValue(p, def);
    }
    ImGui::EndTable();
}

void DebuggerCPUView::DrawFpu(const Panel& p)
{
    const auto& fpcrDef = adm::core::GetCpuRegister(adm::core::CpuRegId::Fpcr);
    const bool onCpu = adm::core::CpuHasRegister(fpcrDef, p.cpuModel, p.fpuModel);
    std::string header =
        onCpu ? fmt::format("FPU  - {}###cpu_group_fpu", adm::core::FpuName(p.fpuModel, p.cpuModel))
        : (fpcrDef.cpuMask & adm::core::CpuModelBit(p.cpuModel)) != 0
            ? std::string("FPU  - none fitted###cpu_group_fpu")
            : fmt::format("FPU  - none on a {}###cpu_group_fpu", p.cpuModel);
    if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        return;

    const bool haveValues = p.ext && p.ext->fpuPresent;
    if (ImGui::BeginTable("CpuFpuTable", 3, ImGuiTableFlags_SizingFixedFit)) {
        for (int i = 0; i < 8; ++i) {
            const auto& def = adm::core::GetCpuRegister(
                static_cast<adm::core::CpuRegId>(static_cast<int>(adm::core::CpuRegId::Fp0) + i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (onCpu)
                ImGui::TextColored(kFpuColor, "%s:", def.name);
            else
                ImGui::TextDisabled("%s:", def.name);
            ExtTooltip(p, def, haveValues);
            ImGui::TableNextColumn();
            if (haveValues) {
                const auto& fp = p.ext->fp[i];
                ImGui::TextUnformatted(adm::core::FormatFpValue(fp).c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", adm::core::FormatFpRaw(fp).c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("96-bit extended: sign/exponent - mantissa hi - mantissa lo");
            } else {
                ImGui::TextDisabled("-");
                ExtTooltip(p, def, false);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", Dashes(24));
            }
        }
        ImGui::EndTable();
    }

    for (auto id :
         {adm::core::CpuRegId::Fpcr, adm::core::CpuRegId::Fpsr, adm::core::CpuRegId::Fpiar}) {
        const auto& def = adm::core::GetCpuRegister(id);
        DrawExtValue(p, def);
        if (haveValues && id != adm::core::CpuRegId::Fpiar) {
            ImGui::SameLine();
            std::string decoded = id == adm::core::CpuRegId::Fpcr
                                      ? adm::core::DecodeFpcr(p.ext->fpcr)
                                      : adm::core::DecodeFpsr(p.ext->fpsr);
            ImGui::TextDisabled("%s", decoded.c_str());
        }
    }
}

} // namespace adm::ui
