#include "Application.h"

#include "MD5.h"

#include "UaeSavestate.h"

#include <GLFW/glfw3.h>
#include "../ui/DmaColors.h"
#include "../ui/FontRoles.h"
#include "../ui/TableLayout.h"
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include <nfd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
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
#include "ScpConverter.h"
#include "TosecManager.h"
#include "ZipUtils.h"

#include <imgui.h>
#include <zip.h>
#include <zlib.h>
#include <filesystem>

#include "AmigaScreenMap.h"
#include "BlitScreenMapper.h"

#include "../ui/ExocetVideoComposer.h"
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
#include "../ui/views/ExocetControlView.h"
#include "../ui/views/ExocetDiskMapView.h"
#include "../ui/views/ExocetMemoryView.h"
#include "../ui/views/ExocetMusicView.h"
#include "../ui/views/ExocetView.h"
#include "../ui/views/FileListView.h"
#include "../ui/views/FileViewer.h"
#include "../ui/views/FloppyDiskView.h"
#include "../ui/views/FloppyDrivesView.h"
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
#include "PrivateFeatures.h"
#include "SecureRandom.h"
#include "SegListResolver.h"
#include "TrainerFile.h"

// glfw3native.h re-introduces windows.h; undef the macros that conflict with our method names.
#ifdef _WIN32
#undef CreateDirectory
#undef DeleteFile
#undef MoveFile
#endif

namespace adm {
namespace core {

const BootBlockInfo Application::s_emptyBootBlockInfo{};

Application::Application()
    : m_memorySearcher(std::make_unique<core::MemorySearcher>()),
      m_hexEditorView(std::make_unique<ui::BlockEditorView>()),
      m_bitmapView(std::make_unique<ui::BitmapView>()),
      m_blockMapView(std::make_unique<ui::BlockMapView>()),
      m_ndosView(std::make_unique<ui::NDOSView>()),
      m_changeMapView(std::make_unique<ui::ChangeMapView>()),
      m_validationView(std::make_unique<ui::ValidationView>()),
      m_bootBlockView(std::make_unique<ui::BootBlockView>()),
      m_aboutView(std::make_unique<ui::AboutView>(&m_textureManager)),
      m_preferencesView(std::make_unique<ui::PreferencesView>()),
      m_onboardingView(std::make_unique<ui::OnboardingView>()),
      m_identifyView(std::make_unique<ui::IdentifyView>(&m_textureManager)),
      m_floppyDiskView(std::make_unique<ui::FloppyDiskView>()),
      m_floppyDrivesView(std::make_unique<ui::FloppyDrivesView>()),
      m_emulatorView(std::make_unique<ui::EmulatorView>()),
      m_debuggerView(std::make_unique<ui::DebuggerView>(m_emulatorView.get())),
      m_memoryMapView(std::make_unique<ui::MemoryMapView>(m_emulatorView.get(), &m_textureManager)),
      m_blitProfiler(std::make_unique<core::BlitProfiler>()),
      m_regWriteProfiler(std::make_unique<core::RegWriteProfiler>()),
      m_diskProfiler(std::make_unique<core::DiskProfiler>()),
      m_blitterView(std::make_unique<ui::BlitterView>(m_blitProfiler.get())),
      m_registerOpsView(std::make_unique<ui::RegisterOpsView>(m_regWriteProfiler.get())),
      m_diskIoView(std::make_unique<ui::DiskIoView>(m_diskProfiler.get())),
      m_exocet(std::make_unique<core::ExocetSession>()),
      m_exocetView(std::make_unique<ui::ExocetView>(m_exocet.get())),
      m_exocetControlView(std::make_unique<ui::ExocetControlView>(m_exocet.get())),
      m_exocetMemoryView(std::make_unique<ui::ExocetMemoryView>(m_exocet.get())),
      m_exocetDiskMapView(std::make_unique<ui::ExocetDiskMapView>(m_exocet.get())),
      m_exocetMusicView(std::make_unique<ui::ExocetMusicView>(m_exocet.get())),
      m_blitterMintermView(std::make_unique<ui::BlitterMintermView>(&m_textureManager)),
      m_dmaStatsView(std::make_unique<ui::DmaStatsView>(m_emulatorView->GetEmulator())),
      m_logView(std::make_unique<ui::LogView>()),
      m_advancedFindView(std::make_unique<ui::AdvancedFindView>()),
      m_tosecBrowserView(std::make_unique<ui::TosecBrowserView>(&m_textureManager))
{
    // Before anything below builds a window table, a menu or the MCP tool list on it.
    core::SetPrivateFeatures(m_userPrefs.Get().privateFeatures ||
                             core::GetLaunchOptions().privateFeatures);
    if (core::PrivateFeatures())
        Logger::Info("Startup: private features (Exocet, crAss engine tools) shown");

    m_logSnapshots.Init(m_emulatorView.get(), m_blitProfiler.get(), m_regWriteProfiler.get(),
                        m_diskProfiler.get(), m_registerOpsView.get());

    m_compiledProgram.Init(m_emulatorView.get(), &m_httpServer);

    core::DocumentTransferOps transferOps;
    transferOps.findDocument = [this](uint32_t docId) -> DiskDocument* {
        for (auto& candidate : m_documents) {
            if (candidate->id == docId)
                return candidate.get();
        }
        return nullptr;
    };
    transferOps.reportError = [this](const std::string& message) {
        m_errorMessage = message;
        m_showErrorModal = true;
    };
    transferOps.updateDocumentViews = [this](DiskDocument* doc, bool preserveNavigation) {
        UpdateDocumentViews(doc, preserveNavigation);
    };
    transferOps.updateHostDirectoryDocument = [this](DiskDocument* doc) {
        UpdateHostDirectoryDocument(doc);
    };
    transferOps.getHostDirectoryTree = [](DiskDocument* doc) -> FileNode {
        return doc && doc->fileListView ? doc->fileListView->GetRoot() : FileNode{};
    };
    m_documentTransfer.Init(std::move(transferOps));

    ui::ViewerLauncherOps viewerOps;
    viewerOps.reportError = [this](const std::string& message) {
        m_errorMessage = message;
        m_showErrorModal = true;
    };
    viewerOps.updateDocumentViews = [this](DiskDocument* doc, bool preserveNavigation) {
        UpdateDocumentViews(doc, preserveNavigation);
    };
    viewerOps.updateHostDirectoryDocument = [this](DiskDocument* doc) {
        UpdateHostDirectoryDocument(doc);
    };
    m_viewerLauncher.Init(&m_fontManager, &m_textureManager, &m_audioEngine, &m_userPrefs,
                          std::move(viewerOps));

    m_debugSession.Init(m_emulatorView.get(), m_debuggerView.get(), &m_gdbServer,
                        &m_cpuTraceHistory, &m_timeline, &m_compiledProgram);

    m_copperRasterCtl.Init(m_emulatorView.get(), m_debuggerView.get());

    ui::BlitOverlayOps blitOps;
    // Pinning a blit also pushes it at the Minterm window, which the overlay does not otherwise
    // know about.
    blitOps.syncMintermToBlit = [this](uint32_t lRef) { SyncMintermToBlit(lRef); };
    m_blitOverlay.Init(m_emulatorView.get(), m_debuggerView.get(), m_blitterView.get(),
                       m_blitProfiler.get(), &m_userPrefs, std::move(blitOps));

    m_documentsPanel = std::make_unique<ui::PanelView>("Documents", kDocumentsPanelDockId);
    m_documentsPanel->SetPermanent(true);
    m_documentsPanel->SetIcon(ICON_FA_FOLDER_OPEN);

    ui::WorkspaceLayoutOps layoutOps;
    layoutOps.windowStates = [this] { return GetWindowStates(); };
    layoutOps.documentViews = [this] {
        std::vector<std::pair<uint32_t, ui::FileListView*>> out;
        out.reserve(m_documents.size());
        for (auto& doc : m_documents)
            out.emplace_back(doc->id, doc->fileListView.get());
        return out;
    };
    layoutOps.screenSize = [this] {
        int w = 0, h = 0;
        if (m_window)
            glfwGetWindowSize(m_window, &w, &h);
        return ImVec2(static_cast<float>(w), static_cast<float>(h));
    };
    m_workspaceLayout.Init(&m_workspaceManager, &m_panelManager, m_documentsPanel.get(),
                           m_hexEditorView.get(), m_debuggerView.get(), &m_transitionManager,
                           kDocumentsPanelDockId, std::move(layoutOps));

    m_hexEditorView->SetDefinitionManager(&m_definitionManager);
    m_audioEngine.Initialize();

    // Set up validation view callback to navigate to blocks
    m_validationView->SetOnBlockClicked(
        [this](uint32_t block) { m_hexEditorView->GoToBlock(block); });

    // The Disk Validator window runs itself: the menu only opens it.
    m_validationView->SetOnValidate([this]() { ValidateDisk(); });

    // Set up boot block viewer navigation
    m_bootBlockView->SetOnNavigate([this](uint32_t block) { m_hexEditorView->GoToBlock(block); });

    // Wire up the boot block library (browse, identify, install)
    m_bootBlockLibrary.Load();
    m_bootBlockView->SetLibrary(&m_bootBlockLibrary);
    m_bootBlockView->SetIdentifyCallback(
        [this](const std::vector<uint8_t>& s0, const std::vector<uint8_t>& s1) {
            return m_brainFile.IdentifyBootBlock(s0, s1);
        });
    m_bootBlockView->SetOnInstallBootBlock(
        [this](const std::vector<uint8_t>& s0, const std::vector<uint8_t>& s1) {
            InstallBootBlock(s0, s1);
        });

    // Wire up the Boot Block Viewer's live-preview tab (isolated second emulator instance).
    {
        std::vector<std::string> modelNames;
        for (const auto& m : adm::GetAllAmigaModels())
            modelNames.emplace_back(m.displayName);
        m_bootBlockView->SetPreviewModels(std::move(modelNames));
    }
    m_bootBlockView->SetOnSelectPreviewModel(
        [this](int idx) { m_bootBlockPreview.SetModelIndex(idx); });
    m_bootBlockView->SetOnStartPreview(
        [this](const std::vector<uint8_t>& s0, const std::vector<uint8_t>& s1) {
            StartBootBlockPreview(s0, s1);
        });
    m_bootBlockView->SetOnStopPreview([this]() { StopBootBlockPreview(); });
    m_bootBlockView->SetPreviewKeyCallback([this](bool down, int key) {
        m_bootBlockPreview.SendKey(down, static_cast<adm::AmigaEmulator::RetroKey>(key));
    });
    m_bootBlockView->SetPreviewMouseCallback(
        [this](int16_t dx, int16_t dy, bool left, bool right, bool middle) {
            m_bootBlockPreview.SetMouseState(dx, dy, left, right, middle);
        });
    m_bootBlockView->SetPreviewFrameProvider([this]() {
        ui::BootBlockView::PreviewFrame f;
        if (m_bootBlockPreview.IsBooted()) {
            f.texture = (ImTextureID)(uintptr_t)m_bootBlockPreview.TextureId();
            f.aspect = m_bootBlockPreview.AspectRatio();
            f.valid = m_bootBlockPreview.TextureId() != 0;
        }
        return f;
    });

    // Wire up Logger to LogView
    Logger::SetCallback(
        [this](LogLevel level, const std::string& msg) { m_logView->AddEntry(level, msg); });

    // Load brainfile database for bootblock identification. It lives alongside the
    // boot block library so the .bin files and their metadata stay together; reuse
    // the library's resolved directory (handles the ./ -> ../ fallback).
    m_brainFile.LoadDatabase((m_bootBlockLibrary.Directory() / "brainfile.json").string());

    // Let the Boot Block Viewer attach brainfile metadata to each library entry
    // (by name) for its Type / Kickstart / Machine filters.
    m_bootBlockView->SetMetadataProvider(
        [this](const std::string& name) { return m_brainFile.FindByName(name); });

    // Record boot blocks newly added to the library in the brainfile too, so the
    // metadata stays in sync with the .bin files on disk.
    m_bootBlockView->SetOnLibraryEntryAdded([this](const std::string& name,
                                                   const std::vector<uint8_t>& s0,
                                                   const std::vector<uint8_t>& s1) {
        core::BootBlockInfo info = m_brainFile.IdentifyBootBlock(s0, s1);
        if (!info.identified) {
            // Build a fresh entry from the saved file when it wasn't recognised.
            info.name = name;
            info.bootable = (s0.size() >= 3 && s0[0] == 'D' && s0[1] == 'O' && s0[2] == 'S');
        }
        info.name = name;
        info.file = "bootblocks/" + core::BootBlockLibrary::SanitizeName(name) + ".bin";
        if (m_brainFile.AddEntry(info))
            m_brainFile.SaveDatabase();
    });

    // Wire up Add Bootblock callbacks
    auto saveBootBlockHandler = [this](const core::BootBlockInfo& info) {
        if (m_brainFile.AddEntry(info)) {
            m_brainFile.SaveDatabase();

            if (!m_activeDocument)
                return;

            // Re-identify the bootblock immediately
            auto sector0 = m_activeDocument->volume.ReadBlock(0);
            auto sector1 = m_activeDocument->volume.ReadBlock(1);

            // Only proceed if we have valid sectors (prevent crash on empty disk)
            if (!sector0.empty() && !sector1.empty()) {
                m_activeDocument->bootBlockInfo = m_brainFile.IdentifyBootBlock(sector0, sector1);

                m_hexEditorView->SetBootBlockInfo(m_activeDocument->bootBlockInfo);
                m_hexEditorView->GoToBlock(m_hexEditorView->GetCurrentBlock());

                if (m_bootBlockView) {
                    m_bootBlockView->SetBootBlockInfo(m_activeDocument->bootBlockInfo);
                }

                Logger::Info("Bootblock re-identified: {}", m_activeDocument->bootBlockInfo.name);
            }
        }
    };

    m_hexEditorView->SetOnSaveBootBlock(saveBootBlockHandler);

    // Initialize TOSEC manager
    TosecManager::Instance().Initialize();

    // Wire up TOSEC Browser download → placeholder document with progress in the
    // Documents panel.
    m_tosecBrowserView->SetOnStartDownload([this](core::DownloadRequest req) {
        core::DemosceneSource src;
        src.source = "tosec";
        src.releaseName = req.filename;
        StartDownloadDocument(req, src);
    });

    // Wire up "Find in TOSEC Browser" from the Identify Disk window
    m_identifyView->SetOnFindInTosec(
        [this](const std::string& md5) { m_tosecBrowserView->SelectEntry(md5); });

    // The Disk Identifier window runs itself: the menu only opens it. This is what its Identify
    // button calls, including from the empty state where the document has no identifier yet.
    m_identifyView->SetOnIdentify([this]() { IdentifyDisk(); });

    m_httpServer.Start();
}

Application::~Application() { Shutdown(); }

void Application::Shutdown()
{
    m_httpServer.Stop();
    m_mcpServer.Stop();

    // Tear down the boot block preview emulator + delete its duplicated core file.
    DestroyBootBlockPreview();

    // Important: Reset logger first
    Logger::SetCallback(nullptr);

    // Shutdown audio engine BEFORE destroying devices/contexts
    m_audioEngine.Shutdown();

    // Join the TOSEC loader here rather than at static destruction, where the Logger and other
    // statics it uses may already be gone.
    TosecManager::Instance().Shutdown();

    // Clear all pending closures and never-completed opens
    m_deferredCloseDocuments.clear();
    m_deferredRunDocuments.clear();

    // Destroy all documents while OpenGL context is alive
    m_activeDocument = nullptr;
    m_documents.clear();
    m_orphanedIdentifiers.clear();

    // Clear shared views
    m_hexEditorView.reset();
    m_bitmapView.reset();
    m_blockMapView.reset();
    m_ndosView.reset();
    m_changeMapView.reset();
    m_validationView.reset();
    m_bootBlockView.reset();
    m_aboutView.reset();
    m_identifyView.reset();
    m_floppyDiskView.reset();
    m_emulatorView.reset();
    m_debuggerView.reset();
    m_memoryMapView.reset();
    m_blitterMintermView.reset();
    m_blitterView.reset();
    m_diskIoView.reset();
    m_exocetView.reset();
    m_exocetControlView.reset();
    m_exocetMemoryView.reset();
    m_exocetDiskMapView.reset();
    m_exocetMusicView.reset();
    m_exocetListen.reset();
    m_exocet.reset(); // joins a kit command still running
    m_dmaStatsView.reset();
    m_blitProfiler.reset();
    m_diskProfiler.reset();
    m_logView.reset();
    m_advancedFindView.reset();
    m_tosecBrowserView.reset();

    // Free any remaining textures
    m_textureManager.ReleaseAll();
    m_logoTexture = 0;
}

void Application::LoadLogoTexture()
{
    const char* svgPath = "assets/logo/peekaboo_wide_thin.svg";
    if (!std::filesystem::exists(svgPath)) {
        Logger::Error("Logo SVG not found: {}", svgPath);
        return;
    }

    // Parse the SVG (nanosvg works with a mutable char* buffer)
    NSVGimage* image = nsvgParseFromFile(svgPath, "px", 96.0f);
    if (!image) {
        Logger::Error("Failed to parse SVG: {}", svgPath);
        return;
    }

    if (image->width <= 0.0f || image->height <= 0.0f) {
        Logger::Error("SVG has zero dimensions: {}", svgPath);
        nsvgDelete(image);
        return;
    }

    // Rasterize at a fixed height that looks good at toolbar scale (64px height)
    // The toolbar icon size is 32px, so 2x gives sharp anti-aliased output
    const int targetH = 128;
    const float scale = static_cast<float>(targetH) / image->height;
    const int targetW = static_cast<int>(image->width * scale + 0.5f);

    std::vector<uint8_t> pixels(static_cast<size_t>(targetW * targetH * 4), 0);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        Logger::Error("Failed to create SVG rasterizer");
        nsvgDelete(image);
        return;
    }

    nsvgRasterize(rast, image, 0.0f, 0.0f, scale, pixels.data(), targetW, targetH, targetW * 4);

    nsvgDeleteRasterizer(rast);

    m_logoAspect = static_cast<float>(targetW) / static_cast<float>(targetH);
    m_logoTexture = m_textureManager.CreateTexture(pixels.data(), targetW, targetH);

    nsvgDelete(image);

    if (m_logoTexture == 0) {
        Logger::Error("Failed to create GL texture for logo");
    } else {
        Logger::Info("Logo texture loaded ({}x{})", targetW, targetH);
    }
}

void Application::Initialize(ImGuiIO& io, GLFWwindow* window, float scale)
{
    m_window = window;
    m_emulatorView->SetWindow(window);
    m_bootBlockView->SetWindow(window);
    // Seed stored windowed geometry so ToggleFullscreen has valid values even before the
    // first GLFW pos/size callback fires.
    glfwGetWindowPos(m_window, &m_windowX, &m_windowY);
    glfwGetWindowSize(m_window, &m_windowWidth, &m_windowHeight);
    // The unscaled style, kept so the interface zoom can rescale from it (see
    // ApplyPendingFontSettings, which also applies this DPI factor on the first frame).
    m_baseStyle = ImGui::GetStyle();
#ifndef __APPLE__
    // On Windows/Linux, apply UI scaling for high-DPI monitors
    // On macOS, the OS handles DPI scaling automatically
    if (scale > 1.0f)
        m_dpiStyleScale = scale;
#endif
    LoadFonts(io, scale);
    SetupSharedCallbacks();
    LoadLogoTexture();

    // Wire up fullscreen toggle
    m_emulatorView->SetOnFullscreenToggle(
        [this](bool fullscreen) { ToggleFullscreen(fullscreen); });

    m_transitionManager.Initialize();
    m_transitionManager.SetDuration(m_userPrefs.Get().transitionDuration);

    // Build the emulator display-filter GL resources up front (see EmulatorView::InitGraphics).
    m_emulatorView->InitGraphics();

    // Fans out rather than replacing: the debugger window still gets its break banner, and an
    // attached GDB gets the stop reply it has been waiting for since the last resume. This runs
    // AFTER the break filter below, so log-mode watchpoint hits the UI deliberately swallows do
    // not reach GDB either.
    m_emulatorView->SetOnDebugBreak([this]() {
        m_debuggerView->NotifyDebugBreak();
        uint32_t stopAddr = 0;
        const auto reason = m_debugSession.ClassifyGdbStop(&stopAddr);
        m_gdbServer.NotifyStopped(reason, stopAddr);
    });
    // Two things can decide a stop was not worth showing: a log-mode watchpoint
    // (which records and keeps going) and a DOS breakpoint whose name predicate
    // did not match. Either one saying "keep running" resumes.
    m_emulatorView->SetDebugBreakFilter([this]() {
        // The stop-on-entry trap is ours and must not surface as a user-visible break.
        if (m_debugSession.ConsumeStopOnEntryWatch())
            return true;
        auto* bps = m_debuggerView->GetBreakpointsView();
        if (bps && bps->ConsumeLogModeBreak())
            return true;
        return ConsumeDosBreak();
    });

    // Every whole emulated frame, at its boundary. The frame-history recorder hangs off
    // this rather than sampling once per redraw because Update() runs a catch-up batch of
    // up to a dozen frames when the host misses a beat, and a sampled recorder saw only the
    // last of them - every frame it missed was a hole that later stranded a seek or stopped
    // a playback dead in the middle of a range.
    // There is one callback slot, and three things need every frame: the timeline recorder,
    // the scenario recorder and Exocet's proofing record. Fanned out here rather than either of
    // them owning it.
    if (auto* emu = m_emulatorView->GetEmulator())
        emu->SetOnFrameComplete([this](uint64_t frame) {
            m_timeline.RecordFrame(frame);
            m_scenarioRecorder.OnFrameComplete(frame);
            if (m_exocet && core::PrivateFeatures())
                m_exocet->OnFrame(frame); // the Exocet proofing record, every frame
        });

    // Anything that runs the machine live takes it back off the timeline first. Without one
    // choke point the transport carries on replaying into a machine that is running
    // underneath it, and the picture stops matching the playhead. Routed through the view
    // so every caller is covered - toolbar, hotkey, console, MCP - rather than only the one
    // button that remembered to ask.
    m_emulatorView->SetOnBeforeMachineRuns([this]() {
        // A replayed frame runs through the very same step path; it must not release the
        // timeline it is being driven by.
        if (!m_timeline.IsReplayInProgress())
            ReleaseTimelineMachine();
    });

    // GIF capture on a stopped emulator boots the active document, same as the Run action.
    m_emulatorView->SetOnStartEmulator([this]() { RunAdf(); });

    // The Exocet video: a composer made for each recording,
    // held by the recorder's compose function, so it goes - context, GL objects and all - when the
    // recording ends. Never set while Exocet is hidden, so the capture window has no option for it.
    if (core::PrivateFeatures()) {
        m_emulatorView->SetExocetVideo(
            [this]() -> std::string {
                if (!m_exocet || !m_exocet->Pack())
                    return "Open a disk with its .exocet beside it first.";
                return {};
            },
            [this](AmigaEmulator::VideoCaptureOptions& options) -> std::string {
                if (!m_exocet || !m_exocet->Pack())
                    return "there is no .exocet open";
                auto composer =
                    std::make_shared<ui::ExocetVideoComposer>(m_exocet.get(), m_emulatorView.get());
                options.composeWidth = ui::ExocetVideoComposer::kWidth;
                options.composeHeight = ui::ExocetVideoComposer::kHeight;
                options.compose =
                    [composer](const AmigaEmulator::VideoCaptureOptions::ComposeSource& f,
                               std::vector<uint8_t>& out) {
                        if (composer->Compose(f.rgba, f.width, f.height, f.scaleX, f.scaleY, out))
                            return true;
                        core::Logger::Error("Exocet video: {}", composer->Error());
                        return false;
                    };
                return {};
            });
    }

    m_emulatorView->SetOnEmulatorReset([this]() {
        // A reset invalidates everything keyed to the old machine: the recorded
        // instruction history, and the DOS breakpoints whose addresses came from
        // library bases that no longer exist.
        m_cpuTraceHistory.Clear();
        m_dosBreakpoints.Clear();
        if (m_debuggerView) {
            m_debuggerView->MarkMemoryRegionsDirty();
            // Reset() reloads the core synchronously, wiping the UAE-side breakpoints. Tell the
            // breakpoints view so it re-injects them on the next frame (same path as stop→start).
            if (auto* bpv = m_debuggerView->GetBreakpointsView())
                bpv->NotifyEmulatorReset();
        }
    });

    m_emulatorView->SetGetDocumentInfos([this]() { return GetDiskDocumentInfos(); });
    m_emulatorView->SetOnMountDocument(
        [this](int drive, uint32_t docId) { MountDocumentToDrive(drive, docId); });
    m_emulatorView->SetOnSetWriteProtect(
        [this](int drive, bool writeProtected) { SetDriveWriteProtected(drive, writeProtected); });
    m_emulatorView->SetOnOpenHostDirectory([this](const core::HostDirectoryMount& mount) {
        OpenHostDirectoryDocument(mount.hostDirectory.string(), mount.readOnly, mount.device,
                                  mount.volumeLabel);
    });

    // Scan for disk templates
    ScanDiskTemplates();

    // Initialize controller types from saved preferences
    const auto& prefs = m_userPrefs.Get();
    for (const auto& [name, type] : prefs.controllerTypes) {
        m_emulatorView->SetControllerType(name, type);
    }
    m_emulatorView->SetKeyboardJoystickEnabled(prefs.keyboardJoystick);

    // Set up the preferences window
    // The Kickstart ROM directory is where the core looks for everything it resolves by name -
    // extended ROMs (CD32, CDTV) and the Kickstarts WHDLoad needs as well as the main ROM. Until
    // now it only reached the core when Preferences were applied, so after a restart a CD32
    // lost its extended ROM. Apply it before any core loads.
    {
        std::error_code ec;
        const auto& romDir = prefs.kickstartRomDir;
        if (!romDir.empty() && std::filesystem::is_directory(std::filesystem::u8path(romDir), ec))
            m_emulatorView->GetEmulator()->SetSystemDirectory(romDir);
    }
    m_preferencesView->SetEmulator(m_emulatorView->GetEmulator());
    m_preferencesView->SetUserPreferences(&m_userPrefs);
    m_preferencesView->SetEmulatorView(m_emulatorView.get());
    m_preferencesView->SetFontManager(&m_fontManager);
    m_preferencesView->SetScreenProfileHooks([this] { return GetScreenProfile(); },
                                             [this](ScreenProfile p) { SetScreenProfile(p); });
    // The setup wizard
    m_onboardingView->SetEmulator(m_emulatorView->GetEmulator());
    m_onboardingView->SetEmulatorView(m_emulatorView.get());
    m_onboardingView->SetUserPreferences(&m_userPrefs);
    m_onboardingView->m_onOpenDisk = [this](const std::string& path) { LoadAdf(path); };
    // A fresh boot: the machine and DH0 the wizard set only take effect at power-on
    m_onboardingView->m_onStart = [this]() {
        if (m_emulatorView->IsRunning())
            m_emulatorView->Stop();
        RunAdf();
    };
    m_onboardingView->m_onOpenSystemFilesPrefs = [this]() { ShowSystemFilesPreferences(); };
    // A machine running on AROS for want of its Kickstart says so, and this is where it points
    m_emulatorView->SetOnFindSystemFiles([this]() { ShowSetupWizard(); });
    m_emulatorView->SetUserPreferences(&m_userPrefs);
    m_emulatorView->SetConfiguredHostMounts(prefs.hostDirectoryMounts, false);
    m_emulatorView->SetHardDriveControllerRoms(prefs.hardDriveControllerRoms, false);
    m_emulatorView->RestoreCdImage(prefs.cdImagePath);
    // Only a port the user chose goes to the core; see AmigaEmulator::SetSerialPort.
    if (!prefs.serialPort.empty()) {
        if (const auto target = core::SerialPortTarget::Parse(prefs.serialPort)) {
            if (target->ListensBeyondThisComputer())
                Logger::Warn("Serial port: listening on {} - reachable from other machines on "
                             "the network",
                             target->ToCoreName());
            m_emulatorView->GetEmulator()->SetSerialPort(*target);
        }
    }
    if (prefs.midiOutDevice >= -1 || prefs.midiInDevice >= 0)
        m_emulatorView->GetEmulator()->SetMidiDevices(prefs.midiOutDevice, prefs.midiInDevice);
    if (!prefs.parallelOutputPath.empty())
        m_emulatorView->GetEmulator()->SetParallelOutput(
            std::filesystem::u8path(prefs.parallelOutputPath));
    m_debuggerView->SetUserPreferences(&m_userPrefs);

    // Apply any persisted emulator settings from previous sessions, on the machine they were
    // written for. Only the model, not SelectModelPreset: the settings already hold its preset.
    for (const auto& info : GetAllAmigaModels()) {
        if (prefs.emulatorModel == info.id)
            m_emulatorView->GetEmulator()->SetModel(info.model);
    }
    if (!prefs.emulatorSettings.empty()) {
        std::unordered_map<std::string, std::string> uvars(prefs.emulatorSettings.begin(),
                                                           prefs.emulatorSettings.end());
        m_emulatorView->GetEmulator()->SetUserVariables(uvars);
    }

    if (prefs.showBlock) {
        m_hexEditorView->Restore();
    } else {
        m_hexEditorView->Close();
    }
    // Note: Restore() also opens Block Details. If showBlockDetails is false, we close it.
    *m_hexEditorView->GetBlockDetailsOpenStatePtr() = prefs.showBlockDetails;

    if (prefs.showDiskBitmap) {
        m_bitmapView->Restore();
    } else {
        m_bitmapView->Close();
    }

    if (prefs.showBlockTypeMap) {
        m_blockMapView->Restore();
    } else {
        m_blockMapView->Close();
    }

    if (prefs.showNdosMap) {
        m_ndosView->Restore();
    } else {
        m_ndosView->Close();
    }

    if (prefs.showBlockChangeMap) {
        m_changeMapView->Restore();
    } else {
        m_changeMapView->Close();
    }

    if (prefs.showIdentifyDisk) {
        m_identifyView->Open();
    }
    if (prefs.showTosecBrowser) {
        m_tosecBrowserView->Open();
    }
    if (prefs.showLog) {
        m_logView->Restore();
    }
    if (prefs.showEmulator) {
        m_emulatorView->Restore();
    }
    m_emulatorView->SetShowPerfOverlay(prefs.showPerfOverlay);
    m_emulatorView->SetShowPerfGraph(prefs.showPerfGraph);
    m_emulatorView->SetRunWhenHidden(prefs.emulatorRunWhenHidden);
    {
        const auto& f = prefs.displayFilters;
        ui::EmulatorFilterParams fp;
        fp.scanlines = f.scanlines;
        fp.scanlineStrength = f.scanlineStrength;
        fp.crt = f.crt;
        fp.maskStrength = f.maskStrength;
        fp.glowStrength = f.glowStrength;
        fp.curvature = f.curvature;
        fp.curveAmount = f.curveAmount;
        fp.vignetteStrength = f.vignetteStrength;
        fp.colorGrade = f.colorGrade;
        fp.gamma = f.gamma;
        fp.contrast = f.contrast;
        fp.saturation = f.saturation;
        fp.warmth = f.warmth;
        m_emulatorView->SetFilterParams(fp);
        *m_emulatorView->GetFiltersWindowOpenPtr() = f.windowOpen;
    }
    m_debuggerView->RestoreWindows(
        prefs.showDebugCpu, prefs.showDebugCustom, prefs.showDebugDisassembly,
        prefs.showDebugCopper, prefs.showDebugMemory, prefs.showDebugDisplay,
        prefs.showDebugSprites, prefs.showDebugBitplanes, prefs.showDebugCia,
        prefs.showFrameScrubber, prefs.showDebugConsole, prefs.showBreakpoints,
        prefs.showAnnotations, prefs.showDebugCallStack, prefs.showBartmanBitmaps,
        prefs.showDebugAudio, prefs.showDebugProfile, prefs.showDebugTrainer);

    // The core-log filter lives in the host, not the core, so it applies whether or not a
    // core has been loaded yet and survives a core swap without being re-pushed.
    AmigaEmulator::SetCoreLogVerbose(prefs.coreVerboseLog);

    // Apply the persisted Bartman debug-support opt-in to the emulator. Stored in the member
    // regardless of core-load order; LoadCore/LoadADF re-assert it before the machine boots.
    if (auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr) {
        emu->SetBartmanDebugSupport(prefs.bartmanDebugEnabled);
        // Chipset display options (Debugger -> Display). Held host-side and re-pushed by
        // LoadCore, so setting them before a core exists is fine.
        emu->SetPaletteVisEnabled(prefs.paletteVis);
        emu->SetBplcon1ScrollEnabled(prefs.bplcon1Scroll);
        {
            const auto& bp = prefs.blocks;
            adm::core::CopperBlock cb;
            cb.enabled = bp.copperEnabled;
            cb.startLine = bp.copperStart;
            cb.endLine = bp.copperEnd;
            emu->SetCopperBlock(cb);
            adm::core::BplPtrBlock pb;
            pb.mask = static_cast<uint8_t>(bp.bplptrMask);
            pb.startLine = bp.bplptrStart;
            pb.endLine = bp.bplptrEnd;
            emu->SetBplPtrBlock(pb);
            emu->SetBlitterWritesEnabled(bp.blitterWrites);
        }
    }

    // Restore the blit overlay, arming the core-side provenance map to match the checkbox.
    {
        ui::BlitOverlaySettings bs;
        const auto& bo = prefs.blitOverlay;
        bs.enabled = bo.enabled;
        bs.useProvenance = bo.useProvenance;
        bs.outlineAll = bo.outlineAll;
        bs.opacity = bo.opacity;
        bs.decayFrames = bo.decayFrames;
        m_debuggerView->SetBlitOverlaySettings(bs);
        m_blitOverlay.SetSettings(bs);
    }

    // The blitter's destination-data capture (which backs the Data column) needs no restore
    // here — SyncBlitDestCapture() arms it from the same preference once the core is loaded.

    // Restore the Copper/DMA raster overlay, arming DMA recording to match the checkbox.
    {
        ui::RasterSettings rs;
        const auto& rp = prefs.raster;
        rs.enabled = rp.enabled;
        rs.showCopper = rp.showCopper;
        rs.showWaitSpans = rp.showWaitSpans;
        rs.showCopperWanted = rp.showCopperWanted;
        rs.showBlanking = rp.showBlanking;
        rs.showGeometryEvents = rp.showGeometryEvents;
        rs.channelMask = static_cast<uint16_t>(rp.channelMask);
        rs.opacity = rp.opacity;
        m_debuggerView->SetRasterSettings(rs);
        m_copperRasterCtl.SetEnabled(rs.enabled);
    }
    m_debuggerView->SetScrubberTickUnit(prefs.frameScrubberUnit);
    m_debuggerView->SetScrubberShowInstrNumbers(prefs.frameScrubberShowInstrNumbers);
    m_debuggerView->SetScrubberHeatMap(prefs.frameScrubberHeatMap);
    if (prefs.spritesScanlinesPanelW > 0.0f)
        m_debuggerView->SetSpritesScanlinesPanelWidth(prefs.spritesScanlinesPanelW);
    if (prefs.spritesDetailPanelH > 0.0f)
        m_debuggerView->SetSpritesDetailPanelHeight(prefs.spritesDetailPanelH);
    if (prefs.memMapLeftColumnW > 0.0f)
        m_memoryMapView->SetLeftColumnWidth(prefs.memMapLeftColumnW);
    if (prefs.memMapPreviewH > 0.0f)
        m_memoryMapView->SetPreviewHeight(prefs.memMapPreviewH);
    m_debuggerView->SetMemoryBytesPerLine(prefs.memoryBytesPerLine);
    m_hexEditorView->SetBytesPerLine(prefs.blockBytesPerLine);
    m_hexEditorView->SetShowTrack(prefs.blockShowTrack);
    if (prefs.showMemoryMap) {
        m_memoryMapView->Restore();
    }
    if (prefs.showBlitter) {
        m_blitterView->Restore();
    }
    if (prefs.showRegisterOps) {
        m_registerOpsView->Restore();
    }
    m_registerOpsView->SetSourceFilter(prefs.registerOpsSource);
    m_registerOpsView->SetChangedOnly(prefs.registerOpsChangedOnly);
    if (prefs.showDiskIo) {
        m_diskIoView->Restore();
    }
    if (prefs.showExocet && core::PrivateFeatures()) {
        m_exocetView->Restore();
    }
    if (prefs.showBlitterMinterm) {
        m_blitterMintermView->Restore();
    }
    if (prefs.showDmaStats) {
        m_dmaStatsView->Restore();
    }

    // Restore user-created panels from last session.
    // These must exist before the first frame so ImGui can reconnect them to
    // the dock nodes recorded in imgui.ini.
    for (const auto& def : prefs.panels)
        m_panelManager.Restore(def.name, def.id);

    // Restore the window that was focused at exit. Deferred a couple of frames so the docking
    // layout has settled and any windows that grab focus on first appearance have done so —
    // we apply the saved focus last, overriding them.
    m_pendingFocusWindow = prefs.focusedWindow;
    m_focusRestoreDelay = 2;
}

void Application::GetDefaultFileListViewPlacement(uint32_t& outDockId, float& outPosX,
                                                  float& outPosY, float& outSizeX, float& outSizeY,
                                                  bool& outHasPosSize) const
{
    outDockId = 0;
    outHasPosSize = false;

    // First check currently open ImGui windows
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx) {
        for (int i = 0; i < ctx->Windows.Size; i++) {
            ImGuiWindow* win = ctx->Windows[i];
            if (strstr(win->Name, "###FileListView_") || strcmp(win->Name, "File Browser") == 0) {
                outDockId = win->DockId;
                if (outDockId == 0) {
                    outHasPosSize = true;
                    outPosX = win->Pos.x;
                    outPosY = win->Pos.y;
                    outSizeX = win->Size.x;
                    outSizeY = win->Size.y;
                }
                return;
            }
        }
    }

    // If not found, try extracting from the saved INI layout settings
    size_t iniSize = 0;
    const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);
    if (iniData && iniSize > 0) {
        std::string iniStr(iniData, iniSize);
        std::istringstream iss(iniStr);
        std::string line;
        bool inFileListViewSection = false;

        while (std::getline(iss, line)) {
            if (line.empty())
                continue;

            if (line[0] == '[') {
                if (line.find("[Window][") != std::string::npos &&
                    (line.find("###FileListView_") != std::string::npos ||
                     line.find("File Browser") != std::string::npos)) {
                    inFileListViewSection = true;
                } else {
                    inFileListViewSection = false;
                }
            } else if (inFileListViewSection) {
                if (line.find("DockId=0x") == 0) {
                    try {
                        size_t commaPos = line.find(',');
                        std::string hexStr = line.substr(7, commaPos - 7);
                        outDockId = std::stoul(hexStr, nullptr, 16);
                        if (outDockId != 0)
                            return; // Found valid DockId, we can stop
                    } catch (...) {
                        // Malformed ini value — keep scanning.
                    }
                } else if (line.find("Pos=") == 0) {
                    try {
                        size_t commaPos = line.find(',');
                        outPosX = std::stof(line.substr(4, commaPos - 4));
                        outPosY = std::stof(line.substr(commaPos + 1));
                        outHasPosSize = true;
                    } catch (...) {
                        // Malformed ini value — keep scanning.
                    }
                } else if (line.find("Size=") == 0) {
                    try {
                        size_t commaPos = line.find(',');
                        outSizeX = std::stof(line.substr(5, commaPos - 5));
                        outSizeY = std::stof(line.substr(commaPos + 1));
                    } catch (...) {
                        // Malformed ini value — keep scanning.
                    }
                }
            }
        }
    }
}

bool Application::LoadAdf(const std::string& path)
{
    // Interactive entry (menu / drag-and-drop / recent files / command line):
    // opening a path that isn't already open adds it as a new tab without disturbing
    // whatever is currently active. Opening a path that IS already open reloads that
    // document from disk (reloadIfOpen) so "Open Recents" always shows the latest
    // version instead of the stale in-memory copy. Either way, replacing a dirty
    // document prompts the user to save first.
    return LoadAdfImpl(path, /*force*/ false, /*interactive*/ true, /*reloadIfOpen*/ true);
}

bool Application::LoadDocumentExternal(const std::string& path, bool force, bool reloadIfOpen)
{
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::u8path(path), ec) && !ec)
        return OpenHostDirectoryDocument(path);
    // Console / MCP entry (non-interactive): a dirty active document is only
    // replaced when force is set; otherwise the load is refused.
    return LoadAdfImpl(path, force, /*interactive*/ false, reloadIfOpen);
}

bool Application::OpenHostDirectoryDocument(const std::string& path, bool readOnly,
                                            std::optional<core::HostMountDevice> assignedDevice,
                                            const std::string& volumeLabel)
{
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(std::filesystem::u8path(path), ec);
    if (ec) {
        Logger::Error("Unable to open host directory '{}': {}", path, ec.message());
        return false;
    }
    for (auto& existing : m_documents) {
        if (existing->kind == core::DocumentKind::HostDirectory && existing->hostDirectory &&
            existing->hostDirectory->GetCanonicalRoot() == canonical) {
            std::string updateError;
            if (!existing->hostDirectory->Open(canonical, readOnly, updateError, assignedDevice,
                                               volumeLabel)) {
                Logger::Error("Unable to update host directory document '{}': {}", path,
                              updateError);
                return false;
            }
            existing->fileListView->SetCapabilities(existing->hostDirectory->GetCapabilities());
            UpdateHostDirectoryDocument(existing.get());
            SwitchActiveDocument(existing.get());
            existing->fileListView->RequestFocus();
            return true;
        }
    }

    auto doc = std::make_unique<DiskDocument>();
    doc->id = m_nextDocId++;
    doc->kind = core::DocumentKind::HostDirectory;
    doc->currentFilePath = path;
    doc->hostDirectory = std::make_unique<core::HostDirectoryDocument>();
    std::string error;
    if (!doc->hostDirectory->Open(canonical, readOnly, error, assignedDevice, volumeLabel)) {
        Logger::Error("Unable to open host directory '{}': {}", path, error);
        return false;
    }
    doc->fileListView = std::make_unique<ui::FileListView>();
    doc->fileListView->SetDocumentId(doc->id);
    doc->fileListView->SetFilename(canonical.filename().string());
    doc->fileListView->SetCapabilities(doc->hostDirectory->GetCapabilities());
    if (ImGui::DockBuilderGetNode(kDocumentsPanelDockId) != nullptr)
        doc->fileListView->SetTargetDockId(kDocumentsPanelDockId);

    DiskDocument* ptr = doc.get();
    doc->fileListView->SetOnOpenFile([this, ptr](uint32_t entry, const std::string& filename) {
        m_viewerLauncher.OpenFileViewer(ptr, entry, filename);
    });
    doc->fileListView->SetOnViewImage([this, ptr](uint32_t entry, const std::string& filename) {
        m_viewerLauncher.OpenImageViewer(ptr, entry, filename);
    });
    doc->fileListView->SetOnListenAudio([this, ptr](uint32_t entry, const std::string& filename) {
        m_viewerLauncher.OpenAudioPlayer(ptr, entry, filename);
    });
    doc->fileListView->SetOnViewHunks([this, ptr](uint32_t entry, const std::string& filename) {
        m_viewerLauncher.OpenHunkViewer(ptr, entry, filename);
    });
    doc->fileListView->SetOnImportFile([this, ptr](uint32_t parent) {
        auto found = ptr->hostEntryIds.find(parent);
        if (found == ptr->hostEntryIds.end())
            return;
        const nfdpathset_t* paths = nullptr;
        if (NFD_OpenDialogMultiple(&paths, nullptr, 0, nullptr) != NFD_OKAY)
            return;
        nfdpathsetsize_t count = 0;
        NFD_PathSet_GetCount(paths, &count);
        std::string error;
        for (nfdpathsetsize_t index = 0; index < count; ++index) {
            nfdchar_t* selected = nullptr;
            NFD_PathSet_GetPath(paths, index, &selected);
            if (selected && !ptr->hostDirectory->Import(found->second, selected, error))
                Logger::Error("Host directory import: {}", error);
        }
        NFD_PathSet_Free(paths);
        ptr->hostDirectory->RefreshNow(error);
        UpdateHostDirectoryDocument(ptr);
    });
    doc->fileListView->SetOnCreateDir([this, ptr](uint32_t parent, const std::string& name) {
        auto found = ptr->hostEntryIds.find(parent);
        if (found == ptr->hostEntryIds.end())
            return;
        std::string error;
        if (!ptr->hostDirectory->CreateDirectory(found->second, name, error))
            Logger::Error("Host directory: {}", error);
        ptr->hostDirectory->RefreshNow(error);
        UpdateHostDirectoryDocument(ptr);
    });
    doc->fileListView->SetOnRenameFile(
        [this, ptr](uint32_t, uint32_t entry, const std::string&, const std::string& name) {
            auto found = ptr->hostEntryIds.find(entry);
            if (found == ptr->hostEntryIds.end())
                return;
            std::string error;
            if (!ptr->hostDirectory->Rename(found->second, name, error))
                Logger::Error("Host directory: {}", error);
            ptr->hostDirectory->RefreshNow(error);
            UpdateHostDirectoryDocument(ptr);
        });
    doc->fileListView->SetOnDeleteFiles(
        [this, ptr](const std::vector<std::pair<uint32_t, std::string>>& entries) {
            std::string error;
            for (const auto& [entry, name] : entries) {
                (void)name;
                auto found = ptr->hostEntryIds.find(entry);
                if (found != ptr->hostEntryIds.end() &&
                    !ptr->hostDirectory->Remove(found->second, error))
                    Logger::Error("Host directory: {}", error);
            }
            ptr->hostDirectory->RefreshNow(error);
            UpdateHostDirectoryDocument(ptr);
        });
    doc->fileListView->SetOnClose([this, ptr](uint32_t) { QueueCloseDocument(ptr); });
    SetupCrossDocumentTransfer(ptr);
    doc->fileListView->SetOnMoveFiles(
        [this, ptr](const std::vector<std::tuple<uint32_t, std::string, uint32_t>>& moves) {
            std::string error;
            for (const auto& [sourceParent, name, parent] : moves) {
                auto sourceParentId = ptr->hostEntryIds.find(sourceParent);
                auto destination = ptr->hostEntryIds.find(parent);
                if (sourceParentId == ptr->hostEntryIds.end() ||
                    destination == ptr->hostEntryIds.end())
                    continue;
                const auto& scan = ptr->hostDirectory->GetScan();
                auto source =
                    std::find_if(scan.entries.begin(), scan.entries.end(),
                                 [&](const core::HostDirectoryEntry& candidate) {
                                     return candidate.parentId == sourceParentId->second &&
                                            candidate.name == name;
                                 });
                if (source != scan.entries.end() &&
                    !ptr->hostDirectory->Move(source->id, destination->second, error))
                    Logger::Error("Host directory move: {}", error);
            }
            ptr->hostDirectory->RefreshNow(error);
            UpdateHostDirectoryDocument(ptr);
        });

    m_documents.push_back(std::move(doc));
    UpdateHostDirectoryDocument(ptr);
    SwitchActiveDocument(ptr);
    m_userPrefs.AddRecentDirectory(path);
    Logger::Info("Opened host directory document '{}' ({})", path,
                 readOnly ? "read-only" : "read-write");
    return true;
}

void Application::UpdateHostDirectoryDocument(DiskDocument* doc)
{
    if (!doc || doc->kind != core::DocumentKind::HostDirectory || !doc->hostDirectory ||
        !doc->fileListView)
        return;
    core::FileNode root;
    root.name = doc->hostDirectory->GetVolumeLabel().empty()
                    ? doc->hostDirectory->GetCanonicalRoot().filename().string()
                    : doc->hostDirectory->GetVolumeLabel();
    root.isDir = true;
    root.startSector = 0;
    root.entryId = core::DocumentEntryId{};
    doc->hostEntryIds.clear();
    doc->hostEntryIds.emplace(0, core::DocumentEntryId{});

    const auto& scan = doc->hostDirectory->GetScan();
    uint32_t nextId = 1;
    std::function<void(core::FileNode&, const core::DocumentEntryId&, uint32_t)> appendChildren;
    appendChildren = [&](core::FileNode& parent, const core::DocumentEntryId& parentId,
                         uint32_t parentNumber) {
        for (const auto& entry : scan.entries) {
            if (entry.parentId != parentId)
                continue;
            const uint32_t number = nextId++;
            doc->hostEntryIds.emplace(number, entry.id);
            core::FileNode node;
            node.entryId = entry.id;
            node.name = entry.name;
            node.isDir = entry.isDirectory;
            node.size = static_cast<int32_t>(std::min<uint64_t>(entry.size, INT32_MAX));
            node.startSector = static_cast<int32_t>(number);
            node.parentSector = static_cast<int32_t>(parentNumber);
            if (entry.amigaNameCollision)
                node.comment = "Amiga case-insensitive name collision";
            else if (entry.isSymlink && !entry.symlinkConfined)
                node.comment = "Symlink target outside document root";
            if (!node.isDir) {
                std::vector<uint8_t> bytes;
                std::string error;
                if (doc->hostDirectory->ReadFilePrefix(entry.id, 4096, bytes, error)) {
                    const auto type = m_dataTypeManager.Identify(bytes);
                    node.dataType = type.name;
                    node.dataTypeGroup = type.group;
                }
            }
            if (node.isDir && !entry.isSymlink)
                appendChildren(node, entry.id, number);
            parent.children.push_back(std::move(node));
        }
    };
    appendChildren(root, core::DocumentEntryId{}, 0);
    doc->fileListView->SetRoot(root);
    doc->fileListView->SetCapabilities(doc->hostDirectory->GetCapabilities());
}

bool Application::LoadAdfImpl(const std::string& pathRef, bool force, bool interactive,
                              bool reloadIfOpen)
{
    // Copy path immediately - the reference may point into the recent files vector,
    // which gets modified by AddRecentFile() later in this function.
    const std::string path = pathRef;

    // A .peek is a debug session, not a disk image; route it to the session loader so the
    // "Open Recents" menu and drag-and-drop both work for sessions.
    {
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (ext == ".peek") {
            OpenSessionFromPath(path);
            return true;
        }
        // A disc image, or a .7z of one, goes into CD0 rather than becoming a document.
        if (ext == ".cue" || ext == ".ccd" || ext == ".mds" || ext == ".nrg" || ext == ".chd" ||
            ext == ".iso" || ext == ".7z") {
            std::string error;
            if (!m_emulatorView->InsertCd(std::filesystem::u8path(path), error)) {
                Logger::Error("CD: {}", error);
                if (interactive) {
                    m_errorMessage = "Could not insert the disc: " + error;
                    m_showErrorModal = true;
                }
                return false;
            }
            m_emulatorView->Open();
            return true;
        }
        // A WHDLoad archive or slave is something to boot, not a disk image to browse.
        if (ext == ".lha" || ext == ".lzh" || ext == ".lzx" || ext == ".slave") {
            std::string error;
            if (!m_emulatorView->LaunchWhdload(std::filesystem::u8path(path), error)) {
                Logger::Error("WHDLoad: {}", error);
                if (interactive) {
                    m_errorMessage = "Could not launch WHDLoad: " + error;
                    m_showErrorModal = true;
                }
                return false;
            }
            return true;
        }
    }

    // If this path is already open, normally focus it instead of opening it again. Compile-and-run
    // requests (and interactive re-opens, see below) opt into reloading because the file
    // contents have just changed on disk.
    DiskDocument* alreadyOpen = nullptr;
    for (auto& existing : m_documents) {
        if (existing->currentFilePath == path) {
            alreadyOpen = existing.get();
            break;
        }
    }
    if (alreadyOpen && !reloadIfOpen) {
        Logger::Info("Document already open: {}", path);
        SwitchActiveDocument(alreadyOpen);
        alreadyOpen->fileListView->RequestFocus();
        return true;
    }

    // Reloading a document that's already open (same path) replaces that document in
    // place, so the user gets the latest version from disk. Opening a *different* path
    // interactively (menu / Open Recents / drag-and-drop) does NOT touch whatever is
    // currently active - it's simply added as another tab. The non-interactive
    // console/MCP path keeps the legacy "replace the active tab" behavior for its
    // edit-build-test loop. If the document being replaced has unsaved edits, give the
    // user a chance to save first (or, for the non-interactive path, refuse the load
    // unless force is set). In every case the replacement is loaded first and the
    // previous document is only dropped once the new one has opened, so a failed load
    // never destroys the document the user already had open.
    DiskDocument* active = alreadyOpen ? alreadyOpen : (interactive ? nullptr : m_activeDocument);
    if (active && active->HasUnsavedChanges() && !force) {
        if (!interactive) {
            Logger::Info(
                "Load refused: active document '{}' has unsaved changes (pass force to replace)",
                active->currentFilePath);
            return false;
        }
        const std::string dialogName =
            active->currentFilePath.empty()
                ? std::string{}
                : std::filesystem::path(active->currentFilePath).filename().string();
        const std::string failMsg =
            dialogName.empty()
                ? std::string("Load failed: the current document has unsaved changes that "
                              "were not saved.")
                : fmt::format("Load failed: '{}' has unsaved changes that were not saved.",
                              dialogName);
        m_unsavedChanges.Enqueue(
            dialogName, [this]() { return SaveAdf(); },
            [this, active, path]() {
                if (LoadAdfInternal(path))
                    QueueCloseDocument(active);
            },
            [this, failMsg]() {
                m_errorMessage = failMsg;
                m_showErrorModal = true;
            });
        return true; // deferred until the user resolves the dialog
    }

    // Which drives currently hold the document we are about to replace? Reloading a
    // rebuilt image has to reach the emulated machine as a disk swap, not just a new
    // tab: MountDisk hot-inserts while running, so after the reload the guest sees the
    // new media and a warm reset boots it. Without this the drive kept serving the
    // previous build until the next full stop/start.
    std::vector<int> drivesHoldingOutgoing;
    if (active && m_emulatorView && m_emulatorView->IsRunning()) {
        for (int d = 0; d < 4; ++d) {
            const auto& st = m_emulatorView->GetDriveState(d);
            if (st.hasDisk && st.documentId == active->id)
                drivesHoldingOutgoing.push_back(d);
        }
    }

    // Nothing to replace (interactive open of a new path), a clean document, or force:
    // open the replacement, then close the previous document only if the load succeeded.
    bool ok = LoadAdfInternal(path);
    if (ok && active)
        QueueCloseDocument(active);

    // Re-insert into every drive that held the old copy. LoadAdfInternal has already
    // made the replacement active, and it carries a fresh document id, so the drive's
    // stale documentId is replaced along with the bytes.
    if (ok && m_activeDocument) {
        for (int d : drivesHoldingOutgoing)
            MountDocumentToDrive(d, m_activeDocument->id);
    }
    return ok;
}

bool Application::LoadAdfInternal(const std::string& path)
{
    Logger::Info("Loading {}...", path);

    // Determine target dock: prefer the Documents panel if its DockSpace is live,
    // otherwise fall back to the legacy placement heuristic.
    uint32_t targetDockId = 0;
    float targetPosX = 0.0f, targetPosY = 0.0f, targetSizeX = 0.0f, targetSizeY = 0.0f;
    bool hasTargetPosSize = false;

    if (ImGui::DockBuilderGetNode(kDocumentsPanelDockId) != nullptr) {
        targetDockId = kDocumentsPanelDockId;
    } else {
        GetDefaultFileListViewPlacement(targetDockId, targetPosX, targetPosY, targetSizeX,
                                        targetSizeY, hasTargetPosSize);
    }

    // Create a new document
    auto doc = std::make_unique<DiskDocument>();
    doc->id = m_nextDocId++;
    doc->initialDockId = targetDockId;
    doc->fileListView = std::make_unique<ui::FileListView>();
    doc->fileListView->SetDocumentId(doc->id);
    if (doc->initialDockId != 0) {
        doc->fileListView->SetTargetDockId(doc->initialDockId);
    } else if (hasTargetPosSize) {
        doc->fileListView->SetTargetPos(targetPosX, targetPosY);
        doc->fileListView->SetTargetSize(targetSizeX, targetSizeY);
    }
    doc->diskSearcher = std::make_unique<core::DiskSearcher>();

    // Hard-disk images are opened whole into memory and browsed a partition at a time. They
    // skip the floppy decoders entirely: an .hdf is raw blocks, and a .hdz is those gzipped.
    {
        std::string extension = std::filesystem::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (extension == ".hdf" || extension == ".hdz")
            return LoadHardfileDocument(std::move(doc), path, extension == ".hdz");
    }

    // Read the file into memory and route it through the shared decoder so local
    // opens recognise formats (DMS / ZIP / HUNK executable / raw ADF) using exactly
    // the same logic as remote downloads (see ConvertToAdfBytes). The file-backed
    // bookkeeping (DMS viewer, originating ZIP/exe path) is layered on top here.
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        Logger::Error("Failed to open file: {}", path);
        return false;
    }
    std::streamsize fileSize = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> fileBytes(static_cast<size_t>(std::max<std::streamsize>(fileSize, 0)));
    if (fileSize > 0 && !in.read(reinterpret_cast<char*>(fileBytes.data()), fileSize)) {
        Logger::Error("Failed to read file: {}", path);
        return false;
    }
    in.close();

    const std::string filename = std::filesystem::path(path).filename().string();
    bool isExe = false;
    AdfDecodeInfo decodeInfo;
    auto adfBytes = ConvertToAdfBytes(std::move(fileBytes), filename, "", isExe, &decodeInfo);

    if (!adfBytes) {
        Logger::Error("Not a recognized Amiga file format: {}", path);
        m_errorMessage = "Not a recognized Amiga file format (expected HUNK executable or "
                         "ADF/extended-ADF/DMS/IPF/SCP/ZIP disk image).";
        m_showErrorModal = true;
        return false;
    }

    if (!doc->volume.Mount(*adfBytes)) {
        Logger::Error("Failed to mount ADF: {}", path);
        return false;
    }

    doc->currentFilePath = path;

    // ZIP provenance is independent of the member's own format: a zipped IPF reports
    // kind == Ipf but still came out of an archive.
    if (!decodeInfo.zipEntryName.empty()) {
        doc->loadedZipPath = path;
        doc->loadedZipEntryName = decodeInfo.zipEntryName;
    }

    switch (decodeInfo.kind) {
        case AdfSourceKind::Dms:
            doc->loadedDmsPath = path;
            // Feed the embedded DMS tabs in this document's window.
            doc->fileListView->SetDmsData(path);
            Logger::Info("Loaded disk from DMS: {}", path);
            break;
        case AdfSourceKind::Zip:
            Logger::Info("Loaded disk from ZIP: {} [{}]", path, decodeInfo.zipEntryName);
            break;
        case AdfSourceKind::Executable:
            doc->loadedExecutablePath = path;
            break;
        case AdfSourceKind::Ipf:
            doc->loadedIpfPath = path;
            doc->ipfTracks = std::move(decodeInfo.ipfTracks);
            // Hand the raw IPF to the emulator so it reads flux-level data (copy
            // protection survives); the mounted ADF stays for browsing.
            doc->emulatorImageBytes = std::move(decodeInfo.rawImageBytes);
            // Feed the embedded Track Map tab in this document's window.
            doc->fileListView->SetIpfTracks(doc->ipfTracks);
            Logger::Info("Loaded disk from IPF: {}", path);
            break;
        case AdfSourceKind::ExtAdf:
            doc->loadedExtAdfPath = path;
            doc->ipfTracks = std::move(decodeInfo.ipfTracks);
            // Hand the raw UAE-1ADF to the emulator so it reads the disk natively
            // (copy protection survives); the mounted ADF stays for browsing.
            doc->emulatorImageBytes = std::move(decodeInfo.rawImageBytes);
            doc->fileListView->SetIpfTracks(doc->ipfTracks);
            Logger::Info("Loaded disk from extended ADF (UAE-1ADF): {}", path);
            break;
        case AdfSourceKind::Scp:
            doc->loadedScpPath = path;
            doc->ipfTracks = std::move(decodeInfo.ipfTracks);
            // Hand the raw flux dump to the emulator: only a handful of an SCP's
            // tracks are usually standard AmigaDOS, so the decoded ADF would boot
            // to nothing. PuAE reads .scp natively.
            doc->emulatorImageBytes = std::move(decodeInfo.rawImageBytes);
            doc->fileListView->SetIpfTracks(doc->ipfTracks);
            Logger::Info("Loaded disk from SuperCard Pro flux image: {}", path);
            break;
        case AdfSourceKind::Adf:
            break;
    }

    // Auto-discover source-level debug symbols: bartman's toolchain emits the
    // runnable a.exe next to a matching a.elf that still carries the symbol table
    // (and DWARF). If one sits beside the loaded file, load it now so the
    // debugger/profiler can map addresses back to functions.
    if (auto elfPath = core::DebugSymbols::FindSiblingElf(path)) {
        auto syms = std::make_unique<core::DebugSymbols>();
        if (syms->LoadFromElf(*elfPath)) {
            Logger::Info("Debugger: loaded {} debug function symbols from sibling ELF: {}",
                         syms->FunctionCount(), *elfPath);
            doc->debugElfPath = *elfPath;
            doc->debugSymbols = std::move(syms);

            // Line-level info from the same ELF (DWARF .debug_line). Optional — a
            // stripped/older ELF may have symbols but no line program.
            auto lines = std::make_unique<core::DebugLines>();
            if (lines->LoadFromElf(*elfPath)) {
                Logger::Info("Debugger: loaded {} source line rows from {}", lines->RowCount(),
                             *elfPath);
                doc->debugLines = std::move(lines);
            }

            // Inline info (DWARF .debug_info) for the flame chart.
            auto inl = std::make_unique<core::DebugInline>();
            if (inl->LoadFromElf(*elfPath)) {
                Logger::Info("Debugger: loaded {} inline scopes from {}", inl->ScopeCount(),
                             *elfPath);
                doc->debugInline = std::move(inl);
            }

            // Call-frame info (DWARF .debug_frame) for unwinding the live stack.
            auto frames = std::make_unique<core::DebugFrame>();
            if (frames->LoadFromElf(*elfPath)) {
                Logger::Info("Debugger: loaded {} call-frame descriptions from {}",
                             frames->FdeCount(), *elfPath);
                doc->debugFrame = std::move(frames);
            }
        } else {
            Logger::Warn("Debugger: found sibling .elf but could not parse its symbol table: {}",
                         *elfPath);
        }
    }

    // The Exocet sync pack, found by the same rules as the .elf. Only JSON is read here; nothing
    // in the pack's kit runs at load. Not looked for while Exocet is hidden.
    if (auto exocetPath =
            core::PrivateFeatures() ? core::ExocetPack::FindSibling(path) : std::nullopt) {
        auto pack = std::make_shared<core::ExocetPack>();
        std::string error;
        if (pack->Load(*exocetPath, &error)) {
            Logger::Info("Exocet: loaded {} tracks, {} keys from {}", pack->Tracks().size(),
                         pack->KeyCount(), *exocetPath);
            doc->exocet = std::move(pack);
        } else {
            Logger::Warn("Exocet: {}", error);
            doc->exocetError = error;
        }
    }

    m_userPrefs.AddRecentFile(path); // Add original path to recent files

    DiskDocument* docPtr = doc.get();

    // Shared post-mount setup (services, searcher, identifier, bootblock).
    FinalizeMountedDocument(docPtr);

    // Add to documents list and switch to it
    m_documents.push_back(std::move(doc));
    SwitchActiveDocument(docPtr);
    UpdateDocumentViews(docPtr);

    return true;
}

bool Application::LoadHardfileDocument(std::unique_ptr<DiskDocument> doc, const std::string& path,
                                       bool compressed)
{
    // The whole image lives in memory (MemoryDevice), so there is a ceiling. A larger image can
    // still be mounted on DH0/DH1 - the emulator reads it from disk.
    constexpr uint64_t kMaxDocumentBytes = 1ull << 30;
    std::vector<uint8_t> bytes;
    std::error_code ec;
    if (compressed) {
        std::string error;
        switch (core::HardfileInspector::Gunzip(std::filesystem::u8path(path), kMaxDocumentBytes,
                                                bytes, error)) {
            case core::HardfileInspector::GunzipResult::Ok:
                break;
            case core::HardfileInspector::GunzipResult::CannotOpen:
                m_errorMessage = fmt::format("Cannot open the compressed hardfile '{}'.", path);
                m_showErrorModal = true;
                return false;
            case core::HardfileInspector::GunzipResult::TooLarge:
                m_errorMessage = "This hardfile decompresses to more than 1 GB, too large to "
                                 "open as a document. It can still be mounted on DH0/DH1.";
                m_showErrorModal = true;
                return false;
            case core::HardfileInspector::GunzipResult::Damaged:
                // not a shorter image that looks whole: a Save As would write it out truncated
                m_errorMessage = fmt::format("The compressed hardfile is damaged: {}", error);
                m_showErrorModal = true;
                return false;
        }
    } else {
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            Logger::Error("Failed to open file: {}", path);
            return false;
        }
        if (size > kMaxDocumentBytes) {
            m_errorMessage = fmt::format(
                "'{}' is {:.0f} MB, too large to open as a document (the limit is 1024 MB). It "
                "can still be mounted on DH0/DH1 in the Hard Drives window.",
                std::filesystem::path(path).filename().string(),
                static_cast<double>(size) / (1024.0 * 1024.0));
            m_showErrorModal = true;
            return false;
        }
        std::ifstream in(path, std::ios::binary);
        bytes.resize(static_cast<size_t>(size));
        if (!in || (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()),
                                         static_cast<std::streamsize>(size)))) {
            Logger::Error("Failed to read file: {}", path);
            return false;
        }
    }
    if (bytes.size() < 512) {
        m_errorMessage = "The hardfile is smaller than one block.";
        m_showErrorModal = true;
        return false;
    }

    if (!doc->volume.Mount(std::move(bytes))) {
        Logger::Error("Failed to mount hardfile: {}", path);
        return false;
    }
    doc->currentFilePath = path;
    if (compressed)
        doc->loadedHdzPath = path;
    Logger::Info("Loaded hardfile {} ({} partition(s))", path, doc->volume.GetPartitions().size());

    m_userPrefs.AddRecentFile(path);
    DiskDocument* docPtr = doc.get();
    FinalizeMountedDocument(docPtr);
    if (compressed) {
        // Saving back would have to recompress; a .hdz opens protected and saves as a .hdf.
        docPtr->volume.SetWriteProtected(true);
        ApplyDocumentWriteProtection(docPtr);
    }
    RefreshPartitionChoices(docPtr);
    m_documents.push_back(std::move(doc));
    SwitchActiveDocument(docPtr);
    UpdateDocumentViews(docPtr);
    return true;
}

void Application::RefreshPartitionChoices(DiskDocument* doc)
{
    if (!doc || !doc->fileListView)
        return;
    if (!doc->IsHardfile()) {
        doc->fileListView->SetPartitions({}, 0);
        return;
    }
    std::vector<ui::FileListView::PartitionChoice> choices;
    choices.push_back({fmt::format("Whole image ({} blocks, raw)",
                                   [&] {
                                       const auto& content = doc->volume.GetImageContent();
                                       return content.size() / 512;
                                   }()),
                       true});
    for (const auto& partition : doc->volume.GetPartitions()) {
        const double megabytes =
            static_cast<double>(partition.lastBlock - partition.firstBlock + 1) * 512.0 /
            (1024.0 * 1024.0);
        choices.push_back(
            {fmt::format("{}  {}  {:.1f} MB  (blocks {}-{})", partition.name, partition.filesystem,
                         megabytes, partition.firstBlock, partition.lastBlock),
             partition.dosFilesystem});
    }
    doc->fileListView->SetPartitions(std::move(choices), doc->volume.GetSelectedPartition());
}

void Application::UpdateHardfileDocumentRuntime(DiskDocument* doc, core::DiskInfo& diskInfo)
{
    const auto& service = m_emulatorView->GetEmulator()->GetHostMountService();
    const bool running = m_emulatorView->IsRunning();
    auto& cache = doc->hardfileRuntime;

    // Which effective mount, if any, is this document's file is settled once per launch (the
    // service's generation moves on every MarkEffective and ClearEffective), per run state and
    // per path - not per frame. std::filesystem::equivalent opens both files each time it is
    // asked, and this runs for every open hardfile document on every frame.
    const uint64_t generation = service.GetEffectiveGeneration();
    if (cache.generation != generation || cache.running != running ||
        cache.path != doc->currentFilePath) {
        cache.generation = generation;
        cache.running = running;
        cache.path = doc->currentFilePath;
        cache.device.reset();
        cache.readWrite = false;
        cache.status.clear();
        cache.lockReason.clear();
        if (running && !doc->currentFilePath.empty()) {
            std::error_code ec;
            for (const auto& mount : service.GetEffectiveMounts()) {
                if (!mount.enabled || !mount.IsHardfile())
                    continue;
                if (!std::filesystem::equivalent(mount.hardfilePath, doc->currentFilePath, ec))
                    continue;
                const char* name = core::HostMountDeviceName(mount.device);
                cache.device = mount.device;
                cache.readWrite = !mount.readOnly;
                cache.status = fmt::format("{}: in the emulator, {}", name,
                                           cache.readWrite ? "read-write" : "read-only");
                if (cache.readWrite)
                    cache.lockReason = fmt::format(
                        "Locked while the emulator has this hardfile read-write on {}. Stop the "
                        "machine or mount it read-only to edit it here; reopen the document to "
                        "see the guest's changes.",
                        name);
                break;
            }
        }
        doc->fileListView->SetWriteProtectLockReason(cache.lockReason);
    }

    if (cache.device) {
        const auto& activity = service.GetActivity()[static_cast<size_t>(*cache.device)];
        diskInfo.hostReads = activity.reads;
        diskInfo.hostWrites = activity.writes;
        diskInfo.hostActivity = service.GetRecentActivityStatus(*cache.device);
        diskInfo.runtimeStatus = cache.status;
    }

    // Two writers on one file corrupt it: the emulator writes the image on disk while this
    // document holds its own copy in memory and would save that copy back over the top.
    const bool lock = cache.device && cache.readWrite;
    if (lock && !doc->writeProtectBeforeEmulatorLock) {
        doc->writeProtectBeforeEmulatorLock = doc->volume.IsWriteProtected();
        doc->volume.SetWriteProtected(true);
        ApplyDocumentWriteProtection(doc);
        Logger::Info("'{}' is locked while the emulator has it read-write on {}",
                     doc->currentFilePath, core::HostMountDeviceName(*cache.device));
    } else if (!lock && doc->writeProtectBeforeEmulatorLock) {
        doc->volume.SetWriteProtected(*doc->writeProtectBeforeEmulatorLock);
        doc->writeProtectBeforeEmulatorLock.reset();
        ApplyDocumentWriteProtection(doc);
    }
}

void Application::SelectDocumentPartition(DiskDocument* doc, int index)
{
    if (!doc || !doc->IsHardfile())
        return;
    const auto partitions = doc->volume.GetPartitions();
    if (!doc->volume.SelectPartition(index) && index >= 0) {
        const std::string name = index < static_cast<int>(partitions.size())
                                     ? partitions[static_cast<size_t>(index)].name
                                     : std::to_string(index);
        m_errorMessage = fmt::format("Partition {} has no filesystem the disk manager can read. "
                                     "Showing the whole image as raw blocks instead.",
                                     name);
        m_showErrorModal = true;
    }
    doc->lastHexViewBlock = 0;
    ConfigureDocumentSearch(doc);
    m_advancedFindView->Clear();
    RefreshPartitionChoices(doc);
    UpdateDocumentViews(doc);
    if (doc == m_activeDocument)
        m_hexEditorView->GoToBlock(0);
}

bool Application::LoadAdfFromTosec(std::vector<uint8_t> data, const std::string& filename)
{
    Logger::Info("TOSEC: loading {} ({} bytes) as in-memory document", filename, data.size());

    bool isExe = false;
    auto adfBytes = ConvertToAdfBytes(std::move(data), filename, "", isExe);
    if (!adfBytes) {
        Logger::Error("TOSEC: failed to prepare ADF for '{}'", filename);
        return false;
    }

    uint32_t targetDockId = 0;
    if (ImGui::DockBuilderGetNode(kDocumentsPanelDockId) != nullptr)
        targetDockId = kDocumentsPanelDockId;

    auto doc = std::make_unique<DiskDocument>();
    doc->id = m_nextDocId++;
    doc->initialDockId = targetDockId;
    doc->fileListView = std::make_unique<ui::FileListView>();
    doc->fileListView->SetDocumentId(doc->id);
    if (targetDockId != 0)
        doc->fileListView->SetTargetDockId(targetDockId);
    doc->diskSearcher = std::make_unique<core::DiskSearcher>();

    if (!doc->volume.Mount(*adfBytes)) {
        Logger::Error("TOSEC: failed to mount ADF for '{}'", filename);
        return false;
    }

    doc->currentFilePath = filename;
    doc->loadedFromUrl = filename; // marks document as not on disk; redirects save → save-as
    if (isExe)
        doc->loadedExecutablePath = filename;

    core::DemosceneSource tosecSrc;
    tosecSrc.source = "tosec";
    tosecSrc.releaseName = filename;
    doc->demosceneSource = std::move(tosecSrc);

    DiskDocument* docPtr = doc.get();
    FinalizeMountedDocument(docPtr);

    m_documents.push_back(std::move(doc));
    SwitchActiveDocument(docPtr);
    UpdateDocumentViews(docPtr);

    Logger::Info("TOSEC: mounted '{}' as in-memory document", filename);
    return true;
}

DiskDocument* Application::CreateInMemoryDiskDocument(std::vector<uint8_t> adfBytes,
                                                      const std::string& displayName)
{
    uint32_t targetDockId = 0;
    if (ImGui::DockBuilderGetNode(kDocumentsPanelDockId) != nullptr)
        targetDockId = kDocumentsPanelDockId;

    auto doc = std::make_unique<DiskDocument>();
    doc->id = m_nextDocId++;
    doc->initialDockId = targetDockId;
    doc->fileListView = std::make_unique<ui::FileListView>();
    doc->fileListView->SetDocumentId(doc->id);
    if (targetDockId != 0)
        doc->fileListView->SetTargetDockId(targetDockId);
    doc->diskSearcher = std::make_unique<core::DiskSearcher>();

    if (!doc->volume.Mount(adfBytes)) {
        Logger::Error("New disk: failed to mount in-memory ADF");
        return nullptr;
    }

    // No backing file on disk yet: mark unsaved so the tab shows [modified] and
    // Save redirects to Save As. currentFilePath stays empty until the user saves.
    doc->forceUnsaved = true;
    doc->fileListView->SetFilename(displayName);

    DiskDocument* docPtr = doc.get();
    FinalizeMountedDocument(docPtr);
    // A disk built here opens writable, whatever the "open protected" preference says:
    // there is no file yet to protect, the caller writes the volume label, the
    // startup-sequence and the boot block into it immediately, and Save redirects to
    // Save As. Protection is for images that already exist.
    docPtr->volume.SetWriteProtected(false);
    ApplyDocumentWriteProtection(docPtr);

    m_documents.push_back(std::move(doc));
    SwitchActiveDocument(docPtr);
    UpdateDocumentViews(docPtr);
    return docPtr;
}

std::optional<std::vector<uint8_t>>
Application::ConvertToAdfBytes(std::vector<uint8_t> bytes, const std::string& filename,
                               std::string fileType, bool& outIsExecutable, AdfDecodeInfo* outInfo,
                               int zipDepth)
{
    outIsExecutable = false;
    if (outInfo)
        *outInfo = AdfDecodeInfo{};

    // Case-insensitive filename suffix test so local opens of FILE.DMS / FILE.ZIP
    // are detected the same as their lowercase counterparts.
    std::string lowerName = filename;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    auto endsWith = [&](const std::string& suffix) {
        return lowerName.size() >= suffix.size() &&
               lowerName.compare(lowerName.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    // Detect HUNK executables by magic bytes when the caller didn't set fileType.
    if (fileType != "executable" && bytes.size() >= 4) {
        uint32_t magic = (static_cast<uint32_t>(bytes[0]) << 24) |
                         (static_cast<uint32_t>(bytes[1]) << 16) |
                         (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
        if (magic == 0x000003F3)
            fileType = "executable";
    }

    if (fileType == "executable") {
        if (outInfo)
            outInfo->kind = AdfSourceKind::Executable;
        auto adfBytes = AmigaEmulator::CreateBootableADF(bytes, filename);
        if (adfBytes.empty()) {
            Logger::Error("Download: failed to create bootable ADF from executable '{}'", filename);
            return std::nullopt;
        }
        outIsExecutable = true;
        return adfBytes;
    }

    if (endsWith(".dms") || (bytes.size() > 4 && bytes[0] == 'D' && bytes[1] == 'M' &&
                             bytes[2] == 'S' && bytes[3] == '!')) {
        if (outInfo)
            outInfo->kind = AdfSourceKind::Dms;
        auto decompressed = DmsConverter::ConvertToBuffer(bytes);
        if (!decompressed) {
            Logger::Error("Download: failed to decompress DMS file '{}'", filename);
            return std::nullopt;
        }
        return std::move(*decompressed);
    }

    if (endsWith(".ipf") || (bytes.size() > 4 && bytes[0] == 'C' && bytes[1] == 'A' &&
                             bytes[2] == 'P' && bytes[3] == 'S')) {
        if (outInfo) {
            outInfo->kind = AdfSourceKind::Ipf;
            // Keep the raw IPF so the emulator can read it at flux level (copy
            // protection survives); the decoded ADF below is for browsing only.
            outInfo->rawImageBytes = bytes;
        }
        std::vector<core::IpfTrackInfo> tracks;
        auto adfBytes = IpfConverter::ConvertToBuffer(bytes, &tracks);
        if (!adfBytes) {
            // CapsImageLoader::LastError() holds the user-facing reason (often a
            // missing capsimg plugin); LoadAdf surfaces it in the error modal.
            Logger::Error("Failed to decode IPF file '{}': {}", filename,
                          core::CapsImageLoader::Instance().LastError());
            return std::nullopt;
        }
        if (outInfo)
            outInfo->ipfTracks = std::move(tracks);
        return std::move(*adfBytes);
    }

    // SuperCard Pro flux dump. Every track is raw flux, so the decoded ADF below
    // only ever holds the tracks that happen to be standard AmigaDOS; the raw SCP
    // is what the emulator runs.
    if (core::ScpConverter::IsScp(bytes)) {
        if (outInfo) {
            outInfo->kind = AdfSourceKind::Scp;
            outInfo->rawImageBytes = bytes;
        }
        std::vector<core::IpfTrackInfo> tracks;
        auto adfBytes = core::ScpConverter::ConvertToBuffer(bytes, &tracks);
        if (!adfBytes) {
            Logger::Error("Failed to decode SuperCard Pro image '{}'", filename);
            return std::nullopt;
        }
        if (outInfo)
            outInfo->ipfTracks = std::move(tracks);
        return std::move(*adfBytes);
    }

    if (endsWith(".zip") || (bytes.size() > 3 && bytes[0] == 'P' && bytes[1] == 'K' &&
                             bytes[2] == 0x03 && bytes[3] == 0x04)) {
        if (outInfo)
            outInfo->kind = AdfSourceKind::Zip;

        // One level only. Nothing we fetch nests archives, and recursing on one is
        // how a small download turns into a zip bomb.
        if (zipDepth > 0) {
            Logger::Error("Refusing to open a ZIP nested inside '{}'", filename);
            return std::nullopt;
        }

        std::vector<uint8_t> member;
        std::string zipEntryName, zipError;
        if (!adm::ExtractFromZipBuffer(bytes, adm::ZipPayload::AnyDiskImage, member, zipEntryName,
                                       zipError)) {
            Logger::Error("Failed to extract a disk image from ZIP '{}': {}", filename, zipError);
            return std::nullopt;
        }
        Logger::Info("Extracted {} from ZIP", zipEntryName);

        // The member is often not a plain ADF — TOSEC wraps IPF images and HUNK
        // executables in exactly the same kind of archive — so re-enter the pipeline
        // under the member's own name and let the format checks above decode it.
        auto decoded =
            ConvertToAdfBytes(std::move(member), zipEntryName, "", outIsExecutable, outInfo, 1);
        if (outInfo) {
            // The recursion reset outInfo, so put the ZIP provenance back on top of
            // the member's own kind. A plain ADF inside still reports as a ZIP load.
            if (outInfo->kind == AdfSourceKind::Adf)
                outInfo->kind = AdfSourceKind::Zip;
            outInfo->zipEntryName = zipEntryName;
        }
        return decoded;
    }

    // Extended ADF (UAE-1ADF). Detected by content magic — the extension is
    // usually a plain ".adf", so this must run before the raw-ADF fallthrough.
    if (core::ExtAdfConverter::IsExtAdf(bytes)) {
        if (outInfo) {
            outInfo->kind = AdfSourceKind::ExtAdf;
            // Keep the raw ext-ADF so the emulator can read it natively (copy
            // protection survives); the decoded ADF below is for browsing only.
            outInfo->rawImageBytes = bytes;
        }
        std::vector<core::IpfTrackInfo> tracks;
        auto adfBytes = core::ExtAdfConverter::ConvertToBuffer(bytes, &tracks);
        if (!adfBytes) {
            Logger::Error("Failed to decode extended ADF (UAE-1ADF) '{}'", filename);
            return std::nullopt;
        }
        if (outInfo)
            outInfo->ipfTracks = std::move(tracks);
        return std::move(*adfBytes);
    }

    return std::move(bytes);
}

std::shared_ptr<const std::vector<uint8_t>> Application::MakeIdentifySource(const DiskDocument* doc)
{
    // Backed by a real file: hash it from disk, as identification always has.
    std::error_code ec;
    if (!doc->currentFilePath.empty() && std::filesystem::is_regular_file(doc->currentFilePath, ec))
        return nullptr;

    // Otherwise currentFilePath is only a display name and opening it fails. Prefer
    // what was downloaded - TOSEC's hashes are over the file as distributed, so the
    // ADF a HUNK executable gets wrapped in would match nothing. Failing that (a
    // formatted disk, a drive read) the mounted image is the only thing there is,
    // and it is the right answer for a plain ADF.
    if (!doc->identifySourceBytes.empty())
        return std::make_shared<const std::vector<uint8_t>>(doc->identifySourceBytes);
    return std::make_shared<const std::vector<uint8_t>>(doc->volume.GetImageContent());
}

void Application::FinalizeMountedDocument(DiskDocument* doc)
{
    m_validationView->Close(); // Close any previous validation results

    // Create file operation service for this document
    doc->fileOpService = std::make_unique<FileOperationService>(doc->volume);

    doc->fileOpService->SetUpdateCallback([this, doc](bool preserveNav) {
        UpdateDocumentViews(doc, preserveNav);
        if (preserveNav && m_activeDocument == doc) {
            m_hexEditorView->GoToBlock(m_hexEditorView->GetCurrentBlock());
        }
    });
    doc->fileOpService->SetErrorCallback([this](const std::string& msg) {
        m_errorMessage = msg;
        m_showErrorModal = true;
    });

    ConfigureDocumentSearch(doc);

    m_advancedFindView->Clear();

    // Set up callbacks for this document's FileListView
    SetupDocumentCallbacks(doc);
    FinalizeDocumentIdentity(doc);
}

void Application::ConfigureDocumentSearch(DiskDocument* doc)
{
    doc->diskSearcher->SetDiskInfo(doc->volume.GetTotalBlocks(),
                                   [doc](uint32_t block) { return doc->volume.ReadBlock(block); });

    if (doc->volume.HasFileSystem()) {
        // Flatten the filesystem tree so the MOD searcher can read files properly
        // (handles non-contiguous blocks and OFS/FFS block headers)
        std::vector<core::DiskSearcher::FilesystemFileInfo> fsFiles;
        std::function<void(const FileNode&)> flatten = [&](const FileNode& node) {
            if (!node.isDir && !node.isDeleted) {
                fsFiles.push_back({node.name, (uint32_t)node.startSector, node.size});
            }
            for (const auto& child : node.children)
                flatten(child);
        };
        flatten(doc->volume.GetFileTree());
        doc->diskSearcher->SetFilesystemInfo(
            std::move(fsFiles), [doc](uint32_t headerBlock) -> std::vector<uint8_t> {
                std::string content = doc->volume.ReadFileContent(headerBlock);
                return std::vector<uint8_t>(content.begin(), content.end());
            });
    } else {
        doc->diskSearcher->SetFilesystemInfo({}, nullptr);
    }
}

void Application::FinalizeDocumentIdentity(DiskDocument* doc)
{
    // Create a per-document disk identifier. Always compute MD5/TOSEC immediately;
    // load full ScreenScraper data from cache if available.
    doc->identifier = std::make_unique<core::DiskIdentifier>();
    // Before anything hashes: a document with no file behind it has to be identified
    // from its bytes. This runs on every mount, including the automatic TOSEC pass
    // just below, so the bytes have to be attached here and not at the point the
    // user asks for a full identify.
    doc->identifier->SetSourceBytes(MakeIdentifySource(doc));
    if (!doc->currentFilePath.empty()) {
        if (doc->identifier->IsInCache(doc->currentFilePath)) {
            Logger::Info("Disk already identified in cache, loading in background: {}",
                         doc->currentFilePath);
            doc->identifier->StartIdentify(doc->currentFilePath);
        } else {
            doc->identifier->StartTosecOnly(doc->currentFilePath);
        }
    }

    // Identify bootblock and check for viruses
    auto sector0 = doc->volume.ReadBlock(0);
    auto sector1 = doc->volume.ReadBlock(1);
    doc->bootBlockInfo = m_brainFile.IdentifyBootBlock(sector0, sector1);

    if (doc->bootBlockInfo.isVirus) {
        Logger::Warn("VIRUS DETECTED: {}", doc->bootBlockInfo.name);
        m_showVirusAlert = true;
    } else if (doc->bootBlockInfo.identified) {
        Logger::Info("Bootblock identified: {} ({})", doc->bootBlockInfo.name,
                     doc->bootBlockInfo.className);
    }

    // Pass bootblock info to hex / boot block views
    m_hexEditorView->SetBootBlockInfo(doc->bootBlockInfo);
    m_bootBlockView->SetData(sector0, sector1);
    m_bootBlockView->SetBootBlockInfo(doc->bootBlockInfo);
}

void Application::StartDownloadDocument(const core::DownloadRequest& req,
                                        const core::DemosceneSource& source)
{
    // Remote loads can arrive while the app is in the background — bring it forward.
    FocusWindow();

    uint32_t targetDockId = 0;
    if (ImGui::DockBuilderGetNode(kDocumentsPanelDockId) != nullptr)
        targetDockId = kDocumentsPanelDockId;

    // Create the placeholder document/tab immediately — it shows download progress
    // and is upgraded in place to a normal disk view once the transfer completes.
    auto doc = std::make_unique<DiskDocument>();
    doc->id = m_nextDocId++;
    doc->initialDockId = targetDockId;
    doc->fileListView = std::make_unique<ui::FileListView>();
    doc->fileListView->SetDocumentId(doc->id);
    if (targetDockId != 0)
        doc->fileListView->SetTargetDockId(targetDockId);
    doc->fileListView->SetFilename(req.filename);  // tab title while downloading
    doc->fileListView->SetDemosceneSource(source); // source line in placeholder
    doc->diskSearcher = std::make_unique<core::DiskSearcher>();

    doc->currentFilePath = req.filename; // display name; tab shows it while downloading
    doc->demosceneSource = source;
    doc->loadState = core::DocLoadState::Downloading;
    doc->downloadRequest = req;
    doc->download = m_downloadManager.Start(req);

    // Wire close/retry handlers so the placeholder tab is interactive.
    SetupDocumentCallbacks(doc.get());

    DiskDocument* docPtr = doc.get();
    m_documents.push_back(std::move(doc));
    SwitchActiveDocument(docPtr);
    docPtr->fileListView->RequestFocus();

    Logger::Info("Download: started '{}' from {}", req.filename, req.url);
}

void Application::PollDownloads()
{
    for (auto& docPtr : m_documents) {
        DiskDocument* doc = docPtr.get();
        if (doc->loadState != core::DocLoadState::Downloading || !doc->download)
            continue;

        core::DownloadState state = doc->download->state.load();
        if (state == core::DownloadState::Error || state == core::DownloadState::Canceled) {
            doc->loadState = core::DocLoadState::Failed;
            std::string err;
            {
                std::lock_guard<std::mutex> lk(doc->download->mutex);
                err = doc->download->error;
            }
            Logger::Error("Download: failed for '{}' — {}", doc->downloadRequest.filename, err);
            continue;
        }
        if (state != core::DownloadState::Done)
            continue;

        // Transfer finished — take the bytes and mount in place.
        std::vector<uint8_t> bytes;
        {
            std::lock_guard<std::mutex> lk(doc->download->mutex);
            bytes = std::move(doc->download->data);
        }

        const std::string& filename = doc->downloadRequest.filename;

        // Keep what arrived off the wire before the decode consumes it: nothing else
        // in the app can reproduce it, and it is the only thing the disk identifier
        // can meaningfully hash for a document with no file behind it.
        doc->identifySourceBytes = bytes;

        bool isExe = false;
        AdfDecodeInfo decodeInfo;
        auto adfBytes = ConvertToAdfBytes(std::move(bytes), filename, doc->downloadRequest.fileType,
                                          isExe, &decodeInfo);
        if (!adfBytes || !doc->volume.Mount(*adfBytes)) {
            doc->loadState = core::DocLoadState::Failed;
            {
                std::lock_guard<std::mutex> lk(doc->download->mutex);
                if (doc->download->error.empty())
                    doc->download->error = "Failed to mount the downloaded disk image";
            }
            Logger::Error("Download: could not mount '{}'", filename);
            continue;
        }

        doc->loadedFromUrl = doc->downloadRequest.url;
        if (isExe)
            doc->loadedExecutablePath = filename;
        if (decodeInfo.kind == AdfSourceKind::Ipf) {
            // Mark IPF provenance, retain the raw image for native emulation, and
            // populate the embedded Track Map tab (same as a local IPF open).
            doc->loadedIpfPath = filename;
            doc->ipfTracks = std::move(decodeInfo.ipfTracks);
            doc->emulatorImageBytes = std::move(decodeInfo.rawImageBytes);
            doc->fileListView->SetIpfTracks(doc->ipfTracks);
        } else if (decodeInfo.kind == AdfSourceKind::ExtAdf) {
            doc->loadedExtAdfPath = filename;
            doc->ipfTracks = std::move(decodeInfo.ipfTracks);
            doc->emulatorImageBytes = std::move(decodeInfo.rawImageBytes);
            doc->fileListView->SetIpfTracks(doc->ipfTracks);
        } else if (decodeInfo.kind == AdfSourceKind::Scp) {
            doc->loadedScpPath = filename;
            doc->ipfTracks = std::move(decodeInfo.ipfTracks);
            doc->emulatorImageBytes = std::move(decodeInfo.rawImageBytes);
            doc->fileListView->SetIpfTracks(doc->ipfTracks);
        }
        doc->loadState = core::DocLoadState::Ready;
        doc->download.reset();

        FinalizeMountedDocument(doc);

        // While the placeholder was unmounted, SwitchActiveDocument took its empty-view
        // branch and skipped wiring the disk searcher / identify view. If this doc is
        // still active, force a full re-switch now that the volume is mounted.
        if (m_activeDocument == doc) {
            m_activeDocument = nullptr; // bypass the equality early-return
            SwitchActiveDocument(doc);
        }
        UpdateDocumentViews(doc);

        Logger::Info("Download: mounted '{}' as in-memory document", filename);
    }
}

void Application::PopulateDataTypes(DiskDocument* doc, FileNode& node)
{
    // For files, read enough bytes to identify the data type
    if (!node.isDir && node.size > 0) {
        std::string content = doc->volume.ReadFileContent(static_cast<uint32_t>(node.startSector));
        if (!content.empty()) {
            size_t headerSize = std::min(content.size(), static_cast<size_t>(2048));
            std::span<const uint8_t> headerData(reinterpret_cast<const uint8_t*>(content.data()),
                                                headerSize);

            auto result = m_dataTypeManager.Identify(headerData);
            if (result.IsValid()) {
                node.dataType = result.name;
                node.dataTypeGroup = result.group;

                // For executables, scan for RomTag to determine if it's a library/device/etc.
                if (result.group == "Executable") {
                    std::vector<uint8_t> fileData(content.begin(), content.end());
                    ResidentType resType = HunkParser::DetectResidentType(fileData);

                    switch (resType) {
                        case ResidentType::Library:
                            node.dataType = "Amiga Library";
                            break;
                        case ResidentType::Device:
                            node.dataType = "Amiga Device";
                            break;
                        case ResidentType::Handler:
                            node.dataType = "Amiga Handler";
                            break;
                        case ResidentType::Resource:
                            node.dataType = "Amiga Resource";
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }

    // Recurse into children
    for (auto& child : node.children) {
        PopulateDataTypes(doc, child);
    }
}

void Application::ProcessDiskWriteEvents()
{
    if (!m_emulatorView->IsRunning())
        return;

    auto* emu = m_emulatorView->GetEmulator();
    if (!emu || emu->GetDiskWriteEventCount() == 0)
        return;

    // If events are in the ring buffer, write-redirect is already off in UAE
    // (adm_record_disk_write is only called when !opt_floppy_write_redirect).
    // No need to re-check the user variable here.

    constexpr int kTrackBytes = 11 * 512; // 5632 bytes per DD track
    if (m_trackScratch.size() != static_cast<size_t>(kTrackBytes))
        m_trackScratch.resize(kTrackBytes);
    uint8_t* trackBuf = m_trackScratch.data();

    int count = emu->GetDiskWriteEventCount();
    const auto* events = emu->GetDiskWriteEvents();
    bool anyUpdated = false;

    for (int i = 0; i < count; i++) {
        int driveIdx = static_cast<int>(events[i].drive);
        int track = static_cast<int>(events[i].track);

        // Map drive index → document via EmulatorView DriveState
        const auto& ds = m_emulatorView->GetDriveState(driveIdx);
        if (!ds.hasDisk || ds.documentId == 0)
            continue;

        // Find the document matching this drive's documentId
        DiskDocument* doc = nullptr;
        for (auto& d : m_documents) {
            if (d->id == ds.documentId) {
                doc = d.get();
                break;
            }
        }
        if (!doc || !doc->volume.IsMounted())
            continue;
        // AdfHandler would refuse every WriteBlock below anyway; skip the track read
        // rather than do the work for nothing. With a core that honours per-drive
        // write protect the guest never gets this far.
        if (doc->IsWriteProtected())
            continue;

        int bytesRead = emu->ReadDiskTrack(driveIdx, track, trackBuf, kTrackBytes);
        if (bytesRead <= 0)
            continue;

        int firstBlock = track * 11;
        int numSectors = bytesRead / 512;
        std::vector<uint8_t> blockVec(512);

        for (int s = 0; s < numSectors; s++) {
            blockVec.assign(trackBuf + s * 512, trackBuf + (s + 1) * 512);
            doc->volume.WriteBlock(static_cast<uint32_t>(firstBlock + s), blockVec);
        }
        anyUpdated = true;
    }

    if (!anyUpdated)
        return;

    // Rate-limit reprocessing to at most once per second during emulation.
    // When the emulator is paused, update immediately for instant feedback.
    bool isPaused = m_emulatorView->IsPaused();
    auto now = std::chrono::steady_clock::now();
    double secsSince = std::chrono::duration<double>(now - m_lastWriteRedirectUpdate).count();

    if (isPaused || secsSince >= 1.0) {
        m_lastWriteRedirectUpdate = now;
        for (auto& d : m_documents) {
            if (d->HasUnsavedChanges())
                UpdateDocumentViews(d.get(), /*preserveNavigation=*/true);
        }
    }
}

void Application::UpdateDocumentViews(DiskDocument* doc, bool preserveNavigation)
{
    if (!doc)
        return;

    // Set the filename (without path) for display
    if (!doc->currentFilePath.empty()) {
        std::string filename = std::filesystem::path(doc->currentFilePath).filename().string();
        doc->fileListView->SetFilename(filename);
    }
    doc->fileListView->SetDemosceneSource(doc->demosceneSource);

    auto root = doc->volume.GetFileTree();

    // Populate data types for all files
    PopulateDataTypes(doc, root);

    // Add deleted files as a special folder
    auto deletedFiles = doc->volume.GetDeletedFiles();
    if (!deletedFiles.empty()) {
        FileNode deletedFolder;
        deletedFolder.name = "[Deleted Files]";
        deletedFolder.isDir = true;
        deletedFolder.isDeleted = true;
        deletedFolder.size = 0;
        deletedFolder.startSector = -1;
        deletedFolder.parentSector = 0;
        deletedFolder.children = std::move(deletedFiles);

        for (auto& child : deletedFolder.children) {
            PopulateDataTypes(doc, child);
        }

        root.children.push_back(std::move(deletedFolder));
    }

    doc->fileListView->SetRoot(root);

    // Only update shared views if this is the active document
    if (doc == m_activeDocument) {
        auto info = doc->volume.GetDiskInfo();
        auto rootInfo = doc->volume.GetRootBlockInfo();
        uint32_t blockSize = rootInfo.blockSize;
        if (blockSize == 0) {
            blockSize = 512;
        }
        m_hexEditorView->SetBlockSize(blockSize);
        m_hexEditorView->SetGeometry(info.heads, info.sectors);

        uint32_t totalBlocks = info.cylinders * info.heads * info.sectors;
        uint32_t rootBlock = rootInfo.rootPtr;

        if (rootBlock == 0) {
            auto bootData = doc->volume.ReadBlock(0);
            if (bootData.size() >= 12) {
                rootBlock = ((uint32_t)bootData[8] << 24) | ((uint32_t)bootData[9] << 16) |
                            ((uint32_t)bootData[10] << 8) | (uint32_t)bootData[11];
            }
        }

        if (rootBlock == 0 && totalBlocks > 0) {
            rootBlock = totalBlocks / 2;
        }

        m_hexEditorView->SetDiskLayout(totalBlocks, rootBlock);
        m_hexEditorView->SetIsFFS(info.type == "FFS");
        m_hexEditorView->SetBootBlockInfo(doc->bootBlockInfo);

        m_bitmapView->SetData(info, doc->volume.GetBlockUsage());
        m_blockMapView->SetData(info, doc->volume.GetAllBlockTypes());
        m_ndosView->SetData(info, doc->volume.GetZeroFilledBlocks());
        m_changeMapView->SetData(info, doc->volume.GetBlockModificationStatus());

        if (!preserveNavigation) {
            m_hexEditorView->GoToBlock(doc->lastHexViewBlock);
        } else {
            uint32_t currentBlock = m_hexEditorView->GetCurrentBlock();
            if (totalBlocks > 0 && currentBlock >= totalBlocks) {
                currentBlock = totalBlocks - 1;
            }
            m_hexEditorView->GoToBlockForce(currentBlock);
        }
    }
}

// Resolve a document id the way the Floppy Disk I/O window means it: 0 is "whichever document
// is active right now", any other value pins that document, and a pinned id that no longer
// exists resolves to nothing so the caller can fall back rather than write the wrong disk.
const DiskDocument* Application::FindWriteDocument(uint32_t id) const
{
    if (id == 0)
        return m_activeDocument;
    for (const auto& doc : m_documents) {
        if (doc->id == id)
            return doc.get();
    }
    return nullptr;
}

void Application::SwitchActiveDocumentById(uint32_t id)
{
    for (auto& doc : m_documents) {
        if (doc->id == id) {
            SwitchActiveDocument(doc.get());
            // Force focus to the FileListView of this document
            if (doc->fileListView) {
                doc->fileListView->RequestFocus();
            }
            break;
        }
    }
}

void Application::SwitchActiveDocument(DiskDocument* doc)
{
    if (m_activeDocument == doc)
        return;

    if (m_activeDocument && m_hexEditorView) {
        m_activeDocument->lastHexViewBlock = m_hexEditorView->GetCurrentBlock();
    }

    m_activeDocument = doc;

    // The hex editor is shared between documents, so its read-only state has to follow
    // whichever one is now active - otherwise it keeps the previous document's.
    if (m_hexEditorView)
        m_hexEditorView->SetReadOnly(
            doc && doc->kind != DocumentKind::HostDirectory ? doc->IsWriteProtected() : false);

    // A null document, or a download placeholder whose volume isn't mounted yet,
    // both render with empty shared views. The placeholder still becomes active so
    // its tab is focused and its dialogs/close behaviour target it.
    if (!doc || !doc->volume.IsMounted()) {
        m_hexEditorView->SetData({});
        m_hexEditorView->SetDiskLayout(0, 0);
        m_hexEditorView->SetDiskSearcher(nullptr);
        DiskInfo emptyInfo{};
        m_bitmapView->SetData(emptyInfo, {});
        m_blockMapView->SetData(emptyInfo, {});
        m_ndosView->SetData(emptyInfo, {});
        m_changeMapView->SetData(emptyInfo, {});
        m_advancedFindView->SetDiskSearcher(nullptr);
        m_advancedFindView->SetDocumentId(0);
        m_advancedFindView->Clear();
        m_validationView->Close();
        m_bootBlockView->SetData({}, {});
        m_bootBlockView->SetBootBlockInfo(s_emptyBootBlockInfo);
        m_bootBlockView->SetDocumentLoaded(false);
        m_identifyView->Clear();
        return;
    }

    // Push document's data to shared views
    auto info = doc->volume.GetDiskInfo();
    auto rootInfo = doc->volume.GetRootBlockInfo();
    uint32_t blockSize = rootInfo.blockSize;
    if (blockSize == 0) {
        blockSize = 512;
    }
    m_hexEditorView->SetBlockSize(blockSize);
    m_hexEditorView->SetGeometry(info.heads, info.sectors);

    uint32_t totalBlocks = info.cylinders * info.heads * info.sectors;
    uint32_t rootBlock = rootInfo.rootPtr;

    if (rootBlock == 0) {
        auto bootData = doc->volume.ReadBlock(0);
        if (bootData.size() >= 12) {
            rootBlock = ((uint32_t)bootData[8] << 24) | ((uint32_t)bootData[9] << 16) |
                        ((uint32_t)bootData[10] << 8) | (uint32_t)bootData[11];
        }
    }
    if (rootBlock == 0 && totalBlocks > 0) {
        rootBlock = totalBlocks / 2;
    }

    m_hexEditorView->SetDiskLayout(totalBlocks, rootBlock);
    m_hexEditorView->SetIsFFS(info.type == "FFS");
    m_hexEditorView->SetBootBlockInfo(doc->bootBlockInfo);

    m_bitmapView->SetData(info, doc->volume.GetBlockUsage());
    m_blockMapView->SetData(info, doc->volume.GetAllBlockTypes());
    m_ndosView->SetData(info, doc->volume.GetZeroFilledBlocks());
    m_changeMapView->SetData(info, doc->volume.GetBlockModificationStatus());

    // Wire up the disk searcher for the new active document
    m_hexEditorView->SetDiskSearcher(doc->diskSearcher.get());
    m_advancedFindView->SetDiskSearcher(doc->diskSearcher.get());
    m_advancedFindView->SetDocumentId(doc->id);

    // Navigate to last remembered block
    m_hexEditorView->GoToBlock(doc->lastHexViewBlock);

    // Update bootblock view
    auto sector0 = doc->volume.ReadBlock(0);
    auto sector1 = doc->volume.ReadBlock(1);
    m_bootBlockView->SetData(sector0, sector1);
    m_bootBlockView->SetBootBlockInfo(doc->bootBlockInfo);

    // Point identify view at the new document's identifier (non-blocking)
    m_identifyView->SetDocument(doc->currentFilePath, doc->identifier.get());

    // Issue 2 fix: if this document has TOSEC data but was missing SS/Pouet metadata when it was
    // last identified, another document of the same title group may have since populated the shared
    // cache.  Attempt a cache refresh so the user sees the metadata without re-identifying.
    if (doc->identifier)
        doc->identifier->RefreshFromCache();
}

void Application::QueueCloseDocument(DiskDocument* doc)
{
    if (std::find(m_deferredCloseDocuments.begin(), m_deferredCloseDocuments.end(), doc) ==
        m_deferredCloseDocuments.end()) {
        m_deferredCloseDocuments.push_back(doc);
    }
}

void Application::CloseDocument(DiskDocument* doc)
{
    if (!doc)
        return;

    // Cancel any in-flight download. The worker keeps the DownloadTask alive via its
    // own shared_ptr, so it finishes (and exits) on its own without blocking us here.
    if (doc->download)
        doc->download->cancel = true;

    // A directory-backed core keeps host filesystem handles independently of the document UI.
    // Stop it before destroying a document that represents one of its effective roots.
    if (doc->kind == core::DocumentKind::HostDirectory && doc->hostDirectory &&
        m_emulatorView->IsRunning()) {
        const auto& effective =
            m_emulatorView->GetEmulator()->GetHostMountService().GetEffectiveMounts();
        const auto root = doc->hostDirectory->GetCanonicalRoot();
        const bool referenced =
            !root.empty() && std::any_of(effective.begin(), effective.end(),
                                         [&](const core::HostDirectoryMount& mount) {
                                             return mount.enabled && mount.hostDirectory == root;
                                         });
        if (referenced) {
            Logger::Info("Closing host directory '{}' stops its active emulator session",
                         root.string());
            m_emulatorView->Stop();
        }
    }

    // Closing a floppy document behaves like removing the physical disk: eject it from every
    // drive that references the document, but leave the machine powered on.
    for (int d = 0; d < 4; d++) {
        if (m_emulatorView->GetDriveState(d).documentId == doc->id)
            m_emulatorView->EjectDrive(d);
    }

    // If identification is still running, orphan the identifier so it completes and caches
    if (doc->identifier) {
        auto state = doc->identifier->GetState();
        if (state == core::IdentifyState::Hashing ||
            state == core::IdentifyState::SearchingByHash ||
            state == core::IdentifyState::SearchingByName) {
            Logger::Info("Identification running for closed document '{}', keeping in background",
                         doc->currentFilePath);
            m_orphanedIdentifiers.push_back(std::move(doc->identifier));
        }
    }

    doc->volume.Unmount();

    // Remove from documents list
    auto it =
        std::find_if(m_documents.begin(), m_documents.end(),
                     [doc](const std::unique_ptr<DiskDocument>& d) { return d.get() == doc; });

    if (it != m_documents.end()) {
        bool wasActive = (doc == m_activeDocument);

        if (wasActive) {
            // Switch to another document or nullptr BEFORE deleting the current active document
            if (m_documents.size() > 1) {
                // Find a document that isn't the one we're about to delete
                auto nextDoc = (m_documents.back().get() == doc) ? m_documents.front().get()
                                                                 : m_documents.back().get();
                SwitchActiveDocument(nextDoc);
            } else {
                SwitchActiveDocument(nullptr);
            }
        }

        m_documents.erase(it);
    }

    Logger::Info("ADF file closed");
}

void Application::RunFramePumpIntegrations()
{
    if (ImGui::IsKeyPressed(ImGuiKey_F12, false) && ImGui::GetIO().KeyShift) {
        m_debuggerView->ToggleConsole();
    }

    // Keep MemorySearcher regions up to date with the live emulator memory
    UpdateMemorySearcherRegions();

    // Dispatch any load request from the Peekaboo Chrome extension (HTTP server thread)
    if (auto ev = m_httpServer.PopPendingLoad())
        OnPeekabooLoadRequested(*ev);

    // Dispatch compile-and-run requests from the Peekaboo VS Code integration.
    if (auto request = m_httpServer.PopPendingRun())
        OnPeekabooRunRequested(*request);

    // Run any pending MCP tool calls (from the MCP server thread) on the main thread, where the
    // emulator and GL context live.
    if (m_mcpServer.IsRunning() && m_mcpHandler) {
        m_mcpServer.DrainPending([this](const std::string& name, const nlohmann::json& args) {
            return m_mcpHandler->Handle(name, args);
        });
    }

    // Halt a stop-on-entry launch as soon as its seglist appears.
    m_debugSession.ServiceStopOnEntry();

    // Tell anyone polling /api/status whether the launched program is in memory yet.
    m_compiledProgram.PublishRunState();

    // Service any GDB packets queued by the RSP socket thread. Must run on this thread: it is
    // the only one allowed to touch the emulator. While the target is halted this drains a whole
    // burst within one frame, which is what keeps a GDB stack unwind from costing a second.
    m_gdbServer.Pump();

    // Convert + mount any download documents whose transfer has finished.
    PollDownloads();
}

void Application::RunFrameDrawWorkspace()
{
    // Ensure every panel's DockSpace host exists before re-asserting the saved tab selection: a
    // panel restored as a hidden tab otherwise never hosts its dockspace, leaving its docked
    // windows floating. While priming is promoting panels we skip re-assertion so it doesn't fight
    // the temporary selection changes.
    if (!m_workspaceLayout.PrimePanelDockHosts())
        m_workspaceLayout.ReassertTabSelection();

    // Render Toolbar at the top (below menu)
    RenderToolbar(m_uiScale);

    // Dockspace - effectively filling the rest of the screen. The returned id is the root of the
    // whole dock tree; DockSpaceOverViewport derives it from the viewport, so this is the only
    // place it can be learned. The MCP UI tools need it to walk and mutate the layout.
    m_mainDockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                                ImGuiDockNodeFlags_PassthruCentralNode);

    // Draw the permanent Documents panel first so its DockSpace is registered
    // before FileListViews try to dock into it.
    m_documentsPanel->Draw();

    // Draw user-created panel windows (each hosts an internal DockSpace)
    for (auto& panel : m_panelManager.Panels())
        panel->Draw();
    m_panelManager.DrawModals();

    // Handle deferred view updates (e.g., after file restore)
    if (m_pendingViewUpdate) {
        m_pendingViewUpdate = false;
        if (m_activeDocument) {
            UpdateDocumentViews(m_activeDocument);
        }
    }

    // Just-restored .peek documents: re-assert their dock into the Documents panel for a few
    // frames so the saved-layout reload can't strand them floating. The Documents panel drew
    // above, so its dockspace node exists now and ForceDockId takes effect on the Draw() below.
    if (m_restoreDockFrames > 0) {
        if (ImGui::DockBuilderGetNode(kDocumentsPanelDockId) != nullptr) {
            for (auto& docPtr : m_documents) {
                if (!docPtr->restoredWindowPlacement)
                    docPtr->fileListView->ForceDockId(kDocumentsPanelDockId);
            }
            m_restoreDockFrames--;
        }
    }
}

void Application::RunFrameDrawDocuments()
{
    // Draw all document FileListViews and per-document viewers
    for (auto& docPtr : m_documents) {
        auto* doc = docPtr.get();

        // Feed the live download snapshot to placeholder documents; clear it for
        // mounted ones so they render the normal disk view.
        if (doc->kind == core::DocumentKind::HostDirectory && doc->hostDirectory &&
            !doc->hostDirectory->IsAvailable()) {
            ui::DownloadStatus st;
            st.isError = true;
            st.showLocate = true;
            st.errorTitle = "Directory unavailable";
            st.errorMsg = fmt::format("The referenced directory '{}' could not be found.",
                                      doc->hostDirectory->GetOriginalRoot().string());
            doc->fileListView->SetDownloadStatus(std::move(st));
        } else if (doc->loadState != core::DocLoadState::Ready && doc->download) {
            auto* task = doc->download.get();
            ui::DownloadStatus st;
            st.isError = (doc->loadState == core::DocLoadState::Failed);
            st.progress = static_cast<float>(task->Progress());
            st.bytesReceived = task->bytesReceived.load();
            st.totalBytes = task->totalBytes.load();
            st.bytesPerSec = task->bytesPerSec.load();
            {
                std::lock_guard<std::mutex> lk(task->mutex);
                st.statusMsg = task->statusMsg;
                st.errorMsg = task->error;
            }
            if (doc->demosceneSource) {
                const auto& src = *doc->demosceneSource;
                if (src.source == "tosec") {
                    st.sourceName = "TOSEC";
                } else if (src.source == "demozoo") {
                    st.sourceName = "demozoo.org";
                    st.sourceUrl =
                        fmt::format("https://demozoo.org/productions/{}/", src.releaseId);
                } else if (src.source == "pouet") {
                    st.sourceName = "pouet.net";
                    st.sourceUrl =
                        fmt::format("https://www.pouet.net/prod.php?which={}", src.releaseId);
                } else if (src.source == "janeway") {
                    st.sourceName = "janeway.exotica.org.uk";
                    st.sourceUrl = fmt::format("https://janeway.exotica.org.uk/release.php?id={}",
                                               src.releaseId);
                }
            }
            doc->fileListView->SetDownloadStatus(std::move(st));
        } else {
            doc->fileListView->SetDownloadStatus(std::nullopt);
        }

        DiskInfo diskInfo{};
        if (doc->kind == core::DocumentKind::HostDirectory && doc->hostDirectory) {
            const auto now = std::chrono::steady_clock::now();
            if (doc->hostDirectory->IsAvailable() && now >= doc->nextHostRefresh) {
                doc->hostDirectory->RequestRefresh();
                doc->nextHostRefresh = now + std::chrono::seconds(1);
            }
            if (doc->hostDirectory->ApplyPendingRefresh())
                UpdateHostDirectoryDocument(doc);

            const auto& scan = doc->hostDirectory->GetScan();
            const auto clamp32 = [](uint64_t value) {
                return static_cast<uint32_t>(
                    std::min<uint64_t>(value, std::numeric_limits<uint32_t>::max()));
            };
            diskInfo.name = doc->hostDirectory->GetVolumeLabel().empty()
                                ? doc->hostDirectory->GetCanonicalRoot().filename().string()
                                : doc->hostDirectory->GetVolumeLabel();
            diskInfo.type = "Host Directory";
            if (const auto device = doc->hostDirectory->GetAssignedDevice()) {
                const auto& service = m_emulatorView->GetEmulator()->GetHostMountService();
                const auto state = service.GetRuntimeState(*device);
                const size_t index = static_cast<size_t>(*device);
                const auto& effectiveMount = service.GetEffectiveMounts()[index];
                const bool sameRoot =
                    effectiveMount.enabled &&
                    effectiveMount.hostDirectory == doc->hostDirectory->GetCanonicalRoot();
                const char* stateName = "assigned";
                if (sameRoot) {
                    switch (state) {
                        case core::HostMountRuntimeState::Confirmed:
                            stateName = "mounted by guest";
                            break;
                        case core::HostMountRuntimeState::Effective:
                            stateName = "effective; awaiting guest";
                            break;
                        case core::HostMountRuntimeState::PendingRestart:
                            stateName = "restart pending";
                            break;
                        case core::HostMountRuntimeState::Failed:
                            stateName = "failed";
                            break;
                        default:
                            stateName = "assigned";
                            break;
                    }
                    const auto& activity = service.GetActivity()[index];
                    diskInfo.hostReads = activity.reads;
                    diskInfo.hostWrites = activity.writes;
                    diskInfo.hostActivity = service.GetRecentActivityStatus(*device);
                } else if (state == core::HostMountRuntimeState::PendingRestart) {
                    stateName = "restart pending";
                }
                diskInfo.runtimeStatus =
                    fmt::format("{}: {}", core::HostMountDeviceName(*device), stateName);
            }
            if (doc->directoryFingerprintChanged)
                diskInfo.warning = "Directory contents changed since this session was saved.";
            diskInfo.sizeBytes = clamp32(scan.capacityBytes);
            diskInfo.freeBytes = clamp32(scan.freeBytes);
            diskInfo.usedBytes =
                clamp32(scan.capacityBytes - std::min(scan.capacityBytes, scan.freeBytes));
            diskInfo.usedPercentage =
                scan.capacityBytes == 0 ? 0.0f
                                        : static_cast<float>(scan.capacityBytes - scan.freeBytes) /
                                              static_cast<float>(scan.capacityBytes);
            diskInfo.isReadOnly = doc->hostDirectory->IsReadOnly();
        } else {
            diskInfo = doc->volume.GetDiskInfo();
            if (doc->IsHardfile())
                UpdateHardfileDocumentRuntime(doc, diskInfo);
        }
        doc->fileListView->SetWriteProtected(doc->IsWriteProtected());
        doc->fileListView->Draw(diskInfo, doc->HasUnsavedChanges(), m_uiScale);

        // Draw per-document file viewers
        for (auto it = doc->fileViewers.begin(); it != doc->fileViewers.end();) {
            if ((*it)->IsOpen()) {
                (*it)->Draw();
                ++it;
            } else {
                it = doc->fileViewers.erase(it);
            }
        }

        // Draw per-document image viewers
        for (auto it = doc->imageViewers.begin(); it != doc->imageViewers.end();) {
            if ((*it)->IsOpen()) {
                (*it)->Draw();
                ++it;
            } else {
                it = doc->imageViewers.erase(it);
            }
        }

        // Draw per-document audio players
        for (auto it = doc->audioPlayers.begin(); it != doc->audioPlayers.end();) {
            if ((*it)->IsOpen()) {
                (*it)->Draw();
                ++it;
            } else {
                it = doc->audioPlayers.erase(it);
            }
        }

        // Draw per-document hunk viewers
        for (auto it = doc->hunkViewers.begin(); it != doc->hunkViewers.end();) {
            if ((*it)->IsOpen()) {
                (*it)->Draw();
                ++it;
            } else {
                it = doc->hunkViewers.erase(it);
            }
        }
    }
}

void Application::RunFrameDrawSharedViews()
{
    // Draw dialogs only for the active document (not per-document)
    if (m_activeDocument) {
        m_activeDocument->fileListView->DrawDialogs();
    }

    // Detect focus change: find which FileListView has focus
    for (auto& docPtr : m_documents) {
        if (docPtr->fileListView->HasFocus() && docPtr.get() != m_activeDocument) {
            SwitchActiveDocument(docPtr.get());
            break;
        }
    }

    // Draw shared views (reflect active document)
    m_hexEditorView->Draw();

    m_bitmapView->Draw();
    m_ndosView->Draw();
    m_changeMapView->Draw();
    m_blockMapView->Draw();

    // Draw validation view if open
    const bool diskMounted = m_activeDocument && m_activeDocument->volume.IsMounted();
    m_validationView->SetDocumentLoaded(diskMounted);
    m_validationView->Draw();
    m_bootBlockView->SetDocumentLoaded(diskMounted);
    m_bootBlockView->Draw();
    UpdateBootBlockPreview();

    DrawErrorModal();
    DrawVirusAlertModal();
    m_unsavedChanges.Draw();
    m_aboutView->Draw();
    m_preferencesView->Draw();
    m_onboardingView->Draw();
    m_identifyView->SetDocumentLoaded(diskMounted);
    m_identifyView->Draw();

    // Issue 1 fix: when the active document's identification completes, force the TOSEC Browser
    // detail panel to re-check the cache for the currently selected entry (the panel only fires
    // a cache lookup on MD5 change, so it would miss metadata added by a just-finished identify).
    if (m_activeDocument && m_activeDocument->identifier) {
        auto curState = m_activeDocument->identifier->GetState();
        if (m_prevTrackedDoc == m_activeDocument &&
            m_prevIdentifyState != core::IdentifyState::Done &&
            curState == core::IdentifyState::Done) {
            m_tosecBrowserView->RefreshPanel();
        }
        m_prevTrackedDoc = m_activeDocument;
        m_prevIdentifyState = curState;
    } else {
        m_prevTrackedDoc = nullptr;
        m_prevIdentifyState = core::IdentifyState::Idle;
    }

    // Unconditional: a background floppy read can complete while the window is shut, and its
    // result must still reach the document layer.
    m_floppyDiskView->Poll();
    m_floppyDiskView->Draw();
    m_floppyDrivesView->Draw();
    m_emulatorView->SetRunWhenHidden(m_userPrefs.Get().emulatorRunWhenHidden);
    m_emulatorView->Draw();
    ProcessDiskWriteEvents();
}

void Application::RunFrameHarvestTelemetry()
{
    // An armed LOOP steps the machine itself, so this has to happen before the harvest
    // below or the replayed frame's telemetry would be a frame late.
    m_timeline.AdvancePlayback();

    // Deliberately outside the "emulator is running" block below: a trainer is matched against
    // the loaded media, which is known long before anything boots. Self-gating and a no-op
    // after the first lookup per document.
    MaybeDiscoverTrainer();

    // Feed debugger data only after a core run. ImGui may redraw several times between emulated
    // frames; the run serial also catches instruction steps and mid-frame breakpoint returns.
    if (m_emulatorView->IsRunning()) {
        auto* emu = m_emulatorView->GetEmulator();

        // Audio debug taps stage samples in lock-free rings; this is the only thread allowed to
        // write them to disk. Unconditional and cheap when no capture is active — it must not be
        // gated on the run-count check below, or a paused emulator would never flush its tail.
        emu->DrainAudioRecorders();

        // Cheap enough to check every frame (one integer compare) and it must not be gated on
        // the run-count below: a lock toggled while the machine is paused has to reach the core
        // before it runs again.
        SyncMemoryProtections();

        // Same reasoning: two bool compares, and it must not sit behind the run-count gate
        // below or capture armed while paused would never reach the core.
        m_blitOverlay.SyncDestCapture();
        m_blitOverlay.SyncProvenance();

        // Settings only - the recording itself rides the emulator's frame-complete hook.
        // Outside the run-count gate below so arming responds to a debugger window opening
        // while the machine is paused.
        m_timeline.UpdateSettings();

        const uint64_t emuRun = emu->GetRunCount();
        if (emuRun != m_lastDebuggerRun) {
            m_lastDebuggerRun = emuRun;
            m_blitProfiler->Update(m_emulatorView->IsRunning() ? m_emulatorView->GetEmulator()
                                                               : nullptr);
            {
                // Fetched here rather than inside the profiler so the profiler stays free of
                // any emulator/UI dependency (and unit-testable); same shape as copperBuf below.
                static adm::AmigaEmulator::RegWriteEvent
                    regWriteBuf[adm::AmigaEmulator::kMaxRegWriteEvents];
                const int n = m_emulatorView->GetRegWriteEvents(
                    regWriteBuf, adm::AmigaEmulator::kMaxRegWriteEvents);
                m_regWriteProfiler->UpdateFromEvents(regWriteBuf, n);
            }

            // Repaint the provenance overlay from this frame's spans. Cheap no-op when the
            // feature is off — the core reports zero spans and the texture is released.
            if (emu->IsBlitterVisEnabled())
                m_blitOverlay.UpdateProvenance(*emu);

            // Session-scoped, so this accumulates rather than rebuilding per frame.
            auto* view = m_emulatorView.get();
            m_diskProfiler->Update(
                emu->GetDiskDmaEvents(), emu->GetDiskDmaEventCount(),
                [view](uint32_t off, uint32_t len, uint8_t* out) {
                    return view->GetDiskDmaPayload(off, len, out);
                },
                static_cast<uint32_t>(emu->GetTotalFrameCount()), emu->GetFrameStartCycle());

            // Flag the transfers whose track actually landed in the backing ADF. This
            // runs after the ingest above so this frame's writes are already recorded.
            const int wcount = emu->GetDiskWriteEventCount();
            const auto* wevents = emu->GetDiskWriteEvents();
            for (int i = 0; i < wcount; ++i)
                m_diskProfiler->MarkPersisted(wevents[i].drive, wevents[i].track);

            if (m_copperEventScratch.empty())
                m_copperEventScratch.resize(adm::AmigaEmulator::kMaxCopperExecEvents);
            auto* copperBuf = m_copperEventScratch.data();
            int copperCount =
                emu->GetCopperExecEvents(copperBuf, adm::AmigaEmulator::kMaxCopperExecEvents);
            std::vector<adm::AmigaEmulator::CopperExecEvent> copperVec(copperBuf,
                                                                       copperBuf + copperCount);
            m_debuggerView->SetScrubberData(m_blitProfiler->GetCurrentFrame(), copperVec,
                                            emu->GetFrameStartCycle());

            // Bartman idle-region markers (debug_start_idle/stop_idle) → scrubber Idle row.
            if (m_idleEventScratch.empty())
                m_idleEventScratch.resize(adm::AmigaEmulator::kMaxIdleEvents);
            auto* idleBuf = m_idleEventScratch.data();
            int idleCount = emu->GetIdleEvents(idleBuf, adm::AmigaEmulator::kMaxIdleEvents);
            m_debuggerView->SetScrubberIdleEvents(
                std::vector<adm::AmigaEmulator::IdleEvent>(idleBuf, idleBuf + idleCount));

            // Mid-frame breakpoint: mark the scrubber with a playhead at the break beam position
            // and dim the not-yet-executed region. The event rows are already clipped to the break.
            const uint32_t breakLRef = static_cast<uint32_t>(emu->GetBreakVpos()) *
                                           adm::core::constants::beam::CCKPerLine +
                                       static_cast<uint32_t>(emu->GetBreakHpos());
            m_debuggerView->SetScrubberBreakMarker(emu->IsPartialFrame(), breakLRef);

            // Forward CPU trace (non-empty only when tracing is enabled).
            if (emu->IsCpuTraceEnabled()) {
                // DebuggerView takes one owned snapshot and shares it with its lazy consumers.
                m_debuggerView->SetCpuTraceData(emu->GetCpuTracePtr(), emu->GetCpuTraceCount(),
                                                emu->GetFrameStartCycle());

                // Fold the frame into the rolling history so find_in_trace and the
                // library-call tracer can look back past this frame. No-op unless
                // configure_cpu_trace_history turned recording on.
                m_cpuTraceHistory.AppendFrame(emu->GetCpuTracePtr(), emu->GetCpuTraceCount(),
                                              emu->GetTotalFrameCount(), emu->GetFrameStartCycle());
            }

            // Source-level debug info: when the active document has symbols from a sibling .elf,
            // recover the running program's load base by walking the AmigaDOS seglist.
            // Pushed every frame regardless of CPU tracing: the Disassembly window's
            // interleaved source needs the line table in Live mode, where no trace exists.
            const auto debugInfo = GetActiveDebugInfo();
            const core::DebugSymbols* functionSymbols = debugInfo.symbols;
            if (functionSymbols) {
                const auto regions = m_emulatorView->GetFreshMemoryRegions();
                auto readLong = [&regions](uint32_t addr, bool& ok) -> uint32_t {
                    for (const auto& r : regions) {
                        if (r.hostPtr && addr >= r.amigaAddr &&
                            static_cast<uint64_t>(addr) + 4 <= r.amigaAddr + r.size) {
                            const uint8_t* p = r.hostPtr + (addr - r.amigaAddr);
                            return (static_cast<uint32_t>(p[0]) << 24) |
                                   (static_cast<uint32_t>(p[1]) << 16) |
                                   (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
                        }
                    }
                    ok = false;
                    return 0;
                };
                auto readByte = [&regions](uint32_t addr, bool& ok) -> uint8_t {
                    for (const auto& r : regions) {
                        if (r.hostPtr && addr >= r.amigaAddr && addr < r.amigaAddr + r.size) {
                            return r.hostPtr[addr - r.amigaAddr];
                        }
                    }
                    ok = false;
                    return 0;
                };
                core::SourceDebugContext ctx;
                ctx.symbols = functionSymbols;
                ctx.lines = debugInfo.lines;
                ctx.inlines = debugInfo.inlines;
                ctx.frames = debugInfo.frames;
                // Only offer the hunk map once it has been relocated; before that its
                // link->runtime answers would be the ELF's own addresses, not this run's.
                ctx.hunkMap = m_compiledProgram.HunkMap().IsRelocated()
                                  ? &m_compiledProgram.HunkMap()
                                  : nullptr;
                ctx.segments = core::SegListResolver::ResolveActiveProcess(readLong, readByte);
                m_debuggerView->SetSourceDebugContext(std::move(ctx));
            } else {
                m_debuggerView->SetSourceDebugContext({});
            }

            // Forward DMA record data to the frame scrubber. At a mid-frame breakpoint stop, use
            // the in-progress (live) buffer so the DMA heatmap matches the partial frame the rest
            // of the timeline shows; otherwise the completed previous frame.
            if (emu->HasDmaDebug()) {
                AmigaEmulator::DmaSlotRecord* dmaPtr = nullptr;
                int hposCount = 0, vposCount = 0;
                const bool ok = emu->IsPartialFrame()
                                    ? emu->GetDmaRecordLive(&dmaPtr, hposCount, vposCount)
                                    : emu->GetDmaRecord(&dmaPtr, hposCount, vposCount);
                if (ok && dmaPtr) {
                    m_debuggerView->SetScrubberDmaData(dmaPtr, hposCount, vposCount);
                    m_copperRasterCtl.Update(emu, dmaPtr, hposCount, vposCount, copperVec);
                } else {
                    m_copperRasterCtl.Update(emu, nullptr, 0, 0, copperVec);
                }
            } else {
                // No DMA debug in this core: the copper trace alone still makes a useful raster.
                m_copperRasterCtl.Update(emu, nullptr, 0, 0, copperVec);
            }
        }

        // Keep this live while paused: debugger register pokes and display-mask changes can alter
        // the decoded geometry without running the core.
        m_debuggerView->SetScrubberOverlayContext(
            m_debuggerView->GetBitplaneLayout(),
            core::AmigaScreenMap::Build(emu->GetDisplayGeometry(),
                                        static_cast<int>(emu->GetWidth()),
                                        static_cast<int>(emu->GetHeight())));
    } else {
        if (m_lastDebuggerRun != UINT64_MAX) {
            m_lastDebuggerRun = UINT64_MAX;
            m_blitProfiler->Clear();
            m_debuggerView->Clear();
        }
    }
}

void Application::RunFrameFinishFrame()
{
    // Seek bar + transport: fed every draw, and whether or not the emulator is running. It is
    // deliberately outside both gates above - playback pauses the frames, and a machine that
    // has not started yet still has to show the recorder it is going to fill.
    {
        uint64_t oldest = 0, newest = 0, first = 0;
        const bool have = m_timeline.History().Range(oldest, newest);
        // The first frame the core will actually accept as a seek target, not oldest + 1:
        // a frame the machine never completed leaves a hole, and offering a number the
        // seek then refuses is how the bar came to promise frames that did not exist.
        const bool haveFirst = m_timeline.History().FirstSeekable(first);
        m_debuggerView->SetScrubberFrameTimeline(
            m_userPrefs.Get().frameHistoryEnabled, m_timeline.History().IsEnabled(),
            haveFirst ? first : 0, have ? newest : 0, m_timeline.Position());
        // Carries a loop armed elsewhere (the console's LOOP command) back to the bar.
        const TimelinePlayback tp = GetTimelinePlayback();
        m_debuggerView->SetScrubberFrameLoop(tp.looping, tp.loopStart, tp.loopEnd, tp.playing,
                                             static_cast<int>(tp.mode), tp.speed,
                                             m_timeline.StopReason());
    }

    // Resolve the scrubber's active blit (hovered/pinned) to this frame's record once, and feed
    // it to the views that highlight it (Bitplanes tiles + Memory Map).
    const core::BlitRecord* activeBlit = m_blitOverlay.ResolveActiveBlit();
    m_debuggerView->SetActiveBlit(activeBlit);
    m_memoryMapView->SetActiveBlit(activeBlit);

    m_debuggerView->Draw();
    m_memoryMapView->Draw();
    m_blitterView->Draw();
    if (auto* emu = m_emulatorView->GetEmulator())
        m_registerOpsView->SetCoreSupported(emu->HasRegWriteLog());
    m_registerOpsView->Draw();
    m_diskIoView->Draw();
    if (core::PrivateFeatures()) {
        UpdateExocet();
        m_exocetControlView->Draw();
        m_exocetView->Draw();
        m_exocetMemoryView->Draw();
        m_exocetDiskMapView->Draw();
        m_exocetMusicView->Draw();
        if (m_exocetListen) {
            if (m_exocetListen->IsOpen())
                m_exocetListen->Draw();
            else
                m_exocetListen.reset(); // closed: its playback stops with it
        }
    }
    m_blitterMintermView->Draw();
    m_dmaStatsView->Draw();
    m_logView->Draw();
    if (m_mcpActivityView)
        m_mcpActivityView->Draw();
    m_advancedFindView->Draw(&m_showAdvancedFind);
    m_tosecBrowserView->Draw();

    // Finish deferred document opens queued by the "Run" callback (insert, activate, boot),
    // now that no loop over m_documents is live.
    if (!m_deferredRunDocuments.empty()) {
        auto pending = std::move(m_deferredRunDocuments);
        m_deferredRunDocuments.clear();
        for (auto& p : pending) {
            DiskDocument* docPtr = p.doc.get();
            FinalizeMountedDocument(docPtr);
            m_documents.push_back(std::move(p.doc));
            SwitchActiveDocument(docPtr);
            UpdateDocumentViews(docPtr);

            if (m_emulatorView->LoadADF(p.adfBytes, docPtr->id)) {
                m_emulatorView->Open();
                Logger::Info("Running executable in new document: {}", p.filename);
            } else {
                Logger::Error("Failed to run executable: {}", p.filename);
                m_errorMessage = "Failed to run executable.\nThe emulator core might not be "
                                 "loaded or the file might be invalid.";
                m_showErrorModal = true;
            }
        }
    }

    // Process deferred document closures
    if (!m_deferredCloseDocuments.empty()) {
        for (auto* doc : m_deferredCloseDocuments) {
            CloseDocument(doc);
        }
        m_deferredCloseDocuments.clear();
    }

    // Clean up orphaned identifiers that have finished running
    if (!m_orphanedIdentifiers.empty()) {
        m_orphanedIdentifiers.erase(
            std::remove_if(m_orphanedIdentifiers.begin(), m_orphanedIdentifiers.end(),
                           [](const std::unique_ptr<core::DiskIdentifier>& id) {
                               auto s = id->GetState();
                               return s == core::IdentifyState::Done ||
                                      s == core::IdentifyState::Error ||
                                      s == core::IdentifyState::Idle;
                           }),
            m_orphanedIdentifiers.end());
    }

    UpdateFocusedWindowTracking();
}

// One phase method per stage of the frame; the phases run in the original order and the
// fullscreen early-out keeps its place between integration pumping and workspace drawing.
void Application::RunFrame()
{
    RunFramePumpIntegrations();

    const bool fullscreen = m_emulatorView->IsFullscreen();
    // Leaving fullscreen: every workspace window was unsubmitted, so this frame they all appear at
    // once and each one's focus-on-appear selects its tab - the last drawn wins (MCP Activity over
    // the Exocet panel). The dock nodes still hold the pre-fullscreen selection; keep it.
    if (m_wasFullscreen && !fullscreen)
        m_workspaceLayout.HoldTabSelection();
    m_wasFullscreen = fullscreen;

    if (fullscreen) {
        m_emulatorView->Draw();
        return;
    }

    RunFrameDrawWorkspace();
    RunFrameDrawDocuments();
    RunFrameDrawSharedViews();
    RunFrameHarvestTelemetry();
    RunFrameFinishFrame();
}

void Application::UpdateFocusedWindowTracking()
{
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx)
        return;

    // Apply the focus restore saved from last session, once the layout has settled.
    if (!m_pendingFocusWindow.empty()) {
        if (m_focusRestoreDelay > 0) {
            m_focusRestoreDelay--;
        } else {
            if (ImGuiWindow* w = ImGui::FindWindowByName(m_pendingFocusWindow.c_str()))
                ImGui::FocusWindow(w);
            m_pendingFocusWindow.clear();
        }
    }

    // Track the currently focused top-level window so it can be persisted at exit. Walk past
    // child windows to the owning window (a docked window's own name reselects its tab via
    // FocusWindow). Skip transient/uninteresting focus targets — quitting via a menu moves
    // focus onto the menu and then the empty viewport host, which would otherwise clobber the
    // last meaningful focus we want to restore next launch.
    ImGuiWindow* nav = ctx->NavWindow;
    while (nav && (nav->Flags & ImGuiWindowFlags_ChildWindow))
        nav = nav->ParentWindow;
    if (nav) {
        const char* name = nav->Name;
        const bool isViewportHost = strncmp(name, "WindowOverViewport", 18) == 0;
        const bool isTransient =
            (nav->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) != 0 ||
            strcmp(name, "##MainMenuBar") == 0;
        if (!isViewportHost && !isTransient)
            m_lastFocusedWindow = name;
    }
}

const std::vector<std::string>& Application::GetAvailableFonts() const { return m_availableFonts; }

std::string Application::GetCurrentFontName() const { return m_userPrefs.Get().fontName; }

void Application::SetCurrentFont(const std::string& fontName)
{
    // Update all document file viewers
    for (auto& doc : m_documents) {
        for (auto& viewer : doc->fileViewers) {
            viewer->SetCurrentFont(fontName);
        }
    }
    m_userPrefs.SetFontName(fontName);
}

void Application::ToggleFullscreen(bool fullscreen)
{
    if (!m_window)
        return;

    bool isCurrentlyFullscreen = glfwGetWindowMonitor(m_window) != nullptr;
    if (fullscreen == isCurrentlyFullscreen) {
        return;
    }

    if (fullscreen) {
        // Un-maximize before going fullscreen so that m_windowWidth/Height (kept current by the
        // GLFW size callback) reflects the pre-maximize size, not the maximized size.
        if (glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED))
            glfwRestoreWindow(m_window);

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(m_window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(m_window, nullptr, m_windowX, m_windowY, m_windowWidth, m_windowHeight,
                             0);
        // glfwSetWindowMonitor does not reliably apply the position on macOS — re-apply it.
        glfwSetWindowPos(m_window, m_windowX, m_windowY);
    }
}

const std::vector<std::string>& Application::GetRecentFiles() const
{
    return m_userPrefs.GetRecentFiles();
}

const std::vector<std::string>& Application::GetRecentDirectories() const
{
    return m_userPrefs.Get().recentDirectories;
}

void Application::SetOnOpenFileRequested(std::function<void(const std::string&)> callback)
{
    m_onOpenFileRequested = std::move(callback);
}

void Application::WireFrameTimeline()
{
    // The frame timeline drives the machine through hooks rather than holding the view: it
    // lives in core, and EmulatorView does not.
    core::FrameTimelineOps timelineOps;
    timelineOps.emulator = [this]() -> AmigaEmulator* {
        return m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    };
    timelineOps.isRunning = [this]() { return m_emulatorView && m_emulatorView->IsRunning(); };
    timelineOps.isPaused = [this]() { return m_emulatorView && m_emulatorView->IsPaused(); };
    timelineOps.setPaused = [this](bool paused) {
        if (m_emulatorView)
            m_emulatorView->SetPausedState(paused);
    };
    timelineOps.stepFrame = [this]() {
        if (m_emulatorView)
            m_emulatorView->StepFrame();
    };
    timelineOps.debuggerOpen = [this]() {
        // The Exocet window counts: its flow is watch, rewind through the history, edit, push.
        return (m_debuggerView && m_debuggerView->IsAnyWindowOpen()) ||
               (m_exocetView && m_exocetView->IsOpen());
    };
    timelineOps.mcpRunning = [this]() { return m_mcpServer.IsRunning(); };
    timelineOps.anyDriveBridged = [this]() {
        return m_emulatorView && m_emulatorView->AnyDriveBridged();
    };
    m_timeline.Init(std::move(timelineOps), &m_userPrefs);

    // These callbacks reference m_activeDocument at call-time (not capture-time)
}

void Application::WireHexEditorCallbacks()
{
    m_hexEditorView->SetOnReadBlock([this](uint32_t block) -> std::vector<uint8_t> {
        if (!m_activeDocument)
            return {};
        return m_activeDocument->volume.ReadBlock(block);
    });

    m_hexEditorView->SetOnAnalyzeBlock(
        [this](uint32_t block, std::span<const uint8_t> data) -> BlockDetails {
            if (!m_activeDocument)
                return {};
            return m_activeDocument->volume.AnalyzeBlock(block, data);
        });

    m_hexEditorView->SetOnRestoreFile([this](uint32_t block) {
        if (!m_activeDocument)
            return false;
        if (m_activeDocument->volume.RestoreDeletedFile(block)) {
            UpdateDocumentViews(m_activeDocument);
            return true;
        }
        return false;
    });

    m_hexEditorView->SetIsBlockModified([this](uint32_t block) {
        if (!m_activeDocument)
            return false;
        return m_activeDocument->volume.IsBlockModified(block);
    });

    m_hexEditorView->SetIsBlockCached([this](uint32_t block) {
        if (!m_activeDocument)
            return false;
        return m_activeDocument->volume.IsBlockCached(block);
    });

    m_hexEditorView->SetOnReadOriginalBlock([this](uint32_t block) -> std::vector<uint8_t> {
        if (!m_activeDocument)
            return {};
        return m_activeDocument->volume.ReadBlockFromDisk(block);
    });

    m_hexEditorView->SetOnWriteBlock([this](uint32_t block, uint32_t offset,
                                            const std::vector<uint8_t>& data) {
        if (!m_activeDocument)
            return false;
        if (m_activeDocument->volume.WriteBlock(block, data)) {
            auto info = m_activeDocument->volume.GetDiskInfo();
            m_changeMapView->SetData(info, m_activeDocument->volume.GetBlockModificationStatus());
            return true;
        }
        return false;
    });

    m_hexEditorView->SetOnRevertBlock([this](uint32_t block) {
        if (!m_activeDocument)
            return false;
        if (m_activeDocument->volume.RevertBlock(block)) {
            auto info = m_activeDocument->volume.GetDiskInfo();
            m_changeMapView->SetData(info, m_activeDocument->volume.GetBlockModificationStatus());
            m_bitmapView->SetData(info, m_activeDocument->volume.GetBlockUsage());
            m_blockMapView->SetData(info, m_activeDocument->volume.GetAllBlockTypes());
            m_ndosView->SetData(info, m_activeDocument->volume.GetZeroFilledBlocks());
            return true;
        }
        return false;
    });

    m_hexEditorView->SetOnBlockChanged([this](uint32_t block) {
        if (!m_activeDocument)
            return;

        m_bitmapView->SetCurrentBlock(block);
        m_blockMapView->SetCurrentBlock(block);
        m_ndosView->SetCurrentBlock(block);
        m_changeMapView->SetCurrentBlock(block);

        auto data = m_activeDocument->volume.ReadBlock(block);
        auto details = m_activeDocument->volume.AnalyzeBlock(block, data);

        if (details.fileHeader != 0) {
            m_activeDocument->fileListView->SetHighlightedFileHeader(details.fileHeader);
            auto blocks = m_activeDocument->volume.GetFileBlocks(details.fileHeader);
            m_bitmapView->SetHighlightedBlocks(blocks);
            m_blockMapView->SetHighlightedBlocks(blocks);
            m_ndosView->SetHighlightedBlocks(blocks);
            m_changeMapView->SetHighlightedBlocks(blocks);
        } else {
            m_activeDocument->fileListView->SetHighlightedFileHeader(0);
            m_bitmapView->SetHighlightedBlocks({});
            m_blockMapView->SetHighlightedBlocks({});
            m_ndosView->SetHighlightedBlocks({});
            m_changeMapView->SetHighlightedBlocks({});
        }
    });
}

void Application::WireFindNavigationCallbacks()
{
    m_advancedFindView->SetOnNavigate(
        [this](const core::SearchResult& result, const std::vector<uint8_t>& pattern,
               bool caseSensitive, core::DiskSearcher::SearchType type) {
            if (m_hexEditorView) {
                ui::BlockEditorView::SearchType hexType =
                    (type == core::DiskSearcher::SearchType::ASCII)
                        ? ui::BlockEditorView::SearchType::ASCII
                        : ui::BlockEditorView::SearchType::Hex;

                m_hexEditorView->SetSearchContext(pattern, caseSensitive, hexType);
                m_hexEditorView->HighlightSearchResult(result.block, result.offset, result.length);
            }
        });

    // Memory search wiring
    m_advancedFindView->SetMemorySearcher(m_memorySearcher.get());

    m_advancedFindView->SetOnNavigateMemory([this](const core::MemorySearchResult& result) {
        m_debuggerView->HighlightMemoryResult(result.address, result.length);
    });

    // Set up search routing based on focused view
    m_debuggerView->SetOnMemoryTabActive([this]() {
        // Logger::Info("Search Context -> Memory");
        m_searchContext = SearchContext::Memory;
    });

    m_hexEditorView->SetOnFocused([this]() {
        // Logger::Info("Search Context -> Disk");
        m_searchContext = SearchContext::Disk;
    });

    m_debuggerView->SetMemorySearcher(m_memorySearcher.get());
}

void Application::WireDebuggerCallbacks()
{
    m_debuggerView->SetOnLoadDocument(
        [this](const std::string& path, bool force) { return LoadDocumentExternal(path, force); });
    m_debuggerView->SetOnGetRecentFiles([this]() { return GetRecentFiles(); });

    // STATE console command. Deliberately the same store and the same EmulatorStateOps
    // helpers the save_state/load_state MCP tools use, so a snapshot taken from one front
    // end is visible to the other and both apply identical compatibility checks.
    {
        auto emulator = [this]() -> adm::AmigaEmulator* {
            return m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
        };
        auto documentPath = [this]() -> std::string {
            auto* doc = GetActiveDocument();
            return doc ? doc->currentFilePath : std::string{};
        };
        m_debuggerView->SetStateSnapshotHooks(
            [this, emulator, documentPath](const std::string& name, bool persist,
                                           std::string& outWarning) -> std::string {
                auto* emu = emulator();
                if (!emu)
                    return "emulator not available";
                const auto r =
                    core::SaveEmulatorState(*emu, m_stateStore, name, documentPath(), persist);
                // A save state captures the emulated machine, not the drive. Nothing in
                // the blob describes where a physical head is or what disk is in the
                // drive, so a later restore silently puts the two out of step. Saving is
                // still allowed - the state is fine for everything except the live slot -
                // but it must not look clean.
                if (m_emulatorView && m_emulatorView->AnyDriveBridged() && r.error.empty()) {
                    const std::string note =
                        "A drive is backed by real hardware; this state does not capture the "
                        "physical disk or head position and will not restore it.";
                    outWarning = outWarning.empty() ? note : outWarning + " " + note;
                    return r.error;
                }
                outWarning = r.warning;
                return r.error;
            },
            [this, emulator, documentPath](const std::string& name) -> std::string {
                auto* emu = emulator();
                if (!emu)
                    return "emulator not available";
                // Restoring a named state puts the machine somewhere the timeline knows
                // nothing about, so it cannot go on replaying onto it: playback walks
                // forward by re-running the next frame on top of the last, and that is only
                // sound while the machine is where the playhead says it is.
                ReleaseTimelineMachine();
                const auto r = core::LoadEmulatorState(*emu, m_stateStore, name, documentPath());
                if (!r.ok)
                    return r.error;
                // The restore may have replaced the core's breakpoint/watchpoint tables, so
                // make the Breakpoints window re-read them instead of trusting its cache.
                if (auto* bps = m_debuggerView->GetBreakpointsView())
                    bps->MarkNeedsRefresh();
                return {};
            },
            [this](const std::string& name) { return m_stateStore.Remove(name); },
            [this]() {
                std::vector<core::ArHost::StateEntry> out;
                for (const auto& s : m_stateStore.List())
                    out.push_back({s.name, s.bytes, s.frame, s.persisted, s.created});
                return out;
            });

        m_debuggerView->SetOnScrubberSeekFrame([this](uint64_t frame) {
            const std::string err = SeekToRecordedFrame(frame);
            if (!err.empty())
                Logger::Info("Frame seek: {}", err);
        });

        m_debuggerView->SetOnScrubberSetFrameLoop([this](uint64_t a, uint64_t b) {
            const std::string err = SetFrameLoop(a, b);
            if (!err.empty())
                Logger::Info("Frame loop: {}", err);
        });

        m_debuggerView->SetOnScrubberClearTimeline([this]() { ClearFrameTimeline(); });

        // The scrubber's record arm is the same switch as Emulator > Frame History > Record
        // frames, so it goes through the preference rather than poking the recorder: that is
        // what makes the choice stick across sessions and keeps the menu tick in step.
        // FrameTimeline::UpdateSettings() runs straight away so the first frame after the click is
        // recorded, instead of the arming waiting a frame for the next Update().
        m_debuggerView->SetOnScrubberSetRecording([this](bool recording) {
            const auto& p = m_userPrefs.Get();
            SetFrameHistorySettings(recording, p.frameHistoryBudgetMB,
                                    p.frameHistoryPersistInSession, p.frameHistoryThumbnails,
                                    p.frameHistoryThumbnailInterval);
            m_timeline.UpdateSettings();
        });

        m_debuggerView->SetOnScrubberTimelineTransport(
            [this](bool playing) { SetTimelinePlaying(playing); },
            [this](int delta) {
                const std::string err = StepTimeline(delta);
                if (!err.empty())
                    Logger::Info("Timeline step: {}", err);
            },
            [this](int mode) { SetTimelinePlayMode(static_cast<TimelinePlayMode>(mode)); },
            [this](double speed) { SetTimelineSpeed(speed); });

        m_debuggerView->SetOnScrubberFrameThumbnail(
            [this](uint64_t frame, std::vector<uint8_t>& rgb, int& w, int& h, uint64_t& gotFrame) {
                return GetFrameThumbnail(frame, rgb, w, h, gotFrame);
            });

        m_debuggerView->SetFrameHistoryHooks(
            [this]() {
                core::ArHost::FrameHistoryInfo info;
                info.available = true;
                info.stats = m_timeline.Stats();
                info.position = m_timeline.Position();
                GetFrameLoop(info.loopStart, info.loopEnd);
                const TimelinePlayback tp = GetTimelinePlayback();
                info.playing = tp.playing;
                info.playMode = static_cast<int>(tp.mode);
                info.playSpeed = tp.speed;
                return info;
            },
            [this](uint64_t frame) { return SeekToRecordedFrame(frame); },
            [this](uint64_t count) { return RewindFrames(count); },
            [this](uint64_t a, uint64_t b) { return SetFrameLoop(a, b); },
            [this](uint64_t a, uint64_t b, std::vector<core::FrameDiffRun>& out, size_t maxRuns,
                   bool& truncated) { return DiffRecordedFrames(a, b, out, maxRuns, truncated); });

        m_debuggerView->SetOnFrameHistoryClear([this]() { ClearFrameTimeline(); });

        m_debuggerView->SetFrameHistoryTransportHooks(
            [this](bool playing) { SetTimelinePlaying(playing); },
            [this](int delta) { return StepTimeline(delta); },
            [this](int mode) { SetTimelinePlayMode(static_cast<TimelinePlayMode>(mode)); },
            [this](double speed) { SetTimelineSpeed(speed); });
    }
    m_debuggerView->SetOnGetOpenDocuments([this]() {
        std::vector<std::string> paths;
        paths.reserve(m_documents.size());
        for (const auto& d : m_documents)
            paths.push_back(d->currentFilePath);
        return paths;
    });
    m_debuggerView->SetOnGetOpenDocumentInfo([this]() {
        std::vector<core::ArHost::DocumentInfo> infos;
        infos.reserve(m_documents.size());
        for (const auto& d : m_documents) {
            core::ArHost::DocumentInfo info;
            info.path = d->currentFilePath;
            info.kind = d->kind == core::DocumentKind::HostDirectory ? "HostDirectory"
                        : d->IsHardfile()                            ? "Hardfile"
                                                                     : "ADF";
            if (d->hostDirectory)
                info.assignedDevice = d->hostDirectory->GetAssignedDevice();
            if (d->IsHardfile()) {
                // The slot this image is configured on, if any.
                std::error_code ec;
                for (const auto& mount : m_emulatorView->GetConfiguredHostMounts()) {
                    if (mount.enabled && mount.IsHardfile() &&
                        std::filesystem::equivalent(mount.hardfilePath, d->currentFilePath, ec)) {
                        info.assignedDevice = mount.device;
                        break;
                    }
                }
            }
            infos.push_back(std::move(info));
        }
        return infos;
    });
    m_debuggerView->SetHostMountCallbacks(
        [this]() { return m_emulatorView->GetConfiguredHostMounts(); },
        [this]() {
            return m_emulatorView->GetEmulator()->GetHostMountService().GetEffectiveMounts();
        },
        [this](const core::HostDirectoryMount& mount, std::string& error) {
            auto mounts = m_emulatorView->GetConfiguredHostMounts();
            mounts[static_cast<size_t>(mount.device)] = mount;
            auto validated = mounts;
            if (!core::UaeConfigBuilder::ValidateMounts(validated, error, false))
                return false;
            m_emulatorView->SetConfiguredHostMounts(validated);
            return true;
        },
        [this](std::string& error) { return m_emulatorView->ApplyHostMountsAndRestart(error); },
        [this](core::HostMountDevice device) {
            const auto& mount =
                m_emulatorView->GetConfiguredHostMounts()[static_cast<size_t>(device)];
            if (!mount.enabled)
                return false;
            if (mount.IsHardfile())
                return LoadAdf(mount.hardfilePath.string());
            return OpenHostDirectoryDocument(mount.hostDirectory.string(), mount.readOnly, device,
                                             mount.volumeLabel);
        },
        [this](core::HostMountDevice device) {
            bool refreshed = false;
            const auto& mount =
                m_emulatorView->GetConfiguredHostMounts()[static_cast<size_t>(device)];
            if (mount.enabled && mount.IsHardfile()) {
                // Re-read the image from disk so the document shows what the guest wrote.
                std::error_code ec;
                for (auto& doc : m_documents) {
                    if (doc->IsHardfile() &&
                        std::filesystem::equivalent(doc->currentFilePath, mount.hardfilePath, ec))
                        return LoadAdfImpl(doc->currentFilePath, false, false, true);
                }
                return false;
            }
            for (auto& doc : m_documents) {
                if (!doc->hostDirectory || doc->hostDirectory->GetAssignedDevice() != device)
                    continue;
                std::string error;
                if (doc->hostDirectory->RefreshNow(error)) {
                    UpdateHostDirectoryDocument(doc.get());
                    refreshed = true;
                }
            }
            return refreshed;
        });
    m_emulatorView->SetOnOpenHardfileDocument(
        [this](const std::filesystem::path& path) { LoadAdf(path.string()); });
    m_debuggerView->SetHardDriveControllerRomsCallback(
        [this](const core::HardDriveControllerRoms& roms) {
            m_emulatorView->SetHardDriveControllerRoms(roms);
        });
    m_debuggerView->SetOnRunDocumentByIndex([this](int idx) {
        if (idx < 0 || idx >= static_cast<int>(m_documents.size()))
            return;
        SwitchActiveDocument(m_documents[static_cast<size_t>(idx)].get());
        RunAdf();
    });
    m_debuggerView->SetOnCloseDocumentByIndex([this](int idx) {
        if (idx < 0 || idx >= static_cast<int>(m_documents.size()))
            return;
        auto* doc = m_documents[static_cast<size_t>(idx)].get();
        if (doc->HasUnsavedChanges()) {
            std::string filename = std::filesystem::path(doc->currentFilePath).filename().string();
            m_unsavedChanges.Enqueue(
                filename,
                [this, doc]() {
                    auto* prev = m_activeDocument;
                    m_activeDocument = doc;
                    bool saved = SaveAdf();
                    m_activeDocument = prev;
                    return saved;
                },
                [this, doc]() { QueueCloseDocument(doc); });
        } else {
            QueueCloseDocument(doc);
        }
    });
    m_debuggerView->SetOnStartEmulator([this]() { RunAdf(); });
    m_debuggerView->SetOnStopEmulator([this]() { m_emulatorView->Stop(); });
    m_debuggerView->SetOnGetBlitLog(
        [this](int limit) { return m_logSnapshots.MakeBlitLogSnapshot(limit); });
    // EXO answers out of core/ExocetQuery.h, as the Exocet MCP tools do
    if (core::PrivateFeatures())
        m_debuggerView->SetOnExocetSession([this]() { return m_exocet.get(); });
    // COV reuses the MCP tool so the console and Claude cannot drift apart.
    m_debuggerView->SetOnLineCoverage(
        [this](const std::string& file, uint32_t line, uint64_t frame) -> nlohmann::json {
            if (!m_mcpHandler)
                return nlohmann::json{{"error", "line coverage unavailable"}};
            return m_mcpHandler->Handle(
                "line_coverage", nlohmann::json{{"file", file}, {"line", line}, {"frame", frame}});
        });
    m_debuggerView->SetOnTraceHistory([this]() { return m_cpuTraceHistory.IsEnabled(); },
                                      [this](bool on) { m_cpuTraceHistory.SetEnabled(on); });
    m_debuggerView->SetOnGetRegLog(
        [this](int limit) { return m_logSnapshots.MakeRegLogSnapshot(limit); });
    m_debuggerView->SetOnGetAudioStats([this] { return MakeAudioLogSnapshot(); });
    m_debuggerView->WirePaulaState();
    m_debuggerView->SetOnAudioRecord(
        [this](const core::AudioRecordRequest& r) { return ApplyAudioRecordRequest(r); });
    m_debuggerView->SetOnScenario(
        [this](const core::ScenarioRequest& r) { return ApplyScenarioRequest(r); });
    m_debuggerView->SetOnGetDiskLog(
        [this](const core::DiskLogQuery& q) { return m_logSnapshots.MakeDiskLogSnapshot(q); });

    // Wire frame scrubber → blitter view (and the Minterm window when synced)
    m_debuggerView->m_onScrubberChanged = [this](uint32_t lRef) {
        m_blitterView->SetHighlightLRef(lRef);
        m_registerOpsView->SetHighlightLRef(lRef);
        SyncMintermToBlit(lRef);
    };

    // Frame scrubber reports which blit to overlay on the emulator (hovered/pinned). The same
    // active blit highlights its row in the Blitter Operations window.
    m_debuggerView->m_onActiveBlitChanged = [this](uint32_t lRef) {
        m_blitOverlay.SetActiveBlitLRef(lRef);
        m_blitterView->SetHoverHighlightLRef(lRef);
        SyncMintermToBlit(lRef);
    };

    // Wire blitter row click → sync scrubber, copper, and CPU trace (and the Minterm window)
    m_blitterView->m_onJumpToLRef = [this](uint32_t lRef) {
        m_debuggerView->JumpToLRef(lRef);
        SyncMintermToBlit(lRef);
    };

    // Wire blitter row hover → highlight corresponding blit bar in frame scrubber
    m_blitterView->m_onHoverLRef = [this](uint32_t lRef) {
        m_debuggerView->SetHighlightedBlit(lRef);
    };

    // Wire blitter channel-address hyperlink → open the Memory window at that address
    m_blitterView->m_onJumpToMemory = [this](uint32_t addr, uint32_t len) {
        if (auto* mem = m_debuggerView->GetMemoryView())
            mem->HighlightMemoryResult(addr, len);
    };

    // Wire Register Operations row click → sync every time-aware window to this write's L-Ref,
    // then make the jump source-aware: a copper write selects the MOVE that made it in the
    // Copper trace, a CPU write points the disassembly at the instruction that made it.
    m_registerOpsView->m_onJumpToLRef = [this](uint32_t lRef, bool fromCopper, uint32_t pc) {
        m_debuggerView->JumpToLRef(lRef);
        if (fromCopper)
            m_debuggerView->JumpToCopperTrace(lRef);
        else if (pc)
            m_debuggerView->NavigateToDisassembly(pc);
    };

    // Wire Register Operations row hover → highlight the matching copper tick in the scrubber
    m_registerOpsView->m_onHoverLRef = [this](uint32_t lRef) {
        m_debuggerView->SetHighlightedCopper(lRef);
    };

    // Wire Register Operations register-name hyperlink → Custom Registers, with the decoder
    // seeded from the value this row wrote rather than the register's current contents.
    m_registerOpsView->m_onJumpToCustomReg = [this](uint32_t offset, uint16_t value) {
        m_debuggerView->NavigateToCustomRegister(offset, value);
    };

    // Wire Disk I/O row hyperlinks: DSKPT → Memory, arming PC → Disassembly, row → scrubber.
    m_diskIoView->m_onJumpToMemory = [this](uint32_t addr, uint32_t len) {
        if (auto* mem = m_debuggerView->GetMemoryView())
            mem->HighlightMemoryResult(addr, len);
    };
    m_diskIoView->m_onJumpToDisasm = [this](uint32_t addr) {
        if (auto* dis = m_debuggerView->GetDisassemblyView())
            dis->GoToAddress(addr);
    };
    m_diskIoView->m_onJumpToLRef = [this](uint32_t lRef) { m_debuggerView->JumpToLRef(lRef); };

    SetupExocet();

    // Wire blitter minterm click → open the Minterm window populated from the selected blit
    m_blitterView->m_onOpenMinterm = [this](uint16_t bltcon0, uint16_t bltcon1) {
        m_blitterMintermView->LoadFromBlit(bltcon0, bltcon1);
    };

    // Wire frame-scrubber blit-detail minterm click → same Minterm window
    m_debuggerView->m_onOpenMinterm = [this](uint16_t bltcon0, uint16_t bltcon1) {
        m_blitterMintermView->LoadFromBlit(bltcon0, bltcon1);
    };

    // Wire the blit overlay settings through to the core-side map and the overlay itself.
    m_debuggerView->m_onBlitOverlayChanged = [this](const ui::BlitOverlaySettings& s) {
        m_blitOverlay.SetSettings(s);
        m_userPrefs.SetBlitOverlayPrefs(
            {s.enabled, s.useProvenance, s.outlineAll, s.opacity, s.decayFrames});
    };

    // Wire the same panel's "Capture destination data" toggle through to the core's D-write ring.
    m_debuggerView->m_onBlitCaptureDestDataChanged = [this](bool on) {
        m_blitOverlay.SetDestCaptureEnabled(on);
        m_userPrefs.SetCaptureBlitDestData(on);
    };

    // Wire overlays onto the emulator image (sprites bbox + geometry DIW/DDF + blit dest)
    m_debuggerView->m_onShowCopperRasterChanged = [this](bool show) {
        m_copperRasterCtl.SetEnabled(show);
    };

    // The raster follows every other window that resolves to a colour clock, and highlights
    // whatever the Copper trace is hovering.
    m_debuggerView->m_onRasterSelect = [this](uint32_t lRef) {
        m_copperRasterCtl.SetSelectedLRef(lRef);
    };
    m_debuggerView->m_onRasterHighlight = [this](uint32_t lRef) {
        m_copperRasterCtl.SetHighlightedLRef(lRef);
    };
}

bool Application::IsRtgDisplayActive() const
{
    if (!m_emulatorView)
        return false;
    const adm::AmigaEmulator* emu = m_emulatorView->GetEmulator();
    return emu && emu->IsRtgDisplayActive();
}

void Application::WireEmulatorOverlayCallbacks()
{
    // Every one of these overlays maps a beam position, a bitplane pointer or a blitter
    // destination onto the picture on screen. A Picasso96 screen is not drawn by the beam at
    // all - no bitplanes, no copper list building it, a frame buffer the CPU writes directly -
    // so their geometry means nothing there and they would paint boxes over an unrelated
    // image. They are suppressed for as long as the RTG screen is the one being shown, and
    // come back by themselves when the guest switches to the chipset display.
    m_emulatorView->SetOnDrawRasterOverlay(
        [this](ImDrawList* dl, ImVec2 rMin, ImVec2 rMax, ImVec2 iMin, ImVec2 iMax) {
            if (IsRtgDisplayActive())
                return;
            m_copperRasterCtl.DrawOverlay(dl, rMin, rMax, iMin, iMax);
        });

    m_emulatorView->SetOnDrawEmulatorOverlay([this](ImDrawList* dl, ImVec2 mn, ImVec2 mx) {
        if (IsRtgDisplayActive())
            return;
        m_debuggerView->DrawSpritesOverlay(dl, mn, mx);
        m_debuggerView->DrawOverlays(dl, mn, mx);
        m_blitOverlay.Draw(dl, mn, mx);
    });

    // Block mouse capture in the emulator window when the geometry overlay or the pixel
    // magnifier is active (both want hover/click interaction with the display surface).
    // The blit overlay only claims the click while Alt is held, so a plain click still hands
    // the mouse to the Amiga while the overlay is up.
    m_emulatorView->SetMouseCaptureBlocker([this]() {
        return m_debuggerView->IsGeometryOverlayVisible() || m_emulatorView->IsMagnifierEnabled() ||
               m_debuggerView->IsRasterOverlayVisible() ||
               (m_debuggerView->IsBlitOverlayVisible() && ImGui::GetIO().KeyAlt);
    });

    // Wire the shared address menu's "Memory Map" item to open the (Application-owned) Memory Map,
    // and give the Memory Map's own Address column the same shared hyperlink + menu.
    m_debuggerView->m_onShowInMemoryMap = [this](uint32_t addr) {
        m_memoryMapView->ShowAddress(addr);
    };
}

void Application::WireAddressContexts()
{
    m_memoryMapView->SetAddressContext(m_debuggerView->GetAddressContext());
    m_registerOpsView->SetAddressContext(m_debuggerView->GetAddressContext());

    // Wire "jump to memory" from SpritesView through to the Memory tab
    m_debuggerView->SetSpritesJumpMemory(
        [this](uint32_t addr) { m_debuggerView->HighlightMemoryResult(addr, 4); });

    // Wire "jump to memory" from BitplanesView (trace mode M button) through to the Memory tab
    m_debuggerView->SetBitplanesJumpMemory(
        [this](uint32_t addr) { m_debuggerView->HighlightMemoryResult(addr, 4); });

    // Wire the Bartman Bitmaps window: "View" buttons jump to the Memory tab, and persist the
    // opt-in support toggle to user preferences.
    m_debuggerView->SetBartmanJumpMemory(
        [this](uint32_t addr) { m_debuggerView->HighlightMemoryResult(addr, 4); });
    m_debuggerView->SetBartmanSupportChanged(
        [this](bool enabled) { m_userPrefs.SetBartmanDebugEnabled(enabled); });
}

void Application::WireFindDataCallbacks()
{
    m_advancedFindView->SetOnSaveData([this](const std::string& filename,
                                             const core::SearchResult& result) {
        if (!m_activeDocument)
            return;
        nfdchar_t* outPath = nullptr;
        nfdresult_t nfdResult = NFD_SaveDialog(&outPath, nullptr, 0, nullptr, filename.c_str());

        if (nfdResult == NFD_OKAY) {
            std::vector<uint8_t> buffer;

            if (result.fromFilesystem) {
                // File is on a filesystem disk: read via adflib to correctly handle
                // OFS/FFS block structure (non-contiguous blocks, block headers, etc.)
                std::string content =
                    m_activeDocument->volume.ReadFileContent(result.filesystemHeaderBlock);
                buffer.assign(content.begin(), content.end());
            } else {
                // NDOS disk: read consecutive 512-byte raw blocks
                buffer.reserve(result.length);
                uint32_t currentBlock = result.block;
                size_t currentOffset = result.offset;
                while (buffer.size() < result.length) {
                    std::vector<uint8_t> blockData =
                        m_activeDocument->volume.ReadBlock(currentBlock);
                    if (blockData.empty())
                        break;
                    size_t toCopy =
                        std::min(result.length - buffer.size(), blockData.size() - currentOffset);
                    buffer.insert(buffer.end(), blockData.begin() + currentOffset,
                                  blockData.begin() + currentOffset + toCopy);
                    currentBlock++;
                    currentOffset = 0;
                }
                if (buffer.size() != result.length) {
                    Logger::Error("Failed to read full data from disk.");
                    m_errorMessage = "Failed to read full data from disk.";
                    m_showErrorModal = true;
                    NFD_FreePath(outPath);
                    return;
                }
            }

            std::ofstream outfile(outPath, std::ios::binary);
            if (outfile) {
                outfile.write((char*)buffer.data(), buffer.size());
                outfile.close();
                // A full disk fails the write, not the open — report it instead of "Saved".
                if (outfile.good()) {
                    Logger::Info("Saved {} bytes to {}", buffer.size(), outPath);
                } else {
                    Logger::Error("Write failed (disk full?): {}", outPath);
                    m_errorMessage = "Failed to write file — the write did not complete.";
                    m_showErrorModal = true;
                }
            } else {
                Logger::Error("Failed to open file for writing: {}", outPath);
                m_errorMessage = "Failed to open file for writing.";
                m_showErrorModal = true;
            }

            NFD_FreePath(outPath);
        }
    });

    m_advancedFindView->SetOnListen([this](const std::string& filename,
                                           const core::SearchResult& result) {
        if (!m_activeDocument)
            return;
        std::vector<uint8_t> buffer;

        if (result.fromFilesystem) {
            // File is on a filesystem disk: read via adflib to correctly handle
            // OFS/FFS block structure (non-contiguous blocks, block headers, etc.)
            std::string content =
                m_activeDocument->volume.ReadFileContent(result.filesystemHeaderBlock);
            buffer.assign(content.begin(), content.end());
        } else {
            // NDOS disk: read consecutive 512-byte raw blocks
            buffer.reserve(result.length);
            uint32_t currentBlock = result.block;
            size_t currentOffset = result.offset;
            while (buffer.size() < result.length) {
                std::vector<uint8_t> blockData = m_activeDocument->volume.ReadBlock(currentBlock);
                if (blockData.empty())
                    break;
                size_t toCopy =
                    std::min(result.length - buffer.size(), blockData.size() - currentOffset);
                buffer.insert(buffer.end(), blockData.begin() + currentOffset,
                              blockData.begin() + currentOffset + toCopy);
                currentBlock++;
                currentOffset = 0;
            }
            if (buffer.size() != result.length) {
                Logger::Error("Failed to read full module data from disk.");
                m_errorMessage = "Failed to read full module data from disk.";
                m_showErrorModal = true;
                return;
            }
        }

        auto module = std::make_unique<core::AudioModule>();
        if (module->Load(buffer, filename)) {
            auto player = std::make_unique<ui::AudioPlayer>(&m_audioEngine);
            player->SetTitle("Audio Player - " + filename);
            player->SetModule(std::move(module));
            player->Open();
            if (m_activeDocument) {
                m_activeDocument->audioPlayers.push_back(std::move(player));
            }
        } else {
            Logger::Error("Failed to load audio module: {}", filename);
            m_errorMessage = "Failed to load audio module (invalid format?).";
            m_showErrorModal = true;
        }
    });

    m_advancedFindView->SetOnSaveMemoryData([this](const std::string& filename,
                                                   const core::MemorySearchResult& result) {
        std::vector<uint8_t> buffer = m_memorySearcher->ReadBytes(result.address, result.length);
        if (buffer.empty()) {
            Logger::Error("Failed to read memory at ${:06X}", result.address);
            return;
        }
        nfdchar_t* outPath = nullptr;
        if (NFD_SaveDialog(&outPath, nullptr, 0, nullptr, filename.c_str()) == NFD_OKAY) {
            std::ofstream outfile(outPath, std::ios::binary);
            if (outfile) {
                outfile.write((char*)buffer.data(), buffer.size());
                outfile.close();
                if (!outfile.good())
                    Logger::Error("Write failed (disk full?): {}", outPath);
            } else {
                Logger::Error("Failed to open file for writing: {}", outPath);
            }
            NFD_FreePath(outPath);
        }
    });

    m_advancedFindView->SetOnListenMemory([this](const std::string& filename,
                                                 const core::MemorySearchResult& result) {
        std::vector<uint8_t> buffer = m_memorySearcher->ReadBytes(result.address, result.length);
        if (buffer.empty()) {
            Logger::Error("Failed to read memory at ${:06X}", result.address);
            return;
        }
        auto module = std::make_unique<core::AudioModule>();
        if (module->Load(buffer, filename)) {
            auto player = std::make_unique<ui::AudioPlayer>(&m_audioEngine);
            player->SetTitle("Audio Player - " + filename);
            player->SetModule(std::move(module));
            player->Open();
            // Attach to active document if available, else keep globally
            if (m_activeDocument) {
                m_activeDocument->audioPlayers.push_back(std::move(player));
            }
        } else {
            Logger::Error("Failed to load audio module from memory: {}", filename);
            m_errorMessage = "Failed to load audio module (invalid format?).";
            m_showErrorModal = true;
        }
    });

    // Block map view callbacks
}

void Application::WireBlockGridCallbacks()
{
    m_bitmapView->SetOnBlockSelected([this](uint32_t block) { m_hexEditorView->GoToBlock(block); });
    m_bitmapView->SetOnClearBlock([this](uint32_t block) { OnClearBlock(block); });
    m_bitmapView->SetOnBlocksSelected([this](const std::set<uint32_t>& blocks) {
        m_blockMapView->SetSelectedBlocks(blocks);
        m_ndosView->SetSelectedBlocks(blocks);
        m_changeMapView->SetSelectedBlocks(blocks);
    });
    m_bitmapView->SetOnSaveBlocks(
        [this](const std::set<uint32_t>& blocks) { OnSaveBlocks(blocks); });
    m_bitmapView->SetOnInsertBinary([this](uint32_t block) { OnInsertBinary(block); });

    m_blockMapView->SetOnBlockSelected(
        [this](uint32_t block) { m_hexEditorView->GoToBlock(block); });
    m_blockMapView->SetOnClearBlock([this](uint32_t block) { OnClearBlock(block); });
    m_blockMapView->SetOnBlocksSelected([this](const std::set<uint32_t>& blocks) {
        m_bitmapView->SetSelectedBlocks(blocks);
        m_ndosView->SetSelectedBlocks(blocks);
        m_changeMapView->SetSelectedBlocks(blocks);
    });
    m_blockMapView->SetOnSaveBlocks(
        [this](const std::set<uint32_t>& blocks) { OnSaveBlocks(blocks); });
    m_blockMapView->SetOnInsertBinary([this](uint32_t block) { OnInsertBinary(block); });

    m_ndosView->SetOnBlockSelected([this](uint32_t block) { m_hexEditorView->GoToBlock(block); });
    m_ndosView->SetOnClearBlock([this](uint32_t block) { OnClearBlock(block); });
    m_ndosView->SetOnBlocksSelected([this](const std::set<uint32_t>& blocks) {
        m_bitmapView->SetSelectedBlocks(blocks);
        m_blockMapView->SetSelectedBlocks(blocks);
        m_changeMapView->SetSelectedBlocks(blocks);
    });
    m_ndosView->SetOnSaveBlocks([this](const std::set<uint32_t>& blocks) { OnSaveBlocks(blocks); });
    m_ndosView->SetOnInsertBinary([this](uint32_t block) { OnInsertBinary(block); });

    m_changeMapView->SetOnBlockSelected(
        [this](uint32_t block) { m_hexEditorView->GoToBlock(block); });
    m_changeMapView->SetOnClearBlock([this](uint32_t block) { OnClearBlock(block); });
    m_changeMapView->SetOnBlocksSelected([this](const std::set<uint32_t>& blocks) {
        m_bitmapView->SetSelectedBlocks(blocks);
        m_blockMapView->SetSelectedBlocks(blocks);
        m_ndosView->SetSelectedBlocks(blocks);
    });
    m_changeMapView->SetOnSaveBlocks(
        [this](const std::set<uint32_t>& blocks) { OnSaveBlocks(blocks); });
    m_changeMapView->SetOnInsertBinary([this](uint32_t block) { OnInsertBinary(block); });
}

void Application::WireMediaCallbacks()
{
    // FloppyDiskView callbacks
    m_floppyDiskView->SetOnReadComplete(
        [this](const std::vector<uint8_t>& buffer) { OnFloppyDiskRead(buffer); });
    // The Write tab's image selector: every open document, and a resolver that turns the one it
    // has selected into "what would be written". Document id 0 means "whichever is active",
    // which is all this window could ever write before the selector existed.
    m_floppyDiskView->SetOnGetDocuments([this]() { return GetDiskDocumentInfos(); });
    m_floppyDiskView->SetOnGetWriteSource([this](uint32_t docId) -> ui::WriteSourceInfo {
        const DiskDocument* doc = FindWriteDocument(docId);
        ui::WriteSourceInfo src;
        if (!doc)
            return src;
        src.id = doc->id;
        src.name = doc->currentFilePath.empty()
                       ? "Untitled"
                       : std::filesystem::path(doc->currentFilePath).filename().string();
        src.filePath = doc->currentFilePath;
        // currentFilePath is a display name as often as it is a path - "[Floppy Disk]" for a
        // disk pulled off a drive, a bare filename for a TOSEC download, empty for New Disk -
        // so gw is only ever pointed at it once it is known to name a real file.
        std::error_code ec;
        src.hasFile = !doc->currentFilePath.empty() &&
                      std::filesystem::is_regular_file(doc->currentFilePath, ec);
        // A container holds the image rather than being one, so its bytes win over its path;
        // and a document with no file behind it has nothing but its bytes.
        const bool isContainer = !doc->loadedZipPath.empty() || !doc->loadedDmsPath.empty();
        src.hasBytes = doc->volume.IsMounted() && (isContainer || !src.hasFile);
        return src;
    });
    m_floppyDiskView->SetOnGetZeroFilledBlocks([this](uint32_t docId) -> std::vector<bool> {
        const DiskDocument* doc = FindWriteDocument(docId);
        if (doc && doc->volume.IsMounted())
            return doc->volume.GetZeroFilledBlocks();
        return {};
    });
    m_floppyDiskView->SetOnGetAdfData([this](uint32_t docId) -> std::vector<uint8_t> {
        const DiskDocument* doc = FindWriteDocument(docId);
        if (!doc || !doc->volume.IsMounted())
            return {};
        return doc->volume.GetFileContent();
    });
    // Format tab: the same blank ADF templates the New Disk menu offers, and a builder that
    // turns one of them plus a volume label into the image written to the physical disk.
    m_floppyDiskView->SetOnGetDiskTemplates([this]() { return m_diskTemplates; });
    m_floppyDiskView->SetOnBuildFormatImage(
        [this](const std::string& templateName, const std::string& label) -> std::vector<uint8_t> {
            auto bytes = ReadDiskTemplate(templateName);
            if (!bytes)
                return {}; // ReadDiskTemplate already surfaced the error
            if (label.empty())
                return std::move(*bytes);

            // A scratch image this function labels and hands straight to the writer; it is
            // never a document, so nothing else ever sees it. AdfHandler defaults to
            // write-protected, which is right for images the user opens and wrong for this.
            AdfHandler handler;
            handler.SetWriteProtected(false);
            if (!handler.Mount(std::move(*bytes))) {
                m_errorMessage = fmt::format("Could not read the '{}' template.", templateName);
                m_showErrorModal = true;
                return {};
            }
            if (!handler.SetVolumeName(label))
                Logger::Warn("Could not label the format image '{}'; writing it unlabelled.",
                             label);
            return handler.GetFileContent();
        });
    m_floppyDiskView->SetOnSettingsChanged([this](int driveIndex, int formatIndex, bool verify,
                                                  bool preErase, bool eraseEmpty, bool readVerify) {
        m_userPrefs.SetGwSettings(driveIndex, formatIndex, verify, preErase, eraseEmpty,
                                  readVerify);
    });

    // Restore saved gw settings
    const auto& prefs = m_userPrefs.Get();
    m_floppyDiskView->SetDriveIndex(prefs.gwDriveIndex);
    m_floppyDiskView->SetFormatIndex(prefs.gwFormatIndex);
    m_floppyDiskView->SetVerify(prefs.gwVerify);
    m_floppyDiskView->SetPreErase(prefs.gwPreErase);
    m_floppyDiskView->SetEraseEmpty(prefs.gwEraseEmpty);
    m_floppyDiskView->SetReadVerify(prefs.gwReadVerify);

    // FloppyDrivesView callbacks - FloppyBridge physical drive support.
    // The view holds no hardware: everything routes to AmigaEmulator, which only writes
    // the core's changed_prefs. PuAE opens or closes the device on its next emulated
    // vertical blank, which is why Attach reports "queued", not "opened".
    m_floppyDrivesView->SetOnGetDriverList([this]() -> std::vector<std::string> {
        auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
        return emu ? emu->GetBridgeDriverList() : std::vector<std::string>{};
    });
    m_floppyDrivesView->SetOnEnumComPorts([this]() -> std::vector<std::string> {
        auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
        return emu ? emu->EnumBridgeComPorts() : std::vector<std::string>{};
    });
    m_floppyDrivesView->SetOnIsRunning(
        [this]() { return m_emulatorView && m_emulatorView->IsRunning(); });
    // The core is loaded lazily on the first emulator start. The window needs to tell
    // "the core says no bridge support" apart from "there is no core to ask yet", or it
    // reports a missing feature that is actually there.
    m_floppyDrivesView->SetOnIsCoreLoaded(
        [this]() { return m_emulatorView && m_emulatorView->IsCoreLoaded(); });
    m_floppyDrivesView->SetOnAttach(
        [this](const ui::FloppyDrivesView::Config& cfg) { return AttachLiveFloppy(cfg); });
    m_floppyDrivesView->SetOnDetach([this](int driveSlot) { DetachLiveFloppy(driveSlot); });
    m_floppyDrivesView->SetOnGetStatus(
        [this](int driveSlot) { return GetLiveFloppyStatus(driveSlot); });
    m_floppyDrivesView->SetOnSettingsChanged([this](const ui::FloppyDrivesView::Config& cfg) {
        core::UserPreferences::Preferences::FbSlotPrefs s;
        s.hardwareType = cfg.hardwareType;
        s.comPort = cfg.comPort;
        s.bridgeMode = cfg.bridgeMode;
        s.bridgeDensity = cfg.bridgeDensity;
        s.driveCable = cfg.driveCable;
        s.autoCache = cfg.autoCache;
        s.smartSpeed = cfg.smartSpeed;
        s.allowWrites = cfg.allowWrites;
        m_userPrefs.SetFbSlotSettings(cfg.driveSlot, s);
    });
    m_floppyDrivesView->SetOnSelectedSlotChanged(
        [this](int slot) { m_userPrefs.SetFbSelectedSlot(slot); });
    // The image half of the window shares the emulator Drives menu's plumbing, so a
    // document mounted from either place is recorded the same way.
    m_floppyDrivesView->SetOnGetDocuments([this]() { return GetDiskDocumentInfos(); });
    m_floppyDrivesView->SetOnMountDocument(
        [this](int slot, uint32_t docId) { MountDocumentToDrive(slot, docId); });
    m_floppyDrivesView->SetOnMountFile([this](int slot) { MountFileToDriveAsDocument(slot); });
    m_floppyDrivesView->SetOnEject([this](int slot) {
        if (m_emulatorView)
            m_emulatorView->EjectDrive(slot);
    });
    m_floppyDrivesView->SetOnSetEnabled([this](int slot, bool enabled) {
        if (m_emulatorView)
            m_emulatorView->SetDriveEnabled(slot, enabled);
    });
    m_floppyDrivesView->SetOnSetWriteProtect(
        [this](int slot, bool writeProtected) { SetDriveWriteProtected(slot, writeProtected); });

    // Restore the saved bridge configuration, one per slot - configuration only. A slot
    // is never re-attached on startup: live hardware state cannot be restored, and
    // spinning up a physical drive without being asked would be wrong.
    for (int d = 0; d < 4; ++d) {
        const auto& s = prefs.fbSlots[static_cast<size_t>(d)];
        ui::FloppyDrivesView::Config fb;
        fb.hardwareType = s.hardwareType;
        fb.comPort = s.comPort;
        fb.bridgeMode = s.bridgeMode;
        fb.bridgeDensity = s.bridgeDensity;
        fb.driveCable = s.driveCable;
        fb.driveSlot = d;
        fb.autoCache = s.autoCache;
        fb.smartSpeed = s.smartSpeed;
        fb.allowWrites = s.allowWrites;
        m_floppyDrivesView->SetSlotConfig(d, fb);
    }
    m_floppyDrivesView->SetSelectedSlot(prefs.fbAssignedDrive);
    if (prefs.showLiveFloppy)
        m_floppyDrivesView->Open();

    SetupMcpServer();
    SetupGdbServer();
}

// Wiring is grouped by collaborator; the calls run in the original single-function
// order (the 835-line body was split along its existing collaborator boundaries).
void Application::SetupSharedCallbacks()
{
    WireFrameTimeline();
    WireHexEditorCallbacks();
    WireFindNavigationCallbacks();
    WireDebuggerCallbacks();
    WireEmulatorOverlayCallbacks();
    WireAddressContexts();
    WireFindDataCallbacks();
    WireBlockGridCallbacks();
    WireMediaCallbacks();
}

std::string Application::GenerateMcpToken()
{
    // The token is the only gate on full emulator/memory control, so it must come from the OS
    // CSPRNG — a seeded PRNG caps the entropy at its seed width regardless of token length.
    std::string token = core::SecureRandomHex(16);
    if (token.empty())
        Logger::Error("MCP: OS random source unavailable; token generation failed");
    return token;
}

core::AudioLogSnapshot Application::MakeAudioLogSnapshot() const
{
    return m_logSnapshots.MakeAudioLogSnapshot();
}

core::ScenarioResult Application::ApplyScenarioRequest(const core::ScenarioRequest& req)
{
    core::ScenarioResult out;
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;

    switch (req.verb) {
        case core::ScenarioRequest::Verb::Status:
            out.ok = true;
            out.message = m_scenarioRecorder.IsRecording()
                              ? "Recording: " + std::to_string(m_scenarioRecorder.FrameCount()) +
                                    " frames captured"
                              : "Not recording";
            return out;

        case core::ScenarioRequest::Verb::Record: {
            if (!emu) {
                out.message = "Error: no machine";
                return out;
            }
            core::ScenarioRecorder::Options options;
            options.coldBoot = req.coldBoot;
            options.notes = req.notes;
            options.anyDriveBridged = [this]() {
                return m_emulatorView && m_emulatorView->AnyDriveBridged();
            };
            options.media = [this]() { return DescribeMountedMedia(); };

            std::string error;
            if (!m_scenarioRecorder.Start(*emu, std::move(options), error)) {
                out.message = "Error: " + error;
                return out;
            }
            m_scenarioPath = req.path;
            out.ok = true;
            out.message =
                std::string("Recording to ") + req.path + (req.coldBoot ? " (cold boot)" : "");
            return out;
        }

        case core::ScenarioRequest::Verb::Stop: {
            std::string error;
            const size_t frames = m_scenarioRecorder.FrameCount();
            if (!m_scenarioRecorder.Stop(m_scenarioPath, error)) {
                out.message = "Error: " + error;
                return out;
            }
            out.ok = true;
            out.message = "Wrote " + m_scenarioPath + ": " + std::to_string(frames) + " frames";
            m_scenarioPath.clear();
            return out;
        }

        case core::ScenarioRequest::Verb::Play: {
            if (!emu) {
                out.message = "Error: no machine";
                return out;
            }
            if (m_scenarioRecorder.IsRecording()) {
                out.message = "Error: still recording - SCEN STOP first";
                return out;
            }
            core::ScenarioFile scenario;
            std::string error;
            if (!scenario.Load(req.path, error)) {
                out.message = "Error: " + error;
                return out;
            }
            // The same guard the headless runner applies: replaying against a different
            // machine or a different disk cannot mean anything, so say so rather than
            // producing frames nobody can interpret.
            {
                core::ScenarioManifest actual;
                actual.model = GetAmigaModelInfo(emu->GetCurrentModel()).id;
                actual.videoStandard = scenario.manifest.videoStandard;
                actual.videoResolution = scenario.manifest.videoResolution;
                for (const auto& [key, value] : emu->GetUserVariables())
                    if (key == "puae_video_standard")
                        actual.videoStandard = value;
                actual.media = DescribeMountedMedia();
                std::string why;
                if (!scenario.MachineMatches(actual, why)) {
                    out.message = "Error: not the machine this was recorded on - " + why;
                    return out;
                }
            }

            // Playback drives the machine itself, so take it off the timeline transport first -
            // the same choke point every other live-running action goes through.
            ReleaseTimelineMachine();

            core::ScenarioPlayer::Options options;
            options.present = true; // this one has a window in front of it
            const core::ScenarioPlayer::Result result =
                core::ScenarioPlayer::Replay(*emu, scenario, options);
            if (!result.ok) {
                out.message = "Error: " + result.error;
                return out;
            }
            out.ok = true;
            out.message =
                "Replayed " + std::to_string(result.framesRun) + " frames from " + req.path;
            if (!scenario.golden.Empty()) {
                const core::ScenarioComparison cmp = core::CompareToGolden(scenario.golden, result);
                out.message += cmp.matched ? " - matched its goldens" : " - " + cmp.summary;
            }
            return out;
        }
    }

    out.message = "Error: unknown scenario verb";
    return out;
}

std::vector<core::ScenarioMedia> Application::DescribeMountedMedia() const
{
    std::vector<core::ScenarioMedia> media;
    if (!m_floppyDrivesView)
        return media;
    // Identity is the content MD5 of the document backing the slot; a slot backed by
    // nothing, or by live hardware, contributes nothing to describe.
    for (int slot = 0; slot < 4; ++slot) {
        const ui::FloppyDrivesView::Status status = GetLiveFloppyStatus(slot);
        if (!status.imageMounted || status.assigned)
            continue;
        core::ScenarioMedia item;
        item.drive = static_cast<unsigned>(slot);
        item.name = status.imageName;
        for (const auto& doc : m_documents) {
            if (std::filesystem::path(doc->currentFilePath).filename().string() != status.imageName)
                continue;
            std::error_code ec;
            item.size = std::filesystem::file_size(doc->currentFilePath, ec);
            if (!ec)
                item.md5 = core::MD5::ComputeFile(doc->currentFilePath);
            break;
        }
        media.push_back(std::move(item));
    }
    return media;
}

core::AudioRecordResult Application::ApplyAudioRecordRequest(const core::AudioRecordRequest& req)
{
    return m_logSnapshots.ApplyAudioRecordRequest(req);
}

bool Application::ArmDiskBreak(int access, int cyl, int side, int sector, uint32_t addrLo,
                               uint32_t addrHi) const
{
    auto* bps = m_debuggerView ? m_debuggerView->GetBreakpointsView() : nullptr;
    if (!bps)
        return false;
    if (m_emulatorView && m_emulatorView->IsRunning()) {
        auto* emu = m_emulatorView->GetEmulator();
        if (!emu || !emu->HasDiskBreak())
            return false; // core predates the disk breakpoint exports
    }
    ui::BreakpointsView::DiskBreak db;
    db.access = access;
    db.cyl = cyl;
    db.side = side;
    db.sector = sector;
    db.addrLo = addrLo;
    db.addrHi = addrHi;
    bps->SetDiskBreak(db);
    return true;
}

core::ActiveDebugInfo Application::GetActiveDebugInfo() const
{
    core::ActiveDebugInfo fromDocument;
    if (m_activeDocument && m_activeDocument->debugSymbols) {
        fromDocument.symbols = m_activeDocument->debugSymbols.get();
        fromDocument.lines = m_activeDocument->debugLines.get();
        fromDocument.inlines = m_activeDocument->debugInline.get();
        fromDocument.frames = m_activeDocument->debugFrame.get();
    }
    return m_compiledProgram.Resolve(fromDocument);
}

bool Application::ConsumeDosBreak()
{
    if (m_dosBreakpoints.Empty())
        return false;
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu)
        return false;
    adm::AmigaEmulator::CpuState cpu{};
    if (!emu->GetCpuState(cpu))
        return false;

    auto readReg = [&cpu](const std::string& name, uint32_t& out) {
        if (name.size() != 2 || name[1] < '0' || name[1] > '7')
            return false;
        const char kind = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
        const int idx = name[1] - '0';
        if (kind == 'd')
            out = cpu.d[idx];
        else if (kind == 'a')
            out = cpu.a[idx];
        else
            return false;
        return true;
    };

    auto decision = m_dosBreakpoints.OnBreak(cpu.pc, readReg, MakeExecReaders());
    if (!decision.ours)
        return false;
    if (decision.resume)
        return true;

    // A real stop: say which rule and what it carried, so the break banner is
    // not just an address.
    if (auto* console = m_debuggerView ? m_debuggerView->GetConsoleView() : nullptr) {
        for (const auto& r : m_dosBreakpoints.List())
            if (r.id == decision.ruleId) {
                console->AddOutputLine(decision.value.empty()
                                           ? fmt::format("*** {}/{}() ***", r.library, r.function)
                                           : fmt::format("*** {}/{}(\"{}\") ***", r.library,
                                                         r.function, decision.value));
                break;
            }
    }
    return false;
}

core::exec::Readers Application::MakeExecReaders()
{
    // Snapshot the region table once per call: the tools that use these do a
    // bounded walk, and re-reading the map per byte would dominate the cost.
    auto regions = std::make_shared<std::vector<adm::AmigaEmulator::MemoryRegion>>();
    if (auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr)
        *regions = emu->GetMemoryRegions();

    auto find = [regions](uint32_t addr) -> const uint8_t* {
        for (const auto& r : *regions)
            if (r.hostPtr && addr >= r.amigaAddr && addr - r.amigaAddr < r.size)
                return r.hostPtr + (addr - r.amigaAddr);
        return nullptr;
    };

    core::exec::Readers readers;
    readers.readByte = [find](uint32_t addr, bool& ok) -> uint8_t {
        const uint8_t* p = find(addr);
        ok = p != nullptr;
        return ok ? *p : uint8_t{0};
    };
    readers.readLong = [find](uint32_t addr, bool& ok) -> uint32_t {
        uint32_t v = 0;
        ok = true;
        for (int i = 0; i < 4; ++i) {
            const uint8_t* p = find(addr + static_cast<uint32_t>(i));
            if (!p) {
                ok = false;
                return 0;
            }
            v = (v << 8) | *p;
        }
        return v;
    };
    readers.isMapped = [find](uint32_t addr) { return find(addr) != nullptr; };
    return readers;
}

ui::UiSources Application::MakeUiSources()
{
    ui::UiSources src;
    src.mainDockId = m_mainDockId;
    // While a workspace load is settling, ApplyPendingWorkspaceLayout has detached every window
    // from every node and ReassertWorkspaceTabSelection is still re-pushing saved tab selections.
    // Geometry read now is in flux, and a dock change made now is undone a frame later.
    src.layoutSettling = m_workspaceLayout.IsSettling();

    for (const auto& w : GetWindowStates()) {
        ui::UiRegistryEntry e;
        e.name = w.name;
        e.imguiId = w.imguiId.empty() ? w.name : w.imguiId;
        e.group = w.groupName;
        e.openPtr = w.openPtr;
        e.isWindow = w.isWindow;
        src.windows.push_back(std::move(e));
    }
    for (const auto& p : m_panelManager.Panels()) {
        ui::UiPanelEntry e;
        e.name = p->GetName();
        e.id = p->GetId();
        e.openPtr = p->GetOpenStatePtr();
        src.panels.push_back(std::move(e));
    }
    if (m_documentsPanel) {
        ui::UiPanelEntry e;
        e.name = m_documentsPanel->GetName();
        e.id = m_documentsPanel->GetId();
        e.openPtr = m_documentsPanel->GetOpenStatePtr();
        e.permanent = true;
        src.panels.push_back(std::move(e));
    }
    return src;
}

// Read an AmigaDOS BSTR (a BPTR to a length-prefixed, non-terminated string).
static std::string ReadBstr(core::ArMemoryAccess& mem, uint32_t bptr)
{
    const uint32_t addr = bptr << 2; // BPTR2APTR
    bool ok = false;
    const uint8_t len = mem.ReadByte(addr, &ok);
    if (!ok || len == 0 || len > 107) // AmigaDOS caps a BSTR at 255, filenames far below that
        return {};
    std::string out;
    out.reserve(len);
    for (uint8_t i = 0; i < len; ++i) {
        const uint8_t c = mem.ReadByte(addr + 1 + i, &ok);
        if (!ok)
            return {};
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// The Breakpoints window lives inside DebuggerView; reach it through there so GDB shares the
// exact same breakpoint list the UI shows.
ui::BreakpointsView* Application::BreakpointsViewPtr()
{
    return m_debuggerView ? m_debuggerView->GetBreakpointsView() : nullptr;
}

void Application::SetupDocumentCallbacks(DiskDocument* doc)
{
    // Images open write-protected unless the preference says otherwise. AdfHandler
    // already defaults to protected; this applies the user's choice and pushes it
    // into the views that gate on it.
    if (doc->kind != DocumentKind::HostDirectory) {
        doc->volume.SetWriteProtected(m_userPrefs.Get().openImagesWriteProtected);
        ApplyDocumentWriteProtection(doc);
        doc->fileListView->SetOnToggleWriteProtect([this](uint32_t id, bool writeProtected) {
            SetDocumentWriteProtected(id, writeProtected);
        });
        doc->fileListView->SetOnSelectPartition(
            [this, doc](uint32_t, int index) { SelectDocumentPartition(doc, index); });
    }

    doc->fileListView->SetOnClose([this, doc](uint32_t /*id*/) {
        if (m_activeDocument != doc)
            SwitchActiveDocument(doc);

        if (doc->HasUnsavedChanges()) {
            std::string filename = std::filesystem::path(doc->currentFilePath).filename().string();
            m_unsavedChanges.Enqueue(
                filename, [this]() { return SaveAdf(); },
                [this, doc]() { QueueCloseDocument(doc); });
        } else {
            QueueCloseDocument(doc);
        }
    });

    // Retry a failed download placeholder using the original request.
    doc->fileListView->SetOnRetryDownload([this, doc](uint32_t /*id*/) {
        Logger::Info("Download: retrying '{}'", doc->downloadRequest.filename);
        doc->download = m_downloadManager.Start(doc->downloadRequest);
        doc->loadState = core::DocLoadState::Downloading;
    });

    doc->fileListView->SetOnFileDoubleClicked(
        [this](uint32_t block) { m_hexEditorView->GoToBlock(block); });

    doc->fileListView->SetOnOpenFile([this, doc](uint32_t sector, const std::string& filename) {
        m_viewerLauncher.OpenFileViewer(doc, sector, filename);
    });

    doc->fileListView->SetOnViewImage([this, doc](uint32_t sector, const std::string& filename) {
        m_viewerLauncher.OpenImageViewer(doc, sector, filename);
    });

    doc->fileListView->SetOnListenAudio([this, doc](uint32_t sector, const std::string& filename) {
        m_viewerLauncher.OpenAudioPlayer(doc, sector, filename);
    });

    doc->fileListView->SetOnViewHunks([this, doc](uint32_t sector, const std::string& filename) {
        m_viewerLauncher.OpenHunkViewer(doc, sector, filename);
    });

    doc->fileListView->SetOnRunFile([this, doc](uint32_t sector, const std::string& filename) {
        std::string content = doc->volume.ReadFileContent(sector);
        if (content.empty()) {
            Logger::Error("Failed to read file for execution: {}", filename);
            return;
        }

        std::vector<uint8_t> exeData(content.begin(), content.end());
        std::vector<uint8_t> adfBytes = AmigaEmulator::CreateBootableADF(exeData, filename);
        if (adfBytes.empty()) {
            Logger::Error("Failed to create bootable ADF for: {}", filename);
            m_errorMessage = "Failed to create bootable disk.\nThe emulator core might not be "
                             "loaded or the file might be invalid.";
            m_showErrorModal = true;
            return;
        }

        uint32_t targetDockId = 0;
        if (ImGui::DockBuilderGetNode(kDocumentsPanelDockId) != nullptr)
            targetDockId = kDocumentsPanelDockId;

        auto runDoc = std::make_unique<DiskDocument>();
        runDoc->id = m_nextDocId++;
        runDoc->initialDockId = targetDockId;
        runDoc->fileListView = std::make_unique<ui::FileListView>();
        runDoc->fileListView->SetDocumentId(runDoc->id);
        if (targetDockId != 0)
            runDoc->fileListView->SetTargetDockId(targetDockId);
        runDoc->diskSearcher = std::make_unique<core::DiskSearcher>();

        if (!runDoc->volume.Mount(adfBytes)) {
            Logger::Error("Failed to mount run ADF for: {}", filename);
            return;
        }

        runDoc->forceUnsaved = true;
        runDoc->fileListView->SetFilename(filename);

        // This callback fires from FileListView::Draw while RunFrame is range-iterating
        // m_documents — pushing here would reallocate the vector under the live loop. Queue
        // the document instead; RunFrame drains the queue after the loop, mirroring how
        // closes are deferred via m_deferredCloseDocuments.
        DeferredRunDocument pending;
        pending.doc = std::move(runDoc);
        pending.adfBytes = std::move(adfBytes);
        pending.filename = filename;
        m_deferredRunDocuments.push_back(std::move(pending));
    });

    doc->fileListView->SetOnSaveFile([this, doc](uint32_t sector, const std::string& fileName) {
        nfdchar_t* outPath = nullptr;
        nfdresult_t result = NFD_SaveDialog(&outPath, nullptr, 0, nullptr, fileName.c_str());

        if (result == NFD_OKAY) {
            std::string content = doc->volume.ReadFileContent(sector);
            std::ofstream outFile(outPath, std::ios::binary);
            if (outFile) {
                outFile.write(content.data(), content.size());
                outFile.close();
                if (outFile.good())
                    Logger::Info("File saved to: {}", outPath);
                else
                    Logger::Error("Write failed (disk full?): {}", outPath);
            } else {
                Logger::Error("Failed to write to file: {}", outPath);
            }
            NFD_FreePath(outPath);
        } else if (result == NFD_CANCEL) {
            Logger::Info("Save cancelled.");
        } else {
            Logger::Error("Error: {}", NFD_GetError());
        }
    });

    doc->fileListView->SetOnRenameFile([this, doc](uint32_t parentSector, uint32_t fileSector,
                                                   const std::string& oldName,
                                                   const std::string& newName) {
        if (doc->volume.RenameFile(parentSector, fileSector, oldName, newName)) {
            Logger::Info("Renamed '{}' to '{}'", oldName, newName);
            doc->fileListView->SetRoot(doc->volume.GetFileTree());
            if (doc == m_activeDocument) {
                m_hexEditorView->GoToBlock(m_hexEditorView->GetCurrentBlock());
            }
        } else {
            Logger::Error("Failed to rename '{}' to '{}'", oldName, newName);
        }
    });

    doc->fileListView->SetOnImportFile(
        [doc](uint32_t destinationSector) { doc->fileOpService->ImportFiles(destinationSector); });

    doc->fileListView->SetOnCreateDir([doc](uint32_t parentSector, const std::string& name) {
        doc->fileOpService->CreateDirectory(parentSector, name);
    });

    doc->fileListView->SetOnDeleteFiles(
        [doc](const std::vector<std::pair<uint32_t, std::string>>& items) {
            doc->fileOpService->DeleteFiles(items);
        });

    doc->fileListView->SetOnScrubFiles(
        [doc](const std::vector<std::pair<uint32_t, std::string>>& items) {
            doc->fileOpService->ScrubFiles(items);
        });

    doc->fileListView->SetOnExportFiles(
        [doc](const std::vector<FileNode>& nodes) { doc->fileOpService->ExportFiles(nodes); });

    doc->fileListView->SetOnPasteFiles([this, doc](uint32_t sourceDocId,
                                                   const std::vector<uint32_t>& sectors,
                                                   uint32_t destParentSector) {
        m_documentTransfer.PasteFiles(sourceDocId, sectors, doc, destParentSector);
    });

    doc->fileListView->SetOnRestoreFile([this, doc](uint32_t sector) {
        if (doc->volume.RestoreDeletedFile(sector)) {
            Logger::Info("Restored deleted file from sector {}", sector);
            m_pendingViewUpdate = true;
            return true;
        }
        Logger::Error("Failed to restore deleted file from sector {}", sector);
        return false;
    });

    doc->fileListView->SetOnSetPermissions(
        [this, doc](const std::vector<uint32_t>& sectors, int32_t access) {
            int successCount = 0;
            int failCount = 0;
            for (uint32_t sector : sectors) {
                if (doc->volume.SetPermissions(sector, access)) {
                    successCount++;
                } else {
                    failCount++;
                    Logger::Error("Failed to set permissions for sector {}", sector);
                }
            }
            if (successCount > 0) {
                if (sectors.size() == 1) {
                    Logger::Info("Permissions set for sector {}", sectors[0]);
                } else {
                    Logger::Info("Permissions set for {} items", successCount);
                }
                doc->fileListView->SetRoot(doc->volume.GetFileTree());
                if (doc == m_activeDocument) {
                    m_hexEditorView->GoToBlock(m_hexEditorView->GetCurrentBlock());
                }
            }
            if (failCount > 0) {
                Logger::Error("Failed to set permissions for {} items", failCount);
            }
        });

    doc->fileListView->SetOnMoveFiles(
        [this, doc](const std::vector<std::tuple<uint32_t, std::string, uint32_t>>& moves) {
            int successCount = 0;
            std::vector<std::string> failures;
            for (const auto& move : moves) {
                if (doc->volume.MoveFile(std::get<0>(move), std::get<1>(move), std::get<2>(move))) {
                    successCount++;
                } else {
                    failures.push_back(std::get<1>(move));
                }
            }

            if (successCount > 0) {
                Logger::Info("Moved {} items", successCount);
                UpdateDocumentViews(doc);
            }

            if (!failures.empty()) {
                std::string msg = fmt::format("Failed to move {} item(s):\n\n", failures.size());
                int shown = std::min(static_cast<int>(failures.size()), 5);
                for (int i = 0; i < shown; ++i)
                    msg += "  " + failures[i] + "\n";
                if (failures.size() > 5)
                    msg += fmt::format("\n... and {} more.", failures.size() - 5);
                msg += "\n(A name already exists in the destination, or the move is not allowed.)";
                Logger::Error("Move failed for {} item(s)", failures.size());
                m_errorMessage = msg;
                m_showErrorModal = true;
            }
        });

    SetupCrossDocumentTransfer(doc);
}

void Application::SetupCrossDocumentTransfer(DiskDocument* destDoc)
{
    if (!destDoc || !destDoc->fileListView)
        return;
    destDoc->fileListView->SetOnCrossDocumentDrop(
        [this, destDoc](uint32_t sourceDocId, const std::vector<uint32_t>& entries,
                        uint32_t destParent, bool isCopy) {
            m_documentTransfer.TransferEntries(sourceDocId, entries, destDoc, destParent, isCopy);
        });
    destDoc->fileListView->SetOnPasteFiles([this, destDoc](uint32_t sourceDocId,
                                                           const std::vector<uint32_t>& entries,
                                                           uint32_t destParent) {
        m_documentTransfer.TransferEntries(sourceDocId, entries, destDoc, destParent, true);
    });
}

void Application::ApplyPendingFontSettings()
{
    // Ctrl+wheel over a dense window, from last frame: 5% a notch, on that role only.
    const auto steps = ui::GlobalFontRoles().TakeSteps();
    if (std::any_of(steps.begin(), steps.end(), [](int n) { return n != 0; })) {
        auto fonts = m_userPrefs.Get().fonts;
        for (int i = 1; i < ui::kFontRoleCount; ++i) {
            float& s = fonts.roles[i].scale;
            s = std::round((s + 0.05f * static_cast<float>(steps[i])) * 20.0f) / 20.0f;
        }
        m_userPrefs.SetFontPrefs(fonts);
    }

    const auto& fonts = m_userPrefs.Get().fonts;
    if (m_appliedFonts && *m_appliedFonts == fonts)
        return;

    ImGuiStyle& style = ImGui::GetStyle();
    if (!m_appliedFonts || m_appliedFonts->uiScale != fonts.uiScale) {
        // Whole-UI zoom: padding, spacing, frame heights and rounding scale with the text, so a
        // larger interface is never a cramped one. Sizes come from the unscaled snapshot because
        // ScaleAllSizes compounds; colours and the font fields come from the live style.
        ImGuiStyle scaled = m_baseStyle;
        std::copy(std::begin(style.Colors), std::end(style.Colors), std::begin(scaled.Colors));
        scaled.ScaleAllSizes(m_dpiStyleScale * fonts.uiScale);
        scaled.FontSizeBase = style.FontSizeBase;
        scaled.FontScaleDpi = style.FontScaleDpi;
        scaled.FontScaleMain = fonts.uiScale;
        style = scaled;
    }

    // A different face or size changes how wide every cell's text is, but fixed-width columns
    // keep the widths they were given (or saved in table_layout.ini) and clip - Topaz is a good
    // deal wider than Cascadia. Queue an auto-fit on every table's columns; each one refits the
    // next time it is drawn. Not on the first application at startup: nothing is drawn yet, and
    // the saved widths are the ones the user chose at these settings.
    if (m_appliedFonts && !m_skipFontAutofit) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        for (int n = 0; n < g.Tables.GetMapSize(); ++n) {
            if (ImGuiTable* table = g.Tables.TryGetMapData(n))
                ImGui::TableSetColumnWidthAutoAll(table);
        }
    }

    auto& roles = ui::GlobalFontRoles();
    roles.Configure(fonts, m_fontManager);
    // nullptr puts the default back: the first font loaded.
    ImGui::GetIO().FontDefault = roles.Font(ui::FontRole::Interface);
    m_appliedFonts = fonts;
    m_skipFontAutofit = false;
}

void Application::LoadFonts(ImGuiIO& io, float scale)
{
    m_fontManager.LoadFonts(io, scale);

    m_availableFonts.clear();

    for (const auto& fontInfo : m_fontManager.GetAvailableFonts()) {
        m_availableFonts.push_back(fontInfo.name);
    }

    // Update existing viewers if any
    for (auto& doc : m_documents) {
        for (auto& viewer : doc->fileViewers) {
            viewer->ClearFonts();
            for (const auto& fontInfo : m_fontManager.GetAvailableFonts()) {
                viewer->AddFont(fontInfo.name, fontInfo.ptr);
            }
            viewer->SetCurrentFont(m_userPrefs.Get().fontName);
        }
    }
}

void Application::ImportFiles(uint32_t destinationSector)
{
    if (m_activeDocument)
        m_activeDocument->fileOpService->ImportFiles(destinationSector);
}

void Application::CreateDirectory(uint32_t parentSector, const std::string& name)
{
    if (m_activeDocument)
        m_activeDocument->fileOpService->CreateDirectory(parentSector, name);
}

void Application::DeleteFiles(const std::vector<std::pair<uint32_t, std::string>>& items)
{
    if (m_activeDocument)
        m_activeDocument->fileOpService->DeleteFiles(items);
}

void Application::ScrubFiles(const std::vector<std::pair<uint32_t, std::string>>& items)
{
    if (m_activeDocument)
        m_activeDocument->fileOpService->ScrubFiles(items);
}

void Application::ExportFiles(const std::vector<FileNode>& nodes)
{
    if (m_activeDocument)
        m_activeDocument->fileOpService->ExportFiles(nodes);
}

// Edit Menu Proxy Methods
void Application::TriggerNewFolder()
{
    if (m_activeDocument)
        m_activeDocument->fileListView->TriggerNewFolder();
}
void Application::TriggerDelete()
{
    if (m_activeDocument)
        m_activeDocument->fileListView->TriggerDelete();
}
void Application::TriggerScrub()
{
    if (m_activeDocument)
        m_activeDocument->fileListView->TriggerScrub();
}
void Application::TriggerRename()
{
    if (m_activeDocument)
        m_activeDocument->fileListView->TriggerRename();
}
void Application::TriggerPermissions()
{
    if (m_activeDocument)
        m_activeDocument->fileListView->TriggerPermissions();
}
void Application::TriggerRestore()
{
    if (m_activeDocument)
        m_activeDocument->fileListView->TriggerRestore();
}
void Application::TriggerExport()
{
    if (m_activeDocument)
        m_activeDocument->fileListView->TriggerExport();
}

bool Application::CanCreateNewFolder() const
{
    return m_activeDocument && m_activeDocument->fileListView->CanCreateNewFolder();
}
bool Application::CanDelete() const
{
    return m_activeDocument && m_activeDocument->fileListView->CanDelete();
}
bool Application::CanScrub() const
{
    return m_activeDocument && m_activeDocument->fileListView->CanScrub();
}
bool Application::CanRename() const
{
    return m_activeDocument && m_activeDocument->fileListView->CanRename();
}
bool Application::CanSetPermissions() const
{
    return m_activeDocument && m_activeDocument->fileListView->CanSetPermissions();
}
bool Application::CanRestore() const
{
    return m_activeDocument && m_activeDocument->fileListView->CanRestore();
}
bool Application::CanExport() const
{
    return m_activeDocument && m_activeDocument->fileListView->CanExport();
}

std::vector<ui::DiskDocumentInfo> Application::GetDiskDocumentInfos() const
{
    std::vector<ui::DiskDocumentInfo> infos;
    infos.reserve(m_documents.size());

    for (const auto& doc : m_documents) {
        ui::DiskDocumentInfo info;
        info.id = doc->id;
        info.name = doc->currentFilePath.empty()
                        ? "Untitled"
                        : std::filesystem::path(doc->currentFilePath).filename().string();
        info.isActive = (doc.get() == m_activeDocument);
        info.isLoadedInEmulator = m_emulatorView->IsDocumentLoaded(doc->id);
        info.isHardDiskImage = doc->IsHardfile();
        info.mountedOnDrive = -1;
        for (int d = 0; d < 4; d++) {
            if (m_emulatorView->GetDriveState(d).documentId == doc->id) {
                info.mountedOnDrive = d;
                break;
            }
        }
        infos.push_back(info);
    }

    return infos;
}

void Application::ValidateDisk()
{
    if (!m_activeDocument || !m_activeDocument->volume.IsMounted()) {
        m_errorMessage = "No disk is currently loaded.";
        m_showErrorModal = true;
        return;
    }

    Logger::Info("Validating disk filesystem...");

    auto result = DiskValidator::Validate(m_activeDocument->volume);

    // Append bootblock internal identification info
    if (m_activeDocument->bootBlockInfo.identified) {
        ValidationIssue issue;
        issue.category = ValidationCategory::BootBlock;
        issue.blockNumber = 0;
        issue.isRepairable = false;

        auto addIssue = [&](const std::string& msg, ValidationSeverity severity) {
            issue.message = msg;
            issue.severity = severity;
            result.issues.push_back(issue);

            switch (severity) {
                case ValidationSeverity::Info:
                    result.infoCount++;
                    break;
                case ValidationSeverity::Warning:
                    result.warningCount++;
                    break;
                case ValidationSeverity::Error:
                    result.errorCount++;
                    break;
                case ValidationSeverity::Success:
                    result.successCount++;
                    break;
            }
        };

        ValidationSeverity baseSeverity = m_activeDocument->bootBlockInfo.isVirus
                                              ? ValidationSeverity::Error
                                              : ValidationSeverity::Info;

        addIssue(fmt::format("Bootblock Name: {}", m_activeDocument->bootBlockInfo.name),
                 baseSeverity);
        addIssue(fmt::format("Bootblock Class: {} ({})", m_activeDocument->bootBlockInfo.className,
                             m_activeDocument->bootBlockInfo.classCode),
                 baseSeverity);

        if (!m_activeDocument->bootBlockInfo.kickstart.empty()) {
            addIssue(
                fmt::format("Bootblock Kickstart: {}", m_activeDocument->bootBlockInfo.kickstart),
                ValidationSeverity::Info);
        }

        if (!m_activeDocument->bootBlockInfo.notes.empty()) {
            addIssue(fmt::format("Bootblock Notes: {}", m_activeDocument->bootBlockInfo.notes),
                     ValidationSeverity::Info);
        }

        addIssue(fmt::format("Bootblock Bootable: {}",
                             m_activeDocument->bootBlockInfo.bootable ? "Yes" : "No"),
                 ValidationSeverity::Info);

        if (!m_activeDocument->bootBlockInfo.crc32.empty()) {
            addIssue(fmt::format("Bootblock CRC32: {}", m_activeDocument->bootBlockInfo.crc32),
                     ValidationSeverity::Info);
        }
    }

    m_validationView->SetResults(result);
    m_validationView->Open();

    if (result.IsClean()) {
        Logger::Info("Disk validation complete: No issues found.");
    } else {
        Logger::Info("Disk validation complete: {} errors, {} warnings, {} info", result.errorCount,
                     result.warningCount, result.infoCount);
    }
}

void Application::OnClearBlock(uint32_t block)
{
    if (!m_activeDocument)
        return;
    m_activeDocument->fileOpService->ClearBlock(block);
    if (m_hexEditorView->GetCurrentBlock() == block) {
        m_hexEditorView->GoToBlock(block);
    }
}

void Application::OnSaveBlocks(const std::set<uint32_t>& blocks)
{
    if (!m_activeDocument)
        return;
    m_activeDocument->fileOpService->SaveBlocks(blocks);
}

void Application::SyncMintermToBlit(uint32_t ref)
{
    if (ref == UINT32_MAX || !m_blitProfiler || !m_blitterMintermView)
        return;

    const auto& records = m_blitProfiler->GetCurrentFrame();

    // An exact hit wins over any range test, and is looked for across the whole frame first: a
    // click in the Blitter window passes that row's own L-Ref, and one blit's [start, start +
    // dmaCycles] span touches the start of the blit after it, so a range test taken in list
    // order answers a back-to-back click with the previous blit.
    const core::BlitRecord* found = nullptr;
    for (const auto& r : records) {
        if (r.lRef == ref || r.startCycle == ref) {
            found = &r;
            break;
        }
    }

    // No row matched, so this is a scrub position: the blit running at that point, else the last
    // one that started before it.
    if (!found) {
        const core::BlitRecord* nearest = nullptr;
        for (const auto& r : records) {
            if (ref >= r.lRef && ref <= r.lRef + r.dmaCycles) {
                found = &r;
                break;
            }
            if (r.lRef <= ref && (!nearest || r.lRef > nearest->lRef))
                nearest = &r;
        }
        if (!found)
            found = nearest;
    }

    if (found)
        m_blitterMintermView->SyncToBlit(found->raw.bltcon0, found->raw.bltcon1);
}

// Applies the debugger panel's blit overlay settings to everything that has to know about them.
//
// The core-side provenance map is the expensive half -- roughly 50 MB of tables plus a per-fetch
// lookup -- so it is armed only when the overlay is both shown and actually using it. Turning it
// off releases the overlay's own frame-sized buffers too; the geometry boxes need neither.

void Application::DrawErrorModal()
{
    if (m_showErrorModal) {
        ImGui::OpenPopup("Error");
        m_showErrorModal = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Dummy(ImVec2(400.0f, 0.0f));
        ImGui::TextWrapped("%s", m_errorMessage.c_str());
        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Application::DrawVirusAlertModal()
{
    if (m_showVirusAlert) {
        ImGui::OpenPopup("Virus Detected!");
        m_showVirusAlert = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Virus Detected!", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 80, 80, 255));
        ImGui::TextUnformatted("WARNING: Bootblock virus detected!");
        ImGui::PopStyleColor();

        ImGui::Separator();
        if (m_activeDocument) {
            ImGui::Text("Name: %s", m_activeDocument->bootBlockInfo.name.c_str());
            ImGui::Text("Class: %s", m_activeDocument->bootBlockInfo.className.c_str());
            if (!m_activeDocument->bootBlockInfo.notes.empty()) {
                ImGui::Text("Notes: %s", m_activeDocument->bootBlockInfo.notes.c_str());
            }
        }
        ImGui::Separator();

        ImGui::TextWrapped("This disk contains a bootblock virus. The disk has been loaded but you "
                           "should exercise caution.");

        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool Application::SaveAdf()
{
    if (!m_activeDocument)
        return false;

    auto* doc = m_activeDocument;
    if (doc->kind == core::DocumentKind::HostDirectory) {
        Logger::Info("Save is not applicable to host-directory documents; changes are immediate");
        return false;
    }

    // Two writers on one file: the emulator has this hardfile open read-write, and saving the
    // in-memory copy over it would destroy what the guest has written since (and maybe its
    // filesystem). Every save path comes through here - Ctrl+S, the menu, "save changes?" on
    // close and exit. Save As to another file is still allowed.
    if (doc->hardfileRuntime.device && doc->hardfileRuntime.readWrite) {
        Logger::Warn("Save refused: the emulator has '{}' read-write", doc->currentFilePath);
        m_errorMessage = doc->hardfileRuntime.lockReason;
        m_showErrorModal = true;
        return false;
    }

    // A .hdz would have to be recompressed; it saves as a plain hardfile.
    if (!doc->loadedHdzPath.empty()) {
        Logger::Info("Peekaboo: document came from a .hdz - redirecting to Save As");
        return SaveAdfAs();
    }

    // If loaded from DMS, redirect to Save As since xdms doesn't support writing DMS
    if (!doc->loadedDmsPath.empty()) {
        Logger::Info("DMS files are read-only. Redirecting to Save As ADF...");
        return SaveAdfAs();
    }

    // Track/flux-level preservation images (IPF, UAE-1ADF, SCP) are read-only here.
    // A plain Save would write the decoded ADF straight over currentFilePath, and
    // that decode is lossy by construction - on a protected disk most tracks are
    // blank in it, and an SCP holds three revolutions of raw flux that no 880 KB
    // ADF can represent. Redirect so the original image survives.
    if (!doc->loadedIpfPath.empty() || !doc->loadedExtAdfPath.empty() ||
        !doc->loadedScpPath.empty()) {
        Logger::Info("Preservation images are read-only. Redirecting to Save As ADF...");
        return SaveAdfAs();
    }

    // If loaded from a remote URL, there is no local path to save to
    if (!doc->loadedFromUrl.empty()) {
        Logger::Info("Peekaboo: file loaded from URL, save not possible — redirecting to Save As");
        return SaveAdfAs();
    }

    // If loaded from an Amiga executable, there is no disk file to save to
    if (!doc->loadedExecutablePath.empty()) {
        Logger::Info("Executable-backed document: redirecting to Save As ADF...");
        return SaveAdfAs();
    }

    // If loaded from ZIP, save back to ZIP
    if (!doc->loadedZipPath.empty()) {
        int err = 0;
        zip_t* archive = zip_open(doc->loadedZipPath.c_str(), 0, &err);
        if (!archive) {
            zip_error_t error;
            zip_error_init_with_code(&error, err);
            Logger::Error("Failed to open ZIP archive for saving: {} ({})", doc->loadedZipPath,
                          zip_error_strerror(&error));
            zip_error_fini(&error);
            m_errorMessage = "Failed to open ZIP archive for saving.";
            m_showErrorModal = true;
            return false;
        }

        const auto& data = doc->volume.GetFileContent();

        zip_source_t* s = zip_source_buffer(archive, data.data(), data.size(), 0);
        if (!s) {
            Logger::Error("Failed to create ZIP source buffer");
            zip_close(archive);
            return false;
        }

        zip_int64_t index = zip_name_locate(archive, doc->loadedZipEntryName.c_str(), 0);

        if (index < 0) {
            if (zip_file_add(archive, doc->loadedZipEntryName.c_str(), s, ZIP_FL_OVERWRITE) < 0) {
                Logger::Error("Failed to add file to ZIP: {}", zip_strerror(archive));
                zip_source_free(s);
                zip_close(archive);
                return false;
            }
        } else {
            if (zip_file_replace(archive, index, s, 0) < 0) {
                Logger::Error("Failed to replace file in ZIP: {}", zip_strerror(archive));
                zip_source_free(s);
                zip_close(archive);
                return false;
            }
        }

        if (zip_close(archive) < 0) {
            Logger::Error("Failed to close/write ZIP archive");
            return false;
        }

        Logger::Info("Updated {} in {}", doc->loadedZipEntryName, doc->loadedZipPath);
        doc->volume.ClearModifiedState();
        auto info = doc->volume.GetDiskInfo();
        m_changeMapView->SetData(info, doc->volume.GetBlockModificationStatus());
        return true;
    }

    // In-memory documents with no backing file yet (e.g. New Disk) have nowhere to
    // save to — prompt for a destination instead of failing.
    if (doc->currentFilePath.empty()) {
        Logger::Info("Document has no backing file — redirecting to Save As");
        return SaveAdfAs();
    }

    // Standard save
    if (doc->volume.Save()) {
        Logger::Info("ADF saved successfully");
        doc->volume.ClearModifiedState();
        auto info = doc->volume.GetDiskInfo();
        m_changeMapView->SetData(info, doc->volume.GetBlockModificationStatus());
        return true;
    }
    Logger::Error("Failed to save ADF");
    m_errorMessage = "Failed to save ADF file.";
    m_showErrorModal = true;
    return false;
}

bool Application::SaveAdfAs()
{
    if (!m_activeDocument)
        return false;

    auto* doc = m_activeDocument;
    if (doc->kind == core::DocumentKind::HostDirectory) {
        Logger::Info(
            "Save As is not applicable to host-directory documents; changes are immediate");
        return false;
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filterItem[1] = {{"Amiga Disk File", "adf"}};
    nfdresult_t result = NFD_SaveDialog(&outPath, filterItem, 1, nullptr, "disk.adf");

    // Reset ImGui input state because NFD_SaveDialog blocks and can miss key-up events
    ImGui::GetIO().ClearInputKeys();

    if (result == NFD_OKAY) {
        if (doc->volume.SaveAs(outPath)) {
            Logger::Info("ADF saved as: {}", outPath);
            doc->volume.ClearModifiedState();
            doc->currentFilePath = outPath;

            // Clear ZIP/DMS/URL/run state as we are now working with a standalone ADF
            doc->loadedZipPath.clear();
            doc->loadedZipEntryName.clear();
            doc->loadedDmsPath.clear();
            doc->loadedFromUrl.clear();
            doc->forceUnsaved = false;

            // Update filename display
            std::string filename = std::filesystem::path(doc->currentFilePath).filename().string();
            doc->fileListView->SetFilename(filename);

            // Refresh change map
            auto info = doc->volume.GetDiskInfo();
            m_changeMapView->SetData(info, doc->volume.GetBlockModificationStatus());

            NFD_FreePath(outPath);
            return true;
        }
        Logger::Error("Failed to save ADF as: {}", outPath);
        m_errorMessage = fmt::format("Failed to save ADF file to:\n{}", outPath);
        m_showErrorModal = true;
        NFD_FreePath(outPath);
        return false;
    } else if (result == NFD_CANCEL) {
        Logger::Info("Save As cancelled.");
    } else {
        Logger::Error("NFD Error: {}", NFD_GetError());
    }
    return false;
}

bool Application::HasUnsavedChanges() const
{
    for (const auto& doc : m_documents) {
        if (doc->HasUnsavedChanges())
            return true;
    }
    return false;
}

bool Application::HasFileLoaded() const { return m_activeDocument != nullptr; }

bool Application::HasDiskImageLoaded() const
{
    return m_activeDocument && m_activeDocument->kind == core::DocumentKind::DiskImage &&
           m_activeDocument->volume.IsMounted();
}

bool Application::HasRunnableMedia() const
{
    if (HasDiskImageLoaded())
        return true;
    const auto& mounts = m_emulatorView->GetConfiguredHostMounts();
    return std::any_of(mounts.begin(), mounts.end(),
                       [](const core::HostDirectoryMount& mount) { return mount.enabled; });
}

void Application::CloseAdf()
{
    if (!m_activeDocument)
        return;
    auto* doc = m_activeDocument;
    if (doc->HasUnsavedChanges()) {
        std::string filename = std::filesystem::path(doc->currentFilePath).filename().string();
        m_unsavedChanges.Enqueue(
            filename, [this]() { return SaveAdf(); }, [this, doc]() { QueueCloseDocument(doc); });
    } else {
        CloseDocument(doc);
    }
}

void Application::CloseAllAdf()
{
    for (auto& doc : m_documents) {
        if (doc->HasUnsavedChanges()) {
            std::string filename = std::filesystem::path(doc->currentFilePath).filename().string();
            auto* docPtr = doc.get();
            m_unsavedChanges.Enqueue(
                filename,
                [this, docPtr]() {
                    auto* prev = m_activeDocument;
                    m_activeDocument = docPtr;
                    bool saved = SaveAdf();
                    m_activeDocument = prev;
                    return saved;
                },
                [this, docPtr]() { QueueCloseDocument(docPtr); });
        } else {
            QueueCloseDocument(doc.get());
        }
    }
}

void Application::RequestExit()
{
    if (!HasUnsavedChanges()) {
        m_wantsToExit = true;
        return;
    }
    // One generic dialog — saves the active document, then exits
    m_unsavedChanges.Enqueue(
        "", [this]() { return SaveAdf(); }, [this]() { m_wantsToExit = true; });
}

bool Application::IsBlockModified(uint32_t block) const
{
    if (!m_activeDocument)
        return false;
    return m_activeDocument->volume.IsBlockModified(block);
}

const BootBlockInfo& Application::GetBootBlockInfo() const
{
    if (m_activeDocument)
        return m_activeDocument->bootBlockInfo;
    return s_emptyBootBlockInfo;
}

void Application::OnInsertBinary(uint32_t startBlock)
{
    if (!m_activeDocument)
        return;
    m_activeDocument->fileOpService->InsertBinary(startBlock);
    m_hexEditorView->GoToBlock(startBlock);
}

void Application::ScanDiskTemplates()
{
    m_diskTemplates.clear();

    if (std::filesystem::exists("images") && std::filesystem::is_directory("images")) {
        for (const auto& entry : std::filesystem::directory_iterator("images")) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                if (ext == ".adf") {
                    m_diskTemplates.push_back(entry.path().filename().string());
                }
            }
        }
        std::sort(m_diskTemplates.begin(), m_diskTemplates.end());
    } else {
        Logger::Error("Images directory not found.");
    }
}

std::optional<std::vector<uint8_t>> Application::ReadDiskTemplate(const std::string& templateName)
{
    std::filesystem::path templatePath = std::filesystem::path("images") / templateName;
    std::ifstream in(templatePath, std::ios::binary);
    if (!in) {
        Logger::Error("Template not found: {}", templatePath.string());
        m_errorMessage = fmt::format("Template not found:\n{}", templatePath.string());
        m_showErrorModal = true;
        return std::nullopt;
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

void Application::OnNewDiskRequested(const std::string& templateName)
{
    auto bytes = ReadDiskTemplate(templateName);
    if (!bytes)
        return; // ReadDiskTemplate already surfaced the error

    // Create the disk purely in memory and mark it unsaved — no filename is
    // requested up front; the user is only prompted when they choose to Save.
    if (CreateInMemoryDiskDocument(std::move(*bytes), "Untitled.adf"))
        Logger::Info("Created new in-memory disk from template: {}", templateName);
    else {
        m_errorMessage = "Failed to create new disk.";
        m_showErrorModal = true;
    }
}

void Application::OnNewDiskAdvanced(const std::string& templateName, const std::string& label,
                                    bool bootable, const std::string& bootBlockName,
                                    bool emptyStartup)
{
    // Resolve the boot block bytes up front so we can fail before touching disk.
    std::vector<uint8_t> bbData;
    if (bootable) {
        bbData = m_bootBlockLibrary.Read(bootBlockName);
        if (bbData.size() < 1024) {
            m_errorMessage =
                fmt::format("Boot block '{}' could not be read from the library.", bootBlockName);
            m_showErrorModal = true;
            return;
        }
    }

    auto bytes = ReadDiskTemplate(templateName);
    if (!bytes)
        return; // ReadDiskTemplate already surfaced the error

    // Build the disk in memory; no filename is requested up front. The tab title
    // follows the volume label (falling back to Untitled) and Save → Save As.
    std::string displayName = label.empty() ? "Untitled.adf" : label + ".adf";
    DiskDocument* doc = CreateInMemoryDiskDocument(std::move(*bytes), displayName);
    if (!doc) {
        m_errorMessage = "Failed to create new disk.";
        m_showErrorModal = true;
        return;
    }

    auto& vol = doc->volume;

    // Apply volume label (best-effort).
    if (!label.empty())
        vol.SetVolumeName(label);

    // Create an empty S/startup-sequence so a standard boot block boots cleanly.
    if (emptyStartup) {
        uint32_t sSector = vol.FindFile(0, "S");
        if (sSector == 0 && vol.CreateDirectory(0, "S"))
            sSector = vol.FindFile(0, "S");
        if (sSector != 0)
            vol.ImportFile(sSector, "startup-sequence", std::vector<uint8_t>{});
        else
            Logger::Warn("Could not create S/ directory for startup-sequence.");
    }

    // Install the selected boot block verbatim onto blocks 0 & 1.
    if (bootable) {
        std::vector<uint8_t> s0(bbData.begin(), bbData.begin() + 512);
        std::vector<uint8_t> s1(bbData.begin() + 512, bbData.begin() + 1024);
        InstallBootBlock(s0, s1);
    }

    // The disk lives in memory and stays marked unsaved (no vol.Save() here) so the
    // label/startup-sequence/boot block edits are persisted only when the user saves.
    // Refresh the views to reflect those edits in the file tree.
    UpdateDocumentViews(doc);

    Logger::Info("Created new in-memory disk from template: {}", templateName);
}

bool Application::CanFormatDisk() const { return GetFormatDiskUnavailableReason()[0] == '\0'; }

const char* Application::GetFormatDiskUnavailableReason() const
{
    if (!HasDiskImageLoaded())
        return "Open a disk image to format it.";
    // AdfHandler::Format is adflib's adfCreateFlop, which lays an 80-cylinder floppy
    // filesystem over block 0 of whatever device it is handed. On a hard-disk image that is the
    // RDB and the start of the first partition - not a format of the partition being browsed.
    if (m_activeDocument->IsHardfile())
        return "Hard-disk images cannot be formatted here. Format a partition from inside the "
               "emulated Amiga instead.";
    if (m_activeDocument->volume.IsWriteProtected())
        return "This image is write-protected.";
    return "";
}

std::string Application::GetActiveDocumentDisplayName() const
{
    if (!m_activeDocument)
        return {};
    if (m_activeDocument->currentFilePath.empty())
        return "Untitled";
    return std::filesystem::path(m_activeDocument->currentFilePath).filename().string();
}

std::string Application::GetActiveVolumeLabel() const
{
    if (!HasDiskImageLoaded())
        return {};
    return m_activeDocument->volume.GetDiskInfo().name;
}

bool Application::IsActiveVolumeOFS() const
{
    if (!HasDiskImageLoaded())
        return true;
    return m_activeDocument->volume.GetDiskInfo().type == "OFS";
}

void Application::OnFormatDisk(const std::string& label, bool useOFS)
{
    if (!HasDiskImageLoaded()) {
        m_errorMessage = "No disk image is currently loaded.";
        m_showErrorModal = true;
        return;
    }

    DiskDocument* doc = m_activeDocument;
    if (doc->IsHardfile()) {
        // See GetFormatDiskUnavailableReason: the floppy formatter would overwrite the RDB.
        m_errorMessage = "Hard-disk images cannot be formatted here.\n"
                         "Format a partition from inside the emulated Amiga instead.";
        m_showErrorModal = true;
        return;
    }
    if (doc->volume.IsWriteProtected()) {
        m_errorMessage = "This image is write-protected.\n"
                         "Clear the protection before formatting it.";
        m_showErrorModal = true;
        return;
    }

    // Format() rewrites the image through the memory device and remounts the volume, so the
    // document is left mounted and dirty - the file on disk changes only when it is saved.
    if (!doc->volume.Format(label, useOFS)) {
        m_errorMessage = "Failed to format the disk image.";
        m_showErrorModal = true;
        return;
    }

    // SetRoot() inside this drops the file tree the old filesystem owned, and with it any
    // selection pointing at sectors that no longer hold what they did. The fileOpService is
    // left alone deliberately: it holds a reference to the volume, not to its contents.
    UpdateDocumentViews(doc);

    Logger::Info("Formatted '{}' as {} with label '{}'", doc->currentFilePath,
                 useOFS ? "OFS" : "FFS", label);
}

void Application::ShowReadDisk() { m_floppyDiskView->Open(ui::FloppyDiskView::Mode::Read); }

void Application::ShowFormatFloppy() { m_floppyDiskView->Open(ui::FloppyDiskView::Mode::Format); }

void Application::ShowCompareFloppy() { m_floppyDiskView->Open(ui::FloppyDiskView::Mode::Compare); }

void Application::OnFloppyDiskRead(const std::vector<uint8_t>& buffer)
{
    // Full disk read complete — create a new document from the buffer.
    auto doc = std::make_unique<DiskDocument>();
    doc->id = m_nextDocId++;
    doc->fileListView = std::make_unique<ui::FileListView>();
    doc->fileListView->SetDocumentId(doc->id);
    if (ImGui::DockBuilderGetNode(kDocumentsPanelDockId) != nullptr)
        doc->fileListView->SetTargetDockId(kDocumentsPanelDockId);
    doc->diskSearcher = std::make_unique<core::DiskSearcher>();

    if (!doc->volume.Mount(buffer)) {
        Logger::Error("Failed to mount floppy disk filesystem");
        m_errorMessage = "Failed to mount the floppy disk filesystem.\n"
                         "The disk may not contain a valid AmigaDOS filesystem.";
        m_showErrorModal = true;
        return;
    }

    doc->currentFilePath = "[Floppy Disk]";
    doc->fileOpService = std::make_unique<FileOperationService>(doc->volume);

    DiskDocument* docPtr = doc.get();

    doc->fileOpService->SetUpdateCallback([this, docPtr](bool preserveNav) {
        UpdateDocumentViews(docPtr, preserveNav);
        if (preserveNav && m_activeDocument == docPtr) {
            m_hexEditorView->GoToBlock(m_hexEditorView->GetCurrentBlock());
        }
    });
    doc->fileOpService->SetErrorCallback([this](const std::string& msg) {
        m_errorMessage = msg;
        m_showErrorModal = true;
    });

    doc->diskSearcher->SetDiskInfo(doc->volume.GetTotalBlocks(), [docPtr](uint32_t block) {
        return docPtr->volume.ReadBlock(block);
    });

    if (doc->volume.HasFileSystem()) {
        std::vector<core::DiskSearcher::FilesystemFileInfo> fsFiles;
        std::function<void(const FileNode&)> flatten = [&](const FileNode& node) {
            if (!node.isDir && !node.isDeleted) {
                fsFiles.push_back({node.name, (uint32_t)node.startSector, node.size});
            }
            for (const auto& child : node.children)
                flatten(child);
        };
        flatten(doc->volume.GetFileTree());
        doc->diskSearcher->SetFilesystemInfo(
            std::move(fsFiles), [docPtr](uint32_t headerBlock) -> std::vector<uint8_t> {
                std::string content = docPtr->volume.ReadFileContent(headerBlock);
                return std::vector<uint8_t>(content.begin(), content.end());
            });
    } else {
        doc->diskSearcher->SetFilesystemInfo({}, nullptr);
    }

    SetupDocumentCallbacks(docPtr);
    m_documents.push_back(std::move(doc));
    SwitchActiveDocument(docPtr);
    UpdateDocumentViews(docPtr);

    // Identify bootblock
    auto sector0 = docPtr->volume.ReadBlock(0);
    auto sector1 = docPtr->volume.ReadBlock(1);
    docPtr->bootBlockInfo = m_brainFile.IdentifyBootBlock(sector0, sector1);

    if (docPtr->bootBlockInfo.isVirus) {
        Logger::Warn("VIRUS DETECTED: {}", docPtr->bootBlockInfo.name);
        m_showVirusAlert = true;
    }

    m_hexEditorView->SetBootBlockInfo(docPtr->bootBlockInfo);
    m_bootBlockView->SetData(sector0, sector1);
    m_bootBlockView->SetBootBlockInfo(docPtr->bootBlockInfo);

    Logger::Info("Floppy disk mounted successfully");
}

void Application::ShowBootBlockView()
{
    // The viewer doubles as the boot block library browser, so it is useful even with no disk
    // loaded — just open it and let the user inspect library entries (Current Disk shows a hint).
    if (m_activeDocument && m_activeDocument->volume.IsMounted()) {
        auto sector0 = m_activeDocument->volume.ReadBlock(0);
        auto sector1 = m_activeDocument->volume.ReadBlock(1);
        m_activeDocument->bootBlockInfo = m_brainFile.IdentifyBootBlock(sector0, sector1);

        m_bootBlockView->SetData(sector0, sector1);
        m_bootBlockView->SetBootBlockInfo(m_activeDocument->bootBlockInfo);
    }

    m_bootBlockView->Open();
}

void Application::InstallBootBlock(const std::vector<uint8_t>& sector0,
                                   const std::vector<uint8_t>& sector1)
{
    if (!m_activeDocument || !m_activeDocument->volume.IsMounted()) {
        m_errorMessage = "No disk is currently loaded.";
        m_showErrorModal = true;
        return;
    }
    if (sector0.size() != 512 || sector1.size() != 512) {
        m_errorMessage = "Invalid boot block data.";
        m_showErrorModal = true;
        return;
    }

    auto& vol = m_activeDocument->volume;
    if (!vol.WriteBlock(0, sector0) || !vol.WriteBlock(1, sector1)) {
        m_errorMessage = "Failed to write boot block.";
        m_showErrorModal = true;
        return;
    }
    vol.InvalidateBlockTypesCache();

    // Re-identify and refresh boot-block-aware views.
    auto s0 = vol.ReadBlock(0);
    auto s1 = vol.ReadBlock(1);
    m_activeDocument->bootBlockInfo = m_brainFile.IdentifyBootBlock(s0, s1);

    m_bootBlockView->SetData(s0, s1);
    m_bootBlockView->SetBootBlockInfo(m_activeDocument->bootBlockInfo);
    m_hexEditorView->SetBootBlockInfo(m_activeDocument->bootBlockInfo);
    m_hexEditorView->GoToBlock(m_hexEditorView->GetCurrentBlock());

    // Reflect the modified blocks in the change/usage maps.
    auto info = vol.GetDiskInfo();
    m_changeMapView->SetData(info, vol.GetBlockModificationStatus());
    m_blockMapView->SetData(info, vol.GetAllBlockTypes());

    Logger::Info("Boot block installed onto loaded disk.");
}

std::vector<std::string> Application::GetBootBlockNames() const
{
    std::vector<std::string> names;
    for (const auto& entry : m_bootBlockLibrary.Entries())
        names.push_back(entry.name);
    return names;
}

void Application::StartBootBlockPreview(const std::vector<uint8_t>& sector0,
                                        const std::vector<uint8_t>& sector1)
{
    m_bootBlockPreview.SetRomDirectory(m_emulatorView->GetRomDirectory());
    if (!m_bootBlockPreview.Start(sector0, sector1))
        m_bootBlockView->NotifyPreviewStopped();
}

void Application::StopBootBlockPreview() { m_bootBlockPreview.Stop(); }

void Application::DestroyBootBlockPreview() { m_bootBlockPreview.Destroy(); }

// The view-facing half of the preview: only the UI knows whether the tab is still open, on
// screen and focused. The machine mechanics are BootBlockPreview::Tick().
void Application::UpdateBootBlockPreview()
{
    if (!m_bootBlockPreview.IsAlive())
        return;

    // Halt (but keep alive for reuse) when the Boot Block Viewer is closed or stopped.
    if (!*m_bootBlockView->GetOpenStatePtr() || !m_bootBlockView->IsPreviewRunning()) {
        m_bootBlockView->ReleasePreviewMouse();
        StopBootBlockPreview();
        return;
    }

    const bool active = m_bootBlockView->IsPreviewTabActive();
    m_bootBlockPreview.Tick(active, active && m_bootBlockView->IsPreviewFocused());
}

void Application::IdentifyDisk()
{
    if (!m_activeDocument || !m_activeDocument->volume.IsMounted()) {
        m_errorMessage = "No disk is currently loaded.";
        m_showErrorModal = true;
        return;
    }

    if (!m_activeDocument->identifier) {
        m_activeDocument->identifier = std::make_unique<core::DiskIdentifier>();
        m_activeDocument->identifier->SetSourceBytes(MakeIdentifySource(m_activeDocument));
        m_identifyView->SetDocument(m_activeDocument->currentFilePath,
                                    m_activeDocument->identifier.get());
    }

    // Only start if not already in progress
    auto state = m_activeDocument->identifier->GetState();
    if (state != core::IdentifyState::Hashing && state != core::IdentifyState::SearchingByHash &&
        state != core::IdentifyState::SearchingByName) {
        m_activeDocument->identifier->StartIdentify(m_activeDocument->currentFilePath);
    }

    m_identifyView->Open();
}

void Application::ShowWriteDisk()
{
    // No early "no disk loaded" guard: the merged window's Write tab shows "No file loaded" and
    // disables the Write button itself, but Info/Bandwidth/RPM and the Read tab stay reachable.
    m_floppyDiskView->Open(ui::FloppyDiskView::Mode::Write);
}

void Application::TriggerGwInfo() { m_floppyDiskView->RunInfo(); }
void Application::TriggerGwBandwidth() { m_floppyDiskView->RunBandwidth(); }
void Application::TriggerGwRPM() { m_floppyDiskView->RunRPM(); }

void Application::RunAdf()
{
    // A hard-disk image boots from DH0: the emulator reads the file on disk, so unsaved edits in
    // the document are not part of that boot. The launch is transient, like a floppy in DF0 or
    // a WHDLoad game: the saved DH0 configuration is neither replaced nor inherited from.
    if (m_activeDocument && m_activeDocument->IsHardfile()) {
        if (m_activeDocument->HasUnsavedChanges())
            Logger::Warn("Booting '{}' from its file on disk; this document's unsaved changes are "
                         "not included",
                         m_activeDocument->currentFilePath);
        std::string error;
        if (!m_emulatorView->RunHardfile(std::filesystem::u8path(m_activeDocument->currentFilePath),
                                         error)) {
            m_errorMessage = "Failed to boot the hardfile from DH0: " + error;
            m_showErrorModal = true;
        }
        return;
    }

    const bool hasDisk = HasDiskImageLoaded();

    // A manually booted disk replaces any previous compile-and-run target.
    m_compiledProgram.ClearDebugInfo();

    // Mount the active document into DF0 unless it's already there. When the user has
    // explicitly configured DF0 via the drive Images submenu with the same document,
    // leave it untouched. If DF0 is empty or holds a stale/different document (e.g.
    // leftover from a previous run that was stopped), replace it so the selected
    // document always boots. MountDocumentToDrive handles IPF/ADF format selection.
    if (hasDisk) {
        const auto& df0 = m_emulatorView->GetDriveState(0);
        // A live drive owns DF0 outright - one slot cannot be backed by both a spinning
        // platter and an image, and the core has already cleared the slot's file.
        if (df0.isBridged) {
            Logger::Info("DF0 is a live physical drive; not mounting the active document into it");
        } else if (!df0.hasDisk || df0.documentId != m_activeDocument->id) {
            MountDocumentToDrive(0, m_activeDocument->id);
        }
    }

    if (!m_emulatorView->PowerOn()) {
        m_errorMessage = "Failed to start the emulator.\nCheck: "
                         "./emulation/puae_libretro.dylib (or .dll) exists.";
        m_showErrorModal = true;
        return;
    }

    m_emulatorView->Open();
}

void Application::SetDocumentWriteProtected(uint32_t docId, bool writeProtected)
{
    DiskDocument* doc = nullptr;
    for (auto& d : m_documents) {
        if (d->id == docId) {
            doc = d.get();
            break;
        }
    }
    if (!doc)
        return;

    if (doc->kind == DocumentKind::HostDirectory) {
        // Host directories have their own read-only switch, with a confirmation
        // step before real files on disk become writable. Leave it to that UI.
        return;
    }

    doc->volume.SetWriteProtected(writeProtected);
    ApplyDocumentWriteProtection(doc);

    // Any drive holding this document follows, so the chip, the Drives menu and the
    // document window cannot disagree.
    if (m_emulatorView) {
        for (int slot = 0; slot < 4; ++slot) {
            const auto& ds = m_emulatorView->GetDriveState(slot);
            if (ds.documentId == docId)
                m_emulatorView->SetDriveWriteProtect(slot, writeProtected);
        }
    }
    Logger::Info("Image '{}' is now {}",
                 doc->currentFilePath.empty() ? std::string("Untitled") : doc->currentFilePath,
                 writeProtected ? "write-protected" : "writable");
}

void Application::SetDriveWriteProtected(int drive, bool writeProtected)
{
    if (!m_emulatorView || drive < 0 || drive > 3)
        return;
    const auto& ds = m_emulatorView->GetDriveState(drive);
    if (ds.isBridged) {
        // The tab on the physical disk decides; the bridge only reports it.
        return;
    }
    m_emulatorView->SetDriveWriteProtect(drive, writeProtected);
    if (ds.documentId != 0)
        SetDocumentWriteProtected(ds.documentId, writeProtected);
}

// Pushes the document's write protection into the views that gate on it. The
// capability set is what greys the toolbar, the context menus and drag-drop; the
// block editor is the one write path that edits bytes rather than entries and so
// needs telling separately.
void Application::ApplyDocumentWriteProtection(DiskDocument* doc)
{
    if (!doc || doc->kind == DocumentKind::HostDirectory)
        return;
    const bool wp = doc->volume.IsWriteProtected();
    if (doc->fileListView) {
        DocumentCapabilities caps;
        caps.browse = true;
        caps.importFiles = !wp;
        caps.createDirectory = !wp;
        caps.rename = !wp;
        caps.deleteEntries = !wp;
        caps.rawBlocks = true;
        caps.allocationMap = true;
        caps.bootBlock = true;
        caps.saveImage = !(doc->hardfileRuntime.device && doc->hardfileRuntime.readWrite);
        caps.writeBlocks = !wp;
        doc->fileListView->SetCapabilities(caps);
        doc->fileListView->SetWriteProtected(wp);
    }
    if (m_hexEditorView && m_activeDocument == doc)
        m_hexEditorView->SetReadOnly(wp);
}

void Application::MountDocumentToDrive(int drive, uint32_t docId)
{
    // One slot cannot be backed by both a spinning platter and an image: the core has
    // already cleared the slot's file, so mounting here would leave the host showing a
    // disk the emulated machine cannot see. Detach the live drive first.
    if (m_emulatorView && m_emulatorView->GetDriveState(drive).isBridged) {
        Logger::Warn("DF{} is a live physical drive - detach it before mounting an image", drive);
        return;
    }

    DiskDocument* doc = nullptr;
    for (auto& d : m_documents) {
        if (d->id == docId) {
            doc = d.get();
            break;
        }
    }
    if (!doc)
        return;
    if (doc->IsHardfile()) {
        m_errorMessage =
            fmt::format("'{}' is a hard-disk image. Mount it on DH0 or DH1 in the "
                        "Hard Drives window instead of a floppy drive.",
                        std::filesystem::path(doc->currentFilePath).filename().string());
        m_showErrorModal = true;
        return;
    }

    const bool useRaw = !doc->emulatorImageBytes.empty();
    const auto& bytes = useRaw ? doc->emulatorImageBytes : doc->volume.GetImageContent();
    // PuAE detects the format by content magic, but the temp file's extension
    // routes the libretro loader: ".ipf" → CAPS, ".scp" → the flux reader,
    // everything else → the native reader (which handles raw ADF and UAE-1ADF
    // alike). Only IPF starts "CAPS" and only a flux dump starts "SCP".
    const bool isIpfBytes = bytes.size() >= 4 && bytes[0] == 'C' && bytes[1] == 'A' &&
                            bytes[2] == 'P' && bytes[3] == 'S';
    const bool isScpBytes =
        bytes.size() >= 3 && bytes[0] == 'S' && bytes[1] == 'C' && bytes[2] == 'P';
    std::string fileExt = "adf";
    if (useRaw && isIpfBytes)
        fileExt = "ipf";
    else if (useRaw && isScpBytes)
        fileExt = "scp";

    std::string name = doc->currentFilePath.empty()
                           ? "Untitled"
                           : std::filesystem::path(doc->currentFilePath).filename().string();
    if (doc->HasUnsavedChanges())
        name += " [modified]";

    m_emulatorView->MountDisk(drive, bytes, name, docId, fileExt, doc->IsWriteProtected());
}

void Application::MountFileToDriveAsDocument(int drive)
{
    if (drive < 0 || drive > 3)
        return;
    if (m_emulatorView && m_emulatorView->GetDriveState(drive).isBridged) {
        Logger::Warn("DF{} is a live physical drive - detach it before mounting an image", drive);
        return;
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"Amiga disk image", "adf,dms,ipf,scp,zip"}};
    const nfdresult_t result = NFD_OpenDialog(&outPath, filters, 1, nullptr);

    // NFD_OpenDialog blocks and can miss key-up events.
    ImGui::GetIO().ClearInputKeys();

    if (result != NFD_OKAY || !outPath)
        return;
    const std::string path(outPath);
    NFD_FreePath(outPath);

    if (!LoadAdf(path))
        return;
    // LoadAdf reports success for a load it only queued behind the unsaved-changes
    // prompt, so mount what actually opened rather than whatever is active.
    if (m_activeDocument && m_activeDocument->currentFilePath == path)
        MountDocumentToDrive(drive, m_activeDocument->id);
}

// ---------------------------------------------------------------------------
// .peek debug-session save / open
// ---------------------------------------------------------------------------

namespace {

std::string DocSourceKind(const DiskDocument* doc)
{
    if (doc->IsHardfile())
        return "hardfile";
    if (!doc->loadedIpfPath.empty() || !doc->emulatorImageBytes.empty())
        return "ipf";
    if (!doc->loadedDmsPath.empty())
        return "dms";
    if (!doc->loadedZipPath.empty())
        return "zip";
    if (!doc->loadedExecutablePath.empty())
        return "executable";
    return "adf";
}

} // namespace

core::PeekSession Application::BuildPeekSession()
{
    core::PeekSession s;
    s.version = core::SessionSerializer::kSupportedVersion;
    s.appVersion = std::string(core::constants::AppVersion);
    s.notes = m_sessionNotes;
    s.activeDocumentId = m_activeDocument ? m_activeDocument->id : 0;

    if (auto* emu = m_emulatorView->GetEmulator())
        s.coreVersion = emu->GetCoreVersion();

    // Documents — embed the decoded in-memory ADF (with edits) plus the raw IPF if present.
    for (const auto& doc : m_documents) {
        core::PeekDocument pd;
        pd.id = doc->id;
        pd.name = doc->currentFilePath.empty()
                      ? "Untitled"
                      : std::filesystem::path(doc->currentFilePath).filename().string();
        pd.originalPath = doc->currentFilePath;
        pd.zipEntryName = doc->loadedZipEntryName;
        pd.sourceKind = DocSourceKind(doc.get());
        pd.forceUnsaved = doc->forceUnsaved;
        pd.writeProtected = doc->IsWriteProtected();
        pd.isIpf = !doc->emulatorImageBytes.empty();
        pd.lastHexViewBlock = doc->lastHexViewBlock;
        pd.kind = doc->kind;
        if (doc->kind == core::DocumentKind::HostDirectory && doc->hostDirectory) {
            pd.canonicalPath = doc->hostDirectory->GetCanonicalRoot().string();
            pd.assignedDevice = doc->hostDirectory->GetAssignedDevice();
            pd.volumeLabel = doc->hostDirectory->GetVolumeLabel();
            pd.readOnly = doc->hostDirectory->IsReadOnly();
            pd.directoryFingerprint = doc->hostDirectory->GetScan().fingerprint;
            if (pd.assignedDevice) {
                const auto& effective =
                    m_emulatorView->GetEmulator()
                        ->GetHostMountService()
                        .GetEffectiveMounts()[static_cast<size_t>(*pd.assignedDevice)];
                pd.effectiveMount = effective.enabled && effective.hostDirectory ==
                                                             doc->hostDirectory->GetCanonicalRoot();
            }
        } else if (doc->IsHardfile()) {
            // Referenced, never embedded: a hardfile can be a gigabyte.
            pd.partition = doc->volume.GetSelectedPartition();
        } else {
            if (doc->volume.IsMounted())
                pd.adfBytes = doc->volume.GetImageContent();
            if (pd.isIpf)
                pd.ipfBytes = doc->emulatorImageBytes;
        }
        if (doc->fileListView) {
            const auto placement = doc->fileListView->GetWindowPlacement();
            if (placement.valid) {
                pd.windowState = {{"x", placement.x},
                                  {"y", placement.y},
                                  {"width", placement.width},
                                  {"height", placement.height},
                                  {"dockId", placement.dockId}};
            }
        }
        s.documents.push_back(std::move(pd));
    }

    const auto& mountService = m_emulatorView->GetEmulator()->GetHostMountService();
    const auto& mounts = m_emulatorView->IsRunning() ? mountService.GetEffectiveMounts()
                                                     : mountService.GetConfiguredMounts();
    for (size_t index = 0; index < mounts.size(); ++index) {
        if (!mounts[index].enabled)
            continue;
        core::PeekMountReference reference;
        reference.descriptor = mounts[index];
        reference.descriptor.source = core::HostMountSource::RestoredSession;
        if (mounts[index].IsHardfile()) {
            // A hardfile is referenced, never embedded, and has no directory fingerprint.
            reference.originalPath = mounts[index].hardfilePath.string();
            s.hostMounts[index] = std::move(reference);
            continue;
        }
        for (const auto& doc : s.documents) {
            if (doc.kind != core::DocumentKind::HostDirectory ||
                doc.canonicalPath != mounts[index].hostDirectory.string())
                continue;
            reference.originalPath = doc.originalPath;
            reference.documentId = doc.id;
            reference.directoryFingerprint = doc.directoryFingerprint;
            break;
        }
        if (reference.originalPath.empty())
            reference.originalPath = mounts[index].hostDirectory.string();
        if (reference.documentId == 0) {
            core::HostDirectoryDocument fingerprintSource;
            std::string fingerprintError;
            if (fingerprintSource.Open(mounts[index].hostDirectory, true, fingerprintError))
                reference.directoryFingerprint = fingerprintSource.GetScan().fingerprint;
            else
                Logger::Warn("Session save: unable to fingerprint '{}': {}",
                             mounts[index].hostDirectory.string(), fingerprintError);
        }
        s.hostMounts[index] = std::move(reference);
    }
    s.hardDriveControllerRoms = mountService.GetControllerRoms();
    s.cdImage = m_emulatorView->GetCdStatus().source.string();

    // Machine snapshot — only valid while the emulator is running.
    if (m_emulatorView->IsRunning()) {
        s.modelId = m_emulatorView->GetCurrentModelId();
        s.snapshot = m_emulatorView->SaveStateForRestore();
        s.snapshotPresent = !s.snapshot.empty();
        s.wasPaused = m_emulatorView->IsPaused();
        const auto& df0 = m_emulatorView->GetDriveState(0);
        s.df0DocId = df0.documentId;
        s.snapshotOwnerDocId = df0.documentId;
        s.snapshotFileExt = df0.fileExt;

        // Live drives: configuration only, never state. A bridged slot has no document,
        // so without this the slot would come back as a plain empty drive with no hint
        // that it was hardware-backed.
        for (int d = 0; d < 4; ++d) {
            const auto& ds = m_emulatorView->GetDriveState(d);
            if (!ds.isBridged)
                continue;
            // The request as it was attached, not the window's current (editable)
            // settings for the slot; the window is only the fallback for a slot
            // bridged before the config was recorded.
            const auto& fb = m_floppyDrivesView->GetSlotConfig(d);
            core::PeekSession::BridgedSlot slot;
            slot.drive = d;
            if (ds.bridgeConfig) {
                slot.hardwareType = ds.bridgeConfig->driverIndex;
                slot.comPort = ds.bridgeConfig->comPort;
                slot.bridgeMode = ds.bridgeConfig->bridgeMode;
                slot.bridgeDensity = ds.bridgeConfig->bridgeDensity;
                slot.driveCable = ds.bridgeConfig->driveCable;
                slot.autoCache = ds.bridgeConfig->autoCache;
                slot.smartSpeed = ds.bridgeConfig->smartSpeed;
                slot.allowWrites = ds.bridgeConfig->allowWrites;
            } else {
                slot.hardwareType = fb.hardwareType;
                slot.comPort = ds.bridgeComPort.empty() ? fb.comPort : ds.bridgeComPort;
                slot.bridgeMode = fb.bridgeMode;
                slot.bridgeDensity = fb.bridgeDensity;
                slot.driveCable = fb.driveCable;
                slot.autoCache = fb.autoCache;
                slot.smartSpeed = fb.smartSpeed;
                slot.allowWrites = fb.allowWrites;
            }
            slot.hardwareName = ds.bridgeHardware;
            s.bridgedSlots.push_back(std::move(slot));
        }

        // The recorded timeline, when the user has asked for it. ENGINE9000's toolbar
        // Save/Restore keeps the whole timeline rather than a single state, so a hard-won
        // rewind context survives the session.
        if (m_userPrefs.Get().frameHistoryPersistInSession &&
            m_timeline.History().FrameCount() > 0) {
            s.frameHistory = m_timeline.History().Serialize();
            s.frameHistoryPresent = !s.frameHistory.empty();
        }
    }

    // Breakpoints / watchpoints.
    if (auto* bpv = m_debuggerView->GetBreakpointsView())
        s.breakpoints = bpv->ExportEntries();

    // View / navigation state.
    nlohmann::json vs;
    if (auto* dis = m_debuggerView->GetDisassemblyView())
        vs["disasmAddress"] = dis->GetNavAddress();
    if (auto* mem = m_debuggerView->GetMemoryView())
        vs["memoryAddress"] = mem->GetCurrentAddress();
    vs["activeDocumentId"] = s.activeDocumentId;
    {
        size_t sz = 0;
        const char* ini = ImGui::SaveIniSettingsToMemory(&sz);
        if (ini && sz > 0)
            vs["imguiLayout"] = std::string(ini, sz);
    }
    s.viewState = std::move(vs);

    return s;
}

DiskDocument* Application::CreateDocumentFromPeek(const core::PeekDocument& pd)
{
    uint32_t targetDockId = 0;
    if (ImGui::DockBuilderGetNode(kDocumentsPanelDockId) != nullptr)
        targetDockId = kDocumentsPanelDockId;

    auto doc = std::make_unique<DiskDocument>();
    doc->id = pd.id; // preserve the original id so snapshot/view-state references stay valid
    doc->initialDockId = targetDockId;
    doc->fileListView = std::make_unique<ui::FileListView>();
    doc->fileListView->SetDocumentId(doc->id);
    if (targetDockId != 0)
        doc->fileListView->SetTargetDockId(targetDockId);
    if (pd.windowState.is_object() && pd.windowState.contains("width") &&
        pd.windowState.contains("height")) {
        ui::FileListView::WindowPlacement placement;
        placement.x = pd.windowState.value("x", 0.0f);
        placement.y = pd.windowState.value("y", 0.0f);
        placement.width = pd.windowState.value("width", 0.0f);
        placement.height = pd.windowState.value("height", 0.0f);
        placement.dockId = pd.windowState.value("dockId", 0u);
        placement.valid = placement.width > 0.0f && placement.height > 0.0f;
        if (placement.valid) {
            doc->fileListView->SetWindowPlacement(placement);
            doc->restoredWindowPlacement = true;
        }
    }
    doc->diskSearcher = std::make_unique<core::DiskSearcher>();

    if (pd.kind == core::DocumentKind::HostDirectory) {
        doc->kind = pd.kind;
        doc->currentFilePath = pd.originalPath.empty() ? pd.canonicalPath : pd.originalPath;
        doc->hostDirectory = std::make_unique<core::HostDirectoryDocument>();
        doc->savedDirectoryFingerprint = pd.directoryFingerprint;
        std::string error;
        const std::string root = pd.canonicalPath.empty() ? pd.originalPath : pd.canonicalPath;
        if (!root.empty() && std::filesystem::is_directory(std::filesystem::path(root)) &&
            doc->hostDirectory->Open(root, pd.readOnly, error, pd.assignedDevice, pd.volumeLabel)) {
            if (pd.directoryFingerprint != doc->hostDirectory->GetScan().fingerprint) {
                doc->directoryFingerprintChanged = true;
                Logger::Warn("Session restore: directory '{}' changed since snapshot", root);
            }
        } else {
            const std::string original = pd.originalPath.empty() ? root : pd.originalPath;
            doc->hostDirectory->SetPlaceholder(original, pd.assignedDevice, pd.volumeLabel,
                                               pd.readOnly);
            Logger::Warn("Session restore: directory '{}' is missing; retained placeholder", root);
        }
        doc->fileListView->SetFilename(pd.name);
        doc->fileListView->SetCapabilities(doc->hostDirectory->GetCapabilities());
        DiskDocument* docPtr = doc.get();
        doc->fileListView->SetOnClose([this, docPtr](uint32_t) { QueueCloseDocument(docPtr); });
        doc->fileListView->SetOnOpenFile(
            [this, docPtr](uint32_t entry, const std::string& filename) {
                m_viewerLauncher.OpenFileViewer(docPtr, entry, filename);
            });
        doc->fileListView->SetOnViewImage(
            [this, docPtr](uint32_t entry, const std::string& filename) {
                m_viewerLauncher.OpenImageViewer(docPtr, entry, filename);
            });
        doc->fileListView->SetOnListenAudio(
            [this, docPtr](uint32_t entry, const std::string& filename) {
                m_viewerLauncher.OpenAudioPlayer(docPtr, entry, filename);
            });
        doc->fileListView->SetOnViewHunks(
            [this, docPtr](uint32_t entry, const std::string& filename) {
                m_viewerLauncher.OpenHunkViewer(docPtr, entry, filename);
            });
        doc->fileListView->SetOnCreateDir([this, docPtr](uint32_t parent, const std::string& name) {
            auto found = docPtr->hostEntryIds.find(parent);
            if (found == docPtr->hostEntryIds.end())
                return;
            std::string error;
            if (!docPtr->hostDirectory->CreateDirectory(found->second, name, error))
                Logger::Error("Host directory: {}", error);
            docPtr->hostDirectory->RefreshNow(error);
            UpdateHostDirectoryDocument(docPtr);
        });
        doc->fileListView->SetOnImportFile([this, docPtr](uint32_t parent) {
            auto found = docPtr->hostEntryIds.find(parent);
            if (found == docPtr->hostEntryIds.end())
                return;
            const nfdpathset_t* paths = nullptr;
            if (NFD_OpenDialogMultiple(&paths, nullptr, 0, nullptr) != NFD_OKAY)
                return;
            nfdpathsetsize_t count = 0;
            NFD_PathSet_GetCount(paths, &count);
            std::string error;
            for (nfdpathsetsize_t index = 0; index < count; ++index) {
                nfdchar_t* selected = nullptr;
                NFD_PathSet_GetPath(paths, index, &selected);
                if (selected && !docPtr->hostDirectory->Import(found->second, selected, error))
                    Logger::Error("Host directory import: {}", error);
            }
            NFD_PathSet_Free(paths);
            docPtr->hostDirectory->RefreshNow(error);
            UpdateHostDirectoryDocument(docPtr);
        });
        doc->fileListView->SetOnRenameFile(
            [this, docPtr](uint32_t, uint32_t entry, const std::string&, const std::string& name) {
                auto found = docPtr->hostEntryIds.find(entry);
                if (found == docPtr->hostEntryIds.end())
                    return;
                std::string error;
                if (!docPtr->hostDirectory->Rename(found->second, name, error))
                    Logger::Error("Host directory: {}", error);
                docPtr->hostDirectory->RefreshNow(error);
                UpdateHostDirectoryDocument(docPtr);
            });
        doc->fileListView->SetOnDeleteFiles(
            [this, docPtr](const std::vector<std::pair<uint32_t, std::string>>& entries) {
                std::string error;
                for (const auto& [entry, name] : entries) {
                    (void)name;
                    auto found = docPtr->hostEntryIds.find(entry);
                    if (found != docPtr->hostEntryIds.end() &&
                        !docPtr->hostDirectory->Remove(found->second, error))
                        Logger::Error("Host directory: {}", error);
                }
                docPtr->hostDirectory->RefreshNow(error);
                UpdateHostDirectoryDocument(docPtr);
            });
        doc->fileListView->SetOnMoveFiles(
            [this, docPtr](const std::vector<std::tuple<uint32_t, std::string, uint32_t>>& moves) {
                std::string error;
                for (const auto& [sourceParent, name, parent] : moves) {
                    auto sourceParentId = docPtr->hostEntryIds.find(sourceParent);
                    auto destination = docPtr->hostEntryIds.find(parent);
                    if (sourceParentId == docPtr->hostEntryIds.end() ||
                        destination == docPtr->hostEntryIds.end())
                        continue;
                    const auto& scan = docPtr->hostDirectory->GetScan();
                    auto source =
                        std::find_if(scan.entries.begin(), scan.entries.end(),
                                     [&](const core::HostDirectoryEntry& candidate) {
                                         return candidate.parentId == sourceParentId->second &&
                                                candidate.name == name;
                                     });
                    if (source != scan.entries.end() &&
                        !docPtr->hostDirectory->Move(source->id, destination->second, error))
                        Logger::Error("Host directory move: {}", error);
                }
                docPtr->hostDirectory->RefreshNow(error);
                UpdateHostDirectoryDocument(docPtr);
            });
        SetupCrossDocumentTransfer(docPtr);
        doc->fileListView->SetOnRetryDownload([this, docPtr](uint32_t) {
            std::string error;
            const auto original = docPtr->hostDirectory->GetOriginalRoot();
            if (!docPtr->hostDirectory->Locate(original, error)) {
                Logger::Warn("Session restore: retrying directory '{}' failed: {}",
                             original.string(), error);
                return;
            }
            docPtr->currentFilePath = original.string();
            docPtr->directoryFingerprintChanged =
                docPtr->savedDirectoryFingerprint &&
                *docPtr->savedDirectoryFingerprint != docPtr->hostDirectory->GetScan().fingerprint;
            docPtr->fileListView->SetCapabilities(docPtr->hostDirectory->GetCapabilities());
            UpdateHostDirectoryDocument(docPtr);
            if (!RecoverRestoredHostMount(docPtr, error))
                Logger::Error("Session restore: unable to reconnect host mount: {}", error);
            Logger::Info("Session restore: directory '{}' is available again", original.string());
        });
        doc->fileListView->SetOnLocateDirectory([this, docPtr](uint32_t) {
            nfdchar_t* selected = nullptr;
            const auto original = docPtr->hostDirectory->GetOriginalRoot().string();
            if (NFD_PickFolder(&selected, original.empty() ? nullptr : original.c_str()) !=
                    NFD_OKAY ||
                !selected)
                return;
            std::string error;
            if (!docPtr->hostDirectory->Locate(selected, error)) {
                Logger::Error("Session restore: unable to locate directory: {}", error);
                NFD_FreePath(selected);
                return;
            }
            docPtr->currentFilePath = selected;
            docPtr->directoryFingerprintChanged =
                docPtr->savedDirectoryFingerprint &&
                *docPtr->savedDirectoryFingerprint != docPtr->hostDirectory->GetScan().fingerprint;
            docPtr->fileListView->SetFilename(
                docPtr->hostDirectory->GetCanonicalRoot().filename().string());
            docPtr->fileListView->SetCapabilities(docPtr->hostDirectory->GetCapabilities());
            UpdateHostDirectoryDocument(docPtr);
            if (!RecoverRestoredHostMount(docPtr, error))
                Logger::Error("Session restore: unable to reconnect host mount: {}", error);
            Logger::Info("Session restore: directory relocated to '{}'", selected);
            NFD_FreePath(selected);
        });
        m_documents.push_back(std::move(doc));
        UpdateHostDirectoryDocument(docPtr);
        if (pd.id >= m_nextDocId)
            m_nextDocId = pd.id + 1;
        return docPtr;
    }

    if (pd.sourceKind == "hardfile") {
        std::string extension = std::filesystem::path(pd.originalPath).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::error_code ec;
        if (pd.originalPath.empty() || !std::filesystem::is_regular_file(pd.originalPath, ec)) {
            Logger::Warn("Session restore: hardfile '{}' is missing; document skipped",
                         pd.originalPath);
            return nullptr;
        }
        if (!LoadHardfileDocument(std::move(doc), pd.originalPath, extension == ".hdz"))
            return nullptr;
        DiskDocument* restored = m_documents.back().get();
        if (pd.partition != restored->volume.GetSelectedPartition())
            SelectDocumentPartition(restored, pd.partition);
        restored->volume.SetWriteProtected(pd.writeProtected || !restored->loadedHdzPath.empty());
        ApplyDocumentWriteProtection(restored);
        if (pd.id >= m_nextDocId)
            m_nextDocId = pd.id + 1;
        return restored;
    }

    if (!doc->volume.Mount(pd.adfBytes)) {
        Logger::Error("Session restore: failed to mount document '{}'", pd.name);
        return nullptr;
    }

    doc->currentFilePath = pd.originalPath.empty() ? pd.name : pd.originalPath;
    doc->loadedZipEntryName = pd.zipEntryName;
    doc->forceUnsaved = pd.forceUnsaved;
    doc->lastHexViewBlock = pd.lastHexViewBlock;
    if (pd.sourceKind == "ipf" && !pd.ipfBytes.empty()) {
        doc->loadedIpfPath = pd.originalPath;
        doc->emulatorImageBytes = pd.ipfBytes;
    } else if (pd.sourceKind == "dms") {
        doc->loadedDmsPath = pd.originalPath;
    } else if (pd.sourceKind == "executable") {
        doc->loadedExecutablePath = pd.originalPath;
    }
    // Documents restored from a session have no backing file on disk by default; a save
    // should prompt for a destination.
    if (pd.originalPath.empty())
        doc->loadedFromUrl = pd.name;

    DiskDocument* docPtr = doc.get();
    FinalizeMountedDocument(docPtr);
    // The session's own flag wins over the "open protected" preference that
    // FinalizeMountedDocument just applied: this is a session being resumed, not a
    // fresh open.
    docPtr->volume.SetWriteProtected(pd.writeProtected);
    ApplyDocumentWriteProtection(docPtr);
    m_documents.push_back(std::move(doc));
    UpdateDocumentViews(docPtr);

    if (pd.id >= m_nextDocId)
        m_nextDocId = pd.id + 1;
    return docPtr;
}

bool Application::RecoverRestoredHostMount(DiskDocument* doc, std::string& outError)
{
    outError.clear();
    if (!doc || !doc->hostDirectory || !doc->hostDirectory->IsAvailable() ||
        !doc->hostDirectory->GetAssignedDevice())
        return true;

    auto* emulator = m_emulatorView->GetEmulator();
    auto& service = emulator->GetHostMountService();
    core::EmulatorLaunchProfile profile;
    if (service.GetEffectiveProfile())
        profile = *service.GetEffectiveProfile();
    else
        profile.modelId = m_emulatorView->GetCurrentModelId();

    const auto device = *doc->hostDirectory->GetAssignedDevice();
    const size_t index = static_cast<size_t>(device);
    auto& mount = profile.mounts[index];
    mount.device = device;
    mount.enabled = true;
    mount.hostDirectory = doc->hostDirectory->GetCanonicalRoot();
    mount.volumeLabel = doc->hostDirectory->GetVolumeLabel().empty()
                            ? core::HostMountDeviceName(device)
                            : doc->hostDirectory->GetVolumeLabel();
    mount.readOnly = doc->hostDirectory->IsReadOnly();
    mount.source = core::HostMountSource::RestoredSession;
    profile.transient = false;
    profile.bootIntent = std::any_of(profile.mounts.begin(), profile.mounts.end(),
                                     [](const core::HostDirectoryMount& candidate) {
                                         return candidate.enabled && candidate.bootPriority > -128;
                                     })
                             ? core::EmulatorBootIntent::Directory
                             : core::EmulatorBootIntent::Floppy;

    if (!m_emulatorView->StartHostMountProfile(profile, outError))
        return false;
    Logger::Info("Session restore: reconnected {} to '{}' without changing global preferences",
                 core::HostMountDeviceName(device), mount.hostDirectory.string());
    return true;
}

bool Application::RestorePeekSession(core::PeekSession& session)
{
    std::vector<std::string> changedMountDirectories;
    std::vector<std::string> readOnlyHardfiles; // restored read-only, not as the file asked
    // 1. A restored debug session replaces the current machine, so stop it explicitly before
    // closing its documents. Ordinary document closure only ejects the corresponding floppies.
    if (m_emulatorView->IsRunning())
        m_emulatorView->Stop();
    std::vector<DiskDocument*> toClose;
    for (auto& d : m_documents)
        toClose.push_back(d.get());
    for (auto* d : toClose)
        CloseDocument(d);
    m_activeDocument = nullptr;

    // 2. Recreate documents, preserving their original ids.
    DiskDocument* activeDoc = nullptr;
    for (const auto& pd : session.documents) {
        DiskDocument* doc = CreateDocumentFromPeek(pd);
        if (doc && doc->id == session.activeDocumentId)
            activeDoc = doc;
    }
    // The restored documents must land in the Documents panel. Opening a .peek also reloads the
    // saved ImGui layout (LoadIniSettingsFromMemory) a frame later, which rebuilds the dock tree
    // and can leave these freshly-created windows floating. Force them back into the Documents
    // panel for a few frames, applied right before the documents are drawn (when the panel's
    // dockspace is guaranteed to exist this frame) so it wins over the layout reload.
    m_restoreDockFrames = 4;

    if (!activeDoc && !m_documents.empty())
        activeDoc = m_documents.front().get();
    if (activeDoc)
        SwitchActiveDocument(activeDoc);

    const bool hasRestoredMounts =
        std::any_of(session.hostMounts.begin(), session.hostMounts.end(),
                    [](const auto& mount) { return mount && mount->descriptor.enabled; });
    if (m_documents.empty() && !hasRestoredMounts) {
        Logger::Warn("Session restore: no documents in .peek");
        m_sessionNotes = session.notes;
        return true;
    }

    // 3. Determine which document drives the emulator (the snapshot owner / DF0).
    uint32_t df0Id = session.df0DocId;
    bool df0Exists = false;
    for (auto& d : m_documents)
        if (d->id == df0Id)
            df0Exists = true;
    if (!df0Exists && !m_documents.empty())
        df0Id = activeDoc ? activeDoc->id : m_documents.front()->id;

    // 4. Set the model BEFORE loading the game — the snapshot is only valid for a matching
    //    memory geometry.
    if (!session.modelId.empty()) {
        for (const auto& m : adm::GetAllAmigaModels()) {
            if (session.modelId == m.id) {
                m_emulatorView->SetModel(m.model);
                break;
            }
        }
    }

    // 5. Boot the DF0 document, then restore the machine snapshot on top of it.
    DiskDocument* df0Doc = nullptr;
    for (auto& d : m_documents) {
        if (d->id == df0Id && d->kind == core::DocumentKind::DiskImage) {
            df0Doc = d.get();
            break;
        }
    }
    // A slot that was backed by live hardware must not be silently re-mounted with an
    // image, and must not be re-attached either: the disk that was in the drive may be
    // gone, and spinning a physical drive up on session load is not something to do
    // unasked. Restore the configuration into the window, leave the slot empty, and say
    // so.
    bool df0WasBridged = false;
    if (!session.bridgedSlots.empty()) {
        std::string slots;
        for (const auto& slot : session.bridgedSlots) {
            if (slot.drive == 0)
                df0WasBridged = true;
            if (!slots.empty())
                slots += ", ";
            slots += fmt::format("DF{}", slot.drive);
        }
        // Every recorded slot gets its configuration back in the window, so re-attaching
        // is one click per drive; the first is brought to the front.
        for (const auto& slot : session.bridgedSlots) {
            ui::FloppyDrivesView::Config fb = m_floppyDrivesView->GetSlotConfig(slot.drive);
            fb.hardwareType = slot.hardwareType;
            fb.comPort = slot.comPort;
            fb.bridgeMode = slot.bridgeMode;
            fb.bridgeDensity = slot.bridgeDensity;
            fb.driveCable = slot.driveCable;
            fb.autoCache = slot.autoCache;
            fb.smartSpeed = slot.smartSpeed;
            fb.allowWrites = slot.allowWrites;
            fb.driveSlot = slot.drive;
            m_floppyDrivesView->SetSlotConfig(slot.drive, fb);
        }
        const auto& first = session.bridgedSlots.front();
        m_floppyDrivesView->SetSelectedSlot(first.drive);

        const std::string what =
            first.hardwareName.empty() ? std::string("live drive") : first.hardwareName;
        const std::string note = fmt::format(
            "{} was backed by a {} on {} in this session. Live hardware is never restored "
            "automatically - reconnect the drive and attach it from Disk > Floppy "
            "Drives.",
            slots, what, first.comPort.empty() ? std::string("its serial port") : first.comPort);
        Logger::Info("Session restore: {}", note);
        m_sessionNotes = m_sessionNotes.empty() ? note : m_sessionNotes + "\n" + note;
    }

    if (df0Doc && !df0WasBridged)
        MountDocumentToDrive(0, df0Id);

    // The session's disc, before the machine boots so it is in the drive at power-on.
    m_emulatorView->EjectCd();
    m_emulatorView->RestoreCdImage(session.cdImage);

    bool booted = false;
    if (hasRestoredMounts) {
        core::EmulatorLaunchProfile profile;
        profile.modelId = session.modelId;
        profile.controllerRoms = session.hardDriveControllerRoms;
        profile.cdImage = m_emulatorView->GetEmulator()->GetConfiguredCdImage();
        for (size_t index = 0; index < session.hostMounts.size(); ++index) {
            profile.mounts[index].device = static_cast<core::HostMountDevice>(index);
            if (!session.hostMounts[index])
                continue;
            profile.mounts[index] = session.hostMounts[index]->descriptor;
            profile.mounts[index].source = core::HostMountSource::RestoredSession;
            std::error_code ec;
            if (profile.mounts[index].IsHardfile()) {
                if (!std::filesystem::is_regular_file(profile.mounts[index].hardfilePath, ec)) {
                    Logger::Warn("Session restore: hardfile '{}' is missing; {} left empty",
                                 profile.mounts[index].hardfilePath.string(),
                                 core::HostMountDeviceName(profile.mounts[index].device));
                    profile.mounts[index].enabled = false;
                } else if (!profile.mounts[index].readOnly &&
                           !core::RestoredHardfileMayWrite(
                               profile.mounts[index],
                               m_userPrefs.Get().hostDirectoryMounts[index])) {
                    profile.mounts[index].readOnly = true;
                    readOnlyHardfiles.push_back(fmt::format(
                        "{} ({})", core::HostMountDeviceName(profile.mounts[index].device),
                        profile.mounts[index].hardfilePath.string()));
                }
            } else if (!std::filesystem::is_directory(profile.mounts[index].hostDirectory, ec)) {
                profile.mounts[index].enabled = false;
            } else if (session.hostMounts[index]->documentId == 0) {
                core::HostDirectoryDocument fingerprintSource;
                std::string fingerprintError;
                if (fingerprintSource.Open(profile.mounts[index].hostDirectory, true,
                                           fingerprintError) &&
                    fingerprintSource.GetScan().fingerprint !=
                        session.hostMounts[index]->directoryFingerprint) {
                    changedMountDirectories.push_back(profile.mounts[index].hostDirectory.string());
                    Logger::Warn("Session restore: mounted directory '{}' changed since snapshot",
                                 profile.mounts[index].hostDirectory.string());
                }
            }
        }
        std::string error;
        auto* emulator = m_emulatorView->GetEmulator();
        if (!m_emulatorView->IsCoreLoaded())
            emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath());
        booted = emulator->StartHostMountProfile(profile, error);
        if (booted)
            m_emulatorView->FlushPendingDisks();
        else if (!error.empty())
            Logger::Error("Session restore: {}", error);
    } else {
        booted = m_emulatorView->PowerOn();
    }
    if (!booted) {
        Logger::Error("Session restore: emulator failed to power on");
        m_errorMessage = "Restored the session, but the emulator failed to start.";
        m_showErrorModal = true;
    } else {
        m_emulatorView->Open();

        bool snapshotRestored = false;
        if (session.snapshotPresent && !session.snapshot.empty()) {
            // PUAE builds the UAE machine lazily: retro_load_game does not run the CPU, and
            // the debugger / save-state machinery only come up once a frame has executed.
            // Run a couple of warm-up frames so retro_unserialize (and the breakpoint debug
            // commands below) operate on a fully constructed machine.
            if (auto* emu = m_emulatorView->GetEmulator()) {
                for (int i = 0; i < 2; ++i)
                    emu->StepOneFrame();
            }

            snapshotRestored = m_emulatorView->LoadState(session.snapshot);
            // The session snapshot is now the machine. Nothing from a previous run's
            // playback may be handed back over the top of it.
            m_timeline.ClearMachineHoldsReplayedFrame();
            if (snapshotRestored && session.frameHistoryPresent && !session.frameHistory.empty()) {
                // Sized against this machine's save state, so a timeline recorded on a
                // different model is refused rather than resumed as a lie.
                const size_t expected = session.snapshot.size();
                if (m_timeline.History().Deserialize(session.frameHistory.data(),
                                                     session.frameHistory.size(), expected)) {
                    uint64_t oldest = 0, newest = 0;
                    m_timeline.History().Range(oldest, newest);
                    m_timeline.SetPosition(newest);
                    // Thumbnails are not part of the saved timeline (they are a scrub
                    // convenience, not state), so anything held is from another run and
                    // would caption the restored frames with the wrong pictures.
                    m_timeline.Thumbnails().Clear();
                    // The restored snapshot *is* the timeline's newest frame, but the
                    // host-side frame counter restarted at the fresh boot. Left alone, the
                    // first frame recorded after this would look like a jump backwards and
                    // trim the entire timeline that was just recovered.
                    if (auto* e = m_emulatorView->GetEmulator())
                        e->SetTotalFrameCount(newest);
                    Logger::Info("Session restore: recovered frame history {}-{}", oldest, newest);
                } else {
                    Logger::Warn("Session restore: the saved frame history does not match this "
                                 "machine and was dropped");
                }
            }
            if (!snapshotRestored) {
                Logger::Warn("Session restore: snapshot could not be applied; booting fresh");
                m_errorMessage =
                    "The machine snapshot could not be restored (it may be from a different "
                    "build or model). The disk was booted fresh instead.";
                m_showErrorModal = true;
            }
        }

        // 6. Freeze the machine at the restored state before touching the debugger, so the
        //    snapshot's PC/registers are exactly what the user sees. A failed restore keeps
        //    running so the fresh boot reaches a usable state.
        if (snapshotRestored && session.wasPaused)
            m_emulatorView->SetPausedState(true);

        // 7. Re-inject breakpoints. The core has now executed frames (during warm-up and
        //    inside retro_unserialize), so the debugger accepts "f"/"w" commands.
        if (auto* bpv = m_debuggerView->GetBreakpointsView())
            bpv->ImportEntries(session.breakpoints);
    }

    // 8. Restore view / navigation state.
    const auto& vs = session.viewState;
    if (vs.is_object()) {
        if (vs.contains("imguiLayout") && vs["imguiLayout"].is_string())
            m_workspaceLayout.SetPendingLayout(vs["imguiLayout"].get<std::string>());
        if (vs.contains("disasmAddress")) {
            if (auto* dis = m_debuggerView->GetDisassemblyView())
                dis->GoToAddress(vs.value("disasmAddress", 0u));
        }
        if (vs.contains("memoryAddress")) {
            if (auto* mem = m_debuggerView->GetMemoryView())
                // focus=false: restore the saved position without yanking window focus to the
                // Memory view, so the layout's active window (imguiLayout) stays focused.
                mem->GoToAddress(vs.value("memoryAddress", 0u), /*focus=*/false);
        }
    }

    // 9. Notes.
    m_sessionNotes = session.notes;
    if (!readOnlyHardfiles.empty()) {
        std::string note = "Restored read-only - not your own read-write mount of the file:";
        for (const auto& h : readOnlyHardfiles)
            note += " " + h + ";";
        note.back() = '.';
        Logger::Info("Session restore: {}", note);
        m_sessionNotes = m_sessionNotes.empty() ? note : m_sessionNotes + "\n" + note;
    }

    if (!changedMountDirectories.empty() && !m_showErrorModal) {
        m_errorMessage = "One or more mounted directories changed since this session was saved:";
        for (const auto& path : changedMountDirectories)
            m_errorMessage += "\n\n" + path;
        m_showErrorModal = true;
    }

    Logger::Info("Session restored: {} document(s), snapshot={}", m_documents.size(),
                 session.snapshotPresent ? "yes" : "no");
    return true;
}

bool Application::WriteSessionTo(const std::string& path)
{
    core::PeekSession session = BuildPeekSession();
    std::string err;
    if (!core::SessionSerializer::Write(path, session, err)) {
        Logger::Error("Failed to save session: {}", err);
        m_errorMessage = "Failed to save session:\n" + err;
        m_showErrorModal = true;
        return false;
    }
    m_currentSessionPath = path;
    m_userPrefs.AddRecentFile(path);
    Logger::Info("Session saved to {}", path);
    return true;
}

void Application::SaveSession()
{
    if (m_documents.empty()) {
        m_errorMessage = "Nothing to save — open a disk first.";
        m_showErrorModal = true;
        return;
    }

    // Quick-save to the session's existing file; fall back to a prompt the first time.
    if (m_currentSessionPath.empty()) {
        SaveSessionAs();
        return;
    }
    WriteSessionTo(m_currentSessionPath);
}

void Application::SaveSessionAs()
{
    if (m_documents.empty()) {
        m_errorMessage = "Nothing to save — open a disk first.";
        m_showErrorModal = true;
        return;
    }

    // Pre-populate the dialog with the current session's name, or derive one from the
    // active document so the suggested filename is meaningful.
    std::string defaultName = "session.peek";
    if (!m_currentSessionPath.empty()) {
        defaultName = std::filesystem::path(m_currentSessionPath).filename().string();
    } else if (m_activeDocument && !m_activeDocument->currentFilePath.empty()) {
        defaultName =
            std::filesystem::path(m_activeDocument->currentFilePath).stem().string() + ".peek";
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filterItem[1] = {{"PEEKaboo Session", "peek"}};
    nfdresult_t result = NFD_SaveDialog(&outPath, filterItem, 1, nullptr, defaultName.c_str());

    // NFD_SaveDialog blocks and can miss key-up events.
    ImGui::GetIO().ClearInputKeys();

    if (result != NFD_OKAY) {
        if (result == NFD_ERROR)
            Logger::Error("NFD Error: {}", NFD_GetError());
        return;
    }

    std::string path = outPath;
    NFD_FreePath(outPath);
    // Ensure a .peek extension.
    if (std::filesystem::path(path).extension() != ".peek")
        path += ".peek";

    WriteSessionTo(path);
}

void Application::OpenSession()
{
    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filterItem[1] = {{"PEEKaboo Session", "peek"}};
    nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 1, nullptr);

    ImGui::GetIO().ClearInputKeys();

    if (result != NFD_OKAY) {
        if (result == NFD_ERROR)
            Logger::Error("NFD Error: {}", NFD_GetError());
        return;
    }

    std::string path = outPath;
    NFD_FreePath(outPath);

    OpenSessionFromPath(path);
}

void Application::OpenSessionFromPath(const std::string& path)
{
    core::PeekSession session;
    std::string err;
    if (!core::SessionSerializer::Read(path, session, err)) {
        Logger::Error("Failed to open session: {}", err);
        m_errorMessage = "Failed to open session:\n" + err;
        m_showErrorModal = true;
        return;
    }

    RestorePeekSession(session);
    m_currentSessionPath = path; // subsequent "Save Session" overwrites this file
    m_userPrefs.AddRecentFile(path);
}

void Application::TogglePauseEmulator()
{
    if (m_emulatorView)
        m_emulatorView->TogglePause();
}

void Application::SetupExocet()
{
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path here = fs::current_path(ec);
#ifdef _WIN32
        const fs::path py = here / "python" / "python.exe";
#else
        const fs::path py = here / "python" / "bin" / "python3";
#endif
        if (fs::is_regular_file(py, ec))
            m_exocetBundledPython = py.string();
        if (fs::is_directory(here / "exocet-tools", ec))
            m_exocetBundledTools = (here / "exocet-tools").string();
    }
    core::ExocetSession::Hooks hooks;
    hooks.block = [this]() -> const core::ExocetBlockState* {
        auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
        return emu && m_emulatorView->IsRunning() ? &emu->GetExocetBlock() : nullptr;
    };
    hooks.memory = [this]() -> const core::ExocetMemoryState* {
        auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
        return emu && m_emulatorView->IsRunning() ? &emu->GetExocetMemory() : nullptr;
    };
    hooks.paused = [this]() { return m_emulatorView && m_emulatorView->IsPaused(); };
    // RAM only - chip, slow, fast: the arena and the block are never anywhere else
    hooks.writeRam = [this](uint32_t addr, const uint8_t* data, size_t size) {
        auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
        if (!emu)
            return false;
        core::ArMemoryAccess mem;
        std::vector<core::ArMemoryAccess::Region> regions;
        for (const auto& r : emu->GetMemoryRegions())
            if (r.hostPtr && r.writable)
                regions.push_back({r.hostPtr, r.amigaAddr, r.size, r.name, r.writable});
        mem.SetRegions(std::move(regions));
        std::string error;
        return mem.Write(addr, data, size, error);
    };
    hooks.releaseTimeline = [this]() { ReleaseTimelineMachine(); };
    hooks.clearTimeline = [this]() { ClearFrameTimeline(); };
    hooks.stateChanged = [this]() {
        if (m_debuggerView)
            m_debuggerView->MarkMemoryRegionsDirty();
    };
    // Write ADF over the pack's disk: ejected first (the running machine holds it), the old one
    // kept beside it, the document reloaded so what is open is what is on the disk.
    hooks.replaceDisk = [this](const std::string& path, const std::vector<uint8_t>& disk,
                               std::string& error) {
        namespace fs = std::filesystem;
        const fs::path target = fs::u8path(path);
        if (m_emulatorView && m_emulatorView->IsRunning())
            m_emulatorView->Stop();
        std::error_code ec;
        if (fs::exists(target, ec)) {
            fs::copy_file(target, fs::path(target.string() + ".bak"),
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error = "could not keep the old disk as .bak: " + ec.message();
                return false;
            }
        }
        const fs::path tmp = fs::path(target.string() + ".tmp");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(disk.data()),
                      static_cast<std::streamsize>(disk.size()));
            if (!out) {
                error = "could not write " + tmp.string();
                return false;
            }
        }
        fs::rename(tmp, target, ec);
        if (ec) {
            error = "could not replace " + path + ": " + ec.message();
            return false;
        }
        LoadDocumentExternal(path, /*force*/ true, /*reloadIfOpen*/ true);
        return true;
    };
    hooks.bootDisk = [this]() { RunAdf(); };
    hooks.listen = [this](const std::vector<uint8_t>& bytes, const std::string& title) {
        m_exocetListen.reset(); // the last one, and what it was playing
        auto module = std::make_unique<core::AudioModule>();
        if (!module->Load(bytes, "audition.mod")) {
            Logger::Error("Exocet: the player could not load {}", title);
            return;
        }
        core::AudioModule* playing = module.get();
        auto player = std::make_unique<ui::AudioPlayer>(&m_audioEngine);
        player->SetTitle("Exocet - " + title + "###ExocetListen");
        player->SetModule(std::move(module));
        player->Open();
        m_audioEngine.Play(playing);
        m_exocetListen = std::move(player);
    };
    hooks.paula = [this]() {
        return m_debuggerView ? m_debuggerView->BuildPaulaSnapshot() : core::PaulaStateSnapshot{};
    };
    hooks.breakAt = [this](int framesAhead) {
        // a frame replayed out of the history is not the demo reaching the row
        auto* emu = m_emulatorView->GetEmulator();
        if (!emu || m_timeline.IsReplayInProgress())
            return;
        if (framesAhead <= 0)
            emu->RequestHostBreak();
        else
            emu->SetFrameBreakTarget(emu->GetTotalFrameCount() + framesAhead);
    };
    m_exocet->SetHooks(std::move(hooks));
    m_exocet->SetTrustStore(
        [this](const std::string& hash) { return m_userPrefs.IsExocetKitTrusted(hash); },
        [this](const std::string& hash) { m_userPrefs.TrustExocetKit(hash); });
    m_exocetView->m_onTogglePause = [this]() { TogglePauseEmulator(); };
    // the oldest frame held is a seek's base, never a destination: the first seekable one on
    m_exocetView->m_recordedRange = [this](uint64_t& first, uint64_t& last) {
        const auto& history = m_timeline.History();
        uint64_t oldest = 0;
        return history.Range(oldest, last) && history.FirstSeekable(first);
    };
    m_exocetView->m_onSeekFrame = [this](uint64_t frame) {
        const std::string err = SeekToRecordedFrame(frame);
        if (!err.empty())
            Logger::Info("Frame seek: {}", err);
    };
    m_exocetView->m_canPickScreenColour = [this]() { return m_emulatorView->IsRunning(); };
    m_exocetView->m_pickScreenColour = [this](std::function<void(bool, uint32_t)> done) {
        m_emulatorView->BeginPixelPick(std::move(done));
    };
    m_exocetControlView->m_onOpenMemory = [this]() { m_exocetMemoryView->Open(); };
    m_exocetControlView->m_onOpenMusic = [this]() { m_exocetMusicView->Open(); };
    m_exocetControlView->m_onShowRow = [this](int row) { m_exocetView->ShowRow(row); };
    m_exocetControlView->m_recordedRange = m_exocetView->m_recordedRange;
    m_exocetControlView->m_onSeekFrame = m_exocetView->m_onSeekFrame;
    m_exocetControlView->m_getFrameThumbnail = [this](uint64_t frame, std::vector<uint8_t>& rgb,
                                                      int& w, int& h, uint64_t& gotFrame) {
        return GetFrameThumbnail(frame, rgb, w, h, gotFrame);
    };
    m_exocetControlView->m_getTools = [this]() {
        const auto& p = m_userPrefs.Get();
        return std::make_pair(p.exocetPython, p.exocetTools);
    };
    m_exocetControlView->m_onToolsChanged = [this](const std::string& python,
                                                   const std::string& tools) {
        m_userPrefs.SetExocetTools(python, tools);
    };
}

void Application::UpdateExocet()
{
    // the preferences first, for development; else what ships beside the executable; else the
    // Python on PATH and no tools directory
    const auto& p = m_userPrefs.Get();
    const std::string python = !p.exocetPython.empty()          ? p.exocetPython
                               : !m_exocetBundledPython.empty() ? m_exocetBundledPython
                                                                : std::string("python");
    const std::string tools = !p.exocetTools.empty() ? p.exocetTools : m_exocetBundledTools;
    m_exocet->SetTools({python, tools});
    auto* doc = GetActiveDocument();
    m_exocet->SetPack(doc ? doc->exocet : nullptr, doc ? doc->exocetError : std::string(),
                      doc ? doc->currentFilePath : std::string());
    m_exocet->Update();
}

void Application::ToggleTurboEmulator()
{
    if (m_emulatorView)
        m_emulatorView->ToggleTurbo();
}

void Application::SetEmulatorMuted(bool mute)
{
    if (m_emulatorView)
        m_emulatorView->SetMuted(mute);
}

void Application::StepFrameEmulator()
{
    if (m_emulatorView) {
        if (m_emulatorView->IsRunning()) {
            if (!m_emulatorView->IsPaused()) {
                m_emulatorView->TogglePause();
            }
            m_emulatorView->StepFrame();
        }
    }
}

void Application::StepFrameBackEmulator()
{
    if (!m_emulatorView || !m_emulatorView->IsRunning())
        return;
    // Stepping backwards only means anything on a stopped machine, and the seek pauses
    // anyway - doing it here keeps the two step directions behaving the same way.
    if (!m_emulatorView->IsPaused())
        m_emulatorView->TogglePause();

    const std::string err = RewindFrames(1);
    if (!err.empty())
        Logger::Info("Step Frame Back: {}", err);
}

void Application::StepInstructionEmulator()
{
    if (m_emulatorView) {
        if (m_emulatorView->IsRunning()) {
            if (!m_emulatorView->IsPaused()) {
                m_emulatorView->TogglePause();
            }
            m_emulatorView->StepInstruction();
        }
    }
}

void Application::StopEmulator()
{
    if (m_emulatorView && m_emulatorView->IsRunning())
        m_emulatorView->Stop();
}

void Application::ResetEmulator()
{
    if (!m_emulatorView || !m_emulatorView->IsRunning())
        return;
    m_emulatorView->Reset();
    // The machine's memory map is rebuilt by the reset, so the debugger's cached regions are
    // stale - the toolbar's Reset button does the same.
    if (m_debuggerView)
        m_debuggerView->MarkMemoryRegionsDirty();
}

void Application::EjectEmulatorDisk()
{
    if (m_emulatorView && m_emulatorView->IsRunning())
        m_emulatorView->Eject();
}

void Application::StepOverEmulator()
{
    // Pauses first if the machine is running, exactly like StepInstructionEmulator - the menu
    // items do not require the user to pause by hand the way the toolbar buttons do.
    if (m_emulatorView && m_emulatorView->IsRunning()) {
        if (!m_emulatorView->IsPaused())
            m_emulatorView->TogglePause();
        m_emulatorView->StepOver();
    }
}

bool Application::IsEmulatorPaused() const
{
    if (m_emulatorView)
        return m_emulatorView->IsPaused();
    return false;
}

bool Application::IsEmulatorTurbo() const
{
    if (m_emulatorView)
        return m_emulatorView->IsTurbo();
    return false;
}

bool Application::IsEmulatorRunning() const
{
    if (m_emulatorView)
        return m_emulatorView->IsRunning();
    return false;
}

void Application::ShowAbout()
{
    if (m_emulatorView) {
        auto* emu = m_emulatorView->GetEmulator();
        std::string ver = emu ? emu->GetCoreVersion() : std::string{};
        if (ver.empty())
            ver = adm::AmigaEmulator::ProbeVersion(adm::AmigaEmulator::GetDefaultCorePath());
        m_aboutView->SetCoreVersion(ver);
    }
    m_aboutView->Open();
}

void Application::ShowPreferences() { m_preferencesView->Open(); }

void Application::ShowAppearancePreferences() { m_preferencesView->OpenAppearance(); }

void Application::ShowSystemFilesPreferences() { m_preferencesView->OpenSystemFiles(); }

void Application::ShowSetupWizard() { m_onboardingView->Open(); }

void Application::MaybeStartOnboarding()
{
    if (m_userPrefs.Get().onboardingDone)
        return;
    // The members' package and every setup from before the wizard already have their ROMs
    if (m_onboardingView->AnyMachineReady()) {
        Logger::Info("Setup wizard: not needed, the ROM folder has a machine's Kickstart");
        m_userPrefs.SetOnboardingDone(true);
        return;
    }
    Logger::Info("Setup wizard: first start without Kickstart ROMs");
    m_onboardingView->Open();
}

void Application::StepUiScale(int direction)
{
    auto fonts = m_userPrefs.Get().fonts;
    fonts.uiScale = UserPreferences::StepUiScale(fonts.uiScale, direction);
    m_userPrefs.SetFontPrefs(fonts);
}

void Application::ShowAdvancedFind()
{
    m_showAdvancedFind = true;
    m_advancedFindView->SetFocus();
}

void Application::MaybeDiscoverTrainer()
{
    auto* doc = m_activeDocument;
    if (!doc || doc->trainerLookupDone || !doc->identifier || !m_debuggerView)
        return;
    auto* trainerView = m_debuggerView->GetTrainerView();
    auto* locks = m_debuggerView->GetLocks();
    if (!trainerView || !locks)
        return;

    // The identifier hashes on a background thread; wait for its result rather than hashing the
    // media a second time here.
    const auto result = doc->identifier->GetResult();
    if (result.md5.empty())
        return;
    doc->trainerLookupDone = true;

    core::TrainerMatch match = core::TrainerMatch::None;
    const std::string path =
        core::TrainerFile::FindForMedia("trainers", result.md5, result.tosecTitleGroupId, match);
    if (path.empty())
        return;

    // Loading replaces the whole working set, so it must not run over locks the user set up by
    // hand. When there are any, the trainer is reported and left for them to open deliberately.
    if (!locks->Empty()) {
        Logger::Info("Trainer: {} matches this disk ({}), but memory locks are already set - "
                     "open it from the Trainer window to replace them.",
                     path, core::ToString(match));
        return;
    }

    // Nothing is ever enabled by loading: a file authored against another release has to prove
    // itself against this one first, and ticking the box is the user's call.
    std::string err;
    if (!trainerView->LoadTrainerFile(path, match, err)) {
        Logger::Warn("Trainer: found {} for this disk but could not load it: {}", path, err);
        return;
    }
    Logger::Info("Trainer: loaded {} ({} match) - {} entr{}, all disabled.", path,
                 core::ToString(match), locks->Size(), locks->Size() == 1 ? "y" : "ies");
    trainerView->Open();
}

void Application::SyncMemoryProtections()
{
    if (!m_debuggerView)
        return;
    auto* locks = m_debuggerView->GetLocks();
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!locks || !emu)
        return;

    const uint64_t rev = locks->Revision();
    if (rev == m_lastLockRevision)
        return;
    m_lastLockRevision = rev;

    // Translate the store's emulator-independent spec into the core's ABI struct. The two are
    // deliberately separate types so MemoryLocks stays unit-testable without an emulator.
    const auto specs = locks->BuildProtections();
    std::vector<adm::AmigaEmulator::ProtectEntry> entries;
    entries.reserve(specs.size());
    for (const auto& sp : specs) {
        adm::AmigaEmulator::ProtectEntry e;
        e.addr = sp.addr;
        e.len = sp.len;
        e.mode = sp.mode;
        e.size = sp.size;
        e.value = sp.value;
        entries.push_back(e);
    }
    emu->SetProtections(entries);
}

void Application::UpdateMemorySearcherRegions()
{
    if (m_emulatorView->IsRunning()) {
        // The search regions derive purely from the memory bank layout, which only changes when
        // banks are added/removed/resized — not on every emulated frame. Skip the rebuild (two
        // vector allocations + per-region string copies + a sort in SetRegions) while the
        // allocation-free layout signature is unchanged.
        const uint64_t sig = m_emulatorView->GetMemoryRegionsSignature();
        if (sig == m_memSearcherRegionSig)
            return;
        m_memSearcherRegionSig = sig;

        const auto regions = m_emulatorView->GetFreshMemoryRegions();
        std::vector<core::MemorySearcher::Region> searchRegions;
        searchRegions.reserve(regions.size());
        for (const auto& r : regions) {
            if (r.hostPtr && r.size > 0) {
                core::MemorySearcher::Region sr;
                sr.data = r.hostPtr;
                sr.baseAddress = r.amigaAddr;
                sr.size = r.size;
                sr.name = r.name;
                searchRegions.push_back(sr);
            }
        }
        m_memorySearcher->SetRegions(std::move(searchRegions));
    } else if (m_memSearcherRegionSig != 0) {
        // Clear once on the running -> stopped transition, not every frame.
        m_memSearcherRegionSig = 0;
        m_memorySearcher->SetRegions({});
    }
}

void Application::TriggerFind()
{
    if (m_searchContext == SearchContext::Disk) {
        m_hexEditorView->ShowFindDialog();
    } else if (m_searchContext == SearchContext::Memory) {
        m_debuggerView->ShowFindDialog();
    }
}

void Application::TriggerFindNext()
{
    if (m_searchContext == SearchContext::Memory) {
        UpdateMemorySearcherRegions();
        m_debuggerView->FindNext();
    } else {
        m_hexEditorView->FindNext();
    }
}

void Application::TriggerFindPrevious()
{
    if (m_searchContext == SearchContext::Memory) {
        UpdateMemorySearcherRegions();
        m_debuggerView->FindPrevious();
    } else {
        m_hexEditorView->FindPrevious();
    }
}

void Application::ShowLog() { m_logView->Open(); }

void Application::ShowTosecBrowser() { m_tosecBrowserView->Open(); }

void Application::ShowFloppyDrives() { m_floppyDrivesView->Open(); }

bool Application::AttachLiveFloppy(const ui::FloppyDrivesView::Config& cfg)
{
    if (!m_emulatorView)
        return false;
    // EmulatorView owns both halves - the emulator call and this view's drive model -
    // so the console LIVE command and this window cannot leave them disagreeing.
    if (!m_emulatorView->AttachBridgeDrive(cfg.driveSlot, cfg.hardwareType, cfg.comPort,
                                           cfg.bridgeMode, cfg.bridgeDensity, cfg.driveCable,
                                           cfg.autoCache, cfg.smartSpeed, cfg.allowWrites))
        return false;

    Logger::Info("Live floppy: DF{} -> {} on {}, mode {}, cable {}, read-ahead {}, smart speed {}, "
                 "writes {} (queued; the device opens on the next emulated vertical blank)",
                 cfg.driveSlot, m_emulatorView->GetDriveState(cfg.driveSlot).bridgeHardware,
                 cfg.comPort, cfg.bridgeMode, cfg.driveCable, cfg.autoCache ? "on" : "off",
                 cfg.smartSpeed ? "on" : "off", cfg.allowWrites ? "ALLOWED" : "protected");
    return true;
}

void Application::DetachLiveFloppy(int driveSlot)
{
    if (!m_emulatorView)
        return;
    m_emulatorView->DetachBridgeDrive(driveSlot);
    Logger::Info("Live floppy: DF{} detached", driveSlot);
}

ui::FloppyDrivesView::Status Application::GetLiveFloppyStatus(int driveSlot) const
{
    ui::FloppyDrivesView::Status out;
    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu || driveSlot < 0 || driveSlot > 3)
        return out;

    out.supported = emu->HasBridgeSupport();

    // The host's assignment comes first: it exists whether or not a core is loaded,
    // and it is what Detach undoes. Everything below it is the core's view of the
    // device, which is empty while the machine is stopped.
    const auto& ds = m_emulatorView->GetDriveState(driveSlot);
    out.enabled = ds.enabled;
    out.imageMounted = !ds.isBridged && ds.hasDisk;
    out.imageName = out.imageMounted ? ds.diskName : std::string();
    out.assigned = ds.isBridged;
    out.pending = m_emulatorView->IsBridgePending(driveSlot);
    out.driverName = ds.bridgeHardware;
    out.comPort = ds.bridgeComPort;

    AmigaEmulator::BridgeStatus st;
    if (out.pending) {
        // Nothing to ask the core; the slot is file-backed as far as it knows.
        out.cylinder = emu->GetCurrentTrack(driveSlot);
    } else if (emu->GetBridgeStatus(static_cast<unsigned>(driveSlot), st)) {
        out.configured = st.configured != 0;
        out.opened = st.opened != 0;
        out.diskInDrive = st.diskInDrive != 0;
        out.writeProtected = st.writeProtected != 0;
        out.cylinder = st.cylinder;
        // The core names the driver only once it has opened; before that (and after a
        // failed open) the host's record is the better label.
        if (st.driverName[0])
            out.driverName = st.driverName;
        out.lastError = st.lastError;
    } else {
        // No status export (an older core): fall back to what the host asked for, so the
        // window still shows which slot it configured rather than looking broken.
        out.configured = emu->IsBridgeDrive(static_cast<unsigned>(driveSlot));
        out.opened = out.configured;
        out.cylinder = emu->GetCurrentTrack(driveSlot);
    }

    // An image slot's protection is the host's own flag. The core reports an empty
    // drive as write-protected too, so its read-back is used to self-heal the setting
    // (AmigaEmulator::PollDiskState) rather than to display it.
    if (!ds.isBridged)
        out.writeProtected = ds.writeProtected;

    // Motor and write activity come from gui_data, which is filetype-agnostic.
    out.motor = emu->IsDriveActive(driveSlot);
    out.writing = emu->IsDriveWriting(driveSlot);
    if (!out.configured)
        out.cylinder = emu->GetCurrentTrack(driveSlot);
    return out;
}

void Application::FocusWindow()
{
    if (!m_window)
        return;
    // glfwRestoreWindow also changes a maximized window back to its normal size. Only call it when
    // the application is actually minimized; the focus logic below handles foreground activation.
    if (glfwGetWindowAttrib(m_window, GLFW_ICONIFIED))
        glfwRestoreWindow(m_window);
#ifdef _WIN32
    // Windows blocks SetForegroundWindow for background apps (just flashes taskbar).
    // Temporarily attach to the foreground thread to gain permission.
    HWND hwnd = glfwGetWin32Window(m_window);
    DWORD foreThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD appThread = GetCurrentThreadId();
    if (foreThread != appThread) {
        AttachThreadInput(foreThread, appThread, TRUE);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(foreThread, appThread, FALSE);
    } else {
        SetForegroundWindow(hwnd);
    }
#else
    glfwFocusWindow(m_window);
#endif
}

void Application::OnPeekabooLoadRequested(const core::PeekabooLoadEvent& ev)
{
    Logger::Info("Peekaboo: received load event — releaseId={} url={}", ev.releaseId, ev.fileUrl);

    core::DemosceneSource src;
    src.releaseId = ev.releaseId;
    src.releaseName = ev.releaseName;
    src.groupName = ev.groupName;
    src.source = ev.source;

    core::DownloadRequest req;
    req.url = ev.fileUrl;
    req.filename = ev.filename;
    req.fileType = ev.fileType;

    StartDownloadDocument(req, src);
}

void Application::OnPeekabooRunRequested(const PeekabooRunRequest& request)
{
    Logger::Info("Peekaboo: launching compiled executable — {}", request.program);

    const std::string& launchMode = m_userPrefs.Get().peekabooLaunchMode;
    bool launched = false;
    if (launchMode != "bootable_adf") {
        std::string fastLaunchError;
        launched = m_emulatorView->RunHostExecutable(request.program, fastLaunchError);
        if (!launched) {
            if (launchMode == "host_directory") {
                Logger::Error("Peekaboo: host-directory launch failed — {}", fastLaunchError);
                m_errorMessage = "Fast compile-and-run failed:\n" + fastLaunchError;
                m_showErrorModal = true;
                return;
            }
            Logger::Warn("Peekaboo: host-directory launch failed; using bootable ADF — {}",
                         fastLaunchError);
        }
    }

    if (!launched) {
        Logger::Info("Peekaboo: launching compiled executable through bootable-ADF compatibility "
                     "mode");
        std::string adfError;
        if (!m_compiledProgram.RunViaBootableAdf(request.program, adfError)) {
            Logger::Error("Peekaboo: compatibility launch failed — {}", adfError);
            m_errorMessage = "Compile-and-run failed:\n" + adfError;
            m_showErrorModal = true;
            return;
        }
    }
    m_compiledProgram.LoadDebugInfo(request.program);
    // From here until its seglist appears, /api/status answers programLoaded:false -- which is
    // what stops the VS Code integration attaching gdb (and so halting the machine) mid-boot.
    m_compiledProgram.BeginWaitForLoad(request.generation);
    if (request.stopOnEntry) {
        m_debugSession.ArmStopOnEntry(request.program);
    }
    if (request.focus)
        FocusWindow();
}

std::vector<ui::WindowState> Application::GetWindowStates()
{
    // {name, openPtr, separatorAfter, groupName, icon, imguiId, isWindow}
    // `imguiId` is the Begin() label's "###" tail and must be kept in step with the Begin() call in
    // the owning view - it is what the MCP UI tools hand to ImGui::FindWindowByName. Leave it empty
    // only when the label carries no "###" and the menu name is already the window's identity.
    std::vector<ui::WindowState> states = {
        {"Amiga Emulator", m_emulatorView->GetOpenStatePtr(), true, "", ICON_FA_GAMEPAD,
         "Amiga Emulator"},
        // The "Disk" group is the top-level Disk menu, which lists these itself in a deliberate
        // order - View > Windows skips the group, so this is not their menu home. Kept together
        // and in menu order here so the registry reads the way the menu does.
        {"Block", m_hexEditorView->GetOpenStatePtr(), false, "Disk", ICON_FA_TABLE_CELLS, "Block"},
        {"Block Details", m_hexEditorView->GetBlockDetailsOpenStatePtr(), false, "Disk",
         ICON_FA_TABLE_LIST, "Block Details"},
        {"Block Type Map", m_blockMapView->GetOpenStatePtr(), false, "Disk", ICON_FA_BORDER_ALL,
         "Block Type Map"},
        {"Disk Bitmap", m_bitmapView->GetOpenStatePtr(), false, "Disk", ICON_FA_TABLE_CELLS_LARGE,
         "Disk Bitmap"},
        // Begins as "Change Map", not as its menu name.
        {"Block Change Map", m_changeMapView->GetOpenStatePtr(), false, "Disk",
         ICON_FA_CODE_COMPARE, "Change Map"},
        {"NDOS Map", m_ndosView->GetOpenStatePtr(), false, "Disk", ICON_FA_CIRCLE_QUESTION,
         "NDOS Map"},
        {"Validation", m_validationView->GetOpenStatePtr(), false, "Disk", ICON_FA_CHECK_DOUBLE,
         "Disk Validation Results"},
        {"Identify Disk Results", m_identifyView->GetOpenStatePtr(), false, "Disk",
         ICON_FA_FINGERPRINT, "Identify Disk"},
        {"Boot Block Viewer", m_bootBlockView->GetOpenStatePtr(), false, "Disk", ICON_FA_POWER_OFF,
         "Boot Block Viewer"},
        // Menu-visible name changed with the Read/Write merge; imguiId is kept as "Write to Disk"
        // so saved dock layouts from before the merge still find this window.
        {"Floppy Disk I/O", m_floppyDiskView->GetOpenStatePtr(), false, "Disk", ICON_FA_FLOPPY_DISK,
         "Write to Disk"},
        {"Floppy Drives", m_floppyDrivesView->GetOpenStatePtr(), false, "Disk", ICON_FA_FLOPPY_DISK,
         "FloppyDrives"},
        {"Hard Drives", m_emulatorView->GetHardDriveEditorOpenPtr(), false, "Disk",
         ICON_FA_HARD_DRIVE, "Hard Drives"},
        {"CD Drive", m_emulatorView->GetCdDriveWindowOpenPtr(), false, "Disk", ICON_FA_COMPACT_DISC,
         "CD Drive"},
        {"TOSEC Browser", m_tosecBrowserView->GetOpenStatePtr(), false, "Disk", ICON_FA_DATABASE,
         "TOSEC Browser"},
        // The perf overlay and graph are drawn into the emulator window's draw list, so they own an
        // open flag but no window of their own - isWindow = false.
        {"Perf Overlay", m_emulatorView->GetShowPerfOverlayPtr(), false, "Performance", "", "",
         false},
        {"Perf Graph", m_emulatorView->GetShowPerfGraphPtr(), false, "Performance", "", "", false},
        // Begins without a "###", so its identity is the whole label, icon included.
        {"Display Filters", m_emulatorView->GetFiltersWindowOpenPtr(), false, "Filters", "",
         ICON_FA_EYE " Display Filters"},
        // Renamed from "GIF Capture" when it took on AVI recording. The id stayed "GIF Capture",
        // which is what old workspaces and MCP callers know it by - see WorkspaceLayout::Load.
        {"GIF & Video Capture", m_emulatorView->GetGifWindowOpenPtr(), false, "Capture", "",
         "GIF Capture"},
        {"CPU", m_debuggerView->GetCpuOpenPtr(), false, "Debugger", ICON_FA_MICROCHIP,
         "CPU##debugger"},
        {"Custom Registers", m_debuggerView->GetCustomOpenPtr(), false, "Debugger", ICON_FA_SLIDERS,
         "Custom Registers##debugger"},
        {"Disassembly", m_debuggerView->GetDisassemblyOpenPtr(), false, "Debugger", ICON_FA_CODE,
         "Disassembly##debugger"},
        {"Profile", m_debuggerView->GetProfileOpenPtr(), false, "Debugger", ICON_FA_CHART_COLUMN,
         "Profile##debugger"},
        {"Copper", m_debuggerView->GetCopperOpenPtr(), false, "Debugger", ICON_FA_LIST_OL,
         "Copper##debugger"},
        {"Memory", m_debuggerView->GetMemoryOpenPtr(), false, "Debugger", ICON_FA_MEMORY,
         "Memory##debugger"},
        {"Display", m_debuggerView->GetDisplayOpenPtr(), false, "Debugger", ICON_FA_DESKTOP,
         "Display##debugger"},
        {"Sprites", m_debuggerView->GetSpritesOpenPtr(), false, "Debugger", ICON_FA_GHOST,
         "Sprites##debugger"},
        {"Bitplanes", m_debuggerView->GetBitplanesOpenPtr(), false, "Debugger", ICON_FA_LAYER_GROUP,
         "Bitplanes##debugger"},
        {"Bartman Bitmaps", m_debuggerView->GetBartmanOpenPtr(), false, "Debugger",
         ICON_FA_FILL_DRIP, "BartmanBitmaps##debugger"},
        {"CIA", m_debuggerView->GetCIAOpenPtr(), false, "Debugger", ICON_FA_CLOCK,
         "CIA Registers##debugger"},
        {"Audio", m_debuggerView->GetAudioOpenPtr(), false, "Debugger", ICON_FA_VOLUME_HIGH,
         "Audio##debugger"},
        {"Frame Scrubber", m_debuggerView->GetScrubberOpenPtr(), false, "Debugger", ICON_FA_FILM,
         "Frame Scrubber##debugger"},
        {"Call Stack", m_debuggerView->GetCallStackOpenPtr(), false, "Debugger",
         ICON_FA_LAYER_GROUP, "Call Stack"},
        {"Debugger Console", m_debuggerView->GetConsoleOpenPtr(), false, "Debugger",
         ICON_FA_TERMINAL, "Debugger Console##debugger"},
        {"Breakpoints", m_debuggerView->GetBreakpointsOpenPtr(), false, "Debugger",
         ICON_FA_CIRCLE_DOT, "Breakpoints##debugger"},
        {"Annotations", m_debuggerView->GetAnnotationsOpenPtr(), false, "Debugger", ICON_FA_TAG,
         "Annotations##memory"},
        {"Trainer", m_debuggerView->GetTrainerOpenPtr(), false, "Debugger", ICON_FA_LOCK,
         "Trainer##debugger"},
        {"Memory Map", m_memoryMapView->GetOpenStatePtr(), false, "Debugger", ICON_FA_MAP,
         "Memory Map"},
        {"Blitter Operations", m_blitterView->GetOpenStatePtr(), false, "Debugger",
         ICON_FA_FILL_DRIP, "Blitter"},
        {"Register Operations", m_registerOpsView->GetOpenStatePtr(), false, "Debugger",
         ICON_FA_MICROCHIP, "RegisterOps"},
        {"Disk I/O", m_diskIoView->GetOpenStatePtr(), false, "Debugger", ICON_FA_FLOPPY_DISK,
         "DiskIo"},
        // "Exocet" is the grid's key (workspaces, the MCP UI tools); the menu calls it Exocet
        // Grid, and the Exocet window - the pack and the kit - is "Exocet Control"
        {"Exocet Control", m_exocetControlView->GetOpenStatePtr(), false, "Exocet",
         ICON_FA_SCREWDRIVER_WRENCH, "ExocetControl"},
        {"Exocet", m_exocetView->GetOpenStatePtr(), false, "Exocet", ICON_FA_TABLE_CELLS, "Exocet"},
        {"Exocet Memory", m_exocetMemoryView->GetOpenStatePtr(), false, "Exocet", ICON_FA_MEMORY,
         "ExocetMemory"},
        {"Exocet Music", m_exocetMusicView->GetOpenStatePtr(), false, "Exocet", ICON_FA_SLIDERS,
         "ExocetMusic"},
        {"Exocet Disk Map", m_exocetDiskMapView->GetOpenStatePtr(), false, "Exocet",
         ICON_FA_BOX_ARCHIVE, "ExocetDiskMap"},
        {"Blitter Minterm", m_blitterMintermView->GetOpenStatePtr(), false, "Debugger",
         ICON_FA_CALCULATOR, "Blitter Minterm"},
        {"DMA Stats", m_dmaStatsView->GetOpenStatePtr(), false, "Debugger", ICON_FA_CHART_COLUMN,
         "DMA Stats##dma_stats"},
        // Grouped so View > Windows skips it: its menu home is Search > Advanced Find..., which
        // already toggles this same flag and ticks when the window is open. The entry stays in
        // the registry because workspaces and the MCP UI tools address it by name.
        {"Advanced Find", &m_showAdvancedFind, false, "Search", ICON_FA_MAGNIFYING_GLASS,
         "Advanced Find"},
        {"Log", m_logView->GetOpenStatePtr(), false, "", ICON_FA_FILE_LINES, "Log"},
    };

    // Hidden, Exocet's windows are not windows at all: no menu entry, nothing a workspace or an
    // MCP UI tool can open
    if (!core::PrivateFeatures())
        std::erase_if(states, [](const auto& s) { return s.groupName == "Exocet"; });

    if (m_mcpActivityView)
        states.push_back({"MCP Activity", m_mcpActivityView->GetOpenStatePtr(), false, "Debugger",
                          ICON_FA_MICROCHIP, "McpActivity"});

    return states;
}

void Application::CreatePanel(const std::string& name) { m_panelManager.Create(name); }

void Application::DeletePanel(uint32_t id) { m_panelManager.Delete(id); }

void Application::RenamePanel(uint32_t id, const std::string& newName)
{
    m_panelManager.Rename(id, newName);
}

void Application::RequestNewPanel() { m_panelManager.RequestNew(); }

void Application::RequestRenamePanel(uint32_t id) { m_panelManager.RequestRename(id); }

void Application::SaveWorkspace(const std::string& name) { m_workspaceLayout.Save(name); }

void Application::LoadWorkspace(const std::string& name) { m_workspaceLayout.Load(name); }

void Application::ApplyPendingWorkspaceLayout() { m_workspaceLayout.ApplyPending(); }

namespace {

// The monitor holding the centre of @p window, or the primary one.
GLFWmonitor* MonitorOf(GLFWwindow* window)
{
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(window, &wx, &wy);
    glfwGetWindowSize(window, &ww, &wh);
    const int cx = wx + ww / 2;
    const int cy = wy + wh / 2;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh)
            return monitors[i];
    }
    return glfwGetPrimaryMonitor();
}

// Whether a window whose top-left corner is (@p x, @p y) has a usable strip of title bar on a
// connected monitor - the same test main() applies to the saved position at startup.
bool TitleBarOnAMonitor(int x, int y)
{
    if (x == -1 && y == -1)
        return false;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        if (x + 200 > mx && x < mx + mw && y >= my && y < my + mh)
            return true;
    }
    return false;
}

} // namespace

ScreenProfile Application::SuggestScreenProfile(GLFWwindow* window, float uiScale)
{
    GLFWmonitor* monitor = window ? MonitorOf(window) : glfwGetPrimaryMonitor();
    if (!monitor)
        return ScreenProfile::Hires;
    int mx = 0, my = 0, mw = 0, mh = 0;
    glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
    float divisor = std::max(uiScale, 0.1f);
#ifndef __APPLE__
    // Screen coordinates are pixels here, and the style is scaled up by the DPI factor (see
    // Initialize). On macOS they are points already.
    float xs = 1.0f, ys = 1.0f;
    glfwGetMonitorContentScale(monitor, &xs, &ys);
    if (xs > 1.0f)
        divisor *= xs;
#endif
    const float effective = static_cast<float>(mw) / divisor;
    const ScreenProfile suggested = ClassifyScreen(effective);
    Logger::Info("Screen profile: work area {}x{} is {:.0f} interface units wide - suggesting {}",
                 mw, mh, effective, ScreenProfileLabel(suggested));
    return suggested;
}

void Application::SetScreenProfile(ScreenProfile profile)
{
    auto& prefs = m_userPrefs.GetMutable();
    const ScreenProfile from = m_workspaceLayout.Profile();
    const bool chosen = !prefs.screenProfile.empty();
    if (chosen && from == profile)
        return;
    const bool fullscreen = m_window && glfwGetWindowMonitor(m_window) != nullptr;

    // While the app runs the views own their splitters and GLFW the window rectangle; the prefs
    // only hear of them at exit. Bring the prefs up to date first, so the outgoing profile stashes
    // what is on screen now.
    if (chosen) {
        if (m_window && !fullscreen) {
            glfwGetWindowPos(m_window, &prefs.windowX, &prefs.windowY);
            glfwGetWindowSize(m_window, &prefs.windowWidth, &prefs.windowHeight);
            prefs.maximized = glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) != 0;
        } else if (fullscreen) {
            prefs.windowX = m_windowX;
            prefs.windowY = m_windowY;
            prefs.windowWidth = m_windowWidth;
            prefs.windowHeight = m_windowHeight;
        }
        if (m_debuggerView) {
            prefs.spritesScanlinesPanelW = m_debuggerView->GetSpritesScanlinesPanelWidth();
            prefs.spritesDetailPanelH = m_debuggerView->GetSpritesDetailPanelHeight();
            prefs.memoryBytesPerLine = m_debuggerView->GetMemoryBytesPerLine();
        }
        if (m_memoryMapView) {
            prefs.memMapLeftColumnW = m_memoryMapView->GetLeftColumnWidth();
            prefs.memMapPreviewH = m_memoryMapView->GetPreviewHeight();
        }
    }

    bool tablesRestored = false;
    if (from != profile)
        tablesRestored = ui::GlobalTableLayout().SwitchFile(ui::TableLayoutStore::FileFor(profile));

    const std::optional<UserPreferences::FontPrefs> fontsBefore = m_appliedFonts;
    const bool restored = m_userPrefs.SwitchScreenProfile(ScreenProfileKey(profile));
    const auto& live = m_userPrefs.Get();

    if (restored) {
        if (m_debuggerView) {
            if (live.spritesScanlinesPanelW > 0.0f)
                m_debuggerView->SetSpritesScanlinesPanelWidth(live.spritesScanlinesPanelW);
            if (live.spritesDetailPanelH > 0.0f)
                m_debuggerView->SetSpritesDetailPanelHeight(live.spritesDetailPanelH);
            m_debuggerView->SetMemoryBytesPerLine(live.memoryBytesPerLine);
        }
        if (m_memoryMapView) {
            if (live.memMapLeftColumnW > 0.0f)
                m_memoryMapView->SetLeftColumnWidth(live.memMapLeftColumnW);
            if (live.memMapPreviewH > 0.0f)
                m_memoryMapView->SetPreviewHeight(live.memMapPreviewH);
        }
        for (auto& doc : m_documents)
            for (auto& viewer : doc->fileViewers)
                viewer->SetCurrentFont(live.fontName);
    }

    // The window rectangle is only moved on a switch the user makes from a running session: the
    // first-launch choice keeps the window the OS (or a package's defaults) just gave it.
    if (m_window && !fullscreen && chosen) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        if (GLFWmonitor* monitor = MonitorOf(m_window))
            glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
        if (restored && live.windowWidth > 0 && live.windowHeight > 0) {
            // Back to the rectangle this profile last had - unless that was on a monitor which is
            // not here now (one prefs file serves a 4K desk and a laptop over Remote Desktop), in
            // which case only its size is kept, clamped to this screen and centred on it.
            if (glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED))
                glfwRestoreWindow(m_window);
            int x = live.windowX, y = live.windowY;
            int w = live.windowWidth, h = live.windowHeight;
            if (!TitleBarOnAMonitor(x, y) && mw > 0) {
                w = std::min(w, mw);
                h = std::min(h, mh);
                x = mx + (mw - w) / 2;
                y = my + (mh - h) / 2;
            }
            glfwSetWindowSize(m_window, w, h);
            glfwSetWindowPos(m_window, x, y);
            if (live.maximized)
                glfwMaximizeWindow(m_window);
        } else if (!glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) && mw > 0) {
            // A profile's first use: keep the window, but bring it onto the screen if it is larger
            // than this one.
            int w = 0, h = 0;
            glfwGetWindowSize(m_window, &w, &h);
            if (w > mw || h > mh) {
                w = mw * 4 / 5;
                h = mh * 4 / 5;
                glfwSetWindowSize(m_window, w, h);
                glfwSetWindowPos(m_window, mx + (mw - w) / 2, my + (mh - h) / 2);
            }
        }
    }

    // A restored column store holds widths chosen at this profile's text sizes: the font change
    // the switch also brings must not auto-fit them away.
    m_skipFontAutofit = tablesRestored && fontsBefore && !(*fontsBefore == live.fonts);

    m_workspaceLayout.SetProfile(profile);
    m_workspaceLayout.ReloadTables();
    Logger::Info("Screen profile: {} (settings {})", ScreenProfileLabel(profile),
                 restored ? "restored" : "carried over");
    if (from != profile && m_onScreenProfileChanged)
        m_onScreenProfileChanged();
}

void Application::SaveSessionState()
{
    std::vector<core::UserPreferences::PanelDef> defs;
    for (const auto& p : m_panelManager.Panels())
        defs.push_back({p->GetName(), p->GetId()});
    m_userPrefs.SavePanels(defs);
}

void Application::RenderToolbar(float dpi_scale)
{
    // Style buttons as flat/minimalist
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * dpi_scale, 8 * dpi_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * dpi_scale, 8 * dpi_scale));

    ImVec2 btnSize(32 * dpi_scale, 32 * dpi_scale);

    ImGuiViewport* viewport = ImGui::GetMainViewport();

    float padding = 8.0f * dpi_scale;
    float iconSize = 32.0f * dpi_scale;
    float windowHeight = iconSize + (padding * 2.0f);

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, windowHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags window_flags = 0 | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("Toolbar", nullptr, window_flags);

    viewport->WorkPos.y += windowHeight;
    viewport->WorkSize.y -= windowHeight;

    bool hasFile = HasFileLoaded();
    bool canSaveImage =
        hasFile && m_activeDocument && m_activeDocument->kind == core::DocumentKind::DiskImage;
    bool hasUnsavedChanges = m_activeDocument && m_activeDocument->HasUnsavedChanges();

    // File Group
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "##Open", btnSize)) {
        nfdchar_t* outPath;
        if (NFD_OpenDialog(&outPath, nullptr, 0, nullptr) == NFD_OKAY) {
            LoadAdf(outPath);
            NFD_FreePath(outPath);
        }
    }
    ImGui::SetItemTooltip("Open Disk");

    ImGui::SameLine();
    ImGui::BeginDisabled(!hasUnsavedChanges);
    if (ImGui::Button(ICON_FA_FLOPPY_DISK "##Save", btnSize)) {
        SaveAdf();
    }
    ImGui::SetItemTooltip("Save Disk");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!canSaveImage);
    if (ImGui::Button(ICON_FA_FILE_EXPORT "##SaveAs", btnSize)) {
        SaveAdfAs();
    }
    ImGui::SetItemTooltip("Save As...");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    // Edit Group
    ImGui::BeginDisabled(!CanCreateNewFolder());
    if (ImGui::Button(ICON_FA_FOLDER_PLUS "##NewFolder", btnSize)) {
        TriggerNewFolder();
    }
    ImGui::SetItemTooltip("New Folder");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!CanDelete());
    if (ImGui::Button(ICON_FA_TRASH "##Delete", btnSize)) {
        TriggerDelete();
    }
    ImGui::SetItemTooltip("Delete");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!CanScrub());
    if (ImGui::Button(ICON_FA_ERASER "##Scrub", btnSize)) {
        TriggerScrub();
    }
    ImGui::SetItemTooltip("Scrub");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!CanRename());
    if (ImGui::Button(ICON_FA_PEN "##Rename", btnSize)) {
        TriggerRename();
    }
    ImGui::SetItemTooltip("Rename");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!CanSetPermissions());
    if (ImGui::Button(ICON_FA_LOCK "##Permissions", btnSize)) {
        TriggerPermissions();
    }
    ImGui::SetItemTooltip("Permissions");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!CanRestore());
    if (ImGui::Button(ICON_FA_ROTATE_LEFT "##Restore", btnSize)) {
        TriggerRestore();
    }
    ImGui::SetItemTooltip("Restore");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!CanExport());
    if (ImGui::Button(ICON_FA_FILE_EXPORT "##Export", btnSize)) {
        TriggerExport();
    }
    ImGui::SetItemTooltip("Export...");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    // Search Group — dispatches to Hex View (ADF) or Emulator Memory tab depending on focus
    bool canSearch = hasFile || m_emulatorView->IsRunning();
    ImGui::BeginDisabled(!canSearch);
    if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS "##Find", btnSize)) {
        TriggerFind();
    }
    ImGui::SetItemTooltip("Find");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CHEVRON_LEFT "##Prev", btnSize)) {
        TriggerFindPrevious();
    }
    ImGui::SetItemTooltip("Find Previous");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "##Next", btnSize)) {
        TriggerFindNext();
    }
    ImGui::SetItemTooltip("Find Next");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    // Emulator Group
    bool emuRunning = m_emulatorView->IsRunning();

    if (emuRunning && !m_emulatorView->IsPaused()) {
        if (ImGui::Button(ICON_FA_PAUSE "##PlayPause", btnSize)) {
            m_emulatorView->TogglePause();
        }
        ImGui::SetItemTooltip("Pause Emulator");
    } else {
        if (ImGui::Button(ICON_FA_PLAY "##PlayPause", btnSize)) {
            if (emuRunning) {
                // While the timeline is driving the machine, Play means "stop replaying and
                // go back to running live" - one obvious way out, rather than a button that
                // quietly means something else depending on state. TogglePause() would do
                // this through the before-runs hook anyway; saying it here keeps the button
                // honest about what it does.
                ReleaseTimelineMachine();
                m_emulatorView->TogglePause();
            } else {
                RunAdf();
            }
            ImGui::SetWindowFocus("Amiga Emulator");
        }
        ImGui::SetItemTooltip(emuRunning ? "Resume Emulator" : "Run in Emulator");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!emuRunning);
    if (ImGui::Button(ICON_FA_STOP "##Stop", btnSize)) {
        m_emulatorView->Stop();
    }
    ImGui::SetItemTooltip("Stop Emulator");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!emuRunning);
    if (ImGui::Button(ICON_FA_ARROW_ROTATE_RIGHT "##Reset", btnSize)) {
        m_emulatorView->Reset();
        m_debuggerView->MarkMemoryRegionsDirty();
    }
    ImGui::SetItemTooltip("Reset Emulator");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!emuRunning);
    if (ImGui::Button(ICON_FA_EJECT "##Eject", btnSize)) {
        m_emulatorView->Eject();
    }
    ImGui::SetItemTooltip("Eject Disk from Emulator");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);

    ImGui::SameLine();
    // Enabled only once the timeline actually holds something to go back to, so the button
    // does not promise a rewind the recorder cannot deliver.
    {
        uint64_t firstSeekable = 0;
        const bool canRewind = m_timeline.History().FirstSeekable(firstSeekable);
        ImGui::BeginDisabled(!emuRunning || !canRewind);
        if (ImGui::Button(ICON_FA_BACKWARD_STEP "##StepFrameBack", btnSize))
            StepFrameBackEmulator();
        ImGui::SetItemTooltip(canRewind ? "Step Frame Back (Shift+F6)"
                                        : "Step Frame Back - open a debugger window to record");
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!emuRunning || !m_emulatorView->IsPaused());
    if (ImGui::Button(ICON_FA_FORWARD_STEP "##StepFrame", btnSize)) {
        m_emulatorView->StepFrame();
    }
    ImGui::SetItemTooltip("Step Frame");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!emuRunning || !m_emulatorView->IsPaused());
    if (ImGui::Button(ICON_FA_ANGLE_RIGHT "##StepInstruction", btnSize)) {
        m_emulatorView->StepInstruction();
    }
    ImGui::SetItemTooltip("Step Instruction");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!emuRunning || !m_emulatorView->IsPaused());
    if (ImGui::Button(ICON_FA_ANGLES_RIGHT "##StepOver", btnSize)) {
        m_emulatorView->StepOver();
    }
    ImGui::SetItemTooltip("Step Over");
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_MICROCHIP "##DebuggerConsole", btnSize)) {
        m_debuggerView->ToggleConsole();
    }
    ImGui::SetItemTooltip("Toggle Debugger Console");

    // Right-aligned logo: position it so it fills the full toolbar height
    if (m_logoTexture != 0) {
        // Use the known icon height (same as buttons) for exact vertical fill
        float logoH = iconSize;
        float logoW = logoH * m_logoAspect;

        // The window content region max X is the right edge; subtract logo width for
        // right-alignment
        float rightEdge = ImGui::GetWindowContentRegionMax().x;
        float logoX = rightEdge - logoW;
        float cursorY = ImGui::GetWindowContentRegionMin().y;

        // Only draw if there is enough room to the right of the current cursor position
        ImGui::SameLine();
        float curPosX = ImGui::GetCursorPosX();
        if (logoX > curPosX) {
            ImGui::SetCursorPos(ImVec2(logoX, cursorY));
            ImGui::Image(ui::TextureManager::ToImTextureID(static_cast<GLuint>(m_logoTexture)),
                         ImVec2(logoW, logoH));
        }
    }

    ImGui::End(); // End Toolbar

    ImGui::PopStyleVar(4); // WindowBorderSize + FrameRounding + ItemSpacing + WindowPadding
}

// ---------------------------------------------------------------------------
// Frame history (rewind)
// ---------------------------------------------------------------------------

ui::MainLayout::DrawContext::FrameHistoryMenu Application::GetFrameHistoryMenuState() const
{
    const auto& prefs = m_userPrefs.Get();
    ui::MainLayout::DrawContext::FrameHistoryMenu m;
    m.enabled = prefs.frameHistoryEnabled;
    m.budgetMB = prefs.frameHistoryBudgetMB;
    m.persistInSession = prefs.frameHistoryPersistInSession;
    m.thumbnails = prefs.frameHistoryThumbnails;
    m.thumbnailInterval = prefs.frameHistoryThumbnailInterval;

    const auto st = m_timeline.Stats();
    m.recording = st.recording && !st.empty;
    m.frames = st.frameCount;
    m.reachFrames = st.estimatedCapacityFrames;

    const auto ts = m_timeline.ThumbnailStats();
    m.thumbnailCount = ts.count;
    m.thumbnailBytes = ts.bytesUsed;
    return m;
}

void Application::SetCoreVerboseLog(bool enabled)
{
    m_userPrefs.SetCoreVerboseLog(enabled);
    AmigaEmulator::SetCoreLogVerbose(enabled);
    // Worth a line of its own: the core log arrives unprefixed apart from "PuAE:", and
    // without this it is not obvious where the sudden traffic came from - or why it stopped.
    Logger::Info("Core log output {}", enabled ? "enabled" : "disabled");
}

} // namespace core
} // namespace adm
