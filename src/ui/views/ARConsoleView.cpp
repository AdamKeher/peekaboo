#include "ARConsoleView.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <fmt/format.h>
#include <nfd.h>

#include "../../core/AmigaEmulator.h"
#include "../../core/Constants.h"
#include "../../core/CopperDisassembler.h"
#include "../../core/HexUtil.h"
#include "../../core/Logger.h"
#include "../../core/MemoryAnnotations.h"
#include "../../core/UaeAssembler.h"
#include "../AddressLink.h"
#include "../IconsFontAwesome6.h"
#include "AddressMenuActions.h"
#include "BreakpointsView.h"
#include "EmulatorHostHooks.h"
#include "EmulatorView.h"

namespace adm::ui {

namespace {

using core::ParseHexAddress;

} // namespace

ARConsoleView::ARConsoleView(EmulatorView* emulatorView, BreakpointsView* breakpointsView,
                             core::MemoryAnnotations* annotations, core::MemoryLocks* locks)
    : m_emulatorView(emulatorView), m_breakpointsView(breakpointsView), m_annotations(annotations),
      m_locks(locks), m_disassembler([emulatorView](uint32_t pc, char* mnemo, int mnemoSz,
                                                    char* hex, int hexSz, uint32_t& nextPc) {
          auto* emu = emulatorView ? emulatorView->GetEmulator() : nullptr;
          if (!emu)
              return false;
          return emu->Disassemble(pc, mnemo, mnemoSz, hex, hexSz, nextPc);
      })
{
    LoadCustomRegisterNames();
    BuildInterpreter();

    m_terminal.onReturn = [this](int row, const std::string& line) { HandleReturn(row, line); };
    m_terminal.onEscape = [this]() {
        const bool wasActive = m_interp->GetMode() != core::ArCommandInterpreter::Mode::None;
        m_interp->CancelInteractive();
        if (wasActive)
            m_terminal.CursorToClearedLine(m_terminal.WritePos());
    };
    m_terminal.onAddressClicked = [this](uint32_t addr) {
        if (m_addrCtx && m_addrCtx->valid())
            RunAddressDefaultAction(addr, *m_addrCtx, AddressLinkHost::Console,
                                    AddressDefault::Memory);
    };
    m_terminal.onAddressContextMenu = [this](uint32_t addr) {
        m_ctxAddr = addr;
        m_openAddrCtx = true;
    };
    m_terminal.onCustomRegClicked = [this](uint32_t offset) {
        if (m_onJumpToCustomReg)
            m_onJumpToCustomReg(offset);
    };

    PrintBanner();
}

void ARConsoleView::LoadCustomRegisterNames()
{
    try {
        std::ifstream f("assets/custom_registers.json");
        if (!f.is_open())
            return;
        m_customRegsDef = nlohmann::json::parse(f, nullptr, false);
        if (!m_customRegsDef.is_object())
            return;
        for (auto it = m_customRegsDef.begin(); it != m_customRegsDef.end(); ++it) {
            if (!it.value().is_object() || !it.value().contains("name") ||
                !it.value()["name"].is_string())
                continue;
            std::string key = it.key();
            if (!key.empty() && key[0] == '$')
                key.erase(0, 1);
            else if (key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X'))
                key.erase(0, 2);
            auto offset = ParseHexAddress(key);
            if (!offset)
                continue;
            uint32_t off = *offset & 0x1FE;
            const auto& v = it.value();
            m_customRegNames[off] = v["name"].get<std::string>();
            if (v.contains("full_name") && v["full_name"].is_string())
                m_customRegDesc[off] = v["full_name"].get<std::string>();
            else if (v.contains("description") && v["description"].is_string())
                m_customRegDesc[off] = v["description"].get<std::string>();
            if (v.contains("doc_name") && v["doc_name"].is_string())
                m_customRegDocName[off] = v["doc_name"].get<std::string>();
        }
    } catch (...) {
        core::Logger::Warn("ARConsole: failed to parse assets/custom_registers.json — register "
                           "names unavailable");
    }

    // Bit-field tables (DCHIP) live in a separate doc file keyed by doc_name.
    try {
        std::ifstream df("assets/custom_registers_docs.json");
        if (df.is_open()) {
            m_customRegDocs = nlohmann::json::parse(df, nullptr, false);
            if (!m_customRegDocs.is_object())
                m_customRegDocs = nlohmann::json{};
        }
    } catch (...) {
        core::Logger::Warn("ARConsole: failed to parse assets/custom_registers_docs.json — "
                           "register docs unavailable");
    }
}

const std::vector<core::ArHost::RegBitField>& ARConsoleView::GetRegBits(uint32_t offset)
{
    offset &= 0x1FE;
    auto cached = m_customRegBitsCache.find(offset);
    if (cached != m_customRegBitsCache.end())
        return cached->second;

    std::vector<core::ArHost::RegBitField> bits;
    auto docIt = m_customRegDocName.find(offset);
    if (docIt != m_customRegDocName.end() && m_customRegDocs.is_object() &&
        m_customRegDocs.contains(docIt->second) && m_customRegDocs[docIt->second].is_string()) {
        const std::string doc = m_customRegDocs[docIt->second].get<std::string>();
        // Parse the markdown "| Bit | Function | ... |" table. Each data row is
        // "| 15 | NAME | ... |" or a range "| 14-13 | NAME | ... |".
        std::istringstream ss(doc);
        std::string line;
        bool inTable = false;
        while (std::getline(ss, line)) {
            // strip CR and surrounding whitespace
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line.front() != '|') {
                if (inTable)
                    break; // table ended (e.g. a "> Note" paragraph)
                continue;
            }
            // Split the row into trimmed cells between pipes.
            std::vector<std::string> cells;
            std::stringstream rs(line);
            std::string cell;
            while (std::getline(rs, cell, '|')) {
                size_t b = cell.find_first_not_of(" \t");
                size_t e = cell.find_last_not_of(" \t");
                cells.push_back(b == std::string::npos ? "" : cell.substr(b, e - b + 1));
            }
            // Drop empty leading/trailing cells produced by the outer pipes.
            if (!cells.empty() && cells.front().empty())
                cells.erase(cells.begin());
            if (!cells.empty() && cells.back().empty())
                cells.pop_back();
            if (cells.size() < 2)
                continue;
            const std::string& c0 = cells[0];
            if (c0 == "Bit") {
                inTable = true;
                continue;
            }
            if (!inTable || c0.find("---") != std::string::npos)
                continue;
            // Parse "<hi>" or "<hi>-<lo>".
            int hi = 0, lo = 0;
            try {
                size_t dash = c0.find('-');
                if (dash == std::string::npos) {
                    hi = lo = std::stoi(c0);
                } else {
                    hi = std::stoi(c0.substr(0, dash));
                    lo = std::stoi(c0.substr(dash + 1));
                }
            } catch (...) {
                break; // first cell isn't a bit spec -> end of table
            }
            if (hi < 0 || hi > 15 || lo < 0 || lo > 15)
                continue;
            if (lo > hi)
                std::swap(hi, lo);
            bits.push_back({hi, lo, cells[1]});
        }
    }
    auto [it, _] = m_customRegBitsCache.emplace(offset, std::move(bits));
    return it->second;
}

void ARConsoleView::BuildInterpreter()
{
    core::ArHost host;
    host.annotations = m_annotations;
    auto* view = m_emulatorView;
    auto* bps = m_breakpointsView;

    auto emulator = [view]() -> adm::AmigaEmulator* {
        return view ? view->GetEmulator() : nullptr;
    };

    host.locks = m_locks;
    // Seglist anchors need the running process's segment bases. Resolved on demand rather
    // than cached: a program that has not loaded yet has no seglist, and one that just did
    // must not be resolved against the previous program's bases.
    host.getSegments = [this]() -> std::vector<core::AmigaSegment> {
        if (!m_mem.HasRegions())
            return {};
        auto readLong = [this](uint32_t a, bool& ok) { return m_mem.ReadLong(a, &ok); };
        auto readByte = [this](uint32_t a, bool& ok) { return m_mem.ReadByte(a, &ok); };
        return core::SegListResolver::ResolveActiveProcess(readLong, readByte);
    };
    host.supportsProtections = [emulator]() {
        auto* emu = emulator();
        return emu && emu->SupportsProtections();
    };

    // Execution control + breakpoint/watchpoint hooks are shared verbatim with the
    // MCP host (see EmulatorHostHooks.h); the AR-console-specific hooks follow.
    WireEmulatorHostHooks(host, view, bps);

    host.getCpuState = [emulator](core::ArCpuState& out) {
        auto* emu = emulator();
        if (!emu)
            return false;
        adm::AmigaEmulator::CpuState st;
        if (!emu->GetCpuState(st))
            return false;
        memcpy(out.d, st.d, sizeof(out.d));
        memcpy(out.a, st.a, sizeof(out.a));
        out.pc = st.pc;
        out.sr = st.sr;
        return true;
    };
    host.getCpuExtState = [emulator](core::CpuExtendedState& out) {
        auto* emu = emulator();
        return emu && emu->GetCpuExtState(out);
    };
    host.setCpuRegister = [emulator](const std::string& reg, uint32_t value) {
        auto* emu = emulator();
        if (!emu || !emu->HasDebugCommand())
            return false;
        // UAE matches SR, USP, VBR ... case-sensitively; only D/A/P are case-blind.
        std::string name = reg;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        std::string out;
        return emu->ExecuteDebugCommand(fmt::format("r {} {:x}", name, value), out);
    };
    host.disassembleOne = [this](uint32_t addr, core::Disassembler::Line& out) {
        return m_disassembler.DisassembleOne(addr, out);
    };
    host.execUaeCommand = [emulator](const std::string& cmd, std::string& out) {
        auto* emu = emulator();
        if (!emu || !emu->HasDebugCommand())
            return false;
        return emu->ExecuteDebugCommand(cmd, out);
    };

    // Breakpoint/watchpoint add/remove/list hooks are wired by WireEmulatorHostHooks
    // above; the AR console additionally exposes "clear all by class" commands.
    host.clearAllBreakpoints = [bps]() {
        if (bps)
            bps->ClearAllByType("PC");
    };
    host.clearAllWatchpoints = [bps]() {
        if (bps) {
            bps->ClearAllByType("Read");
            bps->ClearAllByType("Write");
            bps->ClearAllByType("R+W");
        }
    };

    host.getCopperLc = [emulator](int list, uint32_t& lcAddr) {
        auto* emu = emulator();
        if (!emu)
            return false;
        core::Custom regs{};
        if (!emu->GetCustomRegisters(regs))
            return false;
        lcAddr = core::DecodeAmigaPtr((list == 1) ? regs.cop1lc : regs.cop2lc);
        return true;
    };
    host.disasmCopperIns = [this](uint16_t w1, uint16_t w2) {
        auto ins = core::CopperDisassembler::Disassemble(w1, w2, m_customRegsDef);
        if (ins.details.empty())
            return ins.name;
        return ins.name + " " + ins.details;
    };
    host.customRegName = [this](uint32_t offset) { return CustomRegisterName(offset); };
    host.customRegDesc = [this](uint32_t offset) { return CustomRegisterDescription(offset); };
    host.customRegBits = [this](uint32_t offset) { return GetRegBits(offset); };

    host.requestOpenFile = [this]() -> std::optional<std::string> {
        // A native file dialog blocks the main thread. When a command is driven
        // non-interactively (MCP exec_console via RunCommandCapture) that would
        // freeze the UI and time out the request, so decline to prompt instead.
        if (m_captureSink)
            return std::nullopt;
        nfdchar_t* outPath = nullptr;
        nfdresult_t result = NFD_OpenDialog(&outPath, nullptr, 0, nullptr);
        // NFD blocks and can miss key-up events
        ImGui::GetIO().ClearInputKeys();
        if (result != NFD_OKAY)
            return std::nullopt;
        std::string path(outPath);
        NFD_FreePath(outPath);
        return path;
    };
    host.requestSaveFile = [this]() -> std::optional<std::string> {
        if (m_captureSink)
            return std::nullopt; // see requestOpenFile — never block a captured run
        nfdchar_t* outPath = nullptr;
        nfdresult_t result = NFD_SaveDialog(&outPath, nullptr, 0, nullptr, "memory.bin");
        ImGui::GetIO().ClearInputKeys();
        if (result != NFD_OKAY)
            return std::nullopt;
        std::string path(outPath);
        NFD_FreePath(outPath);
        return path;
    };

    host.onStateChanged = [this]() {
        if (m_onAnyCommandExecuted)
            m_onAnyCommandExecuted();
    };

    host.loadDocument = [this, view](const std::string& path, bool force) -> bool {
        if (view)
            view->SuppressNextFocus();
        bool ok = m_onLoadDocument ? m_onLoadDocument(path, force) : false;
        m_reclaimFocus = true;
        return ok;
    };

    // BuildInterpreter() runs from the constructor, before the application wires m_onSaveState
    // and friends, so these consult the members at call time rather than capturing them.
    host.saveState = [this](const std::string& name, bool persist,
                            std::string& outWarning) -> std::string {
        return m_onSaveState ? m_onSaveState(name, persist, outWarning)
                             : std::string("state snapshots unavailable");
    };
    host.loadState = [this](const std::string& name) -> std::string {
        return m_onLoadState ? m_onLoadState(name) : std::string("state snapshots unavailable");
    };
    host.deleteState = [this](const std::string& name) {
        return m_onDeleteState && m_onDeleteState(name);
    };
    host.listStates = [this]() -> std::vector<core::ArHost::StateEntry> {
        return m_onListStates ? m_onListStates() : std::vector<core::ArHost::StateEntry>{};
    };

    host.frameHistoryInfo = [this]() -> core::ArHost::FrameHistoryInfo {
        return m_onFrameHistoryInfo ? m_onFrameHistoryInfo() : core::ArHost::FrameHistoryInfo{};
    };
    host.frameHistorySeek = [this](uint64_t frame) -> std::string {
        return m_onFrameHistorySeek ? m_onFrameHistorySeek(frame)
                                    : std::string("frame history unavailable");
    };
    host.frameHistoryRewind = [this](uint64_t count) -> std::string {
        return m_onFrameHistoryRewind ? m_onFrameHistoryRewind(count)
                                      : std::string("frame history unavailable");
    };
    host.frameHistoryLoop = [this](uint64_t a, uint64_t b) -> std::string {
        return m_onFrameHistoryLoop ? m_onFrameHistoryLoop(a, b)
                                    : std::string("frame history unavailable");
    };
    host.frameHistoryClear = [this]() {
        if (m_onFrameHistoryClear)
            m_onFrameHistoryClear();
    };
    host.frameHistoryPlay = [this](bool playing) {
        if (m_onFrameHistoryPlay)
            m_onFrameHistoryPlay(playing);
    };
    host.frameHistoryStep = [this](int delta) -> std::string {
        return m_onFrameHistoryStep ? m_onFrameHistoryStep(delta)
                                    : std::string("frame history unavailable");
    };
    host.frameHistoryPlayMode = [this](int mode) {
        if (m_onFrameHistoryPlayMode)
            m_onFrameHistoryPlayMode(mode);
    };
    host.frameHistoryPlaySpeed = [this](double speed) {
        if (m_onFrameHistoryPlaySpeed)
            m_onFrameHistoryPlaySpeed(speed);
    };
    host.frameHistoryDiff = [this](uint64_t a, uint64_t b, std::vector<core::FrameDiffRun>& out,
                                   size_t maxRuns, bool& truncated) -> std::string {
        return m_onFrameHistoryDiff ? m_onFrameHistoryDiff(a, b, out, maxRuns, truncated)
                                    : std::string("frame history unavailable");
    };

    host.getRecentFiles = [this]() -> std::vector<std::string> {
        if (m_onGetRecentFiles)
            return m_onGetRecentFiles();
        return {};
    };

    host.getProfile = [this](const core::ProfileQuery& q) -> core::ProfileSnapshot {
        if (m_onGetProfile)
            return m_onGetProfile(q);
        return {};
    };

    host.lineCoverage = [this](const std::string& file, uint32_t line,
                               uint64_t frame) -> nlohmann::json {
        if (m_onLineCoverage)
            return m_onLineCoverage(file, line, frame);
        return nlohmann::json{{"error", "line coverage unavailable"}};
    };
    host.isTraceHistory = [this]() { return m_onIsTraceHistory && m_onIsTraceHistory(); };
    host.setTraceHistory = [this](bool on) {
        if (m_onSetTraceHistory)
            m_onSetTraceHistory(on);
    };

    host.getBlitLog = [this](int limit) -> core::BlitLogSnapshot {
        if (m_onGetBlitLog)
            return m_onGetBlitLog(limit);
        return {};
    };

    host.exocetSession = [this]() -> core::ExocetSession* {
        return m_onExocetSession ? m_onExocetSession() : nullptr;
    };

    host.getRegLog = [this](int limit) -> core::RegLogSnapshot {
        if (m_onGetRegLog)
            return m_onGetRegLog(limit);
        return {};
    };

    host.getDiskLog = [this](const core::DiskLogQuery& q) -> core::DiskLogSnapshot {
        if (m_onGetDiskLog)
            return m_onGetDiskLog(q);
        return {};
    };

    // LIVE command - FloppyBridge physical drive (Milestone 3). These go
    // straight to the emulator rather than through a callback on Application,
    // because nothing above this layer is involved. Milestone 4 replaces the
    // command with a Live Floppy window.
    host.fbDriverList = [emulator]() -> std::vector<std::string> {
        auto* emu = emulator();
        return emu ? emu->GetBridgeDriverList() : std::vector<std::string>{};
    };
    host.fbComPorts = [emulator]() -> std::vector<std::string> {
        auto* emu = emulator();
        return emu ? emu->EnumBridgeComPorts() : std::vector<std::string>{};
    };
    // Through EmulatorView, not straight to the emulator: attaching has to update the
    // drive model too, or a slot bridged from the console would miss the turbo and
    // save-state guards and would not be recorded in a saved session.
    host.fbAttach = [view](const core::ArHost::FloppyBridgeAttach& r) -> bool {
        return view && view->AttachBridgeDrive(r.drive, r.driverIndex, r.comPort, r.bridgeMode,
                                               r.bridgeDensity, r.driveCable, r.autoCache,
                                               r.smartSpeed, r.allowWrites);
    };
    host.fbDetach = [view](int drive) {
        if (view)
            view->DetachBridgeDrive(drive);
    };
    host.fbStatus = [view, emulator](int drive) -> core::ArHost::FloppyBridgeStatus {
        core::ArHost::FloppyBridgeStatus st;
        auto* emu = emulator();
        if (!emu)
            return st;
        // The view's record, not the emulator's: it includes a slot assigned while the
        // machine is stopped, which the core has not been told about yet.
        if (view && drive >= 0 && drive <= 3) {
            st.bridged = view->GetDriveState(drive).isBridged;
            st.pending = view->IsBridgePending(drive);
        } else {
            st.bridged = emu->IsBridgeDrive(static_cast<unsigned>(drive));
        }
        st.motor = emu->IsDriveActive(drive);
        st.cyl = emu->GetCurrentTrack(drive);
        st.media = emu->IsDiskInserted(drive);
        st.hostProtected = emu->GetRequestedFloppyWriteProtect(drive);
        st.coreProtected = emu->IsCoreFloppyWriteProtected(drive);
        st.protectDrift = emu->GetFloppyWriteProtectDrift(drive);
        // The write gate, for the LIVE table's Write column. The config is the view's
        // record so it answers for a slot attached while the machine is stopped;
        // bridgeProtected is the driver's own answer, which folds the gate, the device's
        // capability and the disk's tab together and is only meaningful once it opened.
        if (view && drive >= 0 && drive <= 3) {
            const auto& ds = view->GetDriveState(drive);
            if (ds.bridgeConfig)
                st.allowWrites = ds.bridgeConfig->allowWrites;
        }
        adm::AmigaEmulator::BridgeStatus bs;
        if (st.bridged && emu->GetBridgeStatus(static_cast<unsigned>(drive), bs))
            st.bridgeProtected = bs.writeProtected != 0;
        return st;
    };

    host.getAudioStats = [this]() -> core::AudioLogSnapshot {
        if (m_onGetAudioStats)
            return m_onGetAudioStats();
        return {};
    };

    host.getPaulaState = [this]() -> core::PaulaStateSnapshot {
        if (m_onGetPaulaState)
            return m_onGetPaulaState();
        return {};
    };

    host.audioRecord = [this](const core::AudioRecordRequest& r) -> core::AudioRecordResult {
        if (m_onAudioRecord)
            return m_onAudioRecord(r);
        return {};
    };

    host.scenario = [this](const core::ScenarioRequest& r) -> core::ScenarioResult {
        if (m_onScenario)
            return m_onScenario(r);
        return {};
    };

    host.startEmulator = [this, view]() {
        if (view)
            view->SuppressNextFocus();
        if (m_onStartEmulator)
            m_onStartEmulator();
    };
    host.stopEmulator = [view]() {
        if (view && view->IsRunning())
            view->Stop();
    };
    host.resetEmulator = [view]() {
        if (view && view->IsRunning())
            view->Reset();
    };
    host.toggleTurbo = [view]() {
        if (view)
            view->ToggleTurbo();
    };
    host.isTurbo = [view]() { return view && view->IsTurbo(); };
    host.stepFrame = [view]() {
        if (view)
            view->StepFrame();
    };
    host.endFrame = [view]() {
        if (view)
            view->EndFrame();
    };

    host.getOpenDocuments = [this]() -> std::vector<std::string> {
        if (m_onGetOpenDocuments)
            return m_onGetOpenDocuments();
        return {};
    };
    host.getOpenDocumentInfo = [this]() -> std::vector<core::ArHost::DocumentInfo> {
        return m_onGetOpenDocumentInfo ? m_onGetOpenDocumentInfo()
                                       : std::vector<core::ArHost::DocumentInfo>{};
    };
    host.getConfiguredHostMounts = [this]() {
        return m_onGetConfiguredHostMounts ? m_onGetConfiguredHostMounts()
                                           : std::array<core::HostDirectoryMount, 2>{};
    };
    host.getEffectiveHostMounts = [this]() {
        return m_onGetEffectiveHostMounts ? m_onGetEffectiveHostMounts()
                                          : std::array<core::HostDirectoryMount, 2>{};
    };
    host.setHostMount = [this](const core::HostDirectoryMount& mount, std::string& error) {
        return m_onSetHostMount && m_onSetHostMount(mount, error);
    };
    host.applyHostMounts = [this](std::string& error) {
        return m_onApplyHostMounts && m_onApplyHostMounts(error);
    };
    host.openHostMount = [this](core::HostMountDevice device) {
        return m_onOpenHostMount && m_onOpenHostMount(device);
    };
    host.refreshHostMount = [this](core::HostMountDevice device) {
        return m_onRefreshHostMount && m_onRefreshHostMount(device);
    };
    host.getHardDriveControllerRoms = [this]() {
        return m_emulatorView
                   ? m_emulatorView->GetEmulator()->GetHostMountService().GetControllerRoms()
                   : core::HardDriveControllerRoms{};
    };
    host.setHardDriveControllerRoms = [this](const core::HardDriveControllerRoms& roms,
                                             std::string& error) {
        if (!m_onSetHardDriveControllerRoms) {
            error = "controller ROMs cannot be changed here";
            return false;
        }
        m_onSetHardDriveControllerRoms(roms);
        return true;
    };
    host.listModels = [this]() {
        std::vector<core::ArHost::ModelInfo> models;
        for (const auto& info : GetAllAmigaModels()) {
            core::ArHost::ModelInfo model;
            model.id = info.id;
            model.name = info.displayName;
            if (m_emulatorView) {
                model.unavailableReason = m_emulatorView->GetModelUnavailableReason(info.model);
                model.available = model.unavailableReason.empty();
            }
            models.push_back(std::move(model));
        }
        return models;
    };
    host.selectModel = [this](const std::string& id, std::string& error) {
        if (!m_emulatorView) {
            error = "no emulator";
            return false;
        }
        for (const auto& info : GetAllAmigaModels()) {
            if (id == info.id) {
                m_emulatorView->SetModel(info.model);
                return true;
            }
        }
        error = "unknown machine";
        return false;
    };
    host.launchWhdload = [this](const std::string& path, std::string& error) {
        return m_emulatorView &&
               m_emulatorView->LaunchWhdload(std::filesystem::u8path(path), error);
    };
    host.getWhdloadStatus = [this]() -> std::optional<core::ArHost::WhdloadStatus> {
        if (!m_emulatorView)
            return std::nullopt;
        const auto game = m_emulatorView->GetActiveWhdloadGame();
        if (!game)
            return std::nullopt;
        return core::ArHost::WhdloadStatus{game->volumeLabel, game->hostDirectory.string(),
                                           game->backingType ==
                                               core::HostMountBackingType::Archive};
    };
    host.getWhdloadSettings = [this]() {
        return m_emulatorView ? m_emulatorView->GetWhdloadSettings() : core::WhdloadSettings{};
    };
    host.setWhdloadSettings = [this](const core::WhdloadSettings& settings) {
        if (m_emulatorView)
            m_emulatorView->SetWhdloadSettings(settings);
    };
    host.getWhdloadSavesDirectory = []() { return EmulatorView::WhdloadSavesDirectory().string(); };
    host.endWhdload = [this](std::string& error) {
        return m_emulatorView && m_emulatorView->ApplyHostMountsAndRestart(error);
    };
    host.getCdStatus = [this]() {
        core::ArHost::CdStatus result;
        if (!m_emulatorView)
            return result;
        const auto status = m_emulatorView->GetCdStatus();
        result.source = status.source.string();
        result.image = status.image.string();
        result.inMachine = status.inMachine;
        result.running = m_emulatorView->IsRunning();
        result.hasDrive = status.hasDrive;
        result.restartPending = status.restartPending;
        result.extracting = status.extracting;
        result.error = status.lastError;
        return result;
    };
    host.insertCd = [this](const std::string& path, std::string& error) {
        return m_emulatorView && m_emulatorView->InsertCd(std::filesystem::u8path(path), error);
    };
    host.ejectCd = [this]() {
        if (m_emulatorView)
            m_emulatorView->EjectCd();
    };
    host.getModelId = [this]() {
        return m_emulatorView ? m_emulatorView->GetCurrentModelId() : std::string();
    };
    host.getMachineStatus = [this]() {
        core::ArHost::MachineStatus status;
        if (!m_emulatorView)
            return status;
        auto* emu = m_emulatorView->GetEmulator();
        // Before the core loads nothing says otherwise; the bundled core has the options.
        status.supported = !emu->IsCoreLoaded() || emu->HasMachineConfig();
        for (const auto& setting : core::kMachineSettingKeys)
            status.settings[setting.key] = emu->ResolveCoreVariable(setting.key);
        status.running = emu->GetMachineConfig();
        return status;
    };
    host.setMachineSetting = [this](const std::string& key, const std::string& value) {
        if (m_emulatorView)
            m_emulatorView->SetMachineSetting(key, value);
    };
    host.getHostPorts = [this]() {
        core::ArHost::HostPortsInfo info;
        if (!m_emulatorView)
            return info;
        auto* emu = m_emulatorView->GetEmulator();
        info.supported = !emu->IsCoreLoaded() || emu->HasHostPorts();
        info.serial = emu->GetSerialPort();
        const auto u8 = emu->GetParallelOutput().u8string();
        info.parallelPath.assign(u8.begin(), u8.end());
        info.status = emu->GetHostPortsStatus();
        info.devices = core::EnumerateHostSerialDevices();
        return info;
    };
    host.setSerialPort = [this](const core::SerialPortTarget& target) {
        if (m_emulatorView)
            m_emulatorView->SetSerialPort(target);
    };
    host.getMidi = [this]() {
        core::ArHost::MidiInfo info;
        if (!m_emulatorView)
            return info;
        auto* emu = m_emulatorView->GetEmulator();
        info.supported = !emu->IsCoreLoaded() || emu->HasMidi();
        info.outDevice = emu->GetMidiOutDevice();
        info.inDevice = emu->GetMidiInDevice();
        // A console listing is an explicit ask, so the cached lists are refreshed (one scan
        // serves both).
        info.outputs = core::EnumerateMidiOutputs(true);
        info.inputs = core::EnumerateMidiInputs();
        info.status = emu->GetHostPortsStatus();
        return info;
    };
    host.getNetwork = [this]() {
        core::ArHost::NetworkInfo info;
        if (!m_emulatorView)
            return info;
        auto* emu = m_emulatorView->GetEmulator();
        info.supported = emu->HasBsdsocket();
        info.bsdsocket = emu->ResolveCoreVariable("puae_bsdsocket") == "enabled";
        info.cardSupported = emu->HasNetworkCard();
        info.a2065 = emu->ResolveCoreVariable("puae_a2065");
        info.uaenet = emu->ResolveCoreVariable("puae_sana2") == "enabled";
        return info;
    };
    host.setNetworkSetting = [this](const std::string& key, const std::string& value) {
        if (m_emulatorView)
            m_emulatorView->SetMachineSetting(key, value);
    };
    host.getSoundCard = [this]() {
        core::ArHost::SoundCardInfo info;
        if (!m_emulatorView)
            return info;
        auto* emu = m_emulatorView->GetEmulator();
        info.supported = emu->HasSoundBoard();
        info.board = emu->ResolveCoreVariable("puae_sound_board");
        return info;
    };
    host.setSoundCard = [this](const std::string& board) {
        if (m_emulatorView)
            m_emulatorView->SetMachineSetting("puae_sound_board", board);
    };
    host.setMidiDevices = [this](int outDevice, int inDevice) {
        if (m_emulatorView)
            m_emulatorView->SetMidiDevices(outDevice, inDevice);
    };
    host.setParallelOutput = [this](const std::string& path) {
        if (m_emulatorView)
            m_emulatorView->SetParallelOutput(std::filesystem::u8path(path));
    };
    host.restartMachine = [this](std::string& error) {
        if (!m_emulatorView) {
            error = "no emulator";
            return false;
        }
        return m_emulatorView->RestartMachine(error);
    };

    host.runDocumentByIndex = [this](int idx) {
        if (m_onRunDocumentByIndex)
            m_onRunDocumentByIndex(idx);
    };

    host.closeDocumentByIndex = [this](int idx) {
        if (m_onCloseDocumentByIndex)
            m_onCloseDocumentByIndex(idx);
    };

    host.assembler = std::make_shared<core::UaeAssembler>(
        [emulator](const std::string& src, uint32_t addr, uint16_t* words, int maxWords) {
            auto* emu = emulator();
            return emu ? emu->Assemble(src, addr, words, maxWords) : -2;
        });

    host.getAppVersion = []() -> std::string {
        return std::string(adm::core::constants::AppVersion);
    };
    host.getCoreVersion = [emulator]() -> std::string {
        auto* emu = emulator();
        return emu ? emu->GetCoreVersion() : std::string();
    };
    host.getFrameCount = [emulator]() -> uint64_t {
        auto* emu = emulator();
        return emu ? emu->GetTotalFrameCount() : 0;
    };

    host.setCpuTrace = [emulator](bool on) {
        if (auto* emu = emulator())
            emu->SetCpuTraceEnabled(on);
    };
    host.isCpuTrace = [emulator]() {
        auto* emu = emulator();
        return emu && emu->IsCpuTraceEnabled();
    };

    // BPL / SPR / PF / DISPLAY. The emulator owns the masks (shared with the Display and
    // Sprites windows and MCP) and re-renders the frame itself when they change.
    host.getDisplayMasks = [emulator]() -> core::DisplayMasks {
        auto* emu = emulator();
        return emu ? emu->GetDisplayMasks() : core::DisplayMasks{};
    };
    host.setDisplayMasks = [emulator](const core::DisplayMasks& masks) {
        if (auto* emu = emulator())
            emu->SetDisplayMasks(masks);
    };
    host.getCustomRegisters = [emulator](core::Custom& regs) {
        auto* emu = emulator();
        return emu && emu->GetCustomRegisters(regs);
    };

    host.readDiskTrack = [emulator](int drive, int track, uint8_t* buf, int len) {
        auto* emu = emulator();
        if (!emu)
            return -1;
        return emu->ReadDiskTrack(drive, track, buf, len);
    };
    host.writeDiskBytes = [emulator](int drive, uint32_t byteOffset, const uint8_t* data,
                                     uint32_t len) {
        auto* emu = emulator();
        if (!emu)
            return false;
        return emu->WriteDiskBytes(drive, byteOffset, data, len);
    };

    host.armBootblockBreak = [emulator](bool on) {
        if (auto* e = emulator())
            e->ArmBreakOnBootblock(on);
    };
    host.isBootblockArmed = [emulator]() {
        auto* e = emulator();
        return e && e->IsBootblockArmed();
    };
    host.consumeBootblockHit = [emulator]() {
        auto* e = emulator();
        return e && e->ConsumeBootblockHit();
    };
    host.bootblockEntry = [emulator]() -> uint32_t {
        auto* e = emulator();
        return e ? e->BootblockEntry() : 0u;
    };
    host.consumeWatchpointHit = [emulator]() -> std::optional<core::ArHost::WatchpointHit> {
        auto* e = emulator();
        if (!e)
            return std::nullopt;
        adm::AmigaEmulator::MemwatchHit hit;
        if (!e->GetLastMemwatchHit(hit))
            return std::nullopt;
        core::ArHost::WatchpointHit out;
        out.pc = hit.pc;
        out.addr = hit.addr;
        out.val = hit.val;
        out.rwi = hit.rwi;
        out.size = hit.size;
        out.accessMask = hit.accessMask;
        return out;
    };

    // Break-on-KPrintF-text: read-clear the matched line so the break banner names it exactly
    // once. (The rule store itself is wired by WireEmulatorHostHooks, shared with MCP.)
    host.consumeKprintfHit = [emulator]() -> std::optional<core::KprintfHit> {
        auto* e = emulator();
        core::KprintfHit hit;
        if (e && e->ConsumeKprintfHit(hit))
            return hit;
        return std::nullopt;
    };

    // Break-on-CPU-exception (EB): arm via the Breakpoints view so the mask persists + re-arms.
    host.armExceptionBreak = [bps](uint64_t mask) {
        if (bps)
            bps->SetExceptionMask(mask);
    };
    host.consumeExceptionHit = [emulator]() -> std::optional<int> {
        auto* e = emulator();
        if (!e)
            return std::nullopt;
        int vec = -1;
        if (e->ConsumeExceptionHit(vec))
            return vec;
        return std::nullopt;
    };
    // Break-on-beam-position (BL).
    host.armBeamBreak = [bps](int vpos, int hpos) {
        if (bps)
            bps->SetBeamBreak(vpos, hpos);
    };
    host.consumeBeamHit = [emulator]() {
        auto* e = emulator();
        return e && e->ConsumeBeamHit();
    };
    // Break-on-disk-DMA (BDSK).
    host.armDiskBreak = [bps, emulator](int access, int cyl, int side, int sector, uint32_t addrLo,
                                        uint32_t addrHi) {
        if (!bps)
            return false;
        auto* e = emulator();
        if (e && !e->HasDiskBreak())
            return false; // core predates the disk breakpoint exports
        ui::BreakpointsView::DiskBreak db;
        db.access = access;
        db.cyl = cyl;
        db.side = side;
        db.sector = sector;
        db.addrLo = addrLo;
        db.addrHi = addrHi;
        bps->SetDiskBreak(db);
        return true;
    };
    host.consumeDiskHit = [emulator]() -> std::optional<core::ArHost::DiskBreakHit> {
        auto* e = emulator();
        if (!e)
            return std::nullopt;
        adm::AmigaEmulator::DiskDmaEvent ev{};
        if (!e->ConsumeDiskBreakHit(ev))
            return std::nullopt;
        core::ArHost::DiskBreakHit h;
        h.isWrite = ev.direction == 3;
        h.drive = ev.drive;
        h.cyl = ev.cyl;
        h.side = ev.side;
        h.pc = ev.pc;
        h.dskpt = ev.dskptStart;
        // The break fires at arm time, before any bytes have moved, so report the
        // programmed length rather than the (still zero) captured payload.
        h.bytes = static_cast<uint32_t>(ev.words) * 2u;
        return h;
    };
    // Break-on-copper-instruction (BC).
    host.toggleCopperBreakpoint = [bps](uint32_t addr) {
        if (bps)
            bps->ToggleCopperBreakpointAt(addr);
    };

    m_interp = std::make_unique<core::ArCommandInterpreter>(&m_mem, std::move(host));
    m_interp->emitLine = [this](const core::ArLine& line) {
        ++m_emitCount;
        if (m_captureSink) {
            if (!m_captureSink->empty())
                *m_captureSink += '\n';
            *m_captureSink += line.text;
        }
        m_terminal.PutLine(line);
    };
    // CLS/CLEAR console command → same action as the Clear toolbar button.
    m_interp->clearScreen = [this]() { Clear(); };
}

void ARConsoleView::RefreshMemoryRegions()
{
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu || !m_emulatorView->IsRunning()) {
        m_mem.SetRegions({});
        return;
    }
    BuildEmulatorMemoryAccess(m_mem, emu);
}

