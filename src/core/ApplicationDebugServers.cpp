#include "Application.h"

#include "UaeSavestate.h"

#include <GLFW/glfw3.h>
#include "../ui/DmaColors.h"
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include <nfd.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <random>
#include <span>
#include <tuple>

// Font Awesome & ImGui
#include <imgui_internal.h>
#include "ui/IconsFontAwesome6.h"

// nanosvg - SVG parser and rasterizer (implementation provided by nanosvg static lib)
#include <nanosvg.h>
#include <nanosvgrast.h>

#include "CapsImageLoader.h"
#include "Constants.h"
#include "DmsConverter.h"
#include "EmulatorStateOps.h"
#include "ExtAdfConverter.h"
#include "HexUtil.h"
#include "IpfConverter.h"
#include "TosecManager.h"
#include "ZipUtils.h"

#include <imgui.h>
#include <zip.h>
#include <filesystem>

#include "AmigaScreenMap.h"
#include "BlitScreenMapper.h"

#include "../ui/views/ARConsoleView.h"
#include "../ui/views/AdvancedFindView.h"
#include "../ui/views/AudioPlayer.h"
#include "../ui/views/BitmapView.h"
#include "../ui/views/BlitOpPresets.h"
#include "../ui/views/BlitterMintermView.h"
#include "../ui/views/BlitterView.h"
#include "../ui/views/BlockEditorView.h"
#include "../ui/views/BlockMapView.h"
#include "../ui/views/BootBlockView.h"
#include "../ui/views/BreakpointsView.h"
#include "../ui/views/ChangeMapView.h"
#include "../ui/views/DebuggerDisassemblyView.h"
#include "../ui/views/DebuggerMemoryView.h"
#include "../ui/views/DebuggerTrainerView.h"
#include "../ui/views/DebuggerView.h"
#include "../ui/views/DiskIoView.h"
#include "../ui/views/DmaStatsView.h"
#include "../ui/views/DmsView.h"
#include "../ui/views/EmulatorHostHooks.h"
#include "../ui/views/EmulatorView.h"
#include "../ui/views/FileListView.h"
#include "../ui/views/FileViewer.h"
#include "../ui/views/FloppyDiskView.h"
#include "../ui/views/HunkViewer.h"
#include "../ui/views/IdentifyView.h"
#include "../ui/views/ImageViewer.h"
#include "../ui/views/LogView.h"
#include "../ui/views/McpActivityView.h"
#include "../ui/views/MemoryMapView.h"
#include "../ui/views/NDOSView.h"
#include "../ui/views/PanelView.h"
#include "../ui/views/RegisterOpsView.h"
#include "../ui/views/TosecBrowserView.h"
#include "../ui/views/ValidationView.h"
#include "AudioModule.h"
#include "DebugFrame.h"
#include "DebugInline.h"
#include "DebugLines.h"
#include "DebugSymbols.h"
#include "DiskSearcher.h"
#include "DiskValidator.h"
#include "ExocetPack.h"
#include "ExocetSession.h"
#include "HunkParser.h"
#include "Logger.h"
#include "MemoryLocks.h"
#include "MemorySearcher.h"
#include "SegListResolver.h"
#include "TrainerFile.h"

// glfw3native.h re-introduces windows.h; undef the macros that conflict with our method names.
#ifdef _WIN32
#undef CreateDirectory
#undef DeleteFile
#undef MoveFile
#endif

// Second translation unit for Application, holding only SetupGdbServer() and
// SetupMcpServer().
//
// Not a decomposition: these two functions are 843 lines of binding ~87 hooks onto ops
// structs, and the members they reach span most of the class, so no collaborator can own
// them without an interface as wide as Application itself.
// What they can do is compile somewhere
// else. Splitting them out of Application.cpp lets the two halves build in parallel instead of
// serially, which is the whole point of this file.

namespace adm {
namespace core {

// Resolve a caller-supplied window key to its ImGui identity. Accepts the View-menu name, the
// ImGui id, a panel name, or the name of any other live window. Shared by the focus and
// screenshot hooks below.
static std::string ResolveUiWindowId(const ui::UiSources& src, const std::string& window,
                                     std::string& err)
{
    for (const auto& e : src.windows) {
        if (e.name != window && e.imguiId != window)
            continue;
        if (!e.isWindow) {
            err = "'" + window +
                  "' is a draw-list overlay drawn into the emulator window, not a window of its "
                  "own; address the Amiga Emulator window instead";
            return {};
        }
        return e.imguiId;
    }
    for (const auto& p : src.panels)
        if (p.name == window)
            return fmt::format("{}##panel_{:08X}", p.name, p.id);
    if (ImGui::FindWindowByName(window.c_str()))
        return window;
    err = "unknown window: '" + window + "' (call list_windows for the addressable names)";
    return {};
}

void Application::SetupGdbServer()
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;

