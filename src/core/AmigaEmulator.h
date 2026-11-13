#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CpuExtState.h"
#include "HostMountService.h"
#include "HostPorts.h"
#include "MachineConfig.h"
#include "VideoCaptureSink.h"
#include "WasapiPushOutput.h"

#include "AmigaCustom.h"
#include "AudioCushion.h"
#include "AudioRecorder.h"
#include "CrassMemMap.h"
#include "CrassProfile.h"
#include "DisplayBlocks.h"
#include "DisplayMasks.h"
#include "ExocetBlock.h"
#include "FrameDigest.h"
#include "FrameInput.h"
#include "KprintfBreakpoints.h"

// Forward declare ImGui/OpenGL types to avoid heavy include dependencies in header
typedef unsigned int GLuint;
struct ma_device;

namespace adm::core {
// KickstartRoms.h, forward-declared: it names AmigaEmulator, so including it here would be a
// cycle. IdentifyRom only takes it by reference.
struct RomIdentity;
} // namespace adm::core

namespace adm {

// Minimal Libretro definitions to avoid external dependency
// These must match the ABI of libretro.h
#define RETRO_API_VERSION 1

struct retro_game_info
{
    const char* path;
    const void* data;
    size_t size;
    const char* meta;
};

struct retro_system_info
{
    const char* library_name;
    const char* library_version;
    const char* valid_extensions;
    bool need_fullpath;
    bool block_extract;
};

struct retro_system_av_info
{
    struct
    {
        unsigned width;
        unsigned height;
        unsigned max_width;
        unsigned max_height;
        float aspect_ratio;
    } geometry;
    struct
    {
        double fps;
        double sample_rate;
    } timing;
};

enum retro_pixel_format {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,
    RETRO_PIXEL_FORMAT_RGB565 = 2,
    RETRO_PIXEL_FORMAT_UNKNOWN = 3
};

// Input devices
#define RETRO_DEVICE_NONE     0
#define RETRO_DEVICE_JOYPAD   1
#define RETRO_DEVICE_MOUSE    2
#define RETRO_DEVICE_KEYBOARD 3
#define RETRO_DEVICE_LIGHTGUN 4
#define RETRO_DEVICE_ANALOG   5
#define RETRO_DEVICE_POINTER  6

// Device subclass macro (matches libretro.h)
#define RETRO_DEVICE_SUBCLASS(base, id) (((id + 1) << 8) | base)

// Input device IDs - Mouse
#define RETRO_DEVICE_ID_MOUSE_X               0
#define RETRO_DEVICE_ID_MOUSE_Y               1
#define RETRO_DEVICE_ID_MOUSE_LEFT            2
#define RETRO_DEVICE_ID_MOUSE_RIGHT           3
#define RETRO_DEVICE_ID_MOUSE_WHEELUP         4
#define RETRO_DEVICE_ID_MOUSE_WHEELDOWN       5
#define RETRO_DEVICE_ID_MOUSE_MIDDLE          6
#define RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP   7
#define RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN 8
#define RETRO_DEVICE_ID_MOUSE_BUTTON_4        9
#define RETRO_DEVICE_ID_MOUSE_BUTTON_5        10

// Input device IDs - Joypad
#define RETRO_DEVICE_ID_JOYPAD_B      0
#define RETRO_DEVICE_ID_JOYPAD_Y      1
#define RETRO_DEVICE_ID_JOYPAD_SELECT 2
#define RETRO_DEVICE_ID_JOYPAD_START  3
#define RETRO_DEVICE_ID_JOYPAD_UP     4
#define RETRO_DEVICE_ID_JOYPAD_DOWN   5
#define RETRO_DEVICE_ID_JOYPAD_LEFT   6
#define RETRO_DEVICE_ID_JOYPAD_RIGHT  7
#define RETRO_DEVICE_ID_JOYPAD_A      8
#define RETRO_DEVICE_ID_JOYPAD_X      9
#define RETRO_DEVICE_ID_JOYPAD_L      10
#define RETRO_DEVICE_ID_JOYPAD_R      11
#define RETRO_DEVICE_ID_JOYPAD_L2     12
#define RETRO_DEVICE_ID_JOYPAD_R2     13
#define RETRO_DEVICE_ID_JOYPAD_L3     14
#define RETRO_DEVICE_ID_JOYPAD_R3     15

// Libretro keyboard callback
typedef void (*retro_keyboard_event_t)(bool down, unsigned keycode, uint32_t character,
                                       uint16_t key_modifiers);

// Libretro callback types
typedef void (*retro_set_environment_t)(bool (*cb)(unsigned cmd, void* data));
typedef void (*retro_set_video_refresh_t)(const void* data, unsigned width, unsigned height,
                                          size_t pitch);
typedef void (*retro_set_audio_sample_t)(int16_t left, int16_t right);
typedef size_t (*retro_set_audio_sample_batch_t)(const int16_t* data, size_t frames);
typedef void (*retro_set_input_poll_t)(void (*cb)());
typedef void (*retro_set_input_state_t)(int16_t (*cb)(unsigned port, unsigned device,
                                                      unsigned index, unsigned id));

typedef void (*retro_init_t)();
typedef void (*retro_deinit_t)();
typedef unsigned (*retro_api_version_t)();
typedef void (*retro_get_system_info_t)(struct retro_system_info* info);
typedef void (*retro_get_system_av_info_t)(struct retro_system_av_info* info);
typedef void (*retro_set_controller_port_device_t)(unsigned port, unsigned device);
typedef void (*retro_reset_t)();
typedef void (*retro_run_t)();
typedef size_t (*retro_serialize_size_t)();
typedef bool (*retro_serialize_t)(void* data, size_t size);
typedef bool (*retro_unserialize_t)(const void* data, size_t size);
typedef bool (*retro_cheat_reset_t)();
typedef void (*retro_cheat_set_t)(unsigned index, bool enabled, const char* code);
typedef bool (*retro_load_game_t)(const struct retro_game_info* game);
typedef bool (*retro_load_game_special_t)(unsigned game_type, const struct retro_game_info* info,
                                          size_t num_info);
typedef void (*retro_unload_game_t)();
typedef unsigned (*retro_get_region_t)();
typedef void* (*retro_get_memory_data_t)(unsigned id);
typedef size_t (*retro_get_memory_size_t)(unsigned id);

// Hardware profiles for Amiga
enum class AmigaModel {
    A500OG,   // Amiga 500 (v1.3, OCS, 0.5MB Chip)
    A500,     // Amiga 500 (v1.3, OCS, 0.5MB Chip + 0.5MB Slow)
    A500Plus, // Amiga 500+ (v2.04, ECS, 1MB Chip)
    A600,     // Amiga 600 (v2.05, ECS, 2MB Chip + 8MB Fast)
    A1200OG,  // Amiga 1200 (v3.1, AGA, 2MB Chip)
    A1200,    // Amiga 1200 (v3.1, AGA, 2MB Chip + 8MB Fast)
    A2000OG,  // Amiga 2000 (v1.3, OCS, 0.5MB Chip + 0.5MB Slow)
    A2000,    // Amiga 2000 (v2.04, ECS, 1MB Chip)
    A4030,    // Amiga 4000/030 (v3.1, AGA, 2MB Chip + 8MB Fast)
    A4040,    // Amiga 4000/040 (v3.1, AGA, 2MB Chip + 8MB Fast)
    A4040RTG, // As A4040, plus an 8MB Zorro III uaegfx board (core 1.99.0)
    CDTV,     // Amiga CDTV (v1.3+ext, ECS, 1MB Chip)
    CD32,     // Amiga CD32 (v3.1+ext, AGA, 2MB Chip)
    CD32FR    // Amiga CD32 (v3.1+ext, AGA, 2MB Chip + 8MB Fast)
};

// Display information for each model preset
struct AmigaModelInfo
{
    AmigaModel model;
    const char* id;          // Stable PEEKaboo model ID used by menus and saved sessions
    const char* coreId;      // PuAE model string
    const char* displayName; // Human-readable menu label
    const char* chipMemory;  // PuAE chip_memory value
    const char* slowMemory;  // PuAE slow_memory value
    const char* fastMemory;  // PuAE fast_memory value
};

const AmigaModelInfo& GetAmigaModelInfo(AmigaModel model);
const std::vector<AmigaModelInfo>& GetAllAmigaModels();

// Controller type for port 1 (joystick port)
enum class ControllerType {
    StandardJoystick, // RETRO_DEVICE_JOYPAD (2-button joystick)
    CD32Controller    // RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1) (7-button CD32 pad)
};

class AmigaEmulator
{
public:
    AmigaEmulator();
    ~AmigaEmulator();

    // Creates a bootable ADF disk image containing the given executable and a startup-sequence
    static std::vector<uint8_t> CreateBootableADF(const std::vector<uint8_t>& exeData,
                                                  const std::string& filename);

    // Default platform-specific path to the bundled PuAE core.
    static std::string GetDefaultCorePath();

    // Copies the default core to a unique temp file and returns its path. Loading a *distinct*
    // dylib file gives the second emulator instance its own copy of the core's C globals, which
    // is what makes running two independent emulators in one process possible. Returns "" on error.
    static std::string DuplicateCoreToTemp();

    // Lightweight probe (dlopen + symbol check + dlclose, no retro_init) that verifies a core
    // file is our patched PuAE fork — i.e. it exports the adm_* symbols the rest of the code
    // dereferences unconditionally. Returns false and fills outError if the file is the wrong
    // build. Used at startup to fail fast instead of crashing later on a missing symbol.
    static bool VerifyCore(const std::string& corePath, std::string& outError);

    // Returns the version string from adm_get_version without a full retro_init.
    // Returns "" if the core file cannot be opened or is missing the symbol.
    static std::string ProbeVersion(const std::string& corePath);

    // Core Management
    bool LoadCore(const std::string& corePath);
    void UnloadCore();

    /**
     * @brief Reset the core's internal state by cycling retro_deinit()/retro_init()
     *        on the already-loaded dylib (no dlclose/dlopen, so no fd/dylib churn).
     *
     * The patched PuAE core accumulates internal state across repeated load_game
     * cycles and wedges after ~9 reloads; a full deinit/init clears that without the
     * file-descriptor leak that recreating the dylib causes on macOS. The host audio
     * device is intentionally left running (it is independent of the core), so the
     * next LoadADF reuses it via InitAudio()'s idempotent guard.
     * @return true if the core was reinitialised.
     */
    bool ReinitCore();
    bool IsCoreLoaded() const { return m_coreLoaded; }
    bool IsGameLoaded() const { return m_gameLoaded; }
    const std::string& GetCoreVersion() const { return m_coreVersionString; }

    // Configuration
    void SetSystemDirectory(const std::string& path);
    const std::string& GetSaveDirectory() const { return m_saveDir; }
    const std::string& GetSystemDirectory() const { return m_systemDir; }
    void SetModel(AmigaModel model);
    // Select a complete built-in machine configuration. Unlike SetModel(), this also replaces
    // model-owned user overrides (Kickstart, CPU/FPU and RAM) so stale Preferences values cannot
    // silently turn an A500 selection into a different machine.
    // Reinitialises an already-loaded core before returning so the next load cannot reuse the
    // previous machine's cached PuAE options.
    bool SelectModelPreset(AmigaModel model);
    AmigaModel GetCurrentModel() const { return m_currentModel; }

    // User-configurable variable overrides.
    // These are checked before all hardcoded defaults in the environment callback,
    // giving the user full control over any PuAE core variable.
    void SetUserVariable(const std::string& key, const std::string& value);
    void ClearUserVariable(const std::string& key);
    void SetUserVariables(const std::unordered_map<std::string, std::string>& vars);
    const std::unordered_map<std::string, std::string>& GetUserVariables() const
    {
        return m_userVariables;
    }
    void ClearAllUserVariables();

    /// The value the core is handed for `key`, applying the same precedence the environment
    /// callback does: user override, then the machine-derived specials, then Peekaboo's default
    /// from EmulatorOptions.h, then the core's own declared default. Empty when nothing supplies
    /// it - which is exactly when RETRO_ENVIRONMENT_GET_VARIABLE answers false.
    std::string ResolveCoreVariable(const std::string& key) const;

    /// What the loaded core declared for `key` in SET_VARIABLES: its own default and the full set
    /// of values it will parse. Empty until a core has been loaded.
    struct CoreOptionDeclaration
    {
        std::string defaultValue;
        std::vector<std::string> values;
    };
    const std::unordered_map<std::string, CoreOptionDeclaration>& GetCoreOptionDeclarations() const
    {
        return m_coreOptionDeclarations;
    }

    // UAE config file loading (.uae format from ./uae/configs/)
    bool LoadUaeConfig(const std::string& configPath);
    void ClearUaeConfig();
    bool HasUaeConfig() const { return !m_uaeConfigPath.empty(); }
    std::string GetUaeConfigName() const;

    // Execution
    bool StartEmpty(); // Boot to Kickstart screen
    // Load a disk image. fileExt sets the temp-file extension PuAE inspects;
    // pass "ipf" or "scp" to feed a raw preservation image to the core's native
    // CAPS / SuperCard Pro flux readers (preserves copy protection), "adf" for
    // decoded sector images.
    bool LoadADF(const std::vector<uint8_t>& adfData, const std::string& fileExt = "adf");
    bool RunExecutable(const std::vector<uint8_t>& exeData,
                       const std::string& filename); // "Instant Run"
    // Fast local launch: boot a tiny host-directory DH0 and expose the executable's build
    // directory read-only as DH1. The HUNK is executed in place and is never copied into an ADF.
    bool RunHostExecutable(const std::string& programPath, std::string& outError);
    bool StartHostMountProfile(const core::EmulatorLaunchProfile& profile, std::string& outError);
    // The configured hard drives plus the CD image and the selected model: what a start through
    // the generated config boots.
    core::EmulatorLaunchProfile CreateLaunchProfile(
        const std::optional<std::filesystem::path>& baseConfig = std::nullopt) const;
    // Whether a start must go through the generated config rather than the core's own content
    // loader: any hard drive, a CD image, or a CD machine.
    bool NeedsGeneratedConfig() const;
    // The disc CD0 gets at the next power-on (empty for none). InsertCd() changes the running
    // machine; this is what survives a restart.
    void SetConfiguredCdImage(const std::filesystem::path& image) { m_configuredCdImage = image; }
    const std::filesystem::path& GetConfiguredCdImage() const { return m_configuredCdImage; }
    bool IsCdMachine() const;
    // Whether the running machine was started with a CD drive (a CD machine, or cdimage0=).
    bool HasCdDrive() const { return m_gameLoaded && m_cdDriveInMachine; }
    bool StartConfiguredHostMounts(
        std::string& outError,
        const std::optional<std::filesystem::path>& baseConfig = std::nullopt);
    core::HostMountService& GetHostMountService() { return m_hostMountService; }
    const core::HostMountService& GetHostMountService() const { return m_hostMountService; }

    // Boots a synthetic ADF whose blocks 0/1 are the given boot block sectors (rest zero), so
    // Kickstart runs it the normal way. Used by the Boot Block Viewer's live-preview tab.
    bool RunBootBlock(const std::vector<uint8_t>& sector0, const std::vector<uint8_t>& sector1);

    // Runtime
    void Update(); // Run one frame (counts toward frame counter)
    // Run exactly one frame, bypassing the time accumulator. Tests and non-visual hosts can
    // suppress the OpenGL upload while still advancing the emulated machine.
    void StepOneFrame(bool presentFrame = true);
    void RunForStep(); // Run one retro_run() call without incrementing the frame counter
    // Complete the current (possibly mid-frame) frame: run until the next vpos==0 boundary,
    // stepping past any breakpoints encountered so the partial frame always finishes. Used by
    // the "End Frame" action to turn a mid-frame breakpoint stop into a viewable whole frame.
    void RunToFrameEnd();
    /**
     * @brief Warm boot: press the machine's Ctrl + Left-Amiga + Right-Amiga keyboard reset.
     *
     * This is the real thing, not a re-creation of the machine: UAE's keyboard layer handles
     * the combination itself (so it works even when the emulated CPU is wedged), every drive
     * keeps the media it had, and Kickstart re-runs its boot sequence — booting again from
     * the disk still sitting in DF0. The reset fires when the combination is released, so it
     * lands a few emulated frames after this returns; the caller must let frames run.
     * @return true if the warm reset was issued; false if it could not be (no game loaded, or
     *         the core never handed us its keyboard callback), in which case HardReset() has
     *         already been run instead.
     */
    bool Reset();

    /**
     * @brief Cold boot: unload and reload the game, rebuilding the machine from scratch.
     *
     * Only the .uae config (if one is in use) or DF0's image is restored — media put into the
     * other drives through InsertDisk() is not, so callers that track drive contents must
     * re-insert it afterwards.
     */
    void HardReset();
    void EjectDisk();
    void SetSpeedMultiplier(double multiplier) { m_speedMultiplier = multiplier; }
    // Restarts wall-clock pacing from now: the machine was not being advanced (paused, hidden),
    // so it owes no emulated time for the gap. Without this the first Update() after a pause ran
    // the pause as catch-up - 12 frames at once, whose audio then sat in the ring ahead of
    // everything played after it, ~15 fields behind the picture.
    void ResyncFrameClock()
    {
        m_lastFrameTime = std::chrono::steady_clock::now();
        m_frameAccumulator = 0.0;
        EndPushStream();
    }
    double GetSpeedMultiplier() const { return m_speedMultiplier; }
    // The machine's own frame rate (50 Hz PAL, 60-ish NTSC), as the core reports it.
    // Anything that steps frames by hand has to pace itself against this - Update()'s
    // wall-clock accumulator is not in the loop for it.
    double GetTargetFps() const { return m_targetFps; }
    // Stops Update()'s catch-up batch as soon as the total frame count reaches `frame`, so a
    // run-until-frame lands on its target instead of wherever the accumulator happened to
    // stop. At warp speed a batch is tens of frames, all of them past the target. 0 clears it.
    void SetFrameStopTarget(uint64_t frame) { m_frameStopTarget = frame; }
    // A host-side break from the frame-complete hook (an Exocet row breakpoint): the catch-up
    // batch stops after the frame that raised it, and IsDebugBreakPending() reports it once.
    void RequestHostBreak() { m_hostBreakPending = true; }
    // Where a host break is expected: the batch is capped there, as a run-until-frame target
    // caps it, so that frame is the one presented and harvested. Cleared once reached; 0 clears.
    void SetFrameBreakTarget(uint64_t frame) { m_frameBreakTarget = frame; }

    // Full machine-state snapshot via libretro retro_serialize/retro_unserialize.
    // Used by the .peek session format to resume a debug session at the exact PC. The restore
    // is frame-granular, not cycle-exact: UAE's state format reads VPOSR/VHPOSR back and
    // discards them, and retro_unserialize runs whole frames until the restore settles, so the
    // machine always resumes at a frame boundary wherever in the frame it was captured.
    // GetStateSize() returns 0 if unsupported or no game is loaded. SaveState() returns an
    // empty vector on failure. LoadState() returns false on size mismatch or core rejection
    // (callers should fall back to a fresh boot); the snapshot is only valid for the same
    // core build, model and mounted game.
    // Fired once for every whole emulated frame, at the moment the frame boundary is
    // crossed, carrying the new GetTotalFrameCount(). Update() runs a catch-up batch of up
    // to a dozen frames per host redraw, so anything that must see *every* frame (the
    // frame-history recorder) has to hang off this rather than sampling once per redraw -
    // sampling misses every frame of a batch but the last. Never fires for a partial frame
    // (a mid-frame breakpoint stop); the frame that completes it fires instead.
    //
    // The callback runs between retro_run() calls with the machine at a frame boundary, so
    // it may serialize state, but it must not run frames or reload the core.
    void SetOnFrameComplete(std::function<void(uint64_t frame)> cb)
    {
        m_onFrameComplete = std::move(cb);
    }

    // Skips the per-frame debug-event harvest (bplpt/sprpt/blit/copper/reg-write/CPU-trace) in
    // RunOneRetroFrame - pure UI-feed data for the debugger windows, with no other consumer.
    // Intended for turbo: none of it is meaningful while the machine is running flat out, and
    // fetching/filtering it every host frame is real cost paid for nobody to look at. Disk
    // event harvesting is unaffected - it backs the turbo auto-exit-on-drive-activity safety
    // check and always keeps running.
    void SetDebugHarvestSuppressed(bool v) { m_debugHarvestSuppressed = v; }
    // Same condition (turbo actively running the machine), for consumers outside
    // RunOneRetroFrame that arm their own core-side capture on a window's open state — e.g.
    // DmaStatsView's SetDebugDma(1), which costs a per-cycle recording pass inside retro_run()
    // itself and is not touched by the host-side harvest this flag otherwise gates.
    bool IsDebugHarvestSuppressed() const { return m_debugHarvestSuppressed; }