void ARConsoleView::PrintBanner()
{
    m_terminal.PutLine(core::ArLine(" ┌─┐┌─┐┌─┐┬┌─┌─┐┌┐ ┌─┐┌─┐", core::ArColor::Highlight));
    m_terminal.PutLine(core::ArLine(" ├─┘├┤ ├┤ ├┴┐├─┤├┴┐│ ││ │", core::ArColor::Highlight));
    m_terminal.PutLine(core::ArLine("┌┴  └─┘└─┘┴ ┴┴ ┴└─┘└─┘└─┴┐", core::ArColor::Highlight));
    m_terminal.PutLine(core::ArLine("│ assembly monitor v.0.1 │", core::ArColor::Info));
    m_terminal.PutLine(core::ArLine("└────────────────────────┘", core::ArColor::Info));
    m_terminal.PutLine(std::string());
    m_terminal.PutLine(std::string());
    m_terminal.MoveCursorToNewLine();
}

void ARConsoleView::HandleReturn(int row, const std::string& line)
{
    RefreshMemoryRegions();

    // Output overwrites the buffer below the executed line (AR screen
    // semantics); in-place refreshes of the executed line use rewriteLine.
    m_terminal.SetWritePos(row + 1);
    m_interp->rewriteLine = [this, row](const core::ArLine& l) { m_terminal.WriteLineAt(row, l); };

    const size_t before = m_emitCount;
    try {
        m_interp->ExecuteLine(line);
    } catch (const std::exception& e) {
        // a backstop: a command that throws costs that command, not Peekaboo
        m_interp->emitLine(core::ArLine(std::string("Error: ") + e.what(), core::ArColor::Error));
    }
    m_interp->rewriteLine = nullptr;

    const bool emitted = (m_emitCount != before);
    const int col = m_interp->PreferredCursorColumn();
    if (emitted && col >= 0) {
        // Interactive mode: cursor onto the freshly written line
        m_terminal.SetCursor(m_terminal.WritePos() - 1, col);
    } else if (emitted) {
        // Cursor onto a cleared line just past the output
        m_terminal.CursorToClearedLine(m_terminal.WritePos());
    } else if (row >= m_terminal.LineCount() - 1) {
        // Nothing executed and we're on the last line: Return still advances
        m_terminal.MoveCursorToNewLine(/*forceNew=*/true);
    } else {
        m_terminal.SetCursor(row + 1, 0);
    }
}