    // Let the Breakpoints window name the source line a PC breakpoint sits on. Resolved on
    // demand rather than stored with the entry: the same program reloaded lands at a different
    // base, and a remembered file:line would then be confidently wrong.
    if (auto* bps = BreakpointsViewPtr()) {
        bps->SetSourceResolver([this](uint32_t addr, std::string& outFile, uint32_t& outLine) {
            const core::DebugLines* lines = GetActiveDebugInfo().lines;
            if (!lines || !lines->IsLoaded())
                return false;
            // Addresses in the UI are runtime; DWARF is link-time. Without the hunk map there
            // is no honest way between the two, so say nothing rather than guess.
            const auto link = m_compiledProgram.HunkMap().ToLink(addr);
            if (!link)
                return false;
            const auto src = lines->ResolveLine(*link);
            if (!src)
                return false;
            outFile = src->file;
            outLine = src->line;
            return true;
        });
    }

    core::GdbTargetOps ops;
    ops.isRunning = [this] { return m_emulatorView && m_emulatorView->IsRunning(); };
    // GdbServer observes the paused flag, it never owns it: EmulatorView remains the single
    // owner of m_isPaused and of the ArmDebugTrace() re-arm path.
    ops.isHalted = [this] { return m_emulatorView && m_emulatorView->IsPaused(); };

    ops.readRegs = [emu](core::GdbRegs& out) {
        if (!emu)
            return false;
        AmigaEmulator::CpuState st{};
        if (!emu->GetCpuState(st))
            return false;
        for (int i = 0; i < 8; ++i) {
            out.d[i] = st.d[i];
            out.a[i] = st.a[i];
        }
        out.pc = st.pc;
        out.sr = st.sr;
        return true;
    };

    ops.writeReg = [emu](int idx, uint32_t value) {
        if (!emu || !emu->HasDebugCommand())
            return false;
        // UAE's own debugger owns register writes; going through its console keeps one code
        // path rather than inventing a second way to poke the CPU.
        std::string name;
        if (idx >= 0 && idx < 8)
            name = "d" + std::to_string(idx);
        else if (idx >= 8 && idx < 15)
            name = "a" + std::to_string(idx - 8);
        else if (idx == 15)
            name = "a7";
        else if (idx == 16)
            name = "sr";
        else if (idx == 17)
            name = "pc";
        else
            return false;
        std::string out;
        return emu->ExecuteDebugCommand(fmt::format("r {} {:x}", name, value), out);
    };

    ops.readMem = [emu](uint32_t addr, uint8_t* out, size_t len, std::vector<bool>* valid) {
        core::ArMemoryAccess mem;
        ui::BuildEmulatorMemoryAccess(mem, emu);
        // ArMemoryAccess flags unmapped bytes rather than filling them, which is exactly what
        // lets an 'm' packet answer E01 instead of handing GDB invented zeroes.
        return mem.Read(addr, out, len, valid);
    };

    ops.writeMem = [emu](uint32_t addr, const uint8_t* data, size_t len, std::string& err) {
        core::ArMemoryAccess mem;
        ui::BuildEmulatorMemoryAccess(mem, emu);
        return mem.Write(addr, data, len, err);
    };

    // --- reverse execution ------------------------------------------------------------
    ops.canReverse = []() {
        // DISABLED, deliberately. The plumbing below is complete and unit-tested, but
        // end-to-end reverse execution does not yet reproduce history: `reverse-stepi` from a
        // jsr lands on the jsr's target -- one instruction forward -- and the machine comes
        // back with the uaelib gate unmapped, so the restore is not putting it where the trace
        // says it was.
        //
        // The divergence check that should have caught this is silently inert: after a frame
        // restore the trace lookup for that frame returns "unknown", which disables the check
        // rather than failing it. Both need fixing together, and the check has to be the thing
        // that is fixed first, because until it works every other result here is unverifiable.
        //
        // Advertising ReverseStep+ meanwhile would put live buttons in VS Code that silently
        // return the wrong machine state, which is precisely the failure mode this project
        // exists to remove. Flip this back on only once a reverse step is shown to land on the
        // instruction the trace recorded.
        return false;
    };
    ops.reverseStep = [this](std::string& outError) {
        if (!m_debugSession.PrepareTimeTravel(outError))
            return false;
        return m_debugSession.StepBack(outError);
    };
    ops.reverseContinue = [this](std::string& outError) {
        if (!m_debugSession.PrepareTimeTravel(outError))
            return false;
        std::vector<uint32_t> addresses;
        if (auto* bps = BreakpointsViewPtr()) {
            for (const auto& entry : bps->GetEntries()) {
                if (entry.type != "PC" || !entry.enabled)
                    continue;
                if (auto addr = core::ParseHexAddress(entry.address))
                    addresses.push_back(*addr);
            }
        }
        return m_debugSession.ReverseContinue(addresses, outError);
    };

    // --- breakpoints ------------------------------------------------------------------
    // Routed through BreakpointsView rather than straight to UAE, so a breakpoint GDB sets also
    // appears in Peekaboo's Breakpoints window and can be removed from either side without the
    // two views disagreeing.
    ops.addBreakpoint = [this](uint32_t addr) {
        auto* bps = BreakpointsViewPtr();
        if (!bps)
            return false;
        const bool added = bps->AddBreakpointAt(addr, "PC");
        // Tag it either way: gdb re-inserts on every resume, and the second insert finds the
        // entry already there. Losing the tag then would make a GDB breakpoint look like one
        // the user set by hand.
        bps->SetBreakpointOrigin(addr, ui::BreakpointsView::kOriginGdb);
        return added;
    };
    ops.removeBreakpoint = [this](uint32_t addr) {
        auto* bps = BreakpointsViewPtr();
        return bps && bps->RemoveEntryAt(addr, "PC");
    };
    ops.addWatchpoint = [this](const core::WatchpointSpec& spec, std::string& outError) {
        auto* bps = BreakpointsViewPtr();
        if (!bps) {
            outError = "no breakpoints view";
            return -1;
        }
        return bps->AddWatchpoint(spec, outError);
    };
    ops.removeWatchpoint = [this](int slot) {
        return BreakpointsViewPtr() && BreakpointsViewPtr()->RemoveWatchpointSlot(slot);
    };