    size_t GetStateSize() const;
    std::vector<uint8_t> SaveState() const;
    bool LoadState(const std::vector<uint8_t>& data) const;

    // A save state that can actually be restored.
    //
    // SaveState() serializes wherever the caller happens to be, which is where retro_run()
    // returned - three scanlines into the next field. PUAE's restore has no raster position to
    // put back (restore_custom() reads VPOSR/VHPOSR and discards them), so it always resumes at
    // vpos 0, and those three scanlines get executed a second time on top of a CPU that already
    // ran them - vertical blank interrupt included. Measured on one demo, that is fatal: restore
    // and run 5 frames and 6,892 bytes of RAM differ from an uninterrupted run; within 30 frames
    // the display is dead. Captured at vpos 0 instead, the same test differs by 0 bytes.
    //
    // So this returns the state the core took inside its own vpos = 0 reset, which is exactly
    // where a restore lands. Falls back to SaveState() (with one warning) on a core too old to
    // export the capture, or before the first frame boundary has gone past.
    std::vector<uint8_t> SaveStateForRestore();

    // Turn the core's per-frame vpos-0 capture on or off. No-op on a core that predates it,
    // and a no-op when it is already in the state asked for.
    //
    // This is not free and it is not passive. PUAE's save_state() deliberately mutates the
    // machine on the way past - save_cia_prepare() catches both CIAs up and reschedules
    // eventtab[ev_cia], and custom_prepare_savestate() fires every pending eventtab2 entry
    // early whenever cycle-exact CPU emulation is off. That is correct for a state you are
    // about to load somewhere else, and wrong to do fifty times a second to a machine that is
    // still running. So it stays off unless something actually wants a restorable state -
    // Application::UpdateFrameHistory() owns that decision.
    void SetStateCaptureEnabled(bool enable);
    bool IsStateCaptureEnabled() const { return m_stateCaptureArmed; }
    bool HasFrameStartStateCapture() const { return m_adm_get_frame_start_state != nullptr; }

    // Let PUAE's own log through to the host Log window, or keep discarding it.
    //
    // The core writes everything - including its warnings - at RETRO_LOG_INFO, and the host
    // log callback drops anything below RETRO_LOG_WARN, so by default not one line of the
    // core's log is visible. Some of those lines are the only direct evidence available for a
    // whole class of question ("BLITTER active while saving state" being the one that made
    // this worth wiring up), so this exists to turn them on while somebody is looking.
    //
    // Off by default: the stream is chatty, it is produced on the emulation thread, and left
    // on it buries the host's own log. Static because the callback PUAE holds is a plain
    // function with no instance to reach.
    static void SetCoreLogVerbose(bool enable);
    static bool IsCoreLogVerbose();

    // True once anything has asked for a restorable state since this game was loaded. The
    // arming policy latches on it: whoever asked once is likely to ask again, and the second
    // ask should not have to pay for the first one having been unarmed.
    bool RestorableStateWasRequested() const { return m_restorableStateRequested; }

    // ---- Frame input capture / replay ----
    // The frame history needs a recorded frame to re-execute identically, and a save
    // state alone does not carry the host input that drove it. Capture records the input
    // as the core resolved it, once per frame, at libretro poll time.
    void SetInputCaptureEnabled(bool enabled);
    bool IsInputCaptureEnabled() const { return m_inputCaptureEnabled; }
    // The input captured for the frame that just ran. Only meaningful with capture on.
    const core::FrameInput& LastFrameInput() const { return m_lastFrameInput; }

    // Replay drives the machine from recorded input instead of the host. The live input
    // state is saved on Begin and restored on End, so a rewind cannot leave a key stuck
    // down. Call SetReplayInput() before each replayed frame; a frame replayed without
    // one runs with no input at all rather than with stale host state.
    void BeginInputReplay();
    void EndInputReplay();
    bool IsReplayingInput() const { return m_replayActive; }
    void SetReplayInput(const core::FrameInput& input);

    // ---- Per-frame digests ----
    // What a frame looked like and sounded like, reduced to three numbers, so a replay can
    // be compared against a recorded one without storing either.
    //
    // Both taps sit in the host-side libretro callbacks, so they work with no OpenGL context
    // and no audio device - which is the only way a headless replay can produce them at all.
    // The video tap in particular runs BEFORE the m_acceptVideoRefresh check: a headless
    // stepper passes presentFrame=false, which clears that flag, and hashing after it would
    // silently digest nothing for exactly the runs this exists to serve.
    //
    // Off by default. The cost is a pass over the framebuffer every frame.
    struct FrameDigest
    {
        uint64_t video = 0; // 0 when the frame delivered no framebuffer at all
        uint64_t audio = 0;
        uint32_t audioSamples = 0;
    };
    void SetFrameDigestEnabled(bool enabled);
    bool IsFrameDigestEnabled() const { return m_frameDigestEnabled; }
    // The digest of the frame that just completed. Meaningless with the digest off.
    FrameDigest GetLastFrameDigest() const { return m_lastFrameDigest; }

    // Digest of every writable RAM bank, in address order. Expensive - it walks all of chip,
    // slow and fast RAM - so a scenario samples it every Nth frame rather than every frame.
    // Reads the banks directly; nothing here comes from custom_storage, which is a write
    // shadow rather than live chipset state.
    uint64_t ComputeRamDigest() const;

    // Multi-drive support (DF0:-DF3:)
    // fileExt selects the temp-file extension, exactly as LoadADF's does: a raw
    // preservation image hot-inserted as ".adf" would reach the core's plain
    // sector reader instead of its CAPS/flux reader.
    bool InsertDisk(unsigned driveIndex, const std::vector<uint8_t>& adfData,
                    const std::string& fileExt = "adf");
    void EjectDisk(unsigned driveIndex);
    // Takes the disk out and puts it straight back, re-inserting the image the core
    // already holds rather than the host's copy - so everything the guest has written
    // since it was mounted stays. Used to present a write-protect change as the disk
    // change it physically is; see the definition for why nothing less works.
    bool ReinsertDisk(unsigned driveIndex);
    unsigned GetNumDriveSlots() const;

    // Drive enable/disable (affects PuAE floppy type at boot)
    void SetDriveEnabled(int drive, bool enabled);

    // Rendering
    GLuint GetTextureID() const { return m_textureID; }
    unsigned int GetWidth() const { return m_width; }
    unsigned int GetHeight() const { return m_height; }
    float GetAspectRatio() const { return m_aspectRatio; }

    // Internal render resolution (PuAE's puae_video_resolution). Super-hires is what AGA
    // programs need to be captured pixel-for-pixel; without it a 1280-wide screen is
    // downsampled into a 640-wide framebuffer before any debugger overlay ever sees it.
    enum class VideoResolution {
        Lores,
        Hires,
        SuperHires,
    };
    void SetVideoResolution(VideoResolution resolution);
    VideoResolution GetVideoResolution() const;

    // Captures the current emulator display texture as an in-memory PNG (RGB). Must be called on
    // the thread that owns the GL context (the main thread). Returns false if there is no frame
    // yet or the encode failed. Used by the MCP server's screenshot tool.
    bool CaptureScreenshotPNG(std::vector<uint8_t>& out) const;

    // Reads the current display texture back as raw top-row-first RGBA8 into `out`, reporting the
    // dimensions via `w`/`h`. Must be called on the GL thread (main thread). Returns false if there
    // is no frame yet. Used by the animated-GIF recorder.
    bool CaptureFrameRGBA(std::vector<uint8_t>& out, int& w, int& h) const;

    // --- Video capture ----------------------------------------------------------------
    // One file with the picture and the sound in it, in the codec the capture window chose.
    // Driven from inside the emulator rather than from the view,
    // because the audio arrives in WriteAudioSamples() and the two have to be written from the
    // same thread and the same frame.
    struct VideoCaptureOptions
    {
        core::VideoCaptureCodec codec = core::VideoCaptureCodec::Zmbv;
        int quality = -1;             // CRF for H.264/H.265; -1 takes the codec's default
        std::string ffmpegExecutable; // from FfmpegTools::Probe, for the FFmpeg codecs

        // A film of a picture made from each frame rather than the frame itself - the Exocet
        // video: `compose` is handed the frame at its
        // boundary, on the main thread with the GL context current, and fills `out` with an RGBA
        // picture of composeWidth x composeHeight. False ends the recording. Empty: the frame as it
        // is.
        struct ComposeSource
        {
            const uint8_t* rgba = nullptr; // the core's frame, top row first
            int width = 0, height = 0;
            int scaleX = 1, scaleY = 1; // the whole-number enlargement towards its shape
        };
        std::function<bool(const ComposeSource& frame, std::vector<uint8_t>& out)> compose;
        int composeWidth = 0;
        int composeHeight = 0;
    };
    // The frames come from the core's framebuffer in VideoRefreshCallback, above the
    // presentation early-out, so a run that is not presenting still records.
    bool StartVideoRecording(const std::string& path, const VideoCaptureOptions& options,
                             std::string& outError);
    bool StopVideoRecording();
    // The size a recording started now would be written at - the core's frame enlarged by
    // whole numbers towards the display shape - and the frame it comes from. False before the
    // first frame.
    bool GetVideoCaptureSize(int& outWidth, int& outHeight, int& sourceWidth,
                             int& sourceHeight) const;
    bool IsVideoRecording() const { return m_videoCapture && m_videoCapture->IsActive(); }
    // An FFmpeg recording that has stopped but is still being written out.
    bool IsVideoFinishing() const;
    uint32_t GetVideoFramesRecorded() const;
    uint64_t GetVideoBytesWritten() const;
    std::string GetVideoPath() const;
    // What the last recording came to: where it was saved, or why it stopped or failed. Empty
    // while one is running or being finished.
    const std::string& GetVideoCaptureStatus() const { return m_videoCaptureStatus; }
    // Collects FFmpeg recordings that have finished writing, and their results. Call once a UI
    // frame; it costs nothing when there are none.
    void PollVideoCapture();

    // Periodic downscaled screen capture, feeding the frame-history scrub preview so
    // dragging the seek bar shows a picture rather than only a frame number.
    //
    // Sampled straight out of the core's framebuffer as it arrives, not read back from the
    // GL texture: a glGetTexImage stalls the pipeline, while a strided nearest-neighbour
    // walk of ~20k pixels every Nth frame does not, and it works headless.
    static constexpr int kThumbnailWidth = 160;
    static constexpr int kThumbnailHeight = 128;

    // `everyNFrames` is clamped to at least 1. Disabling drops the held image.
    void SetThumbnailCapture(bool enabled, int everyNFrames);
    bool IsThumbnailCaptureEnabled() const { return m_thumbEnabled; }
    int GetThumbnailInterval() const { return m_thumbInterval; }

    // Bumped every time a new thumbnail lands; a caller polls it to notice one without
    // having to know the capture cadence.
    uint64_t GetThumbnailSeq() const { return m_thumbSeq; }

    // Copies the newest thumbnail as top-row-first RGB888. False before the first capture.
    bool CopyThumbnail(std::vector<uint8_t>& out, int& w, int& h) const;

    // Display geometry: full frame dimensions, crop offsets, and drawing adjustments.
    // Used by overlay code to correctly map Amiga hardware coordinates to screen positions.
    struct DisplayGeometry
    {
        int fullW = 0; // full native frame width  (retrow, e.g. 720 for PAL hires)
        int fullH = 0; // full native frame height (retroh, e.g. 287 for single-line)
        int cropX = 0; // hires pixels cropped from left  (retrox_crop)
        int cropY = 0; // rows cropped from top           (retroy_crop)
        int visibleLeftBorder =
            0;                // UAE drawing h-offset baked into frame buffer (visible_left_border)
        int frameYAdjust = 0; // first Amiga scanline in frame buffer (thisframe_y_adjust)
    };
    DisplayGeometry GetDisplayGeometry() const;

    // Audio control
    void SetMuted(bool muted) { m_isMuted = muted || m_forceSilent; }
    bool IsMuted() const { return m_isMuted; }
    // Held silence (--silent): the host mute stays on whatever SetMuted() is told, and Paula's
    // channel mask is held at 0 - set now and again after every core load, which resets it.
    void SetForceSilent(bool on);
    bool IsForceSilent() const { return m_forceSilent; }
    // The app's output volume, 0-100 as the slider has it: a gain on what the sound card is fed,
    // the slider's position squared so its travel sounds even. Mute is apart from it.
    void SetVolume(int percent);
    int GetVolume() const { return m_volume; }
    // PuAE's puae_floppy_sound option is an ATTENUATION in percent: "0" is full volume and "100"
    // is silence. These speak in loudness instead - 0 silent, 100 full - and flip the sense on the
    // way to the core, so no caller has to carry the inversion around.
    void SetFloppySound(bool enabled);
    bool GetFloppySound() const { return GetFloppySoundVolume() > 0; }
    void SetFloppySoundVolume(int percent);
    int GetFloppySoundVolume() const;

    // --- Audio debug taps ---
    // Two capture points, so a glitch can be blamed on a layer instead of guessed at:
    //   Core   the libretro batch payload, before Peekaboo's ring buffer — what PuAE synthesised.
    //          Produced on the main thread, so it is wall-clock independent and works headless.
    //   Device what miniaudio hands the sound card — what you actually hear.
    // Clean at Core but corrupt at Device isolates the fault to Peekaboo's host audio path.
    enum class AudioTap { Core, Device };

    bool StartAudioRecording(AudioTap tap, const std::string& path, double maxSeconds,
                             std::string& outError);
    void StopAudioRecording(AudioTap tap);
    void StopAllAudioRecording();
    bool IsAudioRecording(AudioTap tap) const;
    core::AudioRecorder::Stats GetAudioRecordingStats(AudioTap tap) const;
    // Main-thread pump: flushes both taps' staging rings to disk. Cheap when idle.
    void DrainAudioRecorders();

    // Running health of the host audio path. Every field here is a symptom the emulator was
    // previously unable to report: overruns, underruns and resampler losses were all silent.
    // Bucket edges for AudioStats::produceGapHist, in ms.
    static constexpr std::array<double, 7> kProduceGapEdgesMs = {8, 16, 24, 32, 40, 60, 100};

    struct AudioStats
    {
        bool initialized = false;
        bool muted = false;
        bool resampling = false;
        std::string backend; // "wasapi-push" or "miniaudio"
        double coreSampleRate = 0.0;
        uint32_t deviceSampleRate = 0;
        double targetFps = 0.0;

        uint64_t framesProduced = 0;         // frames handed to us by the core
        uint64_t framesDroppedOverrun = 0;   // dropped because the ring was full
        uint64_t framesConsumed = 0;         // frames drained by the device callback
        uint64_t framesHeld = 0;             // output frames faked by repeating the last sample
        uint64_t framesDroppedResampler = 0; // input frames the resampler never consumed
        uint64_t deviceCallbacks = 0;
        uint64_t underrunCallbacks = 0; // callbacks that could not be filled from the ring

        // Ring occupancy, sampled in the device callback.
        uint32_t ringSize = 0;
        uint32_t ringLevel = 0;
        uint32_t ringLevelMin = 0;
        uint32_t ringLevelMax = 0;
        uint32_t ringTarget = 0;    // frames held before output starts (the cushion)
        double ringLevelMean = 0.0; // mean occupancy at callback entry while playing
        uint64_t framesTrimmed = 0; // stale audio dropped to hold the ring at its target
        uint64_t trimEvents = 0;
        uint32_t ringLevelMinPlaying = 0; // lowest occupancy at callback entry while playing
        double pacePpm = 0.0;             // current pace correction, parts per million

        // What the sound card queues behind the ring. latencyMs adds the two: how long after its
        // frame was emulated a sample reaches the device, i.e. how late the sound runs against
        // the picture before the display's own (much smaller) delay is subtracted.
        uint32_t devicePeriodFrames = 0;
        uint32_t deviceBufferFrames = 0;
        double latencyMs = 0.0;

        // Producer jitter: frames delivered per retro_run(). A steady stream is ~882 at 44.1 kHz
        // PAL; a demo whose blocks vary wildly is the signature we are hunting.
        uint32_t lastBlockFrames = 0;
        uint32_t minBlockFrames = 0;
        uint32_t maxBlockFrames = 0;
        uint64_t blockCount = 0;
        double meanBlockFrames = 0.0;

        // Emulated time Update() declined to run. This audio is never generated at all, so it is a
        // hole in the stream that buffering cannot cover.
        uint64_t timeDiscardEvents = 0;
        uint64_t timeDiscardedFrames = 0;
        uint64_t timeDiscardElapsedClamp = 0;     // site 0: host hitch > 100 ms
        uint64_t timeDiscardAccumulatorClamp = 0; // site 1: accumulator collapsed to one frame
        uint64_t timeDiscardFrameStopCap = 0;     // site 2: run-until-frame cap

        // Wall-clock spacing of deliveries from the core.
        double produceGapMeanMs = 0.0;
        double produceGapMaxMs = 0.0;
        uint64_t produceStalls = 0; // gaps > 40 ms, i.e. longer than the device's own period
        // Delivery gaps by size, edges in kProduceGapEdgesMs (the last bucket is everything
        // above the last edge). What the ring has to ride out, so what sizes its target.
        std::array<uint64_t, 8> produceGapHist{};

        // Emulation cost. If updateMaxMs is close to produceGapMaxMs the stall is emulation; if it
        // is far below, the main thread was blocked by something else in the frame.
        double updateMeanMs = 0.0;
        double updateMaxMs = 0.0;
        uint64_t updateSlow = 0;

        // Ground truth for frame length. A PAL frame is 312 x 227 = 70,824 CCK. retroRunCalls
        // above frameBoundaries means retro_run is returning mid-frame.
        uint64_t retroRunCalls = 0;
        uint64_t frameBoundaries = 0;
        double frameCckMean = 0.0;
        uint32_t frameCckMin = 0;
        uint32_t frameCckMax = 0;
    };

    AudioStats GetAudioStats() const;
    void ResetAudioStats();

    // --- Paula channel telemetry ---
    // Everything above measures the HOST audio path (ring buffer, sound card). This block is the
    // chipset side: what each of Paula's four voices actually emitted, straight from the tap in
    // audio.c newsample().
    //
    // DLL wire format — must match adm_audio_sample_t in libretro-core.c exactly.
    struct AudioSampleEvent
    {
        int16_t sample; // raw signed 8-bit sample, sign-extended, BEFORE the channel mask
        uint8_t vol;    // AUDxVOL in effect (0-64)
        uint8_t flags;  // bit0 = channel audible in mask, bit1 = channel DMA on
        uint16_t per;   // AUDxPER in effect
        uint16_t vpos;  // beam position when the sample was emitted
        uint16_t hpos;
        uint32_t cycle; // absolute CCK (get_cycles()/512), same units as every other event
    };
    static constexpr int kAudioChannels = 4;
    static constexpr int kMaxAudioSamples = 2048; // per channel; ~7 PAL frames at period 254

    // Capturing costs the core a call per emitted sample, so it stays off until a viewer asks.
    void SetAudioTraceEnabled(bool enabled);
    bool IsAudioTraceEnabled() const { return m_audioTraceEnabled; }
    // The same tap, asked for by a recording (the Exocet video's scopes) rather than the Audio
    // window: a request of its own, so neither turns the other's off. The tap runs while
    // either wants it.
    void SetAudioTraceForCapture(bool enabled);

    // Samples captured during the most recent retro_run() frame, oldest first.
    // Returns nullptr (and outCount 0) when the channel is silent or the core lacks the tap.
    const AudioSampleEvent* GetAudioTrace(int channel, int& outCount) const;

    // Per-channel mute. Bit n set = channel n audible. Backed by PuAE's audio_channel_mask.
    void SetAudioChannelMask(uint8_t mask);
    uint8_t GetAudioChannelMask() const;

    struct AudioFilterState
    {
        bool ledFilterOn = false; // the one-pole LED filter is currently in circuit
        int model = 0;            // 0 none, 1 A500, 2 A1200, 3 A500 fixed-only
    };
    AudioFilterState GetAudioFilterState() const;

    bool HasAudioDebug() const { return m_adm_get_audio_trace != nullptr; }

    // Drop both the core-side rings and our per-frame cache. Called from every reset/load path —
    // samples captured by the previous machine must not bleed into the new one's scopes.
    void ClearAudioTrace();
    // Push m_audioTraceEnabled down whenever the core disagrees. Cheap enough to call per frame.
    void SyncAudioTraceEnabled();
    // The frame's Paula samples into m_audioTrace, kept to the CCK window [winLo, winHi) when
    // `doFilter`; nothing when no one wants them.
    void HarvestAudioTrace(bool doFilter, uint32_t winLo, uint32_t winHi);

