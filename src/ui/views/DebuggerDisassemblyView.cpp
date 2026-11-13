#include "DebuggerDisassemblyView.h"
#include <imgui.h>
#include <imgui_internal.h> // TablePushBackgroundChannel / TableGetHeaderRowHeight
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include "../../core/AmigaEmulator.h"
#include "../../core/AsmHighlighter.h"
#include "../../core/CpuProfiler.h"
#include "../../core/DebugLines.h"
#include "../../core/DebugSymbols.h"
#include "../../core/EditorLauncher.h"
#include "../../core/Logger.h"
#include "../../core/M68kCycles.h"
#include "../../core/MemoryAnnotations.h"
#include "../../core/UserPreferences.h"
#include "../ArColorPalette.h"
#include "../FontRoles.h"
#include "../IconsFontAwesome6.h"
#include "../RegionColors.h"
#include "../TableLayout.h"
#include "AddressMenuActions.h"
#include "BreakpointsView.h"
#include "EmulatorView.h"
#include "TurboSuppressedPlaceholder.h"
#include "imgui_md.h"

namespace adm::ui {

namespace {

// Language rail (see DrawLanguageRail): a stripe down the left of the Instruction
// column, plus the gap that keeps the row's text off it.
constexpr float kRailWidth = 4.0f;
constexpr float kRailPad = 8.0f;

struct DisassemblyDocRenderer : public imgui_md
{
    bool m_in_native_table = false;
    bool m_in_thead = false;
    int m_native_table_id = 0;
    // Declared column count of the current markdown table, and the column reached so
    // far in the current row. A malformed doc whose row has more cells than the
    // header declared would otherwise call TableNextColumn() past the last column,
    // walking off ImGui's column span (asserts p >= Data && p < DataEnd).
    int m_native_table_cols = 0;
    int m_native_table_col = 0;

    void BLOCK_DOC(bool e) override
    {
        if (e)
            m_native_table_id = 0;
        imgui_md::BLOCK_DOC(e);
    }

    void BLOCK_TABLE(const MD_BLOCK_TABLE_DETAIL* d, bool e) override
    {
        if (e) {
            ImGui::Spacing();
            int cols = d ? static_cast<int>(d->col_count) : 3;
            if (cols < 1)
                cols = 1;
            char id[32];
            snprintf(id, sizeof(id), "##disasm_mdtbl%d", m_native_table_id++);

            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;

            m_in_native_table = ImGui::BeginTable(id, cols, flags);
            m_native_table_cols = cols;
            m_native_table_col = 0;
        } else if (m_in_native_table) {
            ImGui::EndTable();
            m_in_native_table = false;
        }
    }

    void BLOCK_THEAD(bool e) override
    {
        m_is_table_header = e;
        m_in_thead = e;
        if (m_table_header_highlight) {
            if (e)
                ImGui::PushFont(get_font());
            else
                ImGui::PopFont();
        }
    }

    void BLOCK_TBODY(bool e) override
    {
        (void)e;
        if (m_in_native_table)
            m_is_table_header = false;
    }

    void BLOCK_TR(bool e) override
    {
        if (!m_in_native_table)
            return;
        if (e) {
            ImGui::TableNextRow();
            m_native_table_col = 0; // new row: reset the per-row column counter
            if (m_in_thead)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 60, 80, 128));
        }
    }

    // Advance to the next cell, but never past the table's declared column count —
    // md4c can hand us a row with more cells than the header declared.
    void NextTableCell()
    {
        if (m_in_native_table && m_native_table_col < m_native_table_cols) {
            ImGui::TableNextColumn();
            ++m_native_table_col;
        }
    }

    void BLOCK_TH(const MD_BLOCK_TD_DETAIL*, bool e) override
    {
        if (e)
            NextTableCell();
    }

    void BLOCK_TD(const MD_BLOCK_TD_DETAIL*, bool e) override
    {
        if (e)
            NextTableCell();
    }
};

} // namespace

DebuggerDisassemblyView::DebuggerDisassemblyView(EmulatorView* emulatorView,
                                                 BreakpointsView* breakpointsView,
                                                 core::MemoryAnnotations* annotations)
    : m_emulatorView(emulatorView), m_breakpointsView(breakpointsView), m_annotations(annotations),
      m_disassembler([emulatorView](uint32_t pc, char* mnemo, int mnemoSz, char* hex, int hexSz,
                                    uint32_t& nextPc) {
          auto* emu = emulatorView ? emulatorView->GetEmulator() : nullptr;
          if (!emu)
              return false;
          return emu->Disassemble(pc, mnemo, mnemoSz, hex, hexSz, nextPc);
      })
{
}

std::string DebuggerDisassemblyView::LibraryCallSuffix(
    const std::string& mnemonic, const adm::AmigaEmulator::CpuState* state,
    const std::vector<adm::AmigaEmulator::MemoryRegion>* regions)
{
    if (!m_showLibCalls)
        return {};
    if (!m_libAnnotatorLoaded) {
        m_libAnnotatorLoaded = true;
        m_libAnnotator.LoadTables();
    }
    if (!m_libAnnotator.Loaded())
        return {};

    // Strategy (c): resolve the target library from the live a6 + memory. Both
    // hooks are supplied only when state/regions are given (Live mode); otherwise
    // Annotate falls back to the exec default (strategy a).
    core::LibraryCallAnnotator::Context ctx;
    if (state) {
        ctx.readAReg = [state](int aReg, uint32_t& val) {
            if (aReg < 0 || aReg > 7)
                return false;
            val = state->a[aReg];
            return true;
        };
    }
    if (regions) {
        ctx.readByte = [regions](uint32_t addr, uint8_t& out) {
            for (const auto& r : *regions) {
                if (r.hostPtr && addr >= r.amigaAddr && addr < r.amigaAddr + r.size) {
                    out = r.hostPtr[addr - r.amigaAddr];
                    return true;
                }
            }
            return false;
        };
    }

    return m_libAnnotator.Annotate(mnemonic, ctx).value_or(std::string{});
}

void DebuggerDisassemblyView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void DebuggerDisassemblyView::RestoreProfile(bool open)
{
    m_profileOpen = open;
    m_profileNoFocus = open;
}

void DebuggerDisassemblyView::FireTraceSelected(bool active)
{
    if (!m_onTraceEntrySelected)
        return;
    static const adm::AmigaEmulator::CpuTraceEntry kEmpty{};
    if (!active || m_traceData.empty()) {
        m_onTraceEntrySelected(false, kEmpty);
    } else {
        int idx = std::clamp(m_selectedTraceIdx, 0, static_cast<int>(m_traceData.size()) - 1);
        m_onTraceEntrySelected(true, m_traceData[idx]);
    }
}

void DebuggerDisassemblyView::SetTraceData(const adm::AmigaEmulator::CpuTraceEntry* entries,
                                           int count)
{
    m_traceData = entries && count > 0 ? std::span<const adm::AmigaEmulator::CpuTraceEntry>(
                                             entries, static_cast<size_t>(count))
                                       : std::span<const adm::AmigaEmulator::CpuTraceEntry>{};
    // Keep selected index in range
    if (m_selectedTraceIdx >= count)
        m_selectedTraceIdx = count > 0 ? count - 1 : 0;
    m_profilerDirty = true;
    // Defer the expensive formatted-trace rebuild until something actually reads it
    // (Trace-mode draw or a scrubber tooltip query) — see EnsureFormattedTrace().
    m_formattedTraceDirty = true;
}

void DebuggerDisassemblyView::EnsureProfiler()
{
    if (!m_profilerDirty)
        return;
    m_profiler.Update(m_traceData.data(), static_cast<int>(m_traceData.size()));
    m_profilerDirty = false;
}

void DebuggerDisassemblyView::EnsureFormattedTrace()
{
    if (!m_formattedTraceDirty)
        return;
    m_formattedTraceDirty = false;
    RebuildFormattedTrace();
}

void DebuggerDisassemblyView::LoadInstructionDocs()
{
    if (m_instructionDocsLoaded)
        return;
    m_instructionDocsLoaded = true;
    try {
        std::ifstream f("assets/m68k_instruction_docs.json");
        if (!f.is_open()) {
            core::Logger::Warn(
                "DebuggerDisassemblyView: could not open assets/m68k_instruction_docs.json");
            return;
        }
        auto json = nlohmann::json::parse(f);
        if (json.is_object()) {
            for (auto& [key, val] : json.items()) {
                if (val.is_string())
                    m_instructionDocs[key] = val.get<std::string>();
            }
        }
    } catch (const std::exception& e) {
        core::Logger::Warn("DebuggerDisassemblyView: failed to load instruction docs: {}",
                           e.what());
    }
}

std::string
DebuggerDisassemblyView::InstructionDocKeyFromDisasm(const std::string& disasmText) const
{
    // Shared tokenizer yields the uppercased opcode without its size suffix; the doc
    // keys are lowercase, so fold it down (and drop a stray trailing ':' if present).
    std::string token = core::Disassembler::LeadingOpcode(disasmText).op;
    if (!token.empty() && token.back() == ':')
        token.pop_back();
    if (token.empty())
        return {};

    for (char& c : token)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto isOneOf = [&](std::initializer_list<const char*> names) {
        for (const char* n : names) {
            if (token == n)
                return true;
        }
        return false;
    };

    if (isOneOf({"divs", "divu"}))
        return "divs_divu";
    if (isOneOf({"muls", "mulu"}))
        return "muls_mulu";
    if (isOneOf({"asl", "asr"}))
        return "asl_asr";
    if (isOneOf({"lsl", "lsr"}))
        return "lsl_lsr";
    if (isOneOf({"rol", "ror"}))
        return "rol_ror";
    if (isOneOf({"roxl", "roxr"}))
        return "roxl_roxr";
    if (isOneOf({"bhs", "blo", "bhi", "bls", "bcc", "bcs", "bne", "beq", "bvc", "bvs", "bpl", "bmi",
                 "bge", "blt", "bgt", "ble"}))
        return "bcc";
    if (token.rfind("db", 0) == 0)
        return "dbcc";
    if (isOneOf({"scc", "scs", "seq", "sge", "sgt", "shi", "sle", "sls", "slt", "smi", "sne", "spl",
                 "svc", "svs", "sf", "st"}))
        return "scc";

    return token;
}

void DebuggerDisassemblyView::SelectInstructionDocFromDisasm(const std::string& disasmText)
{
    LoadInstructionDocs();
    const std::string key = InstructionDocKeyFromDisasm(disasmText);
    if (key.empty() || !m_instructionDocs.count(key))
        return;

    m_selectedInstructionDocKey = key;

    size_t start = disasmText.find_first_not_of(" \t");
    if (start == std::string::npos) {
        m_selectedInstructionName = key;
        return;
    }
    size_t end = disasmText.find_first_of(" \t", start);
    m_selectedInstructionName =
        disasmText.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

void DebuggerDisassemblyView::DrawDocPanel()
{
    LoadInstructionDocs();

    if (m_selectedInstructionDocKey.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "Select an instruction above to view documentation";
        ImVec2 textSize = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
        return;
    }

    auto it = m_instructionDocs.find(m_selectedInstructionDocKey);
    if (it == m_instructionDocs.end()) {
        ImGui::TextDisabled("No documentation found for '%s'", m_selectedInstructionDocKey.c_str());
        return;
    }

    const std::string& mdText = it->second;

    static DisassemblyDocRenderer renderer;
    renderer.print(mdText.c_str(), mdText.c_str() + mdText.size());
    ImGui::Dummy(ImVec2(0, 20.0f));
}

namespace {
// Control-flow class of a disassembled mnemonic, for the trace nesting gutter.
enum { CF_SEQ = 0, CF_BRANCH = 1, CF_CALL = 2, CF_RETURN = 3 };

int ClassifyControlFlow(const std::string& text)
{
    const std::string op = core::Disassembler::LeadingOpcode(text).op;
    if (op.empty())
        return CF_SEQ;

    if (op == "BSR" || op == "JSR")
        return CF_CALL;
    if (op == "RTS" || op == "RTE" || op == "RTR" || op == "RTD")
        return CF_RETURN;
    if (op == "JMP" || op == "BRA")
        return CF_BRANCH;

    // Bcc and DBcc / DBRA — match the condition-code suffix (shared table) so bit ops
    // (BTST/BSET/BCLR/BCHG) and the like aren't misread as branches.
    if (op.size() >= 2 && op[0] == 'B' && core::Disassembler::IsConditionCode(op.substr(1)))
        return CF_BRANCH;
    if (op.size() >= 3 && op[0] == 'D' && op[1] == 'B' &&
        (op.substr(2) == "RA" || core::Disassembler::IsConditionCode(op.substr(2))))
        return CF_BRANCH;
    return CF_SEQ;
}
} // namespace

// Defined below, next to the profile-mode helpers; declared here because the trace
// rebuild needs the same load base to de-relocate PCs for the DWARF lookup.
static uint32_t ResolveFuncBase(bool useManual, const char* manualBuf,
                                const std::vector<adm::core::AmigaSegment>& segs);

void DebuggerDisassemblyView::RebuildFormattedTrace()
{
    EnsureProfiler();
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    int count = static_cast<int>(m_traceData.size());
    m_formattedTrace.resize(count);

    const auto& deltas = m_profiler.GetDeltaCycles();

    // Disassembling each entry is the dominant cost here, and a trace is loop-heavy: the same
    // PC recurs thousands of times. Memory is fixed for the duration of this rebuild, so cache
    // the decode per PC and reuse it. The cache is local (rebuilt each call) so self-modifying
    // code between frames is handled correctly.
    std::unordered_map<uint32_t, core::Disassembler::Line> disasmCache;
    disasmCache.reserve(static_cast<size_t>(count) / 4 + 16);

    // Call-nesting depth, walked across the whole trace: BSR/JSR push, RTS/RTE/RTR
    // pop. The trace usually begins mid-execution, so we don't know the absolute
    // starting depth — depth is allowed to go negative here and is normalized below so
    // the shallowest point in the captured window becomes 0.
    int depth = 0;
    int minDepth = 0;
    m_traceMaxDepth = 0;

    for (int i = 0; i < count; ++i) {
        const auto& entry = m_traceData[i];
        FormattedTraceRow& row = m_formattedTrace[i];

        uint32_t lRef = uint32_t(entry.vpos) * 228u + uint32_t(entry.hpos);
        row.lRefValue = lRef;

        char buf[32];
        snprintf(buf, sizeof(buf), "L%07X", lRef);
        row.lRef = buf;

        snprintf(buf, sizeof(buf), "$%08X##tr%d", entry.pc, i);
        row.label = buf;

        row.deltaCycles = (i < static_cast<int>(deltas.size())) ? deltas[i] : 0u;

        // Fall-through address = pc + instruction length. Prefer the disassembler's
        // nextAddress (matches the shown mnemonic); raw_len is unreliable as a length.
        uint32_t fallThrough = entry.pc + entry.raw_len;
        const core::Disassembler::Line* line = nullptr;
        if (emu) {
            auto it = disasmCache.find(entry.pc);
            if (it != disasmCache.end()) {
                line = &it->second;
            } else {
                core::Disassembler::Line decoded;
                if (m_disassembler.DisassembleOne(entry.pc, decoded))
                    line = &disasmCache.emplace(entry.pc, std::move(decoded)).first->second;
            }
        }
        if (line) {
            row.hexStr = line->hex;
            row.mnemo = line->mnemonic;
            fallThrough = line->nextAddress;
        } else {
            char hexStr[128] = "";
            if (entry.raw_len >= 2) {
                int pos = 0;
                for (int b = 0; b < entry.raw_len && pos < 126; b += 2) {
                    uint16_t w = (static_cast<uint16_t>(entry.raw[b]) << 8) | entry.raw[b + 1];
                    pos += snprintf(hexStr + pos, sizeof(hexStr) - pos, "%04X ", w);
                }
            }
            row.hexStr = hexStr;
            row.mnemo = "???";
        }

        int kind = ClassifyControlFlow(row.mnemo);
        row.cfKind = (uint8_t)kind;
        if (kind == CF_RETURN)
            --depth;
        row.depth = depth; // raw (may be negative until normalized)
        minDepth = std::min(minDepth, depth);
        if (kind == CF_CALL)
            ++depth;

        // "diverged" = execution did not fall through to pc+len. Uses the actual next
        // executed PC (ground truth), so it marks every taken branch/jump/call/return/
        // exception, and never false-positives on an untaken Bcc.
        row.diverged = (i + 1 < count) && (m_traceData[i + 1].pc != fallThrough);

        // Textbook 68000 instruction cycles, decoded from the raw words. branchTaken
        // (= diverged) disambiguates taken vs not-taken conditional branches; the D
        // registers resolve dynamic shift counts.
        uint16_t words[5] = {0};
        const int nwords = std::min<int>(entry.raw_len / 2, 5);
        for (int wi = 0; wi < nwords; ++wi)
            words[wi] = (uint16_t)((entry.raw[wi * 2] << 8) | entry.raw[wi * 2 + 1]);
        row.nominalCycles = core::EstimateM68000Cycles(words, nwords, row.diverged, entry.d);
    }

    // Normalize so the shallowest point is depth 0, and cap for sanity.
    for (int i = 0; i < count; ++i) {
        int d = std::min(m_formattedTrace[i].depth - minDepth, 32);
        m_formattedTrace[i].depth = d;
        m_traceMaxDepth = std::max(m_traceMaxDepth, d);
    }

    // Interleaved source, one line per change. A trace is execution order, not address
    // order, so the "contiguous run since the last line" idea that Live mode uses is
    // meaningless here — a taken branch makes the gap between two entries arbitrary.
    // Capping the inflation at ~2 rows per entry also keeps the row heights close enough
    // for the ImGuiListClipper this table is drawn with.
    //
    // Gated on m_showSource: this is a ResolveLine plus a string compare per trace entry,
    // and the trace runs to tens of thousands of entries.
    if (m_showSource && m_debugLines && m_debugLines->IsLoaded()) {
        const uint32_t base = ResolveFuncBase(m_useManualBase, m_manualBaseBuf, m_segments);
        std::string prevFile;
        uint32_t prevLine = 0;
        for (int i = 0; i < count; ++i) {
            const uint32_t pc = m_traceData[i].pc;
            if (pc < base)
                continue;
            auto src = m_debugLines->ResolveLine(pc - base);
            if (!src || src->line == 0)
                continue;
            if (core::SourceFileCache::IsPseudoPath(src->file))
                continue;
            auto& row = m_formattedTrace[i];
            row.srcFileIdx = InternSourcePath(src->file);
            if (src->file != prevFile) {
                row.srcFileHeader = m_sourceFileHeaders;
                row.srcLine = src->line;
                prevFile = src->file;
                prevLine = src->line;
            } else if (src->line != prevLine) {
                row.srcLine = src->line;
                prevLine = src->line;
            }
        }
    }
}

void DebuggerDisassemblyView::ApplyProfileSort()
{
    // NOTE: must use (b < a) for descending, NOT (!less).
    // When two elements are equal, !less == true for both comp(a,b) and comp(b,a),
    // violating strict weak ordering and causing std::sort to crash or corrupt memory.
    std::sort(
        m_profileRows.begin(), m_profileRows.end(),
        [this](const adm::core::InstructionStats& a, const adm::core::InstructionStats& b) {
            bool asc = (m_profileSortDir == ImGuiSortDirection_Ascending);
            switch (m_profileSortCol) {
                case 1:
                    return asc ? a.count < b.count : b.count < a.count;
                case 2:
                    return asc ? a.totalCycles < b.totalCycles : b.totalCycles < a.totalCycles;
                case 3:
                    return asc ? a.AvgCycles() < b.AvgCycles() : b.AvgCycles() < a.AvgCycles();
                default:
                    return asc ? a.pc < b.pc : b.pc < a.pc;
            }
        });
}

void DebuggerDisassemblyView::RebuildProfileRows()
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    m_profileRows = m_profiler.GetSorted(); // copy list (initially by totalCycles desc)

    if (emu) {
        for (auto& s : m_profileRows) {
            core::Disassembler::Line line;
            s.mnemonic = m_disassembler.DisassembleOne(s.pc, line) ? line.mnemonic : "???";
        }
    }

    ApplyProfileSort(); // apply user's current sort preference
    m_profilerVersion = m_profiler.GetFrameVersion();
}