    // --- execution --------------------------------------------------------------------
    // ArmDebugTrace before unpausing is not optional: it sets adm_skip_bp_check so the
    // instruction sitting under a breakpoint executes instead of re-triggering it immediately.
    ops.cont = [this, emu]() {
        if (emu && emu->HasArmDebug())
            emu->ArmDebugTrace();
        if (m_emulatorView && m_emulatorView->IsPaused())
            m_emulatorView->TogglePause();
    };
    ops.stepOne = [this]() {
        if (m_emulatorView)
            m_emulatorView->StepInstruction();
    };
    ops.interrupt = [this]() {
        if (m_emulatorView && m_emulatorView->IsRunning() && !m_emulatorView->IsPaused())
            m_emulatorView->TogglePause();
    };
    ops.stepRange = [this, emu](uint32_t lo, uint32_t hi) {
        if (!m_emulatorView || !emu)
            return false;
        // GDB would otherwise single-step this whole range one packet at a time. On -Ofast code
        // a source-level `next` can span thousands of instructions, so stepping locally until
        // the PC leaves [lo, hi) removes a round-trip per instruction.
        constexpr int kMaxSteps = 200000;
        for (int i = 0; i < kMaxSteps; ++i) {
            AmigaEmulator::CpuState cpu{};
            if (!emu->GetCpuState(cpu))
                return false;
            if (cpu.pc < lo || cpu.pc >= hi)
                return true;
            m_emulatorView->StepInstruction();
            // A breakpoint or watchpoint firing mid-range is a real stop and outranks the
            // range.
            if (emu->IsDebugBreakPending())
                return true;
        }
        // Ran out of budget: report where we are rather than looping forever.
        return true;
    };

    // Relocate the section map against the running process, then answer with whatever GDB
    // needs to place symbols. Done on demand rather than per-frame: the seglist only changes
    // when a program is (re)loaded, and walking it costs a handful of emulated-memory reads.
    ops.monitor = [this, emu](const std::string& command) -> std::string {
        std::string cmd = command;
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' '))
            cmd.pop_back();