    // Disk activity
    bool IsDriveActive() const { return m_driveLedState; }
    // True if the drive LED turned on at any point since the last ClearDriveActiveLatch() call.
    // Survives within-batch on/off transitions that IsDriveActive() would miss.
    bool ConsumeDriveActiveLatch()
    {
        bool v = m_driveActiveLatch;
        m_driveActiveLatch = false;
        return v;
    }
    bool IsDriveActive(int driveIndex) const
    {
        if (driveIndex < 0 || driveIndex > 3)
            return false;
        return m_driveMotorState[driveIndex];
    }
    bool IsDriveWriting(int driveIndex) const
    {
        if (driveIndex < 0 || driveIndex > 3)
            return false;
        return m_driveWriteState[driveIndex];
    }
    int GetCurrentTrack(int driveIndex = 0) const
    {
        if (driveIndex < 0 || driveIndex > 3)
            return 0;
        return m_currentTracks[driveIndex];
    }
    // True while the drive holds media, straight from UAE's own gui_data (the same flag its
    // status line uses), so it reports what the emulated machine can actually see rather than
    // what the host last asked for.
    bool IsDiskInserted(int driveIndex) const
    {
        if (driveIndex < 0 || driveIndex > 3)
            return false;
        return m_diskInserted[driveIndex];
    }
    // True when the loaded core exports the per-drive write-protect entry points
    // (PuAE >= 1.78.0). Older cores load fine; protection is then enforced host-side
    // only and the guest still sees a writable disk.
    bool HasWriteProtectSupport() const { return m_adm_set_floppy_write_protect != nullptr; }

    // Forces the drive's write-protect the way a tab on a real disk does. The core
    // reads it live through drive_writeprotected() -> CIAA PRA /WPRO, so the guest
    // notices without an eject. Remembered per slot and re-asserted into the core
    // before every retro_run() - several core paths clear the flag on their own, and
    // one frame without it is one track written to a locked disk. See
    // AssertFloppyWriteProtect().
    void SetFloppyWriteProtect(int driveIndex, bool writeProtected);
    bool GetRequestedFloppyWriteProtect(int driveIndex) const
    {
        if (driveIndex < 0 || driveIndex > 3)
            return true;
        return m_floppyWriteProtect[driveIndex];
    }
    // What the core itself reports, from gui_data - the same flag its status line
    // uses, and therefore what the emulated machine actually sees. Also true for an
    // empty drive and while the global floppy_read_only option is on.
    bool IsDiskWriteProtected(int driveIndex) const
    {
        if (driveIndex < 0 || driveIndex > 3)
            return false;
        return m_diskWriteProtected[driveIndex];
    }
    // The core's own answer, asked live rather than read from the gui_data mirror:
    // drive_writeprotected(), which is what DISK_status_ciaa() feeds to the CIAA PRA
    // /WPRO bit the guest samples. True for an empty drive too. Diagnostic only - the
    // display uses the host's request, see Application::GetFloppyDriveStatus().
    bool IsCoreFloppyWriteProtected(int driveIndex) const
    {
        if (driveIndex < 0 || driveIndex > 3 || !m_adm_get_floppy_write_protect)
            return false;
        return m_adm_get_floppy_write_protect(driveIndex) != 0;
    }
    // How many frames the core was caught having dropped this slot's forced write
    // protection, since the core was loaded. Any non-zero value means a core path
    // cleared floppy[n].forcedwrprot behind the host's back and the emulated machine
    // saw a writable disk for up to a frame - long enough for a whole track write.
    uint32_t GetFloppyWriteProtectDrift(int driveIndex) const
    {
        if (driveIndex < 0 || driveIndex > 3)
            return 0;
        return m_floppyWriteProtectDrift[driveIndex];
    }

    int GetCurrentSide() const { return m_currentSide; }
    bool HasPerUnitHdActivity() const { return m_adm_drain_hd_activity != nullptr; }

    // ---- Media exports (core 1.84.0) ----
    // WHDLoad payload: the core writes its embedded WHDLoad boot files and WHDSaves directory
    // into the save directory, exactly as a WHDLoad content load would, and reports both paths.
    struct WhdloadPayload
    {
        std::filesystem::path whdloadDirectory;
        std::filesystem::path savesDirectory;
    };
    bool HasWhdloadSupport() const { return m_adm_whdload_prepare != nullptr; }
    bool PrepareWhdloadPayload(bool buttonWait, bool noWriteCache, int prefsMode, int theme,
                               WhdloadPayload& out, std::string& outError);
    // CD0 while the machine runs. The insert takes effect on the core's next blkdev poll.
    bool HasCdControl() const { return m_adm_cd_insert && m_adm_cd_eject && m_adm_cd_get_image; }
    bool InsertCd(const std::string& imagePath);
    void EjectCd();
    // The image the running machine has in CD0; empty when the drive is empty or no game runs.
    std::string GetCdImage() const;
    // CD activity lamp from gui_data.cd: 0 off, otherwise the core's LED state.
    int GetCdLed() const;
    // .zip/.7z extraction through the core's own archivers. Stateless; safe off the UI thread.
    bool HasArchiveExtraction() const { return m_adm_extract_archive != nullptr; }

    // Kickstart/extended ROM identification by content, through the core's own ROM database
    // (core 1.96.0). Use core::KickstartRoms rather than calling this directly.
    bool HasRomIdentification() const { return m_adm_identify_rom != nullptr; }
    // 1 identified, 0 read but unknown to the database, -1 unreadable (or no such export).
    int IdentifyRom(const std::filesystem::path& file, const std::filesystem::path& keyDirectory,
                    core::RomIdentity& out) const;
    bool ExtractArchive(const std::filesystem::path& archive,
                        const std::filesystem::path& destination) const;

    // ---- RTG / Picasso96 (core 1.99.0) ----
    // What the uaegfx board is doing. `boardSize` is the configured VRAM in bytes and is 0 when
    // no board is fitted, so it also answers "could RTG ever come up in this machine". While
    // `active` is false the picture on screen is the chipset's and width/height/depth are 0.
    //
    // `depth` is the GUEST's bits per pixel (8 for a 256-colour Workbench), not the host
    // framebuffer's - the core converts every RTG format to the front-end's own pixel format.
    struct RtgState
    {
        uint32_t boardSize = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0;
        bool active = false;
    };
    bool HasRtg() const { return m_adm_get_rtg_state != nullptr; }
    RtgState GetRtgState() const;
    // True while a Picasso96 screen is the picture on screen. Every raster overlay maps a beam
    // position onto the frame and an RTG frame has no beam, so they all check this first.
    bool IsRtgDisplayActive() const { return GetRtgState().active; }

    // ---- Machine configuration (core 1.86.0) ----
    // The chipset and CPU the running machine really has. Empty before a game loads, and on an
    // older core, which has no chipset options either - the interface shows them disabled.
    bool HasMachineConfig() const { return m_adm_get_machine_config != nullptr; }
    std::optional<core::MachineConfig> GetMachineConfig() const;

    // ---- Serial and parallel ports (core 1.89.0) ----
    // The core applies a port on its own thread at the next frame, keeps it across machine
    // reloads, and forgets it only when the DLL is unloaded - LoadCore pushes these again.
    // A port the user never set is not pushed, so a .uae's own serial_port= stands.
    bool HasHostPorts() const { return m_adm_serial_set_port && m_adm_hostio_get_status; }
    void SetSerialPort(const core::SerialPortTarget& target);
    const core::SerialPortTarget& GetSerialPort() const { return m_serialPort; }
    void SetParallelOutput(const std::filesystem::path& path);
    const std::filesystem::path& GetParallelOutput() const { return m_parallelOutput; }
    std::optional<core::HostPortsStatus> GetHostPortsStatus() const;
    // MIDI through host devices (core 1.90.0): out -2 none, -1 the system mapper, 0.. a device;
    // in -1 none. Used while the Amiga runs its UART at MIDI speed.
    bool HasMidi() const { return m_adm_midi_set_devices != nullptr; }
    // bsdsocket.library (core 1.91.0) has no export; the core declaring its option is the test.
    // True before a core is loaded, when nothing says otherwise.
    bool HasBsdsocket() const
    {
        return !m_coreLoaded || GetCoreOptionDeclarations().count("puae_bsdsocket") != 0;
    }
    // The A2065 and uaenet.device (core 1.92.0) are only compiled into Windows builds, where
    // SLIRP's host glue is; the same test.
    bool HasNetworkCard() const
    {
        return !m_coreLoaded || GetCoreOptionDeclarations().count("puae_a2065") != 0;
    }
    // Expansion sound cards (core 1.93.0); the same test.
    bool HasSoundBoard() const
    {
        return !m_coreLoaded || GetCoreOptionDeclarations().count("puae_sound_board") != 0;
    }
    void SetMidiDevices(int outDevice, int inDevice);
    int GetMidiOutDevice() const { return m_midiOutDevice; }
    int GetMidiInDevice() const { return m_midiInDevice; }
    int ConsumeAggregateHdActivity()
    {
        const int status = m_aggregateHdActivityLatch;
        m_aggregateHdActivityLatch = 0;
        return status;
    }
    uint64_t GetAggregateHdActivityCount() const { return m_aggregateHdActivityCount; }

    // Power LED
    bool IsPowerLedOn() const { return m_powerLedOn; }
    uint8_t GetPowerLedBrightness() const { return m_powerLedBrightness; }

    // Input
    enum class MouseButton { Left, Right };
    void SetMouseState(int16_t deltaX, int16_t deltaY, bool leftBtn, bool rightBtn, bool middleBtn);
    // Hold an injected mouse button independently of physical mouse capture, then release it
    // automatically after durationFrames complete emulated frames. Returns the release frame.
    uint64_t ClickMouseButton(MouseButton button, uint32_t durationFrames);
    void SendInput(int port, int device, int index, int id, int16_t value);

    // Hold an injected key (a RetroKey code) for durationFrames complete emulated frames,
    // then release it automatically. Mirrors ClickMouseButton: the hold is independent of
    // the physical keyboard, so a host key-up cannot cancel it and it survives losing
    // window focus. Injected keys only expire while frames advance, so pressing one while
    // paused is safe — the press lands on the next step_frame/resume.
    // `modifiers` are additional RetroKey codes (Shift/Ctrl/Alt/Amiga) held for the same
    // window. Returns the frame at which everything is released.
    // Outcome of a scheduled press. `queued` is true when the press could not start on
    // the requested frame because the same key (or one of its modifiers) was still held
    // by an earlier injection, and was pushed out behind it instead. Without that, two
    // presses of the same key scheduled on one frame - which is every press_key call
    // made while paused, since the frame counter is not advancing - would merge into a
    // single longer hold and the guest would see one keystroke, not two.
    struct KeyPress
    {
        uint64_t pressFrame = 0;   // frame the key actually goes down on
        uint64_t releaseFrame = 0; // frame it comes back up
        bool queued = false;       // deferred behind an earlier press of the same key
    };
    KeyPress PressKey(int retroKeyCode, uint32_t durationFrames,
                      const std::vector<int>& modifiers = {});
    // As PressKey, but the press starts at an absolute emulated frame instead of now.
    // Lets a whole string be queued up front (type_text) without the host having to drive
    // the machine one character at a time. A pressFrame at or before the current frame
    // presses immediately unless it has to queue behind a live hold.
    KeyPress PressKeyAt(int retroKeyCode, uint64_t pressFrame, uint32_t durationFrames,
                        const std::vector<int>& modifiers = {});
    // Increments every time the emulated machine's frame numbering restarts at 0 - i.e.
    // on every core (re)load of a game, which is what start() and the compile-and-run
    // path both do. A warm reset does NOT rebase the counter and so does NOT bump this.
    // Callers that cached a frame number can compare session ids to find out whether it
    // still refers to the same run.
    uint64_t GetSessionId() const { return m_sessionId; }
    // Number of key presses still queued for a future frame.
    size_t GetPendingKeyCount() const { return m_pendingKeys.size(); }
    // True while the given RetroKey code is being held by an injection.
    bool IsInjectedKeyHeld(int retroKeyCode) const;
    // Release every injected key immediately (used when a session stops).
    void ClearInjectedKeys();

    // Level-triggered joystick injection: the given RETRO_DEVICE_ID_JOYPAD_* buttons stay
    // held until changed, unlike PressKey's auto-release. Only the buttons named in
    // `mask`/`values` are touched, so directions and fire can be driven independently.
    //
    // `port` is a LIBRETRO port index (0 or 1), which is the inverse of the Amiga port
    // number: libretro port 0 drives the Amiga's port 1 (JOY1DAT / CIA-A PRA bit 7) and
    // libretro port 1 drives the Amiga's port 0 (JOY0DAT / CIA-A PRA bit 6). PuAE fixes
    // that swap in libretro-mapper.c (`uae_port = (retro_port == 0) ? 1 : 0`). Callers
    // that speak Amiga numbering should translate at their own boundary; McpToolHandler
    // does exactly that for the set_joystick tool.
    static constexpr int kJoypadPorts = 2;
    void SetInjectedJoypadButton(int port, int retroPadId, bool down);
    void ClearInjectedJoypad(int port);
    // Bitmask of currently-injected RETRO_DEVICE_ID_JOYPAD_* buttons for a port.
    uint32_t GetInjectedJoypadMask(int port) const;
    // True once a libretro joypad device has been registered on `port`. Port 1 is only
    // registered on demand (see EnsureJoypadPortDevice) because attaching a joystick to
    // the Amiga's port 0 displaces the emulated mouse that normally lives there.
    bool IsJoypadPortActive(int port) const;
    // Register RETRO_DEVICE_JOYPAD on a libretro port so PuAE's `is_retropad()` accepts
    // input for it. Returns false when the core is not loaded. Idempotent.
    bool EnsureJoypadPortDevice(int port);

    // Controller/Gamepad
    struct ConnectedController
    {
        int id;
        std::string name;
        ControllerType type;
    };

    std::vector<ConnectedController> GetConnectedControllers() const;
    void RefreshConnectedControllers();
    void SetControllerType(const std::string& controllerName, ControllerType type);

    // Old method - deprecated or internal use only
    void SetControllerType(ControllerType type);
    ControllerType GetControllerType() const { return m_controllerType; }
    void PollGamepad();

    // Keyboard joystick: maps cursor keys to directions and A/S to fire
    // buttons, emulating a Standard Joystick on port 1.
    void SetKeyboardJoystickEnabled(bool enabled);
    bool IsKeyboardJoystickEnabled() const { return m_keyboardJoystickEnabled; }
    // Set the state of a single keyboard-joystick button (RETRO_DEVICE_ID_JOYPAD_*).
    void SetKeyboardJoyButton(int retroId, bool down);

    // Per-frame bitplane pointer events (captured during retro_run())
    struct BplptEvent
    {
        uint16_t vpos; // scanline when BPLxPTL was written
        uint8_t plane; // 0-based bitplane index
        uint8_t pad;
        uint32_t ptr;     // full 24-bit Amiga bitplane pointer
        uint16_t bplcon0; // BPLCON0 snapshot at time of write
        uint16_t ddfstrt; // DDFSTRT snapshot at time of write
        uint16_t ddfstop; // DDFSTOP snapshot at time of write
        uint16_t pad2;
        uint32_t cycle; // absolute CCK at time of write (get_cycles()/512)
    };
    static constexpr int kMaxBplptEvents = 512;

    // Returns events captured during the most recent retro_run() frame.
    int GetBplptEvents(BplptEvent* out, int maxEvents) const;

    // Per-frame sprite pointer events (captured during retro_run() when libretro patch active)
    // DLL wire format — must match adm_sprpt_event_t in libretro-core.c exactly.
    struct SprptEvent
    {
        uint16_t vpos;     // scanline when SPRxPTL was written
        uint8_t spriteIdx; // 0-7 sprite index
        uint8_t pad;
        uint32_t ptr;        // full 24-bit Amiga sprite pointer
        uint32_t cycle;      // absolute CCK at time of write (get_cycles()/512)
        uint16_t colors[16]; // COLOR16-31 snapshot; populated from adm_get_sprpt_color_snaps
    };

    // Parallel color snapshot read from adm_get_sprpt_color_snaps (new DLL only).
    struct SprptColorSnap
    {
        uint16_t colors[16]; // COLOR16-31 at arm time
    };
    static constexpr int kMaxSprptEvents = 256;

    // Returns events captured during the most recent retro_run() frame (0 if patch not active).
    int GetSprptEvents(SprptEvent* out, int maxEvents) const;

    // Per-frame blitter events (captured during retro_run() when libretro patch active)
    struct BlitEvent
    {
        uint16_t vpos, hpos;
        uint32_t bltapt, bltbpt, bltcpt, bltdpt;
        uint16_t bltcon0, bltcon1;
        uint16_t bltsize;
        uint16_t vblitsize, hblitsize; // decoded from bltsize or ECS BLTSIZV/H
        uint16_t bltafwm, bltalwm;
        uint16_t bltamod, bltbmod, bltcmod, bltdmod;
        uint16_t bltadat, bltbdat,
            bltcdat;          // pre-loaded data (valid when corresponding USE bit = 0)
        bool fromCopper;      // true = triggered by Copper MOVE
        uint8_t pad[3];       // explicit padding (matches C struct layout)
        uint32_t start_cycle; // absolute CCK at blit start (get_cycles()/512)
        uint32_t end_cycle;   // absolute CCK at blit end   (get_cycles()/512)
        uint32_t blitId;      // provenance id; joins onto BlitVisSpan::blitId (0 = unset)
        uint32_t dataOff;     // first BlitWord slot for this blit; see GetBlitData()
        uint32_t dataWords;   // D writes captured; 0 when destination capture was off
    };
    static constexpr int kMaxBlitEvents = 512;

    // Returns events captured during the most recent retro_run() frame (0 if patch not active).
    int GetBlitEvents(BlitEvent* out, int maxEvents) const;

    // ---- Blitter destination data ("what the blit actually wrote") -----------------------
    // One word the blitter put on the bus through its D channel, captured at the single
    // chip-RAM write choke point that also feeds the provenance map. The address is stored
    // alongside the value rather than relying on write order, which is what lets the host
    // rebuild descending, fill and line-mode blits without re-deriving their geometry.
    //
    // DLL wire format — must match adm_blit_word_t in the build scripts exactly.
    struct BlitWord
    {
        uint32_t addr; // chip address written
        uint16_t val;  // the word written
        uint16_t pad;  // explicit padding — keeps the struct at 8 bytes
    };

    // Turns the core-side D-write capture on or off. It costs a branch per blitter write and
    // a 4 MB ring while on, so the UI enables it only while something is displaying the data.
    void SetBlitDataCaptureEnabled(bool enabled);

    // Reads the core's own flag rather than a host mirror, so a core reload that dropped the
    // arming reports honestly instead of the two drifting apart.
    bool IsBlitDataCaptureEnabled() const;

    // Whether the loaded core exports the capture API at all.
    bool HasBlitDataCapture() const { return m_adm_set_blit_data_enabled != nullptr; }

    // Copies one blit's captured D writes out of the core ring, oldest first. `off`/`count`
    // come from BlitEvent::dataOff/dataWords. Returns the number of words copied, which is
    // 0 once later blits have overwritten that window — the ring drops the oldest data
    // rather than stalling the blitter.
    int GetBlitData(uint32_t off, uint32_t count, BlitWord* out) const;

    // ---- Blitter destination provenance ("blitter-vis") ----------------------------------
    // A run of native output pixels that the display fetched from chip RAM last written by
    // blit `blitId`. Produced by the core patch described in patches/adm_blitvis.c: rather
    // than reconstructing a destination rectangle from bltdpt/bltdmod geometry, the core
    // tags pixels at display-fetch time, so these coordinates are already correct for
    // BPLxMOD, fetch mode, x-scroll delay, plane number, crop and scale.
    //
    // Coordinates are native output pixels, matching GetWidth()/GetHeight() — no
    // AmigaScreenMap conversion needed. x..xEnd are inclusive.
    //
    // A blit whose destination is never fetched for display produces no spans at all; that
    // is inherent to the technique, not a failure. BlitScreenMapper's geometry boxes remain
    // the fallback for that case.
    //
    // DLL wire format — must match adm_blitvis_span_t in patches/adm_blitvis.c exactly.
    struct BlitVisSpan
    {
        uint16_t x;      // first native pixel of the span (inclusive)
        uint16_t y;      // native scanline
        uint16_t xEnd;   // last native pixel of the span (inclusive)
        uint16_t pad;    // explicit padding — keeps the struct at 12 bytes
        uint32_t blitId; // joins onto BlitEvent::blitId
    };
    static constexpr int kMaxBlitVisSpans = 65536;