void ARConsoleView::AddOutputLine(const std::string& line)
{
    m_terminal.SetWritePosToEnd();
    m_terminal.PutLine(line);
    m_terminal.MoveCursorToNewLine();
}

void ARConsoleView::OnDebugBreak()
{
    RefreshMemoryRegions();
    m_terminal.SetWritePosToEnd(); // async output always appends
    m_interp->OnDebugBreak();
    m_terminal.MoveCursorToNewLine();
}

void ARConsoleView::Clear()
{
    m_terminal.Clear();
    PrintBanner();
}

void ARConsoleView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void ARConsoleView::ToggleOpen()
{
    m_open = !m_open;
    if (m_open)
        m_noFocus = true;
}

void ARConsoleView::Draw()
{
    // Scrolling dumps page even while the window is processing input
    if (m_open && m_interp->IsScrolling()) {
        RefreshMemoryRegions();
        m_interp->Tick();
        m_terminal.CursorToClearedLine(m_terminal.WritePos());
    }

    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(760, 400), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }
    if (m_reclaimFocus) {
        ImGui::SetNextWindowFocus();
        m_reclaimFocus = false;
    }

    if (ImGui::Begin(ICON_FA_TERMINAL " Debugger Console###Debugger Console##debugger", &m_open,
                     flags)) {
        const bool running = m_emulatorView && m_emulatorView->IsRunning();
        const bool paused = m_emulatorView && m_emulatorView->IsPaused();

        ImGui::BeginDisabled(!running || paused);
        if (ImGui::Button(ICON_FA_PAUSE " Break")) {
            m_emulatorView->TogglePause();
            OnDebugBreak();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(!running ? "Emulator must be running" : "Emulator is already paused");
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ERASER " Clear"))
            Clear();

        // Clickable status indicator drawn in a fixed-width slot so its size is
        // independent of the current state's label.
        auto statusToggle = [](const char* id, const char* text, ImU32 color, float width) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const bool clicked =
                ImGui::InvisibleButton(id, ImVec2(width, ImGui::GetTextLineHeight()));
            if (ImGui::IsItemHovered())
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::GetWindowDrawList()->AddText(pos, color, text);
            return clicked;
        };

        ImGui::SameLine();
        const float runW = ImGui::CalcTextSize("RUNNING").x;
        const char* runText = paused ? "PAUSED" : (running ? "RUNNING" : "OFF");
        const ImU32 runColor = paused ? ImGui::GetColorU32(ImVec4(1.0f, 0.7f, 0.3f, 1.0f))
                                      : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        if (statusToggle("##runstate", runText, runColor, runW)) {
            if (paused || running) {
                m_emulatorView->TogglePause();
                if (running && !paused)
                    OnDebugBreak();
            } else if (m_onStartEmulator) {
                m_onStartEmulator();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(paused    ? "Click to resume"
                              : running ? "Click to break"
                                        : "Click to run");
        }

        ImGui::SameLine();
        const float modeW = std::max(ImGui::CalcTextSize("INS").x, ImGui::CalcTextSize("OVR").x);
        const char* modeText = m_terminal.IsInsertMode() ? "INS" : "OVR";
        if (statusToggle("##editmode", modeText, ImGui::GetColorU32(ImGuiCol_TextDisabled), modeW))
            m_terminal.ToggleInsertMode();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Editing mode (click or Insert key toggles): OVR = type over, INS = insert");

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Pause, false) && running && !paused) {
            m_emulatorView->TogglePause();
            OnDebugBreak();
        }

        ImGui::Separator();

        // Detect focus before entering the child window so docking doesn't break the check.
        const bool consoleFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        m_terminal.Draw(consoleFocused);

        DrawAddressContextMenu();
    }
    ImGui::End();
}

