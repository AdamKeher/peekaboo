#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "FloppyBridgeLimits.h"
#include "HostMountJson.h"
#include "HostMountService.h"
#include "JsonRead.h"
#include "LaunchOptions.h"
#include "WhdloadLaunch.h"

namespace adm::core {

class UserPreferences
{
public:
    // Settings written before the floppy-sound options were understood carry three broken values:
    // puae_floppy_sound held an attenuation the UI treated as a switch (so "100", meant as "on", is
    // silence), puae_floppy_sound_type named an external sample set that ships with no files (which
    // clears dfxclick and stops drive sound outright), and the mute-when-empty flag used a key the
    // core has never read. That dead key is what identifies such a block; seeing it, translate the
    // three into what the same intent means now. Runs over the persisted preferences and over any
    // custom configuration profile, which stores the same variables.
    template <typename MapT> static void MigrateLegacyFloppySound(MapT& settings)
    {
        auto legacy = settings.find("puae_floppy_sound_empty_drive");
        if (legacy == settings.end())
            return;

        // Old checkbox: "false" meant ticked, i.e. mute drives with no disk in them.
        settings["puae_floppy_sound_empty_mute"] =
            (legacy->second == "false") ? "enabled" : "disabled";
        settings.erase(legacy);

        // Old checkbox: "100" meant ticked. Give that back the core's default 20% loudness.
        auto& volume = settings["puae_floppy_sound"];
        volume = (volume == "0") ? "100" : "80";

        settings["puae_floppy_sound_type"] = "internal";
    }

    // Four settings the CPU & Chipset tab used to write that PuAE has never had an option for:
    // the core derives cpu_compatible, cpu_cycle_exact, cpu_memory_cycle_exact and
    // blitter_cycle_exact from puae_cpu_compatibility alone and never reads a variable for any
    // of them. Every settings file written before that was noticed still carries them, asserting
    // four settings nothing has ever applied. Dropping them changes no behaviour; it stops the
    // file claiming otherwise. Runs over the persisted preferences and over any custom
    // configuration profile, which stores the same variables.
    template <typename MapT> static void DropOptionsTheCoreDoesNotHave(MapT& settings)
    {
        for (const char* key : {"puae_cpu_compatible", "puae_cpu_cycle_exact",
                                "puae_cpu_memory_cycle_exact", "puae_blitter_cycle_exact"}) {
            settings.erase(key);
        }
    }

    static constexpr int MAX_RECENT_FILES = 10;
    const std::string CONFIG_FILENAME = ProfilePath("user_prefs.json").string();

    struct PanelDef
    {
        std::string name;
        uint32_t id = 0;
    };

    // Text sizing. uiScale zooms the whole interface (text, padding, frame heights); each dense
    // role draws its text at uiScale * scale, so a ratio set once holds at every zoom. The role
    // order is FontRole's (ui/FontRoles.h): Interface, Code, Memory, Console.
    static constexpr float kMinUiScale = 0.75f;
    static constexpr float kMaxUiScale = 2.0f;
    static constexpr float kMinRoleScale = 0.6f;
    static constexpr float kMaxRoleScale = 2.0f;
    static constexpr int kFontRoleCount = 4;

    struct FontRolePrefs
    {
        std::string face; // a FontManager face name; empty = the interface font
        float scale = 1.0f;
        bool operator==(const FontRolePrefs&) const = default;
    };

    struct FontPrefs
    {
        float uiScale = 1.0f;
        std::array<FontRolePrefs, kFontRoleCount> roles;
        bool operator==(const FontPrefs&) const = default;
    };

    // The next interface zoom stop in a direction (+1 larger, -1 smaller, 0 back to 100%), on the
    // same ladder browsers use, so Ctrl+= / Ctrl+- land on round, familiar sizes. A value between
    // stops moves to the neighbouring stop, never past it.
    static float StepUiScale(float current, int direction)
    {
        static constexpr float kStops[] = {0.75f, 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f};
        if (direction == 0)
            return 1.0f;
        constexpr float eps = 0.001f;
        if (direction > 0) {
            for (float stop : kStops)
                if (stop > current + eps)
                    return stop;
            return kMaxUiScale;
        }
        for (auto it = std::rbegin(kStops); it != std::rend(kStops); ++it)
            if (*it < current - eps)
                return *it;
        return kMinUiScale;
    }

    static FontPrefs ClampFontPrefs(FontPrefs f)
    {
        f.uiScale = std::clamp(f.uiScale, kMinUiScale, kMaxUiScale);
        // The Interface row has no scale of its own: its size is uiScale.
        f.roles[0].scale = 1.0f;
        for (auto& role : f.roles)
            role.scale = std::clamp(role.scale, kMinRoleScale, kMaxRoleScale);
        return f;
    }

    static nlohmann::json FontPrefsToJson(const FontPrefs& fonts)
    {
        nlohmann::json roles = nlohmann::json::array();
        for (const auto& role : fonts.roles)
            roles.push_back({{"face", role.face}, {"scale", role.scale}});
        return {{"uiScale", fonts.uiScale}, {"roles", roles}};
    }

    static FontPrefs FontPrefsFromJson(const nlohmann::json& jf, FontPrefs f)
    {
        if (!jf.is_object())
            return f;
        if (jf.contains("uiScale") && jf["uiScale"].is_number())
            f.uiScale = jf["uiScale"].get<float>();
        if (jf.contains("roles") && jf["roles"].is_array()) {
            const auto& jr = jf["roles"];
            for (size_t i = 0; i < f.roles.size() && i < jr.size(); ++i) {
                if (!jr[i].is_object())
                    continue;
                if (jr[i].contains("face") && jr[i]["face"].is_string())
                    f.roles[i].face = jr[i]["face"].get<std::string>();
                if (jr[i].contains("scale") && jr[i]["scale"].is_number())
                    f.roles[i].scale = jr[i]["scale"].get<float>();
            }
        }
        return ClampFontPrefs(f);
    }

    // The settings that belong to a screen rather than to the user: text sizes, the main window
    // rectangle, and splitter and column counts measured in pixels. Preferences holds the live
    // profile's copy in its ordinary fields; this is how the other profile's copy is kept.
    // Table column widths are per profile too, in a file of their own (ui/TableLayout.h).
    struct ScreenProfilePrefs
    {
        FontPrefs fonts;
        std::string fontName = "Default";
        int windowWidth = 1280;
        int windowHeight = 720;
        int windowX = -1;
        int windowY = -1;
        bool maximized = false;
        float spritesScanlinesPanelW = 0.0f;
        float spritesDetailPanelH = 0.0f;
        float memMapLeftColumnW = 0.0f;
        float memMapPreviewH = 0.0f;
        int memoryBytesPerLine = 16;
    };

    struct Preferences
    {
        struct BitplanePrefs
        {
            int sourceMode = 0;
            int manualBplCount = 3;
            int manualWidth = 320;
            int manualHeight = 256;
            int manualModulo1 = 0;
            int manualModulo2 = 0;
            bool useManualModulo1 = false;
            bool useManualModulo2 = false;
            bool interleavedAuto = true;
            bool interleavedForce = false;
            bool useRegisterPointers = true;
            std::array<uint32_t, 8> manualPointers{};
            bool useManualStride1 = false;
            bool useManualStride2 = false;
            int manualStride1 = 40;
            int manualStride2 = 40;
            bool overrideWidth = false;
            bool overrideHeight = false;
            bool applyPalette = false;
            bool showComposite = true;
            float zoom = 1.0f;
            bool lockAspect = false;
            bool fitToWindow = true;
            bool showInspector = true;

            bool operator==(const BitplanePrefs&) const = default;
        };

        // Debugger -> Audio window. channelMask is deliberately persisted: leaving a voice
        // muted across a restart and forgetting why is a worse surprise than losing the setting.
        struct AudioPrefs
        {
            uint8_t channelMask = 0x0F; // bit n = Paula channel n audible
            int filterMode = 1;         // 0 off, 1 emulated (follows the power LED), 2 on
            int filterModel = 0;        // 0 auto, 1 A500, 2 A1200
            int timeBaseSamples = 512;  // samples across the width of one scope
            float yZoom = 1.0f;
            bool showRegisters = true;

            bool operator==(const AudioPrefs&) const = default;
        };

        std::string fontName = "Default";
        std::vector<std::string> recentFiles;
        std::vector<std::string> recentDirectories;
        int windowWidth = 1280;
        int windowHeight = 720;
        int windowX = -1;
        int windowY = -1;
        bool maximized = false;
        int gwDriveIndex = 0;
        int gwFormatIndex = 0;
        bool gwVerify = true;
        bool gwPreErase = false;
        bool gwEraseEmpty = false;
        bool gwReadVerify = false;
        // Disk images open write-protected, so an accidental drag or a booted game
        // cannot damage a preserved image. Turn it off for a heavy editing session;
        // the padlock in each document window still overrides it per image.
        bool openImagesWriteProtected = true;
        // FloppyBridge live physical drive, one configuration per
        // emulated slot DF0-DF3 so several real drives can be live at once. hardwareType
        // indexes AmigaEmulator::GetBridgeDriverList(); driveCable is FloppyBridge's
        // DriveSelection (0 = drive A / IBM PC, 1 = drive B, 2-5 = Shugart 0-3) and is
        // genuinely needed - the default failed against real hardware that turned out to be
        // cabled as drive B. Only the *configuration* is persisted, never that a drive was
        // attached: live hardware state is never restored.
        struct FbSlotPrefs
        {
            int hardwareType = 1; // Greaseweazle
            std::string comPort;
            int bridgeMode = 0;    // 0 Fast, 1 Compatible, 2 TurboAmigaDOS, 3 Stalling
            int bridgeDensity = 0; // 0 Auto, 1 DD only, 2 HD only
            int driveCable = 0;
            // Read-ahead and timing. Both default on: without them a bridged drive re-reads
            // a whole revolution on every seek, which measured ~2.5 revolutions per cylinder
            // step against a real trackloader.
            bool autoCache = true;
            bool smartSpeed = true;
            // Never persisted as true by accident: writing real media is destructive, so
            // this is off unless the user turned it on for this slot. Driver 4 only.
            bool allowWrites = false;
        };
        std::array<FbSlotPrefs, 4> fbSlots;
        int fbAssignedDrive = 0; // the slot tab the Live Floppy Drive window last showed
        bool showLiveFloppy = false;
        std::map<std::string, std::string> controllerTypes;
        std::string defaultControllerType = "Standard Joystick";
        bool keyboardJoystick = false;
        bool showBlock = true;
        bool showBlockDetails = true;
        bool showDiskBitmap = true;
        bool showBlockTypeMap = true;
        bool showNdosMap = true;
        bool showBlockChangeMap = true;
        bool showIdentifyDisk = false;
        bool showLog = false;
        bool showEmulator = false;
        bool showDebugger = false; // legacy - kept for migration
        bool showMemoryMap = false;
        bool showDebugCpu = false;
        bool showDebugCustom = false;
        bool showDebugDisassembly = false;
        bool showDebugCopper = false;
        bool showDebugMemory = false;
        bool showDebugDisplay = false;
        bool showDebugSprites = false;
        bool showDebugBitplanes = false;
        // Blit overlay (Debugger -> Display -> Blitter). Off by default: while it is on the
        // core-side provenance map costs a per-fetch lookup and ~50 MB of tables.
        //
        // Replaced the old standalone `showBlitProvenance` bool, which is still read on load so
        // an existing user_prefs.json keeps its setting.
        struct BlitOverlayPrefs
        {
            bool enabled = false;
            bool useProvenance = true;
            bool outlineAll = false;
            float opacity = 0.65f;
            int decayFrames = 5;
        };
        BlitOverlayPrefs blitOverlay;