    // Turns the core-side provenance map on or off. It costs a per-fetch table lookup and
    // roughly 50 MB of tables while on, so the UI enables it only while the overlay is
    // shown. No-op on a core that predates the patch.
    void SetBlitterVisEnabled(bool enabled);
    bool IsBlitterVisEnabled() const { return m_blitVisEnabled; }

    // Whether the loaded core exports the provenance API at all (false on an older DLL).
    bool HasBlitterVis() const { return m_adm_set_blitter_vis_enabled != nullptr; }

    // Spans captured during the most recent retro_run() frame.
    int GetBlitVisSpans(BlitVisSpan* out, int maxSpans) const;
    int GetBlitVisSpanCount() const { return m_blitVisSpanCount; }

    // Core-side diagnostics for the last completed frame: how many words the blitter wrote
    // into the map, and how many of those the display actually fetched. writes > 0 with
    // tags == 0 means the blits are landing somewhere never scanned out.
    struct BlitVisStats
    {
        bool enabled = false;
        int writes = 0; // blitter word-writes stamped last frame
        int tags = 0;   // stamped words the display fetched last frame
        int spans = 0;  // resulting spans
    };
    BlitVisStats GetBlitVisStats() const;

    // Per-frame Bartman idle-region events (debug_start_idle / debug_stop_idle). Captured via the
    // Bartman uaelib gate when a program compiled with gcc8_c_support is running and Bartman debug
    // support is enabled. DLL wire format — must match adm_idle_event_t in libretro-core.c exactly.
    struct IdleEvent
    {
        uint32_t cycle; // absolute CCK at the call (get_cycles()/512) — used for frame windowing
        uint16_t vpos;  // scanline at the call
        uint16_t hpos;  // horizontal CCK at the call (lRef = vpos*CCKPerLine + hpos)
        uint8_t start;  // 1 = start idle region, 0 = stop idle region
        uint8_t pad[3];
    };
    static constexpr int kMaxIdleEvents = 256;

    // Returns idle events captured during the most recent retro_run() frame (0 if the loaded DLL
    // predates idle profiling or no idle markers fired).
    int GetIdleEvents(IdleEvent* out, int maxEvents) const;
    // Whether the loaded core exports idle profiling (drives the scrubber's Idle-row availability).
    bool HasIdleProfiling() const { return m_adm_get_idle_events != nullptr; }

    // Per-frame copper execution events (captured during retro_run() when libretro patch active)
    struct CopperExecEvent
    {
        uint16_t vpos; // scanline when instruction executed
        uint16_t hpos; // horizontal CCK position
        uint32_t addr; // chip-mem address of instruction word1
        uint16_t word1;
        uint16_t word2;
        uint8_t list; // 1=COP1, 2=COP2 (0 if unknown/old core)
        uint8_t pad[3];
        uint32_t absolute_cycle; // absolute CCK at execution (get_cycles()/512)
    };
    static constexpr int kMaxCopperExecEvents = 16384;

    // Returns events captured during the most recent retro_run() frame (0 if patch not active).
    int GetCopperExecEvents(CopperExecEvent* out, int maxEvents) const;
    bool GetCopperExecOverflow() const { return m_copperExecOverflow; }

    // Per-frame custom-register write log (captured during retro_run()). Every CPU and Copper
    // write to $DFFxxx passes through custom_wput_1 in the core, which is where this is hooked,
    // so both sources appear here already discriminated by `fromCopper`.
    //
    // Not visible here: chipset activity that bypasses the register-write path entirely —
    // sprite DMA loading SPRxPOS/CTL/DATA, and bitplane pointer auto-increment. The Sprites
    // and Bitplanes windows cover those.
    //
    // Wire format — must match adm_reg_write_event_t in the PuAE core exactly.
    struct RegWriteEvent
    {
        uint16_t vpos;  // scanline of the write
        uint16_t hpos;  // horizontal CCK position
        uint16_t reg;   // register offset (addr & 0x1FE)
        uint16_t value; // value written
        uint32_t pc;    // writing CPU instruction, or the copper MOVE's address
        bool fromCopper;
        uint8_t pad[3];
        uint32_t absolute_cycle; // absolute CCK at write (get_cycles()/512)
    };
    static_assert(sizeof(RegWriteEvent) == 20,
                  "RegWriteEvent must match adm_reg_write_event_t in the PuAE core");
    static constexpr int kMaxRegWriteEvents = 16384;

    // Returns writes captured during the most recent retro_run() frame (0 if patch not active).
    int GetRegWriteEvents(RegWriteEvent* out, int maxEvents) const;
    bool GetRegWriteOverflow() const { return m_regWriteOverflow; }
    // Whether the loaded core exports the register-write log. Optional rather than required, so
    // a DLL predating the patch still loads and the window reports it instead of crashing.
    bool HasRegWriteLog() const { return m_adm_get_reg_write_events != nullptr; }

    // Per-frame CPU instruction trace (captured during retro_run() when tracing is enabled)
    struct CpuTraceEntry
    {
        uint16_t vpos, hpos; // raster position when instruction executed
        uint32_t pc;         // program counter (instruction address)
        uint8_t raw[10];     // raw instruction bytes (up to 5 words)
        uint8_t raw_len;     // actual byte count captured (2-10)
        uint8_t pad[1];
        uint32_t d[8], a[8]; // D0-D7, A0-A7 at time of execution
        uint16_t sr;         // status register
        uint16_t pad2;
        uint32_t absolute_cycle; // absolute CCK at execution (get_cycles()/512)
    };
    static constexpr int kMaxCpuTraceEntries = 50000;

    // Returns the absolute CCK cycle counter captured at the start of the most recent frame.
    // All event absolute_cycle / start_cycle fields can be normalized by subtracting this.
    uint32_t GetFrameStartCycle() const { return m_frameStartCycle; }
    uint32_t GetPrevFrameStartCycle() const { return m_prevFrameStartCycle; }

    // Mid-frame breakpoint support. When retro_run() stops early (a breakpoint fired before
    // the next vpos==0), the harvested per-frame event views are clipped to the partial window
    // [frame start, break] rather than showing the previous whole frame. These expose where the
    // break landed so the scrubber can draw a playhead and dim the not-yet-executed region.
    bool IsPartialFrame() const { return m_partialFrame; }
    uint32_t GetBreakCycle() const { return m_breakCycle; } // absolute CCK at the stop
    int GetBreakVpos() const { return m_breakVpos; }        // scanline at the stop
    int GetBreakHpos() const { return m_breakHpos; }        // horizontal CCK at the stop
    // CCK offset of the break within the current frame (0 if not partial).
    uint32_t GetBreakFrameOffset() const
    {
        return (m_partialFrame && m_breakCycle >= m_frameStartCycle)
                   ? m_breakCycle - m_frameStartCycle
                   : 0;
    }

    // Live raster position, sampled straight from the core rather than from the latched
    // break state. Anything whose meaning depends on *when* in the field it was sampled --
    // a screenshot, a custom-register read, the blitter log -- is only interpretable next to
    // this. Returns false when the core is too old to export adm_get_beampos.
    // While the emulator is free-running this is a racy snapshot of a moving beam; it is
    // exact only while paused.
    bool GetBeamPosition(int& outVpos, int& outHpos) const;

    // Returns CPU trace entries captured during the most recent retro_run() frame.
    int GetCpuTrace(CpuTraceEntry* out, int max) const;
    // Direct (zero-copy) view of the same buffer; valid until the next frame runs or a
    // clear/load/reset. Prefer this over GetCpuTrace() when the caller only reads the entries.
    const CpuTraceEntry* GetCpuTracePtr() const { return m_cpuTrace; }
    int GetCpuTraceCount() const { return m_cpuTraceCount; }
    bool GetCpuTraceOverflow() const { return m_cpuTraceOverflow; }

    // Enable/disable per-instruction CPU tracing (opt-in; UAE no-ops when disabled)
    void SetCpuTraceEnabled(bool enabled);
    bool IsCpuTraceEnabled() const { return m_cpuTraceEnabled; }
    // Push host-mirrored core-side debug toggles back into the core. A load resets
    // them while the mirror survives; see the definition.
    void ReapplyCoreDebugState() const;

    // DMA record types (mirroring PUAE debug.h DMARECORD_* constants)
    enum DmaRecordType {
        kDmaRecordNone = 0,
        kDmaRecordRefresh = 1,
        kDmaRecordCpu = 2,
        kDmaRecordCopper = 3,
        kDmaRecordAudio = 4,
        kDmaRecordBlitter = 5,
        kDmaRecordBitplane = 6,
        kDmaRecordSprite = 7,
        kDmaRecordDisk = 8,
        kDmaRecordConflict = 9,
    };

    // Per-slot DMA record matching PUAE struct dma_rec layout
    struct DmaSlotRecord
    {
        int hpos, vpos;
        int dhpos;
        uint16_t reg;
        uint64_t dat;
        uint16_t size;
        uint32_t addr;
        uint32_t evt, evt2;
        uint32_t evtdata;
        bool evtdataset;
        int16_t type; // DMARECORD_* constant
        uint16_t extra;
        int8_t intlev, ipl, ipl2;
        uint16_t cf_reg, cf_dat, cf_addr;
        int ciareg;
        int ciamask;
        bool ciarw;
        int ciaphase;
        uint16_t ciavalue;
        bool end;
    };

    // Per-frame DMA slot statistics
    struct DmaFrameStats
    {
        uint32_t copperSlots = 0;
        uint32_t blitterSlots = 0;
        uint32_t cpuSlots = 0;
        uint32_t bitplaneSlots = 0;
        uint32_t spriteSlots = 0;
        uint32_t diskSlots = 0;
        uint32_t audioSlots = 0;
        uint32_t otherSlots = 0;
        uint32_t idleSlots = 0;
        uint32_t totalSlots = 0;
        uint32_t maxSlots = 0;
    };

    // DMA debug API
    bool GetDmaRecord(DmaSlotRecord** outPtr, int& outHposCount, int& outVposCount) const;
    // In-progress frame's DMA record (the buffer currently being filled). Valid while paused
    // at a mid-frame breakpoint — slots past the break read as no-DMA. Falls back to the
    // completed-frame record on cores without the live export.
    bool GetDmaRecordLive(DmaSlotRecord** outPtr, int& outHposCount, int& outVposCount) const;
    void SetDebugDma(int mode);
    int GetDebugDma() const;
    // The DMA record for a recording (the Exocet video's slot map): armed when nothing has it
    // on - call it each frame, a restart clears it - and off again when the recording ends,
    // if it was the recording that armed it. The windows re-arm it every frame they want it.
    void ArmDebugDmaForCapture();
    bool HasDmaDebug() const { return m_adm_get_dma_record != nullptr; }

    // Disk write events — populated after retro_run() when write-redirect is disabled.
    // Each event records the drive index and track number (cyl*2+side) that was written.
    struct DiskWriteEvent
    {
        uint8_t drive;
        uint8_t pad[3];
        int32_t track; // cyl*2+side, 0-159
    };
    static constexpr int kMaxDiskWriteEvents = 64;

    int GetDiskWriteEventCount() const { return m_diskWriteEventCount; }
    const DiskWriteEvent* GetDiskWriteEvents() const { return m_diskWriteEventCache; }
    // Read back 11*512=5632 bytes of decoded sector data for a given drive+track.
    // Returns bytes read, or -1 if the export is unavailable or parameters are invalid.
    int ReadDiskTrack(int drive, int track, uint8_t* buf, int buflen) const;
    // Write numBytes bytes into the disk image at the given ADF byte offset.
    // Reads all 160 tracks, patches the range, and re-inserts via InsertDisk.
    // Returns false if unavailable or parameters are out of range.
    bool WriteDiskBytes(int drive, uint32_t diskByteOffset, const uint8_t* data, uint32_t numBytes);

    // --- FloppyBridge: back a drive slot with a real, spinning floppy ---
    // A bridged slot is a live device handle, not an image, so this is
    // deliberately not a variant of InsertDisk()/EjectDisk(): there is no file,
    // no temp path, and no replace_image_index one-shot. Attaching only writes
    // changed_prefs in the core; PuAE's own DISK_vsync() -> DISK_check_change()
    // opens or closes the hardware on the next emulated vertical blank. Verified
    // against disk.c's "old >= DRV_FB || currprefs...dfxtype >= DRV_FB" branch:
    // attach and detach are both live, with no core restart.
    struct BridgeConfig
    {
        // Index into GetBridgeDriverList(): 0 = DrawBridge, 1 = Greaseweazle,
        // 2 = SuperCard Pro.
        int driverIndex = 1;
        // Serial port name from EnumBridgeComPorts(), e.g. "COM3". Ignored by
        // drivers that auto-detect; required by Greaseweazle and DrawBridge.
        std::string comPort;
        // FloppyBridge::BridgeMode - 0 Fast, 1 Compatible, 2 TurboAmigaDOS, 3 Stalling.
        int bridgeMode = 0;
        // FloppyBridge::BridgeDensityMode - 0 Auto, 1 DD only, 2 HD only.
        int bridgeDensity = 0;
        // Cache tracks in the background whenever the drive is idle, so a track the
        // guest asks for next is already in memory. Without it every seek costs a full
        // revolution before any data can be served.
        bool autoCache = true;
        // Serve an already-captured track ten times faster than the platter turns.
        // FloppyBridge applies this per track and only where the measured bitcell
        // timing is even, so timing-based protection is still served at real speed.
        bool smartSpeed = true;
        // Which physical drive on the cable: 0 = A (PC), 1 = B, 2..5 = Shugart 0..3.
        int driveCable = 0;
        // Let the guest write to the physical disk. OFF BY DEFAULT, and deliberately
        // not derived from the disk's own write-protect tab: writing real media is
        // destructive and irreversible, so a live slot is read-only until something
        // asks for otherwise. With this off the driver reports a write-protected drive
        // to the emulated machine whatever the tab says. Driver 4 (XCopy Standalone,
        // buffered) only - every other driver ignores it, and drivers 0-2 have always
        // followed the tab because that is upstream's behaviour.
        bool allowWrites = false;
    };

    // True when the loaded core exports the bridge configuration entry points
    // (PuAE >= 1.36.0). Older cores load fine and simply report no support.
    // Only meaningful once IsCoreLoaded() is true: with no core loaded this is
    // false because there is nothing to ask, NOT because support is missing.
    // Anything user-facing must check IsCoreLoaded() before reporting on it.
    bool HasBridgeSupport() const { return m_adm_set_drive_bridge_config != nullptr; }

    // Queue an attach. Returns false only when the request was rejected outright
    // (no core support, bad slot, or a config string too long for UAE's 32-char
    // dfxprofile field). A true return means the prefs were accepted, NOT that
    // the hardware opened - the device is opened asynchronously on the next
    // vertical blank, and an open failure is reported through the core log by
    // floppybridge_init2(). Any image mounted in the slot is dropped: a slot
    // cannot be backed by both a live drive and a file.
    bool AttachBridgeDrive(unsigned driveIndex, const BridgeConfig& cfg);

    // Revert the slot to an ordinary file-backed 3.5" DD drive. The slot is left
    // empty; mount an image normally afterwards.
    void DetachBridgeDrive(unsigned driveIndex);

    // Stop every live drive now: each driver's worker thread is joined and its
    // serial port closed, without waiting for a vertical blank. Returns how many
    // were stopped, or -1 if the core cannot be asked.
    //
    // UnloadCore() calls this, and it has to: nothing in UAE's own teardown
    // releases a FloppyBridge driver. DISK_free() closes backing files, and the
    // bridge objects are freed only when a *running* machine's prefs change - so
    // stopping the machine used to leave the motor spinning, auto-cache still
    // stepping the head, and the port still held by a thread that outlived the
    // library it was running in. Unlike DetachBridgeDrive() this is immediate,
    // which is the whole point: after it there are no more vertical blanks.
    int ShutdownBridgeDrives();

    // What the host last asked for on this slot, mirrored locally so it answers
    // before the core has acted on the request. It says the slot is configured
    // as a live drive, not that the device opened - use GetBridgeStatus() for
    // that. Motor, cylinder and media work unchanged through
    // IsDriveActive()/GetCurrentTrack()/IsDiskInserted(): PuAE's gui_data
    // telemetry is filetype-agnostic.
    bool IsBridgeDrive(unsigned driveIndex) const;

    // Live per-slot bridge health, read straight from the core's bridges[] and
    // bridge_driver[] arrays. This is the only way to tell a slot whose device
    // opened from one whose device failed, because a failed open still leaves a
    // driver object attached to the drive. `opened` is the field to trust; when
    // it is false and `configured` is true, `lastError` says why.
    //
    // Layout must stay byte-identical to adm_bridge_status_t in the core.
    struct BridgeStatus
    {
        int configured = 0;
        int opened = 0;
        int diskInDrive = 0;
        int writeProtected = 0;
        int cylinder = 0;
        char driverName[64] = {0};
        char lastError[256] = {0};
    };
    // False if the export is unavailable or driveIndex is out of range, in
    // which case `out` is left zeroed.
    bool GetBridgeStatus(unsigned driveIndex, BridgeStatus& out) const;

    // Hardware types the vendored drivers support, in BridgeConfig::driverIndex
    // order. Safe with no device attached, and safe before the core is loaded -
    // with no core it answers from GetKnownBridgeDriverList() rather than
    // pretending there are no drivers, so the UI is configurable while stopped.
    std::vector<std::string> GetBridgeDriverList() const;

    // The driver names and index order the bundled core is built with, without
    // asking it. Only a stand-in for the real list; a loaded core is authoritative.
    static std::vector<std::string> GetKnownBridgeDriverList();

    // Serial ports the vendored SerialIO enumeration can see right now. Returns
    // empty with no device attached; never throws and never blocks on USB.
    // Falls back to EnumHostSerialPorts() when the core cannot be asked.
    std::vector<std::string> EnumBridgeComPorts() const;

    // Host-side serial port enumeration, no core involved: DOS device names on
    // Windows, the USB ttys on macOS/Linux. Mirrors what FloppyBridge's own
    // SerialIO reports, so the port picked while stopped is one the core will
    // recognise once it is loaded.
    static std::vector<std::string> EnumHostSerialPorts();

    // Serialise a BridgeConfig into FloppyBridge's own pipe-delimited format,
    // "[bridgeIndex|flags|comPort|bridgeMode|bridgeDensity]". Exposed for tests
    // and for the UI to show what will be sent. Returns an empty string if the
    // result would not fit UAE's dfxprofile field.
    static std::string BuildBridgeConfigString(const BridgeConfig& cfg);

    // Disk DMA transfer log — one entry per transfer armed by a DSKLEN write.
    // Must match adm_disk_dma_event_t in the PuAE core byte-for-byte (52 bytes).
    // Unlike DiskWriteEvent (which fires only when a track reaches the backing
    // file), this observes every transfer on the bus, in both directions.
    struct DiskDmaEvent
    {
        uint32_t seq;
        uint32_t startCycle, endCycle; // absolute CCK (get_cycles()/512)
        uint32_t pc;                   // instruction that wrote DSKLEN, or copper IP
        uint8_t fromCopper, direction; // direction: 2 = read, 3 = write
        uint8_t drive, side;           // drive 0xFF = no drive selected
        uint8_t cyl, turbo, aborted, pad;
        uint32_t dskptStart, dskptEnd;
        uint16_t dsklen, words, dsksync, adkcon;
        uint16_t mfmpos, pad2;
        uint32_t payloadOff, payloadLen; // window into the core's payload arena
    };
    static_assert(sizeof(DiskDmaEvent) == 52,
                  "DiskDmaEvent must match adm_disk_dma_event_t in the PuAE core");
    static constexpr int kMaxDiskDmaEvents = 256;

    int GetDiskDmaEventCount() const { return m_diskDmaEventCount; }
    const DiskDmaEvent* GetDiskDmaEvents() const { return m_diskDmaEventCache; }
    // Copies a transfer's captured payload out of the core arena. Returns bytes read.
    int GetDiskDmaPayload(uint32_t off, uint32_t len, uint8_t* out) const;
    bool HasDiskDmaLog() const { return m_adm_get_disk_dma_events != nullptr; }

    // Disk DMA breakpoint. access: -1 disarm, 0 any, 1 read, 2 write.
    // cyl/side/sector < 0 mean "any"; addrHi == 0 means "any address".
    void ArmDiskBreak(int access, int cyl, int side, int sector, uint32_t addrLo,
                      uint32_t addrHi) const;
    bool IsDiskBreakArmed() const;
    // Read-and-clear: fills outEvent with the transfer that tripped the break.
    bool ConsumeDiskBreakHit(DiskDmaEvent& outEvent) const;
    bool HasDiskBreak() const { return m_adm_arm_disk_break != nullptr; }