        if (cmd == "sections" || cmd == "offsets") {
            if (!m_compiledProgram.HunkMap().IsLoaded())
                return "no section map: launch through Peekaboo's VS Code target so the ELF "
                       "is known\n";
            std::string err;
            if (!m_compiledProgram.RelocateHunkMap(err))
                return "not relocated: " + err + "\n";
            // "offsets" gives the add-symbol-file arguments verbatim, so the adapter can paste
            // them straight into a command; "sections" is the human-readable table.
            if (cmd == "offsets")
                return m_compiledProgram.HunkMap().AddSymbolFileArgs() + "\n";
            return m_compiledProgram.HunkMap().DescribeSections();
        }
        if (cmd.rfind("resolve-line ", 0) == 0) {
            // monitor resolve-line <file>:<line> -- the same audit the MCP tool performs, over
            // the socket the debugger is already attached to.
            const std::string arg = cmd.substr(13);
            const size_t colon = arg.rfind(':');
            if (colon == std::string::npos || colon + 1 >= arg.size())
                return "usage: monitor resolve-line <file>:<line>\n";
            const std::string file = arg.substr(0, colon);
            uint32_t line = 0;
            try {
                line = static_cast<uint32_t>(std::stoul(arg.substr(colon + 1)));
            } catch (...) {
                return "usage: monitor resolve-line <file>:<line>\n";
            }
            const core::DebugLines* lines = GetActiveDebugInfo().lines;
            if (!lines || !lines->IsLoaded())
                return "no debug line table is loaded\n";

            const core::LineLookup lookup = lines->ResolveLineToAddresses(file, line);
            std::string out;
            switch (lookup.status) {
                case core::LineLookupStatus::Exact:
                    out =
                        fmt::format("line {} has code at {} site(s):\n", line, lookup.sites.size());
                    break;
                case core::LineLookupStatus::MovedForward:
                    out = fmt::format("line {} generated no code; nearest is line {}:\n", line,
                                      lookup.resolvedLine);
                    break;
                case core::LineLookupStatus::NoCode:
                    out = fmt::format("line {} generated no code\n", line);
                    break;
                case core::LineLookupStatus::FileUnknown:
                    return fmt::format("no file matching \"{}\" in this build\n", file);
                case core::LineLookupStatus::FileNotCovered:
                    return fmt::format("\"{}\" has no line coverage in this build\n", file);
            }
            for (const core::LineSite& site : lookup.sites) {
                std::string runtime;
                if (auto rt = m_compiledProgram.HunkMap().ToRuntime(site.lo))
                    runtime = fmt::format("  runtime ${:08X}", *rt);
                out += fmt::format("  ${:08X}-${:08X}{}{}\n", site.lo, site.hi, runtime,
                                   site.isStmt ? "  (stmt)" : "");
            }
            if (!lookup.nearbyLines.empty() && lookup.sites.empty()) {
                out += "  lines with code nearby:";
                for (uint32_t n : lookup.nearbyLines)
                    out += fmt::format(" {}", n);
                out += "\n";
            }
            return out;
        }
        if (cmd.rfind("line-coverage ", 0) == 0) {
            // monitor line-coverage <file>:<line> [frame]
            std::string arg = cmd.substr(14);
            uint64_t frame = 0;
            if (const size_t sp = arg.rfind(' '); sp != std::string::npos) {
                try {
                    frame = std::stoull(arg.substr(sp + 1));
                    arg = arg.substr(0, sp);
                } catch (...) {
                    // no trailing frame number; the whole argument is the location
                }
            }
            const size_t colon = arg.rfind(':');
            if (colon == std::string::npos || colon + 1 >= arg.size())
                return "usage: monitor line-coverage <file>:<line> [frame]\n";
            uint32_t line = 0;
            try {
                line = static_cast<uint32_t>(std::stoul(arg.substr(colon + 1)));
            } catch (...) {
                return "usage: monitor line-coverage <file>:<line> [frame]\n";
            }
            if (!m_mcpHandler)
                return "line coverage is unavailable\n";

            const nlohmann::json r = m_mcpHandler->Handle(
                "line_coverage",
                nlohmann::json{{"file", arg.substr(0, colon)}, {"line", line}, {"frame", frame}});
            if (r.contains("error"))
                return r["error"].get<std::string>() + "\n";

            std::string out =
                fmt::format("line {}: {} hit(s) across {} of {} stretch(es)\n",
                            r.value("requestedLine", 0u), r.value("totalHits", 0ull),
                            r.value("executedSites", 0ull), r.value("siteCount", 0ull));
            for (const auto& site : r.value("sites", nlohmann::json::array())) {
                out += fmt::format("  ${}-${}  hits {}{}", site.value("lo", std::string{}),
                                   site.value("hi", std::string{}), site.value("hits", 0ull),
                                   site.value("isStmt", false) ? "  (stmt)" : "");
                if (site.contains("inlineStack")) {
                    out += "  [";
                    bool first = true;
                    for (const auto& fn : site["inlineStack"]) {
                        if (!first)
                            out += " > ";
                        out += fn.get<std::string>();
                        first = false;
                    }
                    out += "]";
                }
                out += "\n";
            }
            if (r.contains("hint"))
                out += "  note: " + r["hint"].get<std::string>() + "\n";
            return out;
        }
        if (cmd == "reset") {
            if (auto* e = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr) {
                e->Reset();
                return "reset\n";
            }
            return "no emulator\n";
        }
        return "unknown monitor command: " + cmd +
               "\nsupported: sections, offsets, resolve-line <file>:<line>, "
               "line-coverage <file>:<line> [frame], reset\n";
    };

    m_gdbServer.SetTargetOps(std::move(ops));

    const auto& prefs = m_userPrefs.Get();
    const auto& launch = GetLaunchOptions();
    if (prefs.gdbEnabled || launch.gdbPort) {
        if (!m_gdbServer.Start(launch.gdbPort.value_or(prefs.gdbPort)))
            Logger::Warn("GdbServer: could not start - {}", m_gdbServer.LastError());
    }
}