void ARConsoleView::DrawAddressContextMenu()
{
    if (m_openAddrCtx) {
        ImGui::OpenPopup("##ar_addr_ctx");
        m_openAddrCtx = false;
    }
    // The shared menu body (host=Console so Memory dump / Disassemble print inline). The
    // label/comment editor it opens is drawn by DebuggerView::Draw → DrawAddressLinkModals().
    if (m_addrCtx && m_addrCtx->valid() && ImGui::BeginPopup("##ar_addr_ctx")) {
        DrawAddressMenuBody(m_ctxAddr, *m_addrCtx, AddressLinkHost::Console,
                            AddressDefault::Memory);
        ImGui::EndPopup();
    }
}

void ARConsoleView::ExecuteCommandLine(const std::string& cmd)
{
    RefreshMemoryRegions();
    m_terminal.SetWritePosToEnd();
    m_terminal.PutLine(cmd); // echo the command at the bottom
    HandleReturn(m_terminal.WritePos() - 1, cmd);
}

bool ARConsoleView::ReadMemoryLong(uint32_t addr, uint32_t& out)
{
    RefreshMemoryRegions();
    bool ok = false;
    out = m_mem.ReadLong(addr, &ok);
    return ok;
}

std::string ARConsoleView::CustomRegisterName(uint32_t offset) const
{
    auto it = m_customRegNames.find(offset & 0x1FE);
    return it != m_customRegNames.end() ? it->second : std::string();
}

std::string ARConsoleView::CustomRegisterDescription(uint32_t offset) const
{
    auto it = m_customRegDesc.find(offset & 0x1FE);
    return it != m_customRegDesc.end() ? it->second : std::string();
}

std::string ARConsoleView::RunCommandCapture(const std::string& cmd)
{
    std::string captured;
    m_captureSink = &captured;
    ExecuteCommandLine(cmd);
    m_captureSink = nullptr;
    return captured;
}

} // namespace adm::ui