    // Memory map
    struct MemoryRegion
    {
        uint8_t* hostPtr;   // host memory pointer (null if not RAM-backed)
        uint32_t amigaAddr; // start address in Amiga address space
        uint32_t size;      // size in bytes
        std::string name;   // friendly name from UAE bank label
        bool writable;      // false for ROM
    };

    std::vector<MemoryRegion> GetMemoryRegions() const;

    // Cheap (allocation-free) signature over the raw bank descriptors. Changes only when the memory
    // bank layout changes; 0 when no game/core is loaded. Lets callers gate work that derives from
    // the region set without fetching the full vector every frame.
    uint64_t GetMemoryRegionsSignature() const;

    // CPU State
    struct CpuState
    {
        uint32_t d[8];
        uint32_t a[8];
        uint32_t pc;
        uint16_t sr;
    };
    bool GetCpuState(CpuState& outState) const;
    // Stack pointers, control, MMU and FPU registers for the configured CPU (core 1.102.0).
    bool HasCpuExtState() const { return m_adm_get_cpu_ext_state != nullptr; }
    bool GetCpuExtState(core::CpuExtendedState& out) const;
    bool Disassemble(uint32_t pc, char* mnemo, int mnemoSz, char* hex, int hexSz,
                     uint32_t& nextPc) const;
    // Assembles one 68k source line at addr into words (host-endian opcode words).
    // Returns >0 word count, 0 syntax error, -1 line too long, -2 unavailable.
    int Assemble(const std::string& src, uint32_t addr, uint16_t* words, int maxWords) const;
    bool HasAssembler() const { return m_adm_asm_one != nullptr; }
    bool ExecuteDebugCommand(const std::string& cmd, std::string& output) const;
    bool HasDebugCommand() const { return m_adm_debug_command != nullptr; }

    // Returns true (and clears the flag) if a CPU breakpoint fired during the last Update().
    // Used by EmulatorView to auto-pause when the emulator hits a breakpoint.
    bool IsDebugBreakPending() const;
    // Arms the CPU trace: sets SPCFLAG_BRK + trace mode so active bpnodes are evaluated
    // on the next retro_run() frame. Call after adding breakpoints or before resuming
    // after a step/trace command.
    void ArmDebugTrace() const;
    bool HasArmDebug() const { return m_adm_arm_debug != nullptr; }

    enum class OneShotBreakpointStatus : int {
        Unavailable = -1,
        Idle = 0,
        Armed = 1,
        Hit = 2,
        BudgetExpired = 3,
        Cancelled = 4,
    };
    // A headless-safe, pre-instruction stop. The core returns normally from retro_run()
    // on a hit or after instructionBudget checks, whichever happens first.
    bool ArmOneShotBreakpoint(uint32_t address, uint32_t instructionBudget) const;
    void CancelOneShotBreakpoint() const;
    OneShotBreakpointStatus GetOneShotBreakpointStatus(uint32_t* stopPc = nullptr,
                                                       uint32_t* executed = nullptr) const;
    bool HasOneShotBreakpoint() const
    {
        return m_adm_arm_oneshot_breakpoint && m_adm_cancel_oneshot_breakpoint &&
               m_adm_get_oneshot_breakpoint_status;
    }

    // Break-on-bootblock-entry (FB): a one-shot trap that halts on the first
    // instruction of a disk bootblock (address-agnostic; see build_puae.sh).
    void ArmBreakOnBootblock(bool on) const;
    bool IsBootblockArmed() const;
    uint32_t BootblockEntry() const;
    bool ConsumeBootblockHit() const; // read-and-clear: did FB cause the last break?
    bool HasFb() const { return m_adm_arm_fb != nullptr; }

    // Break-on-CPU-exception (EB): arm UAE's native exception mask (bitmask of vector numbers
    // 0..63; interrupt vectors 24..31 are always masked out core-side). mask 0 disarms.
    void ArmExceptionBreak(uint64_t mask) const;
    bool ConsumeExceptionHit(int& outVector) const; // read-and-clear: did an exception break us?
    bool HasExceptionBreak() const { return m_adm_arm_exception != nullptr; }

    // Break-on-KPrintF-text: the rules matched against the debug output the emulated program
    // writes to the serial port. The store lives here (not in the core) because the text is
    // assembled host-side by DrainSerialOutput(); it outlives stop/start so traps survive a
    // restart. A match raises a host-side break reported by IsDebugBreakPending().
    core::KprintfBreakpoints& KprintfBreakpoints() { return m_kprintfBreakpoints; }
    const core::KprintfBreakpoints& KprintfBreakpoints() const { return m_kprintfBreakpoints; }
    // Read-and-clear: the KPrintF line that caused the current break, if it was one. Used by the
    // console break banner, which must announce a hit exactly once.
    bool ConsumeKprintfHit(core::KprintfHit& out) const;
    // Non-destructive: the most recent KPrintF match this session (sticky until the next match or
    // a core unload), for the Breakpoints window's status line.
    bool GetLastKprintfHit(core::KprintfHit& out) const;

    // Break-on-beam-position (BL): break when the beam reaches scanline `vpos` (line granularity;
    // hpos is best-effort). vpos < 0 disarms. One-shot (auto-disarms when it fires).
    void ArmBeamBreak(int vpos, int hpos = -1) const;
    bool ConsumeBeamHit() const; // read-and-clear: did a beam break fire?
    bool HasBeamBreak() const { return m_adm_arm_beam_break != nullptr; }

    // Memory watchpoint hit details (read-and-clear). Returns true if a memwatch
    // fired since the last call, filling pc (the instruction that performed the
    // access), addr (accessed address), val (value written; 0 for reads),
    // rwi (bit0=read, bit1=write, bit2=instr fetch) and size (1/2/4 bytes).
    struct MemwatchHit
    {
        uint32_t pc = 0;
        uint32_t addr = 0;
        uint32_t val = 0;
        int rwi = 0;
        int size = 0;
        // UAE access mask of the triggering access (MW_MASK_* bits → CPU/Copper/Blitter/…).
        // 0 if the loaded core predates the extended accessor (adm_get_last_memwatch2).
        uint32_t accessMask = 0;
        uint32_t reg = 0; // custom register index for chipset accesses (0xffffffff if n/a)
    };
    // Non-destructive read of the watchpoint hit that caused the current break, if
    // any. The hit is latched in IsDebugBreakPending() (the single point that drains
    // the core's read-and-clear flag) so multiple consumers — the console break
    // banner and the Breakpoints window hit counter — can read it while paused.
    bool GetLastMemwatchHit(MemwatchHit& out) const;

    bool WriteCustomRegister(uint32_t addr, uint16_t value) const;

    // Writes a single CIA register. `addr` is a full CIA-space address ($BFD000-$BFEFFF);
    // CIA-A answers on odd addresses, CIA-B on even ones (see ArMemoryAccess::DecodeCia).
    // Goes straight to UAE's cia_bank.bput between retro_run() calls, exactly as
    // WriteCustomRegister goes to custom_bank.wput. Returns false when the core is not
    // loaded or the address is outside CIA space.
    bool WriteCIARegister(uint32_t addr, uint8_t value) const;

    // --- Memory protection (trainer locks) ---------------------------------------------
    // Blocks guest writes to an address/range, or forces a value there. Applied inside the
    // core's memory_put_{long,word,byte}, so it holds sub-frame rather than being a
    // once-per-frame clamp. Mirrors adm_protect_entry_t in patches/adm_protect.inc; the
    // layout is ABI and is pinned by tests/AdmProtectTest.cpp.
    //
    // NOTE: CPU writes (memory_put_*) and copper MOVEs (custom_wput_copper) are both hooked.
    // Blitter D-channel and disk DMA writes bypass both and are not blocked.
    enum class ProtectMode : uint8_t { Block = 0, Set = 1 };

    struct ProtectEntry
    {
        uint32_t addr = 0;  // first protected byte
        uint32_t len = 1;   // number of protected bytes
        uint8_t mode = 0;   // ProtectMode
        uint8_t size = 1;   // 1/2/4 - width of the forced value (Set only)
        uint32_t value = 0; // forced value, right-aligned (Set only)
    };
    static_assert(sizeof(ProtectEntry) == 16, "ProtectEntry must match adm_protect_entry_t");

    // Replaces the core's protection table wholesale. An empty list disarms it. Returns
    // false when the core is too old to export adm_protect_set (rebuild PuAE).
    bool SetProtections(const std::vector<ProtectEntry>& entries) const;
    bool ClearProtections() const;
    // True when the loaded core exports the protection hooks at all.
    bool SupportsProtections() const { return m_adm_protect_set != nullptr; }

    // Custom Registers State
    bool GetCustomRegisters(core::Custom& outCustom) const;

    // Reads a single custom-chip register word by its DFF000 byte offset (0x000..0x1FE)
    // from a fresh GetCustomRegisters snapshot. Returns false if the snapshot is
    // unavailable or the offset is out of range. Shared by the AR console and MCP
    // memory-access shims so they don't each reimplement the struct-index math.
    bool ReadCustomRegisterWord(uint32_t offset, uint16_t& out) const;

    // CIA Registers State (requires adm_get_cia_state export from PuAE)
    bool GetCIARegisters(core::CIAState& outCIA) const;

    // Reads a single CIA register (0-15) out of a fresh GetCIARegisters snapshot.
    // `chipB` selects CIA-B over CIA-A. Returns false if the snapshot is unavailable or
    // the register does not exist (0xB is unused on the 8520). Shared by the AR console,
    // GDB and MCP memory-access shims so they don't each reimplement the field mapping —
    // the same role ReadCustomRegisterWord plays for the chipset.
    bool ReadCIARegisterByte(bool chipB, int reg, uint8_t& out) const;

    // --- Bartman debug resources (vscode-amiga-debug debug_register_bitmap/palette/copperlist) ---
    // Amiga programs built with Bartman's gcc8_c_support call debug_register_*() which route
    // through the UAE uaelib gate ($F0FF60); the patched PuAE core snapshots each registration
    // into a registry we read here. Opt-in: the gate only exists when the boot rom is installed,
    // enabled via SetBartmanDebugSupport(). Mirrors adm_debug_resource_t in libretro-core.c.
    // Types 0-2 are Bartman's own (vscode-amiga-debug). Profile (3) is ours: the Crass engine
    // registers its raster-line profiler block through the same debug_cmd() call, so it rides the
    // existing registry with no core-side change - see core/CrassProfile.h.
    enum class DebugResourceType : uint16_t {
        Bitmap = 0,
        Palette = 1,
        Copperlist = 2,
        Profile = 3,
        Exocet = 4 // a Crass-engine program's ExoBlock, where it registers one (core/ExocetBlock.h)
    };
    struct DebugResource
    {
        uint32_t address;   // Amiga address of the pixel/colour/copper data
        uint32_t size;      // size in bytes (as computed Amiga-side)
        char name[32];      // caller-supplied label (NUL-terminated)
        uint16_t type;      // DebugResourceType
        uint16_t flags;     // bit0=interleaved bit1=masked bit2=ham (bitmaps)
        int16_t width;      // bitmap width in pixels
        int16_t height;     // bitmap height in rows
        int16_t numPlanes;  // bitmap depth
        int16_t numEntries; // palette entry count
        uint8_t active;     // always 1 for returned entries
        uint8_t pad[3];
    };
    static constexpr int kMaxDebugResources = 256;

    // Copies currently-registered resources into `out` (up to `max`); returns the count.
    // Returns 0 if the loaded core predates the Bartman export.
    int GetDebugResources(DebugResource* out, int max) const;
    // Monotonic counter that changes on any registry mutation — cheap "did anything change?".
    uint32_t GetDebugResourceGeneration() const;
    // Clears the registry (called on program load; also callable by the UI).
    void ClearDebugResources() const;
    bool HasBartmanDebug() const { return m_adm_get_debug_resources != nullptr; }

    // Opt-in: force the UAE boot rom on so the Bartman uaelib gate is installed. Applied to the
    // core immediately and re-asserted on every core/game load; a reset is needed for a runtime
    // toggle to take effect (rtarea is (re)installed at machine init).
    void SetBartmanDebugSupport(bool enabled);
    bool IsBartmanDebugSupportEnabled() const { return m_bartmanDebugEnabled; }

    // Diagnostic for the Bartman gate at rtarea_base+$FF60. `opcode` should read 0x4EB9 (JSR)
    // once the gate is installed — the value Bartman's debug_cmd() guard requires. Returns false
    // if the core predates the diagnostic export or no game is running.
    struct DebugGateInfo
    {
        uint32_t gateAddr = 0;
        uint32_t opcode = 0;
        int bootRomType = 0;
        int force = 0;
    };
    bool GetDebugGateInfo(DebugGateInfo& out) const;
    // True only when the loaded core actually exports the gate diagnostic. Distinguishes
    // "symbol missing from this DLL" from "no game running" so the UI can report the former.
    bool HasDebugGateInfo() const { return m_adm_get_debug_gate_info != nullptr; }

    // Names of the optional Bartman debug exports the loaded core is missing (empty when the
    // core exports the full set). Lets the Bartman window flag a stale/partial DLL explicitly
    // instead of silently degrading.
    std::vector<std::string> GetMissingBartmanSymbols() const;

    // Display masking (does not affect emulated machine state) — see core/DisplayMasks.h.
    // The emulator owns the masks so the Display window, the Sprites window, the AR console
    // and MCP all see the same state. Every setter re-pushes the composed masks to the core
    // and re-renders the current frame, so the change is visible (and captured by
    // CaptureScreenshotPNG) even while paused.
    void SetDisplayMasks(const core::DisplayMasks& masks);
    core::DisplayMasks GetDisplayMasks() const { return m_displayMasks; }
    void SetBitplaneMask(uint8_t mask);  // bit n = BPL(n+1) visible
    void SetSpriteMask(uint8_t mask);    // bit n = sprite n visible
    void SetPlayfieldMask(uint8_t mask); // bit 0 = playfield 1, bit 1 = playfield 2
    uint8_t GetBitplaneMask() const { return m_displayMasks.bitplanes; }
    uint8_t GetSpriteMask() const { return m_displayMasks.sprites; }
    uint8_t GetPlayfieldMask() const { return m_displayMasks.playfields; }
    void RequestRedraw();

    // Chipset-stage display debug options (see PATCHES.md, "palette-vis"). Owned here for the
    // same reason as the masks: several windows can change them and all must agree.
    //
    // Palette visualiser: replaces each scanline's decoded pen data with a ramp through every
    // pen, so the picture becomes a per-scanline palette chart. It acts in the drawing stage,
    // so a change is visible immediately, even while paused.
    //
    // BPLCON1 scroll: false forces the BPLCON1 delay/scroll fields to zero, so the bitplane
    // shifter stops applying fine horizontal scroll. Unlike the above it acts in the fetch
    // stage, so the picture only changes once the emulator runs another frame.
    void SetPaletteVisEnabled(bool enabled);
    bool GetPaletteVisEnabled() const { return m_paletteVisEnabled; }
    void SetBplcon1ScrollEnabled(bool enabled);
    bool GetBplcon1ScrollEnabled() const { return m_bplcon1ScrollEnabled; }
    // Both are optional exports: a core built before this patch resolves them to null, and the
    // UI greys the controls rather than pretending the toggle did something.
    bool HasPaletteVis() const { return m_adm_set_palette_vis != nullptr; }
    bool HasBplcon1Scroll() const { return m_adm_set_bplcon1_scroll != nullptr; }

    // Chipset write blocks (see core/DisplayBlocks.h). Copper-only write suppression over a
    // band of scanlines: the copper keeps running and the timing is unchanged, so what
    // disappears is exactly the copper's contribution to those lines. Both act at write time,
    // so a change only shows once the emulator runs another frame.
    void SetCopperBlock(const core::CopperBlock& block);
    core::CopperBlock GetCopperBlock() const { return m_copperBlock; }
    void SetBplPtrBlock(const core::BplPtrBlock& block);
    core::BplPtrBlock GetBplPtrBlock() const { return m_bplPtrBlock; }
    // Blitter destination writes. Unlike the two above this one is not copper-scoped and has
    // no line range: it gates every blitter D write, from every blit path.
    void SetBlitterWritesEnabled(bool enabled);
    bool GetBlitterWritesEnabled() const { return m_blitterWritesEnabled; }
    bool HasBlitterWrites() const { return m_adm_set_blitter_writes != nullptr; }
    bool HasChipsetBlocks() const
    {
        return m_adm_set_copper_block != nullptr && m_adm_set_bplptr_block != nullptr;
    }

    // Performance profiling
    static constexpr int kPerfHistorySize = 300; // ~6 s at 50 fps

    struct PerfSample
    {
        float coreMs = 0.0f;     // time for retro_run() in ms
        float rollingFps = 0.0f; // 1-second rolling emulated fps
    };

    struct PerfStats
    {
        float currentFps = 0.0f;
        float avgCoreMs = 0.0f;
        float demoFps = 0.0f;
        float idlePercent = 0.0f;
        bool demoAvailable = false;
    };

    PerfStats GetPerfStats() const;
    const PerfSample* GetPerfHistory(int& outCount, int& outHead) const;

    // The running program's own profiler numbers, when it is a Crass-engine build compiled with
    // `#define PROFILE` (core/CrassProfile.h). Unrelated to PerfStats' Demo fps / Idle %, which
    // are derived from Bartman idle events and need CPU trace on; these are exact, cost the
    // emulated machine nothing, and are read straight out of Amiga RAM.
    //
    // Lazy: nothing is read until something asks. The result is cached per core run, so several
    // callers in one host frame share a single read, and the type-3 registry lookup is redone
    // only when the registry generation moves. `present` false means "draw nothing".
    const core::CrassProfileStats& GetCrassProfileStats() const;

    // The running program's Exocet block, when it is a Crass-engine program with a sync
    // timeline (core/ExocetBlock.h): where its timeline is, every track's value, the arena a push
    // writes. Cached per core run like GetCrassProfileStats. A type-4 registry entry is used
    // where there is one; else the block is found by its magic in RAM (a disk that ships
    // registers nothing). `present` false: no block.
    const core::ExocetBlockState& GetExocetBlock() const;

    // The disk's memory map, when the running program's block has one (ENGINE_MEMMAP, block
    // version 2; core/CrassMemMap.h), laid over exec's MemList and free chunks. Cached per core
    // run like the block; a table caught mid-change comes back as it stood, marked `changing`.
    // `present` false: no table.
    const core::ExocetMemoryState& GetExocetMemory() const;

    // Total frames rendered since last boot
    uint64_t GetTotalFrameCount() const { return m_totalFrameCount; }
    // Re-base the counter onto a restored machine state. A save state does not carry this
    // host-side counter, so without it a rewind leaves every frame-keyed consumer (frame
    // history, disk profiler, timed input injections) pointing at the wrong frame.
    // Scheduled input is dropped: its release frames are absolute and now meaningless.
    void SetTotalFrameCount(uint64_t frame);
    // Monotonic core-run serial. Unlike the frame counter, this also advances for instruction
    // steps and partial-frame breakpoint returns, so debugger caches never miss fresh data.
    uint64_t GetRunCount() const { return m_totalRunCount; }