void Application::SetupMcpServer()
{
    auto* view = m_emulatorView.get();
    auto emulator = [view]() -> adm::AmigaEmulator* {
        return view ? view->GetEmulator() : nullptr;
    };

    core::McpHost host;
    host.appVersion = std::string(adm::core::constants::AppVersion);
    host.emulator = emulator;
    host.annotations = m_debuggerView->GetAnnotations();

    // Execution control + breakpoint/watchpoint hooks are shared verbatim with the AR
    // console host (see EmulatorHostHooks.h); MCP-specific hooks follow.
    ui::WireEmulatorHostHooks(host, view, m_debuggerView->GetBreakpointsView());

    host.stepFrame = [view]() {
        if (view)
            view->StepFrame();
    };
    host.getFrameCount = [emulator]() -> uint64_t {
        auto* emu = emulator();
        return emu ? emu->GetTotalFrameCount() : 0;
    };

    // Full watchpoint API (range / read / execute / log mode). Only McpHost carries these,
    // so they are wired here rather than in the shared WireEmulatorHostHooks template.
    host.addWatchpointFull = [this](const core::WatchpointSpec& spec,
                                    std::string& outError) -> int {
        auto* bps = m_debuggerView->GetBreakpointsView();
        if (!bps) {
            outError = "breakpoints window unavailable";
            return -1;
        }
        return bps->AddWatchpoint(spec, outError);
    };
    host.removeWatchpointSlot = [this](int slot) {
        auto* bps = m_debuggerView->GetBreakpointsView();
        return bps && bps->RemoveWatchpointSlot(slot);
    };
    host.listWatchpointsFull = [this]() {
        auto* bps = m_debuggerView->GetBreakpointsView();
        return bps ? bps->ListWatchpoints() : std::vector<core::WatchpointInfo>{};
    };
    host.getWatchpointLog = [this](int slot, int limit, bool clear) {
        auto* bps = m_debuggerView->GetBreakpointsView();
        return bps ? bps->GetWatchpointLog(slot, limit, clear) : std::vector<core::WatchpointHit>{};
    };

    host.states = &m_stateStore;
    host.activeDocumentPath = [this]() -> std::string {
        auto* doc = GetActiveDocument();
        return doc ? doc->currentFilePath : std::string{};
    };
    host.refreshBreakpoints = [this]() {
        if (auto* bps = m_debuggerView->GetBreakpointsView())
            bps->MarkNeedsRefresh();
    };

    host.execConsole = [this](const std::string& cmd) -> std::string {
        if (auto* console = m_debuggerView->GetConsoleView())
            return console->RunCommandCapture(cmd);
        return "console unavailable";
    };

    host.getProfile = [this](const core::ProfileQuery& q) {
        return m_debuggerView->GetProfileSnapshot(q);
    };

    host.getBlitLog = [this](int limit) { return m_logSnapshots.MakeBlitLogSnapshot(limit); };

    host.getCrassProfile = [this]() -> core::CrassProfileStats {
        auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
        return emu ? emu->GetCrassProfileStats() : core::CrassProfileStats{};
    };

    host.exocetSession = [this]() { return m_exocet.get(); };
    host.getExocet = [this]() {
        core::ExocetSnapshot s;
        if (auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr)
            s.block = emu->GetExocetBlock();
        if (auto* doc = GetActiveDocument()) {
            s.pack = doc->exocet;
            s.packError = doc->exocetError;
            s.documentPath = doc->currentFilePath;
        }
        if (m_exocet && m_exocet->Activity().Rows())
            s.activity = m_exocet->Activity().ToJson();
        return s;
    };

    host.getExocetMemory = [this]() {
        auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
        return emu ? emu->GetExocetMemory() : core::ExocetMemoryState{};
    };

    host.getFrameHistory = [this]() { return m_timeline.Stats(); };
    host.getFrameHistoryPosition = [this]() { return m_timeline.Position(); };
    host.frameHistorySeek = [this](uint64_t frame) { return SeekToRecordedFrame(frame); };
    host.frameHistoryRewind = [this](uint64_t count) { return RewindFrames(count); };
    host.frameHistoryLoop = [this](uint64_t a, uint64_t b) { return SetFrameLoop(a, b); };
    host.frameHistoryClear = [this]() { ClearFrameTimeline(); };
    host.frameHistoryPlay = [this](bool playing) { SetTimelinePlaying(playing); };
    host.frameHistoryStep = [this](int delta) { return StepTimeline(delta); };
    host.frameHistoryPlayMode = [this](int mode) {
        SetTimelinePlayMode(static_cast<TimelinePlayMode>(mode));
    };
    host.frameHistoryPlaySpeed = [this](double speed) { SetTimelineSpeed(speed); };
    host.frameHistoryPlayback = [this]() {
        const TimelinePlayback tp = GetTimelinePlayback();
        core::McpHost::FrameHistoryPlayback pb;
        pb.playing = tp.playing;
        pb.mode = static_cast<int>(tp.mode);
        pb.speed = tp.speed;
        pb.looping = tp.looping;
        pb.loopStart = tp.loopStart;
        pb.loopEnd = tp.loopEnd;
        return pb;
    };
    host.frameHistoryDiff = [this](uint64_t a, uint64_t b, std::vector<core::FrameDiffRun>& out,
                                   size_t maxRuns, bool& truncated) {
        return DiffRecordedFrames(a, b, out, maxRuns, truncated);
    };
    host.getRegLog = [this](int limit) { return m_logSnapshots.MakeRegLogSnapshot(limit); };
    host.getAudioStats = [this] { return MakeAudioLogSnapshot(); };
    host.getPaulaState = [this] { return m_debuggerView->BuildPaulaSnapshot(); };
    host.audioRecord = [this](const core::AudioRecordRequest& r) {
        return ApplyAudioRecordRequest(r);
    };
    host.getDiskLog = [this](const core::DiskLogQuery& q) {
        return m_logSnapshots.MakeDiskLogSnapshot(q);
    };
    host.armDiskBreak = [this](int access, int cyl, int side, int sector, uint32_t addrLo,
                               uint32_t addrHi) {
        return ArmDiskBreak(access, cyl, side, sector, addrLo, addrHi);
    };

    host.loadDocument = [this](const std::string& path, bool force, bool reloadIfOpen) {
        return LoadDocumentExternal(path, force, reloadIfOpen);
    };
    host.listDocuments = [this]() {
        std::vector<std::string> paths;
        paths.reserve(m_documents.size());
        for (const auto& d : m_documents)
            paths.push_back(d->currentFilePath);
        return paths;
    };
    host.runDocument = [this](int idx) {
        if (idx < 0 || idx >= static_cast<int>(m_documents.size()))
            return;
        SwitchActiveDocument(m_documents[static_cast<size_t>(idx)].get());
        RunAdf();
    };
    host.closeDocument = [this](int idx, bool force) -> bool {
        if (idx < 0 || idx >= static_cast<int>(m_documents.size()))
            return false;
        auto* doc = m_documents[static_cast<size_t>(idx)].get();
        if (doc->HasUnsavedChanges() && !force)
            return false;
        QueueCloseDocument(doc);
        return true;
    };

    // --- Reverse-engineering tools -----------------------------------------
    // These share the stores Application owns, so the console, the UI and MCP
    // all see the same snapshots, history and breakpoint rules.
    host.memorySnapshots = &m_memorySnapshots;
    host.cpuTraceHistory = &m_cpuTraceHistory;
    host.libraryTracer = &m_libraryTracer;
    host.dosBreakpoints = &m_dosBreakpoints;

    // LVO names come from the generated table the disassembly annotator already
    // uses; the argument metadata is a hand-maintained companion. Both are
    // optional — without them calls are reported by offset and without decoded
    // arguments, which is worth saying out loud rather than failing silently.
    if (!m_libraryTracer.LoadNameTables())
        Logger::Warn("MCP: assets/amiga_lvos.json not loaded — trace_library_calls will "
                     "only see functions listed in amiga_lvo_args.json");
    if (!m_libraryTracer.LoadArgTables())
        Logger::Warn("MCP: assets/amiga_lvo_args.json not loaded — library calls will trace "
                     "without decoded arguments");

    host.execReaders = [this]() { return MakeExecReaders(); };
    host.memoryRegions = [this]() {
        std::vector<core::AddressRegion> out;
        if (auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr)
            for (const auto& r : emu->GetMemoryRegions())
                out.push_back({r.amigaAddr, r.size, r.name, r.writable});
        return out;
    };
    host.liveMemoryRegions = [this]() {
        std::vector<core::LiveMemoryRegion> out;
        if (auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr)
            for (const auto& r : emu->GetMemoryRegions())
                out.push_back({r.amigaAddr, r.size, r.hostPtr, r.name, r.writable});
        return out;
    };
    host.resolveSymbol = [this](uint32_t linkAddr, std::string& name, uint32_t& base,
                                uint32_t& size) {
        const core::DebugSymbols* syms = GetActiveDebugInfo().symbols;
        if (!syms)
            return false;
        const auto* fn = syms->ResolvePc(linkAddr);
        if (!fn)
            return false;
        name = fn->name;
        base = fn->address;
        size = fn->size;
        return true;
    };
    // Where a source line's code actually is. Reported with both link-time and relocated
    // addresses: the first is what the DWARF says, the second is where the running program has
    // it, and a caller auditing a breakpoint needs the second.
    host.resolveSourceLine = [this](const std::string& file, uint32_t line,
                                    bool stmtOnly) -> nlohmann::json {
        const core::DebugLines* lines = GetActiveDebugInfo().lines;
        if (!lines || !lines->IsLoaded())
            return nlohmann::json{{"error", "no debug line table is loaded"}};

        if (!m_compiledProgram.HunkMap().IsRelocated()) {
            std::string ignored;
            m_compiledProgram.RelocateHunkMap(ignored);
        }
        const core::LineLookup lookup = lines->ResolveLineToAddresses(file, line, stmtOnly);
        const char* status = "none";
        switch (lookup.status) {
            case core::LineLookupStatus::Exact:
                status = "exact";
                break;
            case core::LineLookupStatus::MovedForward:
                status = "moved";
                break;
            case core::LineLookupStatus::NoCode:
                status = "none";
                break;
            case core::LineLookupStatus::FileUnknown:
                status = "file_unknown";
                break;
            case core::LineLookupStatus::FileNotCovered:
                status = "file_not_covered";
                break;
        }

        nlohmann::json sites = nlohmann::json::array();
        for (const core::LineSite& site : lookup.sites) {
            nlohmann::json entry{{"lo", fmt::format("{:08X}", site.lo)},
                                 {"hi", fmt::format("{:08X}", site.hi)},
                                 {"line", site.line},
                                 {"isStmt", site.isStmt}};
            if (auto runtimeLo = m_compiledProgram.HunkMap().ToRuntime(site.lo))
                entry["runtimeLo"] = fmt::format("{:08X}", *runtimeLo);
            if (auto runtimeHi = m_compiledProgram.HunkMap().ToRuntime(site.hi - 1))
                entry["runtimeHi"] = fmt::format("{:08X}", *runtimeHi + 1);
            // Name the function, and the inline chain when the line was inlined -- that chain
            // is exactly the context GDB's "contains no code" message leaves out.
            if (const core::DebugSymbols* syms = GetActiveDebugInfo().symbols) {
                if (const auto* fn = syms->ResolvePc(site.lo))
                    entry["function"] = fn->name;
            }
            if (const core::DebugInline* inl = GetActiveDebugInfo().inlines) {
                const auto chain = inl->ResolveInlineStack(site.lo);
                if (chain.size() > 1)
                    entry["inlineStack"] = chain;
            }
            sites.push_back(std::move(entry));
        }

        nlohmann::json out{{"status", status},
                           {"requestedLine", lookup.requestedLine},
                           {"resolvedLine", lookup.resolvedLine},
                           {"sites", std::move(sites)},
                           {"relocated", m_compiledProgram.HunkMap().IsRelocated()}};
        if (!lookup.nearbyLines.empty())
            out["nearbyLines"] = lookup.nearbyLines;
        return out;
    };

    host.lineCoverage = [this](const std::string& file, uint32_t line,
                               uint64_t frame) -> nlohmann::json {
        // Relocate on demand. Coverage is meaningless without runtime addresses, and requiring
        // the caller to have run `monitor sections` first would make it fail for no reason a
        // user could see. Failure is fine and reported downstream as "not relocated".
        if (!m_compiledProgram.HunkMap().IsRelocated()) {
            std::string ignored;
            m_compiledProgram.RelocateHunkMap(ignored);
        }
        const auto info = GetActiveDebugInfo();
        core::LineCoverage::Inputs in;
        in.lines = info.lines;
        in.inlines = info.inlines;
        in.hunkMap = &m_compiledProgram.HunkMap();
        in.history = &m_cpuTraceHistory;

        const core::LineCoverageResult r = core::LineCoverage::Query(in, file, line, frame);

        const char* status = "none";
        switch (r.status) {
            case core::LineLookupStatus::Exact:
                status = "exact";
                break;
            case core::LineLookupStatus::MovedForward:
                status = "moved";
                break;
            case core::LineLookupStatus::NoCode:
                status = "none";
                break;
            case core::LineLookupStatus::FileUnknown:
                status = "file_unknown";
                break;
            case core::LineLookupStatus::FileNotCovered:
                status = "file_not_covered";
                break;
        }

        nlohmann::json sites = nlohmann::json::array();
        for (const core::CoveredSite& site : r.sites) {
            nlohmann::json entry{{"lo", fmt::format("{:08X}", site.lo)},
                                 {"hi", fmt::format("{:08X}", site.hi)},
                                 {"isStmt", site.isStmt},
                                 {"hits", site.hits}};
            if (site.runtimeLo)
                entry["runtimeLo"] = fmt::format("{:08X}", site.runtimeLo);
            if (site.runtimeHi)
                entry["runtimeHi"] = fmt::format("{:08X}", site.runtimeHi);
            if (!site.inlineStack.empty())
                entry["inlineStack"] = site.inlineStack;
            if (site.haveSample) {
                entry["firstSeq"] = site.firstSeq;
                entry["lastSeq"] = site.lastSeq;
                entry["firstFrame"] = site.firstFrame;
                entry["firstLRef"] = site.firstLRef;
                nlohmann::json regs;
                for (int i = 0; i < 8; ++i) {
                    regs[fmt::format("d{}", i)] = fmt::format("{:08X}", site.d[i]);
                    regs[fmt::format("a{}", i)] = fmt::format("{:08X}", site.a[i]);
                }
                regs["sr"] = fmt::format("{:04X}", site.sr);
                entry["registersAtFirstHit"] = std::move(regs);
            }
            sites.push_back(std::move(entry));
        }

        nlohmann::json out{{"available", r.available},         {"status", status},
                           {"requestedLine", r.requestedLine}, {"resolvedLine", r.resolvedLine},
                           {"relocated", r.relocated},         {"totalHits", r.totalHits},
                           {"executedSites", r.executedSites}, {"siteCount", r.sites.size()},
                           {"sites", std::move(sites)}};
        if (!r.hint.empty())
            out["hint"] = r.hint;
        return out;
    };

    host.debugSymbolsInfo = [this]() {
        core::DebugSymbolsInfo info;
        if (const core::DebugSymbols* syms = GetActiveDebugInfo().symbols) {
            info.loaded = true;
            info.path = syms->SourcePath();
            info.functionCount = syms->FunctionCount();
        }
        return info;
    };
    host.customRegName = [this](uint32_t offset) -> std::string {
        auto* console = m_debuggerView ? m_debuggerView->GetConsoleView() : nullptr;
        return console ? console->CustomRegisterName(offset) : std::string{};
    };
    host.customRegDesc = [this](uint32_t offset) -> std::string {
        auto* console = m_debuggerView ? m_debuggerView->GetConsoleView() : nullptr;
        return console ? console->CustomRegisterDescription(offset) : std::string{};
    };
    host.activeDiskDocument = [this]() {
        core::DiskDocumentAccess access;
        if (auto* doc = GetActiveDocument()) {
            access.volume = &doc->volume;
            access.searcher = doc->diskSearcher.get();
            access.path = doc->currentFilePath;
            access.volumeName =
                doc->volume.IsMounted() ? doc->volume.GetDiskInfo().name : std::string{};
        }
        return access;
    };

    // Session control (mirror the AR console START/STOP/EF commands).
    host.startEmulator = [this]() { RunAdf(); };
    host.stopEmulator = [view]() {
        if (view && view->IsRunning())
            view->Stop();
    };
    host.endFrame = [view]() {
        if (view)
            view->EndFrame();
    };
    host.onStateChanged = [this]() {
        if (m_debuggerView)
            m_debuggerView->MarkMemoryRegionsDirty();
    };

    // --- Peekaboo's own UI ---------------------------------------------------
    // These reach the host application rather than the guest. All of them run on the main
    // thread inside McpServer::DrainPending, which is called at the top of RunFrame - before
    // any window has been submitted this frame. That is exactly when ImGui dock state may be
    // mutated, and exactly when the default framebuffer must NOT be read (hence the deferred
    // AppCapture).

    host.getUiLayout = [this]() { return ui::BuildLayoutSnapshot(MakeUiSources()); };

    host.setWindowOpen = [this](const std::string& window, bool open, std::string& err) {
        const ui::UiSources src = MakeUiSources();
        for (const auto& e : src.windows) {
            if (e.name != window && e.imguiId != window)
                continue;
            if (!e.openPtr) {
                err = "'" + window + "' has no open flag";
                return false;
            }
            *e.openPtr = open;
            return true;
        }
        for (const auto& p : src.panels) {
            if (p.name != window)
                continue;
            if (p.permanent && !open) {
                err = "panel '" + window + "' is permanent and cannot be closed";
                return false;
            }
            if (p.openPtr)
                *p.openPtr = open;
            return true;
        }
        err = "unknown window: '" + window + "' (call list_windows for the addressable names)";
        return false;
    };

    host.focusWindow = [this](const std::string& window, std::string& err) {
        const std::string id = ResolveUiWindowId(MakeUiSources(), window, err);
        if (id.empty())
            return false;
        ImGui::SetWindowFocus(id.c_str());
        ui::FocusContainingPanels(id.c_str());
        return true;
    };

    host.dockWindow = [this](const core::UiDockRequest& req, std::string& err) {
        return ui::ApplyDockRequest(req, MakeUiSources(), err);
    };

    host.requestAppCapture = [this](const core::UiCaptureRequest& req, std::string& err) {
        if (!m_appCapture.Request(req)) {
            err = "a capture is already in flight";
            return false;
        }
        return true;
    };
    host.pollAppCapture = [this]() { return m_appCapture.Take(); };

    host.windowRect = [this](const std::string& window, core::UiCaptureRequest& out,
                             std::string& err) {
        const std::string id = ResolveUiWindowId(MakeUiSources(), window, err);
        if (id.empty())
            return false;
        if (!ui::FindWindowRect(id.c_str(), out.x, out.y, out.w, out.h)) {
            err = "'" + window +
                  "' has never been submitted, so it has no position yet; open_window it first";
            return false;
        }
        out.wholeApp = false;
        return true;
    };

    host.listWorkspaces = [this]() {
        std::vector<std::string> names;
        for (const auto& ws : m_workspaceManager.GetWorkspaces())
            names.push_back(ws.name);
        return names;
    };
    host.getScreenProfile = [this]() { return std::string(ScreenProfileKey(GetScreenProfile())); };
    host.setScreenProfile = [this](const std::string& key, std::string& err) {
        const auto profile = ParseScreenProfile(key);
        if (!profile) {
            err = "unknown screen profile: '" + key + "' (hires or lores)";
            return false;
        }
        SetScreenProfile(*profile);
        return true;
    };
    host.workspaceHasOwnLayout = [this](const std::string& name) {
        return m_workspaceManager.HasLayout(name, GetScreenProfile());
    };
    host.loadWorkspace = [this](const std::string& name, std::string& err) {
        if (!m_workspaceManager.FindWorkspace(name)) {
            err = "unknown workspace: '" + name + "'";
            return false;
        }
        LoadWorkspace(name);
        return true;
    };
    host.saveWorkspace = [this](const std::string& name, std::string& err) {
        if (name.empty()) {
            err = "workspace name must not be empty";
            return false;
        }
        SaveWorkspace(name);
        return true;
    };

    host.getUiLog = [this](int limit, const std::string& level) {
        core::UiLogSnapshot snap;
        if (m_logView)
            snap = m_logView->Snapshot(limit, level);
        return snap;
    };
    host.getImguiIni = []() { return ui::SaveImGuiIni(); };

    host.injectUiInput = [this](const core::UiInputEvent& ev, std::string& err) {
        core::UiInputEvent resolved = ev;
        if (resolved.kind == core::UiInputEvent::Kind::Key &&
            !ui::ParseImGuiKeyName(resolved.keyName, resolved.key)) {
            err = "unknown key name: '" + resolved.keyName +
                  "'; use ImGui names such as F5, Enter, Escape, LeftArrow, Keypad0";
            return false;
        }
        m_uiInput.Push(resolved);
        return true;
    };
    host.uiInputBusy = [this]() { return m_uiInput.Busy(); };

    m_mcpHandler = std::make_unique<core::McpToolHandler>(std::move(host));
    m_mcpActivityView = std::make_unique<ui::McpActivityView>(m_mcpHandler.get());
    m_mcpActivityView->Restore(m_userPrefs.Get().showMcpActivity);

    core::McpToolHandler* handler = m_mcpHandler.get();
    m_mcpServer.SetManifestProvider([handler]() { return handler->Manifest(); });

    const auto& prefs = m_userPrefs.Get();
    const auto& launch = GetLaunchOptions();
    // A port or token given on the command line is this launch's only: it is not written back.
    if (prefs.mcpEnabled || launch.mcpPort || launch.mcpToken) {
        const int port = launch.mcpPort.value_or(prefs.mcpPort);
        std::string token = launch.mcpToken.value_or(prefs.mcpToken);
        if (token.empty()) {
            token = GenerateMcpToken();
            if (token.empty()) {
                // The server rejects every request without a token (fail closed), so starting
                // it would only produce confusing 401s.
                Logger::Error("MCP: server not started — no auth token available");
                return;
            }
            if (!launch.mcpPort)
                m_userPrefs.SetMcpSettings(true, prefs.mcpPort, token);
        }
        m_mcpServer.Start(port, token);
    }
}

} // namespace core
} // namespace adm