        // Blitter destination capture (Blitter Operations -> "Data"). Off by default: the
        // core keeps a 4 MB ring of every word the blitter writes while it is on.
        bool captureBlitDestData = false;

        // Frame history (the "state buffer"): the rolling per-frame save-state timeline
        // that makes stepping backwards - and re-running a past frame with capture on -
        // possible. Recording only runs while a debugger window is open, so it costs
        // nothing during ordinary disk work. Off by default even so: a save state per frame
        // is real memory and real per-frame time, and most debugging never rewinds - so it
        // is armed deliberately, from the Frame Scrubber's record button or the Emulator >
        // Frame History menu.
        // Let PUAE's own log through into the Log window. Off by default - the core writes
        // several lines per save state and per disk event, all of it from the emulation
        // thread, so it is turned on to read a specific diagnostic and turned off again.
        bool coreVerboseLog = false;
        bool frameHistoryEnabled = false;
        int frameHistoryBudgetMB = 256;
        int frameHistoryKeyframeInterval = 60;
        // Whether a saved .peek session carries the whole recorded timeline. Off by
        // default: the timeline runs to hundreds of megabytes, which is not something to
        // write into every saved session without being asked for it.
        bool frameHistoryPersistInSession = false;
        // Periodic screen thumbnails alongside the timeline, so dragging the frame seek bar
        // shows the picture at that frame instead of just a number. One capture every N
        // frames, sampled from the core's framebuffer.
        bool frameHistoryThumbnails = true;
        int frameHistoryThumbnailInterval = 10;

        // Chipset display debug options (Debugger -> Display). Defaults are the hardware
        // behaviour: no palette ramp, BPLCON1 scroll honoured.
        bool paletteVis = false;
        bool bplcon1Scroll = true;

        // Copper / bitplane-pointer write blocks (Debugger -> Display). These suppress real
        // register writes, so they are persisted OFF-by-default and the line range is kept:
        // coming back to a session with a block silently armed would be baffling, but the
        // range is the part worth remembering.
        struct BlockPrefs
        {
            bool copperEnabled = false;
            int copperStart = 52;
            int copperEnd = 308;
            int bplptrMask = 0;
            int bplptrStart = 52;
            int bplptrEnd = 308;
            bool blitterWrites = true;
        };
        BlockPrefs blocks;

        // Copper / DMA raster overlay (Debugger -> Display -> Raster). Off by default: it
        // adjusts the emulator window and arms the core's DMA recording while it is on.
        struct RasterPrefs
        {
            bool enabled = false;
            bool showCopper = true;
            bool showWaitSpans = false;
            bool showCopperWanted = true;
            bool showBlanking = true;
            bool showGeometryEvents = true;
            int channelMask = 0;
            float opacity = 0.6f;
        };
        RasterPrefs raster;

        // Interleaved C/C++ source in the Debugger -> Disassembly listing. Off by
        // default: it needs DWARF line info AND the source tree present on this machine,
        // and source rows are expensive in vertical space. `detail` therefore defaults to
        // the tightest mode rather than the fullest one.
        struct SourcePrefs
        {
            bool showInDisassembly = false;
            int detail = 0;       // 0 = statement only, 1 = code only, 2 = full gap fill
            int maxGapLines = 24; // cap on an objdump -S style gap fill
            int tabWidth = 4;
            bool showFileHeaders = true;
            bool showLineNumbers = true;
            // Extra directories to search when the DWARF-recorded path does not exist
            // locally, tried in order after the .elf-relative probes.
            std::vector<std::string> searchRoots;

            bool operator==(const SourcePrefs&) const = default;
        };
        SourcePrefs source;

        BitplanePrefs bitplanes;
        bool showDebugAudio = false;
        AudioPrefs audio;
        bool showBartmanBitmaps = false;
        bool bartmanDebugEnabled = false; // opt-in force-boot-rom for the Bartman uaelib gate
        bool showDebugCia = false;
        bool showDebugConsole = false;
        bool showBreakpoints = false;
        bool showAnnotations = false;
        bool showDebugCallStack = false;
        bool showDebugTrainer = false;
        bool showDebugProfile = false;
        bool showBlitter = false;
        bool showRegisterOps = false;
        // Register Operations filters: source (0 all, 1 CPU, 2 Copper) and "changed only".
        int registerOpsSource = 0;
        bool registerOpsChangedOnly = false;
        bool showDiskIo = false;
        // Exocet: the window, the Python and tools directory kit commands
        // run with (empty: the ones Peekaboo ships, else `python` on PATH), and the kits the user
        // has said may run, by ExocetKitHash.
        bool showExocet = false;
        // Exocet and the crAss engine's tools shown at all (core/PrivateFeatures.h). No control
        // in Preferences: set by hand in this file.
        bool privateFeatures = false;
        std::string exocetPython;
        std::string exocetTools;
        std::vector<std::string> exocetTrustedKits;
        bool showBlitterMinterm = false;
        bool showDmaStats = false;
        bool showFrameScrubber = false;
        int frameScrubberUnit = 0; // 0=%, 1=µs, 2=Cycles, 3=Rasterlines
        bool frameScrubberShowInstrNumbers = true;
        int frameScrubberHeatMap = 0; // top-row heatmap: 0=CPU density, 1=bus stalls
        // Sprites window resizable panels (0 = use the view's built-in default)
        float spritesScanlinesPanelW = 0.0f;
        float spritesDetailPanelH = 0.0f;
        // Memory Map window splitters (0 = use the view's built-in default)
        float memMapLeftColumnW = 0.0f;
        float memMapPreviewH = 0.0f;
        int memoryBytesPerLine = 16; // Memory window hex columns (16/24/32)
        int blockBytesPerLine = 16;  // Block window hex columns (16/24/32)
        bool blockShowTrack = false; // Block window shows every sector of the track
        bool showPerfOverlay = false;
        bool showPerfGraph = false;
        // Emulator display post-process filters. Plain mirror of ui::EmulatorFilterParams
        // (core must not depend on the UI header); Application bridges the two. Defaults match
        // the param struct, and all effects start disabled.
        struct DisplayFilterPrefs
        {
            bool windowOpen = false;
            bool scanlines = false;
            float scanlineStrength = 0.25f;
            bool crt = false;
            float maskStrength = 0.30f;
            float glowStrength = 0.15f;
            bool curvature = false;
            float curveAmount = 1.0f;
            float vignetteStrength = 1.0f;
            bool colorGrade = false;
            float gamma = 1.10f;
            float contrast = 1.08f;
            float saturation = 1.15f;
            float warmth = 0.04f;
        };
        DisplayFilterPrefs displayFilters;
        bool showTosecBrowser = false;
        std::string kickstartRomDir; // directory containing kickstart ROM files
        float transitionDuration = 0.6f;
        std::map<std::string, std::string> emulatorSettings; // PuAE core variable overrides
        // The machine those settings belong to (AmigaModelInfo id). Selecting a model writes its
        // Kickstart and memory sizes into emulatorSettings, so restoring one without the other
        // started an A500 with an A4000's ROM and 32-bit memory - which crashed the core.
        std::string emulatorModel;
        std::vector<PanelDef> panels; // user-created panels (restored on next launch)
        bool emulatorRunWhenHidden = true;
        FontPrefs fonts;
        // Compile-and-run backend: automatic, host_directory, or bootable_adf.
        std::string peekabooLaunchMode = "automatic";
        std::array<HostDirectoryMount, 2> hostDirectoryMounts = [] {
            std::array<HostDirectoryMount, 2> mounts;
            mounts[0].device = HostMountDevice::DH0;
            mounts[0].volumeLabel = "DH0";
            mounts[1].device = HostMountDevice::DH1;
            mounts[1].volumeLabel = "DH1";
            return mounts;
        }();
        std::array<bool, 2> hardDriveGroupsOpen{true, true};
        // Boot ROMs for the A590/A2091 and A4091 SCSI cards a hardfile can be attached to.
        HardDriveControllerRoms hardDriveControllerRoms;
        // WHDLoad.prefs choices applied before every WHDLoad launch.
        WhdloadSettings whdload;
        // The disc (image or archive of one) CD0 gets at power-on.
        std::string cdImagePath;
        // GIF & Video Capture. The codec by VideoCaptureCodecInfo::id; empty until the user has
        // chosen one, and the window then picks H.264 when FFmpeg can do it, ZMBV otherwise.
        std::string videoCaptureCodec;
        int videoCaptureQuality = -1; // CRF for H.264/H.265; -1 takes the codec's default
        std::string videoCapturePath;
        std::string ffmpegPath; // empty: find FFmpeg on PATH
        // The emulator's output volume, 0-100 (the Emulator window's volume popup). Mute is not
        // kept: coming back to silence and forgetting why is the worse surprise.
        int emulatorVolume = 100;
        // Serial port target in the core's spelling ("TCP://0.0.0.0:1234", "COM3"), and the
        // file the parallel port writes to. Empty: the user never chose one.
        std::string serialPort;
        std::string parallelOutputPath;
        // MIDI devices for the serial port at MIDI speed: out -2 none, -1 mapper; in -1 none.
        int midiOutDevice = -2;
        int midiInDevice = -1;
        // MCP server (Claude Desktop integration). Token gates the local HTTP API; generated on
        // first enable. showMcpActivity persists the MCP Activity window's open state.
        bool mcpEnabled = false;
        int mcpPort = 7475;
        std::string mcpToken;
        // GDB remote-serial-protocol server, so a real m68k-amiga-elf-gdb (and through it, VS
        // Code) can debug the running program. Off by default: the port grants full read/write
        // access to emulated memory, so it is opt-in even on loopback. NOT 2345 -- Bartman's
        // extension hardcodes that port in two places.
        bool gdbEnabled = false;
        int gdbPort = 2346;
        bool showMcpActivity = false;
        // ImGui name of the window that was focused at exit, restored on next launch so the
        // last-used panel comes back to the front instead of an arbitrary window stealing focus.
        std::string focusedWindow;
        // Name of the workspace to restore at startup. Workspace contents are stored in
        // workspaces.json and are only changed by the explicit Save Workspace action.
        std::string activeWorkspace;
        // The live screen profile, "hires" or "lores" (core/ScreenProfile.h). Empty until the
        // user has chosen one; the first launch then suggests one for the screen it is on.
        std::string screenProfile;
        // The screen-dependent settings of every profile NOT in use, keyed like screenProfile.
        // The live profile's are the ordinary fields above; SwitchScreenProfile swaps them.
        std::map<std::string, ScreenProfilePrefs> screenProfiles;
        // The setup wizard (ui/views/OnboardingView) has run, or was not needed: the ROM folder
        // already held a preset's Kickstart at the first start that asked.
        bool onboardingDone = false;
    };

    UserPreferences() { Load(); }