    // Keyboard support
    enum class RetroKey {
        Unknown = 0,
        Backspace = 8,
        Tab = 9,
        Clear = 12,
        Return = 13,
        Pause = 19,
        Escape = 27,
        Space = 32,
        Exclaim = 33,
        DoubleQuote = 34,
        Hash = 35,
        Dollar = 36,
        Ampersand = 38,
        Quote = 39,
        LeftParen = 40,
        RightParen = 41,
        Asterisk = 42,
        Plus = 43,
        Comma = 44,
        Minus = 45,
        Period = 46,
        Slash = 47,
        Num0 = 48,
        Num1 = 49,
        Num2 = 50,
        Num3 = 51,
        Num4 = 52,
        Num5 = 53,
        Num6 = 54,
        Num7 = 55,
        Num8 = 56,
        Num9 = 57,
        Colon = 58,
        Semicolon = 59,
        Less = 60,
        Equals = 61,
        Greater = 62,
        Question = 63,
        At = 64,
        LeftBracket = 91,
        Backslash = 92,
        RightBracket = 93,
        Caret = 94,
        Underscore = 95,
        Backquote = 96,
        A = 97,
        B = 98,
        C = 99,
        D = 100,
        E = 101,
        F = 102,
        G = 103,
        H = 104,
        I = 105,
        J = 106,
        K = 107,
        L = 108,
        M = 109,
        N = 110,
        O = 111,
        P = 112,
        Q = 113,
        R = 114,
        S = 115,
        T = 116,
        U = 117,
        V = 118,
        W = 119,
        X = 120,
        Y = 121,
        Z = 122,
        Delete = 127,
        Kp0 = 256,
        Kp1 = 257,
        Kp2 = 258,
        Kp3 = 259,
        Kp4 = 260,
        Kp5 = 261,
        Kp6 = 262,
        Kp7 = 263,
        Kp8 = 264,
        Kp9 = 265,
        KpPeriod = 266,
        KpDivide = 267,
        KpMultiply = 268,
        KpMinus = 269,
        KpPlus = 270,
        KpEnter = 271,
        KpEquals = 272,
        Up = 273,
        Down = 274,
        Right = 275,
        Left = 276,
        Insert = 277,
        Home = 278,
        End = 279,
        PageUp = 280,
        PageDown = 281,
        F1 = 282,
        F2 = 283,
        F3 = 284,
        F4 = 285,
        F5 = 286,
        F6 = 287,
        F7 = 288,
        F8 = 289,
        F9 = 290,
        F10 = 291,
        F11 = 292,
        F12 = 293,
        F13 = 294,
        F14 = 295,
        F15 = 296,
        NumLock = 300,
        CapsLock = 301,
        ScrollLock = 302,
        RShift = 303,
        LShift = 304,
        RCtrl = 305,
        LCtrl = 306,
        RAlt = 307,
        LAlt = 308,
        RMeta = 309,
        LMeta = 310,
        LSuper = 311,
        RSuper = 312,
        Mode = 313,
        Compose = 314,
        Help = 315,
        Print = 316,
        SysReq = 317,
        Break = 318,
        Menu = 319,
        Power = 320,
        Euro = 321,
        Undo = 322
    };

    void SetKeyState(RetroKey key, bool down);
    void SendKeyEvent(bool down, RetroKey key);

private:
    // Core function pointers
    void* m_libHandle = nullptr;

    typedef void (*retro_get_system_info_t)(struct retro_system_info* info);
    typedef void (*retro_get_system_av_info_t)(struct retro_system_av_info* info);
    typedef void (*retro_init_t)(void);
    typedef void (*retro_deinit_t)(void);
    typedef unsigned (*retro_api_version_t)(void);
    typedef void (*retro_set_environment_t)(bool (*cb)(unsigned cmd, void* data));
    typedef void (*retro_set_video_refresh_t)(void (*cb)(const void* data, unsigned width,
                                                         unsigned height, size_t pitch));
    typedef void (*retro_set_audio_sample_t)(void (*cb)(int16_t left, int16_t right));
    typedef void (*retro_set_audio_sample_batch_t)(size_t (*cb)(const int16_t* data,
                                                                size_t frames));
    typedef void (*retro_set_input_poll_t)(void (*cb)(void));
    typedef void (*retro_set_input_state_t)(int16_t (*cb)(unsigned port, unsigned device,
                                                          unsigned index, unsigned id));
    typedef bool (*retro_load_game_t)(const struct retro_game_info* game);
    typedef void (*retro_run_t)(void);
    typedef void (*retro_reset_t)(void);
    typedef void (*retro_unload_game_t)(void);
    typedef void* (*retro_get_memory_data_t)(unsigned id);
    typedef size_t (*retro_get_memory_size_t)(unsigned id);

    // Disk Control Interface
    typedef bool (*retro_set_eject_state_t)(bool ejected);
    typedef bool (*retro_get_eject_state_t)(void);
    typedef unsigned (*retro_get_image_index_t)(void);
    typedef bool (*retro_set_image_index_t)(unsigned index);
    typedef unsigned (*retro_get_num_images_t)(void);
    typedef bool (*retro_replace_image_index_t)(unsigned index, const struct retro_game_info* info);
    typedef bool (*retro_add_image_index_t)(const struct retro_game_info* info);

    struct retro_disk_control_callback
    {
        retro_set_eject_state_t set_eject_state;
        retro_get_eject_state_t get_eject_state;
        retro_get_image_index_t get_image_index;
        retro_set_image_index_t set_image_index;
        retro_get_num_images_t get_num_images;
        retro_replace_image_index_t replace_image_index;
        retro_add_image_index_t add_image_index;
    };

    typedef void (*retro_get_label_t)(unsigned index, char* label, size_t len);

    struct retro_disk_control_ext_callback
    {
        retro_set_eject_state_t set_eject_state;
        retro_get_eject_state_t get_eject_state;
        retro_get_image_index_t get_image_index;
        retro_set_image_index_t set_image_index;
        retro_get_num_images_t get_num_images;
        retro_replace_image_index_t replace_image_index;
        retro_add_image_index_t add_image_index;

        retro_get_label_t get_label;
    };

    retro_init_t m_retro_init = nullptr;
    retro_deinit_t m_retro_deinit = nullptr;
    retro_api_version_t m_retro_api_version = nullptr;
    retro_get_system_info_t m_retro_get_system_info = nullptr;
    retro_get_system_av_info_t m_retro_get_system_av_info = nullptr;
    retro_set_environment_t m_retro_set_environment = nullptr;
    retro_set_video_refresh_t m_retro_set_video_refresh = nullptr;
    retro_set_audio_sample_t m_retro_set_audio_sample = nullptr;
    retro_set_audio_sample_batch_t m_retro_set_audio_sample_batch = nullptr;
    retro_set_input_poll_t m_retro_set_input_poll = nullptr;
    retro_set_input_state_t m_retro_set_input_state = nullptr;
    retro_set_controller_port_device_t m_retro_set_controller_port_device = nullptr;
    retro_load_game_t m_retro_load_game = nullptr;
    retro_run_t m_retro_run = nullptr;
    retro_reset_t m_retro_reset = nullptr;
    retro_unload_game_t m_retro_unload_game = nullptr;
    retro_get_memory_data_t m_retro_get_memory_data = nullptr;
    retro_get_memory_size_t m_retro_get_memory_size = nullptr;
    retro_serialize_size_t m_retro_serialize_size = nullptr;
    retro_serialize_t m_retro_serialize = nullptr;
    retro_unserialize_t m_retro_unserialize = nullptr;

    typedef void (*adm_get_cpu_state_t)(uint32_t* regs_out, uint32_t* pc, uint16_t* sr);
    adm_get_cpu_state_t m_adm_get_cpu_state = nullptr;
    typedef int (*adm_get_cpu_ext_state_t)(core::CpuExtRaw* out, int size);
    adm_get_cpu_ext_state_t m_adm_get_cpu_ext_state = nullptr;

    typedef uint32_t (*adm_disasm_one_t)(uint32_t pc, char* mnemoBuf, int mnemoSize, char* hexBuf,
                                         int hexSize);
    adm_disasm_one_t m_adm_disasm_one = nullptr;

    typedef int (*adm_asm_one_t)(const char* src, uint32_t pc, uint16_t* outWords, int maxWords);
    adm_asm_one_t m_adm_asm_one = nullptr;

    typedef int (*adm_debug_command_t)(const char* cmd, char* output_buf, int buf_len);
    adm_debug_command_t m_adm_debug_command = nullptr;

    typedef int (*adm_get_debug_break_pending_t)(void);
    adm_get_debug_break_pending_t m_adm_get_debug_break_pending = nullptr;

    typedef void (*adm_arm_debug_t)(void);
    adm_arm_debug_t m_adm_arm_debug = nullptr;

    typedef int (*adm_arm_oneshot_breakpoint_t)(uint32_t pc, uint32_t instructionBudget);
    adm_arm_oneshot_breakpoint_t m_adm_arm_oneshot_breakpoint = nullptr;
    typedef void (*adm_cancel_oneshot_breakpoint_t)(void);
    adm_cancel_oneshot_breakpoint_t m_adm_cancel_oneshot_breakpoint = nullptr;
    typedef int (*adm_get_oneshot_breakpoint_status_t)(uint32_t* stopPc, uint32_t* executed);
    adm_get_oneshot_breakpoint_status_t m_adm_get_oneshot_breakpoint_status = nullptr;

    // Break-on-bootblock-entry (FB) trap
    typedef void (*adm_arm_fb_t)(int on);
    adm_arm_fb_t m_adm_arm_fb = nullptr;
    typedef int (*adm_is_fb_armed_t)(void);
    adm_is_fb_armed_t m_adm_is_fb_armed = nullptr;
    typedef uint32_t (*adm_get_fb_entry_t)(void);
    adm_get_fb_entry_t m_adm_get_fb_entry = nullptr;
    typedef int (*adm_get_fb_hit_t)(void);
    adm_get_fb_hit_t m_adm_get_fb_hit = nullptr;

    // Break-on-CPU-exception (EB)
    typedef void (*adm_arm_exception_t)(uint32_t mask_lo, uint32_t mask_hi);
    adm_arm_exception_t m_adm_arm_exception = nullptr;
    typedef int (*adm_get_exception_hit_t)(int* nr);
    adm_get_exception_hit_t m_adm_get_exception_hit = nullptr;

    // Break-on-beam-position (BL)
    typedef void (*adm_arm_beam_break_t)(int vpos, int hpos);
    adm_arm_beam_break_t m_adm_arm_beam_break = nullptr;
    typedef int (*adm_get_beam_hit_t)(void);
    adm_get_beam_hit_t m_adm_get_beam_hit = nullptr;

    // Memory watchpoint hit accessor (read-and-clear in the core)
    typedef int (*adm_get_last_memwatch_t)(uint32_t* pc, uint32_t* addr, uint32_t* val, int* rwi,
                                           int* size);
    adm_get_last_memwatch_t m_adm_get_last_memwatch = nullptr;
    // Extended accessor (newer cores): also returns the triggering access mask + register.
    // Optional — falls back to m_adm_get_last_memwatch when absent.
    typedef int (*adm_get_last_memwatch2_t)(uint32_t* pc, uint32_t* addr, uint32_t* val, int* rwi,
                                            int* size, uint32_t* access_mask, uint32_t* reg);
    adm_get_last_memwatch2_t m_adm_get_last_memwatch2 = nullptr;
    // Host-side latch of the last memwatch hit, refreshed on every break so it can be
    // read by more than one consumer while paused. mutable: latched from the const
    // IsDebugBreakPending() query (which already drains core read-and-clear flags).
    mutable MemwatchHit m_latchedMemwatch{};
    mutable bool m_hasLatchedMemwatch = false;

    typedef void (*adm_set_bitplane_mask_t)(uint8_t mask);
    adm_set_bitplane_mask_t m_adm_set_bitplane_mask = nullptr;

    typedef void (*adm_set_sprite_mask_t)(uint8_t mask);
    adm_set_sprite_mask_t m_adm_set_sprite_mask = nullptr;

    typedef void (*adm_set_palette_vis_t)(uint8_t enabled);
    adm_set_palette_vis_t m_adm_set_palette_vis = nullptr;

    // int, not uint8_t: this one is defined in custom.c (it needs the file-static
    // bplcon1_written), where uint8_t is not in scope.
    typedef void (*adm_set_bplcon1_scroll_t)(int enabled);
    adm_set_bplcon1_scroll_t m_adm_set_bplcon1_scroll = nullptr;

    // Both live in custom.c alongside the state they gate, so they take plain ints.
    typedef void (*adm_set_copper_block_t)(int enabled, int startLine, int endLine);
    adm_set_copper_block_t m_adm_set_copper_block = nullptr;

    typedef void (*adm_set_bplptr_block_t)(int mask, int startLine, int endLine);
    adm_set_bplptr_block_t m_adm_set_bplptr_block = nullptr;

    typedef void (*adm_set_blitter_writes_t)(uint8_t enabled);
    adm_set_blitter_writes_t m_adm_set_blitter_writes = nullptr;

    // Host-side source of truth for the debug display masks; re-pushed to the core whenever it
    // changes and after a core load, so the selection survives loading a new program.
    core::DisplayMasks m_displayMasks{};
    void PushDisplayMasks();

    // Same contract for the chipset display options. Defaults match the hardware: no palette
    // ramp, BPLCON1 scroll honoured.
    bool m_paletteVisEnabled = false;
    bool m_bplcon1ScrollEnabled = true;
    core::CopperBlock m_copperBlock{};
    core::BplPtrBlock m_bplPtrBlock{};
    bool m_blitterWritesEnabled = true;
    void PushDisplayOptions();

    // PuAE exposes bytes written to the emulated serial port through this drain API.
    // KPrintF uses that port, so assemble the byte stream into Debug-level LogView lines.
    typedef int (*adm_drain_serial_output_t)(char* out, int maxLen);
    adm_drain_serial_output_t m_adm_drain_serial_output = nullptr;
    std::string m_serialLogLine;
    bool m_serialLastWasCr = false;
    void DrainSerialOutput();
    void FlushSerialLogLine();

    // Break-on-KPrintF-text state. FlushSerialLogLine() offers each completed line to the rules;
    // a match arms m_kprintfBreakPending, which IsDebugBreakPending() reports (and clears) so
    // EmulatorView pauses on the frame that produced the text. mutable: both are drained from
    // the const IsDebugBreakPending() / ConsumeKprintfHit() queries.
    core::KprintfBreakpoints m_kprintfBreakpoints;
    mutable bool m_kprintfBreakPending = false;
    // m_hasKprintfHit is consumable (ConsumeKprintfHit, one announcement per hit);
    // m_hasLatchedKprintf is sticky so the UI can keep showing the last match.
    mutable bool m_hasKprintfHit = false;
    mutable bool m_hasLatchedKprintf = false;
    core::KprintfHit m_kprintfHit;

    typedef void (*adm_redraw_frame_t)(void);
    adm_redraw_frame_t m_adm_redraw_frame = nullptr;

    typedef int (*adm_get_bplpt_events_t)(void* out, int max);
    adm_get_bplpt_events_t m_adm_get_bplpt_events = nullptr;

    typedef void (*adm_clear_bplpt_events_t)(void);
    adm_clear_bplpt_events_t m_adm_clear_bplpt_events = nullptr;

    typedef int (*adm_get_sprpt_events_t)(void* out, int max);
    adm_get_sprpt_events_t m_adm_get_sprpt_events = nullptr;

    // Optional — only present in DLLs rebuilt with the colour-snapshot patch.
    typedef int (*adm_get_sprpt_color_snaps_t)(void* out, int max);
    adm_get_sprpt_color_snaps_t m_adm_get_sprpt_color_snaps = nullptr;

    typedef void (*adm_clear_sprpt_events_t)(void);
    adm_clear_sprpt_events_t m_adm_clear_sprpt_events = nullptr;

    typedef int (*adm_get_blit_events_t)(void* out, int max);
    adm_get_blit_events_t m_adm_get_blit_events = nullptr;

    typedef void (*adm_clear_blit_events_t)(void);
    adm_clear_blit_events_t m_adm_clear_blit_events = nullptr;

    // Blitter destination data capture
    typedef void (*adm_set_blit_data_enabled_t)(int);
    adm_set_blit_data_enabled_t m_adm_set_blit_data_enabled = nullptr;

    typedef int (*adm_get_blit_data_enabled_t)(void);
    adm_get_blit_data_enabled_t m_adm_get_blit_data_enabled = nullptr;

    typedef int (*adm_get_blit_data_t)(uint32_t, uint32_t, void*);
    adm_get_blit_data_t m_adm_get_blit_data = nullptr;

    // Fallback for a core that exports the setter but not the getter.
    bool m_blitDataCaptureEnabled = false;

    // Blitter-vis (optional — absent on cores predating the provenance patch)
    typedef void (*adm_set_blitter_vis_enabled_t)(int);
    adm_set_blitter_vis_enabled_t m_adm_set_blitter_vis_enabled = nullptr;

    typedef int (*adm_get_blitter_vis_spans_t)(void* out, int max);
    adm_get_blitter_vis_spans_t m_adm_get_blitter_vis_spans = nullptr;

    typedef void (*adm_get_blitter_vis_stats_t)(int*, int*, int*, int*);
    adm_get_blitter_vis_stats_t m_adm_get_blitter_vis_stats = nullptr;

    typedef int (*adm_get_idle_events_t)(void* out, int max);
    adm_get_idle_events_t m_adm_get_idle_events = nullptr;

    typedef void (*adm_clear_idle_events_t)(void);
    adm_clear_idle_events_t m_adm_clear_idle_events = nullptr;

    typedef int (*adm_get_copper_events_t)(void* out, int max, int* overflow);
    adm_get_copper_events_t m_adm_get_copper_events = nullptr;

    typedef void (*adm_clear_copper_events_t)(void);
    adm_clear_copper_events_t m_adm_clear_copper_events = nullptr;

    // Custom-register write log — optional, so a core predating the patch still loads
    // (HasRegWriteLog() reports false and the Register Operations window says so).
    typedef int (*adm_get_reg_write_events_t)(void* out, int max, int* overflow);
    adm_get_reg_write_events_t m_adm_get_reg_write_events = nullptr;

    typedef void (*adm_clear_reg_write_events_t)(void);
    adm_clear_reg_write_events_t m_adm_clear_reg_write_events = nullptr;

    typedef void (*adm_set_cpu_trace_enabled_t)(int);
    adm_set_cpu_trace_enabled_t m_adm_set_cpu_trace_enabled = nullptr;

    typedef int (*adm_get_cpu_trace_t)(void* out, int max, int* overflow);
    adm_get_cpu_trace_t m_adm_get_cpu_trace = nullptr;
    // Exact completed-frame export added in ADM PuAE 1.16. Optional so older patched
    // cores retain the continuous-ring + CCK-filtering path.
    adm_get_cpu_trace_t m_adm_get_cpu_trace_frame = nullptr;

    typedef void (*adm_clear_cpu_trace_t)(void);
    adm_clear_cpu_trace_t m_adm_clear_cpu_trace = nullptr;

    typedef const char* (*adm_get_version_t)(void);
    adm_get_version_t m_adm_get_version = nullptr;

    typedef int (*adm_drain_hd_activity_t)(void*, int);
    adm_drain_hd_activity_t m_adm_drain_hd_activity = nullptr;
    typedef int (*adm_get_active_filesystem_units_t)(int*, int);
    adm_get_active_filesystem_units_t m_adm_get_active_filesystem_units = nullptr;
    // Media exports (core 1.84.0). Optional: an older core leaves them null and the features
    // that need them are shown disabled.
    typedef int (*adm_whdload_prepare_t)(int, int, int, int, char*, char*, int);
    adm_whdload_prepare_t m_adm_whdload_prepare = nullptr;
    typedef int (*adm_cd_insert_t)(const char*);
    adm_cd_insert_t m_adm_cd_insert = nullptr;
    typedef void (*adm_cd_eject_t)(void);
    adm_cd_eject_t m_adm_cd_eject = nullptr;
    typedef int (*adm_cd_get_image_t)(char*, int);
    adm_cd_get_image_t m_adm_cd_get_image = nullptr;
    typedef int (*adm_extract_archive_t)(const char*, const char*);
    adm_extract_archive_t m_adm_extract_archive = nullptr;
    typedef int (*adm_identify_rom_t)(const char* path, const char* keydir, char* name,
                                      int nameSize, char* model, int modelSize, int* ver, int* rev,
                                      int* subver, int* subrev, unsigned* crc32, unsigned* size,
                                      unsigned* type, int* cloanto);
    adm_identify_rom_t m_adm_identify_rom = nullptr;
    // RTG / Picasso96 (core 1.99.0).
    typedef void (*adm_get_rtg_state_t)(uint32_t* boardSize, uint32_t* width, uint32_t* height,
                                        uint32_t* depth, uint8_t* active);
    adm_get_rtg_state_t m_adm_get_rtg_state = nullptr;
    // ExtractArchive() runs on whatever host thread calls it (the CD Drive window's extraction
    // worker) and executes inside the DLL. UnloadCore() frees that DLL, so it waits here for any
    // such call to return, and refuses new ones from the moment it decides to unload until
    // LoadCore() has a core again.
    mutable std::mutex m_offThreadCoreMutex;
    mutable std::condition_variable m_offThreadCoreIdle;
    mutable int m_offThreadCoreCalls = 0;
    bool m_offThreadCoreBlocked = false;
    // Machine configuration (core 1.86.0).
    typedef int (*adm_get_machine_config_t)(int*, int);
    adm_get_machine_config_t m_adm_get_machine_config = nullptr;
    // Serial and parallel ports (core 1.89.0).
    typedef int (*adm_serial_set_port_t)(const char*);
    adm_serial_set_port_t m_adm_serial_set_port = nullptr;
    typedef int (*adm_parallel_set_output_t)(const char*);
    adm_parallel_set_output_t m_adm_parallel_set_output = nullptr;
    typedef int (*adm_hostio_get_status_t)(void*, int);
    adm_hostio_get_status_t m_adm_hostio_get_status = nullptr;
    typedef int (*adm_midi_set_devices_t)(int, int);
    adm_midi_set_devices_t m_adm_midi_set_devices = nullptr;
    int m_midiOutDevice = -2;
    int m_midiInDevice = -1;
    bool m_midiDevicesSet = false;
    core::SerialPortTarget m_serialPort;
    bool m_serialPortSet = false;
    std::filesystem::path m_parallelOutput;
    bool m_parallelOutputSet = false;
    bool m_reportedAggregateHdFallback = false;

