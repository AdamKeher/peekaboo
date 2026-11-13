#include "DebuggerCopperView.h"
#include <fmt/format.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include "../../core/AmigaCustom.h"
#include "../../core/AmigaEmulator.h"
#include "../../core/CopperDisassembler.h"
#include "../AddressLink.h"
#include "../FontRoles.h"
#include "../HyperlinkText.h"
#include "../IconsFontAwesome6.h"
#include "../TableLayout.h"
#include "BreakpointsView.h"
#include "EmulatorView.h"
#include "RegisterDocRenderer.h"
#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

DebuggerCopperView::DebuggerCopperView(EmulatorView* emulatorView) : m_emulatorView(emulatorView) {}

void DebuggerCopperView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void DebuggerCopperView::JumpToTraceLRef(uint32_t lRef)
{
    m_open = true;
    m_requestFocus = true;
    m_mode = Mode::Trace;
    m_scrubberLRef = lRef;

    // Select the matching trace row: prefer an exact L-Ref hit, else the last row at or before
    // it. m_formattedLog is the row list the table renders - it interleaves pointer-write rows
    // with the copper's instructions, so it is the only list whose indices the UI can use.
    int sel = -1;
    for (int i = 0; i < static_cast<int>(m_formattedLog.size()); ++i) {
        uint32_t rowLRef = m_formattedLog[i].lRefValue;
        if (rowLRef == lRef) {
            sel = i;
            break;
        }
        if (rowLRef <= lRef)
            sel = i;
        else
            break;
    }
    if (sel >= 0) {
        m_selectedTraceIdx = sel;
        m_traceScrollToSelected = true;
    }
}

void DebuggerCopperView::Draw()
{
    if (!m_open)
        return;

    if (m_requestFocus) {
        ImGui::SetNextWindowFocus();
        m_requestFocus = false;
    }

    ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }

    bool running = m_emulatorView && m_emulatorView->IsRunning();

    std::string title = ICON_FA_LIST_OL " Copper###Copper##debugger";
    // if (m_execOverflow)
    //     title = "Copper##[log truncated] debugger";

    // Begin() returns false when the window is collapsed or its dock tab is inactive — skip the
    // (expensive) trace rebuild and content entirely in that case.
    if (ImGui::Begin(title.c_str(), &m_open, flags)) {
        if (m_turboSuppressed) {
            DrawTurboSuppressedPlaceholder();
        } else {
            if (running)
                RebuildExecutionLog();
            else
                Clear();
            DrawContent();
        }
    }
    ImGui::End();
}

void DebuggerCopperView::Clear()
{
    if (!m_executionLog.empty()) {
        m_executionLog.clear();
        m_formattedLog.clear();
    }
    m_pointerWrites.clear();
    m_midFrameRewrite = false;
    m_midFrameSummary.clear();
    m_firstPointerRow = -1;
    m_execOverflow = false;
    m_lastRebuildEmuFrame = UINT64_MAX; // force a rebuild on next run
}

void DebuggerCopperView::RebuildExecutionLog()
{
    if (!m_emulatorView || !m_emulatorView->GetEmulator())
        return;

    // The copper trace only changes at frame boundaries; skip the rebuild + per-event string
    // reformat while the emulator is paused on the same frame.
    uint64_t emuFrame = m_emulatorView->GetEmulatorFrameCount();
    if (emuFrame == m_lastRebuildEmuFrame)
        return;
    m_lastRebuildEmuFrame = emuFrame;

    auto* emu = m_emulatorView->GetEmulator();
    // Member scratch (was a function-local static shared across every instance of this view).
    if (m_eventScratch.empty())
        m_eventScratch.resize(adm::AmigaEmulator::kMaxCopperExecEvents);
    adm::AmigaEmulator::CopperExecEvent* buf = m_eventScratch.data();
    int count = emu->GetCopperExecEvents(buf, adm::AmigaEmulator::kMaxCopperExecEvents);
    m_execOverflow = emu->GetCopperExecOverflow();

    m_executionLog.assign(buf, buf + count);

    // List assignments are recomputed in RebuildFormattedLog, which walks the trace and the
    // register-write log together: a CPU strobe of COPJMP1/COPJMP2 switches lists without
    // leaving anything behind in the trace, so the trace on its own cannot tell which list ran.
    RebuildPointerWrites();
    RebuildFormattedLog();
}

namespace {

const char* CopperPointerRegName(uint16_t reg)
{
    switch (reg) {
        case 0x080:
            return "COP1LCH";
        case 0x082:
            return "COP1LCL";
        case 0x084:
            return "COP2LCH";
        case 0x086:
            return "COP2LCL";
        case 0x088:
            return "COPJMP1";
        case 0x08A:
            return "COPJMP2";
        default:
            return "COP???";
    }
}

constexpr uint32_t ChipAddr(uint32_t a) { return a & 0x00FFFFFFu; }

} // namespace

void DebuggerCopperView::RebuildPointerWrites()
{
    m_pointerWrites.clear();
    if (!m_emulatorView || !m_emulatorView->GetEmulator())
        return;

    auto* emu = m_emulatorView->GetEmulator();
    // Optional export. On a core predating the register-write log the copper's own MOVEs are
    // still visible as ordinary trace rows; only the CPU-side writes are lost.
    if (!emu->HasRegWriteLog())
        return;

    if (m_regWriteScratch.empty())
        m_regWriteScratch.resize(adm::AmigaEmulator::kMaxRegWriteEvents);
    int count =
        emu->GetRegWriteEvents(m_regWriteScratch.data(), adm::AmigaEmulator::kMaxRegWriteEvents);

    for (int i = 0; i < count; ++i) {
        const auto& ev = m_regWriteScratch[i];
        if (ev.reg < 0x080 || ev.reg > 0x08A)
            continue;
        PointerWrite pw;
        pw.lRef = uint32_t(ev.vpos) * kCCKPerLine + uint32_t(ev.hpos);
        pw.reg = ev.reg;
        pw.value = ev.value;
        pw.pc = ev.pc;
        pw.fromCopper = ev.fromCopper;
        m_pointerWrites.push_back(pw);
    }
}