    void SetOnboardingDone(bool done)
    {
        if (m_prefs.onboardingDone == done)
            return;
        m_prefs.onboardingDone = done;
        Save();
    }

    const Preferences& Get() const { return m_prefs; }

    // Allow modifying preferences directly
    Preferences& GetMutable() { return m_prefs; }

    void SetShowIdentifyDisk(bool show)
    {
        m_prefs.showIdentifyDisk = show;
        Save();
    }

    void SetActiveWorkspace(const std::string& name)
    {
        m_prefs.activeWorkspace = name;
        Save();
    }

    /**
     * @brief Make @p to the live screen profile, swapping the screen-dependent settings.
     *
     * The live profile's settings are stashed under its key and @p to's stored ones become live.
     * A profile with nothing stored starts from the live values. The first choice ever (no
     * profile yet) stashes nothing - the package ships with every profile stored and none
     * chosen, so a first run picks up the shipped settings of whichever the user takes.
     * @return true when @p to's stored settings were restored, i.e. the live values changed.
     */
    bool SwitchScreenProfile(const std::string& to)
    {
        const std::string from = m_prefs.screenProfile;
        if (from == to)
            return false;
        if (!from.empty())
            m_prefs.screenProfiles[from] = CaptureScreenProfile(m_prefs);
        bool restored = false;
        if (auto it = m_prefs.screenProfiles.find(to); it != m_prefs.screenProfiles.end()) {
            ApplyScreenProfile(it->second, m_prefs);
            m_prefs.screenProfiles.erase(it);
            restored = true;
        }
        m_prefs.screenProfile = to;
        Save();
        return restored;
    }

    static ScreenProfilePrefs CaptureScreenProfile(const Preferences& p)
    {
        ScreenProfilePrefs s;
        s.fonts = p.fonts;
        s.fontName = p.fontName;
        s.windowWidth = p.windowWidth;
        s.windowHeight = p.windowHeight;
        s.windowX = p.windowX;
        s.windowY = p.windowY;
        s.maximized = p.maximized;
        s.spritesScanlinesPanelW = p.spritesScanlinesPanelW;
        s.spritesDetailPanelH = p.spritesDetailPanelH;
        s.memMapLeftColumnW = p.memMapLeftColumnW;
        s.memMapPreviewH = p.memMapPreviewH;
        s.memoryBytesPerLine = p.memoryBytesPerLine;
        return s;
    }

    static void ApplyScreenProfile(const ScreenProfilePrefs& s, Preferences& p)
    {
        p.fonts = s.fonts;
        p.fontName = s.fontName;
        p.windowWidth = s.windowWidth;
        p.windowHeight = s.windowHeight;
        p.windowX = s.windowX;
        p.windowY = s.windowY;
        p.maximized = s.maximized;
        p.spritesScanlinesPanelW = s.spritesScanlinesPanelW;
        p.spritesDetailPanelH = s.spritesDetailPanelH;
        p.memMapLeftColumnW = s.memMapLeftColumnW;
        p.memMapPreviewH = s.memMapPreviewH;
        p.memoryBytesPerLine = s.memoryBytesPerLine;
    }

    void SetShowLog(bool show)
    {
        m_prefs.showLog = show;
        Save();
    }

    void SetShowEmulator(bool show)
    {
        m_prefs.showEmulator = show;
        Save();
    }

    void SetShowDebugger(bool show)
    {
        m_prefs.showDebugger = show;
        Save();
    }

    void SetShowDebugCpu(bool show)
    {
        m_prefs.showDebugCpu = show;
        Save();
    }
    void SetShowDebugCustom(bool show)
    {
        m_prefs.showDebugCustom = show;
        Save();
    }
    void SetShowDebugDisassembly(bool show)
    {
        m_prefs.showDebugDisassembly = show;
        Save();
    }
    void SetShowDebugCopper(bool show)
    {
        m_prefs.showDebugCopper = show;
        Save();
    }
    void SetShowDebugMemory(bool show)
    {
        m_prefs.showDebugMemory = show;
        Save();
    }
    void SetShowDebugDisplay(bool show)
    {
        m_prefs.showDebugDisplay = show;
        Save();
    }
    void SetShowDebugSprites(bool show)
    {
        m_prefs.showDebugSprites = show;
        Save();
    }
    void SetCoreVerboseLog(bool enabled)
    {
        m_prefs.coreVerboseLog = enabled;
        Save();
    }
    void SetFrameHistoryEnabled(bool enabled)
    {
        m_prefs.frameHistoryEnabled = enabled;
        Save();
    }
    void SetFrameHistoryBudgetMB(int mb)
    {
        m_prefs.frameHistoryBudgetMB = mb;
        Save();
    }
    void SetFrameHistoryKeyframeInterval(int frames)
    {
        m_prefs.frameHistoryKeyframeInterval = frames;
        Save();
    }
    void SetFrameHistoryPersistInSession(bool persist)
    {
        m_prefs.frameHistoryPersistInSession = persist;
        Save();
    }
    void SetFrameHistoryThumbnails(bool enabled)
    {
        m_prefs.frameHistoryThumbnails = enabled;
        Save();
    }
    void SetFrameHistoryThumbnailInterval(int frames)
    {
        m_prefs.frameHistoryThumbnailInterval = std::max(1, frames);
        Save();
    }
    void SetRasterPrefs(const Preferences::RasterPrefs& r)
    {
        m_prefs.raster = r;
        Save();
    }
    void SetBlitOverlayPrefs(const Preferences::BlitOverlayPrefs& b)
    {
        m_prefs.blitOverlay = b;
        Save();
    }
    void SetCaptureBlitDestData(bool on)
    {
        m_prefs.captureBlitDestData = on;
        Save();
    }
    void SetPaletteVis(bool enabled)
    {
        m_prefs.paletteVis = enabled;
        Save();
    }
    void SetBplcon1Scroll(bool enabled)
    {
        m_prefs.bplcon1Scroll = enabled;
        Save();
    }
    void SetBlockPrefs(const Preferences::BlockPrefs& b)
    {
        m_prefs.blocks = b;
        Save();
    }
    void SetShowDebugBitplanes(bool show)
    {
        m_prefs.showDebugBitplanes = show;
        Save();
    }

    void SetBitplaneSettings(const Preferences::BitplanePrefs& settings)
    {
        m_prefs.bitplanes = settings;
        Save();
    }
    void SetShowDebugAudio(bool show)
    {
        m_prefs.showDebugAudio = show;
        Save();
    }
    void SetAudioSettings(const Preferences::AudioPrefs& settings)
    {
        m_prefs.audio = settings;
        Save();
    }
    void SetShowBartmanBitmaps(bool show)
    {
        m_prefs.showBartmanBitmaps = show;
        Save();
    }
    void SetBartmanDebugEnabled(bool enabled)
    {
        m_prefs.bartmanDebugEnabled = enabled;
        Save();
    }

    void SetPeekabooLaunchMode(const std::string& mode)
    {
        if (mode != "automatic" && mode != "host_directory" && mode != "bootable_adf")
            return;
        m_prefs.peekabooLaunchMode = mode;
        Save();
    }

    void SetHostDirectoryMounts(const std::array<HostDirectoryMount, 2>& mounts)
    {
        m_prefs.hostDirectoryMounts = mounts;
        Save();
    }

    void SetHardDriveControllerRoms(const HardDriveControllerRoms& roms)
    {
        if (m_prefs.hardDriveControllerRoms == roms)
            return;
        m_prefs.hardDriveControllerRoms = roms;
        Save();
    }

    void SetHardDriveGroupOpen(size_t index, bool open)
    {
        if (index >= m_prefs.hardDriveGroupsOpen.size() ||
            m_prefs.hardDriveGroupsOpen[index] == open) {
            return;
        }
        m_prefs.hardDriveGroupsOpen[index] = open;
        Save();
    }

    void AddRecentDirectory(const std::string& path)
    {
        if (path.empty())
            return;
        auto& directories = m_prefs.recentDirectories;
        directories.erase(std::remove(directories.begin(), directories.end(), path),
                          directories.end());
        directories.insert(directories.begin(), path);
        if (directories.size() > MAX_RECENT_FILES)
            directories.resize(MAX_RECENT_FILES);
        Save();
    }
    void SetShowDebugCia(bool show)
    {
        m_prefs.showDebugCia = show;
        Save();
    }
    void SetShowDebugCallStack(bool show)
    {
        m_prefs.showDebugCallStack = show;
        Save();
    }
    void SetShowDebugTrainer(bool show)
    {
        m_prefs.showDebugTrainer = show;
        Save();
    }
    void SetShowDebugProfile(bool show)
    {
        m_prefs.showDebugProfile = show;
        Save();
    }
    void SetShowDebugConsole(bool show)
    {
        m_prefs.showDebugConsole = show;
        Save();
    }
    void SetShowBreakpoints(bool show)
    {
        m_prefs.showBreakpoints = show;
        Save();
    }
    void SetShowAnnotations(bool show)
    {
        m_prefs.showAnnotations = show;
        Save();
    }
    void SetShowBlitter(bool show)
    {
        m_prefs.showBlitter = show;
        Save();
    }
    void SetShowRegisterOps(bool show)
    {
        m_prefs.showRegisterOps = show;
        Save();
    }
    void SetRegisterOpsFilters(int source, bool changedOnly)
    {
        m_prefs.registerOpsSource = source;
        m_prefs.registerOpsChangedOnly = changedOnly;
        Save();
    }
    void SetShowDiskIo(bool show)
    {
        m_prefs.showDiskIo = show;
        Save();
    }
    void SetShowExocet(bool show)
    {
        if (m_prefs.showExocet == show)
            return;
        m_prefs.showExocet = show;
        Save();
    }
    void SetExocetTools(const std::string& python, const std::string& tools)
    {
        m_prefs.exocetPython = python;
        m_prefs.exocetTools = tools;
        Save();
    }
    bool IsExocetKitTrusted(const std::string& hash) const
    {
        const auto& k = m_prefs.exocetTrustedKits;
        return !hash.empty() && std::find(k.begin(), k.end(), hash) != k.end();
    }
    void TrustExocetKit(const std::string& hash)
    {
        if (hash.empty() || IsExocetKitTrusted(hash))
            return;
        m_prefs.exocetTrustedKits.push_back(hash);
        Save();
    }
    void SetShowBlitterMinterm(bool show)
    {
        m_prefs.showBlitterMinterm = show;
        Save();
    }
    void SetShowDmaStats(bool show)
    {
        m_prefs.showDmaStats = show;
        Save();
    }
    void SetShowFrameScrubber(bool show)
    {
        m_prefs.showFrameScrubber = show;
        Save();
    }

    void SetFrameScrubberSettings(int unit, bool showInstrNumbers, int heatMap)
    {
        m_prefs.frameScrubberUnit = unit;
        m_prefs.frameScrubberShowInstrNumbers = showInstrNumbers;
        m_prefs.frameScrubberHeatMap = heatMap;
        Save();
    }
    void SetSpritesPanelSizes(float scanlinesPanelW, float detailPanelH)
    {
        m_prefs.spritesScanlinesPanelW = scanlinesPanelW;
        m_prefs.spritesDetailPanelH = detailPanelH;
        Save();
    }
    void SetMemMapPanelSizes(float leftW, float previewH)
    {
        m_prefs.memMapLeftColumnW = leftW;
        m_prefs.memMapPreviewH = previewH;
        Save();
    }
    void SetMemoryBytesPerLine(int bpl)
    {
        m_prefs.memoryBytesPerLine = bpl;
        Save();
    }
    void SetBlockBytesPerLine(int bpl)
    {
        m_prefs.blockBytesPerLine = bpl;
        Save();
    }
    void SetBlockShowTrack(bool show)
    {
        m_prefs.blockShowTrack = show;
        Save();
    }

