#include "DebuggerCallStackView.h"
#include "../FontRoles.h"
#include "../TableLayout.h"

#include <imgui.h>

#include <filesystem>

#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

namespace {

std::string BaseName(const std::string& path)
{
    if (path.empty())
        return {};
    return std::filesystem::path(path).filename().string();
}

const char* TraceHint(DebuggerCallStackView::TraceState state)
{
    switch (state) {
        case DebuggerCallStackView::TraceState::NoSymbols:
            return "No debug symbols. Load a program with a sibling .elf.";
        case DebuggerCallStackView::TraceState::NoCpuTrace:
            return "CPU tracing is off, so no frame has been reconstructed.\n"
                   "Enable it in the Disassembly window (Trace/Profile) or the CPU window.";
        case DebuggerCallStackView::TraceState::NoSegments:
            return "Symbols are loaded but the program's load address is unknown -- no\n"
                   "AmigaDOS process was found. Start the program, or use Live mode.";
        case DebuggerCallStackView::TraceState::NothingAtPlayhead:
            return "Nothing traced at this position -- the playhead is over ROM, a library\n"
                   "call or an untraced gap. Scrub the Frame Scrubber timeline.";
        case DebuggerCallStackView::TraceState::Ok:
            break;
    }
    return "";
}

} // namespace

bool DebuggerCallStackView::ShowLive() const
{
    if (m_mode == Mode::Live)
        return true;
    if (m_mode == Mode::Trace)
        return false;
    return m_haveLive; // Auto: the live stack is only fed while the machine is halted
}

void DebuggerCallStackView::Draw()
{
    if (!m_open)
        return;

    if (m_noFocus) {
        ImGui::SetNextWindowFocus();
        m_noFocus = false;
    }
    ImGui::SetNextWindowSize(ImVec2(420.0f, 280.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Call Stack", &m_open)) {
        ImGui::End();
        return;
    }

    if (m_turboSuppressed) {
        DrawTurboSuppressedPlaceholder();
        ImGui::End();
        return;
    }

    const bool live = ShowLive();

    ImGui::SetNextItemWidth(90.0f);
    int mode = static_cast<int>(m_mode);
    if (ImGui::Combo("##mode", &mode, "Auto\0Live\0Trace\0"))
        m_mode = static_cast<Mode>(mode);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Live  - unwind the halted CPU's stack (DWARF call-frame info)\n"
                          "Trace - slice the Frame Scrubber's flame chart at the playhead\n"
                          "Auto  - Live while paused, Trace while running");
    }
    ImGui::SameLine();
    if (live) {
        ImGui::TextDisabled("Live  at halted PC");
    } else {
        ImGui::TextDisabled("Trace  at playhead L%07X", m_lRef);
    }
    ImGui::Separator();

    PushFontRole(FontRole::Code);
    if (live)
        DrawLive();
    else
        DrawTrace();
    PopFontRole();

    ImGui::End();
}

void DebuggerCallStackView::DrawLive()
{
    if (!m_haveLive) {
        ImGui::TextWrapped("The machine is running. Pause, step, or hit a breakpoint to unwind "
                           "the live stack.");
        return;
    }

    if (m_live.frames.empty()) {
        ImGui::TextWrapped("%s", adm::core::DescribeUnwindStop(m_live, m_liveRegion).c_str());
        return;
    }

    if (ImGui::BeginTable("##livestack", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              kAdjustableColumns | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 26.0f);
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < m_live.frames.size(); ++i) {
            const core::UnwoundFrame& f = m_live.frames[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%d", static_cast<int>(i));

            ImGui::TableSetColumnIndex(1);
            const std::string name = f.name.empty() ? "<unknown>" : f.name;
            // Frame 0 is where the CPU actually is; the rest are callers, dimmed so the
            // live frame stands out. Same convention as the trace view.
            const bool dim = (i != 0);
            if (dim)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const bool selected = ImGui::Selectable(name.c_str(), false,
                                                    ImGuiSelectableFlags_SpanAllColumns |
                                                        ImGuiSelectableFlags_AllowOverlap);
            if (dim)
                ImGui::PopStyleColor();
            if (f.inlined) {
                ImGui::SameLine();
                ImGui::TextDisabled("(inlined)");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", name.c_str());
                if (!f.file.empty()) {
                    ImGui::Text("%s:%u", f.file.c_str(), f.line);
                    ImGui::TextDisabled("double-click to open in editor");
                }
                ImGui::Separator();
                ImGui::Text("pc  $%08X", f.pc);
                if (!f.inlined) {
                    ImGui::Text("sp  $%08X", f.sp);
                    ImGui::Text("cfa $%08X", f.cfa);
                } else {
                    ImGui::TextDisabled("inlined - shares its caller's frame");
                }
                ImGui::EndTooltip();
            }
            // Single click follows the frame in the Disassembly window; double click opens
            // the source. Opening an editor on every click would be far too eager.
            if (selected && m_onNavigate)
                m_onNavigate(f.pc);
            if (m_onOpenSource && !f.file.empty() && f.line > 0 && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                m_onOpenSource(f.file, f.line);
            }

            ImGui::TableSetColumnIndex(2);
            if (f.file.empty())
                ImGui::TextDisabled("-");
            else
                ImGui::TextDisabled("%s:%u", BaseName(f.file).c_str(), f.line);

            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("$%08X", f.pc);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Always say how the walk ended: a stack truncated because the trail was lost and one
    // that genuinely reached the outermost frame look identical otherwise.
    ImGui::Separator();
    ImGui::TextDisabled("%s", adm::core::DescribeUnwindStop(m_live, m_liveRegion).c_str());
}

void DebuggerCallStackView::DrawTrace()
{
    if (m_traceState != TraceState::Ok || m_frames.empty()) {
        const TraceState state =
            (m_traceState == TraceState::Ok) ? TraceState::NothingAtPlayhead : m_traceState;
        ImGui::TextWrapped("%s", TraceHint(state));
        return;
    }

    if (ImGui::BeginTable("##callstack", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                              kAdjustableColumns)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // Innermost (deepest) frame first — the conventional call-stack order.
        const int n = static_cast<int>(m_frames.size());
        for (int i = n - 1; i >= 0; --i) {
            const adm::core::CallFrame& f = m_frames[static_cast<size_t>(i)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%d", f.depth);
            ImGui::TableSetColumnIndex(1);
            // The innermost frame is the live one; callers are dimmed.
            if (i == n - 1)
                ImGui::TextUnformatted(f.name.empty() ? "<unknown>" : f.name.c_str());
            else
                ImGui::TextDisabled("%s", f.name.empty() ? "<unknown>" : f.name.c_str());
        }
        ImGui::EndTable();
    }
}

} // namespace adm::ui