void DebuggerCopperView::RebuildFormattedLog()
{
    LoadCustomRegistersDef();

    const int execCount = static_cast<int>(m_executionLog.size());
    const int ptrCount = static_cast<int>(m_pointerWrites.size());

    m_formattedLog.clear();
    m_formattedLog.reserve(static_cast<size_t>(execCount + ptrCount));
    m_firstPointerRow = -1;
    m_midFrameRewrite = false;
    m_midFrameSummary.clear();

    // Running copper pointers. The copper restarts from COP1LC at vblank, so the first
    // instruction executed this frame is the COP1LC that was in effect then - a seed the
    // end-of-frame register snapshot cannot give us.
    uint32_t cop1 = execCount > 0 ? ChipAddr(m_executionLog[0].addr) : 0;
    uint32_t cop2 = 0;
    uint8_t cop1Have = execCount > 0 ? 0x3 : 0; // bit0 = high word known, bit1 = low word known
    uint8_t cop2Have = 0;

    // The list the copper is running right now, carried across the merged stream below. It moves
    // on a COPJMP, whether the copper strobed one itself with a MOVE (visible in the trace) or
    // the CPU wrote it directly (visible only in the register-write log) - which is why neither
    // log alone can label the rows that follow a switch the other one made. The copper restarts
    // from COP1 at vblank, so the frame opens on list 1.
    int activeList = 1;

    // Where each COPJMP strobe really landed, read back from the next instruction executed -
    // more trustworthy than our own pointer bookkeeping. Fed from both logs: CPU strobes come
    // from the register log, the copper's own COPJMP MOVEs only from the trace.
    std::vector<uint32_t> targets1, targets2;
    const uint32_t seed1 = cop1;

    auto noteTarget = [](std::vector<uint32_t>& v, uint32_t addr) {
        if (std::find(v.begin(), v.end(), addr) == v.end())
            v.push_back(addr);
    };

    int cpuLcWrites = 0, copperLcWrites = 0, strobeCount = 0;
    int lcWrites1 = 0, lcWrites2 = 0;

    int execIdx = 0, ptrIdx = 0, instrNumber = 0;
    char buf[64];

    // Merge the two per-frame logs by L-Ref. On a tie the instruction wins: a copper MOVE is
    // recorded when it is fetched, and the register write it performs lands a cycle or two later.
    while (execIdx < execCount || ptrIdx < ptrCount) {
        bool takeExec;
        if (execIdx >= execCount)
            takeExec = false;
        else if (ptrIdx >= ptrCount)
            takeExec = true;
        else {
            uint32_t execLRef = uint32_t(m_executionLog[execIdx].vpos) * kCCKPerLine +
                                uint32_t(m_executionLog[execIdx].hpos);
            takeExec = execLRef <= m_pointerWrites[ptrIdx].lRef;
        }

        if (takeExec) {
            auto& ev = m_executionLog[execIdx];
            m_formattedLog.emplace_back();
            FormattedCopperRow& row = m_formattedLog.back();
            const int rowIdx = static_cast<int>(m_formattedLog.size()) - 1;

            row.kind = RowKind::Instruction;
            row.execIdx = execIdx;

            uint32_t lRef = uint32_t(ev.vpos) * kCCKPerLine + uint32_t(ev.hpos);
            row.lRefValue = lRef;
            row.vline = lRef / kCCKPerLine;
            row.hclk = lRef % kCCKPerLine;

            snprintf(buf, sizeof(buf), "L%07X##ctr%d", lRef, rowIdx);
            row.lRefLabel = buf;

            // Whatever the core reported, the running list is the one tracked across both logs.
            ev.list = static_cast<uint8_t>(activeList);
            row.listStr = activeList == 2 ? "COP2" : "COP1";

            snprintf(buf, sizeof(buf), "%d", ++instrNumber);
            row.numStr = buf;

            snprintf(buf, sizeof(buf), "$%08X", ev.addr);
            row.addrStr = buf;

            adm::core::CopperInstruction inst =
                adm::core::CopperDisassembler::Disassemble(ev.word1, ev.word2, m_customRegsDef);

            row.wordsStr = inst.formattedWords;
            row.instrName = inst.name;
            row.details = inst.details;
            row.instrType = inst.type;
            row.isColor = inst.isColor;
            row.r = inst.r;
            row.g = inst.g;
            row.b = inst.b;
            row.word1 = ev.word1;
            row.word2 = ev.word2;
            row.addr = ev.addr;

            // A copper MOVE to COPJMP is handled inline by UAE's copper state machine and never
            // reaches custom_wput_1, so it is absent from the register-write log - the trace is
            // the only place it appears. Resolve its target the same way as a CPU strobe:
            // whatever the copper fetched next.
            if (inst.type == adm::core::CopperInstruction::Type::MOVE) {
                const uint16_t movedReg = ev.word1 & 0x01FEu;
                if (movedReg == 0x0088 || movedReg == 0x008A) {
                    ++strobeCount;
                    // The MOVE itself still belongs to the list it was fetched from; everything
                    // after it belongs to the list it just selected.
                    activeList = (movedReg == 0x008A) ? 2 : 1;
                    uint32_t target = UINT32_MAX;
                    if (execIdx + 1 < execCount)
                        target = ChipAddr(m_executionLog[execIdx + 1].addr);
                    if (target != UINT32_MAX) {
                        noteTarget(movedReg == 0x008A ? targets2 : targets1, target);
                        row.ptrNote = "jumps to";
                        row.ptrResolved = target;
                    }
                }
            }

            ++execIdx;
            continue;
        }

        const PointerWrite& pw = m_pointerWrites[ptrIdx++];

        // Track the pointer whoever wrote it, so a later strobe and the end-of-frame comparison
        // below both work off the real value.
        switch (pw.reg) {
            case 0x080:
                cop1 = (cop1 & 0x0000FFFFu) | (uint32_t(pw.value) << 16);
                cop1Have |= 1;
                ++lcWrites1;
                break;
            case 0x082:
                cop1 = (cop1 & 0xFFFF0000u) | (pw.value & 0xFFFEu);
                cop1Have |= 2;
                ++lcWrites1;
                break;
            case 0x084:
                cop2 = (cop2 & 0x0000FFFFu) | (uint32_t(pw.value) << 16);
                cop2Have |= 1;
                ++lcWrites2;
                break;
            case 0x086:
                cop2 = (cop2 & 0xFFFF0000u) | (pw.value & 0xFFFEu);
                cop2Have |= 2;
                ++lcWrites2;
                break;
            default:
                break;
        }

        const bool isStrobe = (pw.reg == 0x088 || pw.reg == 0x08A);
        const bool isCop2 = (pw.reg == 0x084 || pw.reg == 0x086 || pw.reg == 0x08A);
        const bool haveBoth = isCop2 ? (cop2Have == 3) : (cop1Have == 3);

        uint32_t resolved = UINT32_MAX;
        if (isStrobe) {
            ++strobeCount;
            // A CPU strobe leaves no row in the copper's own trace, so this is the only place the
            // switch is visible - every instruction row after it runs from the list it selected.
            activeList = isCop2 ? 2 : 1;
            // A strobe's real target is wherever the copper fetched next.
            if (execIdx < execCount)
                resolved = ChipAddr(m_executionLog[execIdx].addr);
            else if (haveBoth)
                resolved = ChipAddr(isCop2 ? cop2 : cop1);
            if (resolved != UINT32_MAX)
                noteTarget(isCop2 ? targets2 : targets1, resolved);
        } else {
            if (pw.fromCopper)
                ++copperLcWrites;
            else
                ++cpuLcWrites;
            if (haveBoth)
                resolved = ChipAddr(isCop2 ? cop2 : cop1);
        }

        const char* regName = CopperPointerRegName(pw.reg);
        const char* listName = isCop2 ? "COP2" : "COP1";

        // A write made by the copper itself already has a row - the MOVE that made it. Annotate
        // that row rather than duplicating it.
        if (pw.fromCopper) {
            int limit = std::max(0, static_cast<int>(m_formattedLog.size()) - 8);
            for (int k = static_cast<int>(m_formattedLog.size()) - 1; k >= limit; --k) {
                FormattedCopperRow& prev = m_formattedLog[k];
                if (prev.kind != RowKind::Instruction || ChipAddr(prev.addr) != ChipAddr(pw.pc))
                    continue;
                if (resolved != UINT32_MAX) {
                    prev.ptrNote = isStrobe ? "jumps to" : "list now";
                    prev.ptrResolved = resolved;
                }
                break;
            }
            continue;
        }

        m_formattedLog.emplace_back();
        FormattedCopperRow& row = m_formattedLog.back();
        const int rowIdx = static_cast<int>(m_formattedLog.size()) - 1;

        row.kind = RowKind::PointerWrite;
        row.execIdx = -1;
        row.lRefValue = pw.lRef;
        row.vline = pw.lRef / kCCKPerLine;
        row.hclk = pw.lRef % kCCKPerLine;
        snprintf(buf, sizeof(buf), "L%07X##ctr%d", pw.lRef, rowIdx);
        row.lRefLabel = buf;
        row.listStr = listName;
        row.numStr = "--";
        row.addr = pw.pc;
        snprintf(buf, sizeof(buf), "$%08X", pw.pc);
        row.addrStr = buf;
        snprintf(buf, sizeof(buf), "$%04X", pw.value);
        row.wordsStr = buf;
        row.instrName = regName;
        row.instrType = adm::core::CopperInstruction::Type::NOOP;
        row.ptrRegName = regName;
        row.ptrPc = pw.pc;
        row.ptrResolved = resolved;
        row.ptrIsStrobe = isStrobe;

        // The trailing space is deliberate: the resolved address is drawn after this text as a
        // hyperlink to the Memory window.
        if (isStrobe)
            row.details = resolved != UINT32_MAX
                              ? fmt::format("CPU strobe - copper restarts {} at ", listName)
                              : fmt::format("CPU strobe - copper restarts {}", listName);
        else
            row.details = resolved != UINT32_MAX
                              ? fmt::format("CPU write - {}LC now ", listName)
                              : fmt::format("CPU write - {} = ${:04X}", regName, pw.value);

        if (m_firstPointerRow < 0)
            m_firstPointerRow = rowIdx;
    }

    // The Live tab decodes from COP1LC/COP2LC as they stand at the end of the frame. Flag the
    // cases where that is not the list the copper actually ran.
    //
    // Every test is gated on an actual write to the pointer in question. Without a write the
    // register cannot have moved, so a mismatch there would only mean our COP1 seed was off -
    // which it can be, if the copper's vblank restart landed a cycle the far side of the frame
    // boundary and the window opens mid-list. Gating keeps that from crying wolf every frame.
    const uint32_t endCop1 = ChipAddr(cop1);
    const uint32_t endCop2 = ChipAddr(cop2);

    // Pointer moved during the frame, so the register no longer names the list that ran.
    const bool stale1 = lcWrites1 > 0 && cop1Have == 3 && endCop1 != seed1;
    const bool stale2 = cop2Have == 3 && !targets2.empty() && endCop2 != targets2.front();

    // Pointer moved and was then jumped to, so two different lists ran in the one frame.
    const bool switched1 =
        targets1.size() > 1 || (targets1.size() == 1 && lcWrites1 > 0 && targets1.front() != seed1);
    const bool switched2 = targets2.size() > 1;

    m_midFrameRewrite = stale1 || stale2 || switched1 || switched2;

    if (m_midFrameRewrite) {
        if (switched1 || switched2)
            m_midFrameSummary += "The copper was jumped to a different list part-way through the "
                                 "frame. ";
        if (stale1)
            m_midFrameSummary +=
                fmt::format("COP1 ran from ${:06X}, COP1LC now reads ${:06X}. ", seed1, endCop1);
        if (stale2)
            m_midFrameSummary += fmt::format("COP2 ran from ${:06X}, COP2LC now reads ${:06X}. ",
                                             targets2.front(), endCop2);
        m_midFrameSummary +=
            fmt::format("COPxLC writes this frame: {} from the CPU, {} from the copper; {} COPJMP "
                        "strobe(s). ",
                        cpuLcWrites, copperLcWrites, strobeCount);
        m_midFrameSummary += "Click to open the Trace tab, which shows what actually executed.";
    }

    if (m_selectedTraceIdx >= static_cast<int>(m_formattedLog.size()))
        m_selectedTraceIdx = std::max(0, static_cast<int>(m_formattedLog.size()) - 1);
}