    void SetShowPerfOverlay(bool show)
    {
        m_prefs.showPerfOverlay = show;
        Save();
    }
    void SetShowPerfGraph(bool show)
    {
        m_prefs.showPerfGraph = show;
        Save();
    }

    void SetShowTosecBrowser(bool show)
    {
        m_prefs.showTosecBrowser = show;
        Save();
    }

    void SetFbSlotSettings(int slot, const Preferences::FbSlotPrefs& settings)
    {
        if (slot < 0 || slot > 3)
            return;
        m_prefs.fbSlots[static_cast<size_t>(slot)] = settings;
        Save();
    }

    void SetFbSelectedSlot(int slot)
    {
        if (slot < 0 || slot > 3)
            return;
        m_prefs.fbAssignedDrive = slot;
        Save();
    }

    void SetShowLiveFloppy(bool show)
    {
        m_prefs.showLiveFloppy = show;
        Save();
    }

    void SetEmulatorRunWhenHidden(bool v)
    {
        m_prefs.emulatorRunWhenHidden = v;
        Save();
    }

    // persist = false changes the live value only, for a slider mid-drag; the release saves.
    void SetFontPrefs(const FontPrefs& fonts, bool persist = true)
    {
        m_prefs.fonts = ClampFontPrefs(fonts);
        if (persist)
            Save();
    }

    void SetMcpSettings(bool enabled, int port, const std::string& token)
    {
        m_prefs.mcpEnabled = enabled;
        m_prefs.mcpPort = port;
        m_prefs.mcpToken = token;
        Save();
    }

    void SetGdbSettings(bool enabled, int port)
    {
        m_prefs.gdbEnabled = enabled;
        m_prefs.gdbPort = port;
        Save();
    }

    void SetShowMcpActivity(bool show)
    {
        m_prefs.showMcpActivity = show;
        Save();
    }

    void SetWhdloadSettings(const WhdloadSettings& settings)
    {
        if (m_prefs.whdload == settings)
            return;
        m_prefs.whdload = settings;
        Save();
    }

    void SetHostPorts(const std::string& serialPort, const std::string& parallelOutputPath)
    {
        if (m_prefs.serialPort == serialPort && m_prefs.parallelOutputPath == parallelOutputPath)
            return;
        m_prefs.serialPort = serialPort;
        m_prefs.parallelOutputPath = parallelOutputPath;
        Save();
    }

    void SetMidiDevices(int outDevice, int inDevice)
    {
        if (m_prefs.midiOutDevice == outDevice && m_prefs.midiInDevice == inDevice)
            return;
        m_prefs.midiOutDevice = outDevice;
        m_prefs.midiInDevice = inDevice;
        Save();
    }

    void SetVideoCapture(const std::string& codec, int quality, const std::string& path)
    {
        if (m_prefs.videoCaptureCodec == codec && m_prefs.videoCaptureQuality == quality &&
            m_prefs.videoCapturePath == path)
            return;
        m_prefs.videoCaptureCodec = codec;
        m_prefs.videoCaptureQuality = quality;
        m_prefs.videoCapturePath = path;
        Save();
    }

    void SetEmulatorVolume(int percent)
    {
        percent = std::clamp(percent, 0, 100);
        if (m_prefs.emulatorVolume == percent)
            return;
        m_prefs.emulatorVolume = percent;
        Save();
    }

    void SetFfmpegPath(const std::string& path)
    {
        if (m_prefs.ffmpegPath == path)
            return;
        m_prefs.ffmpegPath = path;
        Save();
    }

    void SetCdImagePath(const std::string& path)
    {
        if (m_prefs.cdImagePath == path)
            return;
        m_prefs.cdImagePath = path;
        Save();
    }

    void SetKickstartRomDir(const std::string& dir)
    {
        m_prefs.kickstartRomDir = dir;
        Save();
    }

    void SetTransitionDuration(float duration)
    {
        m_prefs.transitionDuration = duration;
        Save();
    }

    void SetEmulatorSettings(const std::map<std::string, std::string>& settings)
    {
        m_prefs.emulatorSettings = settings;
        Save();
    }

    void SetEmulatorModel(const std::string& modelId)
    {
        m_prefs.emulatorModel = modelId;
        Save();
    }

    void SavePanels(const std::vector<PanelDef>& panels)
    {
        m_prefs.panels = panels;
        Save();
    }

    void SetFocusedWindow(const std::string& name)
    {
        m_prefs.focusedWindow = name;
        Save();
    }

    bool GetShowPerfOverlay() const { return m_prefs.showPerfOverlay; }
    bool GetShowPerfGraph() const { return m_prefs.showPerfGraph; }

    const Preferences::DisplayFilterPrefs& GetDisplayFilters() const
    {
        return m_prefs.displayFilters;
    }
    void SetDisplayFilters(const Preferences::DisplayFilterPrefs& f)
    {
        m_prefs.displayFilters = f;
        Save();
    }

    void SetShowMemoryMap(bool show)
    {
        m_prefs.showMemoryMap = show;
        Save();
    }

    void SetShowBlock(bool show)
    {
        m_prefs.showBlock = show;
        Save();
    }

    void SetShowBlockDetails(bool show)
    {
        m_prefs.showBlockDetails = show;
        Save();
    }

    void SetShowDiskBitmap(bool show)
    {
        m_prefs.showDiskBitmap = show;
        Save();
    }

    void SetShowBlockTypeMap(bool show)
    {
        m_prefs.showBlockTypeMap = show;
        Save();
    }

    void SetShowNdosMap(bool show)
    {
        m_prefs.showNdosMap = show;
        Save();
    }

    void SetShowBlockChangeMap(bool show)
    {
        m_prefs.showBlockChangeMap = show;
        Save();
    }

    void SetFontName(const std::string& fontName)
    {
        m_prefs.fontName = fontName;
        Save();
    }

    void AddRecentFile(const std::string& path)
    {
        if (path.empty())
            return;

        // Normalize path
        std::string normalizedPath;
        try {
            normalizedPath = std::filesystem::absolute(path).string();
        } catch (...) {
            normalizedPath = path;
        }

        // Remove existing entry if present
        auto& files = m_prefs.recentFiles;
        files.erase(std::remove(files.begin(), files.end(), normalizedPath), files.end());

        // Insert at front
        files.insert(files.begin(), normalizedPath);

        // Limit size
        if (files.size() > MAX_RECENT_FILES) {
            files.resize(MAX_RECENT_FILES);
        }

        Save();
    }

    void SaveWindowCoords(int x, int y, int width, int height, bool maximized)
    {
        m_prefs.windowX = x;
        m_prefs.windowY = y;
        m_prefs.windowWidth = width;
        m_prefs.windowHeight = height;
        m_prefs.maximized = maximized;
        Save();
    }

    void SetGwSettings(int driveIndex, int formatIndex, bool verify, bool preErase, bool eraseEmpty,
                       bool readVerify)
    {
        m_prefs.gwDriveIndex = driveIndex;
        m_prefs.gwFormatIndex = formatIndex;
        m_prefs.gwVerify = verify;
        m_prefs.gwPreErase = preErase;
        m_prefs.gwEraseEmpty = eraseEmpty;
        m_prefs.gwReadVerify = readVerify;
        Save();
    }

    void SetOpenImagesWriteProtected(bool value)
    {
        m_prefs.openImagesWriteProtected = value;
        Save();
    }

    void SetControllerType(const std::string& controllerName, const std::string& type)
    {
        m_prefs.controllerTypes[controllerName] = type;
        Save();
    }

    void SetKeyboardJoystick(bool enabled)
    {
        m_prefs.keyboardJoystick = enabled;
        Save();
    }

    bool GetKeyboardJoystick() const { return m_prefs.keyboardJoystick; }

    std::string GetControllerType(const std::string& controllerName) const
    {
        auto it = m_prefs.controllerTypes.find(controllerName);
        if (it != m_prefs.controllerTypes.end()) {
            return it->second;
        }
        return m_prefs.defaultControllerType;
    }

    const std::vector<std::string>& GetRecentFiles() const { return m_prefs.recentFiles; }

    void SetSourceShowInDisassembly(bool show)
    {
        m_prefs.source.showInDisassembly = show;
        Save();
    }

    void SetSourceDetail(int detail)
    {
        m_prefs.source.detail = detail;
        Save();
    }

    void SetSourceSearchRoots(std::vector<std::string> roots)
    {
        m_prefs.source.searchRoots = std::move(roots);
        Save();
    }

    void SetSourceOptions(int maxGapLines, int tabWidth, bool showFileHeaders, bool showLineNumbers)
    {
        m_prefs.source.maxGapLines = maxGapLines;
        m_prefs.source.tabWidth = tabWidth;
        m_prefs.source.showFileHeaders = showFileHeaders;
        m_prefs.source.showLineNumbers = showLineNumbers;
        Save();
    }

private:
    Preferences m_prefs;