void DebuggerDisassemblyView::SetFunctionSymbols(const adm::core::DebugSymbols* syms,
                                                 const adm::core::DebugLines* lines,
                                                 std::vector<adm::core::AmigaSegment> segments)
{
    // Application calls this every frame. Only mark dependents dirty when an input
    // actually moved: otherwise the source mapping pass (a ResolveLine per cached
    // instruction) and, far worse, RebuildFormattedTrace over the whole CPU trace would
    // re-run at 60 Hz. The segment signature is exactly what ResolveFuncBase consumes.
    const uint64_t segSig =
        segments.empty() ? 0ull
                         : (static_cast<uint64_t>(segments.size()) << 32) | segments.front().base;
    const bool changed =
        (syms != m_debugSymbols) || (lines != m_debugLines) || (segSig != m_segSignature);

    m_debugSymbols = syms;
    m_debugLines = lines;
    m_segments = std::move(segments);
    m_segSignature = segSig;

    if (changed) {
        m_funcRowsDirty = true;
        m_sourceCache.SetElfPath(syms ? syms->SourcePath() : std::string{});
        m_sourceLex.clear();
        m_sourcePaths.clear();
        m_sourceMapDirty = true;
        m_formattedTraceDirty = true;
    }
}

void DebuggerDisassemblyView::SetUserPreferences(adm::core::UserPreferences* prefs)
{
    m_userPrefs = prefs;
    ApplySourcePrefs();
}

void DebuggerDisassemblyView::ApplySourcePrefs()
{
    if (!m_userPrefs)
        return;
    const auto& sp = m_userPrefs->Get().source;
    m_showSource = sp.showInDisassembly;
    m_sourceDetail = static_cast<SourceDetail>(std::clamp(sp.detail, 0, 2));
    m_sourceMaxGap = std::clamp(sp.maxGapLines, 1, 200);
    m_sourceFileHeaders = sp.showFileHeaders;
    m_sourceLineNumbers = sp.showLineNumbers;
    m_srcRootsEdit = sp.searchRoots;
    m_sourceCache.SetTabWidth(sp.tabWidth);
    m_sourceCache.SetSearchRoots(m_srcRootsEdit);
    m_sourceLex.clear();
    m_sourceMapDirty = true;
    m_formattedTraceDirty = true;
}

void DebuggerDisassemblyView::PersistSourceRoots()
{
    m_sourceCache.SetSearchRoots(m_srcRootsEdit);
    m_sourceLex.clear();
    m_sourceMapDirty = true;
    m_formattedTraceDirty = true;
    if (m_userPrefs)
        m_userPrefs->SetSourceSearchRoots(m_srcRootsEdit);
}

// Compact "file.c:NN" for an instruction PC, using the DWARF line info + load base.
// Returns empty when no line info is available or the address doesn't resolve.
static std::string SourceLineLabel(const adm::core::DebugLines* lines, uint32_t base, uint32_t pc)
{
    if (!lines || !lines->IsLoaded() || pc < base)
        return {};
    auto src = lines->ResolveLine(pc - base);
    if (!src)
        return {};
    // Show just the basename to keep the column narrow.
    size_t slash = src->file.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? src->file : src->file.substr(slash + 1);
    return name + ":" + std::to_string(src->line);
}

// The load base actually used to de-relocate PCs: the manual override when set and
// valid, otherwise the first code segment from the seglist. Returns 0 if unknown.
static uint32_t ResolveFuncBase(bool useManual, const char* manualBuf,
                                const std::vector<adm::core::AmigaSegment>& segs)
{
    if (useManual) {
        uint32_t base = 0;
        if (sscanf(manualBuf, "%x", &base) == 1) {
            return base;
        }
        return 0;
    }
    return segs.empty() ? 0 : segs.front().base;
}

void DebuggerDisassemblyView::ApplyFunctionSort()
{
    std::sort(
        m_funcRows.begin(), m_funcRows.end(),
        [this](const adm::core::FunctionStats& a, const adm::core::FunctionStats& b) {
            bool asc = (m_funcSortDir == ImGuiSortDirection_Ascending);
            switch (m_funcSortCol) {
                case 0:
                    return asc ? a.name < b.name : b.name < a.name;
                case 3:
                    return asc ? a.count < b.count : b.count < a.count;
                case 4:
                    return asc ? a.AvgCycles() < b.AvgCycles() : b.AvgCycles() < a.AvgCycles();
                default: // 1 (%frame) and 2 (Total cy) both order by totalCycles
                    return asc ? a.totalCycles < b.totalCycles : b.totalCycles < a.totalCycles;
            }
        });
}

void DebuggerDisassemblyView::RebuildFunctionRows()
{
    std::vector<adm::core::AmigaSegment> segs;
    if (m_useManualBase) {
        uint32_t base = ResolveFuncBase(true, m_manualBaseBuf, m_segments);
        if (base != 0) {
            // Assume a generous code span; the ELF's function ranges bound resolution.
            segs.push_back({base, 0x400000u});
        }
    } else {
        segs = m_segments;
    }

    if (m_debugSymbols) {
        m_funcProfiler.Update(m_profiler.GetSorted(), segs, *m_debugSymbols);
    } else {
        m_funcProfiler.Clear();
    }
    m_funcRows = m_funcProfiler.GetSorted();
    ApplyFunctionSort();
    m_funcProfilerVersion = m_profiler.GetFrameVersion();
    m_funcRowsDirty = false;
}

void DebuggerDisassemblyView::EnsureFunctionRollup()
{
    EnsureProfiler();
    if (m_funcRowsDirty || m_funcProfilerVersion != m_profiler.GetFrameVersion())
        RebuildFunctionRows();
}

bool DebuggerDisassemblyView::HasProfileData()
{
    EnsureProfiler();
    return !m_profiler.GetSorted().empty();
}

uint32_t DebuggerDisassemblyView::GetProfileTotalCycles()
{
    EnsureProfiler();
    uint32_t total = 0;
    for (const auto& s : m_profiler.GetSorted())
        total += s.totalCycles;
    return total;
}

void DebuggerDisassemblyView::GetProfileCycleSplit(uint32_t& resolved, uint32_t& unresolved)
{
    EnsureFunctionRollup();
    resolved = m_funcProfiler.GetResolvedCycles();
    unresolved = m_funcProfiler.GetUnresolvedCycles();
}

std::vector<adm::core::InstructionStats>
DebuggerDisassemblyView::GetProfileInstructions(int maxRows)
{
    EnsureProfiler();
    auto rows = m_profiler.GetSorted(); // already sorted by totalCycles desc
    if (maxRows > 0 && static_cast<int>(rows.size()) > maxRows)
        rows.resize(static_cast<size_t>(maxRows));
    if (m_emulatorView && m_emulatorView->GetEmulator()) {
        for (auto& s : rows) {
            core::Disassembler::Line line;
            s.mnemonic = m_disassembler.DisassembleOne(s.pc, line) ? line.mnemonic : "???";
        }
    }
    return rows;
}

std::vector<adm::core::FunctionStats> DebuggerDisassemblyView::GetProfileFunctions(int maxRows)
{
    EnsureFunctionRollup();
    auto rows = m_funcProfiler.GetSorted(); // sorted by totalCycles desc
    if (maxRows > 0 && static_cast<int>(rows.size()) > maxRows)
        rows.resize(static_cast<size_t>(maxRows));
    return rows;
}