void DebuggerCopperView::LoadRegisterDocs()
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

void DebuggerCopperView::DrawDocPanel()
{
    if (m_selectedDocName.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "Select a MOVE instruction to view register documentation";
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

void DebuggerCopperView::LoadCustomRegistersDef()
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

void DebuggerCopperView::LoadBitfieldDefs()
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

void DebuggerCopperView::DrawBitfieldTooltip(const std::string& regName, const std::string& docName,
                                             uint16_t value)
{
    LoadBitfieldDefs();

    // Match the decoder's lookup order: a register-name-specific entry first (BLTCON0/BLTCON1 share
    // a doc group but differ), else the shared doc_name entry.
    const nlohmann::json* def = nullptr;
    if (!regName.empty() && m_bitfieldDefs.contains(regName))
        def = &m_bitfieldDefs[regName];
    else if (!docName.empty() && m_bitfieldDefs.contains(docName))
        def = &m_bitfieldDefs[docName];
    if (!def)
        return;

    // Plain registers carry "fields"; variant registers (BLTCON0/1) carry "variants" — a copper
    // MOVE can't tell modes apart, so show the first variant's layout.
    const nlohmann::json* fields = nullptr;
    if (def->contains("fields") && (*def)["fields"].is_array()) {
        fields = &(*def)["fields"];
    } else if (def->contains("variants") && (*def)["variants"].is_array() &&
               !(*def)["variants"].empty()) {
        const auto& v0 = (*def)["variants"][0];
        if (v0.contains("fields") && v0["fields"].is_array())
            fields = &v0["fields"];
    }
    if (!fields || fields->empty())
        return;

    ImGui::BeginTooltip();
    ImGui::Text("%s = $%04X", regName.empty() ? docName.c_str() : regName.c_str(), value);
    ImGui::Separator();
    if (ImGui::BeginTable("##bftip", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Bit");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Field");
        ImGui::TableHeadersRow();
        for (const auto& jf : *fields) {
            int hi = jf.value("hi", 0);
            int lo = jf.value("lo", 0);
            std::string name = jf.value("name", std::string());
            int width = hi - lo + 1;
            if (width < 1 || width > 16)
                continue;
            uint32_t fmask = (width >= 16) ? 0xFFFFu : ((1u << width) - 1u);
            uint32_t fval = (static_cast<uint32_t>(value) >> lo) & fmask;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (width == 1)
                ImGui::Text("%d", hi);
            else
                ImGui::Text("%d-%d", hi, lo);

            ImGui::TableSetColumnIndex(1);
            char bin[17];
            for (int b = 0; b < width; ++b)
                bin[b] = ((fval >> (width - 1 - b)) & 1u) ? '1' : '0';
            bin[width] = '\0';
            ImVec4 col = fval ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            // Multi-bit fields also show the field's hex value (e.g. "101/$5"); a single bit is
            // already unambiguous as 0/1.
            if (width > 1)
                ImGui::TextColored(col, "%s/$%X", bin, fval);
            else
                ImGui::TextColored(col, "%s", bin);

            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(fval ? ImVec4(1, 1, 1, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s",
                               name.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndTooltip();
}

const std::vector<DebuggerCopperView::DecoderField>& DebuggerCopperView::ActiveDecoderFields() const
{
    static const std::vector<DecoderField> kEmpty;
    if (m_decoderMode >= 0 && m_decoderMode < static_cast<int>(m_decoderModeFields.size()))
        return m_decoderModeFields[m_decoderMode];
    return kEmpty;
}

void DebuggerCopperView::SetDecoderFields(const std::string& regName, const std::string& docName)
{
    LoadBitfieldDefs();
    m_decoderModes.clear();
    m_decoderModeFields.clear();
    m_decoderMode = 0;

    // Register-specific definitions take precedence because registers such as BLTCON0 and BLTCON1
    // share documentation but have different field layouts.
    const nlohmann::json* def = nullptr;
    if (!regName.empty() && m_bitfieldDefs.contains(regName))
        def = &m_bitfieldDefs[regName];
    else if (!docName.empty() && m_bitfieldDefs.contains(docName))
        def = &m_bitfieldDefs[docName];
    if (!def)
        return;

    auto parseFields = [](const nlohmann::json& fields) {
        std::vector<DecoderField> result;
        for (const auto& jsonField : fields) {
            DecoderField field;
            field.hi = jsonField.value("hi", 0);
            field.lo = jsonField.value("lo", 0);
            field.name = jsonField.value("name", std::string());
            field.desc = jsonField.value("desc", std::string());
            result.push_back(std::move(field));
        }
        return result;
    };

    if (def->contains("variants") && (*def)["variants"].is_array()) {
        for (const auto& variant : (*def)["variants"]) {
            m_decoderModes.push_back(variant.value("mode", std::string()));
            m_decoderModeFields.push_back(parseFields(
                variant.contains("fields") ? variant["fields"] : nlohmann::json::array()));
        }
    } else if (def->contains("fields") && (*def)["fields"].is_array()) {
        m_decoderModes.push_back(std::string());
        m_decoderModeFields.push_back(parseFields((*def)["fields"]));
    }
}

void DebuggerCopperView::UpdateDecoder(const std::string& docName, const std::string& regName,
                                       uint16_t value)
{
    if (regName != m_decoderRegName) {
        m_decoderDocName = docName;
        m_decoderRegName = regName;
        SetDecoderFields(regName, docName);
        m_decoderValue = value;
        m_lastSyncValue = value;
    } else if (m_decoderSync) {
        // Preserve calculator edits until the selected Copper instruction's source value changes.
        if (static_cast<int64_t>(value) != m_lastSyncValue)
            m_decoderValue = value;
        m_lastSyncValue = value;
    }
}

void DebuggerCopperView::SelectMoveInstruction(uint16_t word1, uint16_t word2, uint32_t addr,
                                               int traceIdx)
{
    const uint16_t regOffset = word1 & 0x01FE;
    char hex[16];
    snprintf(hex, sizeof(hex), "%03X", regOffset);

    std::string regName;
    std::string docName;
    if (m_customRegsDef.contains(hex)) {
        const auto& def = m_customRegsDef[hex];
        if (def.contains("name") && def["name"].is_string())
            regName = def["name"].get<std::string>();
        if (def.contains("doc_name") && def["doc_name"].is_string())
            docName = def["doc_name"].get<std::string>();
    }
    if (regName.empty())
        return;

    m_selectedRegisterName = regName;
    if (!docName.empty())
        m_selectedDocName = docName;
    if (traceIdx >= 0) {
        m_selectedDecoderTraceIdx = traceIdx;
        m_selectedLiveInstructionAddr = UINT32_MAX;
    } else {
        m_selectedLiveInstructionAddr = addr;
        m_selectedDecoderTraceIdx = -1;
    }
    UpdateDecoder(docName, regName, word2);
}

uint32_t DebuggerCopperView::DecoderFieldValue(const DecoderField& field) const
{
    const int width = field.hi - field.lo + 1;
    const uint32_t mask = (width >= 32) ? 0xFFFFFFFFu : (((1u << width) - 1u) << field.lo);
    return (m_decoderValue & mask) >> field.lo;
}

void DebuggerCopperView::SetDecoderFieldValue(const DecoderField& field, uint32_t value)
{
    const int width = field.hi - field.lo + 1;
    const uint32_t mask = (width >= 32) ? 0xFFFFFFFFu : (((1u << width) - 1u) << field.lo);
    m_decoderValue = (m_decoderValue & ~mask) | ((value << field.lo) & mask);
}

void DebuggerCopperView::DrawDecoderPanel()
{
    if (m_decoderRegName.empty()) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* message = "Select a MOVE instruction";
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
        ImGui::TextDisabled("%s", message);
        return;
    }

    ImGui::TextUnformatted(m_decoderRegName.c_str());
    ImGui::SameLine();
    char valueBuffer[16];
    snprintf(valueBuffer, sizeof(valueBuffer), "%04X", m_decoderValue & 0xFFFFu);
    ImGui::SetNextItemWidth(56.0f);
    if (ImGui::InputText("##copdecval", valueBuffer, sizeof(valueBuffer),
                         ImGuiInputTextFlags_CharsHexadecimal |
                             ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_AutoSelectAll)) {
        m_decoderValue = static_cast<uint32_t>(strtoul(valueBuffer, nullptr, 16)) & 0xFFFFu;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Sync", &m_decoderSync);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Follow the selected Copper MOVE's value.\nTurn off to freeze the value "
                          "and use the panel as a calculator.");
    }

    if (m_decoderModes.size() > 1) {
        for (int i = 0; i < static_cast<int>(m_decoderModes.size()); ++i) {
            if (i > 0)
                ImGui::SameLine();
            ImGui::RadioButton(m_decoderModes[i].c_str(), &m_decoderMode, i);
        }
    }

    ImGui::Separator();
    const std::vector<DecoderField>& fields = ActiveDecoderFields();
    if (fields.empty()) {
        ImGui::TextDisabled("No field breakdown available.");
        return;
    }

    static const ImVec4 kSet = {0.30f, 0.85f, 0.30f, 1.0f};
    static const ImVec4 kClear = {0.55f, 0.55f, 0.55f, 1.0f};
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable;
    PushFontRole(FontRole::Code);
    if (ImGui::BeginTable("##copdecfields", 4, flags)) {
        ImGui::TableSetupColumn("Bits", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Val", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("##info", ImGuiTableColumnFlags_WidthFixed, 18.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < fields.size(); ++i) {
            const DecoderField& field = fields[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            ImGui::TableSetColumnIndex(0);
            if (field.hi == field.lo)
                ImGui::Text("%d", field.hi);
            else
                ImGui::Text("%d-%d", field.hi, field.lo);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(field.name.c_str());

            ImGui::TableSetColumnIndex(2);
            if (field.hi == field.lo) {
                const bool set = (m_decoderValue >> field.lo) & 1u;
                ImGui::PushStyleColor(ImGuiCol_Text, set ? kSet : kClear);
                if (ImGui::Selectable(set ? "1" : "0", false, ImGuiSelectableFlags_None,
                                      ImVec2(16.0f, 0.0f))) {
                    m_decoderValue ^= (1u << field.lo);
                }
                ImGui::PopStyleColor();
            } else {
                const int width = field.hi - field.lo + 1;
                const int maxValue = (width >= 31) ? 0x7FFFFFFF : ((1 << width) - 1);
                int value = static_cast<int>(DecoderFieldValue(field));
                ImGui::SetNextItemWidth(36.0f);
                if (ImGui::DragInt("##v", &value, 0.1f, 0, maxValue, "%d"))
                    SetDecoderFieldValue(field, static_cast<uint32_t>(value));
            }

            ImGui::TableSetColumnIndex(3);
            if (!field.desc.empty()) {
                ImGui::TextDisabled("(i)");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
                    ImGui::TextUnformatted(field.desc.c_str());
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

void DebuggerCopperView::DrawDetailsCell(adm::core::CopperInstruction::Type type, uint16_t word1,
                                         uint16_t word2, const std::string& details, uint32_t addr,
                                         int traceIdx)
{
    if (type == adm::core::CopperInstruction::Type::MOVE) {
        uint16_t regOffset = word1 & 0x01FE;
        char hex[16];
        snprintf(hex, sizeof(hex), "%03X", regOffset);
        std::string regName;
        std::string docName;
        if (m_customRegsDef.contains(hex)) {
            auto& def = m_customRegsDef[hex];
            if (def.contains("name") && def["name"].is_string())
                regName = def["name"].get<std::string>();
            if (def.contains("doc_name") && def["doc_name"].is_string())
                docName = def["doc_name"].get<std::string>();
        }
        if (!regName.empty()) {
            ImGui::TextColored(kHyperlinkColor, "%s", regName.c_str());
            bool clicked = false;
            if (HoverableLink(&clicked)) {
                // Hover popup: break the MOVE's value down into named bit fields, when the register
                // has a bit-field descriptor.
                DrawBitfieldTooltip(regName, docName, word2);
            }
            if (clicked) {
                SelectMoveInstruction(word1, word2, addr, traceIdx);
                if (m_onJumpToCustomReg)
                    m_onJumpToCustomReg(regOffset, word2);
            }
            ImGui::SameLine(0, 0);
            ImGui::Text(" = $%04X", word2);
            return;
        }
    }
    ImGui::TextUnformatted(details.c_str());
}

void DebuggerCopperView::DrawCopperBreakpointGutter(uint32_t addr, int rowId)
{
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(rowId);
    const bool clicked = ImGui::InvisibleButton("##cbp", ImVec2(h, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    bool has = false;
    if (m_breakpointsView) {
        if (clicked)
            m_breakpointsView->ToggleCopperBreakpointAt(addr);
        has = m_breakpointsView->GetCopperBreakpointStatus(addr).first;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
    const float r = h * 0.32f;
    if (has)
        dl->AddCircleFilled(c, r, IM_COL32(230, 70, 70, 255));
    else if (hovered && m_breakpointsView)
        dl->AddCircle(c, r, IM_COL32(230, 70, 70, 150), 0, 1.5f);

    if (hovered && m_breakpointsView) {
        if (has)
            ImGui::SetTooltip("Copper breakpoint — click to remove");
        else
            ImGui::SetTooltip("Set Copper breakpoint at $%08X\n"
                              "(breaks when the Copper executes this instruction)",
                              addr);
    }
}

void DebuggerCopperView::DrawCopperAddressLink(uint32_t addr)
{
    if (!m_addrCtx || !m_addrCtx->valid()) {
        ImGui::Text("$%08X", addr);
        return;
    }
    char suffix[24];
    snprintf(suffix, sizeof(suffix), "cop_%X", addr);
    DrawAddressLink(suffix, addr & 0x00FFFFFFu, "$%08X", *m_addrCtx);
}

void DebuggerCopperView::DrawMidFrameBadge()
{
    // Only shown when it has something to say. The Live tab is unchanged and still decodes the
    // register as it stands; this is the hint that doing so is misleading for this frame.
    if (!m_midFrameRewrite)
        return;

    const ImVec4 warn(1.0f, 0.75f, 0.25f, 1.0f);

    ImGui::SameLine(0.0f, 20.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, warn);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(warn.x, warn.y, warn.z, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(warn.x, warn.y, warn.z, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(warn.x, warn.y, warn.z, 0.40f));
    const bool clicked = ImGui::Button(ICON_FA_TRIANGLE_EXCLAMATION " Registers Adjusted Midframe");
    ImGui::PopStyleColor(4);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(m_midFrameSummary.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    if (clicked) {
        m_mode = Mode::Trace;
        if (m_firstPointerRow >= 0) {
            m_selectedTraceIdx = m_firstPointerRow;
            m_traceScrollToSelected = true;
            if (m_onJumpToLRef)
                m_onJumpToLRef(m_formattedLog[m_firstPointerRow].lRefValue);
        }
    }
}

void DebuggerCopperView::DrawContent()
{
    LoadCustomRegistersDef();
    LoadRegisterDocs();

    bool liveActive = (m_mode == Mode::Live);
    bool traceActive = (m_mode == Mode::Trace);

    if (liveActive)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Live"))
        m_mode = Mode::Live;
    if (liveActive)
        ImGui::PopStyleColor();

    ImGui::SameLine();

    bool hasTrace = !m_formattedLog.empty();
    if (traceActive)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::BeginDisabled(!hasTrace);
    if (ImGui::Button("Trace"))
        m_mode = Mode::Trace;
    ImGui::EndDisabled();
    if (traceActive)
        ImGui::PopStyleColor();
    if (!hasTrace && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Run the emulator to capture copper trace data");

    if (traceActive && hasTrace) {
        int total = static_cast<int>(m_formattedLog.size());
        ImGui::SameLine(0, 16);
        ImGui::BeginDisabled(m_selectedTraceIdx <= 0);
        if (ImGui::Button(ICON_FA_ANGLES_LEFT " Prev")) {
            --m_selectedTraceIdx;
            m_traceScrollToSelected = true;
            if (m_onJumpToLRef)
                m_onJumpToLRef(m_formattedLog[m_selectedTraceIdx].lRefValue);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_selectedTraceIdx >= total - 1);
        if (ImGui::Button("Next " ICON_FA_ANGLES_RIGHT)) {
            ++m_selectedTraceIdx;
            m_traceScrollToSelected = true;
            if (m_onJumpToLRef)
                m_onJumpToLRef(m_formattedLog[m_selectedTraceIdx].lRefValue);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%d / %d", m_selectedTraceIdx + 1, total);
    }

    ImGui::Separator();

    ImGui::Checkbox("Show Documentation", &m_showCopperDocs);
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::Checkbox("Show Decoder", &m_showDecoder);
    DrawMidFrameBadge();
    ImGui::Spacing();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float splitterThickness = 4.0f;
    float topH = avail.y;
    float bottomH = 0.0f;

    if (m_showCopperDocs) {
        topH = avail.y * m_copperSplitRatio;
        bottomH = avail.y - topH - splitterThickness;
        if (topH < 50.0f)
            topH = 50.0f;
        if (bottomH < 50.0f)
            bottomH = 50.0f;
    }

    ImGui::BeginChild("##CopperTop", ImVec2(0, topH), false, ImGuiWindowFlags_None);
    const float decoderSplitterThickness = 4.0f;
    const float regionWidth = ImGui::GetContentRegionAvail().x;
    const float regionHeight = ImGui::GetContentRegionAvail().y;
    const bool showDecoderPane = m_showDecoder && regionWidth >= 304.0f;
    float listWidth = regionWidth;
    if (showDecoderPane) {
        listWidth = std::clamp(regionWidth * m_decoderSplitRatio, 150.0f,
                               regionWidth - 150.0f - decoderSplitterThickness);
    }

    ImGui::BeginChild("##CopperList", ImVec2(listWidth, regionHeight), false,
                      ImGuiWindowFlags_None);
    if (m_mode == Mode::Live)
        DrawLiveMode();
    else
        DrawTraceMode();
    ImGui::EndChild();

    if (showDecoderPane) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Button("##CopperDecoderSplitter", ImVec2(decoderSplitterThickness, regionHeight));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemActive()) {
            const float newListWidth = listWidth + ImGui::GetIO().MouseDelta.x;
            if (newListWidth >= 150.0f &&
                newListWidth <= regionWidth - 150.0f - decoderSplitterThickness) {
                m_decoderSplitRatio = newListWidth / regionWidth;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("##CopperDecoder", ImVec2(0, regionHeight), false, ImGuiWindowFlags_None);
        DrawDecoderPanel();
        ImGui::EndChild();
    }
    ImGui::EndChild();

    if (m_showCopperDocs) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Button("##CopperSplitter", ImVec2(-1, splitterThickness));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemActive()) {
            float delta = ImGui::GetIO().MouseDelta.y;
            float newTop = topH + delta;
            float newBottom = bottomH - delta;
            if (newTop >= 50.0f && newBottom >= 50.0f) {
                m_copperSplitRatio = newTop / avail.y;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

        ImGui::BeginChild("##CopperDocsBottom", ImVec2(0, bottomH), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        DrawDocPanel();
        ImGui::EndChild();
    }
}

void DebuggerCopperView::DrawLiveMode()
{
    LoadCustomRegistersDef();

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

    // COP1LC / COP2LC are word-swapped in the Custom snapshot; normalize to 24-bit Amiga addresses.
    uint32_t cop1Addr = regsValid ? adm::core::DecodeAmigaPtr(customRegs.cop1lc) : 0;
    uint32_t cop2Addr = regsValid ? adm::core::DecodeAmigaPtr(customRegs.cop2lc) : 0;

    // Refresh the region list only when the bank layout changes (same pattern as
    // DebuggerMemoryView/BitplanesView) instead of copying the vector every frame.
    if (regsValid) {
        const uint64_t regionsSig = m_emulatorView->GetMemoryRegionsSignature();
        if (regionsSig != m_regionsSig) {
            m_regions = m_emulatorView->GetFreshMemoryRegions();
            m_regionsSig = regionsSig;
        }
    }
    const auto& regions = m_regions;

    auto findRegion = [&](uint32_t addr) -> const adm::AmigaEmulator::MemoryRegion* {
        uint32_t masked = addr & 0x00FFFFFFu;
        for (const auto& r : regions) {
            if (masked >= r.amigaAddr && masked < r.amigaAddr + r.size)
                return &r;
        }
        return nullptr;
    };

    ImGui::BeginDisabled(!running);
    ImGui::Text("Copperlist Source:");
    ImGui::SameLine();
    ImGui::RadioButton("COP1LC", &m_copperListSource, 0);
    ImGui::SameLine();
    ImGui::RadioButton("COP2LC", &m_copperListSource, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Custom", &m_copperListSource, 2);

    uint32_t startAddr = 0;
    if (regsValid) {
        if (m_copperListSource == 0) {
            startAddr = cop1Addr;
        } else if (m_copperListSource == 1) {
            startAddr = cop2Addr;
        } else {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::InputText("##CopperAddr", m_copperCustomAddrStr, sizeof(m_copperCustomAddrStr),
                             ImGuiInputTextFlags_CharsHexadecimal);
            startAddr = strtoul(m_copperCustomAddrStr, nullptr, 16);
        }
        ImGui::Text("Start Address: $%08X", startAddr);
    } else {
        if (m_copperListSource == 2) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::InputText("##CopperAddr", m_copperCustomAddrStr, sizeof(m_copperCustomAddrStr),
                             ImGuiInputTextFlags_CharsHexadecimal);
        }
        ImGui::Text("Start Address: $--------");
    }
    ImGui::Separator();
    ImGui::EndDisabled();

    const adm::AmigaEmulator::MemoryRegion* activeRegion =
        regsValid ? findRegion(startAddr) : nullptr;
    bool hasValidRegion = activeRegion && activeRegion->hostPtr;

    if (regsValid && !hasValidRegion) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1),
                           "Address $%08X is not in a valid memory region.", startAddr);
        return;
    }

    // Pre-scan the copper list so the table can use ImGuiListClipper for virtual scrolling.
    // Cached per (start address, emulated frame): the scan disassembles up to 4096 instructions
    // (a JSON lookup + three strings each), which used to run every UI frame and defeat the
    // clipper below. Copper memory only changes at emulated-frame boundaries.
    const uint64_t liveFrame = m_emulatorView->GetEmulatorFrameCount();
    if (!hasValidRegion) {
        m_liveRows.clear();
        m_liveRowsAddr = ~0u;
    } else if (liveFrame != m_liveRowsFrame || startAddr != m_liveRowsAddr) {
        m_liveRowsFrame = liveFrame;
        m_liveRowsAddr = startAddr;
        m_liveRows.clear();
        uint32_t addr = startAddr & 0x00FFFFFFu;
        for (int i = 0; i < 4096; ++i) {
            if (addr + 3 >= activeRegion->amigaAddr + activeRegion->size)
                break;
            uint32_t offset = addr - activeRegion->amigaAddr;
            const uint8_t* ptr = activeRegion->hostPtr + offset;
            uint16_t w1 = (ptr[0] << 8) | ptr[1];
            uint16_t w2 = (ptr[2] << 8) | ptr[3];
            auto& row = m_liveRows.emplace_back();
            row.addr = addr;
            row.inst = adm::core::CopperDisassembler::Disassemble(w1, w2, m_customRegsDef);
            row.word1 = w1;
            addr += 4;
            bool isEnd = (row.inst.type == adm::core::CopperInstruction::Type::WAIT &&
                          row.inst.word1 == 0xFFFF && row.inst.word2 == 0xFFFE);
            bool isCopJmp =
                (row.inst.type == adm::core::CopperInstruction::Type::MOVE &&
                 ((row.inst.word1 & 0x1FE) == 0x0088 || (row.inst.word1 & 0x1FE) == 0x008A));
            if (isEnd || isCopJmp)
                break;
        }
    }
    const auto& liveRows = m_liveRows;

    ImGui::BeginDisabled(!regsValid);
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                            kAdjustableColumns;

    PushFontRole(FontRole::Code);
    if (ImGui::BeginTable("CopperTable", 5, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Words", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(liveRows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& row = liveRows[i];
                const auto& inst = row.inst;

                ImGui::TableNextRow();

                std::string docName;
                std::string regName;
                if (inst.type == adm::core::CopperInstruction::Type::MOVE) {
                    uint16_t regOffset = row.word1 & 0x01FE;
                    char hex[16];
                    snprintf(hex, sizeof(hex), "%03X", regOffset);
                    if (m_customRegsDef.contains(hex)) {
                        auto& def = m_customRegsDef[hex];
                        if (def.contains("name") && def["name"].is_string())
                            regName = def["name"].get<std::string>();
                        if (def.contains("doc_name") && def["doc_name"].is_string())
                            docName = def["doc_name"].get<std::string>();
                    }
                }

                if (row.addr == m_selectedLiveInstructionAddr && !regName.empty())
                    UpdateDecoder(docName, regName, inst.word2);

                bool isSelected = row.addr == m_selectedLiveInstructionAddr;
                bool inSelectedGroup =
                    !isSelected && !docName.empty() && docName == m_selectedDocName;
                if (inSelectedGroup)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 85, 75, 110));
                char selectableId[64];
                snprintf(selectableId, sizeof(selectableId), "##liverow_%X_%d", row.addr, i);

                ImGui::TableSetColumnIndex(0);
                const ImVec2 col0Pos = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable(selectableId, isSelected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    if (inst.type == adm::core::CopperInstruction::Type::MOVE)
                        SelectMoveInstruction(row.word1, inst.word2, row.addr, -1);
                }
                // Breakpoint gutter (left of the row number): toggles a Copper breakpoint on
                // this instruction's address. Drawn after the row Selectable (AllowOverlap) so
                // the gutter click takes priority over row selection.
                ImGui::SetCursorScreenPos(col0Pos);
                DrawCopperBreakpointGutter(row.addr, i);
                ImGui::SameLine();
                ImGui::Text("%d", i + 1);

                ImGui::TableSetColumnIndex(1);
                DrawCopperAddressLink(row.addr);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", inst.formattedWords.c_str());

                ImGui::TableSetColumnIndex(3);
                if (inst.type == adm::core::CopperInstruction::Type::MOVE) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1), "%s", inst.name.c_str());
                } else if (inst.type == adm::core::CopperInstruction::Type::WAIT) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1), "%s", inst.name.c_str());
                } else if (inst.type == adm::core::CopperInstruction::Type::SKIP) {
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1), "%s", inst.name.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", inst.name.c_str());
                }

                ImGui::TableSetColumnIndex(4);
                DrawDetailsCell(inst.type, row.word1, inst.word2, inst.details, row.addr);

                if (inst.isColor) {
                    ImGui::SameLine();
                    ImVec4 color(inst.r, inst.g, inst.b, 1.0f);
                    ImGui::ColorButton(
                        "##CopColor", color,
                        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker |
                            ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoAlpha,
                        ImVec2(16, 16));
                }
            }
        }
        clipper.End();

        ImGui::EndTable();
    }
    PopFontRole();
    ImGui::EndDisabled();
}

void DebuggerCopperView::DrawTraceMode()
{
    if (m_formattedLog.empty()) {
        if (m_onHoverLRef)
            m_onHoverLRef(UINT32_MAX);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "No copper trace — run the emulator to capture data.";
        ImVec2 textSize = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
        return;
    }

    // Compute scrubber row: last event with lRef <= m_scrubberLRef
    int scrubberRow = -1;
    if (m_scrubberLRef != UINT32_MAX) {
        for (int i = 0; i < static_cast<int>(m_formattedLog.size()); ++i) {
            if (m_formattedLog[i].lRefValue <= m_scrubberLRef)
                scrubberRow = i;
            else
                break;
        }
    }

    // Scroll-to when scrubber position changes
    bool scrollToScrubber = (scrubberRow != m_lastScrubberHighlightRowTrace);
    if (scrollToScrubber)
        m_lastScrubberHighlightRowTrace = scrubberRow;

    ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                 kAdjustableColumns;

    uint32_t hoveredLRef = UINT32_MAX;

    PushFontRole(FontRole::Code);
    if (ImGui::BeginTable("CopperTraceTable", 7, tableFlags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("L-Ref", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("List", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Words", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        int count = static_cast<int>(m_formattedLog.size());

        ImGuiListClipper clipper;
        clipper.Begin(count);
        if (scrubberRow >= 0 && scrollToScrubber)
            clipper.IncludeItemByIndex(scrubberRow);
        if (m_traceScrollToSelected)
            clipper.IncludeItemByIndex(m_selectedTraceIdx);

        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& row = m_formattedLog[i];
                bool isScrubberRow = (i == scrubberRow);
                bool isFuture = (scrubberRow >= 0 && i > scrubberRow);
                bool isSelected = (i == m_selectedTraceIdx);

                ImGui::TableNextRow();

                if (isFuture)
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);

                // Tint the synthetic pointer-write rows so they read as interruptions in the
                // copper's own instruction stream rather than as instructions themselves.
                if (row.kind == RowKind::PointerWrite)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 150, 40, 34));

                if (isScrubberRow) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 220, 60, 40));
                    if (scrollToScrubber) {
                        float rowY = ImGui::GetCursorScreenPos().y;
                        float rowH = ImGui::GetTextLineHeightWithSpacing();
                        float winY = ImGui::GetWindowPos().y;
                        float winH = ImGui::GetWindowSize().y;
                        float frozenH = ImGui::GetTextLineHeightWithSpacing();
                        float visH = std::max(winH - frozenH, rowH);
                        if (rowY < winY + frozenH)
                            ImGui::SetScrollHereY(std::clamp(4.0f * rowH / visH, 0.0f, 1.0f));
                        else if (rowY + rowH > winY + winH)
                            ImGui::SetScrollHereY(
                                std::clamp(1.0f - 5.0f * rowH / visH, 0.0f, 1.0f));
                    }
                }

                if (isSelected && m_traceScrollToSelected) {
                    float rowY = ImGui::GetCursorScreenPos().y;
                    float rowH = ImGui::GetTextLineHeightWithSpacing();
                    float winY = ImGui::GetWindowPos().y;
                    float winH = ImGui::GetWindowSize().y;
                    float frozenH = ImGui::GetTextLineHeightWithSpacing();
                    float visH = std::max(winH - frozenH, rowH);
                    if (rowY < winY + frozenH)
                        ImGui::SetScrollHereY(std::clamp(4.0f * rowH / visH, 0.0f, 1.0f));
                    else if (rowY + rowH > winY + winH)
                        ImGui::SetScrollHereY(std::clamp(1.0f - 5.0f * rowH / visH, 0.0f, 1.0f));
                    m_traceScrollToSelected = false;
                }

                // L-Ref column — Selectable spans all columns
                ImGui::TableSetColumnIndex(0);

                std::string docName;
                std::string regName;
                if (row.instrType == adm::core::CopperInstruction::Type::MOVE) {
                    uint16_t regOffset = row.word1 & 0x01FE;
                    char hex[16];
                    snprintf(hex, sizeof(hex), "%03X", regOffset);
                    if (m_customRegsDef.contains(hex)) {
                        auto& def = m_customRegsDef[hex];
                        if (def.contains("name") && def["name"].is_string())
                            regName = def["name"].get<std::string>();
                        if (def.contains("doc_name") && def["doc_name"].is_string())
                            docName = def["doc_name"].get<std::string>();
                    }
                }

                if (i == m_selectedDecoderTraceIdx && !regName.empty())
                    UpdateDecoder(docName, regName, row.word2);

                // Highlight the clicked row, and tint rows sharing the selected doc group.
                bool rowHighlighted =
                    isSelected || (!docName.empty() && docName == m_selectedDocName);
                if (ImGui::Selectable(row.lRefLabel.c_str(), rowHighlighted,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_DontClosePopups |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    m_selectedTraceIdx = i;
                    m_traceScrollToSelected = true;
                    if (m_onJumpToLRef)
                        m_onJumpToLRef(row.lRefValue);

                    if (row.instrType == adm::core::CopperInstruction::Type::MOVE)
                        SelectMoveInstruction(row.word1, row.word2, row.addr, i);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Line %u, Clock %u", row.vline, row.hclk);
                    hoveredLRef = row.lRefValue;
                }

                // List column
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(row.listStr.c_str());

                // # column
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(row.numStr.c_str());

                // Address column
                ImGui::TableSetColumnIndex(3);
                DrawCopperAddressLink(row.addr);

                // Words column
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(row.wordsStr.c_str());

                // Instruction column
                ImGui::TableSetColumnIndex(5);
                if (row.kind == RowKind::PointerWrite) {
                    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.30f, 1), "%s", row.instrName.c_str());
                } else if (row.instrType == adm::core::CopperInstruction::Type::MOVE) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1), "%s", row.instrName.c_str());
                } else if (row.instrType == adm::core::CopperInstruction::Type::WAIT) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1), "%s", row.instrName.c_str());
                } else if (row.instrType == adm::core::CopperInstruction::Type::SKIP) {
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1), "%s", row.instrName.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", row.instrName.c_str());
                }

                // Details column
                ImGui::TableSetColumnIndex(6);
                if (row.kind == RowKind::PointerWrite) {
                    ImGui::TextUnformatted(row.details.c_str());
                    if (row.ptrResolved != UINT32_MAX) {
                        ImGui::SameLine(0.0f, 0.0f);
                        DrawCopperAddressLink(row.ptrResolved);
                    }
                } else {
                    DrawDetailsCell(row.instrType, row.word1, row.word2, row.details, row.addr, i);
                    if (row.ptrResolved != UINT32_MAX && !row.ptrNote.empty()) {
                        ImGui::SameLine(0.0f, 0.0f);
                        ImGui::TextDisabled("   %s ", row.ptrNote.c_str());
                        ImGui::SameLine(0.0f, 0.0f);
                        DrawCopperAddressLink(row.ptrResolved);
                    }
                }

                if (row.isColor) {
                    ImGui::SameLine();
                    ImVec4 color(row.r, row.g, row.b, 1.0f);
                    ImGui::ColorButton(
                        "##CopTrColor", color,
                        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker |
                            ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoAlpha,
                        ImVec2(16, 16));
                }

                if (isFuture)
                    ImGui::PopStyleVar();
            }
        }
        clipper.End();

        ImGui::EndTable();
    }
    PopFontRole();

    if (m_onHoverLRef)
        m_onHoverLRef(hoveredLRef);
}

} // namespace adm::ui