    void Save()
    {
        nlohmann::json j;
        j["fontName"] = m_prefs.fontName;
        j["recentFiles"] = m_prefs.recentFiles;
        j["recentDirectories"] = m_prefs.recentDirectories;
        j["windowWidth"] = m_prefs.windowWidth;
        j["windowHeight"] = m_prefs.windowHeight;
        j["windowX"] = m_prefs.windowX;
        j["windowY"] = m_prefs.windowY;
        j["maximized"] = m_prefs.maximized;
        j["gwDriveIndex"] = m_prefs.gwDriveIndex;
        j["gwFormatIndex"] = m_prefs.gwFormatIndex;
        j["gwVerify"] = m_prefs.gwVerify;
        j["gwPreErase"] = m_prefs.gwPreErase;
        j["gwEraseEmpty"] = m_prefs.gwEraseEmpty;
        j["gwReadVerify"] = m_prefs.gwReadVerify;
        j["openImagesWriteProtected"] = m_prefs.openImagesWriteProtected;
        // One object per slot, DF0 first. Replaces the single fbHardwareType/fbComPort/...
        // set that older files carry; those are still read (into their fbAssignedDrive
        // slot) but no longer written.
        j["fbSlots"] = nlohmann::json::array();
        for (const auto& s : m_prefs.fbSlots) {
            j["fbSlots"].push_back({{"hardwareType", s.hardwareType},
                                    {"comPort", s.comPort},
                                    {"bridgeMode", s.bridgeMode},
                                    {"bridgeDensity", s.bridgeDensity},
                                    {"driveCable", s.driveCable},
                                    {"autoCache", s.autoCache},
                                    {"smartSpeed", s.smartSpeed},
                                    {"allowWrites", s.allowWrites}});
        }
        j["fbAssignedDrive"] = m_prefs.fbAssignedDrive;
        j["showLiveFloppy"] = m_prefs.showLiveFloppy;
        j["controllerTypes"] = m_prefs.controllerTypes;
        j["controllerType"] = m_prefs.defaultControllerType; // Save for backward compatibility
        j["keyboardJoystick"] = m_prefs.keyboardJoystick;
        j["showBlock"] = m_prefs.showBlock;
        j["showBlockDetails"] = m_prefs.showBlockDetails;
        j["showDiskBitmap"] = m_prefs.showDiskBitmap;
        j["showBlockTypeMap"] = m_prefs.showBlockTypeMap;
        j["showNdosMap"] = m_prefs.showNdosMap;
        j["showBlockChangeMap"] = m_prefs.showBlockChangeMap;
        j["showIdentifyDisk"] = m_prefs.showIdentifyDisk;
        j["showLog"] = m_prefs.showLog;
        j["showEmulator"] = m_prefs.showEmulator;
        j["showDebugger"] = m_prefs.showDebugger;
        j["showMemoryMap"] = m_prefs.showMemoryMap;
        j["showDebugCpu"] = m_prefs.showDebugCpu;
        j["showDebugCustom"] = m_prefs.showDebugCustom;
        j["showDebugDisassembly"] = m_prefs.showDebugDisassembly;
        j["showDebugCopper"] = m_prefs.showDebugCopper;
        j["showDebugMemory"] = m_prefs.showDebugMemory;
        j["showDebugDisplay"] = m_prefs.showDebugDisplay;
        j["showDebugSprites"] = m_prefs.showDebugSprites;
        j["coreVerboseLog"] = m_prefs.coreVerboseLog;
        j["frameHistoryEnabled"] = m_prefs.frameHistoryEnabled;
        j["frameHistoryBudgetMB"] = m_prefs.frameHistoryBudgetMB;
        j["frameHistoryKeyframeInterval"] = m_prefs.frameHistoryKeyframeInterval;
        j["frameHistoryPersistInSession"] = m_prefs.frameHistoryPersistInSession;
        j["frameHistoryThumbnails"] = m_prefs.frameHistoryThumbnails;
        j["frameHistoryThumbnailInterval"] = m_prefs.frameHistoryThumbnailInterval;
        j["showDebugBitplanes"] = m_prefs.showDebugBitplanes;
        const auto& bo = m_prefs.blitOverlay;
        j["blitOverlay"] = {
            {"enabled", bo.enabled},         {"useProvenance", bo.useProvenance},
            {"outlineAll", bo.outlineAll},   {"opacity", bo.opacity},
            {"decayFrames", bo.decayFrames},
        };
        j["captureBlitDestData"] = m_prefs.captureBlitDestData;
        j["paletteVis"] = m_prefs.paletteVis;
        j["bplcon1Scroll"] = m_prefs.bplcon1Scroll;
        const auto& bp = m_prefs.blocks;
        j["blocks"] = {
            {"copperEnabled", bp.copperEnabled}, {"copperStart", bp.copperStart},
            {"copperEnd", bp.copperEnd},         {"bplptrMask", bp.bplptrMask},
            {"bplptrStart", bp.bplptrStart},     {"bplptrEnd", bp.bplptrEnd},
            {"blitterWrites", bp.blitterWrites},
        };
        const auto& rp = m_prefs.raster;
        j["raster"] = {
            {"enabled", rp.enabled},
            {"showCopper", rp.showCopper},
            {"showWaitSpans", rp.showWaitSpans},
            {"showCopperWanted", rp.showCopperWanted},
            {"showBlanking", rp.showBlanking},
            {"showGeometryEvents", rp.showGeometryEvents},
            {"channelMask", rp.channelMask},
            {"opacity", rp.opacity},
        };
        const auto& sp = m_prefs.source;
        j["source"] = {
            {"showInDisassembly", sp.showInDisassembly},
            {"detail", sp.detail},
            {"maxGapLines", sp.maxGapLines},
            {"tabWidth", sp.tabWidth},
            {"showFileHeaders", sp.showFileHeaders},
            {"showLineNumbers", sp.showLineNumbers},
            {"searchRoots", sp.searchRoots},
        };
        const auto& b = m_prefs.bitplanes;
        j["bitplanes"] = {
            {"sourceMode", b.sourceMode},
            {"manualBplCount", b.manualBplCount},
            {"manualWidth", b.manualWidth},
            {"manualHeight", b.manualHeight},
            {"manualModulo1", b.manualModulo1},
            {"manualModulo2", b.manualModulo2},
            {"useManualModulo1", b.useManualModulo1},
            {"useManualModulo2", b.useManualModulo2},
            {"interleavedAuto", b.interleavedAuto},
            {"interleavedForce", b.interleavedForce},
            {"useRegisterPointers", b.useRegisterPointers},
            {"manualPointers", b.manualPointers},
            {"useManualStride1", b.useManualStride1},
            {"useManualStride2", b.useManualStride2},
            {"manualStride1", b.manualStride1},
            {"manualStride2", b.manualStride2},
            {"overrideWidth", b.overrideWidth},
            {"overrideHeight", b.overrideHeight},
            {"applyPalette", b.applyPalette},
            {"showComposite", b.showComposite},
            {"zoom", b.zoom},
            {"lockAspect", b.lockAspect},
            {"fitToWindow", b.fitToWindow},
            {"showInspector", b.showInspector},
        };
        j["showDebugAudio"] = m_prefs.showDebugAudio;
        const auto& au = m_prefs.audio;
        j["audio"] = {
            {"channelMask", au.channelMask},
            {"filterMode", au.filterMode},
            {"filterModel", au.filterModel},
            {"timeBaseSamples", au.timeBaseSamples},
            {"yZoom", au.yZoom},
            {"showRegisters", au.showRegisters},
        };
        j["showBartmanBitmaps"] = m_prefs.showBartmanBitmaps;
        j["bartmanDebugEnabled"] = m_prefs.bartmanDebugEnabled;
        j["showDebugCia"] = m_prefs.showDebugCia;
        j["showDebugCallStack"] = m_prefs.showDebugCallStack;
        j["showDebugTrainer"] = m_prefs.showDebugTrainer;
        j["showDebugProfile"] = m_prefs.showDebugProfile;
        j["showDebugConsole"] = m_prefs.showDebugConsole;
        j["showBreakpoints"] = m_prefs.showBreakpoints;
        j["showAnnotations"] = m_prefs.showAnnotations;
        j["showBlitter"] = m_prefs.showBlitter;
        j["showRegisterOps"] = m_prefs.showRegisterOps;
        j["registerOpsSource"] = m_prefs.registerOpsSource;
        j["registerOpsChangedOnly"] = m_prefs.registerOpsChangedOnly;
        j["showDiskIo"] = m_prefs.showDiskIo;
        j["showExocet"] = m_prefs.showExocet;
        j["privateFeatures"] = m_prefs.privateFeatures;
        j["onboardingDone"] = m_prefs.onboardingDone;
        j["exocetPython"] = m_prefs.exocetPython;
        j["exocetTools"] = m_prefs.exocetTools;
        j["exocetTrustedKits"] = m_prefs.exocetTrustedKits;
        j["showBlitterMinterm"] = m_prefs.showBlitterMinterm;
        j["showDmaStats"] = m_prefs.showDmaStats;
        j["showFrameScrubber"] = m_prefs.showFrameScrubber;
        j["frameScrubberUnit"] = m_prefs.frameScrubberUnit;
        j["frameScrubberShowInstrNumbers"] = m_prefs.frameScrubberShowInstrNumbers;
        j["frameScrubberHeatMap"] = m_prefs.frameScrubberHeatMap;
        j["spritesScanlinesPanelW"] = m_prefs.spritesScanlinesPanelW;
        j["spritesDetailPanelH"] = m_prefs.spritesDetailPanelH;
        j["memMapLeftColumnW"] = m_prefs.memMapLeftColumnW;
        j["memMapPreviewH"] = m_prefs.memMapPreviewH;
        j["memoryBytesPerLine"] = m_prefs.memoryBytesPerLine;
        j["blockBytesPerLine"] = m_prefs.blockBytesPerLine;
        j["blockShowTrack"] = m_prefs.blockShowTrack;
        j["showPerfOverlay"] = m_prefs.showPerfOverlay;
        j["showPerfGraph"] = m_prefs.showPerfGraph;
        {
            const auto& f = m_prefs.displayFilters;
            j["displayFilters"] = {
                {"windowOpen", f.windowOpen},
                {"scanlines", f.scanlines},
                {"scanlineStrength", f.scanlineStrength},
                {"crt", f.crt},
                {"maskStrength", f.maskStrength},
                {"glowStrength", f.glowStrength},
                {"curvature", f.curvature},
                {"curveAmount", f.curveAmount},
                {"vignetteStrength", f.vignetteStrength},
                {"colorGrade", f.colorGrade},
                {"gamma", f.gamma},
                {"contrast", f.contrast},
                {"saturation", f.saturation},
                {"warmth", f.warmth},
            };
        }
        j["showTosecBrowser"] = m_prefs.showTosecBrowser;
        j["kickstartRomDir"] = m_prefs.kickstartRomDir;
        j["transitionDuration"] = m_prefs.transitionDuration;
        j["emulatorSettings"] = m_prefs.emulatorSettings;
        j["emulatorModel"] = m_prefs.emulatorModel;
        j["emulatorRunWhenHidden"] = m_prefs.emulatorRunWhenHidden;
        j["fonts"] = FontPrefsToJson(m_prefs.fonts);
        j["peekabooLaunchMode"] = m_prefs.peekabooLaunchMode;
        nlohmann::json mounts = nlohmann::json::array();
        for (const auto& mount : m_prefs.hostDirectoryMounts) {
            const auto directoryUtf8 = mount.hostDirectory.generic_u8string();
            const std::string directory(reinterpret_cast<const char*>(directoryUtf8.data()),
                                        directoryUtf8.size());
            nlohmann::json saved = {{"device", HostMountDeviceName(mount.device)},
                                    {"enabled", mount.enabled},
                                    {"directory", directory},
                                    {"label", mount.volumeLabel},
                                    {"readOnly", mount.readOnly},
                                    {"bootPriority", mount.bootPriority}};
            WriteHardfileFields(saved, mount);
            mounts.push_back(std::move(saved));
        }
        j["hostDirectoryMounts"] = std::move(mounts);
        j["hardDriveControllerRoms"] = ControllerRomsJson(m_prefs.hardDriveControllerRoms);
        j["cdImagePath"] = m_prefs.cdImagePath;
        j["serialPort"] = m_prefs.serialPort;
        j["parallelOutputPath"] = m_prefs.parallelOutputPath;
        j["midiOutDevice"] = m_prefs.midiOutDevice;
        j["midiInDevice"] = m_prefs.midiInDevice;
        j["emulatorVolume"] = m_prefs.emulatorVolume;
        j["videoCapture"] = {{"codec", m_prefs.videoCaptureCodec},
                             {"quality", m_prefs.videoCaptureQuality},
                             {"path", m_prefs.videoCapturePath},
                             {"ffmpegPath", m_prefs.ffmpegPath}};
        j["whdload"] = {{"buttonWait", m_prefs.whdload.buttonWait},
                        {"noWriteCache", m_prefs.whdload.noWriteCache},
                        {"prefsMode", m_prefs.whdload.prefsMode},
                        {"theme", m_prefs.whdload.theme}};
        j["hardDriveGroupsOpen"] = m_prefs.hardDriveGroupsOpen;
        j["mcpEnabled"] = m_prefs.mcpEnabled;
        j["mcpPort"] = m_prefs.mcpPort;
        j["mcpToken"] = m_prefs.mcpToken;
        j["gdbEnabled"] = m_prefs.gdbEnabled;
        j["gdbPort"] = m_prefs.gdbPort;
        j["showMcpActivity"] = m_prefs.showMcpActivity;
        j["focusedWindow"] = m_prefs.focusedWindow;
        j["activeWorkspace"] = m_prefs.activeWorkspace;
        j["screenProfile"] = m_prefs.screenProfile;
        {
            nlohmann::json profiles = nlohmann::json::object();
            for (const auto& [key, s] : m_prefs.screenProfiles) {
                profiles[key] = {{"fonts", FontPrefsToJson(s.fonts)},
                                 {"fontName", s.fontName},
                                 {"windowWidth", s.windowWidth},
                                 {"windowHeight", s.windowHeight},
                                 {"windowX", s.windowX},
                                 {"windowY", s.windowY},
                                 {"maximized", s.maximized},
                                 {"spritesScanlinesPanelW", s.spritesScanlinesPanelW},
                                 {"spritesDetailPanelH", s.spritesDetailPanelH},
                                 {"memMapLeftColumnW", s.memMapLeftColumnW},
                                 {"memMapPreviewH", s.memMapPreviewH},
                                 {"memoryBytesPerLine", s.memoryBytesPerLine}};
            }
            j["screenProfiles"] = profiles;
        }

        nlohmann::json panelsArr = nlohmann::json::array();
        for (const auto& pd : m_prefs.panels)
            panelsArr.push_back({{"name", pd.name}, {"id", pd.id}});
        j["panels"] = panelsArr;

        std::ofstream out(CONFIG_FILENAME);
        if (out.is_open()) {
            out << j.dump(4);
        }
    }