void DebuggerDisassemblyView::DrawFunctionTable()
{
    EnsureFunctionRollup();

    const uint32_t res = m_funcProfiler.GetResolvedCycles();
    const uint32_t unres = m_funcProfiler.GetUnresolvedCycles();
    const uint32_t tot = res + unres;
    const float resPct = tot ? 100.0f * static_cast<float>(res) / static_cast<float>(tot) : 0.0f;

    if (m_funcRows.empty() && res == 0) {
        if (!m_segments.empty() || m_useManualBase) {
            ImGui::TextDisabled(
                "No sampled PCs fell inside the program's code segment this frame.");
        } else {
            ImGui::TextDisabled("Program base not found (no AmigaDOS process running). Tick "
                                "'Manual base' if the demo took over the machine.");
        }
    } else {
        ImGui::TextDisabled(
            "%zu functions  |  %.1f%% resolved (%u cy)  |  %.1f%% in ROM/library/gaps",
            m_funcRows.size(), resPct, res, 100.0f - resPct);
    }
    ImGui::Separator();

    const uint32_t base = ResolveFuncBase(m_useManualBase, m_manualBaseBuf, m_segments);

    ImGui::BeginChild("##func_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::BeginTable("##func_table", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingFixedFit | kAdjustableColumns |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("% frame",
                                ImGuiTableColumnFlags_WidthFixed |
                                    ImGuiTableColumnFlags_DefaultSort |
                                    ImGuiTableColumnFlags_PreferSortDescending,
                                64.0f);
        ImGui::TableSetupColumn("Total cy", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("\xc3\xb8 cy", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableHeadersRow();

        if (auto* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty && !m_funcRows.empty()) {
                if (specs->SpecsCount > 0) {
                    m_funcSortCol = specs->Specs[0].ColumnIndex;
                    m_funcSortDir = specs->Specs[0].SortDirection;
                }
                ApplyFunctionSort();
                specs->SpecsDirty = false;
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_funcRows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& f = m_funcRows[i];
                ImGui::TableNextRow();

                // Function name — click jumps the listing to its runtime address.
                ImGui::TableSetColumnIndex(0);
                char lbl[160];
                snprintf(lbl, sizeof(lbl), "%s##fn%d", f.name.c_str(), i);
                bool sel = false;
                if (ImGui::Selectable(lbl, &sel,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    if (base != 0) {
                        m_mode = Mode::Live;
                        NavigateToAddress(base + f.address, true);
                    }
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.1f%%", f.pctFrame * 100.0f);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", f.totalCycles);

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%u", f.count);

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.1f", f.AvgCycles());
            }
        }
        clipper.End();
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void DebuggerDisassemblyView::SetTracePositionLRef(uint32_t lRef)
{
    // Find the first trace entry at or after the given lRef
    for (int i = 0; i < static_cast<int>(m_traceData.size()); ++i) {
        uint32_t entryLRef = static_cast<uint32_t>(m_traceData[i].vpos) * 228u +
                             static_cast<uint32_t>(m_traceData[i].hpos);
        if (entryLRef >= lRef) {
            m_selectedTraceIdx = i;
            m_traceScrollToSelected = true;
            if (m_open)
                m_mode = Mode::Trace;
            FireTraceSelected(true);
            return;
        }
    }
    // If lRef is beyond all entries, snap to the last one
    if (!m_traceData.empty()) {
        m_selectedTraceIdx = static_cast<int>(m_traceData.size()) - 1;
        m_traceScrollToSelected = true;
        if (m_open)
            m_mode = Mode::Trace;
        FireTraceSelected(true);
    }
}

bool DebuggerDisassemblyView::GetTraceRowDisplay(int index, std::string& mnemo, int& nominalCycles,
                                                 uint32_t& deltaCycles, uint32_t& pc)
{
    EnsureFormattedTrace();
    if (index < 0 || index >= static_cast<int>(m_formattedTrace.size()) ||
        index >= static_cast<int>(m_traceData.size()))
        return false;
    const auto& r = m_formattedTrace[index];
    mnemo = r.mnemo;
    nominalCycles = r.nominalCycles;
    deltaCycles = r.deltaCycles;
    pc = m_traceData[index].pc;
    return true;
}

void DebuggerDisassemblyView::Draw()
{
    // Profile is a separate window fed by the same state, so either one being open is
    // reason enough to run — and to clear stale data when the machine stops.
    if (!m_open && !m_profileOpen)
        return;

    if (m_emulatorView && !m_emulatorView->IsRunning()) {
        Clear();
    }

    if (m_open) {
        ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_FirstUseEver);

        ImGuiWindowFlags flags = 0;
        if (m_noFocus) {
            flags |= ImGuiWindowFlags_NoFocusOnAppearing;
            m_noFocus = false;
        }

        if (ImGui::Begin(ICON_FA_CODE " Disassembly###Disassembly##debugger", &m_open, flags)) {
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

    DrawProfileWindow();
}

void DebuggerDisassemblyView::Clear()
{
    m_traceData = {};
    m_formattedTrace.clear();
    m_formattedTraceDirty = false;
    m_profileRows.clear();
    m_profiler.Clear();
    m_profilerDirty = false;
    m_mode = Mode::Live;
    m_traceSearchAddr[0] = '\0';
    m_traceSearchValue = 0;
    m_traceSearchActive = false;
    m_selectedInstructionDocKey.clear();
    m_selectedInstructionName.clear();
    m_highlightedAddr = 0xFFFFFFFF;
    m_sourcePaths.clear();
    m_sourceLex.clear();
    m_sourceBudgetHit = false;
    m_sourceMapDirty = true;
}

DebuggerDisassemblyView::DisasmInstruction
DebuggerDisassemblyView::MakeInstruction(uint32_t addr, core::Disassembler::Line& line)
{
    DisasmInstruction inst;
    inst.address = addr;
    inst.nextAddress = line.nextAddress;
    inst.sectionEnd = core::Disassembler::IsSectionEnd(line.mnemonic);
    inst.text = std::move(line.mnemonic);
    inst.hex = std::move(line.hex);
    return inst;
}

bool DebuggerDisassemblyView::DecodeAlignsTo(uint32_t start, uint32_t target, int maxIters) const
{
    uint32_t a = start;
    for (int it = 0; it < maxIters && a < target; ++it) {
        core::Disassembler::Line line;
        if (!m_disassembler.DisassembleOne(a, line) || line.nextAddress <= a)
            return false;
        a = line.nextAddress;
    }
    return a == target;
}

void DebuggerDisassemblyView::RebuildDisasmCache(uint32_t pc)
{
    m_disasmCache.clear();
    m_disasmPcIdx = -1;

    // Pick the farthest-back start (up to kLeadBytes before the PC) whose forward
    // decode lands exactly on the PC. This gives the PC real context above it, which a
    // backward jump (RTS, loop branch) needs in order to scroll up — a small fixed
    // lead-in would pin the PC near the top with nothing to reveal. m68k is
    // variable-length, so we probe for an aligned boundary rather than assume one.
    const uint32_t kLeadBytes = 0x80;
    uint32_t startAddr = pc;
    for (uint32_t lead = kLeadBytes & ~1u;; lead -= 2) {
        uint32_t s = (pc >= lead) ? (pc - lead) : 0u;
        if (DecodeAlignsTo(s, pc, 80)) {
            startAddr = s;
            break;
        }
        if (lead == 0) {
            startAddr = pc;
            break;
        }
    }

    auto fillCache = [this, pc](uint32_t addr) {
        for (int iter = 0; iter < 300 && m_disasmCache.size() < 250; ++iter) {
            core::Disassembler::Line line;
            if (!m_disassembler.DisassembleOne(addr, line))
                break;
            if (line.nextAddress <= addr)
                break;

            m_disasmCache.push_back(MakeInstruction(addr, line));

            if (addr == pc && m_disasmPcIdx == -1)
                m_disasmPcIdx = (int)m_disasmCache.size() - 1;

            addr = m_disasmCache.back().nextAddress;
        }
    };

    fillCache(startAddr);

    // Disassembling from before the PC may not land exactly on it (variable-length
    // instructions); retry starting at the PC itself so it's always in the cache.
    if (m_disasmPcIdx == -1 && !m_disasmCache.empty()) {
        m_disasmCache.clear();
        fillCache(pc);
    }

    m_disasmCachedPc = pc;
    m_disasmScrollToPc = true;
    m_pendingScrollAddRows = 0; // a fresh window invalidates any pending up-growth comp

    AnalyzeBranches();
    RefreshSourceMapping();
}

void DebuggerDisassemblyView::ExtendCacheUp(int chunk)
{
    if (m_disasmCache.empty())
        return;
    const uint32_t front = m_disasmCache.front().address;
    if (front == 0)
        return;

    // m68k is variable-length, so we can't just step back a fixed size: probe for a
    // start address that decodes forward and lands exactly on `front`. Scan from
    // ~chunk instructions back toward `front` and take the first aligned boundary
    // (the farthest-back clean lead-in within the probe window).
    const uint32_t wantBytes = (uint32_t)chunk * 4u; // ~chunk instrs at ~4 bytes avg
    const uint32_t base = (front >= wantBytes) ? (front - wantBytes) : 0u;
    uint32_t startAddr = front;
    for (uint32_t s = base & ~1u; s < front; s += 2)
        if (DecodeAlignsTo(s, front, 200)) {
            startAddr = s;
            break;
        }
    if (startAddr >= front)
        return; // no clean lead-in found in the probe window

    // Decode [startAddr, front) into a prefix and splice it on.
    std::vector<DisasmInstruction> prefix;
    for (uint32_t addr = startAddr; addr < front;) {
        core::Disassembler::Line line;
        if (!m_disassembler.DisassembleOne(addr, line) || line.nextAddress <= addr)
            break;
        DisasmInstruction inst = MakeInstruction(addr, line);
        addr = inst.nextAddress;
        prefix.push_back(std::move(inst));
    }
    if (prefix.empty() || prefix.back().nextAddress != front)
        return;

    const int count = (int)prefix.size();
    const int srcBefore = TotalSourceRows();
    m_disasmCache.insert(m_disasmCache.begin(), std::make_move_iterator(prefix.begin()),
                         std::make_move_iterator(prefix.end()));
    // The prepend shifts every cache index down by `count`.
    if (m_disasmPcIdx >= 0)
        m_disasmPcIdx += count;
    if (m_scrollToIdx >= 0)
        m_scrollToIdx += count;
    // The run continuity at the old first entry changes once instructions precede it,
    // so the mapping has to be redone before the row delta can be measured.
    AnalyzeBranches();
    RefreshSourceMapping();
    // Keep the view anchored: the new rows add content above the current scroll, so
    // compensate next frame (applied in DrawLiveMode). Interleaved source rows count
    // too — without the delta the listing jumps by their height on every up-growth.
    m_pendingScrollAddRows += count + (TotalSourceRows() - srcBefore);
}

void DebuggerDisassemblyView::ExtendCacheDown(int chunk)
{
    if (m_disasmCache.empty())
        return;
    uint32_t addr = m_disasmCache.back().nextAddress;
    int added = 0;
    while (added < chunk) {
        core::Disassembler::Line line;
        if (!m_disassembler.DisassembleOne(addr, line) || line.nextAddress <= addr)
            break;
        DisasmInstruction inst = MakeInstruction(addr, line);
        addr = inst.nextAddress;
        m_disasmCache.push_back(std::move(inst));
        ++added;
    }
    if (added > 0) {
        AnalyzeBranches(); // appended rows never shift existing indices, just re-link arrows
        RefreshSourceMapping();
    }
}

namespace {

// Parses the first "$hex" run in [s, s+len) as a 32-bit value (0 if none).
uint32_t ParseHexAfterDollar(const char* s, int len)
{
    for (int i = 0; i < len; ++i)
        if (s[i] == '$')
            return (uint32_t)strtoul(s + i + 1, nullptr, 16);
    return 0;
}

bool HasDollar(const char* s, int len)
{
    for (int i = 0; i < len; ++i)
        if (s[i] == '$')
            return true;
    return false;
}

// True if the leading mnemonic is JMP/JSR (absolute control-flow → follow in disasm).
bool OpcodeIsJmpJsr(const std::string& text)
{
    const std::string op = core::Disassembler::LeadingOpcode(text).op;
    return op == "JMP" || op == "JSR";
}

// True if the destination operand (after the last comma) is an address register
// (a0..a7 / sp). Used so a "move.l #$DFF180,a5" immediate becomes a memory link.
bool LastOperandIsAddrReg(const std::string& text)
{
    auto comma = text.find_last_of(',');
    if (comma == std::string::npos)
        return false;
    size_t i = comma + 1;
    while (i < text.size() && std::isspace((unsigned char)text[i]))
        ++i;
    if (i >= text.size())
        return false;
    char c0 = (char)std::tolower((unsigned char)text[i]);
    if (c0 == 'a' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '7')
        return true;
    if (c0 == 's' && i + 1 < text.size() && std::tolower((unsigned char)text[i + 1]) == 'p')
        return true;
    return false;
}

} // namespace

void DebuggerDisassemblyView::NavigateToAddress(uint32_t addr, bool recordHistory)
{
    if (recordHistory) {
        uint32_t cur = m_navOverride ? m_navAddr : m_prevPc;
        if (cur != 0xFFFFFFFF)
            m_navBack.push_back(cur);
        m_navFwd.clear();
    }

    m_navOverride = true;
    m_navAddr = addr;

    // Find addr in the current cache; rebuild around it if it isn't there.
    auto findIdx = [this](uint32_t a) -> int {
        for (int i = 0; i < (int)m_disasmCache.size(); ++i)
            if (m_disasmCache[i].address == a)
                return i;
        return -1;
    };
    int idx = findIdx(addr);
    if (idx < 0) {
        RebuildDisasmCache(addr);
        idx = findIdx(addr);
        // RebuildDisasmCache requests a PC-scroll; we scroll to the target instead.
        m_disasmScrollToPc = false;
        m_scrollPcIfHidden = false;
        m_scrollPcCenter = false;
    }
    m_scrollToIdx = idx;
    m_doScrollToIdx = (idx >= 0);
}

void DebuggerDisassemblyView::AnalyzeBranches()
{
    m_maxBranchLane = -1;

    // address -> cache index, for resolving branch targets to rows.
    std::unordered_map<uint32_t, int> addrToIdx;
    addrToIdx.reserve(m_disasmCache.size() * 2);
    for (int i = 0; i < (int)m_disasmCache.size(); ++i)
        addrToIdx[m_disasmCache[i].address] = i;

    // Identify branches whose target lands on a known instruction in the cache.
    struct Span
    {
        int idx, lo, hi; // cache index of the branch; [lo,hi] rows it spans
    };
    std::vector<Span> spans;
    for (int i = 0; i < (int)m_disasmCache.size(); ++i) {
        auto& inst = m_disasmCache[i];
        inst.isBranch = false;
        inst.targetIdx = -1;
        inst.branchLane = 0;
        uint32_t target = 0;
        bool conditional = false;
        if (!core::Disassembler::ParseRelativeBranch(inst.text, inst.address, target, conditional))
            continue;
        inst.isBranch = true;
        inst.branchConditional = conditional;
        inst.branchTarget = target;
        auto it = addrToIdx.find(target);
        if (it == addrToIdx.end())
            continue; // target outside the cache window — no arrow drawn
        inst.targetIdx = it->second;
        int lo = std::min(i, inst.targetIdx);
        int hi = std::max(i, inst.targetIdx);
        spans.push_back({i, lo, hi});
    }

    // Greedy lane assignment: innermost (shortest) spans get the lanes nearest the
    // instruction column so nested branches read like brackets without crossing.
    std::sort(spans.begin(), spans.end(),
              [](const Span& a, const Span& b) { return (a.hi - a.lo) < (b.hi - b.lo); });
    std::vector<std::vector<std::pair<int, int>>> laneRanges; // per-lane occupied [lo,hi]
    for (const auto& s : spans) {
        int lane = 0;
        for (;; ++lane) {
            if (lane >= (int)laneRanges.size()) {
                laneRanges.emplace_back();
                break;
            }
            bool overlap = false;
            for (const auto& r : laneRanges[lane])
                if (s.lo <= r.second && r.first <= s.hi) {
                    overlap = true;
                    break;
                }
            if (!overlap)
                break;
        }
        laneRanges[lane].push_back({s.lo, s.hi});
        m_disasmCache[s.idx].branchLane = lane;
        m_maxBranchLane = std::max(m_maxBranchLane, lane);
    }
}

void DebuggerDisassemblyView::DrawContent()
{
    // Mode toggle header
    bool liveActive = (m_mode == Mode::Live);
    bool traceActive = (m_mode == Mode::Trace);
    bool hasTrace = !m_traceData.empty();

    // Trace mode reads the formatted rows (both in the nav header below and DrawTraceMode);
    // rebuild them now if stale. Live mode doesn't touch them, so they stay deferred.
    if (traceActive)
        EnsureFormattedTrace();

    if (liveActive)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Live")) {
        m_mode = Mode::Live;
        FireTraceSelected(false);
    }
    if (liveActive)
        ImGui::PopStyleColor();

    ImGui::SameLine();

    if (traceActive)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::BeginDisabled(!hasTrace);
    if (ImGui::Button("Trace"))
        m_mode = Mode::Trace;
    ImGui::EndDisabled();
    if (traceActive)
        ImGui::PopStyleColor();
    if (!hasTrace && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Enable \"CPU Trace\" and run the emulator to capture trace data");

    // Profile is not a mode here at all: it is its own window, opened from the Window
    // menu like any other, so this toolbar carries nothing for it.

    // CPU Trace enable toggle — visible when running, so the user can arm it
    // without needing to open the CPU debugger window separately
    if (m_emulatorView && m_emulatorView->IsRunning()) {
        auto* emu = m_emulatorView->GetEmulator();
        bool traceEnabled = emu ? emu->IsCpuTraceEnabled() : false;
        ImGui::SameLine(0, 16);
        if (ImGui::Checkbox("CPU Trace", &traceEnabled) && emu)
            emu->SetCpuTraceEnabled(traceEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Record every M68K instruction per frame\n~14k-17k entries/frame on "
                              "A500 (~3.7 MB)");
    }

    if (traceActive && hasTrace) {
        ImGui::SameLine(0, 16);
        ImGui::BeginDisabled(m_selectedTraceIdx <= 0);
        if (ImGui::Button(ICON_FA_ANGLES_LEFT " Prev")) {
            --m_selectedTraceIdx;
            m_traceScrollToSelected = true;
            FireTraceSelected(true);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_selectedTraceIdx >= static_cast<int>(m_traceData.size()) - 1);
        if (ImGui::Button("Next " ICON_FA_ANGLES_RIGHT)) {
            ++m_selectedTraceIdx;
            m_traceScrollToSelected = true;
            FireTraceSelected(true);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%d / %d", m_selectedTraceIdx + 1,
                            static_cast<int>(m_traceData.size()));

        if (m_selectedTraceIdx >= 0 &&
            m_selectedTraceIdx < static_cast<int>(m_formattedTrace.size()))
            SelectInstructionDocFromDisasm(m_formattedTrace[m_selectedTraceIdx].mnemo);
    }

    ImGui::Separator();
    ImGui::Checkbox("Show Documentation", &m_showDocs);
    ImGui::SameLine(0, 16);
    ImGui::Checkbox("Library calls", &m_showLibCalls);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Annotate JSR/JMP (aN,-$xxx) library calls with the\n"
                          "resolved function name (e.g. exec/DoIO()).\n"
                          "Disable if the per-line memory lookup is too costly.");
    DrawSourceToolbar();
    // Drawn from DrawContent, not DrawLiveMode, so it is reachable in Trace mode too.
    DrawSourcePathsPopup();
    ImGui::Spacing();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float splitterThickness = 4.0f;
    float topH = avail.y;
    float bottomH = 0.0f;

    if (m_showDocs) {
        topH = avail.y * m_docsSplitRatio;
        bottomH = avail.y - topH - splitterThickness;
        if (topH < 50.0f)
            topH = 50.0f;
        if (bottomH < 50.0f)
            bottomH = 50.0f;
    }

    ImGui::BeginChild("##DisasmTop", ImVec2(0, topH), false, ImGuiWindowFlags_None);
    if (m_mode == Mode::Live)
        DrawLiveMode();
    else
        DrawTraceMode();
    ImGui::EndChild();

    if (m_showDocs) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Button("##DisasmSplitter", ImVec2(-1, splitterThickness));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemActive()) {
            float delta = ImGui::GetIO().MouseDelta.y;
            float newTop = topH + delta;
            float newBottom = bottomH - delta;
            if (newTop >= 50.0f && newBottom >= 50.0f)
                m_docsSplitRatio = newTop / avail.y;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

        ImGui::BeginChild("##DisasmBottom", ImVec2(0, bottomH), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        DrawDocPanel();
        ImGui::EndChild();
    }
}

void DebuggerDisassemblyView::DrawLiveMode()
{
    bool running = m_emulatorView && m_emulatorView->IsRunning();
    adm::AmigaEmulator::CpuState state{};
    bool stateValid = false;
    if (running) {
        if (!m_emulatorView->GetCpuState(state)) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            const char* msg = "Disassembly unavailable (core does not export adm_disasm_one)";
            ImVec2 textSize = ImGui::CalcTextSize(msg);
            ImGui::SetCursorPos(
                ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
            ImGui::TextDisabled("%s", msg);
            return;
        }
        stateValid = true;
    }

    // While the CPU is free-running (Play / TA / step-over) hold the last stopped
    // listing instead of chasing the live PC — at run speed that scrolls violently
    // and is unreadable. We anchor the highlight to the break point and snap back to
    // the live PC the moment execution stops (paused) again. Memory-visualisation
    // views (copper/sprites/etc.) stay live by design; only CPU inspection freezes.
    bool paused = m_emulatorView && m_emulatorView->IsPaused();
    bool freeze = running && !paused;

    uint32_t pc = stateValid ? state.pc : 0;
    if (freeze)
        pc = m_disasmCachedPc;

    // Execution just resumed via a sustained Play/continue (not a transient step-over,
    // and not a step which stays paused): discard any listing growth the user scrolled
    // up while paused and snap back to the compact rebuild window around the resume PC,
    // so the next stop shows a clean, small listing. m_prevPc holds the PC we stopped at.
    bool stepOverRun = m_emulatorView && m_emulatorView->IsStepOverRun();
    if (freeze && !m_prevFreeze && !stepOverRun) {
        uint32_t anchor = (m_prevPc != 0xFFFFFFFF) ? m_prevPc : m_disasmCachedPc;
        if (anchor != 0xFFFFFFFF) {
            RebuildDisasmCache(anchor);
            pc = anchor;
            m_navOverride = false;
        }
    }
    m_prevFreeze = freeze;

    // Memory snapshot used for the region bar and for library-call resolution
    // (strategy c). Fetched once per frame here rather than per row.
    std::vector<adm::AmigaEmulator::MemoryRegion> regions;
    if (stateValid)
        regions = m_emulatorView->GetFreshMemoryRegions();

    if (!stateValid)
        m_disasmCache.clear();

    if (stateValid && !freeze) {
        bool pcMoved = (pc != m_prevPc);
        // The live PC moving (a step / breakpoint hit) ends any address-link
        // navigation and resumes following the PC.
        if (pcMoved)
            m_navOverride = false;
    }
    if (stateValid && !freeze && m_navOverride) {
        // Frozen on a navigated address: keep m_prevPc current (so the back-history
        // anchor stays right) but don't re-snap the listing to the live PC.
        m_prevPc = pc;
    } else if (stateValid && !freeze) {
        bool cacheStale = m_disasmCache.empty();
        if (!cacheStale) {
            uint32_t front = m_disasmCache.front().address;
            uint32_t back = m_disasmCache.back().address;
            cacheStale = (pc < front || pc > back);
        }
        bool pcMoved = (pc != m_prevPc);
        if (pcMoved && m_prevPc != 0xFFFFFFFF)
            m_scrollPcForward = (pc > m_prevPc);

        if (cacheStale) {
            RebuildDisasmCache(pc); // sets m_disasmScrollToPc + m_disasmPcIdx
        } else if (pcMoved) {
            // PC moved within the cached range: re-find its row and request a lazy,
            // direction-aware scroll that only fires if the PC has left the viewport.
            int idx = -1;
            for (int k = 0; k < (int)m_disasmCache.size(); ++k)
                if (m_disasmCache[k].address == pc) {
                    idx = k;
                    break;
                }
            if (idx < 0) {
                RebuildDisasmCache(pc); // pc not on a decoded boundary — realign
            } else {
                m_disasmPcIdx = idx;
                m_scrollPcIfHidden = true;
            }
        }
        m_prevPc = pc;
    }

    // A debug break (breakpoint hit / FB bootblock trap) asks the listing to show where
    // execution stopped, overriding any active address-link navigation. Deferred to here
    // — the break fires mid-emulation; by now state.pc is the stopped PC.
    if (m_forceGoToPc && stateValid && !freeze) {
        m_forceGoToPc = false;
        m_navOverride = false;
        m_doScrollToIdx = false;
        if (m_disasmCache.empty() || pc != m_disasmCachedPc)
            RebuildDisasmCache(pc);
        m_disasmScrollToPc = true;
        m_scrollPcCenter = true; // center the PC, like the "Go to PC" button
    }

    // Back / forward through address-link navigation history.
    ImGui::BeginDisabled(!running || m_navBack.empty());
    if (ImGui::Button(ICON_FA_CHEVRON_LEFT "##navback")) {
        uint32_t target = m_navBack.back();
        m_navBack.pop_back();
        m_navFwd.push_back(m_navOverride ? m_navAddr : pc);
        NavigateToAddress(target, /*recordHistory=*/false);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back");
    ImGui::SameLine();
    ImGui::BeginDisabled(!running || m_navFwd.empty());
    if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "##navfwd")) {
        uint32_t target = m_navFwd.back();
        m_navFwd.pop_back();
        m_navBack.push_back(m_navOverride ? m_navAddr : pc);
        NavigateToAddress(target, /*recordHistory=*/false);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Forward");
    ImGui::SameLine();

    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Go to PC")) {
        m_navOverride = false; // resume following the live PC
        m_doScrollToIdx = false;
        if (pc != m_disasmCachedPc)
            RebuildDisasmCache(pc);
        m_disasmScrollToPc = true;
        m_scrollPcCenter = true; // explicit jump: center rather than direction edge
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (freeze)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), ICON_FA_PLAY " Running...");
    else if (stateValid)
        ImGui::TextDisabled("PC: $%08X", pc);
    else
        ImGui::TextDisabled("PC: $--------");

    // Manual address entry: type any hex address and press Enter to disassemble from
    // there, independent of the PC. Uses the same navigation override as link-following
    // so the listing stays put until the user steps or clicks "Go to PC".
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    ImGui::SetNextItemWidth(90.0f);
    auto navToTyped = [&]() {
        const char* s = m_gotoAddrBuf;
        while (*s == ' ' || *s == '$' || *s == '#')
            ++s; // tolerate a leading $ / # / spaces
        if (*s) {
            uint32_t addr = (uint32_t)strtoul(s, nullptr, 16);
            NavigateToAddress(addr);
        }
    };
    if (ImGui::InputTextWithHint("##gotoaddr", "goto $addr", m_gotoAddrBuf, sizeof(m_gotoAddrBuf),
                                 ImGuiInputTextFlags_CharsHexadecimal |
                                     ImGuiInputTextFlags_EnterReturnsTrue))
        navToTyped();
    ImGui::SameLine();
    if (ImGui::Button("Go##gotoaddr"))
        navToTyped();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Disassemble from a typed hex address");

    ImGui::Separator();

    const ImU32 amberBg = IM_COL32(180, 120, 0, 80);
    // User click-to-highlight row background (dark blue).
    const ImU32 highlightBg = IM_COL32(30, 55, 110, 200);

    // Thin colored bar showing PC's memory region
    if (stateValid) {
        ImU32 barColorU32 = IM_COL32(60, 60, 60, 180);
        for (int ri = 0; ri < (int)regions.size(); ri++) {
            const auto& mr = regions[ri];
            if (pc >= mr.amigaAddr && pc < mr.amigaAddr + mr.size) {
                barColorU32 = GetMemRegionColor(ri);
                break;
            }
        }
        ImVec4 barCol = ImGui::ColorConvertU32ToFloat4(barColorU32);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, barCol);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##disasmbar", ImVec2(4.0f, avail.y), false,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4);
    }

    PushFontRole(FontRole::Code);
    ImGui::BeginDisabled(!running);
    ImGui::BeginChild("##disasm_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::BeginTable("##disasm_table", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingFixedFit | kAdjustableColumns |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        const float rowH = ImGui::GetTextLineHeight() + 2.0f;
        // PushID bias for separator rows so their IDs never collide with the
        // instruction row at the same cache index (PushID(i)).
        const int kSepRowIdBias = 1 << 20;
        // Branch-arrow gutter: widens with the deepest nesting so arrows never collide.
        const float kLaneStep = 5.0f;
        const float gutterW = (m_maxBranchLane >= 0)
                                  ? std::min(80.0f, 9.0f + (float)(m_maxBranchLane + 1) * kLaneStep)
                                  : 9.0f;
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide,
                                rowH);
        ImGui::TableSetupColumn(
            "", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, gutterW);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // Compensate scroll for instructions just prepended by ExtendCacheUp: the new
        // rows pushed the previous content down, so add their height back to keep the
        // listing visually anchored where the user was reading.
        if (m_pendingScrollAddRows > 0) {
            ImGui::SetScrollY(ImGui::GetScrollY() + (float)m_pendingScrollAddRows * rowH);
            m_pendingScrollAddRows = 0;
        }

        const float circleR = rowH * 0.3f;
        const ImU32 colEnabled = IM_COL32(210, 50, 50, 255);
        const ImU32 colDisabled = IM_COL32(210, 50, 50, 180);

        // Section-end rules are drawn after EndTable (a cross-column line would be
        // clipped to the current cell mid-table); collect their Y positions here.
        std::vector<float> sepYs;

        // Branch arrows are drawn after the row loop on the window draw list (cell
        // clipping would chop a multi-row line). Capture the gutter's X span once and
        // the actual screen-Y of every visible row — the table's real row pitch differs
        // from GetTextLineHeight(), so we use measured positions (not an estimate) and
        // only extrapolate off-screen endpoints from the measured pitch.
        float gutterX0 = 0.0f, gutterX1 = 0.0f;
        bool gutterGeomSet = false;
        std::vector<float> rowY(m_disasmCache.size(), -1.0f); // center Y per cache index
        if (m_sourceMapDirty)
            RefreshSourceMapping();

        int firstVisIdx = -1, lastVisIdx = -1;
        float firstVisY = 0.0f, lastVisY = 0.0f;

        // Render every cached row directly — no ImGuiListClipper. Each cache entry can
        // emit a variable number of table rows (comment block, label, instruction,
        // section separator) of differing heights, which violates the clipper's
        // uniform-item-height assumption. That made the force-included PC row land at a
        // wrong extrapolated Y (so scroll-to-PC missed on branch/breakpoint rebuilds),
        // and when the probed row was tall it submitted only a handful of rows (the
        // "only the top 8 lines render" bug). The cache is bounded (<=250 instructions,
        // see fillCache) so submitting all rows every frame is cheap, and SetScrollHereY
        // now uses each row's true on-screen position.
        {
            for (int i = 0; i < (int)m_disasmCache.size(); ++i) {
                const auto& inst = m_disasmCache[i];
                bool isPC = (inst.address == pc);

                // Label line: render the user label (if any) on its own row just
                // above the instruction it names. Branch arrows key off the
                // instruction row (rowY[i]) so this extra row doesn't affect them.
                if (m_annotations) {
                    auto block = m_annotations->GetBlock(inst.address);
                    auto lbl = m_annotations->GetLabel(inst.address);
                    // A rule is drawn above the topmost pre-instruction row (block or
                    // label) when the preceding instruction didn't already terminate a
                    // section. A block carries its own "; ====" divider, so it needs no
                    // extra rule.
                    bool needRule = lbl && !block && (i == 0 || !m_disasmCache[i - 1].sectionEnd);

                    // Comment block: multi-line "; ..." header above the label, dim
                    // grey, filling the (stretch) Instruction column to the right edge.
                    if (block)
                        DrawCommentBlock(inst.address, *block);

                    if (lbl) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(2);
                        const ImVec2 lblPos = ImGui::GetCursorScreenPos();
                        if (needRule)
                            sepYs.push_back(lblPos.y);
                        // A spacer establishes the row height; the label text itself is painted on
                        // the window draw list under a clip rect spanning the Address column out to
                        // the window's right edge (like DrawCommentBlock) so long labels aren't
                        // clipped to the narrow Address cell.
                        ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
                        ImDrawList* lblDl = ImGui::GetWindowDrawList();
                        const float lblRightX =
                            ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                        const ImU32 lblColor =
                            ImGui::GetColorU32(ImVec4(0.95f, 0.85f, 0.45f, 1.0f));
                        const std::string lblText = *lbl + ":";
                        lblDl->PushClipRect(ImVec2(lblPos.x, lblDl->GetClipRectMin().y),
                                            ImVec2(lblRightX, lblDl->GetClipRectMax().y), true);
                        lblDl->AddText(lblPos, lblColor, lblText.c_str());
                        lblDl->PopClipRect();
                    }
                }

                // Interleaved source sits closest to the code it explains: comment
                // block, then label, then source, then the instruction. rowY[i] is still
                // measured from the instruction row below, so branch arrows are unaffected.
                if (m_showSource && inst.srcEmitLast != 0)
                    DrawSourceRows(inst, rowH);

                ImGui::TableNextRow();

                const bool isHighlighted = (inst.address == m_highlightedAddr);
                if (isHighlighted)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, highlightBg);
                else if (isPC)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, amberBg);

                ImGui::PushID(i);

                // --- Column 0: status circle ---
                ImGui::TableSetColumnIndex(0);
                const ImVec2 col0Pos = ImGui::GetCursorScreenPos();
                const float col0W = ImGui::GetContentRegionAvail().x;

                auto [hasBp, bpEnabled] = m_breakpointsView->GetBreakpointStatus(inst.address);

                if (ImGui::InvisibleButton("##bp_toggle", ImVec2(col0W, rowH))) {
                    m_breakpointsView->ToggleBreakpointAt(inst.address);
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    m_breakpointsView->RemoveBreakpointAt(inst.address);
                }
                if (hasBp && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(bpEnabled ? "Enabled" : "Disabled");
                }

                const ImVec2 center(col0Pos.x + col0W * 0.5f, col0Pos.y + rowH * 0.5f);
                if (hasBp) {
                    if (bpEnabled)
                        ImGui::GetWindowDrawList()->AddCircleFilled(center, circleR, colEnabled);
                    else
                        ImGui::GetWindowDrawList()->AddCircle(center, circleR, colDisabled, 0,
                                                              1.5f);
                }
                rowY[i] = center.y;
                if (firstVisIdx < 0) {
                    firstVisIdx = i;
                    firstVisY = center.y;
                }
                lastVisIdx = i;
                lastVisY = center.y;

                // --- Column 1: branch-arrow gutter (drawn after the loop) ---
                ImGui::TableSetColumnIndex(1);
                if (!gutterGeomSet) {
                    const ImVec2 gp = ImGui::GetCursorScreenPos();
                    gutterX0 = gp.x;
                    gutterX1 = gp.x + ImGui::GetContentRegionAvail().x;
                    gutterGeomSet = true;
                }

                // --- Column 2: address ---
                ImGui::TableSetColumnIndex(2);
                char label[32];
                snprintf(label, sizeof(label), "$%08X##disasm%d", inst.address, i);
                bool selected = false;
                ImGui::Selectable(label, &selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, 0));
                if (selected) {
                    SelectInstructionDocFromDisasm(inst.text);
                    // Toggle the persistent line highlight (only one line at a time).
                    m_highlightedAddr =
                        (m_highlightedAddr == inst.address) ? 0xFFFFFFFF : inst.address;
                }
                // Right-click on the line (away from an address operand) opens the
                // context menu targeting this instruction's address.
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    m_ctxLineAddr = inst.address;
                    m_ctxHasOperand = false;
                    m_openDisasmCtx = true;
                }
                if (isPC && m_selectedInstructionDocKey.empty())
                    SelectInstructionDocFromDisasm(inst.text);

                // --- Column 3: bytes ---
                ImGui::TableSetColumnIndex(3);
                ImGui::TextDisabled("%s", inst.hex.c_str());

                // --- Column 4: mnemonic (syntax-highlighted, one span per token) ---
                ImGui::TableSetColumnIndex(4);
                // Only when source is interleaved: in a pure disassembly listing every
                // row is the same language, so the rail would be decoration.
                if (m_showSource)
                    DrawLanguageRail(rowH, /*isSource=*/false);
                auto toks = core::TokenizeAsm(inst.text);
                const bool opIsJmpJsr = OpcodeIsJmpJsr(inst.text);
                const bool destAddrReg = LastOperandIsAddrReg(inst.text);
                for (size_t t = 0; t < toks.size(); ++t) {
                    if (t)
                        ImGui::SameLine(0, 0);
                    const auto& tk = toks[t];
                    const char* begin = inst.text.data() + tk.col;
                    const char* end = begin + tk.len;

                    // Decide whether this token is a clickable address link, what
                    // address it resolves to, and whether it is a control-flow target
                    // (follow in the listing) vs a data address (open the memory view).
                    bool hasLink = false;
                    uint32_t linkVal = 0;
                    bool linkCtrl = false;
                    if (inst.isBranch && tk.type == core::AsmTokenType::Immediate) {
                        // Relative branches print the displacement as an immediate; the
                        // absolute target is precomputed in AnalyzeBranches.
                        hasLink = true;
                        linkVal = inst.branchTarget;
                        linkCtrl = true;
                    } else if (tk.type == core::AsmTokenType::Address && HasDollar(begin, tk.len)) {
                        hasLink = true;
                        linkVal = ParseHexAfterDollar(begin, tk.len);
                        linkCtrl = opIsJmpJsr; // JMP/JSR $abs follow; data refs go to memory
                    } else if (tk.type == core::AsmTokenType::Immediate && destAddrReg &&
                               HasDollar(begin, tk.len)) {
                        // e.g. "move.l #$DFF180,a5" — the immediate is an address.
                        hasLink = true;
                        linkVal = ParseHexAfterDollar(begin, tk.len);
                    }

                    if (hasLink) {
                        const ImVec4 linkCol(0.45f, 0.78f, 1.0f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, linkCol);
                        ImGui::TextUnformatted(begin, end);
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            const ImVec2 mn = ImGui::GetItemRectMin();
                            const ImVec2 mx = ImGui::GetItemRectMax();
                            ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y),
                                                                ImVec2(mx.x, mx.y),
                                                                ImGui::GetColorU32(linkCol), 1.0f);
                        }
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                            if (linkCtrl) {
                                // Deferred: NavigateToAddress may rebuild the cache
                                // we're iterating. Handled after the table.
                                m_pendingNav = true;
                                m_pendingNavAddr = linkVal;
                            } else if (m_addrCtx && m_addrCtx->jumpToMemory) {
                                m_addrCtx->jumpToMemory(linkVal, 1);
                            }
                        }
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                            m_ctxLineAddr = inst.address;
                            m_ctxHasOperand = true;
                            m_ctxOperandAddr = linkVal;
                            m_openDisasmCtx = true;
                        }
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ResolveArColor(core::AsmTokenColor(tk.type)));
                        ImGui::TextUnformatted(begin, end);
                        ImGui::PopStyleColor();
                    }
                }
                std::string libSuffix = LibraryCallSuffix(inst.text, stateValid ? &state : nullptr,
                                                          stateValid ? &regions : nullptr);
                if (!libSuffix.empty()) {
                    ImGui::SameLine(0, 0);
                    ImGui::TextColored(ImVec4(0.55f, 0.70f, 0.95f, 1.0f), "%s", libSuffix.c_str());
                }
                if (m_annotations) {
                    std::string branchLbl =
                        m_annotations->BuildBranchLabelSuffix(inst.address, inst.text);
                    if (!branchLbl.empty()) {
                        ImGui::SameLine(0, 0);
                        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%s",
                                           branchLbl.c_str());
                    }
                    std::string suffix =
                        m_annotations->BuildAnnotationSuffix(inst.address, inst.text);
                    if (!suffix.empty()) {
                        ImGui::SameLine(0, 0);
                        ImGui::TextColored(ImVec4(0.45f, 0.75f, 0.45f, 1.0f), "%s", suffix.c_str());
                    }
                }

                if (isPC && (m_disasmScrollToPc || m_scrollPcIfHidden)) {
                    bool doScroll = m_disasmScrollToPc; // forced jumps always scroll
                    if (!doScroll) {
                        // Lazy: scroll only if the PC row has drifted near/out of view.
                        // Use the visible window frame (GetWindowHeight) for the bottom —
                        // ContentRegionMax.y is the full scrollable height, not the viewport.
                        const float y = col0Pos.y + rowH * 0.5f;
                        const float top = ImGui::GetWindowPos().y;
                        const float bot = top + ImGui::GetWindowHeight();
                        const float margin = rowH * 2.0f;
                        doScroll = (y < top + margin) || (y > bot - margin);
                    }
                    if (doScroll) {
                        // Direction-aware edge keeps the branch source on-screen when it
                        // fits; the explicit Go-to-PC button centers instead.
                        const float ratio = m_scrollPcCenter    ? 0.40f
                                            : m_scrollPcForward ? 0.75f
                                                                : 0.25f;
                        ImGui::SetScrollHereY(ratio);
                    }
                    m_disasmScrollToPc = false;
                    m_scrollPcIfHidden = false;
                    m_scrollPcCenter = false;
                }

                // Scroll to an address-link navigation target (independent of the PC).
                if (m_doScrollToIdx && i == m_scrollToIdx) {
                    ImGui::SetScrollHereY(0.4f);
                    m_doScrollToIdx = false;
                }

                ImGui::PopID();

                // Section separator: a dedicated row below each flow-terminating
                // instruction (RTS/JMP/...). It carries a "+" button on the left
                // that adds a label to the start of the next routine, and a rule
                // line is drawn across it after the table (see sepYs).
                if (inst.sectionEnd) {
                    ImGui::TableNextRow();
                    ImGui::PushID(i + kSepRowIdBias);
                    ImGui::TableSetColumnIndex(0);
                    const ImVec2 sepPos = ImGui::GetCursorScreenPos();
                    sepYs.push_back(sepPos.y);
                    if (m_annotations) {
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 1));
                        if (ImGui::SmallButton("+##addlbl")) {
                            m_labelTargetAddr = inst.nextAddress;
                            m_labelInputBuf[0] = '\0';
                            if (auto ex = m_annotations->GetLabel(inst.nextAddress))
                                std::snprintf(m_labelInputBuf, sizeof(m_labelInputBuf), "%s",
                                              ex->c_str());
                            m_openLabelPopup = true;
                        }
                        ImGui::PopStyleVar();
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Add label at $%08X", inst.nextAddress);
                    }
                    ImGui::PopID();
                }
            }
        }

        // Separator rules + branch arrows are stroked onto the table's BACKGROUND draw
        // channel (TablePushBackgroundChannel). That channel spans the whole table with
        // the host clip rect, so the strokes are not confined to — nor re-clipped to — an
        // individual column's draw channel the way they were when drawn into the current
        // cell channel. That column confinement is why, once the listing grew past the
        // viewport and the user scrolled, the gutter lines connecting nearby branches
        // vanished. The background channel renders beneath cell text, which is fine: the
        // gutter holds no text and the routine-break rules sit between rows. An explicit
        // clip (excluding the frozen header band) keeps off-screen rows from bleeding out.
        ImGui::TablePushBackgroundChannel();
        ImDrawList* decoDl = ImGui::GetWindowDrawList();
        const ImVec2 decoWinPos = ImGui::GetWindowPos();
        const ImVec2 decoClipMin(decoWinPos.x, decoWinPos.y + ImGui::TableGetHeaderRowHeight());
        const ImVec2 decoClipMax(decoWinPos.x + ImGui::GetWindowWidth(),
                                 decoWinPos.y + ImGui::GetWindowHeight());
        decoDl->PushClipRect(decoClipMin, decoClipMax, true);

        // Solid rule under each flow-terminating instruction (AR-style routine break).
        if (!sepYs.empty()) {
            const float x0 = decoWinPos.x + ImGui::GetWindowContentRegionMin().x;
            const float x1 = decoWinPos.x + ImGui::GetWindowContentRegionMax().x;
            const ImU32 sepCol = ResolveArColor(core::ArColor::Comment);
            for (float y : sepYs)
                decoDl->AddLine(ImVec2(x0, y), ImVec2(x1, y), sepCol, 1.0f);
        }

        // Branch-destination arrows: a vertical line in the gutter joins each
        // PC-relative branch to its in-cache target, with an arrowhead pointing into
        // the target row. Off-screen endpoints simply continue past the clip edge.
        if (gutterGeomSet && m_maxBranchLane >= 0) {
            const float rx = gutterX1 - 2.0f; // arrowhead tip / stub end (instruction side)
            const ImU32 colCond = IM_COL32(205, 150, 70, 190);   // Bcc / DBcc
            const ImU32 colUncond = IM_COL32(95, 160, 230, 210); // BRA / BSR
            const ImU32 colPc = IM_COL32(255, 210, 90, 255);     // branch at the PC
            const ImU32 colHi = IM_COL32(130, 200, 255, 255);    // connected to highlighted line

            // Measured row pitch (falls back to the estimate if only one row is visible).
            const float pitch = (lastVisIdx > firstVisIdx)
                                    ? (lastVisY - firstVisY) / (float)(lastVisIdx - firstVisIdx)
                                    : rowH;
            auto yForRow = [&](int idx) -> float {
                if (idx >= 0 && idx < (int)rowY.size() && rowY[idx] >= 0.0f)
                    return rowY[idx];                                  // exact, measured position
                return firstVisY + (float)(idx - firstVisIdx) * pitch; // off-screen estimate
            };

            for (int i = 0; i < (int)m_disasmCache.size(); ++i) {
                const auto& inst = m_disasmCache[i];
                if (!inst.isBranch || inst.targetIdx < 0)
                    continue;

                const float srcY = yForRow(i);
                const float dstY = yForRow(inst.targetIdx);
                const float vx = rx - 4.0f - (float)inst.branchLane * kLaneStep;

                // An arrow is emphasised when the highlighted line is either its branch
                // source or its branch destination.
                const bool hiConnected =
                    m_highlightedAddr != 0xFFFFFFFF &&
                    (inst.address == m_highlightedAddr ||
                     m_disasmCache[inst.targetIdx].address == m_highlightedAddr);

                const bool atPc = (inst.address == pc);
                const ImU32 col = hiConnected              ? colHi
                                  : atPc                   ? colPc
                                  : inst.branchConditional ? colCond
                                                           : colUncond;
                const float thick = (hiConnected || atPc) ? 2.0f : 1.0f;

                decoDl->AddLine(ImVec2(vx, srcY), ImVec2(vx, dstY), col, thick); // vertical run
                decoDl->AddLine(ImVec2(vx, srcY), ImVec2(rx, srcY), col, thick); // source tick
                decoDl->AddLine(ImVec2(vx, dstY), ImVec2(rx - 3.0f, dstY), col, thick); // tgt stub
                // Arrowhead pointing right into the destination instruction.
                decoDl->AddTriangleFilled(ImVec2(rx, dstY), ImVec2(rx - 5.0f, dstY - 3.0f),
                                          ImVec2(rx - 5.0f, dstY + 3.0f), col);
            }
        }

        decoDl->PopClipRect();
        ImGui::TablePopBackgroundChannel();

        // Grow the decoded listing when the user scrolls to an edge, so they can browse
        // past the fixed rebuild window without re-centring on the PC. Only while paused
        // (the growth is discarded on resume) and unbounded by design. Triggered only on
        // an actual scroll this frame (not while merely parked at an edge — e.g. the PC
        // sitting at scroll 0) and only when the listing is genuinely scrollable.
        if (paused && !m_disasmCache.empty()) {
            const float sy = ImGui::GetScrollY();
            const float smax = ImGui::GetScrollMaxY();
            const float edge = rowH * 3.0f;
            const bool scrolled = (m_lastScrollY >= 0.0f) && std::fabs(sy - m_lastScrollY) > 0.5f;
            // Suppress while a programmatic scroll is in flight (PC follow / nav jump).
            const bool progScroll = m_disasmScrollToPc || m_scrollPcIfHidden || m_doScrollToIdx;
            if (scrolled && !progScroll && smax > edge) {
                if (sy <= edge)
                    ExtendCacheUp(kExtendChunk);
                else if (sy >= smax - edge)
                    ExtendCacheDown(kExtendChunk);
            }
            m_lastScrollY = sy;
        } else {
            m_lastScrollY = -1.0f; // reset so a fresh listing doesn't read a stale delta
        }

        ImGui::EndTable();
    }
    PopFontRole();
    ImGui::EndChild();
    ImGui::EndDisabled();

    // Process a deferred in-listing follow now that the cache is no longer iterated.
    if (m_pendingNav) {
        m_pendingNav = false;
        NavigateToAddress(m_pendingNavAddr);
    }

    DrawDisasmContextMenu();
    DrawSourceContextMenu();

    // Quick add-label popup (opened from a section-separator "+" button). Editing
    // an empty label clears it via SetLabel's empty-string handling.
    if (m_openLabelPopup) {
        ImGui::OpenPopup("##disasm_add_label");
        m_openLabelPopup = false;
    }
    if (ImGui::BeginPopup("##disasm_add_label")) {
        ImGui::Text("Label for $%08X", m_labelTargetAddr);
        ImGui::SetNextItemWidth(220);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        bool commit = ImGui::InputText("##lbl", m_labelInputBuf, sizeof(m_labelInputBuf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Save") || commit) && m_annotations) {
            m_annotations->SetLabel(m_labelTargetAddr, m_labelInputBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Multi-line comment-block editor modal.
    if (m_openBlockEdit) {
        ImGui::OpenPopup("##disasm_block");
        m_openBlockEdit = false;
    }
    if (ImGui::BeginPopup("##disasm_block")) {
        ImGui::Text("Comment block for $%08X", m_ctxLineAddr);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextMultiline("##blk", m_blockBuf, sizeof(m_blockBuf), ImVec2(420, 160));
        if (ImGui::Button("Save") && m_annotations) {
            m_annotations->SetBlock(m_ctxLineAddr, m_blockBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void DebuggerDisassemblyView::DrawCommentBlock(uint32_t addr, const std::string& block, int addrCol)
{
    // Each line is its own table row, drawn from the Address column out to the
    // window's right edge. A full-width Selectable per line provides the hover
    // highlight and click capture (double-click edits, right-click opens the
    // context menu); the text itself is painted on the window draw list (like the
    // section-separator rules) under a clip rect that spans Address→right-edge, so
    // it isn't clipped to the narrow Address cell. Dim grey, matching the comment
    // colour (customisable via Asm.Comment).
    const ImU32 grey = ResolveArColor(core::ArColor::Comment);
    const float eqW = ImGui::CalcTextSize("=").x;
    const float lineH = ImGui::GetTextLineHeight();
    const float rightX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Open the first row to measure the Address column's left edge, then size the
    // "; ====" dividers to fill from there to the right edge and render every line.
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(addrCol);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    // The text is stroked manually (below) so it can span from the Address column out
    // to the right edge instead of being clipped to the narrow cell. Capture the
    // current cell's vertical clip bounds — they already exclude the frozen header and
    // the scrolled-off area — so off-screen rows can be clamped to them and not bleed
    // out of the window, while the wide horizontal span is preserved.
    const float bodyClipTop = dl->GetClipRectMin().y;
    const float bodyClipBot = dl->GetClipRectMax().y;
    int cols = (eqW > 0.0f) ? (int)((rightX - pos.x) / eqW) : 16;
    if (cols < 8)
        cols = 8;
    const auto lines = core::MemoryAnnotations::BuildCommentBlockLines(block, cols);

    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(addrCol);
            pos = ImGui::GetCursorScreenPos();
        }

        char id[40];
        std::snprintf(id, sizeof(id), "##blk_%08X_%zu", addr, i);
        ImGui::Selectable(id, false,
                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                              ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(0, lineH));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_ctxLineAddr = addr;
            std::snprintf(m_blockBuf, sizeof(m_blockBuf), "%s", block.c_str());
            m_openBlockEdit = true;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            m_ctxLineAddr = addr;
            m_ctxHasOperand = false;
            m_openDisasmCtx = true;
        }

        // Span X from the Address column to the right edge (wide), but clamp Y to the
        // visible table body so a scrolled-off row's text can't bleed out of the window.
        const float top = std::max(pos.y, bodyClipTop);
        const float bot = std::min(pos.y + lineH, bodyClipBot);
        if (bot > top) {
            dl->PushClipRect(ImVec2(pos.x, top), ImVec2(rightX, bot), false);
            dl->AddText(pos, grey, lines[i].c_str());
            dl->PopClipRect();
        }
    }
}

// =====================================================================================
// Interleaved C/C++ source
// =====================================================================================

int DebuggerDisassemblyView::InternSourcePath(const std::string& path)
{
    for (size_t i = 0; i < m_sourcePaths.size(); ++i)
        if (m_sourcePaths[i] == path)
            return static_cast<int>(i);
    m_sourcePaths.push_back(path);
    return static_cast<int>(m_sourcePaths.size()) - 1;
}

const DebuggerDisassemblyView::SourceLexCache&
DebuggerDisassemblyView::LexStatesFor(const core::SourceFile& file)
{
    SourceLexCache& entry = m_sourceLex[file.dwarfPath];
    if (entry.fileVersion == file.version && !entry.states.empty())
        return entry;

    // One O(file) pass buys O(line) tokenizing for any single line thereafter, which is
    // what makes drawing a window into the middle of a file affordable.
    entry.fileVersion = file.version;
    entry.states = core::ScanCLineStates(file.lines);
    entry.commentOrBlank.assign(file.lines.size(), false);
    for (size_t i = 0; i < file.lines.size(); ++i)
        entry.commentOrBlank[i] = core::IsCommentOrBlankLine(file.lines[i], entry.states[i]);
    return entry;
}

void DebuggerDisassemblyView::RefreshSourceMapping()
{
    m_sourceMapDirty = false;
    m_sourceBudgetHit = false;
    for (auto& inst : m_disasmCache) {
        inst.srcFileIdx = -1;
        inst.srcLine = 0;
        inst.srcEmitFirst = 0;
        inst.srcEmitLast = 0;
        inst.srcFileHeader = false;
        inst.srcTruncated = false;
    }
    // m_sourcePaths is deliberately NOT cleared here: it is append-only for the lifetime
    // of one set of debug symbols, and Trace mode's rows index into the same vector.
    if (!m_showSource || !m_debugLines || !m_debugLines->IsLoaded() || m_disasmCache.empty())
        return;

    const uint32_t base = ResolveFuncBase(m_useManualBase, m_manualBaseBuf, m_segments);
    const bool codeOnly = (m_sourceDetail == SourceDetail::CodeOnly);
    const bool fill = (m_sourceDetail != SourceDetail::StatementOnly);

    std::string prevFile;
    uint32_t prevLine = 0;
    int emitted = 0;

    for (auto& inst : m_disasmCache) {
        if (inst.address < base)
            continue;
        auto src = m_debugLines->ResolveLine(inst.address - base);
        if (!src || src->line == 0)
            continue;
        // <artificial>/<built-in> name compiler-generated code, not a file. Emitting a
        // "source not found" row for them would be noise about something that never existed.
        if (core::SourceFileCache::IsPseudoPath(src->file))
            continue;

        inst.srcFileIdx = InternSourcePath(src->file);
        inst.srcLine = src->line;

        if (emitted >= kMaxSourceRowsPerListing) {
            m_sourceBudgetHit = true;
            continue;
        }

        if (src->file != prevFile) {
            // A new file: no gap fill, we have no idea what preceded it here.
            inst.srcFileHeader = m_sourceFileHeaders;
            inst.srcEmitFirst = inst.srcEmitLast = src->line;
            emitted += m_sourceFileHeaders ? 2 : 1;
            prevFile = src->file;
            prevLine = src->line;
            continue;
        }
        if (src->line == prevLine)
            continue; // same statement across several instructions — show it once
        if (src->line < prevLine || !fill) {
            // Backwards (a loop tail, or an optimiser reorder) or gap fill disabled.
            inst.srcEmitFirst = inst.srcEmitLast = src->line;
            prevLine = src->line;
            ++emitted;
            continue;
        }

        uint32_t first = prevLine + 1;
        const uint32_t cap = static_cast<uint32_t>(m_sourceMaxGap);
        if (src->line - first + 1 > cap) {
            first = src->line - cap + 1;
            inst.srcTruncated = true;
            ++emitted;
        }
        inst.srcEmitFirst = first;
        inst.srcEmitLast = src->line;
        prevLine = src->line;

        // CodeOnly drops comment/blank lines, so the row count is content-dependent.
        // Ask for the real number rather than assuming the whole span.
        emitted += codeOnly ? SourceRowsFor(inst) : static_cast<int>(src->line - first + 1);
    }
}

int DebuggerDisassemblyView::SourceRowsFor(const DisasmInstruction& inst)
{
    if (!m_showSource || inst.srcEmitLast == 0 || inst.srcFileIdx < 0 ||
        inst.srcFileIdx >= static_cast<int>(m_sourcePaths.size()))
        return 0;

    int rows = inst.srcFileHeader ? 1 : 0;

    const core::SourceFile& file = m_sourceCache.Get(m_sourcePaths[inst.srcFileIdx]);
    if (!file.Ok())
        return rows + 1; // a single dim "(source not found)" marker, not a whole run

    if (inst.srcTruncated && inst.srcEmitFirst > 1)
        ++rows;

    const bool codeOnly = (m_sourceDetail == SourceDetail::CodeOnly);
    const SourceLexCache* lex = codeOnly ? &LexStatesFor(file) : nullptr;
    // Clamp to the file before looping — see the note in DrawSourceRunAt. Skipping out-of-range
    // lines inside the body is not enough: the loop still runs once per line in the range.
    const uint32_t firstRow = std::max<uint32_t>(inst.srcEmitFirst, 1);
    const uint32_t lastRow = std::min<uint32_t>(inst.srcEmitLast, file.LineCount());
    for (uint32_t line = firstRow; line <= lastRow; ++line) {
        // The line the instruction actually maps to is always shown, even when it is a
        // comment — dropping it would leave the instruction with no label at all.
        if (codeOnly && line != lastRow && line - 1 < lex->commentOrBlank.size() &&
            lex->commentOrBlank[line - 1])
            continue;
        ++rows;
    }
    return rows;
}

int DebuggerDisassemblyView::TotalSourceRows()
{
    int total = 0;
    for (const auto& inst : m_disasmCache)
        total += SourceRowsFor(inst);
    return total;
}

// Opens a source row's file in VS Code at that line. `code -g` hands the request to an
// already-running instance over VS Code's own IPC, so a live debug session lands in the
// window already showing the project rather than spawning a second editor.
void DebuggerDisassemblyView::OpenSourceInEditor(const std::string& dwarfPath, uint32_t line)
{
    const core::SourceFile& file = m_sourceCache.Get(dwarfPath);
    // The DWARF path is whatever the compiler recorded and may not exist here; the resolved
    // path is the one we actually read, so it is the one the editor can open.
    const std::string& target = file.Ok() ? file.resolvedPath : dwarfPath;

    std::string err;
    if (!core::OpenInVsCode(target, line, err))
        core::Logger::Error("Open in VS Code failed: {}", err);
}

void DebuggerDisassemblyView::DrawLanguageRail(float rowH, bool isSource)
{
    // Cell padding is stripped back off so the stripe sits on the column's true left
    // edge and butts up against the stripe of the row above — a continuous rail per
    // language reads far faster than one dash per row.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cellPos = ImGui::GetCursorScreenPos();
    const ImVec2 pad = ImGui::GetStyle().CellPadding;
    // Inside a cell the draw list is clipped to the column, so its clip rect is the
    // column's own left edge — and clamping to it keeps the full width visible instead
    // of letting part of the stripe fall outside the cell and get cut.
    const float colLeft = std::max(dl->GetClipRectMin().x, cellPos.x - pad.x);
    const float x0 = colLeft + 1.0f;
    const float y0 = cellPos.y - pad.y;
    const float y1 = y0 + rowH + pad.y * 2.0f;

    // Derived from the palette (so a retheme carries), with the disassembly rail held
    // back in alpha: the machine code is still the primary content, the C is the
    // annotation, and the eye should be able to find either without reading text.
    const ImU32 col =
        isSource
            ? (ResolveArColor(core::ArColor::SrcHeader) | IM_COL32(0, 0, 0, 255))
            : ((ResolveArColor(core::ArColor::AsmOpcode) & 0x00FFFFFF) | IM_COL32(0, 0, 0, 150));
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + kRailWidth, y1), col);

    // Push the row's text clear of the rail. Every language shares the offset, so the
    // code and the source it came from still start at one common x.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (x0 - cellPos.x) + kRailWidth + kRailPad);
}

// Row background for a source run: the language rail's own hue at low alpha, so the rail
// and the band read as one object and a retheme carries both. The alpha sits well clear of
// ImGui's zebra stripe (white at ~15) so a run reads as one solid block rather than
// shimmering, and far below the selection (alpha 200) and PC row colours so it can never be
// mistaken for either.
static ImU32 SourceRowBg()
{
    return (ResolveArColor(core::ArColor::SrcHeader) & 0x00FFFFFF) | IM_COL32(0, 0, 0, 38);
}

void DebuggerDisassemblyView::DrawSourceNoteRow(uint32_t anchorAddr, const char* idSuffix,
                                                core::ArColor color, const std::string& text,
                                                float rowH, int selCol, int textCol, bool rail)
{
    ImGui::TableNextRow();
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, SourceRowBg());
    if (!ImGui::TableSetColumnIndex(selCol))
        return;

    char id[64];
    std::snprintf(id, sizeof(id), "##%s_%08X", idSuffix, anchorAddr);
    ImGui::Selectable(id, false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, rowH));

    if (ImGui::TableSetColumnIndex(textCol)) {
        if (rail)
            DrawLanguageRail(rowH, /*isSource=*/true);
        ImGui::PushStyleColor(ImGuiCol_Text, ResolveArColor(color));
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
    }
}