    typedef uint32_t (*adm_get_frame_start_cycles_t)(void);
    adm_get_frame_start_cycles_t m_adm_get_frame_start_cycles = nullptr;
    // Current beam position (optional; older cores lack it — partial-frame view then disabled).
    typedef void (*adm_get_beampos_t)(uint32_t* cycle, int* vpos, int* hpos);
    adm_get_beampos_t m_adm_get_beampos = nullptr;

    // Frame-start save-state capture. PUAE restores a state at vpos 0 but retro_serialize()
    // runs where retro_run() returned (vpos 3), so a state taken the ordinary way comes back
    // with the chipset rewound behind the CPU. These let the core take the state inside its
    // own vpos = 0 reset instead. Optional: a core built before 1.29.0 lacks them.
    typedef void (*adm_set_state_capture_t)(int enable);
    adm_set_state_capture_t m_adm_set_state_capture = nullptr;
    typedef size_t (*adm_get_frame_start_state_t)(void* dst, size_t max, uint64_t* seq);
    adm_get_frame_start_state_t m_adm_get_frame_start_state = nullptr;
    // Sequence of the capture most recently handed out, so a stale one is never reused.
    uint64_t m_lastFrameStartStateSeq = 0;
    bool m_warnedNoFrameStartCapture = false;
    // What the core was last *told*, not what we would like it to be. Kept separate so the
    // per-redraw arming call is a comparison rather than a DLL call, and reset explicitly at
    // every game load - a first load skips UnloadCore(), so a mirror that is only cleared
    // there goes stale and the core silently keeps the previous session's setting.
    bool m_stateCaptureArmed = false;
    bool m_restorableStateRequested = false;

    typedef void (*adm_get_display_geometry_t)(int*, int*, int*, int*, int*, int*);
    adm_get_display_geometry_t m_adm_get_display_geometry = nullptr;

    // DMA debug record access
    typedef int (*adm_get_dma_record_t)(void** out_ptr, int* out_hpos, int* out_vpos);
    adm_get_dma_record_t m_adm_get_dma_record = nullptr;
    adm_get_dma_record_t m_adm_get_dma_record_live = nullptr; // optional (newer cores)

    typedef void (*adm_set_debug_dma_t)(int mode);
    adm_set_debug_dma_t m_adm_set_debug_dma = nullptr;

    typedef int (*adm_get_debug_dma_t)(void);
    adm_get_debug_dma_t m_adm_get_debug_dma = nullptr;

    // Paula channel telemetry — all optional, so a core predating the audio patch still loads
    // (HasAudioDebug() reports false and the Audio window says so).
    typedef int (*adm_get_audio_trace_t)(int ch, void* out, int max);
    adm_get_audio_trace_t m_adm_get_audio_trace = nullptr;

    typedef void (*adm_clear_audio_trace_t)(void);
    adm_clear_audio_trace_t m_adm_clear_audio_trace = nullptr;

    typedef void (*adm_set_audio_trace_enabled_t)(int enable);
    adm_set_audio_trace_enabled_t m_adm_set_audio_trace_enabled = nullptr;

    typedef void (*adm_set_audio_channel_mask_t)(uint8_t mask);
    adm_set_audio_channel_mask_t m_adm_set_audio_channel_mask = nullptr;

    typedef uint8_t (*adm_get_audio_channel_mask_t)(void);
    adm_get_audio_channel_mask_t m_adm_get_audio_channel_mask = nullptr;

    typedef int (*adm_get_audio_filter_state_t)(void);
    adm_get_audio_filter_state_t m_adm_get_audio_filter_state = nullptr;

    typedef int (*adm_get_disk_write_events_t)(void*, int);
    adm_get_disk_write_events_t m_adm_get_disk_write_events = nullptr;

    typedef void (*adm_clear_disk_write_events_t)(void);
    adm_clear_disk_write_events_t m_adm_clear_disk_write_events = nullptr;

    typedef int (*adm_read_disk_track_t)(int, int, uint8_t*, int);
    adm_read_disk_track_t m_adm_read_disk_track = nullptr;

    // FloppyBridge configuration. Resolved optionally: a core predating these
    // exports still loads, and HasBridgeSupport() reports false.
    typedef int (*adm_set_drive_bridge_config_t)(int, const char*);
    adm_set_drive_bridge_config_t m_adm_set_drive_bridge_config = nullptr;

    typedef int (*adm_clear_drive_bridge_config_t)(int);
    adm_clear_drive_bridge_config_t m_adm_clear_drive_bridge_config = nullptr;

    typedef int (*adm_get_bridge_driver_list_t)(char*, int);
    adm_get_bridge_driver_list_t m_adm_get_bridge_driver_list = nullptr;

    typedef int (*adm_enum_bridge_com_ports_t)(char*, int);
    adm_enum_bridge_com_ports_t m_adm_enum_bridge_com_ports = nullptr;

    typedef int (*adm_get_drive_bridge_status_t)(int, void*);
    adm_get_drive_bridge_status_t m_adm_get_drive_bridge_status = nullptr;

    // Optional (PuAE >= 1.81.0): synchronous teardown of every live drive.
    typedef int (*adm_shutdown_drive_bridges_t)(void);
    adm_shutdown_drive_bridges_t m_adm_shutdown_drive_bridges = nullptr;

    // Per-drive write protect. Resolved optionally, like the bridge exports.
    typedef int (*adm_set_floppy_write_protect_t)(int, int);
    adm_set_floppy_write_protect_t m_adm_set_floppy_write_protect = nullptr;

    typedef int (*adm_get_floppy_write_protect_t)(int);
    adm_get_floppy_write_protect_t m_adm_get_floppy_write_protect = nullptr;

    // What the host last asked for per slot. Images open protected, so does a slot.
    bool m_floppyWriteProtect[4] = {true, true, true, true};

    // Pushes m_floppyWriteProtect into the core. Called before every retro_run()
    // because the core clears the flag on its own from several paths - see the
    // definition.
    void AssertFloppyWriteProtect();

    // Times the core was found to have dropped a slot's protection. Reset with the core.
    uint32_t m_floppyWriteProtectDrift[4] = {0, 0, 0, 0};

    // One warning per core is enough when the exports are missing entirely.
    bool m_warnedNoWriteProtectSupport = false;

    // Mirror of what AttachBridgeDrive()/DetachBridgeDrive() last requested per
    // slot. The core exports no query, and gui_data cannot distinguish a bridged
    // slot from an empty file-backed one.
    bool m_bridgeDrive[4] = {false, false, false, false};

    // Disk DMA log + breakpoint. Resolved optionally: a core predating these
    // exports still loads, and the feature reports itself unavailable instead.
    typedef int (*adm_get_disk_dma_events_t)(void*, int);
    adm_get_disk_dma_events_t m_adm_get_disk_dma_events = nullptr;

    typedef void (*adm_clear_disk_dma_events_t)(void);
    adm_clear_disk_dma_events_t m_adm_clear_disk_dma_events = nullptr;

    typedef int (*adm_get_disk_dma_payload_t)(uint32_t, uint32_t, void*);
    adm_get_disk_dma_payload_t m_adm_get_disk_dma_payload = nullptr;

    typedef void (*adm_arm_disk_break_t)(int, int, int, int, uint32_t, uint32_t);
    adm_arm_disk_break_t m_adm_arm_disk_break = nullptr;

    typedef int (*adm_is_disk_break_armed_t)(void);
    adm_is_disk_break_armed_t m_adm_is_disk_break_armed = nullptr;

    typedef int (*adm_get_disk_break_hit_t)(void*);
    adm_get_disk_break_hit_t m_adm_get_disk_break_hit = nullptr;

    // CIA state snapshot (adm_get_cia_state from cia.c patch)
    struct adm_cia_chip_t
    {
        uint8_t pra, prb, dra, drb;
        uint8_t talo, tahi, tblo, tbhi;
        uint8_t todlo, todmid, todhi;
        uint8_t sdr, icr, imask, cra, crb;
    };
    struct adm_cia_state_t
    {
        adm_cia_chip_t a;
        adm_cia_chip_t b;
    };
    typedef void (*adm_get_cia_state_t)(adm_cia_state_t* out);
    adm_get_cia_state_t m_adm_get_cia_state = nullptr;

    // Bartman debug resource registry (optional — present only in newer cores)
    typedef int (*adm_get_debug_resources_t)(void* out, int max);
    adm_get_debug_resources_t m_adm_get_debug_resources = nullptr;
    typedef uint32_t (*adm_get_debug_resource_generation_t)(void);
    adm_get_debug_resource_generation_t m_adm_get_debug_resource_generation = nullptr;
    typedef void (*adm_clear_debug_resources_t)(void);
    adm_clear_debug_resources_t m_adm_clear_debug_resources = nullptr;
    typedef void (*adm_set_force_boot_rom_t)(int on);
    adm_set_force_boot_rom_t m_adm_set_force_boot_rom = nullptr;
    typedef void (*adm_get_debug_gate_info_t)(uint32_t*, uint32_t*, int*, int*);
    adm_get_debug_gate_info_t m_adm_get_debug_gate_info = nullptr;
    bool m_bartmanDebugEnabled = false;

    retro_disk_control_callback m_diskControl = {};
    retro_disk_control_ext_callback m_diskControlExt = {};

    // State
    bool m_coreLoaded = false;
    bool m_gameLoaded = false;
    std::string m_coreVersionString;
    bool m_driveEnabled[4] = {true, true, false, false};
    std::string m_systemDir;
    std::string m_saveDir;
    bool m_ownsSaveDir = false;
    std::string m_tempAdfPath;
    std::string m_tempAdfPaths[4]; // Per-drive temp ADF paths
    std::string m_hostLaunchWorkspacePath;
    core::HostMountService m_hostMountService;
    std::filesystem::path m_configuredCdImage;
    bool m_cdDriveInMachine = false;
    AmigaModel m_currentModel = AmigaModel::A500;
    std::unordered_map<std::string, std::string> m_coreVariables; // key -> default value
    // Everything the core declared, overridden keys included, so the option schema can be
    // checked against the core that is actually loaded.
    std::unordered_map<std::string, CoreOptionDeclaration> m_coreOptionDeclarations;
    std::unordered_map<std::string, std::string>
        m_userVariables; // user overrides (highest priority)
    // Backing store for the string handed back through RETRO_ENVIRONMENT_GET_VARIABLE; the core
    // only reads it before the next call.
    std::string m_variableScratch;
    bool m_variablesUpdated = false;

    // Path of the currently loaded UAE config file (empty when using default model settings)
    std::string m_uaeConfigPath;
    VideoResolution m_videoResolution = VideoResolution::Lores;

    // Video
    GLuint m_textureID = 0;
    unsigned int m_width = 320;
    unsigned int m_height = 256;
    bool m_acceptVideoRefresh = true;

    // Storage spec the framebuffer texture was last (re)allocated with. When the incoming frame
    // matches, the per-frame path uses glTexSubImage2D instead of reallocating via glTexImage2D.
    int m_texAllocW = 0;
    int m_texAllocH = 0;
    unsigned int m_texAllocFormat = 0; // GLenum
    unsigned int m_texAllocType = 0;   // GLenum
    unsigned int m_maxWidth = 720;
    unsigned int m_maxHeight = 576;
    float m_aspectRatio = 4.0f / 3.0f;
    std::vector<uint8_t> m_videoBuffer;
    retro_pixel_format m_pixelFormat = RETRO_PIXEL_FORMAT_UNKNOWN;

    // Scrub-preview thumbnails. Heap-backed on purpose: this class is already megabytes and
    // is stack-allocated in places, so per-frame buffers must not grow the object.
    bool m_thumbEnabled = false;
    int m_thumbInterval = 10;
    int m_thumbCountdown = 0;
    uint64_t m_thumbSeq = 0;
    int m_thumbW = 0;
    int m_thumbH = 0;
    std::vector<uint8_t> m_thumbRGB;
    // Downsamples one incoming frame into m_thumbRGB and bumps m_thumbSeq.
    void CaptureThumbnail(const void* data, unsigned width, unsigned height, size_t pitch);

    // Timing
    double m_targetFps = 50.0; // PAL default
    double m_speedMultiplier = 1.0;
    std::chrono::steady_clock::time_point m_lastFrameTime;
    double m_frameAccumulator = 0.0;
    bool m_debugHarvestSuppressed = false;

    // Performance profiling
    PerfSample m_perfHistory[kPerfHistorySize] = {};
    int m_perfHistoryHead = 0;  // next-write index (circular)
    int m_perfHistoryCount = 0; // valid samples (0..kPerfHistorySize)
    int m_fpsFrameCount = 0;
    uint64_t m_totalFrameCount = 0;
    uint64_t m_totalRunCount = 0;
    std::chrono::steady_clock::time_point m_fpsTimer;
    float m_rollingFps = 0.0f;
    float m_lastCoreMs = 0.0f;

    // Demo timing derived from Bartman idle markers. A start marker is the point where the CPU
    // finishes one logical demo frame; the following stop marker is where CPU work resumes.
    static constexpr int kDemoPerfWindowFrames = 50;
    struct DemoPerfInterval
    {
        uint32_t totalCycles = 0;
        uint32_t idleCycles = 0;
    };
    DemoPerfInterval m_demoPerfIntervals[kDemoPerfWindowFrames] = {};
    int m_demoPerfHead = 0;
    int m_demoPerfCount = 0;
    uint64_t m_demoTotalCycles = 0;
    uint64_t m_demoIdleCycles = 0;
    uint64_t m_demoTimingCycles = 0;
    uint64_t m_demoTimingFrames = 0;
    uint64_t m_demoAccumulatedIdleCycles = 0;
    uint64_t m_demoIdleAtLastFrameEnd = 0;
    uint32_t m_demoLastFrameEndCycle = 0;
    uint32_t m_demoIdleStartCycle = 0;
    bool m_demoHaveFrameEnd = false;
    bool m_demoIdleActive = false;
    bool m_demoSawIdleStop = false;
    bool m_demoPerfAvailable = false;
    float m_demoFps = 0.0f;
    float m_demoIdlePercent = 0.0f;

    // Audio
    ma_device* m_audioDevice = nullptr;
    void* m_audioResampler = nullptr; // ma_resampler*, opaque to avoid header include
    // Read by the miniaudio callback thread, written on the main thread.
    std::atomic<bool> m_audioInitialized{false};
    bool m_needsResampling = false;
    double m_coreSampleRate = 44100.0;
    uint32_t m_deviceSampleRate = 44100;
    uint32_t m_audioInitCoreRate = 0; // core rate the live device was built for (0 = none)
    static constexpr size_t kAudioRingSize = 32768; // frames (each frame = 2 int16 samples)
    float m_audioRing[kAudioRingSize * 2] = {};     // interleaved stereo float
    std::atomic<size_t> m_audioWritePos{0};
    std::atomic<size_t> m_audioReadPos{0};

    // Target ring occupancy before the device starts consuming, and again after any underrun.
    // The core delivers audio in uneven bursts (it flushes whenever its internal buffer fills, so
    // block size depends on what the emulated program is doing), while the sound card consumes at
    // a fixed rate. Without a cushion the ring sits near empty and starves constantly.
    uint32_t m_audioTargetFrames = 0;
    std::atomic<bool> m_audioPrimed{false};
    // Keeps the ring at that target: anything queued beyond it is latency, not cushion. Audio
    // thread only, except InitAudio() resets it while the device is stopped.
    core::AudioCushion m_audioCushion;
    // The device's own queue, as opened. Diagnostic only.
    uint32_t m_audioDevicePeriodFrames = 0;
    uint32_t m_audioDeviceBufferFrames = 0;

    // Windows: the core's audio written straight into the WASAPI queue in place of the miniaudio
    // device and the ring in front of it (see WasapiPushOutput). Main thread only.
    std::unique_ptr<core::WasapiPushOutput> m_pushOut;
    bool m_pushActive = false;
    double m_pushQueuedEma = 0.0; // the queue just before each write, smoothed, for pacing
    std::chrono::steady_clock::time_point m_pushLastWrite{};
    std::chrono::steady_clock::time_point m_pushLastDeviceCheck{};
    std::chrono::steady_clock::time_point m_pushLastReopen{};
    std::vector<int16_t> m_pushScratch;
    std::vector<int16_t> m_pushGainScratch; // a block at the volume's gain
    std::vector<float> m_pushTapScratch;
    // The last frame queued, so a stop can fade from it (EndPushStream).
    int16_t m_pushLastL = 0;
    int16_t m_pushLastR = 0;
    bool m_pushTail = false;
    // The machine stopped advancing: queue a short fade to silence after the last sample.
    void EndPushStream();
    bool OpenPushOutput(uint32_t coreRate);
    void ReopenPushOutput();
    // The cushion's target and range for the push output's period, on an open and a reopen
    void ConfigurePushCushion(uint32_t coreRate);
    void PushAudio(const int16_t* data, size_t frames);
    // Whether the push queue was written recently enough to count as playing - an empty queue
    // then is a starve, not a fresh start after a pause.
    bool PushPlaying(std::chrono::steady_clock::time_point now) const;
    // Last genuine output sample, so an underrun can fade from it instead of stepping to silence.
    // Written by the audio thread; InitAudio() also resets them from the main thread, so atomic.
    std::atomic<float> m_audioLastOutL{0.0f};
    std::atomic<float> m_audioLastOutR{0.0f};

    // Audio debug taps. m_tapCore is fed on the main thread, m_tapDevice on the miniaudio
    // callback thread; both stage into their own lock-free ring and are written by
    // DrainAudioRecorders() on the main thread.
    core::AudioRecorder m_tapCore;
    core::AudioRecorder m_tapDevice;

    // Video capture. Both streams are written from the main thread inside the frame loop: the
    // audio as the core delivers it in WriteAudioSamples(), the picture at the frame boundary.
    std::unique_ptr<core::VideoCaptureSink> m_videoCapture;
    // Stopped FFmpeg recordings still muxing, kept alive until they are done, with the reason
    // they stopped (if it was not the user) to put in front of their result.
    struct FinishingCapture
    {
        std::unique_ptr<core::VideoCaptureSink> sink;
        std::string reason;
    };
    std::vector<FinishingCapture> m_finishingCaptures;
    // The recording that ended last, kept so its frame count, size and path can still be read.
    std::unique_ptr<core::VideoCaptureSink> m_lastCapture;
    std::string m_videoCaptureStatus;
    // The frame the core last handed us, converted to RGBA. Filled in VideoRefreshCallback from
    // the raw framebuffer - NOT from CaptureFrameRGBA, which reads back the GL texture: that
    // needs a context (so it captures nothing headless) and a per-frame glGetTexImage would
    // stall the pipeline. Written out at the frame boundary, once the frame is known to be whole.
    std::vector<uint8_t> m_captureFrameScratch;
    bool m_captureFramePending = false;
    int m_captureFrameWidth = 0;
    int m_captureFrameHeight = 0;
    // The size the film was started at, for a recording of the frame itself (not composed)
    int m_captureSourceWidth = 0;
    int m_captureSourceHeight = 0;
    uint32_t m_captureFramesRepeated = 0; // dupes written as the last picture again
    bool m_captureArmedDebugDma = false;  // ArmDebugDmaForCapture turned debug_dma on
    // The recording's composer (VideoCaptureOptions::compose) and the picture it makes; empty
    // for a film of the frame itself.
    std::function<bool(const VideoCaptureOptions::ComposeSource&, std::vector<uint8_t>&)>
        m_videoCompose;
    std::vector<uint8_t> m_videoComposed;
    void CaptureVideoFrame(const void* data, unsigned width, unsigned height, size_t pitch);
    // Writes the frame just completed - or the last one again, when the core did not draw it -
    // to the recording, composed when it is a composed film.
    void WriteCapturedFrame();
    // Ends the recording, parking it if it still has writing to do. `reason` is why, when it
    // was not the user's choice.
    void EndVideoCapture(const std::string& reason);
    struct CaptureGeometry
    {
        int width = 0, height = 0; // the core's frame
        int scaleX = 1, scaleY = 1;
        double aspect = 4.0 / 3.0;
    };
    bool GetCaptureGeometry(CaptureGeometry& out) const;
    const core::VideoCaptureSink* LatestVideoCapture() const;