    void Load()
    {
        if (std::filesystem::exists(CONFIG_FILENAME)) {
            std::ifstream in(CONFIG_FILENAME);
            if (in.is_open()) {
                try {
                    nlohmann::json j;
                    in >> j;

                    jsonread::ReadIf(j, "fontName", m_prefs.fontName);
                    if (j.contains("recentFiles") && j["recentFiles"].is_array()) {
                        for (const auto& file : j["recentFiles"]) {
                            if (file.is_string()) {
                                m_prefs.recentFiles.push_back(file.get<std::string>());
                            }
                        }
                        // Limit size
                        if (m_prefs.recentFiles.size() > MAX_RECENT_FILES) {
                            m_prefs.recentFiles.resize(MAX_RECENT_FILES);
                        }
                    }
                    if (j.contains("recentDirectories") && j["recentDirectories"].is_array()) {
                        for (const auto& directory : j["recentDirectories"]) {
                            if (directory.is_string())
                                m_prefs.recentDirectories.push_back(directory.get<std::string>());
                        }
                        if (m_prefs.recentDirectories.size() > MAX_RECENT_FILES)
                            m_prefs.recentDirectories.resize(MAX_RECENT_FILES);
                    }

                    jsonread::ReadIf(j, "windowWidth", m_prefs.windowWidth);
                    jsonread::ReadIf(j, "windowHeight", m_prefs.windowHeight);
                    jsonread::ReadIf(j, "windowX", m_prefs.windowX);
                    jsonread::ReadIf(j, "windowY", m_prefs.windowY);
                    jsonread::ReadIf(j, "maximized", m_prefs.maximized);
                    jsonread::ReadIf(j, "gwDriveIndex", m_prefs.gwDriveIndex);
                    jsonread::ReadIf(j, "gwFormatIndex", m_prefs.gwFormatIndex);
                    jsonread::ReadIf(j, "gwVerify", m_prefs.gwVerify);
                    jsonread::ReadIf(j, "gwPreErase", m_prefs.gwPreErase);
                    jsonread::ReadIf(j, "gwEraseEmpty", m_prefs.gwEraseEmpty);
                    jsonread::ReadIf(j, "gwReadVerify", m_prefs.gwReadVerify);
                    jsonread::ReadIf(j, "openImagesWriteProtected",
                                     m_prefs.openImagesWriteProtected);
                    if (j.contains("fbAssignedDrive") && j["fbAssignedDrive"].is_number_integer()) {
                        const int slot = j["fbAssignedDrive"];
                        m_prefs.fbAssignedDrive = (slot >= 0 && slot <= 3) ? slot : 0;
                    }
                    if (j.contains("fbSlots") && j["fbSlots"].is_array()) {
                        size_t idx = 0;
                        for (const auto& node : j["fbSlots"]) {
                            if (idx >= m_prefs.fbSlots.size())
                                break;
                            if (node.is_object()) {
                                auto& s = m_prefs.fbSlots[idx];
                                jsonread::ReadIf(node, "hardwareType", s.hardwareType);
                                jsonread::ReadIf(node, "comPort", s.comPort);
                                jsonread::ReadIf(node, "bridgeMode", s.bridgeMode);
                                jsonread::ReadIf(node, "bridgeDensity", s.bridgeDensity);
                                jsonread::ReadIf(node, "driveCable", s.driveCable);
                                jsonread::ReadIf(node, "autoCache", s.autoCache);
                                jsonread::ReadIf(node, "smartSpeed", s.smartSpeed);
                                jsonread::ReadIf(node, "allowWrites", s.allowWrites);
                            }
                            ++idx;
                        }
                    } else if (j.contains("fbHardwareType") || j.contains("fbComPort")) {
                        // A file from before per-slot configuration: one set of settings and
                        // the slot they applied to. Land them in that slot.
                        auto& s = m_prefs.fbSlots[static_cast<size_t>(m_prefs.fbAssignedDrive)];
                        jsonread::ReadIf(j, "fbHardwareType", s.hardwareType);
                        jsonread::ReadIf(j, "fbComPort", s.comPort);
                        jsonread::ReadIf(j, "fbBridgeMode", s.bridgeMode);
                        jsonread::ReadIf(j, "fbBridgeDensity", s.bridgeDensity);
                        jsonread::ReadIf(j, "fbDriveCable", s.driveCable);
                        jsonread::ReadIf(j, "fbAutoCache", s.autoCache);
                        jsonread::ReadIf(j, "fbSmartSpeed", s.smartSpeed);
                    }
                    // indices into the Floppy Drives window's tables: clamp what the file says
                    for (auto& s : m_prefs.fbSlots) {
                        s.bridgeMode = std::clamp(s.bridgeMode, 0, kMaxBridgeMode);
                        s.bridgeDensity = std::clamp(s.bridgeDensity, 0, kMaxBridgeDensity);
                        s.driveCable = std::clamp(s.driveCable, 0, kMaxDriveCable);
                    }
                    jsonread::ReadIf(j, "showLiveFloppy", m_prefs.showLiveFloppy);
                    if (j.contains("controllerTypes") && j["controllerTypes"].is_object()) {
                        for (auto& [key, value] : j["controllerTypes"].items()) {
                            if (value.is_string()) {
                                m_prefs.controllerTypes[key] = value.get<std::string>();
                            }
                        }
                    } else if (j.contains("controllerType")) {
                        // Migration path: use old single setting for "Default"
                        jsonread::ReadIf(j, "controllerType", m_prefs.defaultControllerType);
                    }
                    jsonread::ReadIf(j, "keyboardJoystick", m_prefs.keyboardJoystick);
                    jsonread::ReadIf(j, "showBlock", m_prefs.showBlock);
                    jsonread::ReadIf(j, "showBlockDetails", m_prefs.showBlockDetails);
                    jsonread::ReadIf(j, "showDiskBitmap", m_prefs.showDiskBitmap);
                    jsonread::ReadIf(j, "showBlockTypeMap", m_prefs.showBlockTypeMap);
                    jsonread::ReadIf(j, "showNdosMap", m_prefs.showNdosMap);
                    jsonread::ReadIf(j, "showBlockChangeMap", m_prefs.showBlockChangeMap);
                    jsonread::ReadIf(j, "showIdentifyDisk", m_prefs.showIdentifyDisk);
                    jsonread::ReadIf(j, "showLog", m_prefs.showLog);
                    jsonread::ReadIf(j, "showEmulator", m_prefs.showEmulator);
                    jsonread::ReadIf(j, "showDebugger", m_prefs.showDebugger);
                    jsonread::ReadIf(j, "showMemoryMap", m_prefs.showMemoryMap);
                    jsonread::ReadIf(j, "showDebugCpu", m_prefs.showDebugCpu);
                    jsonread::ReadIf(j, "showDebugCustom", m_prefs.showDebugCustom);
                    jsonread::ReadIf(j, "showDebugDisassembly", m_prefs.showDebugDisassembly);
                    jsonread::ReadIf(j, "showDebugCopper", m_prefs.showDebugCopper);
                    jsonread::ReadIf(j, "showDebugMemory", m_prefs.showDebugMemory);
                    jsonread::ReadIf(j, "showDebugDisplay", m_prefs.showDebugDisplay);
                    jsonread::ReadIf(j, "showDebugSprites", m_prefs.showDebugSprites);
                    jsonread::ReadIf(j, "coreVerboseLog", m_prefs.coreVerboseLog);
                    jsonread::ReadIf(j, "frameHistoryEnabled", m_prefs.frameHistoryEnabled);
                    jsonread::ReadIf(j, "frameHistoryBudgetMB", m_prefs.frameHistoryBudgetMB);
                    jsonread::ReadIf(j, "frameHistoryKeyframeInterval",
                                     m_prefs.frameHistoryKeyframeInterval);
                    jsonread::ReadIf(j, "frameHistoryPersistInSession",
                                     m_prefs.frameHistoryPersistInSession);
                    jsonread::ReadIf(j, "frameHistoryThumbnails", m_prefs.frameHistoryThumbnails);
                    if (jsonread::ReadIf(j, "frameHistoryThumbnailInterval",
                                         m_prefs.frameHistoryThumbnailInterval))
                        m_prefs.frameHistoryThumbnailInterval =
                            std::max(1, m_prefs.frameHistoryThumbnailInterval);
                    jsonread::ReadIf(j, "showDebugBitplanes", m_prefs.showDebugBitplanes);
                    // Pre-merge sessions only had the provenance half of the overlay; carry it
                    // over as the master enable so an existing preferences file keeps working.
                    jsonread::ReadIf(j, "showBlitProvenance", m_prefs.blitOverlay.enabled);
                    if (j.contains("blitOverlay") && j["blitOverlay"].is_object()) {
                        const auto& b = j["blitOverlay"];
                        auto& bo = m_prefs.blitOverlay;
                        jsonread::ReadIf(b, "enabled", bo.enabled);
                        jsonread::ReadIf(b, "useProvenance", bo.useProvenance);
                        jsonread::ReadIf(b, "outlineAll", bo.outlineAll);
                        if (jsonread::ReadIf(b, "opacity", bo.opacity))
                            bo.opacity = std::clamp(bo.opacity, 0.0f, 1.0f);
                        // 30 is blitvis::kMaxDecayFrames, hardcoded rather than included: this
                        // header is pulled in almost everywhere and BlitVisRaster.h drags
                        // AmigaEmulator.h behind it. BlitterVisOverlay clamps again anyway.
                        if (jsonread::ReadIf(b, "decayFrames", bo.decayFrames))
                            bo.decayFrames = std::clamp(bo.decayFrames, 1, 30);
                    }
                    jsonread::ReadIf(j, "captureBlitDestData", m_prefs.captureBlitDestData);
                    jsonread::ReadIf(j, "paletteVis", m_prefs.paletteVis);
                    jsonread::ReadIf(j, "bplcon1Scroll", m_prefs.bplcon1Scroll);
                    if (j.contains("blocks") && j["blocks"].is_object()) {
                        const auto& b = j["blocks"];
                        auto& bp = m_prefs.blocks;
                        jsonread::ReadIf(b, "copperEnabled", bp.copperEnabled);
                        jsonread::ReadIf(b, "copperStart", bp.copperStart);
                        jsonread::ReadIf(b, "copperEnd", bp.copperEnd);
                        jsonread::ReadIf(b, "bplptrMask", bp.bplptrMask);
                        jsonread::ReadIf(b, "bplptrStart", bp.bplptrStart);
                        jsonread::ReadIf(b, "bplptrEnd", bp.bplptrEnd);
                        jsonread::ReadIf(b, "blitterWrites", bp.blitterWrites);
                    }
                    if (j.contains("source") && j["source"].is_object()) {
                        const auto& js = j["source"];
                        auto& sp = m_prefs.source;
                        jsonread::ReadIf(js, "showInDisassembly", sp.showInDisassembly);
                        jsonread::ReadIf(js, "detail", sp.detail);
                        jsonread::ReadIf(js, "maxGapLines", sp.maxGapLines);
                        jsonread::ReadIf(js, "tabWidth", sp.tabWidth);
                        jsonread::ReadIf(js, "showFileHeaders", sp.showFileHeaders);
                        jsonread::ReadIf(js, "showLineNumbers", sp.showLineNumbers);
                        // Defensive: a hand-edited prefs file with a non-array here would
                        // otherwise throw and reset every preference (see recentFiles).
                        if (js.contains("searchRoots") && js["searchRoots"].is_array()) {
                            sp.searchRoots.clear();
                            for (const auto& r : js["searchRoots"])
                                if (r.is_string())
                                    sp.searchRoots.push_back(r.get<std::string>());
                        }
                    }
                    if (j.contains("raster") && j["raster"].is_object()) {
                        const auto& r = j["raster"];
                        auto& rp = m_prefs.raster;
                        jsonread::ReadIf(r, "enabled", rp.enabled);
                        jsonread::ReadIf(r, "showCopper", rp.showCopper);
                        // Migration: showDma used to gate the whole DMA layer independently
                        // of the channel mask. It is gone, so a stored "off" has to be carried
                        // over as an empty mask or every channel would switch itself on.
                        if (r.contains("showDma") && r["showDma"].is_boolean() &&
                            !r["showDma"].get<bool>())
                            rp.channelMask = 0;
                        jsonread::ReadIf(r, "showWaitSpans", rp.showWaitSpans);
                        jsonread::ReadIf(r, "showCopperWanted", rp.showCopperWanted);
                        jsonread::ReadIf(r, "showBlanking", rp.showBlanking);
                        jsonread::ReadIf(r, "showGeometryEvents", rp.showGeometryEvents);
                        jsonread::ReadIf(r, "channelMask", rp.channelMask);
                        jsonread::ReadIf(r, "opacity", rp.opacity);
                    }
                    if (j.contains("bitplanes") && j["bitplanes"].is_object()) {
                        const auto& jb = j["bitplanes"];
                        auto& b = m_prefs.bitplanes;
                        jsonread::ReadIf(jb, "sourceMode", b.sourceMode);
                        jsonread::ReadIf(jb, "manualBplCount", b.manualBplCount);
                        jsonread::ReadIf(jb, "manualWidth", b.manualWidth);
                        jsonread::ReadIf(jb, "manualHeight", b.manualHeight);
                        jsonread::ReadIf(jb, "manualModulo1", b.manualModulo1);
                        jsonread::ReadIf(jb, "manualModulo2", b.manualModulo2);
                        jsonread::ReadIf(jb, "useManualModulo1", b.useManualModulo1);
                        jsonread::ReadIf(jb, "useManualModulo2", b.useManualModulo2);
                        jsonread::ReadIf(jb, "interleavedAuto", b.interleavedAuto);
                        jsonread::ReadIf(jb, "interleavedForce", b.interleavedForce);
                        jsonread::ReadIf(jb, "useRegisterPointers", b.useRegisterPointers);
                        if (jb.contains("manualPointers") && jb["manualPointers"].is_array()) {
                            const auto count =
                                std::min(b.manualPointers.size(), jb["manualPointers"].size());
                            for (size_t i = 0; i < count; ++i) {
                                if (jb["manualPointers"][i].is_number_unsigned())
                                    b.manualPointers[i] = jb["manualPointers"][i].get<uint32_t>();
                            }
                        }
                        jsonread::ReadIf(jb, "useManualStride1", b.useManualStride1);
                        jsonread::ReadIf(jb, "useManualStride2", b.useManualStride2);
                        jsonread::ReadIf(jb, "manualStride1", b.manualStride1);
                        jsonread::ReadIf(jb, "manualStride2", b.manualStride2);
                        jsonread::ReadIf(jb, "overrideWidth", b.overrideWidth);
                        jsonread::ReadIf(jb, "overrideHeight", b.overrideHeight);
                        jsonread::ReadIf(jb, "applyPalette", b.applyPalette);
                        jsonread::ReadIf(jb, "showComposite", b.showComposite);
                        jsonread::ReadIf(jb, "zoom", b.zoom);
                        jsonread::ReadIf(jb, "lockAspect", b.lockAspect);
                        jsonread::ReadIf(jb, "fitToWindow", b.fitToWindow);
                        jsonread::ReadIf(jb, "showInspector", b.showInspector);
                    }
                    jsonread::ReadIf(j, "showDebugAudio", m_prefs.showDebugAudio);
                    if (j.contains("audio") && j["audio"].is_object()) {
                        const auto& ja = j["audio"];
                        auto& au = m_prefs.audio;
                        au.channelMask = static_cast<uint8_t>(
                            jsonread::ReadOr<int>(ja, "channelMask", au.channelMask) & 0x0F);
                        jsonread::ReadIf(ja, "filterMode", au.filterMode);
                        jsonread::ReadIf(ja, "filterModel", au.filterModel);
                        jsonread::ReadIf(ja, "timeBaseSamples", au.timeBaseSamples);
                        jsonread::ReadIf(ja, "yZoom", au.yZoom);
                        jsonread::ReadIf(ja, "showRegisters", au.showRegisters);
                    }
                    jsonread::ReadIf(j, "showBartmanBitmaps", m_prefs.showBartmanBitmaps);
                    jsonread::ReadIf(j, "bartmanDebugEnabled", m_prefs.bartmanDebugEnabled);
                    jsonread::ReadIf(j, "showDebugCia", m_prefs.showDebugCia);
                    jsonread::ReadIf(j, "showDebugCallStack", m_prefs.showDebugCallStack);
                    jsonread::ReadIf(j, "showDebugTrainer", m_prefs.showDebugTrainer);
                    jsonread::ReadIf(j, "showDebugProfile", m_prefs.showDebugProfile);
                    jsonread::ReadIf(j, "showDebugConsole", m_prefs.showDebugConsole);
                    jsonread::ReadIf(j, "showBreakpoints", m_prefs.showBreakpoints);
                    jsonread::ReadIf(j, "showAnnotations", m_prefs.showAnnotations);
                    jsonread::ReadIf(j, "showBlitter", m_prefs.showBlitter);
                    jsonread::ReadIf(j, "showRegisterOps", m_prefs.showRegisterOps);
                    jsonread::ReadIf(j, "registerOpsSource", m_prefs.registerOpsSource);
                    jsonread::ReadIf(j, "registerOpsChangedOnly", m_prefs.registerOpsChangedOnly);
                    jsonread::ReadIf(j, "showDiskIo", m_prefs.showDiskIo);
                    jsonread::ReadIf(j, "showExocet", m_prefs.showExocet);
                    jsonread::ReadIf(j, "privateFeatures", m_prefs.privateFeatures);
                    jsonread::ReadIf(j, "onboardingDone", m_prefs.onboardingDone);
                    if (j.contains("exocetPython") && j["exocetPython"].is_string())
                        m_prefs.exocetPython = j["exocetPython"];
                    if (j.contains("exocetTools") && j["exocetTools"].is_string())
                        m_prefs.exocetTools = j["exocetTools"];
                    // Defensive, as searchRoots: a non-array must not reset every preference.
                    if (j.contains("exocetTrustedKits") && j["exocetTrustedKits"].is_array()) {
                        m_prefs.exocetTrustedKits.clear();
                        for (const auto& h : j["exocetTrustedKits"])
                            if (h.is_string())
                                m_prefs.exocetTrustedKits.push_back(h.get<std::string>());
                    }
                    jsonread::ReadIf(j, "showBlitterMinterm", m_prefs.showBlitterMinterm);
                    jsonread::ReadIf(j, "showDmaStats", m_prefs.showDmaStats);
                    jsonread::ReadIf(j, "showFrameScrubber", m_prefs.showFrameScrubber);
                    jsonread::ReadIf(j, "frameScrubberUnit", m_prefs.frameScrubberUnit);
                    jsonread::ReadIf(j, "frameScrubberShowInstrNumbers",
                                     m_prefs.frameScrubberShowInstrNumbers);
                    jsonread::ReadIf(j, "frameScrubberHeatMap", m_prefs.frameScrubberHeatMap);
                    jsonread::ReadIf(j, "spritesScanlinesPanelW", m_prefs.spritesScanlinesPanelW);
                    jsonread::ReadIf(j, "spritesDetailPanelH", m_prefs.spritesDetailPanelH);
                    jsonread::ReadIf(j, "memMapLeftColumnW", m_prefs.memMapLeftColumnW);
                    jsonread::ReadIf(j, "memMapPreviewH", m_prefs.memMapPreviewH);
                    jsonread::ReadIf(j, "memoryBytesPerLine", m_prefs.memoryBytesPerLine);
                    jsonread::ReadIf(j, "blockBytesPerLine", m_prefs.blockBytesPerLine);
                    jsonread::ReadIf(j, "blockShowTrack", m_prefs.blockShowTrack);
                    jsonread::ReadIf(j, "showPerfOverlay", m_prefs.showPerfOverlay);
                    jsonread::ReadIf(j, "showPerfGraph", m_prefs.showPerfGraph);
                    if (j.contains("displayFilters")) {
                        const auto& jf = j["displayFilters"];
                        auto& f = m_prefs.displayFilters;
                        auto getB = [&](const char* k, bool& v) { jsonread::ReadIf(jf, k, v); };
                        auto getF = [&](const char* k, float& v) { jsonread::ReadIf(jf, k, v); };
                        getB("windowOpen", f.windowOpen);
                        getB("scanlines", f.scanlines);
                        getF("scanlineStrength", f.scanlineStrength);
                        getB("crt", f.crt);
                        getF("maskStrength", f.maskStrength);
                        getF("glowStrength", f.glowStrength);
                        getB("curvature", f.curvature);
                        getF("curveAmount", f.curveAmount);
                        getF("vignetteStrength", f.vignetteStrength);
                        getB("colorGrade", f.colorGrade);
                        getF("gamma", f.gamma);
                        getF("contrast", f.contrast);
                        getF("saturation", f.saturation);
                        getF("warmth", f.warmth);
                    }
                    jsonread::ReadIf(j, "showTosecBrowser", m_prefs.showTosecBrowser);
                    jsonread::ReadIf(j, "kickstartRomDir", m_prefs.kickstartRomDir);
                    jsonread::ReadIf(j, "transitionDuration", m_prefs.transitionDuration);
                    if (j.contains("emulatorSettings") && j["emulatorSettings"].is_object()) {
                        for (auto& [k, v] : j["emulatorSettings"].items()) {
                            if (v.is_string())
                                m_prefs.emulatorSettings[k] = v.get<std::string>();
                        }
                        MigrateLegacyFloppySound(m_prefs.emulatorSettings);
                        DropOptionsTheCoreDoesNotHave(m_prefs.emulatorSettings);
                    }
                    if (j.contains("emulatorModel") && j["emulatorModel"].is_string())
                        m_prefs.emulatorModel = j["emulatorModel"].get<std::string>();
                    jsonread::ReadIf(j, "emulatorRunWhenHidden", m_prefs.emulatorRunWhenHidden);
                    if (j.contains("fonts") && j["fonts"].is_object())
                        m_prefs.fonts = FontPrefsFromJson(j["fonts"], m_prefs.fonts);
                    if (j.contains("peekabooLaunchMode") && j["peekabooLaunchMode"].is_string()) {
                        const std::string mode = j["peekabooLaunchMode"].get<std::string>();
                        if (mode == "automatic" || mode == "host_directory" ||
                            mode == "bootable_adf") {
                            m_prefs.peekabooLaunchMode = mode;
                        }
                    }
                    if (j.contains("hardDriveControllerRoms"))
                        m_prefs.hardDriveControllerRoms =
                            ControllerRomsFromJson(j["hardDriveControllerRoms"]);
                    if (j.contains("cdImagePath") && j["cdImagePath"].is_string())
                        m_prefs.cdImagePath = j["cdImagePath"].get<std::string>();
                    if (j.contains("serialPort") && j["serialPort"].is_string())
                        m_prefs.serialPort = j["serialPort"].get<std::string>();
                    if (j.contains("parallelOutputPath") && j["parallelOutputPath"].is_string())
                        m_prefs.parallelOutputPath = j["parallelOutputPath"].get<std::string>();
                    if (j.contains("midiOutDevice") && j["midiOutDevice"].is_number_integer())
                        m_prefs.midiOutDevice = j["midiOutDevice"].get<int>();
                    if (j.contains("midiInDevice") && j["midiInDevice"].is_number_integer())
                        m_prefs.midiInDevice = j["midiInDevice"].get<int>();
                    if (j.contains("emulatorVolume") && j["emulatorVolume"].is_number_integer())
                        m_prefs.emulatorVolume = std::clamp(j["emulatorVolume"].get<int>(), 0, 100);
                    if (j.contains("videoCapture") && j["videoCapture"].is_object()) {
                        const auto& v = j["videoCapture"];
                        jsonread::ReadIf(v, "codec", m_prefs.videoCaptureCodec);
                        m_prefs.videoCaptureQuality = std::clamp(
                            jsonread::ReadOr(v, "quality", m_prefs.videoCaptureQuality), -1, 51);
                        jsonread::ReadIf(v, "path", m_prefs.videoCapturePath);
                        jsonread::ReadIf(v, "ffmpegPath", m_prefs.ffmpegPath);
                    }
                    if (j.contains("whdload") && j["whdload"].is_object()) {
                        const auto& w = j["whdload"];
                        jsonread::ReadIf(w, "buttonWait", m_prefs.whdload.buttonWait);
                        jsonread::ReadIf(w, "noWriteCache", m_prefs.whdload.noWriteCache);
                        m_prefs.whdload.prefsMode = std::clamp(
                            jsonread::ReadOr(w, "prefsMode", m_prefs.whdload.prefsMode), 0, 3);
                        m_prefs.whdload.theme =
                            std::clamp(jsonread::ReadOr(w, "theme", m_prefs.whdload.theme), 0, 1);
                    }
                    if (j.contains("hostDirectoryMounts") && j["hostDirectoryMounts"].is_array()) {
                        const auto& savedMounts = j["hostDirectoryMounts"];
                        for (size_t index = 0; index < m_prefs.hostDirectoryMounts.size() &&
                                               index < savedMounts.size();
                             ++index) {
                            const auto& saved = savedMounts[index];
                            if (!saved.is_object())
                                continue;
                            auto mount = m_prefs.hostDirectoryMounts[index];
                            mount.device = static_cast<HostMountDevice>(index);
                            if (saved.contains("enabled") && saved["enabled"].is_boolean())
                                mount.enabled = saved["enabled"].get<bool>();
                            if (saved.contains("directory") && saved["directory"].is_string()) {
                                const auto directory = saved["directory"].get<std::string>();
                                mount.hostDirectory = std::filesystem::path(std::u8string(
                                    reinterpret_cast<const char8_t*>(directory.data()),
                                    directory.size()));
                            }
                            if (saved.contains("label") && saved["label"].is_string())
                                mount.volumeLabel = saved["label"].get<std::string>();
                            if (saved.contains("readOnly") && saved["readOnly"].is_boolean())
                                mount.readOnly = saved["readOnly"].get<bool>();
                            if (saved.contains("bootPriority") &&
                                saved["bootPriority"].is_number_integer())
                                mount.bootPriority = saved["bootPriority"].get<int>();
                            ReadHardfileFields(saved, mount);

                            if (mount.enabled) {
                                std::array<HostDirectoryMount, 2> single{};
                                single[index] = mount;
                                single[index].device = static_cast<HostMountDevice>(index);
                                single[1 - index].device = static_cast<HostMountDevice>(1 - index);
                                std::string ignored;
                                if (!UaeConfigBuilder::ValidateMounts(single, ignored, false))
                                    mount.enabled = false;
                                else
                                    mount = single[index];
                            }
                            m_prefs.hostDirectoryMounts[index] = std::move(mount);
                        }

                        auto validated = m_prefs.hostDirectoryMounts;
                        std::string ignored;
                        if (UaeConfigBuilder::ValidateMounts(validated, ignored, false)) {
                            m_prefs.hostDirectoryMounts = std::move(validated);
                        } else {
                            // Individually valid entries can still conflict by canonical path or
                            // volume label. Preserve DH0 and disable the later conflicting slot.
                            m_prefs.hostDirectoryMounts[1].enabled = false;
                            validated = m_prefs.hostDirectoryMounts;
                            if (UaeConfigBuilder::ValidateMounts(validated, ignored, false))
                                m_prefs.hostDirectoryMounts = std::move(validated);
                            else
                                m_prefs.hostDirectoryMounts[0].enabled = false;
                        }
                    }
                    if (j.contains("hardDriveGroupsOpen") && j["hardDriveGroupsOpen"].is_array()) {
                        const auto& groups = j["hardDriveGroupsOpen"];
                        const auto count =
                            std::min(m_prefs.hardDriveGroupsOpen.size(), groups.size());
                        for (size_t index = 0; index < count; ++index) {
                            if (groups[index].is_boolean())
                                m_prefs.hardDriveGroupsOpen[index] = groups[index].get<bool>();
                        }
                    }
                    jsonread::ReadIf(j, "gdbEnabled", m_prefs.gdbEnabled);
                    jsonread::ReadIf(j, "gdbPort", m_prefs.gdbPort);
                    jsonread::ReadIf(j, "mcpEnabled", m_prefs.mcpEnabled);
                    jsonread::ReadIf(j, "mcpPort", m_prefs.mcpPort);
                    if (j.contains("mcpToken") && j["mcpToken"].is_string())
                        m_prefs.mcpToken = j["mcpToken"].get<std::string>();
                    jsonread::ReadIf(j, "showMcpActivity", m_prefs.showMcpActivity);
                    if (j.contains("focusedWindow") && j["focusedWindow"].is_string())
                        m_prefs.focusedWindow = j["focusedWindow"].get<std::string>();
                    if (j.contains("activeWorkspace") && j["activeWorkspace"].is_string())
                        m_prefs.activeWorkspace = j["activeWorkspace"].get<std::string>();
                    if (j.contains("screenProfile") && j["screenProfile"].is_string())
                        m_prefs.screenProfile = j["screenProfile"].get<std::string>();
                    if (j.contains("screenProfiles") && j["screenProfiles"].is_object()) {
                        for (const auto& [key, js] : j["screenProfiles"].items()) {
                            if (!js.is_object() || key == m_prefs.screenProfile)
                                continue;
                            ScreenProfilePrefs s;
                            if (js.contains("fonts"))
                                s.fonts = FontPrefsFromJson(js["fonts"], FontPrefs{});
                            if (js.contains("fontName") && js["fontName"].is_string())
                                s.fontName = js["fontName"].get<std::string>();
                            const auto readInt = [&js](const char* k, int& v) {
                                if (js.contains(k) && js[k].is_number_integer())
                                    v = js[k].get<int>();
                            };
                            const auto readFloat = [&js](const char* k, float& v) {
                                if (js.contains(k) && js[k].is_number())
                                    v = js[k].get<float>();
                            };
                            readInt("windowWidth", s.windowWidth);
                            readInt("windowHeight", s.windowHeight);
                            readInt("windowX", s.windowX);
                            readInt("windowY", s.windowY);
                            if (js.contains("maximized") && js["maximized"].is_boolean())
                                s.maximized = js["maximized"].get<bool>();
                            readFloat("spritesScanlinesPanelW", s.spritesScanlinesPanelW);
                            readFloat("spritesDetailPanelH", s.spritesDetailPanelH);
                            readFloat("memMapLeftColumnW", s.memMapLeftColumnW);
                            readFloat("memMapPreviewH", s.memMapPreviewH);
                            readInt("memoryBytesPerLine", s.memoryBytesPerLine);
                            m_prefs.screenProfiles[key] = s;
                        }
                    }
                    if (j.contains("panels") && j["panels"].is_array()) {
                        for (const auto& pd : j["panels"]) {
                            PanelDef def;
                            if (pd.contains("name") && pd["name"].is_string())
                                def.name = pd["name"].get<std::string>();
                            if (pd.contains("id") && pd["id"].is_number_unsigned())
                                def.id = pd["id"].get<uint32_t>();
                            if (!def.name.empty() && def.id != 0)
                                m_prefs.panels.push_back(def);
                        }
                    }

                } catch (...) {
                    // Unreadable as a whole (not JSON, or a read nothing type-checks): start
                    // fresh, but keep the file as user_prefs.json.bad - the next Save() would
                    // otherwise make the loss permanent.
                    m_prefs = Preferences();
                    std::error_code ec;
                    std::filesystem::copy_file(CONFIG_FILENAME, CONFIG_FILENAME + ".bad",
                                               std::filesystem::copy_options::overwrite_existing,
                                               ec);
                }
            }
        }
    }
};

} // namespace adm::core