void DebuggerDisassemblyView::DrawSourceLineRow(uint32_t anchorAddr, const std::string& dwarfPath,
                                                const core::SourceFile& file, uint32_t line,
                                                float rowH, int selCol, int textCol, bool rail)
{
    ImGui::TableNextRow();
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, SourceRowBg());
    if (!ImGui::TableSetColumnIndex(selCol))
        return;

    // Captured before the Selectable moves the cursor: the line number is painted on
    // the draw list from this cell's own geometry.
    const ImVec2 addrCellPos = ImGui::GetCursorScreenPos();

    char id[48];
    std::snprintf(id, sizeof(id), "##src_%08X_%u", anchorAddr, line);
    ImGui::Selectable(id, false,
                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                          ImGuiSelectableFlags_AllowDoubleClick,
                      ImVec2(0, rowH));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        OpenSourceInEditor(dwarfPath, line);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(file.resolvedPath.c_str());
        if (file.dwarfPath != file.resolvedPath)
            ImGui::TextDisabled("DWARF: %s", file.dwarfPath.c_str());
        ImGui::TextDisabled("line %u", line);
        ImGui::EndTooltip();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && m_debugLines) {
        // Resolve once, on open: ResolveLineToAddresses scans the line table linearly,
        // so re-running it every frame the popup is open would be a real per-frame cost.
        m_srcCtxPath = dwarfPath;
        m_srcCtxLine = line;
        m_srcCtxAddr = anchorAddr;
        m_srcCtxLookup = m_debugLines->ResolveLineToAddresses(dwarfPath, line);
        m_openSrcCtx = true;
    }

    // Line number in the Address column: a source row's "where am I" is its line,
    // exactly as an instruction row's is its address, so they share one column and one
    // reading position — left-aligned, starting at the same x as the "$" of the
    // addresses above and below it. Painted on the draw list rather than submitted as
    // an item so it can share the cell with the (AllowOverlap) Selectable without
    // stealing its hover.
    if (m_sourceLineNumbers) {
        char num[16];
        const int n = std::snprintf(num, sizeof(num), "%u", line);
        ImGui::GetWindowDrawList()->AddText(addrCellPos, ResolveArColor(core::ArColor::SrcGutter),
                                            num, num + n);
    }

    if (!ImGui::TableSetColumnIndex(textCol))
        return;

    if (rail)
        DrawLanguageRail(rowH, /*isSource=*/true);

    const std::string_view text = file.Line(line);
    if (text.empty()) {
        ImGui::TextUnformatted("");
        return;
    }

    const SourceLexCache& lex = LexStatesFor(file);
    core::CLexState st = (line - 1 < lex.states.size()) ? lex.states[line - 1] : core::CLexState{};
    const auto toks = core::TokenizeCLine(text, st);

    // Coalesce adjacent tokens that resolve to the same colour (whitespace between two
    // identifiers, runs of punctuation) — roughly halves the ImGui item count for free.
    bool first = true;
    size_t runStart = 0;
    ImU32 runColor = 0;
    auto flush = [&](size_t end) {
        if (end <= runStart)
            return;
        if (!first)
            ImGui::SameLine(0, 0);
        first = false;
        ImGui::PushStyleColor(ImGuiCol_Text, runColor);
        ImGui::TextUnformatted(text.data() + runStart, text.data() + end);
        ImGui::PopStyleColor();
    };
    for (size_t t = 0; t < toks.size(); ++t) {
        const ImU32 c = ResolveArColor(core::CTokenColor(toks[t].type));
        if (t == 0) {
            runStart = static_cast<size_t>(toks[t].col);
            runColor = c;
            continue;
        }
        if (c != runColor) {
            flush(static_cast<size_t>(toks[t].col));
            runStart = static_cast<size_t>(toks[t].col);
            runColor = c;
        }
    }
    if (!toks.empty())
        flush(static_cast<size_t>(toks.back().col + toks.back().len));
}

