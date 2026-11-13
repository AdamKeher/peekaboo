#include "CompiledProgram.h"

#include <filesystem>
#include <fstream>
#include <vector>

#include "../ui/views/EmulatorHostHooks.h"
#include "../ui/views/EmulatorView.h"
#include "AmigaEmulator.h"
#include "ArMemoryAccess.h"
#include "DebugFrame.h"
#include "DebugInline.h"
#include "DebugLines.h"
#include "DebugSymbols.h"
#include "Logger.h"
#include "PeekabooHttpServer.h"
#include "SegListResolver.h"

namespace adm::core {

namespace {

} // namespace

// The DWARF readers are held by unique_ptr to forward-declared types, so both of these have to
// be defined where those types are complete.
CompiledProgram::CompiledProgram() = default;
CompiledProgram::~CompiledProgram() = default;

void CompiledProgram::Init(ui::EmulatorView* emulatorView, PeekabooHttpServer* httpServer)
{
    m_emulatorView = emulatorView;
    m_httpServer = httpServer;
}

void CompiledProgram::ClearDebugInfo()
{
    m_debugSymbols.reset();
    m_debugLines.reset();
    m_debugInline.reset();
    m_debugFrame.reset();
}

void CompiledProgram::BeginWaitForLoad(uint64_t generation)
{
    m_awaitingLoad = true;
    m_loaded = false;
    m_generation = generation;
}

ActiveDebugInfo CompiledProgram::Resolve(const ActiveDebugInfo& docFallback) const
{
    ActiveDebugInfo info;
    info.symbols = m_debugSymbols.get();
    info.lines = m_debugLines.get();
    info.inlines = m_debugInline.get();
    info.frames = m_debugFrame.get();
    // Exactly the original condition: the document only wins when it actually has symbols, so a
    // compiled target with line info but no symbol table still gets its line info used.
    if (!info.symbols && docFallback.symbols)
        return docFallback;
    return info;
}
void CompiledProgram::PublishRunState()
{
    core::PeekabooRunState state;
    state.sectionMap = m_hunkMap.IsLoaded();
    state.paused = m_emulatorView && m_emulatorView->IsPaused();
    state.running = m_emulatorView && m_emulatorView->IsRunning();

    // Walking the seglist costs a handful of emulated-memory reads, so only do it while someone
    // is waiting for the answer, and only every few frames. Once found it stays found: the
    // seglist does not move until the next program is loaded, which re-arms the wait.
    if (m_awaitingLoad && (++m_frameCounter % 10) == 0) {
        std::string err;
        if (RelocateHunkMap(err)) {
            m_loaded = true;
            m_awaitingLoad = false;
            m_lastError.clear();
        } else if (err != m_lastError) {
            // Say why, once per distinct reason. Discarding this string made a stuck wait
            // indistinguishable from a slow boot: the VS Code adapter sits in
            // waitForProgramLoaded printing "still booting" for its full three minutes and
            // never attaches, while the one message naming the cause was computed here and
            // thrown away ten times a second.
            m_lastError = err;
            Logger::Warn("Peekaboo run: program not located yet - {}", err);
        }
    }
    state.programLoaded = m_loaded;
    state.generation = m_generation;
    state.segments = m_loaded ? static_cast<int>(m_hunkMap.Sections().size()) : 0;
    m_httpServer->PublishRunState(state);
}

bool CompiledProgram::RelocateHunkMap(std::string& outError)
{
    if (!m_hunkMap.IsLoaded()) {
        outError = "no section map has been built";
        return false;
    }
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu) {
        outError = "no emulator";
        return false;
    }

    // Same seglist walk the profiler uses: ExecBase -> ThisTask -> pr_CLI -> cli_Module, then
    // the BPTR chain. No core hook needed.
    core::ArMemoryAccess mem;
    // Rebuilt per call — cheap, and staying fresh across bank reallocation (reset/model change)
    // is the point. Same pattern as McpToolHandler's RefreshMem.
    ui::BuildEmulatorMemoryAccess(mem, emu);
    auto readLong = [&mem](uint32_t addr, bool& ok) { return mem.ReadLong(addr, &ok); };
    auto readByte = [&mem](uint32_t addr, bool& ok) { return mem.ReadByte(addr, &ok); };

    const auto segments = core::SegListResolver::ResolveActiveProcess(readLong, readByte);
    if (segments.empty()) {
        outError = "no AmigaDOS process seglist found (is the program running?)";
        return false;
    }
    return m_hunkMap.Relocate(segments, outError);
}

void CompiledProgram::LoadDebugInfo(const std::string& programPath)
{
    m_debugSymbols.reset();
    m_debugLines.reset();
    m_debugInline.reset();
    m_debugFrame.reset();
    m_hunkMap.Clear();

    auto elfPath = core::DebugSymbols::FindSiblingElf(programPath);
    if (!elfPath) {
        Logger::Info("Debugger: no unambiguous sibling ELF found for compile-and-run target");
        return;
    }

    // Pair ELF sections with the hunks elf2hunk produced. Relocation happens later, once the
    // program is actually loaded and the seglist can be walked.
    std::string hunkErr;
    if (m_hunkMap.Build(*elfPath, programPath, hunkErr)) {
        Logger::Info("Debugger: mapped {} ELF sections to hunks", m_hunkMap.Sections().size());
    } else {
        // Not fatal: symbols and line info still work, only GDB relocation is unavailable.
        Logger::Warn("Debugger: could not map ELF sections to hunks - {}", hunkErr);
    }

    auto symbols = std::make_unique<core::DebugSymbols>();
    if (!symbols->LoadFromElf(*elfPath)) {
        Logger::Warn("Debugger: found sibling .elf but could not parse its symbol table: {}",
                     *elfPath);
        return;
    }
    Logger::Info("Debugger: loaded {} debug function symbols from sibling ELF: {}",
                 symbols->FunctionCount(), *elfPath);
    m_debugSymbols = std::move(symbols);

    auto lines = std::make_unique<core::DebugLines>();
    if (lines->LoadFromElf(*elfPath)) {
        Logger::Info("Debugger: loaded {} source line rows from {}", lines->RowCount(), *elfPath);
        m_debugLines = std::move(lines);
    }

    auto inlines = std::make_unique<core::DebugInline>();
    if (inlines->LoadFromElf(*elfPath)) {
        Logger::Info("Debugger: loaded {} inline scopes from {}", inlines->ScopeCount(), *elfPath);
        m_debugInline = std::move(inlines);
    }

    auto frames = std::make_unique<core::DebugFrame>();
    if (frames->LoadFromElf(*elfPath)) {
        Logger::Info("Debugger: loaded {} call-frame descriptions from {}", frames->FdeCount(),
                     *elfPath);
        m_debugFrame = std::move(frames);
    }
}

bool CompiledProgram::RunViaBootableAdf(const std::string& programPath, std::string& outError)
{
    std::ifstream stream(programPath, std::ios::binary | std::ios::ate);
    if (!stream) {
        outError = "unable to open compiled executable";
        return false;
    }
    const std::streamsize size = stream.tellg();
    if (size < 4) {
        outError = "compiled executable is too small";
        return false;
    }
    stream.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
        outError = "unable to read compiled executable";
        return false;
    }
    if (bytes[0] != 0x00 || bytes[1] != 0x00 || bytes[2] != 0x03 || bytes[3] != 0xF3) {
        outError = "compiled executable does not begin with HUNK_HEADER";
        return false;
    }
    return m_emulatorView->RunExecutable(
        bytes, std::filesystem::path(programPath).filename().string(), outError);
}

} // namespace adm::core