    // Audio telemetry. Written from two threads, so every counter is atomic; they are diagnostic
    // totals, not synchronisation, hence relaxed ordering throughout.
    std::atomic<uint64_t> m_audioFramesProduced{0};
    std::atomic<uint64_t> m_audioFramesDroppedOverrun{0};
    std::atomic<uint64_t> m_audioFramesConsumed{0};
    std::atomic<uint64_t> m_audioFramesHeld{0};
    std::atomic<uint64_t> m_audioFramesDroppedResampler{0};
    std::atomic<uint64_t> m_audioDeviceCallbacks{0};
    std::atomic<uint64_t> m_audioUnderrunCallbacks{0};
    std::atomic<uint32_t> m_audioRingLevel{0};
    std::atomic<uint32_t> m_audioRingLevelMin{0xFFFFFFFFu};
    std::atomic<uint32_t> m_audioRingLevelMax{0};
    std::atomic<uint64_t> m_audioRingLevelTotal{0}; // summed at callback entry while playing
    std::atomic<uint64_t> m_audioRingLevelSamples{0};
    std::atomic<uint64_t> m_audioFramesTrimmed{0};
    std::atomic<uint64_t> m_audioTrimEvents{0};
    std::atomic<uint32_t> m_audioRingLevelMinPlaying{0xFFFFFFFFu};
    // The ring's level smoothed over ~half a second of output, for the pace controller. Written
    // by the audio thread, read by Update().
    std::atomic<float> m_audioRingLevelEma{0.0f};
    // The pace correction Update() last applied (main thread).
    double m_audioPace = 1.0;
    std::atomic<uint32_t> m_audioLastBlockFrames{0};
    std::atomic<uint32_t> m_audioMinBlockFrames{0xFFFFFFFFu};
    std::atomic<uint32_t> m_audioMaxBlockFrames{0};
    std::atomic<uint64_t> m_audioBlockCount{0};
    std::atomic<uint64_t> m_audioBlockFrameTotal{0};

    // Emulated time Update() chose not to run. Audio for it is never generated, so it leaves a
    // hole no host-side buffering can cover — a distinct fault from a ring underrun. Sites:
    // 0 = elapsed clamped to 100 ms, 1 = accumulator collapsed to one frame, 2 = frame-stop cap.
    std::atomic<uint64_t> m_audioTimeDiscardEvents{0};
    std::atomic<uint64_t> m_audioTimeDiscardedFrames{0};
    std::atomic<uint64_t> m_audioTimeDiscardBySite[3] = {};

    // Wall-clock spacing of deliveries from the core, to separate "the main thread stalled" from
    // "the core delivered uneven blocks".
    std::chrono::steady_clock::time_point m_audioLastProduceTime{};
    bool m_audioHaveLastProduce = false;
    std::atomic<uint64_t> m_audioProduceGapCount{0};
    std::atomic<uint64_t> m_audioProduceGapTotalMs{0}; // microseconds, summed
    std::atomic<uint32_t> m_audioProduceGapMaxUs{0};
    std::atomic<uint64_t> m_audioProduceStalls{0}; // gaps > 40 ms
    std::atomic<uint64_t> m_audioProduceGapHist[8] = {};

    // Time spent inside Update() (i.e. emulating), so a delivery gap can be attributed to
    // emulation cost versus host-side work elsewhere in the frame.
    std::atomic<uint32_t> m_audioUpdateMaxUs{0};
    std::atomic<uint64_t> m_audioUpdateTotalUs{0};
    std::atomic<uint64_t> m_audioUpdateCount{0};
    std::atomic<uint64_t> m_audioUpdateSlow{0}; // Update() calls taking > 40 ms

    // Frame-length diagnostics, to separate "the machine is running short frames" from "the core
    // produced the wrong amount of audio for a normal frame".
    std::atomic<uint64_t> m_diagRetroRunCalls{0};
    std::atomic<uint64_t> m_diagFrameBoundaries{0};
    std::atomic<uint64_t> m_diagFrameCckTotal{0};
    std::atomic<uint64_t> m_diagFrameCckCount{0};
    std::atomic<uint32_t> m_diagFrameCckMin{0xFFFFFFFFu};
    std::atomic<uint32_t> m_diagFrameCckMax{0};

    // CD32 device ID captured from SET_CONTROLLER_INFO
    unsigned m_cd32DeviceId = 0;

    // Per-controller configuration
    std::map<std::string, ControllerType> m_controllerConfigs;

    // Point the core's CAPS plugin loader at our bundled capsimg via the
    // ADM_CAPSIMG_PATH env var (needed for native IPF reading).
    void SetCapsImgEnv();

    // Unique per process AND per instance, for temp disk image names. See the definition:
    // m_instanceId alone collides across processes.
    std::string TempImageTag() const;
    void InitAudio();
    void ShutdownAudio();
    void WriteAudioSamples(const int16_t* data, size_t frames);
    static void AudioDeviceCallback(ma_device* device, void* output, const void* input,
                                    uint32_t frameCount);
    // Fills out[validFrames..frameCount) when the ring ran dry, and un-primes so the cushion is
    // rebuilt. Returns the number of frames it had to invent.
    static uint32_t FillUnderrun(AmigaEmulator* emu, float* out, uint32_t validFrames,
                                 uint32_t frameCount);
    // Skips the oldest `drop` of the `available` queued frames, crossfading into the new read
    // position. Returns the new read position.
    static size_t DropOldestAudio(AmigaEmulator* emu, size_t readPos, size_t available,
                                  size_t drop);
    // Records emulated time Update() decided not to run. site: 0 elapsed clamp, 1 accumulator
    // clamp, 2 frame-stop cap.
    void RecordDiscardedEmulatedTime(double seconds, int site);

    // Audio control
    // Written by SetMuted() on the main thread, read by the miniaudio callback thread.
    std::atomic<bool> m_isMuted{false};
    std::atomic<bool> m_forceSilent{false}; // SetForceSilent
    std::atomic<int> m_volume{100};         // SetVolume
    float VolumeGain() const;
    // Loudness to restore when floppy sound is switched back on. The core's own default is 20%.
    int m_floppySoundVolume = 20;

    // Disk activity
    bool m_driveLedState = false;
    bool m_driveActiveLatch = false;
    int m_aggregateHdActivityLatch = 0;
    uint64_t m_aggregateHdActivityCount = 0;
    bool m_driveMotorState[4] = {false, false, false, false};
    bool m_driveWriteState[4] = {false, false, false, false};
    bool m_diskInserted[4] = {false, false, false, false};
    bool m_diskWriteProtected[4] = {false, false, false, false};
    void* m_guiDataPtr = nullptr;          // PuAE gui_data symbol for drive state
    int m_currentTracks[4] = {0, 0, 0, 0}; // Current cylinder (0-79) for each drive
    int m_currentSide = 0;                 // Current side (0 or 1)

    // Power LED state (from PuAE gui_data)
    bool m_powerLedOn = false;
    uint8_t m_powerLedBrightness = 0; // 0-255, dim = audio filter active

    // UAE memory bank pointers (dlsym'd from dylib data section, already exported)
    void* m_chipMemBankPtr = nullptr;
    void* m_bogoMemBankPtr = nullptr;
    void* m_fastMemBankPtr = nullptr;   // array of up to 4 boards
    void* m_z3FastMemBankPtr = nullptr; // array of up to 4 boards
    void* m_z3ChipMemBankPtr = nullptr;
    void* m_kickMemBankPtr = nullptr;
    // The CD32/CDTV extended ROM at $F00000. Optional: exported from core 1.97.0 on, and absent
    // on every other machine, so a null here is the normal case rather than a stale core.
    void* m_extendedKickMemBankPtr = nullptr;
    // The uaegfx RTG board's VRAM. Optional: exported from core 1.99.0 on, and empty unless a
    // graphics card is configured, so a null here is the normal case rather than a stale core.
    void* m_gfxMemBankPtr = nullptr;

    // Cache for GetMemoryRegions(): the friendly-name mapping + sort is comparatively expensive and
    // runs every frame across several debugger views, but the bank layout only changes on
    // reset/config. We rebuild only when a cheap signature of the raw bank descriptors moves.
    mutable std::vector<MemoryRegion> m_cachedRegions;
    mutable uint64_t m_cachedRegionsSig = 0;
    mutable bool m_cachedRegionsValid = false;

    // Cache for GetCrassProfileStats(). The block is re-read once per core run (so a paused
    // machine re-reads nothing); the registry scan that finds the type-3 entry is redone only
    // when the resource generation moves. A zero address means "no profiler registered".
    mutable core::CrassProfileStats m_crassProfile;
    mutable uint64_t m_crassProfileRun = ~0ull;
    mutable uint32_t m_crassProfileResourceGen = 0;
    mutable bool m_crassProfileResourceGenValid = false;
    mutable uint32_t m_crassProfileAddr = 0;

    // The same for GetExocetBlock(): the block re-read once per core run, the type-4 registry
    // entry looked up again only when the resource generation moves.
    mutable core::ExocetBlockState m_exocetBlock;
    mutable uint64_t m_exocetBlockRun = ~0ull;
    mutable uint32_t m_exocetResourceGen = 0;
    mutable bool m_exocetResourceGenValid = false;
    mutable uint32_t m_exocetBlockAddr = 0;
    // An unregistered block (a disk that ships): where the scan found it, and when it last ran
    mutable uint32_t m_exocetScanAddr = 0;
    mutable uint64_t m_exocetScanRun = 0;
    mutable bool m_exocetScanRunValid = false;
    // GetExocetMemory()'s last read
    mutable core::ExocetMemoryState m_exocetMemory;
    mutable uint64_t m_exocetMemoryRun = ~0ull;
    void* m_customBankPtr = nullptr;
    void* m_ciaBankPtr = nullptr;
    void* m_customStoragePtr = nullptr;

    // Memory protection exports (adm_protect_* in patches/adm_protect.inc). Null on a core
    // built before the memory-protect patch; SupportsProtections() reports that.
    typedef void (*adm_protect_set_t)(const void*, int);
    typedef void (*adm_protect_clear_t)(void);
    adm_protect_set_t m_adm_protect_set = nullptr;
    adm_protect_clear_t m_adm_protect_clear = nullptr;
    int* m_adm_protect_count = nullptr;

    // Rebuild PUAE's custom_storage[] write shadow from a savestate blob. The shadow is not
    // part of the savestate and restore_custom() never writes back into it, so without this
    // every write-only register reads stale (or zero) after a restore — see UaeSavestate.h.
    // Returns false when the blob could not be parsed, leaving the shadow untouched.
    bool RebuildCustomStorage(const std::vector<uint8_t>& state) const;

    bool RunOneRetroFrame(bool countFrame, bool presentFrame = true, bool harvestFrame = true);
    void UpdateDemoPerformance(uint32_t frameStart, uint32_t frameEnd);
    void ResetDemoPerformance();
    void HarvestDiskEventsAndState();
    void PollDiskState();
    void PollHostMountActivity();
    void ClearInjectedMouseButtons();
    void ClearExpiredInjectedKeys();
    static void LedStateCallback(int led, int state);

    // Mouse input state
    int16_t m_mouseDeltaX = 0;
    int16_t m_mouseDeltaY = 0;
    bool m_mouseLeft = false;
    bool m_mouseRight = false;
    bool m_mouseMiddle = false;
    bool m_injectedMouseLeft = false;
    bool m_injectedMouseRight = false;
    uint64_t m_injectedMouseLeftReleaseFrame = 0;
    uint64_t m_injectedMouseRightReleaseFrame = 0;

    // Keyboard state
    std::unordered_set<int> m_pressedKeys;
    // Injected keys: RetroKey code -> frame at which the hold expires. Kept apart from
    // m_pressedKeys so a physical key-up (or losing focus) cannot cancel an injected hold.
    std::unordered_map<int, uint64_t> m_injectedKeys;

    // ---- Frame input capture / replay ----
    // Capture snapshots the resolved input at poll time, which is after PollGamepad has
    // rewritten the joypad from the physical controller and before the core queries it.
    void CaptureFrameInput();
    // Writes a recorded frame's input back into the live members, so the query path stays
    // a single code path: replay works by making the live state be the recorded state.
    void ApplyInputState(const core::FrameInput& in);

    std::function<void(uint64_t)> m_onFrameComplete;
    bool m_inputCaptureEnabled = false;
    bool m_replayActive = false;
    bool m_replayFramePending = false;
    core::FrameInput m_lastFrameInput;
    bool m_frameDigestEnabled = false;
    FrameDigest m_lastFrameDigest;
    uint64_t m_videoDigestPending = 0;
    core::AudioDigestAccumulator m_audioDigestPending;
    core::FrameInput m_replayInput;
    core::FrameInput m_savedLiveInput; // live state parked for the duration of a replay
    // Key edges delivered since the last poll. PuAE builds its keyboard queue from the
    // callback, not from the state query, so the edges have to be replayed, not just the
    // held-key set.
    std::vector<core::FrameInput::KeyEvent> m_pendingKeyEvents;
    // Presses scheduled for a future frame (type_text queues a whole string at once).
    struct PendingKey
    {
        int code = 0;
        uint64_t pressFrame = 0;
        uint32_t durationFrames = 1;
    };
    std::vector<PendingKey> m_pendingKeys;
    retro_keyboard_event_t m_keyboardCallback = nullptr;

    // Joypad state for the physical/host controller (libretro port 0 only).
    bool m_joypadButtons[16] = {};
    // Injected joypad buttons, OR'd into the polled state, per libretro port. Level-
    // triggered: they stay set until explicitly cleared, so a held direction survives
    // PollGamepad rewriting m_joypadButtons from a physical controller.
    bool m_injectedJoyButtons[kJoypadPorts][16] = {};
    // Whether retro_set_controller_port_device() has been told a joypad lives on each
    // libretro port. Port 0 is registered at load; port 1 only when first driven.
    bool m_joypadPortActive[kJoypadPorts] = {};
    // Bumped whenever m_totalFrameCount is rebased to 0. See GetSessionId().
    uint64_t m_sessionId = 0;
    std::vector<int> m_connectedJoystickIds;
    ControllerType m_controllerType = ControllerType::StandardJoystick;

    // Keyboard joystick state (OR'd into m_joypadButtons during PollGamepad)
    bool m_keyboardJoystickEnabled = false;
    bool m_kbJoyButtons[16] = {};

    // Bplpt event cache (populated during Update() → retro_run())
    BplptEvent m_bplptEvents[kMaxBplptEvents];
    int m_bplptEventCount = 0;

    // Sprpt event cache (populated during Update() → retro_run())
    SprptEvent m_sprptEvents[kMaxSprptEvents];
    int m_sprptEventCount = 0;

    // Paula channel trace cache (populated during Update() → retro_run(), only while enabled).
    // 4 × 2048 × 16 bytes = 128 KB, heap-allocated on first use rather than inlined: this object
    // is already ~5 MB of inline caches, so it can only ever be created on the heap. Also means a
    // session that never opens the Audio window pays nothing.
    std::vector<AudioSampleEvent> m_audioTrace[kAudioChannels];
    int m_audioTraceCount[kAudioChannels] = {};
    bool m_audioTraceEnabled = false;    // what the UI asked for
    bool m_audioTraceForCapture = false; // ... and what a recording did
    bool AudioTraceWanted() const { return m_audioTraceEnabled || m_audioTraceForCapture; }
    // What the core was last actually told. Arming while no core is loaded (a workspace that
    // restores the Audio window open before the first disk) can only set the intent, so the two
    // must be tracked apart or the request is silently lost.
    bool m_audioTraceEnabledCore = false;

    // Scratch buffers used during RunOneRetroFrame() to avoid large stack allocations.
    // Stored as class members (heap) so they don't blow the main-thread stack.
    struct SprptWireScratch
    {
        uint16_t vpos;
        uint8_t spriteIdx;
        uint8_t pad;
        uint32_t ptr;
        uint32_t cycle;
    }; // must stay 12 bytes — matches DLL adm_sprpt_event_t ABI
    static_assert(sizeof(SprptWireScratch) == 12, "SprptWireScratch must be 12 bytes");
    SprptWireScratch m_sprptWireBuf[kMaxSprptEvents];    // 256 × 12 = 3 072 bytes
    SprptColorSnap m_sprptColorSnapBuf[kMaxSprptEvents]; // 256 × 32 = 8 192 bytes

    // Blit event cache (populated during Update() → retro_run())
    BlitEvent m_blitEvents[kMaxBlitEvents];
    int m_blitEventCount = 0;

    // Drops the core-side provenance map. Called wherever the emulated machine's chip RAM
    // is replaced wholesale (reset, savestate restore, disk swap) — stale stamps would
    // otherwise describe memory that no longer exists.
    void ClearBlitterVisMap();

    // Blitter-vis span cache (populated during Update() → retro_run()). Heap-allocated and
    // only while enabled: 64 K spans × 12 bytes is too large to sit in the object.
    std::vector<BlitVisSpan> m_blitVisSpans;
    int m_blitVisSpanCount = 0;
    bool m_blitVisEnabled = false;

    // Idle-region event cache (populated during Update() → retro_run(); empty on older cores)
    IdleEvent m_idleEvents[kMaxIdleEvents];
    int m_idleEventCount = 0;

    // Copper exec event cache (populated during Update() → retro_run())
    CopperExecEvent m_copperExecEvents[kMaxCopperExecEvents];
    int m_copperExecEventCount = 0;
    bool m_copperExecOverflow = false;

    // Custom-register write cache (populated during Update() → retro_run()).
    // Heap-allocated, sized on first harvest: kMaxRegWriteEvents * 20 bytes is ~320 KB, which
    // only a session that actually profiles register writes should pay for.
    std::vector<RegWriteEvent> m_regWriteEvents;
    int m_regWriteEventCount = 0;
    bool m_regWriteOverflow = false;

    // Frame start cycle (absolute CCK from adm_get_frame_start_cycles(), updated each frame)
    uint32_t m_frameStartCycle = 0;
    uint32_t m_prevFrameStartCycle = 0;

    // Frame count Update()'s catch-up batch stops at (0 = no limit). See SetFrameStopTarget.
    uint64_t m_frameStopTarget = 0;
    uint64_t m_frameBreakTarget = 0;         // SetFrameBreakTarget
    mutable bool m_hostBreakPending = false; // RequestHostBreak

    // Partial-frame state, latched in RunOneRetroFrame when retro_run() stops mid-frame.
    // Stable while paused (RunOneRetroFrame isn't called again until the user steps/resumes).
    bool m_partialFrame = false;
    uint32_t m_breakCycle = 0; // absolute CCK at the mid-frame stop
    int m_breakVpos = 0;
    int m_breakHpos = 0;

    // CPU instruction trace cache (populated during Update() → retro_run() when enabled)
    CpuTraceEntry m_cpuTrace[kMaxCpuTraceEntries];
    int m_cpuTraceCount = 0;
    bool m_cpuTraceOverflow = false;
    bool m_cpuTraceEnabled = false;

    // Disk write event cache (populated during Update() → retro_run() when write-redirect off)
    DiskWriteEvent m_diskWriteEventCache[kMaxDiskWriteEvents];
    int m_diskWriteEventCount = 0;

    // Disk DMA transfer cache (drained each frame alongside the write events)
    DiskDmaEvent m_diskDmaEventCache[kMaxDiskDmaEvents];
    int m_diskDmaEventCount = 0;

    // Static "active" instance used to route libretro C callbacks back to a C++ object.
    // libretro callbacks carry no user-data, but they only fire synchronously inside a core
    // call on the calling thread, so each core-entry method scopes s_instance to itself via
    // ActiveInstanceScope. This lets a second (preview) emulator coexist with the main one.
    static AmigaEmulator* s_instance;

    struct ActiveInstanceScope
    {
        AmigaEmulator* prev;
        explicit ActiveInstanceScope(AmigaEmulator* self) : prev(s_instance) { s_instance = self; }
        ~ActiveInstanceScope() { s_instance = prev; }
    };

    // Unique per-instance id (folded into temp file paths so two instances don't collide).
    int m_instanceId = 0;
    static int s_nextInstanceId;

    // Callbacks
    static bool EnvironmentCallback(unsigned cmd, void* data);
    static void VideoRefreshCallback(const void* data, unsigned width, unsigned height,
                                     size_t pitch);
    static void AudioSampleCallback(int16_t left, int16_t right);
    static size_t AudioSampleBatchCallback(const int16_t* data, size_t frames);
    static void InputPollCallback();
    static int16_t InputStateCallback(unsigned port, unsigned device, unsigned index, unsigned id);
};

} // namespace adm