void DebuggerDisassemblyView::DrawSourceRunAt(uint32_t anchorAddr, int fileIdx, uint32_t firstLine,
                                              uint32_t lastLine, bool header, bool truncated,
                                              float rowH, int selCol, int textCol, bool rail)
{
    if (fileIdx < 0 || fileIdx >= static_cast<int>(m_sourcePaths.size()) || lastLine == 0)
        return;
    const std::string& dwarfPath = m_sourcePaths[fileIdx];
    const core::SourceFile& file = m_sourceCache.Get(dwarfPath);

    const size_t slash = dwarfPath.find_last_of("/\\");
    const std::string basename =
        (slash == std::string::npos) ? dwarfPath : dwarfPath.substr(slash + 1);

    if (header) {
        DrawSourceNoteRow(anchorAddr, "srchdr", core::ArColor::SrcHeader,
                          "; ---- " + basename + " ----", rowH, selCol, textCol, rail);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(file.Ok() ? file.resolvedPath.c_str() : dwarfPath.c_str());
            if (file.Ok() && file.dwarfPath != file.resolvedPath)
                ImGui::TextDisabled("DWARF: %s", file.dwarfPath.c_str());
            ImGui::EndTooltip();
        }
    }

    if (!file.Ok()) {
        // One dim marker per instruction that would have shown source, not one per line
        // of the run — an unresolved file must not cost more screen than a resolved one.
        const char* why = "source not found";
        switch (file.status) {
            case core::SourceFileStatus::ReadFailed:
                why = "source could not be read";
                break;
            case core::SourceFileStatus::TooLarge:
                why = "source file too large";
                break;
            case core::SourceFileStatus::Binary:
                why = "not a text file";
                break;
            default:
                break;
        }
        char note[512];
        std::snprintf(note, sizeof(note), "; %s:%u  (%s)", basename.c_str(), lastLine, why);
        DrawSourceNoteRow(anchorAddr, "srcmiss", core::ArColor::SrcMissing, note, rowH, selCol,
                          textCol, rail);
        return;
    }

    if (truncated && firstLine > 1) {
        char note[96];
        std::snprintf(note, sizeof(note), "; ... %u lines skipped", firstLine - 1);
        DrawSourceNoteRow(anchorAddr, "srcskip", core::ArColor::SrcComment, note, rowH, selCol,
                          textCol, rail);
    }

    const bool codeOnly = (m_sourceDetail == SourceDetail::CodeOnly);
    const SourceLexCache& lex = LexStatesFor(file);
    // Clamp the run to the file's real extent BEFORE looping. firstLine/lastLine come from the
    // DWARF line table, and a bad value there must not be able to hang the UI: skipping
    // out-of-range lines inside the body still costs one iteration per line, so a lastLine of
    // 0xFFFFFFFF spun here for ~4 billion iterations a frame — and never terminated at all,
    // because `line <= 0xFFFFFFFF` is always true and ++line wraps back to 0. Measured on an LTO
    // build whose line register went negative and was cast to uint32_t (fixed in DebugLines too).
    const uint32_t firstRow = std::max<uint32_t>(firstLine, 1);
    const uint32_t lastRow = std::min<uint32_t>(lastLine, file.LineCount());
    for (uint32_t line = firstRow; line <= lastRow; ++line) {
        if (codeOnly && line != lastRow && line - 1 < lex.commentOrBlank.size() &&
            lex.commentOrBlank[line - 1])
            continue;
        DrawSourceLineRow(anchorAddr, dwarfPath, file, line, rowH, selCol, textCol, rail);
    }
}

