#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "ElfHunkMap.h"

namespace adm::ui {
class EmulatorView;
} // namespace adm::ui

namespace adm::core {

class DebugFrame;
class DebugInline;
class DebugLines;
class DebugSymbols;
class PeekabooHttpServer;
struct DiskDocumentDebugInfo;

/**
 * @brief The debug info to use for the running program.
 *
 * The compiled host build's when one is loaded, otherwise the active document's sibling .elf.
 * The four travel together, so they are chosen together.
 */
struct ActiveDebugInfo
{
    const DebugSymbols* symbols = nullptr;
    const DebugLines* lines = nullptr;
    const DebugInline* inlines = nullptr;
    const DebugFrame* frames = nullptr;
};

/**
 * @brief The compile-and-run target: its debug info, its hunk map, and whether it has loaded.
 *
 * A local host build launched through /api/run, which is not a disk and so has no DiskDocument
 * to hang its debug metadata off - keeping it here means the fast path does not manufacture an
 * ADF just to make sibling ELF data available.
 *
 * Owns three things that used to be spread across Application: the four parsed DWARF readers,
 * the ELF-section-to-hunk map GDB needs to place a breakpoint, and the "has the program's
 * seglist appeared yet?" state /api/status answers from.
 *
 * Deliberately does not own the launch itself. Choosing between the host-directory fast path
 * and the bootable-ADF fallback, reporting the failure and arming stop-on-entry are decisions
 * about the whole application, so Application::OnPeekabooRunRequested keeps them and calls in
 * here for the parts that are about the program.
 *
 * Extracted from Application.
 */
class CompiledProgram
{
public:
    CompiledProgram();
    ~CompiledProgram();

    CompiledProgram(const CompiledProgram&) = delete;
    CompiledProgram& operator=(const CompiledProgram&) = delete;

    /// Both are owned by Application and outlive this object; the view may be null.
    void Init(ui::EmulatorView* emulatorView, PeekabooHttpServer* httpServer);

    /**
     * @brief Boot a compiled executable by wrapping it in a bootable ADF.
     *
     * The compatibility path, used when the host-directory launch is unavailable or failed.
     */
    bool RunViaBootableAdf(const std::string& programPath, std::string& outError);

    /// Parse the sibling .elf for @p programPath, replacing whatever was loaded before.
    void LoadDebugInfo(const std::string& programPath);

    /// Drop the parsed debug info. For when a manually booted disk replaces the target.
    void ClearDebugInfo();

    /**
     * @brief Start waiting for the launched program's seglist to appear.
     *
     * Until it does, PublishRunState() answers programLoaded:false - which is what stops the
     * VS Code integration attaching gdb (and so halting the machine) mid-boot.
     */
    void BeginWaitForLoad(uint64_t generation);

    /// Publishes "has the program finished loading?" for GET /api/status. Called once a frame.
    void PublishRunState();

    // Walk the running process's seglist and apply the bases to the hunk map.
    bool RelocateHunkMap(std::string& outError);

    /**
     * @brief The debug info to use, preferring the compiled target's over the document's.
     *
     * @param docFallback The active document's readers, or a default-constructed value when
     *                    there is no document. Supplied by the caller because which document is
     *                    active is not this class's business.
     */
    ActiveDebugInfo Resolve(const ActiveDebugInfo& docFallback) const;

    /// The hunk map, handed out by reference: it is a self-contained component with its own API
    /// and several callers drive it directly.
    ElfHunkMap& HunkMap() { return m_hunkMap; }
    const ElfHunkMap& HunkMap() const { return m_hunkMap; }

private:
    ui::EmulatorView* m_emulatorView = nullptr;
    PeekabooHttpServer* m_httpServer = nullptr;

    // Debug metadata for a local compile-and-run target.
    std::unique_ptr<DebugSymbols> m_debugSymbols;
    std::unique_ptr<DebugLines> m_debugLines;
    std::unique_ptr<DebugInline> m_debugInline;
    std::unique_ptr<DebugFrame> m_debugFrame;

    // ELF section <-> AmigaDOS hunk map for the compile-and-run target, so GDB can be
    // told where each independently allocated hunk actually landed.
    ElfHunkMap m_hunkMap;

    // Set when a compile-and-run request arrives, cleared once the program's seglist is found.
    // While set, the seglist is re-walked periodically so /api/status can answer honestly.
    bool m_awaitingLoad = false;
    bool m_loaded = false;
    // Last seglist-walk failure, so the reason is logged once rather than ten times a second.
    std::string m_lastError;
    // Which /api/run this readiness refers to; see PeekabooRunState::generation.
    uint64_t m_generation = 0;
    int m_frameCounter = 0;
};

} // namespace adm::core