void DebuggerDisassemblyView::DrawSourceRows(const DisasmInstruction& inst, float rowH)
{
    DrawSourceRunAt(inst.address, inst.srcFileIdx, inst.srcEmitFirst, inst.srcEmitLast,
                    inst.srcFileHeader, inst.srcTruncated, rowH, /*selCol=*/2, /*textCol=*/4,
                    /*rail=*/true);
}

void DebuggerDisassemblyView::DrawTraceSourceRows(uint32_t addr, const FormattedTraceRow& row,
                                                  float rowH)
{
    DrawSourceRunAt(addr, row.srcFileIdx, row.srcLine, row.srcLine, row.srcFileHeader,
                    /*truncated=*/false, rowH, /*selCol=*/4, /*textCol=*/8, /*rail=*/false);
}

void DebuggerDisassemblyView::DrawSourceToolbar()
{
    const bool hasLines = m_debugLines && m_debugLines->IsLoaded();

    ImGui::SameLine(0, 16);
    ImGui::BeginDisabled(!hasLines);
    if (ImGui::Checkbox("Source", &m_showSource)) {
        m_sourceMapDirty = true;
        m_formattedTraceDirty = true;
        if (m_userPrefs)
            m_userPrefs->SetSourceShowInDisassembly(m_showSource);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(
            hasLines ? "Interleave the C/C++ source lines the DWARF line table names.\n"
                       "Needs the source tree present on this machine."
                     : "No DWARF line info loaded — open a program built with -g that has\n"
                       "a sibling .elf next to it.");

    // Density is the control that matters: source rows are cheap to add and expensive in
    // screen space, so it sits in the toolbar rather than behind the options popup.
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasLines || !m_showSource);
    ImGui::SetNextItemWidth(160.0f);
    int detail = static_cast<int>(m_sourceDetail);
    if (ImGui::Combo("##src_detail", &detail, "Statement only\0Code only\0Full\0")) {
        m_sourceDetail = static_cast<SourceDetail>(detail);
        m_sourceMapDirty = true;
        if (m_userPrefs)
            m_userPrefs->SetSourceDetail(detail);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("How much source to show between two statements:\n"
                          "  Statement only — just the line each statement maps to\n"
                          "  Code only      — fill the gap, minus comment and blank lines\n"
                          "  Full           — every line in the gap (objdump -S)");

    ImGui::SameLine();
    ImGui::BeginDisabled(!hasLines);
    if (ImGui::SmallButton("Source paths..."))
        m_openSourcePaths = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Where to look for the source files DWARF names, plus the\n"
                          "finer display switches. Useful precisely when source is missing.");
}

void DebuggerDisassemblyView::DrawSourcePathsPopup()
{
    if (m_openSourcePaths) {
        ImGui::OpenPopup("##src_paths");
        m_openSourcePaths = false;
    }
    if (!ImGui::BeginPopup("##src_paths"))
        return;

    ImGui::TextDisabled("DWARF records the paths the compiler saw. When those do not exist");
    ImGui::TextDisabled("here, the .elf directory is searched, then these roots.");
    ImGui::Separator();

    if (!m_sourceCache.ElfPath().empty())
        ImGui::TextDisabled("ELF: %s", m_sourceCache.ElfPath().c_str());

    // --- search roots ---
    ImGui::SeparatorText("Search roots");
    int removeIdx = -1, moveUpIdx = -1;
    for (size_t i = 0; i < m_srcRootsEdit.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("^") && i > 0)
            moveUpIdx = static_cast<int>(i);
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
            removeIdx = static_cast<int>(i);
        ImGui::SameLine();
        ImGui::TextUnformatted(m_srcRootsEdit[i].c_str());
        ImGui::PopID();
    }
    if (moveUpIdx > 0) {
        std::swap(m_srcRootsEdit[moveUpIdx], m_srcRootsEdit[moveUpIdx - 1]);
        PersistSourceRoots();
    }
    if (removeIdx >= 0) {
        m_srcRootsEdit.erase(m_srcRootsEdit.begin() + removeIdx);
        PersistSourceRoots();
    }

    ImGui::SetNextItemWidth(360.0f);
    const bool submitted =
        ImGui::InputText("##src_root_input", m_srcRootInput, sizeof(m_srcRootInput),
                         ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add") || submitted) && m_srcRootInput[0] != '\0') {
        m_srcRootsEdit.emplace_back(m_srcRootInput);
        m_srcRootInput[0] = '\0';
        PersistSourceRoots();
    }

    // --- display switches ---
    ImGui::SeparatorText("Display");
    bool optionsChanged = false;
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt("Max gap lines", &m_sourceMaxGap, 1, 100))
        optionsChanged = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Cap on a single gap fill. Only applies in Code only / Full.");
    int tabWidth = m_sourceCache.TabWidth();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt("Tab width", &tabWidth, 1, 16)) {
        m_sourceCache.SetTabWidth(tabWidth);
        m_sourceLex.clear();
        optionsChanged = true;
    }
    if (ImGui::Checkbox("File headers", &m_sourceFileHeaders))
        optionsChanged = true;
    ImGui::SameLine(0, 16);
    if (ImGui::Checkbox("Line numbers", &m_sourceLineNumbers))
        optionsChanged = true;
    if (optionsChanged) {
        m_sourceMapDirty = true;
        m_formattedTraceDirty = true;
        if (m_userPrefs)
            m_userPrefs->SetSourceOptions(m_sourceMaxGap, m_sourceCache.TabWidth(),
                                          m_sourceFileHeaders, m_sourceLineNumbers);
    }

    // --- diagnostics ---
    ImGui::SeparatorText("Files in the line table");
    if (ImGui::SmallButton("Rescan")) {
        m_sourceCache.Clear();
        m_sourceLex.clear();
        m_sourceMapDirty = true;
        m_formattedTraceDirty = true;
    }
    ImGui::SameLine();
    const auto stats = m_sourceCache.GetStats();
    ImGui::TextDisabled("%zu/%zu resolved", stats.resolved, stats.entries);

    if (m_debugLines && ImGui::BeginTable("##src_files", 3,
                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                              ImGuiTableFlags_ScrollY | kAdjustableColumns,
                                          ImVec2(660.0f, 220.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("DWARF path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Resolved to", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableHeadersRow();

        const auto& files = m_debugLines->Files();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(files.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(files[i].c_str());

                if (core::SourceFileCache::IsPseudoPath(files[i])) {
                    if (ImGui::TableSetColumnIndex(1))
                        ImGui::TextDisabled("compiler-generated");
                    continue;
                }

                auto resolved = m_sourceCache.ResolvePath(files[i]);
                ImGui::TableSetColumnIndex(1);
                if (resolved) {
                    ImGui::TextUnformatted(resolved->c_str());
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ResolveArColor(core::ArColor::SrcMissing));
                    ImGui::TextUnformatted("not found");
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        // "It doesn't work" becomes "it looked here, here and here".
                        ImGui::BeginTooltip();
                        ImGui::TextDisabled("Probed:");
                        for (const auto& c : m_sourceCache.ProbeCandidates(files[i]))
                            ImGui::TextUnformatted(c.c_str());
                        ImGui::EndTooltip();
                    }
                }

                ImGui::TableSetColumnIndex(2);
                if (resolved) {
                    // How many trailing path components actually agree — a low score on a
                    // resolved file means the match is a guess worth eyeballing.
                    ImGui::TextDisabled("%d", core::DebugLines::PathScore(files[i], *resolved));
                }
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndPopup();
}

void DebuggerDisassemblyView::DrawSourceContextMenu()
{
    if (m_openSrcCtx) {
        ImGui::OpenPopup("##src_ctx");
        m_openSrcCtx = false;
    }
    if (!ImGui::BeginPopup("##src_ctx"))
        return;

    const size_t slash = m_srcCtxPath.find_last_of("/\\");
    const std::string basename =
        (slash == std::string::npos) ? m_srcCtxPath : m_srcCtxPath.substr(slash + 1);
    ImGui::TextDisabled("%s:%u", basename.c_str(), m_srcCtxLine);
    ImGui::Separator();

    const uint32_t base = ResolveFuncBase(m_useManualBase, m_manualBaseBuf, m_segments);
    if (base == 0 && m_segments.empty())
        ImGui::TextDisabled("load base unknown — assuming 0");

    const auto& lk = m_srcCtxLookup;
    switch (lk.status) {
        case core::LineLookupStatus::Exact:
        case core::LineLookupStatus::MovedForward: {
            if (lk.status == core::LineLookupStatus::MovedForward)
                ImGui::TextDisabled("line %u generated no code; nearest is %u", lk.requestedLine,
                                    lk.resolvedLine);
            if (lk.sites.size() == 1) {
                DrawBreakpointMenuItems(m_breakpointsView, lk.sites.front().lo + base);
            } else if (lk.sites.size() > 1) {
                // Inlining and -flto duplicate a line across several addresses; silently
                // picking the first would put the breakpoint somewhere the user did not ask.
                char label[64];
                std::snprintf(label, sizeof(label), "Set breakpoint here (%zu sites)",
                              lk.sites.size());
                if (ImGui::BeginMenu(label)) {
                    for (const auto& site : lk.sites) {
                        const uint32_t addr = site.lo + base;
                        auto [hasBp, bpEnabled] = m_breakpointsView->GetBreakpointStatus(addr);
                        (void)bpEnabled;
                        char item[48];
                        std::snprintf(item, sizeof(item), "$%08X", addr);
                        if (ImGui::MenuItem(item, nullptr, hasBp))
                            m_breakpointsView->ToggleBreakpointAt(addr);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("All sites"))
                        for (const auto& site : lk.sites)
                            if (!m_breakpointsView->GetBreakpointStatus(site.lo + base).first)
                                m_breakpointsView->ToggleBreakpointAt(site.lo + base);
                    ImGui::EndMenu();
                }
            }
            break;
        }
        case core::LineLookupStatus::NoCode: {
            ImGui::BeginDisabled();
            ImGui::MenuItem("Set breakpoint here");
            ImGui::EndDisabled();
            ImGui::TextDisabled("line %u generated no code", lk.requestedLine);
            if (!lk.nearbyLines.empty() && ImGui::BeginMenu("Nearest lines with code")) {
                for (uint32_t nl : lk.nearbyLines) {
                    char item[32];
                    std::snprintf(item, sizeof(item), "line %u", nl);
                    if (ImGui::MenuItem(item)) {
                        auto alt = m_debugLines->ResolveLineToAddresses(m_srcCtxPath, nl);
                        if (!alt.sites.empty())
                            m_breakpointsView->ToggleBreakpointAt(alt.sites.front().lo + base);
                    }
                }
                ImGui::EndMenu();
            }
            break;
        }
        case core::LineLookupStatus::FileUnknown:
        case core::LineLookupStatus::FileNotCovered: {
            ImGui::BeginDisabled();
            ImGui::MenuItem("Set breakpoint here");
            ImGui::EndDisabled();
            ImGui::TextDisabled(lk.status == core::LineLookupStatus::FileUnknown
                                    ? "file is not in this ELF's line table"
                                    : "this file's compilation unit was not parsed");
            break;
        }
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Open in VS Code"))
        OpenSourceInEditor(m_srcCtxPath, m_srcCtxLine);
    if (ImGui::MenuItem("Copy file:line"))
        ImGui::SetClipboardText((m_srcCtxPath + ":" + std::to_string(m_srcCtxLine)).c_str());
    if (ImGui::MenuItem("Copy source path"))
        ImGui::SetClipboardText(m_srcCtxPath.c_str());
    if (ImGui::MenuItem("Follow in disassembly"))
        NavigateToAddress(m_srcCtxAddr);
    if (ImGui::MenuItem("Reload source file")) {
        m_sourceCache.Forget(m_srcCtxPath);
        m_sourceLex.erase(m_srcCtxPath);
        m_sourceMapDirty = true;
    }

    ImGui::EndPopup();
}

void DebuggerDisassemblyView::DrawDisasmContextMenu()
{
    if (m_openDisasmCtx) {
        ImGui::OpenPopup("##disasm_ctx");
        m_openDisasmCtx = false;
    }
    if (ImGui::BeginPopup("##disasm_ctx")) {
        ImGui::TextDisabled("$%08X", m_ctxLineAddr);
        ImGui::Separator();

        // Navigation — operand-specific items first when an address was clicked.
        if (m_ctxHasOperand) {
            if (ImGui::MenuItem("Follow operand in disassembly"))
                NavigateToAddress(m_ctxOperandAddr);
            if (ImGui::MenuItem("Show operand in memory") && m_addrCtx && m_addrCtx->jumpToMemory)
                m_addrCtx->jumpToMemory(m_ctxOperandAddr, 1);
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Follow line in disassembly"))
            NavigateToAddress(m_ctxLineAddr);
        if (ImGui::MenuItem("Show line in memory") && m_addrCtx && m_addrCtx->jumpToMemory)
            m_addrCtx->jumpToMemory(m_ctxLineAddr, 1);
        ImGui::Separator();

        // Breakpoint toggle + watchpoint submenu (shared with the AR console menu).
        if (m_breakpointsView) {
            DrawBreakpointMenuItems(m_breakpointsView, m_ctxLineAddr);
            ImGui::Separator();
        }

        // Annotations — label and comment are independent fields.
        if (m_annotations) {
            auto label = m_annotations->GetLabel(m_ctxLineAddr);
            if (ImGui::MenuItem(label ? "Edit label..." : "Set label...")) {
                snprintf(m_annotBuf, sizeof(m_annotBuf), "%s", label ? label->c_str() : "");
                m_annotEditIsLabel = true;
                m_openAnnotEdit = true;
            }
            if (label && ImGui::MenuItem("Clear label"))
                m_annotations->RemoveLabel(m_ctxLineAddr);
            auto comment = m_annotations->GetComment(m_ctxLineAddr);
            if (ImGui::MenuItem(comment ? "Edit comment..." : "Set comment...")) {
                snprintf(m_annotBuf, sizeof(m_annotBuf), "%s", comment ? comment->c_str() : "");
                m_annotEditIsLabel = false;
                m_openAnnotEdit = true;
            }
            if (comment && ImGui::MenuItem("Clear comment"))
                m_annotations->RemoveComment(m_ctxLineAddr);
            auto block = m_annotations->GetBlock(m_ctxLineAddr);
            if (ImGui::MenuItem(block ? "Edit comment block..." : "Set comment block...")) {
                snprintf(m_blockBuf, sizeof(m_blockBuf), "%s", block ? block->c_str() : "");
                m_openBlockEdit = true;
            }
            if (block && ImGui::MenuItem("Clear comment block"))
                m_annotations->RemoveBlock(m_ctxLineAddr);
            ImGui::Separator();
        }

        // Clipboard
        if (ImGui::MenuItem("Copy address")) {
            char b[16];
            snprintf(b, sizeof(b), "$%08X", m_ctxLineAddr);
            ImGui::SetClipboardText(b);
        }
        if (ImGui::MenuItem("Copy hex bytes")) {
            for (const auto& in : m_disasmCache)
                if (in.address == m_ctxLineAddr) {
                    ImGui::SetClipboardText(in.hex.c_str());
                    break;
                }
        }
        ImGui::EndPopup();
    }

    // Label / comment editor modal (opened from the menu above).
    if (m_openAnnotEdit) {
        ImGui::OpenPopup("##disasm_annot");
        m_openAnnotEdit = false;
    }
    if (ImGui::BeginPopup("##disasm_annot")) {
        ImGui::Text("%s for $%08X", m_annotEditIsLabel ? "Label" : "Comment", m_ctxLineAddr);
        ImGui::SetNextItemWidth(280);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool commit = ImGui::InputText("##an", m_annotBuf, sizeof(m_annotBuf),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Save") || commit) && m_annotations) {
            if (m_annotEditIsLabel)
                m_annotations->SetLabel(m_ctxLineAddr, m_annotBuf);
            else
                m_annotations->SetComment(m_ctxLineAddr, m_annotBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void DebuggerDisassemblyView::DrawTraceMode()
{
    if (m_traceData.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "No trace data — enable CPU tracing and run the emulator";
        ImVec2 textSize = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
        return;
    }

    // Search UI
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Search PC:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::InputText("##trace_search", m_traceSearchAddr, sizeof(m_traceSearchAddr),
                         ImGuiInputTextFlags_CharsHexadecimal)) {
        if (m_traceSearchAddr[0] != '\0') {
            m_traceSearchValue = (uint32_t)strtoul(m_traceSearchAddr, nullptr, 16);
            m_traceSearchActive = true;
        } else {
            m_traceSearchActive = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        m_traceSearchAddr[0] = '\0';
        m_traceSearchActive = false;
    }

    ImGui::Separator();

    const ImU32 amberBg = IM_COL32(180, 120, 0, 100);
    const ImU32 matchBg = IM_COL32(0, 120, 180, 80);

    float availX = ImGui::GetContentRegionAvail().x;
    float minimapWidth = m_traceSearchActive ? 20.0f : 0.0f;
    float tableWidth =
        availX - minimapWidth - (m_traceSearchActive ? ImGui::GetStyle().ItemSpacing.x : 0.0f);

    // Control-flow visualization, paused-only (chasing the live PC at run speed makes it
    // unreadable, and the per-row work is wasteful): a gutter column holds taken-branch /
    // call / return markers, and the instruction text is indented by call-nesting depth.
    // The gutter column is always present so the layout doesn't jump.
    const bool paused = m_emulatorView && m_emulatorView->IsPaused();
    const float gutterW = paused ? 11.0f : 6.0f;
    const float kIndentPx = 13.0f; // per nesting level

    PushFontRole(FontRole::Code);
    ImGui::BeginChild("##trace_scroll", ImVec2(tableWidth, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::BeginTable("##trace_table", 9,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingFixedFit | kAdjustableColumns |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        const float rowH = ImGui::GetTextLineHeight() + 2.0f;
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide,
                                rowH);
        ImGui::TableSetupColumn(
            "", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, gutterW);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("L-Ref", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("CCK", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("68k", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch);

        // Manual header row so the cycle columns (and the index) can carry tooltips.
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < 9; ++c) {
            if (!ImGui::TableSetColumnIndex(c))
                continue;
            ImGui::TableHeader(ImGui::TableGetColumnName(c));
            if (ImGui::IsItemHovered()) {
                if (c == 2)
                    ImGui::SetTooltip(
                        "Sequential trace index. Matches the '#N' label shown over this\n"
                        "instruction's DMA slot-run in the Frame Scrubber when zoomed in.");
                else if (c == 5)
                    ImGui::SetTooltip(
                        "Measured color-clocks elapsed to the next traced instruction.\n"
                        "1 CCK = 2 CPU cycles. Includes DMA / chip-bus contention, so a\n"
                        "value far above the 68k column means the CPU was stalled.");
                else if (c == 6)
                    ImGui::SetTooltip(
                        "Textbook 68000 instruction cycles (CPU clocks, no contention).\n"
                        "Best-effort estimate; MUL/DIV are worst-case, '-' = not modeled.");
            }
        }

        const float circleR = rowH * 0.3f;
        const ImU32 colEnabled = IM_COL32(210, 50, 50, 255);
        const ImU32 colDisabled = IM_COL32(210, 50, 50, 180);

        // m_formattedTrace is rebuilt in lockstep with m_traceData, but clamp the row
        // count to the smaller of the two so a transient desync can never index either
        // vector out of bounds (GetTraceRowDisplay guards the same pair for callers).
        int count = std::min(static_cast<int>(m_traceData.size()),
                             static_cast<int>(m_formattedTrace.size()));

        // Per-visible-row geometry for the gutter markers, drawn after the clipper loop
        // (a marker drawn mid-cell would be clipped to that cell's channel).
        struct GutterRow
        {
            int idx;
            float y0, y1; // row top / bottom in screen space
        };
        std::vector<GutterRow> gutterRows;
        float gutterX1 = 0.0f;
        bool gutterGeomSet = false;

        ImGuiListClipper clipper;
        clipper.Begin(count);
        if (m_traceScrollToSelected)
            clipper.IncludeItemByIndex(m_selectedTraceIdx);

        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& row = m_formattedTrace[i];
                bool isSelected = (i == m_selectedTraceIdx);
                bool isMatch = m_traceSearchActive && (m_traceData[i].pc == m_traceSearchValue);
                bool isHover =
                    (m_traceHoverLRef != UINT32_MAX) && (row.lRefValue == m_traceHoverLRef);
                bool isFuture = (i > m_selectedTraceIdx);
                uint32_t addr = m_traceData[i].pc;

                // Comment block + label lines above the instruction (own table rows).
                if (m_annotations) {
                    if (auto block = m_annotations->GetBlock(addr))
                        DrawCommentBlock(addr, *block, /*addrCol=*/4);
                    auto lbl = m_annotations->GetLabel(addr);
                    if (lbl) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%s:", lbl->c_str());
                    }
                }

                if (m_showSource && row.srcLine != 0)
                    DrawTraceSourceRows(addr, row, rowH);

                ImGui::TableNextRow();

                if (isSelected) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, amberBg);
                    if (m_traceScrollToSelected) {
                        float rowY = ImGui::GetCursorScreenPos().y;
                        float rowH2 = ImGui::GetTextLineHeightWithSpacing();
                        float winY = ImGui::GetWindowPos().y;
                        float winH = ImGui::GetWindowSize().y;
                        float frozenH = ImGui::GetTextLineHeightWithSpacing();
                        float visH = std::max(winH - frozenH, rowH2);
                        if (rowY < winY + frozenH)
                            ImGui::SetScrollHereY(std::clamp(4.0f * rowH2 / visH, 0.0f, 1.0f));
                        else if (rowY + rowH2 > winY + winH)
                            ImGui::SetScrollHereY(
                                std::clamp(1.0f - 5.0f * rowH2 / visH, 0.0f, 1.0f));
                        m_traceScrollToSelected = false;
                    }
                } else if (isMatch) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, matchBg);
                } else if (isHover) {
                    // Mirror the Frame Scrubber's white DMA-slot outline as a row tint.
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(70, 90, 120, 150));
                }

                ImGui::PushID(i);

                // --- Column 0: status circle ---
                ImGui::TableSetColumnIndex(0);
                const ImVec2 col0Pos = ImGui::GetCursorScreenPos();
                const float col0W = ImGui::GetContentRegionAvail().x;

                auto [hasBp, bpEnabled] = m_breakpointsView->GetBreakpointStatus(addr);

                if (ImGui::InvisibleButton("##bp_toggle", ImVec2(col0W, rowH))) {
                    m_breakpointsView->ToggleBreakpointAt(addr);
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    m_breakpointsView->RemoveBreakpointAt(addr);
                }
                if (hasBp && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(bpEnabled ? "Enabled" : "Disabled");
                }

                const ImVec2 center(col0Pos.x + col0W * 0.5f, col0Pos.y + rowH * 0.5f);
                if (hasBp) {
                    if (bpEnabled)
                        ImGui::GetWindowDrawList()->AddCircleFilled(center, circleR, colEnabled);
                    else
                        ImGui::GetWindowDrawList()->AddCircle(center, circleR, colDisabled, 0,
                                                              1.5f);
                }

                // --- Column 1: control-flow gutter (lines drawn after the loop) ---
                if (paused) {
                    ImGui::TableSetColumnIndex(1);
                    if (!gutterGeomSet) {
                        const ImVec2 gp = ImGui::GetCursorScreenPos();
                        gutterX1 = gp.x + ImGui::GetContentRegionAvail().x;
                        gutterGeomSet = true;
                    }
                    gutterRows.push_back({i, col0Pos.y, col0Pos.y + rowH});
                }

                // # column — sequential trace index (matches the scrubber's "#N" slot label)
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%d", i);

                // L-Ref column
                ImGui::TableSetColumnIndex(3);
                if (isFuture)
                    ImGui::TextDisabled("%s", row.lRef.c_str());
                else
                    ImGui::TextUnformatted(row.lRef.c_str());

                // Address column — selectable spans all columns
                ImGui::TableSetColumnIndex(4);
                bool sel = isSelected;
                if (isFuture)
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                if (ImGui::Selectable(row.label.c_str(), &sel,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap,
                                      ImVec2(0, 0))) {
                    m_selectedTraceIdx = i;
                    SelectInstructionDocFromDisasm(row.mnemo);
                    FireTraceSelected(true);
                    if (m_onJumpToLRef)
                        m_onJumpToLRef(row.lRefValue);
                }
                if (isFuture)
                    ImGui::PopStyleColor();

                // CCK column — measured color-clocks elapsed (0 for last entry, unknown)
                ImGui::TableSetColumnIndex(5);
                if (row.deltaCycles > 0) {
                    if (isFuture)
                        ImGui::TextDisabled("%u", row.deltaCycles);
                    else
                        ImGui::Text("%u", row.deltaCycles);
                } else {
                    ImGui::TextDisabled("-");
                }

                // 68k column — textbook instruction cycles. Highlighted amber when the
                // measured time (in CPU cycles = CCK*2) runs well over nominal, flagging
                // a bus/DMA stall.
                ImGui::TableSetColumnIndex(6);
                if (row.nominalCycles >= 0) {
                    // measured CPU cycles = CCK*2; flag when it exceeds ~1.5x nominal.
                    const bool stalled =
                        row.deltaCycles > 0 && (int)(row.deltaCycles * 4) > row.nominalCycles * 3;
                    if (isFuture)
                        ImGui::TextDisabled("%d", row.nominalCycles);
                    else if (stalled)
                        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "%d",
                                           row.nominalCycles);
                    else
                        ImGui::Text("%d", row.nominalCycles);
                } else {
                    ImGui::TextDisabled("-");
                }

                // Bytes column
                ImGui::TableSetColumnIndex(7);
                ImGui::TextDisabled("%s", row.hexStr.c_str());

                // Instruction column: past/current = white, future = gray
                ImGui::TableSetColumnIndex(8);
                // Indent by call-nesting depth so subroutine bodies step inward (paused
                // only — depth is computed in RebuildFormattedTrace).
                if (paused && row.depth > 0) {
                    ImGui::Dummy(ImVec2((float)row.depth * kIndentPx, 0.0f));
                    ImGui::SameLine(0, 0);
                }
                if (isFuture)
                    ImGui::TextDisabled("%s", row.mnemo.c_str());
                else
                    ImGui::TextUnformatted(row.mnemo.c_str());
                // Trace rows are historical, so the live a6 can't identify the
                // target library; fall back to the exec default (strategy a).
                std::string libSuffix = LibraryCallSuffix(row.mnemo, nullptr, nullptr);
                if (!libSuffix.empty()) {
                    ImGui::SameLine(0, 0);
                    ImGui::TextColored(ImVec4(0.55f, 0.70f, 0.95f, 1.0f), "%s", libSuffix.c_str());
                }
                if (m_annotations) {
                    std::string branchLbl = m_annotations->BuildBranchLabelSuffix(addr, row.mnemo);
                    if (!branchLbl.empty()) {
                        ImGui::SameLine(0, 0);
                        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%s",
                                           branchLbl.c_str());
                    }
                    std::string suffix = m_annotations->BuildAnnotationSuffix(addr, row.mnemo);
                    if (!suffix.empty()) {
                        ImGui::SameLine(0, 0);
                        ImGui::TextColored(ImVec4(0.45f, 0.75f, 0.45f, 1.0f), "%s", suffix.c_str());
                    }
                }

                ImGui::PopID();
            }
        }
        clipper.End();

        // --- Control-flow gutter markers (paused only) ---
        // Drawn on the table's background channel so they aren't clipped to a single
        // cell (same approach as the live-mode branch arrows). A small filled triangle
        // marks each row where execution did not fall through, colored by kind.
        if (paused && gutterGeomSet && !gutterRows.empty()) {
            ImGui::TablePushBackgroundChannel();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 winPos = ImGui::GetWindowPos();
            const ImVec2 clipMin(winPos.x, winPos.y + ImGui::TableGetHeaderRowHeight());
            const ImVec2 clipMax(winPos.x + ImGui::GetWindowWidth(),
                                 winPos.y + ImGui::GetWindowHeight());
            dl->PushClipRect(clipMin, clipMax, true);

            const ImU32 colCall = IM_COL32(120, 200, 120, 235);  // BSR/JSR (push)
            const ImU32 colReturn = IM_COL32(230, 160, 70, 235); // RTS/RTE/RTR (pop)
            const ImU32 colBranch = IM_COL32(95, 160, 230, 225); // taken Bcc/BRA/JMP

            for (const GutterRow& gr : gutterRows) {
                const auto& fr = m_formattedTrace[gr.idx];
                if (!fr.diverged)
                    continue;
                const ImU32 mc = (fr.cfKind == CF_CALL)     ? colCall
                                 : (fr.cfKind == CF_RETURN) ? colReturn
                                                            : colBranch;
                const float yMid = (gr.y0 + gr.y1) * 0.5f;
                const float rx = gutterX1 - 1.0f;
                dl->AddTriangleFilled(ImVec2(rx, yMid), ImVec2(rx - 6.0f, yMid - 3.5f),
                                      ImVec2(rx - 6.0f, yMid + 3.5f), mc);
            }

            dl->PopClipRect();
            ImGui::TablePopBackgroundChannel();
        }

        ImGui::EndTable();
    }
    ImGui::EndChild();
    PopFontRole();

    if (m_traceSearchActive) {
        ImGui::SameLine();

        // Minimap (Gully)
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImVec2 size = ImVec2(minimapWidth, ImGui::GetContentRegionAvail().y);
        ImGui::InvisibleButton("##minimap", size);
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
            float relY = (ImGui::GetIO().MousePos.y - cursor.y) / size.y;
            int targetIdx =
                std::clamp((int)(relY * m_traceData.size()), 0, (int)m_traceData.size() - 1);
            m_selectedTraceIdx = targetIdx;
            m_traceScrollToSelected = true;
            FireTraceSelected(true);
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(cursor, ImVec2(cursor.x + size.x, cursor.y + size.y),
                                IM_COL32(30, 30, 30, 255));

        int totalTrace = (int)m_traceData.size();
        if (totalTrace > 0) {
            // Draw match ticks
            for (int i = 0; i < totalTrace; ++i) {
                if (m_traceData[i].pc == m_traceSearchValue) {
                    float y = cursor.y + (float)i / (float)totalTrace * size.y;
                    drawList->AddLine(ImVec2(cursor.x, y), ImVec2(cursor.x + size.x, y),
                                      IM_COL32(0, 180, 255, 255));
                }
            }

            // Draw selection marker
            float selY = cursor.y + (float)m_selectedTraceIdx / (float)totalTrace * size.y;
            drawList->AddRectFilled(ImVec2(cursor.x, selY - 1), ImVec2(cursor.x + size.x, selY + 1),
                                    IM_COL32(255, 200, 0, 255));
        }
    }
}

void DebuggerDisassemblyView::DrawProfileWindow()
{
    if (!m_profileOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_profileNoFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_profileNoFocus = false;
    }

    if (ImGui::Begin(ICON_FA_CHART_COLUMN " Profile###Profile##debugger", &m_profileOpen, flags)) {
        if (m_turboSuppressed) {
            DrawTurboSuppressedPlaceholder();
        } else {
            // Standing alone, the window has to be able to arm the capture it reports on —
            // it can no longer borrow the Disassembly toolbar's copy of this toggle.
            if (m_emulatorView && m_emulatorView->IsRunning()) {
                auto* emu = m_emulatorView->GetEmulator();
                bool traceEnabled = emu ? emu->IsCpuTraceEnabled() : false;
                if (ImGui::Checkbox("CPU Trace", &traceEnabled) && emu)
                    emu->SetCpuTraceEnabled(traceEnabled);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Record every M68K instruction per frame\n~14k-17k "
                                      "entries/frame on A500 (~3.7 MB)");
                ImGui::Separator();
            }
            DrawProfileMode();
        }
    }
    ImGui::End();
}

void DebuggerDisassemblyView::DrawProfileMode()
{
    if (m_traceData.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "No trace data — enable CPU tracing and run the emulator";
        ImVec2 textSize = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
        ImGui::TextDisabled("%s", msg);
        return;
    }

    EnsureProfiler();
    // Lazy rebuild when profiler data changes
    if (m_profilerVersion != m_profiler.GetFrameVersion())
        RebuildProfileRows();

    // Source-level grouping: when a sibling .elf has been loaded for this document,
    // offer a per-function view instead of the raw per-PC hotspot table.
    if (m_debugSymbols) {
        ImGui::Checkbox("Group by function", &m_groupByFunction);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered() && !m_debugSymbols->SourcePath().empty()) {
            ImGui::SetTooltip("Symbols: %s", m_debugSymbols->SourcePath().c_str());
        }
        if (m_groupByFunction) {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            if (ImGui::Checkbox("Manual base", &m_useManualBase))
                m_funcRowsDirty = true;
            if (m_useManualBase) {
                ImGui::SameLine();
                ImGui::TextUnformatted("$");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::InputText("##manualbase", m_manualBaseBuf, sizeof(m_manualBaseBuf),
                                     ImGuiInputTextFlags_CharsHexadecimal))
                    m_funcRowsDirty = true;
            }
            ImGui::Separator();
            DrawFunctionTable();
            return;
        }
    }

    // Summary header
    int totalEntries = static_cast<int>(m_traceData.size());
    int uniquePCs = static_cast<int>(m_profileRows.size());
    ImGui::TextDisabled(
        "%d instructions  |  %d unique PCs  |  cycles = elapsed CCKs (not ISA cycles)",
        totalEntries, uniquePCs);
    ImGui::Separator();

    // A Source (file:line) column is shown when DWARF line info is loaded. The
    // column is ALWAYS declared (constant column count) and merely disabled when
    // there is no line info — changing a table's column count across frames for the
    // same ID corrupts ImGui's internal column span and asserts.
    const bool hasLines = m_debugLines && m_debugLines->IsLoaded();
    const uint32_t lineBase = ResolveFuncBase(m_useManualBase, m_manualBaseBuf, m_segments);

    PushFontRole(FontRole::Code);
    ImGui::BeginChild("##profile_scroll", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::BeginTable("##profile_table", 7,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingFixedFit | kAdjustableColumns |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // Columns 0-3 are sortable; Range cy and Instruction are display-only.
        ImGui::TableSetupColumn("Address",
                                ImGuiTableColumnFlags_WidthFixed |
                                    ImGuiTableColumnFlags_DefaultSort |
                                    ImGuiTableColumnFlags_PreferSortAscending,
                                90.0f);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("Total cy", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("\xc3\xb8 cy", // ø cy
                                ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn(
            "Range cy", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 90.0f);
        ImGui::TableSetupColumn("Instruction",
                                ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
        ImGui::TableSetupColumn("Source",
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort |
                                    (hasLines ? 0 : ImGuiTableColumnFlags_Disabled),
                                150.0f);
        ImGui::TableHeadersRow();

        // Apply sort if ImGui signals a change
        if (auto* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty && !m_profileRows.empty()) {
                if (specs->SpecsCount > 0) {
                    m_profileSortCol = specs->Specs[0].ColumnIndex;
                    m_profileSortDir = specs->Specs[0].SortDirection;
                }
                ApplyProfileSort();
                specs->SpecsDirty = false;
            }
        }

        int rowCount = static_cast<int>(m_profileRows.size());
        ImGuiListClipper clipper;
        clipper.Begin(rowCount);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& s = m_profileRows[i];
                ImGui::TableNextRow();

                // Address
                ImGui::TableSetColumnIndex(0);
                char addrBuf[32];
                snprintf(addrBuf, sizeof(addrBuf), "$%08X##pr%d", s.pc, i);
                bool sel = false;
                if (ImGui::Selectable(addrBuf, &sel,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap,
                                      ImVec2(0, 0))) {
                    // Switch the Disassembly window to Trace mode and search for this
                    // address — raising that window too, since we are no longer in it.
                    m_open = true;
                    m_mode = Mode::Trace;
                    snprintf(m_traceSearchAddr, sizeof(m_traceSearchAddr), "%08X", s.pc);
                    m_traceSearchValue = s.pc;
                    m_traceSearchActive = true;
                    // Find the first occurrence in trace and scroll to it
                    for (int ti = 0; ti < (int)m_traceData.size(); ++ti) {
                        if (m_traceData[ti].pc == s.pc) {
                            m_selectedTraceIdx = ti;
                            m_traceScrollToSelected = true;
                            FireTraceSelected(true);
                            break;
                        }
                    }
                }

                // Count
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", s.count);

                // Total cy
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", s.totalCycles);

                // Avg cy (ø)
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.1f", s.AvgCycles());

                // Range cy
                ImGui::TableSetColumnIndex(4);
                char rangeBuf[32];
                snprintf(rangeBuf, sizeof(rangeBuf), "%u-%u", s.minCycles, s.maxCycles);
                ImGui::TextUnformatted(rangeBuf);

                // Instruction
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(s.mnemonic.empty() ? "???" : s.mnemonic.c_str());

                // Source (file:line). The column is disabled when no line info is
                // loaded, so TableSetColumnIndex returns false and we skip it.
                if (hasLines && ImGui::TableSetColumnIndex(6)) {
                    std::string src = SourceLineLabel(m_debugLines, lineBase, s.pc);
                    if (!src.empty())
                        ImGui::TextDisabled("%s", src.c_str());
                }
            }
        }
        clipper.End();

        ImGui::EndTable();
    }
    ImGui::EndChild();
    PopFontRole();
}

} // namespace adm::ui
