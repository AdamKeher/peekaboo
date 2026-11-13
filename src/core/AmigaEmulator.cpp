#include "AmigaEmulator.h"
#include "AdfWrapper.h"
#include "ArMemoryAccess.h"
#include "AviRecorder.h"
#include "CdMedia.h"
#include "EmulatorOptions.h"
#include "ExecIntrospect.h"
#include "FfmpegRecorder.h"
#include "HostExecutableLauncher.h"
#include "KickstartRoms.h"
#include "Logger.h"
#include "UaeSavestate.h"

#include <miniaudio.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_set>
#include <vector>

// Platform specific includes for dynamic loading
// Platform specific includes for dynamic loading
#ifdef _WIN32
#include <glad/glad.h>
#include <windows.h>
#undef CreateDirectory
#undef DeleteFile
#else
#include <dlfcn.h>
#include <glad/glad.h>
#include <unistd.h>
#endif

#include <GLFW/glfw3.h>

#include <stb_image_write.h> // declarations only; implementation in external/stb/stb_image_write_impl.cpp

namespace {
// PuAE's write-protect handling (disk_setwriteprotect -> diskfile_readonly) chmods the
// underlying image file on the host filesystem. A leftover read-only temp image would
// otherwise block both rewriting (ofstream open fails with EACCES) and deletion
// (throwing fs::remove terminates the app).
void ClearReadOnlyAttribute(const std::string& path)
{
    std::error_code ec;
    std::filesystem::permissions(path, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add, ec);
}

void RemoveTempDiskImage(std::string& path)
{
    if (path.empty())
        return;
    ClearReadOnlyAttribute(path);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec)
        adm::core::Logger::Warn("Emulator: Failed to remove temp disk image {}: {}", path,
                                ec.message());
    path.clear();
}
} // namespace

// Libretro constants usually defined in libretro.h
#define RETRO_ENVIRONMENT_GET_OVERSCAN                       2
#define RETRO_ENVIRONMENT_GET_CAN_DUPE                       3
#define RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL              8
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY               9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT                   10
#define RETRO_ENVIRONMENT_GET_VARIABLE                       15
#define RETRO_ENVIRONMENT_SET_VARIABLES                      16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE                17
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE                  27
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY                 31
#define RETRO_ENVIRONMENT_SET_MESSAGE                        6
#define RETRO_ENVIRONMENT_SET_MESSAGE_EXT                    60
#define RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION           52
#define RETRO_ENVIRONMENT_GET_LED_INTERFACE                  46
#define RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE         25
#define RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION 57
#define RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE     58
#define RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK              12
#define RETRO_ENVIRONMENT_SET_CONTROLLER_INFO                35
#define RETRO_DEVICE_JOYPAD                                  1
#define RETRO_DEVICE_MOUSE                                   2
#define RETRO_DEVICE_KEYBOARD                                3

#define RETRO_DEVICE_ID_MOUSE_X     0
#define RETRO_DEVICE_ID_MOUSE_Y     1
#define RETRO_DEVICE_ID_MOUSE_LEFT  2
#define RETRO_DEVICE_ID_MOUSE_RIGHT 3
// Redefinition removed: RETRO_DEVICE_ID_MOUSE_MIDDLE is defined in the header

struct retro_variable
{
    const char* key;
    const char* value;
};

enum retro_log_level {
    RETRO_LOG_DEBUG = 0,
    RETRO_LOG_INFO,
    RETRO_LOG_WARN,
    RETRO_LOG_ERROR,
    RETRO_LOG_DUMMY = 2147483647
};

typedef void (*retro_log_printf_t)(enum retro_log_level level, const char* fmt, ...);

struct retro_log_callback
{
    retro_log_printf_t log;
};

struct retro_message
{
    const char* msg;
    unsigned frames;
};

typedef void (*retro_set_led_state_t)(int led, int state);

struct retro_led_interface
{
    retro_set_led_state_t set_led_state;
};

// UAE addrbank layout — matches sources/src/include/memory.h in libretro-uae.
// sizeof == 184 bytes (0xB8), verified by consecutive bank symbol spacing in nm output.
// TCHAR is char in libretro builds (ASCII mode).
struct UAEAddrBank
{
    void* lget;                 // +0
    void* wget;                 // +8
    void* bget;                 // +16
    void* lput;                 // +24
    void* wput;                 // +32
    void* bput;                 // +40
    void* xlateaddr;            // +48
    void* check;                // +56
    uint8_t* baseaddr;          // +64  host RAM pointer
    const char* label;          // +72  short name (e.g. "chip memory")
    const char* name;           // +80  display name (e.g. "Chip memory")
    void* lgeti;                // +88
    void* wgeti;                // +96
    int flags;                  // +104
    int jit_read_flag;          // +108
    int jit_write_flag;         // +112
    int _pad;                   // +116  alignment padding for next pointer
    void* sub_banks;            // +120
    uint32_t mask;              // +128
    uint32_t startmask;         // +132
    uint32_t start;             // +136  Amiga address space start
    uint32_t allocated_size;    // +140  bytes allocated
    uint32_t reserved_size;     // +144
    uint8_t* baseaddr_direct_r; // +152
    uint8_t* baseaddr_direct_w; // +160
    uint32_t startaccessmask;   // +168
    bool barrier;               // +172
    uint32_t protectmode;       // +176
};
static_assert(offsetof(UAEAddrBank, baseaddr) == 64, "UAEAddrBank.baseaddr offset mismatch");
static_assert(offsetof(UAEAddrBank, label) == 72, "UAEAddrBank.label offset mismatch");
static_assert(offsetof(UAEAddrBank, start) == 136, "UAEAddrBank.start offset mismatch");
static_assert(offsetof(UAEAddrBank, allocated_size) == 140,
              "UAEAddrBank.allocated_size offset mismatch");
// The whole struct, not just the fields read: fastmem_bank and z3fastmem_bank are arrays walked
// by this stride. At 144 bytes the second "bank" started 40 bytes into the first, and its garbage
// function pointers crashed an A4000/040 the first time the Memory Map drew.
static_assert(sizeof(UAEAddrBank) == 184, "UAEAddrBank size mismatch (sizeof(addrbank))");

// PuAE gui_data structures for reading drive state directly from the core.
// These must match the ABI of sources/src/include/gui.h in libretro-uae.
// TCHAR is char in libretro builds.
struct puae_gui_info_drive
{
    bool drive_motor;
    uint8_t drive_track;
    bool drive_writing;
    bool drive_disabled;
    char df[256];
    uint32_t crc32;
    bool floppy_protected;
    bool floppy_inserted;
};

struct puae_gui_info
{
    bool powerled;
    uint8_t powerled_brightness;
    int8_t drive_side;
    int8_t hd;
    int8_t cd;
    int8_t md;
    int8_t net;
    int cpu_halted;
    int fps, idle;
    int fps_color;
    int sndbuf, sndbuf_status;
    bool sndbuf_avail;
    puae_gui_info_drive drives[4];
};

// Whether the core's own log is let through. Read on the emulation thread, written from the
// UI thread when the Emulator menu toggles it, so it is atomic rather than a plain bool.
static std::atomic<bool> g_coreVerboseLog{false};

// PUAE hands us everything it writes through this callback, and the libretro build of
// write_log() emits *all* of it at RETRO_LOG_INFO - warnings included. Filtering below
// RETRO_LOG_WARN therefore threw the whole core log away, which is why diagnostics that would
// have settled real questions (save_blitter_new()'s "BLITTER active while saving state" is the
// one that prompted this) have never been readable from inside Peekaboo.
//
// It is still off by default. PUAE at INFO writes several lines per save state and per disk
// event, this runs on the emulation thread, and at fifty frames a second the volume would bury
// the host's own log. The Emulator menu turns it on for as long as somebody is reading it.
//
// Levels are mapped rather than flattened: the old code logged everything as a warning, so the
// few genuine warnings that did get through were indistinguishable from ordinary chatter.
void adm::AmigaEmulator::SetCoreLogVerbose(bool enable)
{
    g_coreVerboseLog.store(enable, std::memory_order_relaxed);
}

bool adm::AmigaEmulator::IsCoreLogVerbose()
{
    return g_coreVerboseLog.load(std::memory_order_relaxed);
}

static void CoreLog(enum retro_log_level level, const char* fmt, ...)
{
    if (level < RETRO_LOG_WARN && !g_coreVerboseLog.load(std::memory_order_relaxed))
        return;
    va_list args;
    va_start(args, fmt);
    // UAE's help block is a few KB emitted in one call, so a long line is truncated here.
    // That is deliberate: this runs on the emulation thread, and a diagnostic is still a
    // diagnostic with its tail cut off.
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // Strip trailing newlines; Logger adds its own line terminator.
    std::string msg(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
    if (msg.empty())
        return;
    switch (level) {
        case RETRO_LOG_ERROR:
            adm::core::Logger::Error("PuAE: {}", msg);
            break;
        case RETRO_LOG_WARN:
            adm::core::Logger::Warn("PuAE: {}", msg);
            break;
        default:
            adm::core::Logger::Info("PuAE: {}", msg);
            break;
    }
}

namespace adm {

AmigaEmulator* AmigaEmulator::s_instance = nullptr;
int AmigaEmulator::s_nextInstanceId = 0;

static const std::vector<AmigaModelInfo> kAmigaModels = {
    {AmigaModel::A500OG, "A500OG", "A500OG", "A500 OG - Amiga 500 (v1.3, OCS, 0.5MB Chip)", "512",
     "0", "0"},
    {AmigaModel::A500, "A500", "A500", "A500 - Amiga 500 (v1.3, OCS, 0.5MB Chip + 0.5MB Slow)",
     "512", "512", "0"},
    {AmigaModel::A500Plus, "A500+", "A500PLUS", "A500+ - Amiga 500+ (v2.04, ECS, 1MB Chip)", "1024",
     "0", "0"},
    {AmigaModel::A600, "A600", "A600", "A600 - Amiga 600 (v2.05, ECS, 2MB Chip + 8MB Fast)", "2048",
     "0", "8192"},
    {AmigaModel::A1200OG, "A1200OG", "A1200OG", "A1200 OG - Amiga 1200 (v3.1, AGA, 2MB Chip)",
     "2048", "0", "0"},
    {AmigaModel::A1200, "A1200", "A1200", "A1200 - Amiga 1200 (v3.1, AGA, 2MB Chip + 8MB Fast)",
     "2048", "0", "8192"},
    {AmigaModel::A2000OG, "A2000OG", "A2000OG",
     "A2000 OG - Amiga 2000 (v1.3, OCS, 0.5MB Chip + 0.5MB Slow)", "512", "512", "0"},
    {AmigaModel::A2000, "A2000", "A2000", "A2000 - Amiga 2000 (v2.04, ECS, 1MB Chip)", "1024", "0",
     "0"},
    {AmigaModel::A4030, "A4030", "A4030", "A4030 - Amiga 4000/030 (v3.1, AGA, 2MB Chip + 8MB Fast)",
     "2048", "0", "8192"},
    {AmigaModel::A4040, "A4040", "A4040", "A4040 - Amiga 4000/040 (v3.1, AGA, 2MB Chip + 8MB Fast)",
     "2048", "0", "8192"},
    // Same machine as A4040 (same coreId, same ROM), plus an 8 MB Zorro III uaegfx board. The
    // A4000 is the natural home for RTG: 68030/040 with a 32-bit address space, which is what a
    // Zorro III board needs. Nothing appears on the RTG screen until the guest has Picasso96
    // installed.
    {AmigaModel::A4040RTG, "A4040RTG", "A4040",
     "A4040 RTG - Amiga 4000/040 (v3.1, AGA, 2MB Chip + 8MB Fast + 8MB RTG)", "2048", "0", "8192"},
    {AmigaModel::CDTV, "CDTV", "CDTV", "CDTV - Amiga CDTV (v1.3+ext, ECS, 1MB Chip)", "1024", "0",
     "0"},
    {AmigaModel::CD32, "CD32", "CD32", "CD32 - Amiga CD32 (v3.1+ext, AGA, 2MB Chip)", "2048", "0",
     "0"},
    {AmigaModel::CD32FR, "CD32FR", "CD32FR",
     "CD32FR - Amiga CD32 (v3.1+ext, AGA, 2MB Chip + 8MB Fast)", "2048", "0", "8192"},
};

const AmigaModelInfo& GetAmigaModelInfo(AmigaModel model)
{
    for (const auto& info : kAmigaModels) {
        if (info.model == model)
            return info;
    }
    return kAmigaModels[1]; // Default to A500
}

const std::vector<AmigaModelInfo>& GetAllAmigaModels() { return kAmigaModels; }

AmigaEmulator::AmigaEmulator()
{
    m_instanceId = s_nextInstanceId++;
    s_instance = this;
    m_fpsTimer = std::chrono::steady_clock::now();
    RefreshConnectedControllers();
}

AmigaEmulator::~AmigaEmulator()
{
    // The CoreAudio device is owned for the lifetime of this instance, not per core
    // load (see UnloadCore). Release it here, at the instance's true end of life.
    // It goes first so the audio thread — the device tap's producer — is stopped before the taps
    // are closed; the taps then flush everything that thread staged.
    ShutdownAudio();
    StopAllAudioRecording();
    UnloadCore();
    if (!m_hostLaunchWorkspacePath.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(m_hostLaunchWorkspacePath, ec);
        if (ec) {
            core::Logger::Warn("Emulator: could not remove fast-launch workspace '{}': {}",
                               m_hostLaunchWorkspacePath, ec.message());
        }
    }
    if (m_ownsSaveDir && !m_saveDir.empty()) {
        std::error_code tempError;
        std::error_code absoluteError;
        auto tempDirectory = std::filesystem::absolute(
                                 std::filesystem::temp_directory_path(tempError), absoluteError)
                                 .lexically_normal();
        if (tempDirectory.filename().empty())
            tempDirectory = tempDirectory.parent_path();
        const auto saveDirectory =
            std::filesystem::absolute(m_saveDir, absoluteError).lexically_normal();
        const std::string filename = saveDirectory.filename().string();
        const bool isOwnedSessionDirectory = !tempError && !absoluteError &&
                                             saveDirectory.parent_path() == tempDirectory &&
                                             filename.starts_with("adm_puae_session_");
        if (!isOwnedSessionDirectory) {
            core::Logger::Warn("Emulator: refusing to remove unexpected save directory '{}'",
                               m_saveDir);
        } else {
            std::error_code removeError;
            std::filesystem::remove_all(saveDirectory, removeError);
            if (removeError) {
                core::Logger::Warn("Emulator: could not remove session save directory '{}': {}",
                                   m_saveDir, removeError.message());
            }
        }
    }
    if (s_instance == this)
        s_instance = nullptr;
}

// Symbols that identify our patched PuAE fork. These are dereferenced unconditionally on the
// per-frame path (RunOneRetroFrame) and at load time, so a core missing any of them is not the
// build this program targets and must be rejected rather than crashed on.
// Keep in sync with the unconditional m_adm_* calls in LoadCore() / RunOneRetroFrame().
static const char* const kRequiredCoreSymbols[] = {
    "retro_init",
    "retro_run",
    // Called unconditionally in LoadCore() right after the null check on retro_init/retro_run —
    // a core exporting those two but not the setters would be a null-pointer call.
    "retro_set_environment",
    "retro_set_video_refresh",
    "retro_set_audio_sample",
    "retro_set_audio_sample_batch",
    "retro_set_input_poll",
    "retro_set_input_state",
    "adm_get_version",
    "adm_get_frame_start_cycles",
    "adm_get_bplpt_events",
    "adm_get_sprpt_events",
    "adm_get_blit_events",
    // Required, not optional: adm_blit_event_t gained a blitId field alongside the blitter-vis
    // patch, so a DLL without this export also has the older, smaller blit-event struct and
    // would feed GetBlitEvents() records at the wrong stride. Reject it instead.
    "adm_get_blitter_vis_spans",
    // Same reasoning again: adm_blit_event_t grew a data_off/data_words pair alongside the
    // destination-data capture, so a core without this export also has the older, smaller
    // blit-event struct.
    "adm_get_blit_data",
    "adm_get_copper_events",
    "adm_get_cpu_trace",
    "adm_drain_serial_output",
    "adm_arm_oneshot_breakpoint",
    "adm_cancel_oneshot_breakpoint",
    "adm_get_oneshot_breakpoint_status",
};

std::string AmigaEmulator::GetDefaultCorePath()
{
#ifdef _WIN32
    return "./emulation/puae_libretro.dll";
#else
    return "./emulation/puae_libretro.dylib";
#endif
}

std::string AmigaEmulator::DuplicateCoreToTemp()
{
    namespace fs = std::filesystem;
    fs::path src = GetDefaultCorePath();
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        core::Logger::Error("Emulator: DuplicateCoreToTemp: source core not found: {}",
                            src.string());
        return {};
    }

#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    // The process id keeps Peekaboos running side by side off each other's copy.
    fs::path dst =
        fs::temp_directory_path() / ("adm_puae_preview_" + std::to_string(pid) + "_" +
                                     std::to_string(s_nextInstanceId) + src.extension().string());

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        core::Logger::Error("Emulator: DuplicateCoreToTemp: copy failed: {}", ec.message());
        return {};
    }
    return dst.string();
}

bool AmigaEmulator::VerifyCore(const std::string& corePath, std::string& outError)
{
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(corePath.c_str());
#else
    void* handle = dlopen(corePath.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
    if (!handle) {
        outError = "Could not open emulator core '" + corePath + "'";
#ifndef _WIN32
        if (const char* err = dlerror())
            outError += std::string(": ") + err;
#endif
        return false;
    }

    auto sym = [&](const char* name) -> void* {
#ifdef _WIN32
        return (void*)GetProcAddress(handle, name);
#else
        return dlsym(handle, name);
#endif
    };

    std::string missing;
    for (const char* name : kRequiredCoreSymbols) {
        if (!sym(name)) {
            if (!missing.empty())
                missing += ", ";
            missing += name;
        }
    }

#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif

    if (!missing.empty()) {
        outError = "Emulator core '" + corePath +
                   "' is not the required patched PuAE fork (missing symbols: " + missing +
                   "). Rebuild the core with scripts/build_puae.";
        return false;
    }
    return true;
}

std::string AmigaEmulator::ProbeVersion(const std::string& corePath)
{
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(corePath.c_str());
#else
    void* handle = dlopen(corePath.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
    if (!handle)
        return {};

#ifdef _WIN32
    auto fn = (const char* (*)(void))GetProcAddress(handle, "adm_get_version");
#else
    auto fn = (const char* (*)(void))dlsym(handle, "adm_get_version");
#endif

    std::string version;
    if (fn)
        version = fn();

#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
    return version;
}

bool AmigaEmulator::LoadCore(const std::string& corePath)
{
    ActiveInstanceScope _g(this);

    if (m_coreLoaded)
        UnloadCore();

#ifdef _WIN32
    m_libHandle = LoadLibraryA(corePath.c_str());
#else
    m_libHandle = dlopen(corePath.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif

    if (!m_libHandle) {
#ifndef _WIN32
        core::Logger::Error("Emulator: Failed to load core: {}", dlerror());
#endif
        return false;
    }

    auto loadSym = [&](const char* name) -> void* {
#ifdef _WIN32
        return (void*)GetProcAddress((HMODULE)m_libHandle, name);
#else
        return dlsym(m_libHandle, name);
#endif
    };

    m_retro_init = (retro_init_t)loadSym("retro_init");
    m_retro_deinit = (retro_deinit_t)loadSym("retro_deinit");
    m_retro_api_version = (retro_api_version_t)loadSym("retro_api_version");
    m_retro_get_system_info = (retro_get_system_info_t)loadSym("retro_get_system_info");
    m_retro_get_system_av_info = (retro_get_system_av_info_t)loadSym("retro_get_system_av_info");
    m_retro_set_environment = (retro_set_environment_t)loadSym("retro_set_environment");
    m_retro_set_video_refresh = (retro_set_video_refresh_t)loadSym("retro_set_video_refresh");
    m_retro_set_audio_sample = (retro_set_audio_sample_t)loadSym("retro_set_audio_sample");
    m_retro_set_audio_sample_batch =
        (retro_set_audio_sample_batch_t)loadSym("retro_set_audio_sample_batch");
    m_retro_set_input_poll = (retro_set_input_poll_t)loadSym("retro_set_input_poll");
    m_retro_set_input_state = (retro_set_input_state_t)loadSym("retro_set_input_state");
    m_retro_set_controller_port_device =
        (retro_set_controller_port_device_t)loadSym("retro_set_controller_port_device");
    m_retro_load_game = (retro_load_game_t)loadSym("retro_load_game");
    m_retro_run = (retro_run_t)loadSym("retro_run");
    m_retro_reset = (retro_reset_t)loadSym("retro_reset");
    m_retro_unload_game = (retro_unload_game_t)loadSym("retro_unload_game");
    m_retro_serialize_size = (retro_serialize_size_t)loadSym("retro_serialize_size");
    m_retro_serialize = (retro_serialize_t)loadSym("retro_serialize");
    m_retro_unserialize = (retro_unserialize_t)loadSym("retro_unserialize");

    if (!m_retro_init || !m_retro_run || !m_retro_set_environment || !m_retro_set_video_refresh ||
        !m_retro_set_audio_sample || !m_retro_set_audio_sample_batch || !m_retro_set_input_poll ||
        !m_retro_set_input_state) {
        core::Logger::Error("Emulator: Failed to load required symbols from core.");
        UnloadCore();
        return false;
    }

    m_retro_set_environment(EnvironmentCallback);
    m_retro_set_video_refresh(VideoRefreshCallback);
    m_retro_set_audio_sample(AudioSampleCallback);
    m_retro_set_audio_sample_batch(AudioSampleBatchCallback);
    m_retro_set_input_poll(InputPollCallback);
    m_retro_set_input_state(InputStateCallback);

    m_retro_init();

    // Set controller port device - PuAE uses port 0 for all input (mouse, keyboard, joypad)
    // Keyboard works through the dedicated keyboard callback, not through port device setting
    if (m_retro_set_controller_port_device) {
        unsigned device;
        if (m_controllerType == ControllerType::CD32Controller) {
            device = (m_cd32DeviceId != 0) ? m_cd32DeviceId
                                           : RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0);
        } else {
            device = RETRO_DEVICE_JOYPAD;
        }
        core::Logger::Info("Emulator: LoadCore: Setting controller port 0 to device {} "
                           "(cd32DeviceId={})",
                           device, m_cd32DeviceId);
        m_retro_set_controller_port_device(0, device);
        m_joypadPortActive[0] = true;
        // Port 1 stays unregistered until set_joystick asks for the Amiga's port 0,
        // so the emulated mouse keeps that port by default.
        m_joypadPortActive[1] = false;
    }

    SetModel(m_currentModel);

    // Resolve PuAE's gui_data symbol for direct drive state access
    m_guiDataPtr = loadSym("gui_data");

    // Resolve custom CPU state access
    m_adm_get_cpu_state = (adm_get_cpu_state_t)loadSym("adm_get_cpu_state");
    // Control/FPU/MMU registers (core 1.102.0); optional so an older DLL still loads.
    m_adm_get_cpu_ext_state = (adm_get_cpu_ext_state_t)loadSym("adm_get_cpu_ext_state");

    // Resolve disassembler / assembler
    m_adm_disasm_one = (adm_disasm_one_t)loadSym("adm_disasm_one");
    m_adm_asm_one = (adm_asm_one_t)loadSym("adm_asm_one");
    m_adm_debug_command = (adm_debug_command_t)loadSym("adm_debug_command");
    m_adm_get_debug_break_pending =
        (adm_get_debug_break_pending_t)loadSym("adm_get_debug_break_pending");
    m_adm_arm_debug = (adm_arm_debug_t)loadSym("adm_arm_debug");
    m_adm_arm_oneshot_breakpoint =
        (adm_arm_oneshot_breakpoint_t)loadSym("adm_arm_oneshot_breakpoint");
    m_adm_cancel_oneshot_breakpoint =
        (adm_cancel_oneshot_breakpoint_t)loadSym("adm_cancel_oneshot_breakpoint");
    m_adm_get_oneshot_breakpoint_status =
        (adm_get_oneshot_breakpoint_status_t)loadSym("adm_get_oneshot_breakpoint_status");
    m_adm_arm_fb = (adm_arm_fb_t)loadSym("adm_arm_fb");
    m_adm_is_fb_armed = (adm_is_fb_armed_t)loadSym("adm_is_fb_armed");
    m_adm_get_fb_entry = (adm_get_fb_entry_t)loadSym("adm_get_fb_entry");
    m_adm_get_fb_hit = (adm_get_fb_hit_t)loadSym("adm_get_fb_hit");
    m_adm_arm_exception = (adm_arm_exception_t)loadSym("adm_arm_exception");
    m_adm_get_exception_hit = (adm_get_exception_hit_t)loadSym("adm_get_exception_hit");
    m_adm_arm_beam_break = (adm_arm_beam_break_t)loadSym("adm_arm_beam_break");
    m_adm_get_beam_hit = (adm_get_beam_hit_t)loadSym("adm_get_beam_hit");
    m_adm_get_last_memwatch = (adm_get_last_memwatch_t)loadSym("adm_get_last_memwatch");
    m_adm_get_last_memwatch2 = (adm_get_last_memwatch2_t)loadSym("adm_get_last_memwatch2");

    // Resolve display mask setters
    m_adm_set_bitplane_mask = (adm_set_bitplane_mask_t)loadSym("adm_set_bitplane_mask");
    m_adm_set_sprite_mask = (adm_set_sprite_mask_t)loadSym("adm_set_sprite_mask");
    m_adm_redraw_frame = (adm_redraw_frame_t)loadSym("adm_redraw_frame");
    // Resolve the chipset display options (see PATCHES.md, "palette-vis")
    m_adm_set_palette_vis = (adm_set_palette_vis_t)loadSym("adm_set_palette_vis");
    m_adm_set_bplcon1_scroll = (adm_set_bplcon1_scroll_t)loadSym("adm_set_bplcon1_scroll");
    m_adm_set_copper_block = (adm_set_copper_block_t)loadSym("adm_set_copper_block");
    m_adm_set_bplptr_block = (adm_set_bplptr_block_t)loadSym("adm_set_bplptr_block");
    m_adm_set_blitter_writes = (adm_set_blitter_writes_t)loadSym("adm_set_blitter_writes");
    // Re-apply whatever masking the user had selected before this core was (re)loaded.
    PushDisplayMasks();
    PushDisplayOptions();

    // Resolve event capture APIs
    m_adm_get_bplpt_events = (adm_get_bplpt_events_t)loadSym("adm_get_bplpt_events");
    m_adm_clear_bplpt_events = (adm_clear_bplpt_events_t)loadSym("adm_clear_bplpt_events");
    m_adm_get_sprpt_events = (adm_get_sprpt_events_t)loadSym("adm_get_sprpt_events");
    m_adm_get_sprpt_color_snaps =
        (adm_get_sprpt_color_snaps_t)loadSym("adm_get_sprpt_color_snaps"); // optional
    m_adm_clear_sprpt_events = (adm_clear_sprpt_events_t)loadSym("adm_clear_sprpt_events");
    m_adm_get_blit_events = (adm_get_blit_events_t)loadSym("adm_get_blit_events");
    m_adm_clear_blit_events = (adm_clear_blit_events_t)loadSym("adm_clear_blit_events");
    m_adm_set_blit_data_enabled = (adm_set_blit_data_enabled_t)loadSym("adm_set_blit_data_enabled");
    m_adm_get_blit_data = (adm_get_blit_data_t)loadSym("adm_get_blit_data");
    m_adm_get_blit_data_enabled = (adm_get_blit_data_enabled_t)loadSym("adm_get_blit_data_enabled");
    // Blitter-vis. Present on any core new enough to pass the required-symbol check above;
    // the null guards remain so a partial DLL degrades instead of crashing.
    m_adm_set_blitter_vis_enabled =
        (adm_set_blitter_vis_enabled_t)loadSym("adm_set_blitter_vis_enabled");
    m_adm_get_blitter_vis_spans = (adm_get_blitter_vis_spans_t)loadSym("adm_get_blitter_vis_spans");
    m_adm_get_blitter_vis_stats = (adm_get_blitter_vis_stats_t)loadSym("adm_get_blitter_vis_stats");
    m_adm_get_idle_events = (adm_get_idle_events_t)loadSym("adm_get_idle_events");       // optional
    m_adm_clear_idle_events = (adm_clear_idle_events_t)loadSym("adm_clear_idle_events"); // optional
    m_adm_get_copper_events = (adm_get_copper_events_t)loadSym("adm_get_copper_events");
    m_adm_clear_copper_events = (adm_clear_copper_events_t)loadSym("adm_clear_copper_events");
    // Register-write log — optional (see HasRegWriteLog).
    m_adm_get_reg_write_events = (adm_get_reg_write_events_t)loadSym("adm_get_reg_write_events");
    m_adm_clear_reg_write_events =
        (adm_clear_reg_write_events_t)loadSym("adm_clear_reg_write_events");

    // Resolve CPU trace API
    m_adm_set_cpu_trace_enabled = (adm_set_cpu_trace_enabled_t)loadSym("adm_set_cpu_trace_enabled");
    m_adm_get_cpu_trace = (adm_get_cpu_trace_t)loadSym("adm_get_cpu_trace");
    m_adm_get_cpu_trace_frame = (adm_get_cpu_trace_t)loadSym("adm_get_cpu_trace_frame"); // optional
    m_adm_clear_cpu_trace = (adm_clear_cpu_trace_t)loadSym("adm_clear_cpu_trace");

    // Resolve the emulated serial output drain used by KPrintF.
    m_adm_drain_serial_output = (adm_drain_serial_output_t)loadSym("adm_drain_serial_output");
    m_serialLogLine.clear();
    m_serialLastWasCr = false;
    m_kprintfBreakPending = false;
    m_hasKprintfHit = false;
    m_hasLatchedKprintf = false;

    // Resolve CIA state snapshot
    m_adm_get_cia_state = (adm_get_cia_state_t)loadSym("adm_get_cia_state");

    // Resolve Bartman debug resource registry (optional — newer cores only)
    m_adm_get_debug_resources = (adm_get_debug_resources_t)loadSym("adm_get_debug_resources");
    m_adm_get_debug_resource_generation =
        (adm_get_debug_resource_generation_t)loadSym("adm_get_debug_resource_generation");
    m_adm_clear_debug_resources = (adm_clear_debug_resources_t)loadSym("adm_clear_debug_resources");
    m_adm_set_force_boot_rom = (adm_set_force_boot_rom_t)loadSym("adm_set_force_boot_rom");
    m_adm_get_debug_gate_info = (adm_get_debug_gate_info_t)loadSym("adm_get_debug_gate_info");

    // Resolve version query
    m_adm_get_version = (adm_get_version_t)loadSym("adm_get_version");
    m_adm_drain_hd_activity =
        (adm_drain_hd_activity_t)loadSym("adm_drain_hd_activity"); // optional, phase-4 core
    m_adm_get_active_filesystem_units = (adm_get_active_filesystem_units_t)loadSym(
        "adm_get_active_filesystem_units"); // optional, phase-5 core
    // Optional, core 1.84.0: host-generated WHDLoad and CD boots.
    m_adm_whdload_prepare = (adm_whdload_prepare_t)loadSym("adm_whdload_prepare");
    m_adm_cd_insert = (adm_cd_insert_t)loadSym("adm_cd_insert");
    m_adm_cd_eject = (adm_cd_eject_t)loadSym("adm_cd_eject");
    m_adm_cd_get_image = (adm_cd_get_image_t)loadSym("adm_cd_get_image");
    m_adm_extract_archive = (adm_extract_archive_t)loadSym("adm_extract_archive");
    // Optional, core 1.96.0: identify a Kickstart by content against the core's ROM database.
    m_adm_identify_rom = (adm_identify_rom_t)loadSym("adm_identify_rom");
    // Optional, core 1.99.0: RTG / Picasso96 board and screen state.
    m_adm_get_rtg_state = (adm_get_rtg_state_t)loadSym("adm_get_rtg_state");
    // Optional, core 1.86.0: the chipset and CPU the machine is running.
    m_adm_get_machine_config = (adm_get_machine_config_t)loadSym("adm_get_machine_config");
    // Optional, core 1.89.0: host serial and parallel ports. A fresh DLL has no port; give it
    // the ones the user chose.
    m_adm_serial_set_port = (adm_serial_set_port_t)loadSym("adm_serial_set_port");
    m_adm_parallel_set_output = (adm_parallel_set_output_t)loadSym("adm_parallel_set_output");
    m_adm_hostio_get_status = (adm_hostio_get_status_t)loadSym("adm_hostio_get_status");
    m_adm_midi_set_devices = (adm_midi_set_devices_t)loadSym("adm_midi_set_devices");
    if (m_adm_midi_set_devices && m_midiDevicesSet)
        m_adm_midi_set_devices(m_midiOutDevice, m_midiInDevice);
    if (m_adm_serial_set_port && m_serialPortSet)
        m_adm_serial_set_port(m_serialPort.ToCoreName().c_str());
    if (m_adm_parallel_set_output && m_parallelOutputSet)
        m_adm_parallel_set_output(
            reinterpret_cast<const char*>(m_parallelOutput.u8string().c_str()));

    // Resolve frame start cycle query
    m_adm_get_frame_start_cycles =
        (adm_get_frame_start_cycles_t)loadSym("adm_get_frame_start_cycles");
    m_adm_get_beampos = (adm_get_beampos_t)loadSym("adm_get_beampos");
    m_adm_set_state_capture = (adm_set_state_capture_t)loadSym("adm_set_state_capture");
    m_adm_get_frame_start_state = (adm_get_frame_start_state_t)loadSym("adm_get_frame_start_state");

    // Resolve display geometry query (crop/frame info for overlay coordinate mapping)
    m_adm_get_display_geometry = (adm_get_display_geometry_t)loadSym("adm_get_display_geometry");

    // DMA debug record access
    m_adm_get_dma_record = (adm_get_dma_record_t)loadSym("adm_get_dma_record");
    m_adm_get_dma_record_live = (adm_get_dma_record_t)loadSym("adm_get_dma_record_live");
    m_adm_set_debug_dma = (adm_set_debug_dma_t)loadSym("adm_set_debug_dma");
    m_adm_get_debug_dma = (adm_get_debug_dma_t)loadSym("adm_get_debug_dma");

    // Paula channel telemetry — optional, so cores predating the audio patch still load.
    m_adm_get_audio_trace = (adm_get_audio_trace_t)loadSym("adm_get_audio_trace");
    m_adm_clear_audio_trace = (adm_clear_audio_trace_t)loadSym("adm_clear_audio_trace");
    m_adm_set_audio_trace_enabled =
        (adm_set_audio_trace_enabled_t)loadSym("adm_set_audio_trace_enabled");
    m_adm_set_audio_channel_mask =
        (adm_set_audio_channel_mask_t)loadSym("adm_set_audio_channel_mask");
    m_adm_get_audio_channel_mask =
        (adm_get_audio_channel_mask_t)loadSym("adm_get_audio_channel_mask");
    m_adm_get_audio_filter_state =
        (adm_get_audio_filter_state_t)loadSym("adm_get_audio_filter_state");
    m_adm_get_disk_write_events = (adm_get_disk_write_events_t)loadSym("adm_get_disk_write_events");
    m_adm_clear_disk_write_events =
        (adm_clear_disk_write_events_t)loadSym("adm_clear_disk_write_events");
    m_adm_read_disk_track = (adm_read_disk_track_t)loadSym("adm_read_disk_track");
    // Optional (PuAE >= 1.36.0): FloppyBridge live-drive configuration.
    m_adm_set_drive_bridge_config =
        (adm_set_drive_bridge_config_t)loadSym("adm_set_drive_bridge_config");
    m_adm_clear_drive_bridge_config =
        (adm_clear_drive_bridge_config_t)loadSym("adm_clear_drive_bridge_config");
    m_adm_get_bridge_driver_list =
        (adm_get_bridge_driver_list_t)loadSym("adm_get_bridge_driver_list");
    m_adm_enum_bridge_com_ports = (adm_enum_bridge_com_ports_t)loadSym("adm_enum_bridge_com_ports");
    m_adm_get_drive_bridge_status =
        (adm_get_drive_bridge_status_t)loadSym("adm_get_drive_bridge_status");
    // Optional (PuAE >= 1.81.0): synchronous live-drive teardown for unload.
    m_adm_shutdown_drive_bridges =
        (adm_shutdown_drive_bridges_t)loadSym("adm_shutdown_drive_bridges");
    // Optional (PuAE >= 1.78.0): per-drive write protect.
    m_adm_set_floppy_write_protect =
        (adm_set_floppy_write_protect_t)loadSym("adm_set_floppy_write_protect");
    m_adm_get_floppy_write_protect =
        (adm_get_floppy_write_protect_t)loadSym("adm_get_floppy_write_protect");
    // Optional (PuAE >= 1.19.0): the disk DMA log degrades to "unavailable" on
    // older cores rather than blocking the load.
    m_adm_get_disk_dma_events = (adm_get_disk_dma_events_t)loadSym("adm_get_disk_dma_events");
    m_adm_clear_disk_dma_events = (adm_clear_disk_dma_events_t)loadSym("adm_clear_disk_dma_events");
    m_adm_get_disk_dma_payload = (adm_get_disk_dma_payload_t)loadSym("adm_get_disk_dma_payload");
    m_adm_arm_disk_break = (adm_arm_disk_break_t)loadSym("adm_arm_disk_break");
    m_adm_is_disk_break_armed = (adm_is_disk_break_armed_t)loadSym("adm_is_disk_break_armed");
    m_adm_get_disk_break_hit = (adm_get_disk_break_hit_t)loadSym("adm_get_disk_break_hit");

    // Resolve UAE memory bank symbols for dynamic memory map.
    // These are exported data symbols (addrbank globals) in the dylib.
    m_chipMemBankPtr = loadSym("chipmem_bank");
    m_bogoMemBankPtr = loadSym("bogomem_bank");
    m_fastMemBankPtr = loadSym("fastmem_bank");
    m_z3FastMemBankPtr = loadSym("z3fastmem_bank");
    m_z3ChipMemBankPtr = loadSym("z3chipmem_bank");
    m_kickMemBankPtr = loadSym("kickmem_bank");
    m_extendedKickMemBankPtr = loadSym("extendedkickmem_bank");
    m_gfxMemBankPtr = loadSym("gfxmem_bank");
    m_customBankPtr = loadSym("custom_bank");
    m_ciaBankPtr = loadSym("cia_bank");
    m_customStoragePtr = loadSym("custom_storage");

    // Memory protection (trainer locks). Absent on a core built before the memory-protect
    // patch, which SupportsProtections() reports rather than silently doing nothing.
    m_adm_protect_set = (adm_protect_set_t)loadSym("adm_protect_set");
    m_adm_protect_clear = (adm_protect_clear_t)loadSym("adm_protect_clear");
    m_adm_protect_count = (int*)loadSym("adm_protect_count");

    // Re-assert the Bartman force-boot-rom flag on every core load so the uaelib debug gate
    // is present from the next machine boot when the user has enabled the feature.
    if (m_adm_set_force_boot_rom)
        m_adm_set_force_boot_rom(m_bartmanDebugEnabled ? 1 : 0);

    // Reject any core that isn't our patched fork: the per-frame path and several debug getters
    // dereference these adm_* pointers without null-checks, so a missing symbol would crash.
    // Fail cleanly here instead. (See kRequiredCoreSymbols for the full list.)
    if (!m_adm_get_version || !m_adm_get_frame_start_cycles || !m_adm_get_bplpt_events ||
        !m_adm_get_sprpt_events || !m_adm_get_blit_events || !m_adm_get_copper_events ||
        !m_adm_get_cpu_trace || !m_adm_drain_serial_output) {
        core::Logger::Error(
            "Emulator: core '{}' is not the required patched PuAE fork (missing adm_* symbols).",
            corePath);
        UnloadCore();
        return false;
    }

    m_coreVersionString = m_adm_get_version();
    core::Logger::Info("Emulator: PuAE ADM version: {}", m_coreVersionString);

    m_coreLoaded = true;
    {
        // The exports are resolved: off-thread callers (ExtractArchive) may enter the DLL again.
        std::lock_guard<std::mutex> lock(m_offThreadCoreMutex);
        m_offThreadCoreBlocked = false;
    }
    return true;
}

void AmigaEmulator::UnloadCore()
{
    ActiveInstanceScope _g(this);

    // A background archive extraction (the CD Drive window's worker) is inside this DLL;
    // FreeLibrary() under it would take that thread down with the mapping. Wait it out here
    // and turn away any that arrives from now until a core is loaded again. Stop, closing the
    // emulator window and the power lamp all come through here, so this is the one place.
    {
        std::unique_lock<std::mutex> lock(m_offThreadCoreMutex);
        m_offThreadCoreBlocked = true;
        if (m_offThreadCoreCalls > 0)
            core::Logger::Info("Emulator: waiting for a background archive extraction to finish "
                               "before unloading the core");
        m_offThreadCoreIdle.wait(lock, [this] { return m_offThreadCoreCalls == 0; });
    }

    ClearInjectedMouseButtons();

    // A film cannot outlive the machine it was recording: close it here so the file is left
    // complete (headers patched, index written) rather than as a truncated stub.
    EndVideoCapture({});

    // Preserve a final KPrintF fragment even when it did not end with a newline.
    if (m_coreLoaded) {
        DrainSerialOutput();
        FlushSerialLogLine();
    }

    // Deliberately do NOT ShutdownAudio() here. Tearing down and recreating the
    // CoreAudio device on every power-cycle exhausts the macOS audio HAL after ~9
    // cycles (the same exhaustion InitAudio's reuse guard avoids per reload). The
    // host ma_device is independent of the core; leaving it alive lets the next load
    // reuse it via InitAudio()'s idempotent guard. The device is released in the
    // destructor, at the instance's true end of life.

    if (m_coreLoaded) {
        // Stop the live drives before anything else. Each FloppyBridge driver runs a
        // worker thread inside this library and holds a serial port open, and neither
        // retro_unload_game() nor retro_deinit() -> DISK_free() releases them. Left
        // running, the thread keeps the motor on and the head stepping after the
        // machine has stopped, then survives the FreeLibrary()/dlclose() below -
        // the next core load maps the same image at the same base and the orphan
        // wakes up inside it. Cores from 1.81.0 also do this in retro_deinit(); this
        // call is the one that reports, and it is idempotent.
        ShutdownBridgeDrives();
        if (m_retro_unload_game && m_gameLoaded) {
            m_retro_unload_game();
        }
        if (m_retro_deinit) {
            m_retro_deinit();
        }
        m_gameLoaded = false;
        m_coreLoaded = false;
        m_guiDataPtr = nullptr;
        m_chipMemBankPtr = nullptr;
        m_bogoMemBankPtr = nullptr;
        m_fastMemBankPtr = nullptr;
        m_z3FastMemBankPtr = nullptr;
        m_z3ChipMemBankPtr = nullptr;
        m_kickMemBankPtr = nullptr;
        m_extendedKickMemBankPtr = nullptr;
        m_gfxMemBankPtr = nullptr;
        m_customBankPtr = nullptr;
        m_ciaBankPtr = nullptr;
        m_customStoragePtr = nullptr;
        m_adm_protect_set = nullptr;
        m_adm_protect_clear = nullptr;
        m_adm_protect_count = nullptr;
        m_adm_get_cpu_state = nullptr;
        m_adm_get_cpu_ext_state = nullptr;
        m_adm_debug_command = nullptr;
        m_adm_get_debug_break_pending = nullptr;
        m_adm_arm_debug = nullptr;
        m_adm_arm_oneshot_breakpoint = nullptr;
        m_adm_cancel_oneshot_breakpoint = nullptr;
        m_adm_get_oneshot_breakpoint_status = nullptr;
        m_adm_arm_fb = nullptr;
        m_adm_is_fb_armed = nullptr;
        m_adm_get_fb_entry = nullptr;
        m_adm_get_fb_hit = nullptr;
        m_adm_arm_exception = nullptr;
        m_adm_get_exception_hit = nullptr;
        m_adm_arm_beam_break = nullptr;
        m_adm_get_beam_hit = nullptr;
        m_adm_get_last_memwatch = nullptr;
        m_adm_get_last_memwatch2 = nullptr;
        m_adm_set_bitplane_mask = nullptr;
        m_adm_set_sprite_mask = nullptr;
        m_adm_set_palette_vis = nullptr;
        m_adm_set_bplcon1_scroll = nullptr;
        m_adm_set_copper_block = nullptr;
        m_adm_set_bplptr_block = nullptr;
        m_adm_set_blitter_writes = nullptr;
        m_adm_redraw_frame = nullptr;
        m_adm_get_bplpt_events = nullptr;
        m_adm_clear_bplpt_events = nullptr;
        m_bplptEventCount = 0;
        m_adm_get_sprpt_events = nullptr;
        m_adm_get_sprpt_color_snaps = nullptr;
        m_adm_clear_sprpt_events = nullptr;
        m_sprptEventCount = 0;
        m_adm_get_blit_events = nullptr;
        m_adm_clear_blit_events = nullptr;
        m_blitEventCount = 0;
        m_adm_set_blit_data_enabled = nullptr;
        m_adm_get_blit_data = nullptr;
        m_adm_get_blit_data_enabled = nullptr;
        m_blitDataCaptureEnabled = false;
        m_adm_set_blitter_vis_enabled = nullptr;
        m_adm_get_blitter_vis_spans = nullptr;
        m_adm_get_blitter_vis_stats = nullptr;
        m_blitVisSpans.clear();
        m_blitVisSpans.shrink_to_fit();
        m_blitVisSpanCount = 0;
        m_blitVisEnabled = false;
        m_adm_get_idle_events = nullptr;
        m_adm_clear_idle_events = nullptr;
        m_idleEventCount = 0;
        m_adm_get_copper_events = nullptr;
        m_adm_clear_copper_events = nullptr;
        m_copperExecEventCount = 0;
        m_copperExecOverflow = false;
        m_adm_get_reg_write_events = nullptr;
        m_adm_clear_reg_write_events = nullptr;
        m_regWriteEvents.clear();
        m_regWriteEvents.shrink_to_fit();
        m_regWriteEventCount = 0;
        m_regWriteOverflow = false;
        m_adm_set_cpu_trace_enabled = nullptr;
        m_adm_get_cpu_trace = nullptr;
        m_adm_get_cpu_trace_frame = nullptr;
        m_adm_clear_cpu_trace = nullptr;
        m_adm_get_audio_trace = nullptr;
        m_adm_clear_audio_trace = nullptr;
        m_adm_set_audio_trace_enabled = nullptr;
        m_adm_set_audio_channel_mask = nullptr;
        m_adm_get_audio_channel_mask = nullptr;
        m_adm_get_audio_filter_state = nullptr;
        // Deliberately NOT clearing m_audioTraceEnabled: that is the UI's request, and it has
        // to survive a core swap so the next core gets armed again by SyncAudioTraceEnabled().
        m_audioTraceEnabledCore = false;
        for (int i = 0; i < kAudioChannels; ++i)
            m_audioTraceCount[i] = 0;
        m_adm_drain_serial_output = nullptr;
        m_serialLogLine.clear();
        m_serialLastWasCr = false;
        // The KPrintF rules themselves are host-side and deliberately survive an unload; only
        // the pending hit is dropped so a stale break doesn't fire into the next session.
        m_kprintfBreakPending = false;
        m_hostBreakPending = false;
        m_frameBreakTarget = 0;
        m_hasKprintfHit = false;
        m_hasLatchedKprintf = false;
        m_cpuTraceCount = 0;
        m_cpuTraceOverflow = false;
        m_adm_get_version = nullptr;
        m_adm_drain_hd_activity = nullptr;
        m_adm_get_active_filesystem_units = nullptr;
        m_adm_whdload_prepare = nullptr;
        m_adm_cd_insert = nullptr;
        m_adm_cd_eject = nullptr;
        m_adm_cd_get_image = nullptr;
        m_adm_extract_archive = nullptr;
        m_adm_identify_rom = nullptr;
        m_adm_get_rtg_state = nullptr;
        m_adm_get_machine_config = nullptr;
        m_adm_serial_set_port = nullptr;
        m_adm_parallel_set_output = nullptr;
        m_adm_hostio_get_status = nullptr;
        m_adm_midi_set_devices = nullptr;
        m_reportedAggregateHdFallback = false;
        m_adm_asm_one = nullptr;
        m_adm_get_frame_start_cycles = nullptr;
        m_adm_get_beampos = nullptr;
        m_adm_set_state_capture = nullptr;
        m_adm_get_frame_start_state = nullptr;
        m_lastFrameStartStateSeq = 0;
        m_stateCaptureArmed = false;
        m_restorableStateRequested = false;
        m_adm_get_display_geometry = nullptr;
        m_adm_get_dma_record = nullptr;
        m_adm_get_dma_record_live = nullptr;
        m_adm_set_debug_dma = nullptr;
        m_adm_get_debug_dma = nullptr;
        m_adm_get_disk_write_events = nullptr;
        m_adm_clear_disk_write_events = nullptr;
        m_adm_read_disk_track = nullptr;
        m_adm_set_drive_bridge_config = nullptr;
        m_adm_clear_drive_bridge_config = nullptr;
        m_adm_get_bridge_driver_list = nullptr;
        m_adm_enum_bridge_com_ports = nullptr;
        m_adm_get_drive_bridge_status = nullptr;
        m_adm_shutdown_drive_bridges = nullptr;
        m_adm_set_floppy_write_protect = nullptr;
        m_adm_get_floppy_write_protect = nullptr;
        // The core is gone, so no slot is bridged any more.
        for (bool& b : m_bridgeDrive)
            b = false;
        // Drift counts a core's behaviour, so it belongs to the core that produced it.
        for (uint32_t& d : m_floppyWriteProtectDrift)
            d = 0;
        m_warnedNoWriteProtectSupport = false;
        m_adm_get_disk_dma_events = nullptr;
        m_adm_clear_disk_dma_events = nullptr;
        m_adm_get_disk_dma_payload = nullptr;
        m_adm_arm_disk_break = nullptr;
        m_adm_is_disk_break_armed = nullptr;
        m_adm_get_disk_break_hit = nullptr;
        m_diskDmaEventCount = 0;
        m_adm_get_debug_resources = nullptr;
        m_adm_get_debug_resource_generation = nullptr;
        m_adm_clear_debug_resources = nullptr;
        m_adm_set_force_boot_rom = nullptr;
        m_adm_get_debug_gate_info = nullptr;
        m_diskWriteEventCount = 0;
        m_frameStartCycle = 0;
        ResetDemoPerformance();

        if (m_textureID) {
            glDeleteTextures(1, &m_textureID);
            m_textureID = 0;
        }
        m_texAllocW = m_texAllocH = 0; // force reallocation when the texture is recreated
    }

    if (m_libHandle) {
#ifdef _WIN32
        FreeLibrary((HMODULE)m_libHandle);
#else
        dlclose(m_libHandle);
#endif
        m_libHandle = nullptr;
    }

    RemoveTempDiskImage(m_tempAdfPath);

    // Clean up per-drive temp files
    for (auto& path : m_tempAdfPaths)
        RemoveTempDiskImage(path);

    m_hostMountService.ClearEffective();
    m_aggregateHdActivityLatch = 0;
    m_aggregateHdActivityCount = 0;
}

bool AmigaEmulator::ReinitCore()
{
    ActiveInstanceScope _g(this);

    if (!m_coreLoaded || !m_libHandle || !m_retro_init || !m_retro_deinit)
        return false;

    // Drop any loaded game and tear down the core's runtime state, then re-init the
    // *same* dylib. This clears the per-reload state PuAE accumulates (which wedges
    // the emulator after ~9 in-place reloads) without dlclose/dlopen of a fresh
    // dylib copy, which leaks file descriptors on macOS.

    // Same reason as UnloadCore(): retro_deinit() below tears the core's runtime state
    // down, and a live drive's worker thread is not part of that state.
    ShutdownBridgeDrives();

    if (m_retro_unload_game && m_gameLoaded) {
        m_retro_unload_game();
        m_gameLoaded = false;
    }

    // NOTE: deliberately do NOT ShutdownAudio() here. The host ma_device is
    // independent of the core; leaving it running lets the next LoadADF reuse it via
    // InitAudio()'s idempotent guard instead of churning a CoreAudio device per load.

    m_retro_deinit();

    // Re-install callbacks and re-initialise. The dylib handle and all resolved
    // symbol/data pointers stay valid (same image), so they need not be re-resolved.
    m_retro_set_environment(EnvironmentCallback);
    m_retro_set_video_refresh(VideoRefreshCallback);
    m_retro_set_audio_sample(AudioSampleCallback);
    m_retro_set_audio_sample_batch(AudioSampleBatchCallback);
    m_retro_set_input_poll(InputPollCallback);
    m_retro_set_input_state(InputStateCallback);

    m_retro_init();

    if (m_retro_set_controller_port_device) {
        unsigned device;
        if (m_controllerType == ControllerType::CD32Controller)
            device = (m_cd32DeviceId != 0) ? m_cd32DeviceId
                                           : RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0);
        else
            device = RETRO_DEVICE_JOYPAD;
        m_retro_set_controller_port_device(0, device);
        m_joypadPortActive[0] = true;
        m_joypadPortActive[1] = false; // re-registered on demand by set_joystick
    }

    SetModel(m_currentModel);
    return true;
}

uint64_t AmigaEmulator::GetMemoryRegionsSignature() const
{
    // Cheap signature over the raw bank descriptors (no allocation). Callers that only need to know
    // whether the bank layout changed (e.g. to avoid rebuilding derived data every frame) can poll
    // this instead of fetching the full region vector. Returns 0 when no game/core is loaded.
    if (!m_coreLoaded || !m_gameLoaded)
        return 0;

    uint64_t sig = 1469598103934665603ull; // FNV-1a offset basis
    auto mix = [&](uint64_t v) { sig = (sig ^ (v + 0x9e3779b97f4a7c15ull)) * 1099511628211ull; };
    auto hashBanks = [&](void* bankPtr, int maxEntries) {
        if (!bankPtr) {
            mix(0);
            return;
        }
        for (int i = 0; i < maxEntries; i++) {
            auto* bank = reinterpret_cast<UAEAddrBank*>(static_cast<uint8_t*>(bankPtr) +
                                                        i * sizeof(UAEAddrBank));
            mix(reinterpret_cast<uintptr_t>(bank->baseaddr));
            mix(static_cast<uint64_t>(bank->start));
            mix(static_cast<uint64_t>(bank->allocated_size));
        }
    };
    hashBanks(m_chipMemBankPtr, 1);
    hashBanks(m_bogoMemBankPtr, 1);
    hashBanks(m_fastMemBankPtr, 4);
    hashBanks(m_z3FastMemBankPtr, 4);
    hashBanks(m_z3ChipMemBankPtr, 1);
    hashBanks(m_kickMemBankPtr, 1);
    hashBanks(m_extendedKickMemBankPtr, 1);
    hashBanks(m_gfxMemBankPtr, 1);
    return sig;
}

std::vector<AmigaEmulator::MemoryRegion> AmigaEmulator::GetMemoryRegions() const
{
    if (!m_coreLoaded || !m_gameLoaded) {
        m_cachedRegionsValid = false;
        return {};
    }

    // If the bank-descriptor signature matches the last build, hand back the cached vector — its
    // friendly names ("Chip RAM", "Fast RAM", ...) are all short enough for std::string SSO, so the
    // return copy doesn't heap-allocate either.
    {
        const uint64_t sig = GetMemoryRegionsSignature();
        if (m_cachedRegionsValid && sig == m_cachedRegionsSig)
            return m_cachedRegions;
        m_cachedRegionsSig = sig;
    }

    std::vector<MemoryRegion> regions;

    // Helper: iterate up to maxEntries consecutive UAEAddrBank entries starting at bankPtr.
    // Adds any entry where allocated_size > 0 and baseaddr != null.
    // forceName: use fallbackName even when the bank carries a label. mapped_malloc_dynamic()
    // labels every bank it allocates "*" when the caller passes none, which is how the RTG
    // board's VRAM came out as a third "Fast RAM (*)" row instead of itself.
    auto addBanks = [&](void* bankPtr, const char* fallbackName, bool writable, int maxEntries = 1,
                        bool forceName = false) {
        if (!bankPtr)
            return;
        for (int i = 0; i < maxEntries; i++) {
            auto* bank = reinterpret_cast<UAEAddrBank*>(static_cast<uint8_t*>(bankPtr) +
                                                        i * sizeof(UAEAddrBank));
            // Ignore regions that have a size of 1 byte (or 0/null)
            if (bank->allocated_size <= 1 || bank->baseaddr == nullptr)
                continue;

            // Skip placeholder/invalid banks: UAE marks unused banks with a name
            // starting with 'H' followed by a non-ASCII byte (e.g. "Hï...").
            // These show up as "H?" in some renderers but the second byte is NOT
            // ASCII '?' (0x3F) — it is a high byte (> 0x7F).
            auto isUaePlaceholder = [](const char* s) {
                return s && s[0] == 'H' && (unsigned char)s[1] > 0x7F;
            };

            if (isUaePlaceholder(bank->name) || isUaePlaceholder(bank->label))
                continue;

            // Determine name: prefer bank label, fall back to our name
            std::string name =
                (!forceName && bank->label && bank->label[0]) ? bank->label : fallbackName;

            MemoryRegion r;
            // `baseaddr` is NOT uniformly biased to the bank's own start. Chip, Slow and
            // Kickstart are, so `baseaddr[addr - start]` reads them correctly; the Z2 fast
            // bank is biased to address 0 instead, and the same arithmetic then lands
            // `start` bytes short and reads back zeros. That silently broke every host-side
            // reader on a machine with Fast RAM -- including the AmigaDOS seglist walk the
            // VS Code debugger waits on, which needs ExecBase and finds it in Fast RAM.
            //
            // UAE's own translator is the authority on where a bank really lives, so ask it
            // instead of assuming. `check` first: memory.h documents it as the guard that
            // keeps `xlateaddr` from calling abort() on a bank that cannot translate.
            uint8_t* hostBase = bank->baseaddr;
            // Bytes of the allocation before the one the bank's start translates to; see below.
            uint32_t wrapLead = 0;
            bool dropTail = false;
            if (bank->xlateaddr && bank->check) {
                auto checkFn = reinterpret_cast<int (*)(uint32_t, uint32_t)>(bank->check);
                auto xlateFn = reinterpret_cast<uint8_t* (*)(uint32_t)>(bank->xlateaddr);
                if (checkFn(bank->start, 1)) {
                    if (uint8_t* translated = xlateFn(bank->start))
                        hostBase = translated;
                }
                // A Z2 board is allocated before autoconfig gives it an address, so UAE fixes
                // the bank's `startaccessmask` (start & mask) at 0 and later maps the board at
                // $200000: the bank translates addr to baseaddr + (addr & mask). For a 4 or 8 MB
                // board that stores the board rotated - $200000 lands 2 MB into the block and
                // the top of the board wraps round to its first byte. Reading the whole size
                // from the translated start runs off the end of the allocation (it crashed the
                // Memory Map), so the tail is reported as a second region once the translator
                // confirms where it wrapped - and left out, not guessed, if it does not.
                const uint32_t lead = static_cast<uint32_t>(hostBase - bank->baseaddr);
                if (hostBase > bank->baseaddr && lead < bank->allocated_size) {
                    const uint32_t tailStart = bank->start + (bank->allocated_size - lead);
                    const bool wraps =
                        checkFn(tailStart, 1) && xlateFn(tailStart) == bank->baseaddr;
                    wrapLead = lead;
                    dropTail = !wraps;
                }
            }
            r.hostPtr = hostBase;
            r.amigaAddr = bank->start;
            r.size = bank->allocated_size;
            r.writable = writable;

            // Apply lookup table for prettier names
            if (name == "chip")
                name = "Chip RAM";
            else if (name == "kick")
                name = "Kickstart ROM";
            else if (name == "fast")
                name = "Fast RAM";
            else if (name == "slow")
                name = "Slow RAM";
            else if (name == "bogo")
                name = "Slow RAM (bogo)";
            else if (name == "chip memory")
                name = "Chip RAM";
            else if (name == "kickstart ROM")
                name = "Kickstart ROM";
            else if (name == "fast memory")
                name = "Fast RAM";
            else if (name == "*")
                name = "Fast RAM (*)";
            else if (name == "slow memory")
                name = "Slow RAM";
            else if (name == "z3 fast memory")
                name = "Z3 Fast RAM";
            else if (name == "z3 chip memory")
                name = "Z3 Chip RAM";
            // The extended ROM sits at $E00000 on a CD32 and $F00000 on a CDTV, and UAE labels
            // the bank after wherever it landed.
            else if (name == "extended ROM" || name == "rom_e0" || name == "rom_f0")
                name = "Extended ROM";

            // Fix for Kickstart ROM showing at 0x000000 due to boot overlay
            if (name == "Kickstart ROM" && r.amigaAddr == 0) {
                if (r.size == 512 * 1024) {
                    r.amigaAddr = 0xF80000;
                } else if (r.size == 256 * 1024) {
                    r.amigaAddr = 0xFC0000;
                }
            }

            r.name = name;
            if (wrapLead != 0) {
                MemoryRegion tail = r;
                r.size = bank->allocated_size - wrapLead;
                tail.amigaAddr = r.amigaAddr + r.size;
                tail.size = wrapLead;
                tail.hostPtr = bank->baseaddr;
                regions.push_back(r);
                if (!dropTail)
                    regions.push_back(tail);
                continue;
            }
            regions.push_back(r);
        }
    };

    addBanks(m_chipMemBankPtr, "chip memory", true);
    addBanks(m_bogoMemBankPtr, "slow memory", true);
    addBanks(m_fastMemBankPtr, "fast memory", true, 4);
    addBanks(m_z3FastMemBankPtr, "z3 fast memory", true, 4);
    addBanks(m_z3ChipMemBankPtr, "z3 chip memory", true);
    addBanks(m_kickMemBankPtr, "kickstart ROM", false);
    // Only a CD32 or CDTV has one; on every other machine the bank is empty and
    // addBanks contributes nothing.
    addBanks(m_extendedKickMemBankPtr, "extended ROM", false);
    // The uaegfx board. Empty unless puae_gfxcard_size fitted one, so like the extended ROM
    // above this normally contributes nothing.
    addBanks(m_gfxMemBankPtr, "RTG VRAM", true, 1, /*forceName=*/true);

    std::sort(regions.begin(), regions.end(), [](const MemoryRegion& a, const MemoryRegion& b) {
        return a.amigaAddr < b.amigaAddr;
    });

    m_cachedRegions = regions;
    m_cachedRegionsValid = true;
    return regions;
}

bool AmigaEmulator::GetCpuExtState(core::CpuExtendedState& out) const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_get_cpu_ext_state)
        return false;
    core::CpuExtRaw raw{};
    const int n = m_adm_get_cpu_ext_state(&raw, static_cast<int>(sizeof(raw)));
    auto parsed = core::ParseCpuExtState(raw, n);
    if (!parsed)
        return false;
    out = std::move(*parsed);
    return true;
}

bool AmigaEmulator::GetCpuState(CpuState& outState) const
{
    if (!m_coreLoaded || !m_gameLoaded)
        return false;

    uint32_t regs[16];
    m_adm_get_cpu_state(regs, &outState.pc, &outState.sr);

    for (int i = 0; i < 8; i++) {
        outState.d[i] = regs[i];
        outState.a[i] = regs[i + 8];
    }

    return true;
}

bool AmigaEmulator::Disassemble(uint32_t pc, char* mnemo, int mnemoSz, char* hex, int hexSz,
                                uint32_t& nextPc) const
{
    if (!m_coreLoaded || !m_gameLoaded)
        return false;
    nextPc = m_adm_disasm_one(pc, mnemo, mnemoSz, hex, hexSz);
    return nextPc != pc; // nextPc == pc indicates disasm failure
}

int AmigaEmulator::Assemble(const std::string& src, uint32_t addr, uint16_t* words,
                            int maxWords) const
{
    if (!m_adm_asm_one || !m_coreLoaded || !m_gameLoaded)
        return -2;
    return m_adm_asm_one(src.c_str(), addr, words, maxWords);
}

void AmigaEmulator::PushDisplayMasks()
{
    // Both exports are optional (older cores / no core loaded yet) — the host-side masks are
    // still remembered and re-pushed once a core that has them is loaded.
    if (m_adm_set_bitplane_mask)
        m_adm_set_bitplane_mask(core::EffectiveBitplaneMask(m_displayMasks));
    if (m_adm_set_sprite_mask)
        m_adm_set_sprite_mask(m_displayMasks.sprites);
    // Re-render the buffered frame so the new masking is visible immediately, including while
    // paused — this is what makes screenshots (MCP `screenshot`) reflect the current masks.
    RequestRedraw();
}

void AmigaEmulator::PushDisplayOptions()
{
    // Optional exports, same as the masks: remembered host-side and pushed once a core that has
    // them is loaded.
    if (m_adm_set_palette_vis)
        m_adm_set_palette_vis(m_paletteVisEnabled ? 1 : 0);
    if (m_adm_set_bplcon1_scroll)
        m_adm_set_bplcon1_scroll(m_bplcon1ScrollEnabled ? 1 : 0);
    if (m_adm_set_copper_block) {
        m_adm_set_copper_block(m_copperBlock.enabled ? 1 : 0, m_copperBlock.startLine,
                               m_copperBlock.endLine);
    }
    if (m_adm_set_bplptr_block) {
        m_adm_set_bplptr_block(m_bplPtrBlock.mask, m_bplPtrBlock.startLine, m_bplPtrBlock.endLine);
    }
    if (m_adm_set_blitter_writes)
        m_adm_set_blitter_writes(m_blitterWritesEnabled ? 1 : 0);
    // The palette ramp is applied in the drawing stage, so re-rendering the buffered frame shows
    // it straight away. The BPLCON1 override is applied during bitplane fetch, so that one only
    // takes effect on the next emulated frame — the redraw costs nothing and is harmless.
    RequestRedraw();
}

void AmigaEmulator::FlushSerialLogLine()
{
    if (m_serialLogLine.empty())
        return;

    core::Logger::LogMessage(core::LogLevel::Debug, "KPrintF: " + m_serialLogLine);

    // Break-on-KPrintF-text: offer the completed line to the traps. Matching is line-based, so
    // the break lands at the end of the line (or, for output with no newline, up to one frame
    // later when the fragment is flushed) rather than inside the KPrintF call itself.
    if (auto hit = m_kprintfBreakpoints.MatchLine(m_serialLogLine)) {
        m_kprintfHit = std::move(*hit);
        m_hasKprintfHit = true;
        m_hasLatchedKprintf = true;
        m_kprintfBreakPending = true;
        core::Logger::LogMessage(core::LogLevel::Info,
                                 "KPrintF breakpoint hit: \"" + m_kprintfHit.text + "\"");
    }

    m_serialLogLine.clear();
}

void AmigaEmulator::DrainSerialOutput()
{
    if (!m_adm_drain_serial_output)
        return;

    constexpr int kDrainSize = 4096;
    constexpr size_t kMaxLineSize = 4096;
    char bytes[kDrainSize];
    bool receivedBytes = false;

    while (true) {
        int count = m_adm_drain_serial_output(bytes, kDrainSize);
        if (count <= 0)
            break;

        receivedBytes = true;
        count = std::min(count, kDrainSize);
        for (int i = 0; i < count; ++i) {
            unsigned char c = static_cast<unsigned char>(bytes[i]);

            // Treat either convention as a line ending without emitting a duplicate
            // empty entry for CRLF pairs, including pairs split across two drains.
            if (c == '\r') {
                FlushSerialLogLine();
                m_serialLastWasCr = true;
                continue;
            }
            if (c == '\n') {
                if (!m_serialLastWasCr)
                    FlushSerialLogLine();
                m_serialLastWasCr = false;
                continue;
            }
            m_serialLastWasCr = false;

            if (c == '\b') {
                if (!m_serialLogLine.empty()) {
                    bool removeUtf8Sequence =
                        (static_cast<unsigned char>(m_serialLogLine.back()) & 0xc0) == 0x80;
                    m_serialLogLine.pop_back();
                    while (removeUtf8Sequence && !m_serialLogLine.empty() &&
                           (static_cast<unsigned char>(m_serialLogLine.back()) & 0xc0) == 0x80) {
                        m_serialLogLine.pop_back();
                    }
                    if (removeUtf8Sequence && !m_serialLogLine.empty())
                        m_serialLogLine.pop_back();
                }
            } else if (c == '\t' || (c >= 0x20 && c < 0x7f)) {
                m_serialLogLine.push_back(static_cast<char>(c));
            } else if (c >= 0x80) {
                // ImGui expects UTF-8. Amiga serial output is normally Latin-1, so
                // convert extended bytes rather than passing invalid UTF-8 through.
                m_serialLogLine.push_back(static_cast<char>(c < 0xc0 ? 0xc2 : 0xc3));
                m_serialLogLine.push_back(static_cast<char>(c < 0xc0 ? c : c - 0x40));
            }

            if (m_serialLogLine.size() >= kMaxLineSize)
                FlushSerialLogLine();
        }

        if (count < kDrainSize)
            break;
    }

    // KPrintF calls do not have to include a newline. Once a whole emulated frame
    // passes without another byte, treat the accumulated fragment as complete.
    if (!receivedBytes)
        FlushSerialLogLine();
}

void AmigaEmulator::SetDisplayMasks(const core::DisplayMasks& masks)
{
    m_displayMasks = masks;
    m_displayMasks.playfields &= 0x3;
    PushDisplayMasks();
}

void AmigaEmulator::SetBitplaneMask(uint8_t mask)
{
    m_displayMasks.bitplanes = mask;
    PushDisplayMasks();
}

void AmigaEmulator::SetSpriteMask(uint8_t mask)
{
    m_displayMasks.sprites = mask;
    PushDisplayMasks();
}

void AmigaEmulator::SetPlayfieldMask(uint8_t mask)
{
    m_displayMasks.playfields = mask & 0x3;
    PushDisplayMasks();
}

void AmigaEmulator::SetPaletteVisEnabled(bool enabled)
{
    if (m_paletteVisEnabled == enabled)
        return;
    m_paletteVisEnabled = enabled;
    PushDisplayOptions();
}

void AmigaEmulator::SetBplcon1ScrollEnabled(bool enabled)
{
    if (m_bplcon1ScrollEnabled == enabled)
        return;
    m_bplcon1ScrollEnabled = enabled;
    PushDisplayOptions();
}

void AmigaEmulator::SetCopperBlock(const core::CopperBlock& block)
{
    m_copperBlock.enabled = block.enabled;
    m_copperBlock.startLine = core::ClampBlockLine(block.startLine);
    m_copperBlock.endLine = core::ClampBlockLine(block.endLine);
    PushDisplayOptions();
}

void AmigaEmulator::SetBplPtrBlock(const core::BplPtrBlock& block)
{
    m_bplPtrBlock.mask = block.mask & core::kBplPtrBlockMaskAll;
    m_bplPtrBlock.startLine = core::ClampBlockLine(block.startLine);
    m_bplPtrBlock.endLine = core::ClampBlockLine(block.endLine);
    PushDisplayOptions();
}

void AmigaEmulator::SetBlitterWritesEnabled(bool enabled)
{
    if (m_blitterWritesEnabled == enabled)
        return;
    m_blitterWritesEnabled = enabled;
    PushDisplayOptions();
}

void AmigaEmulator::RequestRedraw()
{
    if (m_adm_redraw_frame)
        m_adm_redraw_frame();
}

int AmigaEmulator::GetBplptEvents(BplptEvent* out, int maxEvents) const
{
    int count = std::min(m_bplptEventCount, maxEvents);
    if (out && count > 0)
        std::memcpy(out, m_bplptEvents, static_cast<size_t>(count) * sizeof(BplptEvent));
    return count;
}

int AmigaEmulator::GetSprptEvents(SprptEvent* out, int maxEvents) const
{
    int count = std::min(m_sprptEventCount, maxEvents);
    if (out && count > 0)
        std::memcpy(out, m_sprptEvents, static_cast<size_t>(count) * sizeof(SprptEvent));
    return count;
}

int AmigaEmulator::GetBlitEvents(BlitEvent* out, int maxEvents) const
{
    int count = std::min(m_blitEventCount, maxEvents);
    if (out && count > 0)
        std::memcpy(out, m_blitEvents, static_cast<size_t>(count) * sizeof(BlitEvent));
    return count;
}

static_assert(sizeof(AmigaEmulator::BlitVisSpan) == 12,
              "BlitVisSpan must match adm_blitvis_span_t ABI (12 bytes)");

static_assert(sizeof(AmigaEmulator::BlitWord) == 8,
              "BlitWord must match adm_blit_word_t ABI (8 bytes)");

void AmigaEmulator::SetBlitDataCaptureEnabled(bool enabled)
{
    if (!m_adm_set_blit_data_enabled)
        return; // core predates the destination-data capture

    // Deliberately not short-circuited on the mirror: this is also the path that re-arms
    // capture after a core reload, where the mirror and the core have gone out of step.
    m_adm_set_blit_data_enabled(enabled ? 1 : 0);
    m_blitDataCaptureEnabled = enabled;
}

bool AmigaEmulator::IsBlitDataCaptureEnabled() const
{
    if (m_adm_get_blit_data_enabled)
        return m_adm_get_blit_data_enabled() != 0;
    return m_blitDataCaptureEnabled;
}

int AmigaEmulator::GetBlitData(uint32_t off, uint32_t count, BlitWord* out) const
{
    if (!m_coreLoaded || !m_adm_get_blit_data || !out || count == 0)
        return 0;
    return m_adm_get_blit_data(off, count, out);
}

void AmigaEmulator::SetBlitterVisEnabled(bool enabled)
{
    if (!m_adm_set_blitter_vis_enabled)
        return; // core predates the provenance patch
    if (m_blitVisEnabled == enabled)
        return;

    m_adm_set_blitter_vis_enabled(enabled ? 1 : 0);
    m_blitVisEnabled = enabled;

    if (enabled) {
        m_blitVisSpans.resize(kMaxBlitVisSpans);
    } else {
        m_blitVisSpans.clear();
        m_blitVisSpans.shrink_to_fit();
    }
    m_blitVisSpanCount = 0;
}

int AmigaEmulator::GetBlitVisSpans(BlitVisSpan* out, int maxSpans) const
{
    int count = std::min(m_blitVisSpanCount, maxSpans);
    if (out && count > 0)
        std::memcpy(out, m_blitVisSpans.data(), static_cast<size_t>(count) * sizeof(BlitVisSpan));
    return count;
}

void AmigaEmulator::ClearBlitterVisMap()
{
    // Re-arming the enabled state clears the map without reallocating its ~50 MB of tables.
    if (m_blitVisEnabled && m_adm_set_blitter_vis_enabled)
        m_adm_set_blitter_vis_enabled(1);
    m_blitVisSpanCount = 0;
}

AmigaEmulator::BlitVisStats AmigaEmulator::GetBlitVisStats() const
{
    BlitVisStats stats;
    if (!m_adm_get_blitter_vis_stats)
        return stats;
    int enabled = 0, writes = 0, tags = 0, spans = 0;
    m_adm_get_blitter_vis_stats(&enabled, &writes, &tags, &spans);
    stats.enabled = enabled != 0;
    stats.writes = writes;
    stats.tags = tags;
    stats.spans = spans;
    return stats;
}

static_assert(sizeof(AmigaEmulator::IdleEvent) == 12,
              "IdleEvent must match adm_idle_event_t ABI (12 bytes)");

int AmigaEmulator::GetIdleEvents(IdleEvent* out, int maxEvents) const
{
    int count = std::min(m_idleEventCount, maxEvents);
    if (out && count > 0)
        std::memcpy(out, m_idleEvents, static_cast<size_t>(count) * sizeof(IdleEvent));
    return count;
}

void AmigaEmulator::ResetDemoPerformance()
{
    for (auto& interval : m_demoPerfIntervals)
        interval = {};
    m_demoPerfHead = 0;
    m_demoPerfCount = 0;
    m_demoTotalCycles = 0;
    m_demoIdleCycles = 0;
    m_demoTimingCycles = 0;
    m_demoTimingFrames = 0;
    m_demoAccumulatedIdleCycles = 0;
    m_demoIdleAtLastFrameEnd = 0;
    m_demoLastFrameEndCycle = 0;
    m_demoIdleStartCycle = 0;
    m_demoHaveFrameEnd = false;
    m_demoIdleActive = false;
    m_demoSawIdleStop = false;
    m_demoPerfAvailable = false;
    m_demoFps = 0.0f;
    m_demoIdlePercent = 0.0f;
}

void AmigaEmulator::UpdateDemoPerformance(uint32_t frameStart, uint32_t frameEnd)
{
    if (!m_cpuTraceEnabled || !m_bartmanDebugEnabled || !m_adm_get_idle_events)
        return;

    const uint32_t frameCycles = frameEnd - frameStart;
    if (frameCycles == 0)
        return;

    // Derive the emulated CCK rate from actual core frame boundaries instead of assuming PAL.
    m_demoTimingCycles += frameCycles;
    ++m_demoTimingFrames;

    IdleEvent events[kMaxIdleEvents];
    int eventCount = m_adm_get_idle_events(events, kMaxIdleEvents);
    eventCount = std::min(eventCount, kMaxIdleEvents);

    bool haveFrameEndMarker = false;
    uint32_t frameEndMarker = 0;
    for (int i = 0; i < eventCount; ++i) {
        const IdleEvent& event = events[i];
        if (event.cycle - frameStart >= frameCycles)
            continue;

        if (event.start != 0) {
            // Starts separated by stops are split idle segments. Keep the last segment start in
            // this video frame as its CPU-finished marker, but retain every segment for idle time.
            if (!m_demoIdleActive) {
                m_demoIdleActive = true;
                m_demoIdleStartCycle = event.cycle;
                haveFrameEndMarker = true;
                frameEndMarker = event.cycle;
            }
        } else if (m_demoIdleActive) {
            m_demoAccumulatedIdleCycles += event.cycle - m_demoIdleStartCycle;
            m_demoIdleActive = false;
            m_demoSawIdleStop = true;
        }
    }

    if (!haveFrameEndMarker)
        return;

    if (m_demoHaveFrameEnd && m_demoSawIdleStop) {
        const uint32_t totalCycles = frameEndMarker - m_demoLastFrameEndCycle;
        if (totalCycles > 0) {
            if (m_demoPerfCount == kDemoPerfWindowFrames) {
                const DemoPerfInterval& oldest = m_demoPerfIntervals[m_demoPerfHead];
                m_demoTotalCycles -= oldest.totalCycles;
                m_demoIdleCycles -= oldest.idleCycles;
            } else {
                ++m_demoPerfCount;
            }

            const uint64_t idleDelta = m_demoAccumulatedIdleCycles - m_demoIdleAtLastFrameEnd;
            const uint32_t idleCycles =
                static_cast<uint32_t>(std::min<uint64_t>(idleDelta, totalCycles));
            m_demoPerfIntervals[m_demoPerfHead] = {totalCycles, idleCycles};
            m_demoPerfHead = (m_demoPerfHead + 1) % kDemoPerfWindowFrames;
            m_demoTotalCycles += totalCycles;
            m_demoIdleCycles += idleCycles;

            const double cckPerSecond = m_targetFps * static_cast<double>(m_demoTimingCycles) /
                                        static_cast<double>(m_demoTimingFrames);
            m_demoFps = static_cast<float>(cckPerSecond * static_cast<double>(m_demoPerfCount) /
                                           static_cast<double>(m_demoTotalCycles));
            m_demoIdlePercent = static_cast<float>(100.0 * static_cast<double>(m_demoIdleCycles) /
                                                   static_cast<double>(m_demoTotalCycles));
            m_demoPerfAvailable = true;
        }
    }

    m_demoLastFrameEndCycle = frameEndMarker;
    m_demoIdleAtLastFrameEnd = m_demoAccumulatedIdleCycles;
    m_demoHaveFrameEnd = true;
    m_demoSawIdleStop = false;
}

int AmigaEmulator::GetCopperExecEvents(CopperExecEvent* out, int maxEvents) const
{
    int count = std::min(m_copperExecEventCount, maxEvents);
    if (out && count > 0)
        std::memcpy(out, m_copperExecEvents, static_cast<size_t>(count) * sizeof(CopperExecEvent));
    return count;
}

int AmigaEmulator::GetRegWriteEvents(RegWriteEvent* out, int maxEvents) const
{
    int count = std::min(m_regWriteEventCount, maxEvents);
    if (out && count > 0)
        std::memcpy(out, m_regWriteEvents.data(),
                    static_cast<size_t>(count) * sizeof(RegWriteEvent));
    return count;
}

bool AmigaEmulator::GetBeamPosition(int& outVpos, int& outHpos) const
{
    if (!m_adm_get_beampos)
        return false;
    uint32_t cycle = 0;
    int vpos = 0, hpos = 0;
    m_adm_get_beampos(&cycle, &vpos, &hpos);
    outVpos = vpos;
    outHpos = hpos;
    return true;
}

int AmigaEmulator::GetCpuTrace(CpuTraceEntry* out, int max) const
{
    int count = std::min(m_cpuTraceCount, max);
    if (out && count > 0)
        std::memcpy(out, m_cpuTrace, static_cast<size_t>(count) * sizeof(CpuTraceEntry));
    return count;
}

void AmigaEmulator::ReapplyCoreDebugState() const
{
    // retro_load_game() re-initialises the core, which silently resets every core-side debug
    // toggle -- but the host's mirror of them survives. The result is a split brain:
    // IsCpuTraceEnabled() keeps returning true while the core has stopped tracing, so the
    // profiler, the frame scrubber, trace history and COV all report "no trace" with no
    // indication why, and toggling the setting off and on again is the only cure. Push the
    // mirrored state back into the core after every load instead.
    if (m_adm_set_cpu_trace_enabled)
        m_adm_set_cpu_trace_enabled(m_cpuTraceEnabled ? 1 : 0);
    // The channel mask is core state too, back to all four on a fresh core.
    if (m_forceSilent && m_adm_set_audio_channel_mask)
        m_adm_set_audio_channel_mask(0);
}

void AmigaEmulator::SetCpuTraceEnabled(bool enabled)
{
    if (m_cpuTraceEnabled != enabled)
        ResetDemoPerformance();
    m_cpuTraceEnabled = enabled;
    m_adm_set_cpu_trace_enabled(enabled ? 1 : 0);
}

AmigaEmulator::DisplayGeometry AmigaEmulator::GetDisplayGeometry() const
{
    DisplayGeometry g;
    if (m_adm_get_display_geometry)
        m_adm_get_display_geometry(&g.fullW, &g.fullH, &g.cropX, &g.cropY, &g.visibleLeftBorder,
                                   &g.frameYAdjust);
    return g;
}

bool AmigaEmulator::GetDmaRecord(DmaSlotRecord** outPtr, int& outHposCount, int& outVposCount) const
{
    if (!m_adm_get_dma_record) {
        if (outPtr)
            *outPtr = nullptr;
        outHposCount = 0;
        outVposCount = 0;
        return false;
    }
    void* ptr = nullptr;
    int count = m_adm_get_dma_record(&ptr, &outHposCount, &outVposCount);
    if (outPtr)
        *outPtr = reinterpret_cast<DmaSlotRecord*>(ptr);
    return count > 0;
}

bool AmigaEmulator::GetDmaRecordLive(DmaSlotRecord** outPtr, int& outHposCount,
                                     int& outVposCount) const
{
    // Fall back to the completed-frame record on cores without the live export.
    auto fn = m_adm_get_dma_record_live ? m_adm_get_dma_record_live : m_adm_get_dma_record;
    if (!fn) {
        if (outPtr)
            *outPtr = nullptr;
        outHposCount = 0;
        outVposCount = 0;
        return false;
    }
    void* ptr = nullptr;
    int count = fn(&ptr, &outHposCount, &outVposCount);
    if (outPtr)
        *outPtr = reinterpret_cast<DmaSlotRecord*>(ptr);
    return count > 0;
}

void AmigaEmulator::SetDebugDma(int mode)
{
    if (m_adm_set_debug_dma)
        m_adm_set_debug_dma(mode);
}

int AmigaEmulator::GetDebugDma() const
{
    if (m_adm_get_debug_dma)
        return m_adm_get_debug_dma();
    return 0;
}

void AmigaEmulator::ArmDebugDmaForCapture()
{
    if (!HasDmaDebug() || GetDebugDma() != 0)
        return;
    SetDebugDma(1);
    m_captureArmedDebugDma = true;
}

int AmigaEmulator::ReadDiskTrack(int drive, int track, uint8_t* buf, int buflen) const
{
    if (!m_adm_read_disk_track)
        return -1;
    return m_adm_read_disk_track(drive, track, buf, buflen);
}

bool AmigaEmulator::WriteDiskBytes(int drive, uint32_t diskByteOffset, const uint8_t* data,
                                   uint32_t numBytes)
{
    if (!m_adm_read_disk_track)
        return false;
    if (drive < 0 || drive > 3)
        return false;

    constexpr int kTrackSize = 11 * 512; // 5632
    constexpr int kNumTracks = 160;
    constexpr uint32_t kDiskTotal = (uint32_t)kNumTracks * kTrackSize;

    if (diskByteOffset + numBytes > kDiskTotal)
        return false;

    std::vector<uint8_t> adf(kDiskTotal, 0);
    for (int t = 0; t < kNumTracks; t++)
        m_adm_read_disk_track(drive, t, adf.data() + t * kTrackSize, kTrackSize);

    memcpy(adf.data() + diskByteOffset, data, numBytes);
    return InsertDisk(drive, adf);
}

// --- FloppyBridge -----------------------------------------------------------

std::string AmigaEmulator::BuildBridgeConfigString(const BridgeConfig& cfg)
{
    // Upstream's own serialisation, matching BridgeConfig::toString() in
    // patches/floppybridge/floppybridge_lib.cpp, so a string written here is
    // interchangeable with one written by WinUAE's FloppyBridge.dll:
    //     [bridgeIndex|flags|comPort|bridgeMode|bridgeDensity]
    // flags packs autoCache (1), driveCable low bit (2), autoDetectComPort (4),
    // smartSpeed (8) and driveCable high bits (16|32). autoDetectComPort is
    // always off here: the UI picks a port from EnumBridgeComPorts().
    //
    // Bit 64 is allowWrites, and it is OURS rather than upstream's - upstream leaves
    // that bit free, so a string written here still reads correctly in WinUAE's
    // FloppyBridge.dll (which ignores the bit) and a string from it reads as
    // writes-not-allowed here, which is the safe default. See
    // patches/floppybridge/floppybridge_lib.cpp, which parses it.
    const unsigned cable = (unsigned)cfg.driveCable;
    const unsigned flags = (cfg.autoCache ? 1u : 0u) | ((cable & 1u) << 1) | ((cable & 6u) << 3) |
                           (cfg.smartSpeed ? 8u : 0u) | (cfg.allowWrites ? 64u : 0u);

    std::string s = "[" + std::to_string(cfg.driverIndex) + "|" + std::to_string(flags) + "|" +
                    cfg.comPort + "|" + std::to_string(cfg.bridgeMode) + "|" +
                    std::to_string(cfg.bridgeDensity) + "]";

    // UAE's floppyslot::dfxprofile is TCHAR[32] and the core prefixes
    // "FloppyBridge:" (13 chars). Refuse rather than let the core truncate.
    constexpr size_t kMaxProfile = 32;
    constexpr size_t kPrefixLen = 13;
    if (s.size() + kPrefixLen + 1 > kMaxProfile)
        return std::string();

    return s;
}

bool AmigaEmulator::AttachBridgeDrive(unsigned driveIndex, const BridgeConfig& cfg)
{
    if (driveIndex >= 4 || !m_adm_set_drive_bridge_config)
        return false;

    // Require a port. Measured: opening a port that does not exist fails in ~12 ms,
    // but leaving the field empty makes the driver auto-scan every serial device on
    // the machine and handshake with whatever answers - 819 ms on this machine, ~41
    // frames of stall inside DISK_vsync(). The UI always picks a port from
    // EnumBridgeComPorts(), so refusing here costs nothing.
    if (cfg.comPort.empty())
        return false;

    const std::string config = BuildBridgeConfigString(cfg);
    if (config.empty())
        return false;

    if (m_adm_set_drive_bridge_config((int)driveIndex, config.c_str()) == 0)
        return false;

    m_bridgeDrive[driveIndex] = true;
    return true;
}

void AmigaEmulator::DetachBridgeDrive(unsigned driveIndex)
{
    if (driveIndex >= 4)
        return;

    m_bridgeDrive[driveIndex] = false;
    if (m_adm_clear_drive_bridge_config)
        m_adm_clear_drive_bridge_config((int)driveIndex);
}

int AmigaEmulator::ShutdownBridgeDrives()
{
    const bool anyBridged =
        std::any_of(std::begin(m_bridgeDrive), std::end(m_bridgeDrive), [](bool b) { return b; });

    if (!m_adm_shutdown_drive_bridges) {
        // Only worth saying when a slot is actually live: on an older core this is
        // the difference between a clean stop and a drive that keeps knocking.
        if (anyBridged)
            core::Logger::Warn("Live floppy: this core cannot stop its live drives on unload "
                               "(no adm_shutdown_drive_bridges; rebuild the core, expect "
                               "1.81.0 or later). The drive may keep running after the "
                               "machine stops.");
        return -1;
    }

    const int stopped = m_adm_shutdown_drive_bridges();
    if (stopped > 0)
        core::Logger::Info("Live floppy: stopped {} live drive(s) before unloading the core",
                           stopped);
    for (bool& b : m_bridgeDrive)
        b = false;
    return stopped;
}

bool AmigaEmulator::IsBridgeDrive(unsigned driveIndex) const
{
    return driveIndex < 4 && m_bridgeDrive[driveIndex];
}

bool AmigaEmulator::GetBridgeStatus(unsigned driveIndex, BridgeStatus& out) const
{
    out = BridgeStatus{};
    if (driveIndex >= 4 || !m_adm_get_drive_bridge_status)
        return false;
    static_assert(sizeof(BridgeStatus) == 5 * sizeof(int) + 64 + 256,
                  "BridgeStatus must stay byte-identical to adm_bridge_status_t");
    return m_adm_get_drive_bridge_status((int)driveIndex, &out) != 0;
}

namespace {
// Options the environment callback answers from the selected machine or the attached controller,
// before it ever looks at what the core declared for itself.
bool IsMachineDerivedVariable(const std::string& key)
{
    static const std::unordered_set<std::string> kKeys = {
        "puae_model",   "puae_chip_memory",  "puae_slow_memory",     "puae_fast_memory",
        "puae_joyport", "puae_joyport_type", "puae_cd32pad_options", "puae_retropad_options"};
    return kKeys.count(key) != 0;
}

// True when Peekaboo supplies this option itself, so remembering the core's own default for it
// would only be misleading - ResolveCoreVariable() never gets that far.
bool IsCoreVariableOverridden(const std::string& key)
{
    return IsMachineDerivedVariable(key) || !core::EmulatorOptionDefault(key).empty();
}

// Split the core's newline-separated list export into a vector, dropping the
// trailing empty element the final '\n' produces.
std::vector<std::string> SplitBridgeList(const char* buf)
{
    std::vector<std::string> out;
    const char* p = buf;
    while (*p) {
        const char* nl = strchr(p, '\n');
        if (!nl) {
            out.emplace_back(p);
            break;
        }
        if (nl != p)
            out.emplace_back(p, (size_t)(nl - p));
        p = nl + 1;
    }
    return out;
}
} // namespace

std::vector<std::string> AmigaEmulator::GetKnownBridgeDriverList()
{
    // Only a fallback for when the core is not loaded - GetBridgeDriverList()
    // asks the core itself whenever it can. The index order is fixed by
    // staticDriverInfo() in patches/floppybridge/floppybridge_lib.cpp, and the
    // names are each driver's own staticBridgeInformation().
    //
    // 0-2 are upstream FloppyDriveBridge. 3 and 4 are ours, and both are
    // streaming drivers: one track at a time, read from the platter on demand,
    // no cache and no read-ahead.
    // 5 is the XCopy board again, driven the way the Greaseweazle is: bounded
    // reads into the template's revolution cache.
    return {"DrawBridge aka Arduino Reader/Writer", "Greaseweazle", "SuperCard Pro",
            "XCopy Standalone (streaming)", "XCopy Standalone (buffered)"};
}

std::vector<std::string> AmigaEmulator::GetBridgeDriverList() const
{
    // Before the core is loaded there is nothing to ask, and "no drivers" would be
    // a lie: the drivers are compiled into the bundled core. Answer from the known
    // list so the UI can be configured with the emulator stopped. Once the core is
    // loaded it is the authority - including when it says nothing, which is what an
    // old core without bridge support genuinely means.
    if (!m_coreLoaded)
        return GetKnownBridgeDriverList();

    if (!m_adm_get_bridge_driver_list)
        return {};

    char buf[1024] = {0};
    if (m_adm_get_bridge_driver_list(buf, (int)sizeof(buf)) < 0)
        return {};
    return SplitBridgeList(buf);
}

std::vector<std::string> AmigaEmulator::EnumBridgeComPorts() const
{
    // Same reasoning as the driver list, except the fallback is a real host-side
    // enumeration rather than a fixed table: the ports exist whether or not the
    // core is loaded, and the user has to be able to pick one before starting.
    if (!m_adm_enum_bridge_com_ports)
        return EnumHostSerialPorts();

    // Enumeration walks the OS device list, not the serial link, so this does
    // not block on USB even with a device attached.
    char buf[4096] = {0};
    if (m_adm_enum_bridge_com_ports(buf, (int)sizeof(buf)) < 0)
        return {};
    return SplitBridgeList(buf);
}

std::vector<std::string> AmigaEmulator::EnumHostSerialPorts()
{
    std::vector<std::string> out;
#ifdef _WIN32
    // QueryDosDevice with a null name lists every DOS device; the COMn entries are
    // exactly the ports present right now. kernel32 only - no setupapi, no registry.
    std::vector<char> buf(65536);
    DWORD len = QueryDosDeviceA(nullptr, buf.data(), (DWORD)buf.size());
    if (len == 0)
        return out;
    for (const char* p = buf.data(); *p; p += strlen(p) + 1) {
        if (strncmp(p, "COM", 3) != 0)
            continue;
        const char* digits = p + 3;
        if (*digits == 0)
            continue;
        bool numeric = true;
        for (const char* d = digits; *d; ++d)
            numeric = numeric && (*d >= '0' && *d <= '9');
        if (numeric)
            out.emplace_back(p);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return std::stoi(a.substr(3)) < std::stoi(b.substr(3));
    });
#elif defined(__APPLE__)
    // Mirrors SerialIO.cpp's own macOS scan: USB serial devices only.
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/dev", ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("tty.usb", 0) == 0)
            out.push_back("/dev/" + name);
    }
    std::sort(out.begin(), out.end());
#else
    // Mirrors SerialIO.cpp's Linux scan: a tty whose device link goes through usb,
    // which skips the virtual and bluetooth ttys that would otherwise flood the list.
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/sys/class/tty", ec)) {
        const std::string name = e.path().filename().string();
        std::error_code linkEc;
        // canonical() resolves the class symlink into /sys/devices/...; the "usb"
        // component only appears on the real device path, never on a virtual tty.
        const auto real = std::filesystem::canonical(e.path(), linkEc);
        if (linkEc)
            continue;
        const std::string t = real.string();
        if (t.find("/usb") == std::string::npos)
            continue;
        out.push_back("/dev/" + name);
    }
    std::sort(out.begin(), out.end());
#endif
    return out;
}

bool AmigaEmulator::GetCustomRegisters(core::Custom& outCustom) const
{
    if (!m_coreLoaded || !m_gameLoaded)
        return false;

    uint16_t buffer[256];
    std::memset(buffer, 0, sizeof(buffer));

    // Read from custom_storage to avoid side effects of reading write-only registers
    {
        struct CustomStore
        {
            uint16_t value;
            uint32_t pc;
        };
        auto* store = reinterpret_cast<CustomStore*>(m_customStoragePtr);
        for (int i = 0; i < 256; i++) {
            buffer[i] = store[i].value;
        }
    }

    {
        auto* bank = reinterpret_cast<UAEAddrBank*>(m_customBankPtr);
        if (bank->wget) {
            typedef uint32_t (*uae_wget_t)(uint32_t);
            uae_wget_t uae_wget = reinterpret_cast<uae_wget_t>(bank->wget);

            // Only read registers that are explicitly safe (no side effects on read)
            // and where the read value (hardware state) is more useful than the written value.
            auto safe_read = [&](uint32_t offset) {
                buffer[offset / 2] = static_cast<uint16_t>(uae_wget(0xDFF000 + offset));
            };

            safe_read(0x002); // DMACONR
            safe_read(0x004); // VPOSR
            safe_read(0x006); // VHPOSR
            safe_read(0x00A); // JOY0DAT
            safe_read(0x00C); // JOY1DAT
            safe_read(0x010); // ADKCONR
            safe_read(0x012); // POT0DAT
            safe_read(0x014); // POT1DAT
            safe_read(0x016); // POTGOR
            safe_read(0x01C); // INTENAR
            safe_read(0x01E); // INTREQR
            safe_read(0x07C); // DENISEID
            safe_read(0x1DA); // HHPOSR
        }
    }

    std::memcpy(&outCustom, buffer, sizeof(core::Custom));
    return true;
}

bool AmigaEmulator::ReadCustomRegisterWord(uint32_t offset, uint16_t& out) const
{
    if (offset / 2 >= sizeof(core::Custom) / 2)
        return false;
    core::Custom regs{};
    if (!GetCustomRegisters(regs))
        return false;
    out = reinterpret_cast<const volatile uint16_t*>(&regs)[offset / 2];
    return true;
}

bool AmigaEmulator::ExecuteDebugCommand(const std::string& cmd, std::string& output) const
{
    ActiveInstanceScope _g(const_cast<AmigaEmulator*>(this));

    output.clear();
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_debug_command || cmd.empty())
        return false;

    constexpr int kMaxOut = 256 * 1024;
    std::vector<char> out(static_cast<size_t>(kMaxOut), '\0');
    int written = m_adm_debug_command(cmd.c_str(), out.data(), kMaxOut);
    if (written < 0)
        return false;

    out[static_cast<size_t>(kMaxOut - 1)] = '\0';
    const size_t len = std::min(static_cast<size_t>(written), static_cast<size_t>(kMaxOut - 1));
    output.assign(out.data(), len);
    if (written >= kMaxOut - 1) {
        output.append("\n[warn] Debug output truncated by host buffer.\n");
    }
    return true;
}

bool AmigaEmulator::IsDebugBreakPending() const
{
    // Host-side KPrintF-text break (see FlushSerialLogLine). Drained first, but the core's own
    // read-and-clear flag is still consumed below so a breakpoint that fired in the same frame
    // doesn't stop us a second time on resume.
    // An Exocet row breakpoint (RequestHostBreak) is reported the same way.
    const bool kprintfBreak = m_kprintfBreakPending || m_hostBreakPending;
    m_kprintfBreakPending = false;
    m_hostBreakPending = false;

    if (!m_coreLoaded || !m_gameLoaded || !m_adm_get_debug_break_pending)
        return kprintfBreak;
    if (m_adm_get_debug_break_pending() == 0) {
        // Nothing from the core: a KPrintF match is the whole reason for this break, so clear
        // any stale watchpoint details that would otherwise be reported alongside it.
        if (kprintfBreak)
            m_hasLatchedMemwatch = false;
        return kprintfBreak;
    }

    // A break just fired this frame. Latch the watchpoint hit (if any) that caused
    // it so the console banner and the Breakpoints window can both read it while
    // paused. Refresh every break so a later PC break clears any stale watch hit.
    m_hasLatchedMemwatch = false;
    if (m_adm_get_last_memwatch2) {
        MemwatchHit h{};
        if (m_adm_get_last_memwatch2(&h.pc, &h.addr, &h.val, &h.rwi, &h.size, &h.accessMask,
                                     &h.reg) != 0) {
            m_latchedMemwatch = h;
            m_hasLatchedMemwatch = true;
        }
    } else if (m_adm_get_last_memwatch) {
        MemwatchHit h{};
        if (m_adm_get_last_memwatch(&h.pc, &h.addr, &h.val, &h.rwi, &h.size) != 0) {
            m_latchedMemwatch = h;
            m_hasLatchedMemwatch = true;
        }
    }
    return true;
}

bool AmigaEmulator::ConsumeKprintfHit(core::KprintfHit& out) const
{
    if (!m_hasKprintfHit)
        return false;
    out = m_kprintfHit;
    m_hasKprintfHit = false;
    return true;
}

bool AmigaEmulator::GetLastKprintfHit(core::KprintfHit& out) const
{
    if (!m_hasLatchedKprintf)
        return false;
    out = m_kprintfHit;
    return true;
}

void AmigaEmulator::ArmDebugTrace() const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_arm_debug)
        return;
    m_adm_arm_debug();
}

bool AmigaEmulator::ArmOneShotBreakpoint(uint32_t address, uint32_t instructionBudget) const
{
    if (!m_coreLoaded || !m_gameLoaded || !HasOneShotBreakpoint() || instructionBudget == 0)
        return false;
    return m_adm_arm_oneshot_breakpoint(address, instructionBudget) != 0;
}

void AmigaEmulator::CancelOneShotBreakpoint() const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_cancel_oneshot_breakpoint)
        return;
    m_adm_cancel_oneshot_breakpoint();
}

AmigaEmulator::OneShotBreakpointStatus
AmigaEmulator::GetOneShotBreakpointStatus(uint32_t* stopPc, uint32_t* executed) const
{
    if (stopPc)
        *stopPc = 0;
    if (executed)
        *executed = 0;
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_get_oneshot_breakpoint_status)
        return OneShotBreakpointStatus::Unavailable;
    return static_cast<OneShotBreakpointStatus>(
        m_adm_get_oneshot_breakpoint_status(stopPc, executed));
}

void AmigaEmulator::ArmBreakOnBootblock(bool on) const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_arm_fb)
        return;
    m_adm_arm_fb(on ? 1 : 0);
}

bool AmigaEmulator::IsBootblockArmed() const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_is_fb_armed)
        return false;
    return m_adm_is_fb_armed() != 0;
}

uint32_t AmigaEmulator::BootblockEntry() const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_get_fb_entry)
        return 0;
    return m_adm_get_fb_entry();
}

bool AmigaEmulator::ConsumeBootblockHit() const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_get_fb_hit)
        return false;
    return m_adm_get_fb_hit() != 0;
}

void AmigaEmulator::ArmExceptionBreak(uint64_t mask) const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_arm_exception)
        return;
    m_adm_arm_exception(static_cast<uint32_t>(mask & 0xFFFFFFFFu),
                        static_cast<uint32_t>(mask >> 32));
}

bool AmigaEmulator::ConsumeExceptionHit(int& outVector) const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_get_exception_hit)
        return false;
    int nr = -1;
    if (m_adm_get_exception_hit(&nr) != 0) {
        outVector = nr;
        return true;
    }
    return false;
}

void AmigaEmulator::ArmBeamBreak(int vpos, int hpos) const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_arm_beam_break)
        return;
    m_adm_arm_beam_break(vpos, hpos);
}

int AmigaEmulator::GetDiskDmaPayload(uint32_t off, uint32_t len, uint8_t* out) const
{
    if (!m_coreLoaded || !m_adm_get_disk_dma_payload || !out || len == 0)
        return 0;
    return m_adm_get_disk_dma_payload(off, len, out);
}

void AmigaEmulator::ArmDiskBreak(int access, int cyl, int side, int sector, uint32_t addrLo,
                                 uint32_t addrHi) const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_arm_disk_break)
        return;
    m_adm_arm_disk_break(access, cyl, side, sector, addrLo, addrHi);
}

bool AmigaEmulator::IsDiskBreakArmed() const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_is_disk_break_armed)
        return false;
    return m_adm_is_disk_break_armed() != 0;
}

bool AmigaEmulator::ConsumeDiskBreakHit(DiskDmaEvent& outEvent) const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_get_disk_break_hit)
        return false;
    return m_adm_get_disk_break_hit(&outEvent) != 0;
}

bool AmigaEmulator::ConsumeBeamHit() const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_get_beam_hit)
        return false;
    return m_adm_get_beam_hit() != 0;
}

bool AmigaEmulator::GetLastMemwatchHit(MemwatchHit& out) const
{
    if (!m_hasLatchedMemwatch)
        return false;
    out = m_latchedMemwatch;
    return true;
}

bool AmigaEmulator::WriteCustomRegister(uint32_t addr, uint16_t value) const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_customBankPtr)
        return false;
    if (addr < 0xDFF000 || addr > 0xDFF1FE || (addr & 1u) != 0u)
        return false;

    auto* bank = reinterpret_cast<UAEAddrBank*>(m_customBankPtr);
    if (!bank->wput)
        return false;

    // Intentional live write: calls UAE's chip register handler directly between
    // retro_run() calls. This is the expected behaviour for debug register poking
    // and works correctly in the single-threaded execution model.
    typedef void (*uae_wput_t)(uint32_t, uint32_t);
    uae_wput_t uae_wput = reinterpret_cast<uae_wput_t>(bank->wput);
    uae_wput(addr, value);
    return true;
}

bool AmigaEmulator::WriteCIARegister(uint32_t addr, uint8_t value) const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_ciaBankPtr)
        return false;
    // CIA space, matching ArMemoryAccess::kCiaBase/kCiaEnd. Literals rather than that
    // header, so this file keeps no dependency on the console's memory facade.
    if (addr < 0xBFD000 || addr >= 0xBFF000)
        return false;

    auto* bank = reinterpret_cast<UAEAddrBank*>(m_ciaBankPtr);
    if (!bank->bput)
        return false;

    // Same contract as WriteCustomRegister: a live write straight into UAE's CIA handler
    // between retro_run() calls. cia_bput decodes the chip select from the address itself,
    // so no chip/register split is needed here.
    typedef void (*uae_bput_t)(uint32_t, uint32_t);
    uae_bput_t uae_bput = reinterpret_cast<uae_bput_t>(bank->bput);
    uae_bput(addr, value);
    return true;
}

bool AmigaEmulator::SetProtections(const std::vector<ProtectEntry>& entries) const
{
    if (!m_coreLoaded || !m_adm_protect_set)
        return false;
    // The core copies out of this buffer, so a temporary is fine; passing an empty list
    // through the same call disarms the table (adm_protect_set treats count<=0 as clear).
    m_adm_protect_set(entries.empty() ? nullptr : entries.data(), static_cast<int>(entries.size()));
    return true;
}

bool AmigaEmulator::ClearProtections() const
{
    if (!m_coreLoaded || !m_adm_protect_clear)
        return false;
    m_adm_protect_clear();
    return true;
}

bool AmigaEmulator::ReadCIARegisterByte(bool chipB, int reg, uint8_t& out) const
{
    core::CIAState cia{};
    if (!GetCIARegisters(cia))
        return false;
    const core::CIAChip& c = chipB ? cia.b : cia.a;
    switch (reg) {
        case 0x0:
            out = c.pra;
            return true;
        case 0x1:
            out = c.prb;
            return true;
        case 0x2:
            out = c.dra;
            return true;
        case 0x3:
            out = c.drb;
            return true;
        case 0x4:
            out = c.talo;
            return true;
        case 0x5:
            out = c.tahi;
            return true;
        case 0x6:
            out = c.tblo;
            return true;
        case 0x7:
            out = c.tbhi;
            return true;
        case 0x8:
            out = c.todlo;
            return true;
        case 0x9:
            out = c.todmid;
            return true;
        case 0xA:
            out = c.todhi;
            return true;
        case 0xC:
            out = c.sdr;
            return true;
        case 0xD:
            out = c.icr;
            return true;
        case 0xE:
            out = c.cra;
            return true;
        case 0xF:
            out = c.crb;
            return true;
        default:
            return false; // 0xB is unused on the 8520
    }
}

bool AmigaEmulator::GetCIARegisters(core::CIAState& outCIA) const
{
    if (!m_coreLoaded || !m_gameLoaded)
        return false;

    adm_cia_state_t raw{};
    m_adm_get_cia_state(&raw);

    outCIA.a.pra = raw.a.pra;
    outCIA.a.prb = raw.a.prb;
    outCIA.a.dra = raw.a.dra;
    outCIA.a.drb = raw.a.drb;
    outCIA.a.talo = raw.a.talo;
    outCIA.a.tahi = raw.a.tahi;
    outCIA.a.tblo = raw.a.tblo;
    outCIA.a.tbhi = raw.a.tbhi;
    outCIA.a.todlo = raw.a.todlo;
    outCIA.a.todmid = raw.a.todmid;
    outCIA.a.todhi = raw.a.todhi;
    outCIA.a.sdr = raw.a.sdr;
    outCIA.a.icr = raw.a.icr;
    outCIA.a.imask = raw.a.imask;
    outCIA.a.cra = raw.a.cra;
    outCIA.a.crb = raw.a.crb;

    outCIA.b.pra = raw.b.pra;
    outCIA.b.prb = raw.b.prb;
    outCIA.b.dra = raw.b.dra;
    outCIA.b.drb = raw.b.drb;
    outCIA.b.talo = raw.b.talo;
    outCIA.b.tahi = raw.b.tahi;
    outCIA.b.tblo = raw.b.tblo;
    outCIA.b.tbhi = raw.b.tbhi;
    outCIA.b.todlo = raw.b.todlo;
    outCIA.b.todmid = raw.b.todmid;
    outCIA.b.todhi = raw.b.todhi;
    outCIA.b.sdr = raw.b.sdr;
    outCIA.b.icr = raw.b.icr;
    outCIA.b.imask = raw.b.imask;
    outCIA.b.cra = raw.b.cra;
    outCIA.b.crb = raw.b.crb;

    return true;
}

// --- Bartman debug resources ---------------------------------------------------------------
// The host-side DebugResource mirrors the core's adm_debug_resource_t byte-for-byte, so the
// core's getter writes straight into our array.
static_assert(sizeof(AmigaEmulator::DebugResource) == 56,
              "DebugResource must match adm_debug_resource_t layout");

int AmigaEmulator::GetDebugResources(DebugResource* out, int max) const
{
    if (!m_adm_get_debug_resources || !out || max <= 0)
        return 0;
    return m_adm_get_debug_resources(out, max);
}

uint32_t AmigaEmulator::GetDebugResourceGeneration() const
{
    return m_adm_get_debug_resource_generation ? m_adm_get_debug_resource_generation() : 0;
}

void AmigaEmulator::ClearDebugResources() const
{
    if (m_adm_clear_debug_resources)
        m_adm_clear_debug_resources();
}

// --- Crass engine PROFILE block ------------------------------------------------------------
// A Crass-engine production built with `#define PROFILE` registers its ProfileBlock as a type-3
// Bartman resource. Reading it is a plain memory read of ~20 + 30 + 16*38 bytes; nothing is
// armed in the core and the emulated machine is not perturbed, which is the whole reason the
// engine publishes the numbers this way instead of being watched.
const core::CrassProfileStats& AmigaEmulator::GetCrassProfileStats() const
{
    if (m_crassProfileRun == m_totalRunCount)
        return m_crassProfile;
    m_crassProfileRun = m_totalRunCount;
    m_crassProfile = core::CrassProfileStats{};

    if (!m_coreLoaded || !m_gameLoaded || !m_adm_get_debug_resources)
        return m_crassProfile;

    // Re-scan the registry only when it has actually changed. The generation also moves on the
    // clear a program load performs, so a stale address from the previous program is dropped
    // rather than followed into whatever now occupies that memory.
    const uint32_t gen = GetDebugResourceGeneration();
    if (!m_crassProfileResourceGenValid || gen != m_crassProfileResourceGen) {
        m_crassProfileResourceGen = gen;
        m_crassProfileResourceGenValid = true;
        m_crassProfileAddr = 0;
        std::vector<DebugResource> resources(kMaxDebugResources);
        const int count = GetDebugResources(resources.data(), kMaxDebugResources);
        for (int i = 0; i < count; ++i) {
            if (resources[i].type == static_cast<uint16_t>(DebugResourceType::Profile)) {
                m_crassProfileAddr = resources[i].address;
                break;
            }
        }
    }
    if (m_crassProfileAddr == 0)
        return m_crassProfile;

    // Same guest-memory path read_memory uses: an ArMemoryAccess over the host-mapped banks.
    // Only writable banks are offered as valid targets, which is exactly chip/slow/fast RAM -
    // Kickstart is the one read-only region, and a profiler block can never live in ROM.
    core::ArMemoryAccess mem;
    std::vector<core::ArMemoryAccess::Region> memRegions;
    std::vector<core::CrassProfileRam> ram;
    for (const auto& r : GetMemoryRegions()) {
        if (!r.hostPtr || !r.writable)
            continue;
        memRegions.push_back({r.hostPtr, r.amigaAddr, r.size, r.name, r.writable});
        ram.push_back({r.amigaAddr, r.size});
    }
    if (ram.empty())
        return m_crassProfile;
    mem.SetRegions(std::move(memRegions));

    auto read = [&mem](uint32_t addr, uint8_t* out, uint32_t len) {
        std::vector<bool> valid;
        mem.Read(addr, out, len, &valid);
        for (uint32_t i = 0; i < len; ++i) {
            if (i >= valid.size() || !valid[i])
                return false;
        }
        return true;
    };
    core::ReadCrassProfile(read, ram, m_crassProfileAddr, m_crassProfile);
    return m_crassProfile;
}

// --- Crass engine Exocet block ---------------------------------------------------------------
// A program may register its ExoBlock as a type-4 Bartman resource (core/ExocetBlock.h); a disk
// that ships registers nothing - no call that only means something under an emulator - and its
// block is found by its magic. As with the PROFILE block, reading it is plain memory reads:
// nothing in the core is armed.
const core::ExocetBlockState& AmigaEmulator::GetExocetBlock() const
{
    if (m_exocetBlockRun == m_totalRunCount)
        return m_exocetBlock;
    m_exocetBlockRun = m_totalRunCount;
    m_exocetBlock = core::ExocetBlockState{};

    if (!m_coreLoaded || !m_gameLoaded)
        return m_exocetBlock;

    // The registry moves on every clear a program load performs, so an address from the
    // previous program is dropped rather than followed into whatever holds that memory now.
    if (m_adm_get_debug_resources) {
        const uint32_t gen = GetDebugResourceGeneration();
        if (!m_exocetResourceGenValid || gen != m_exocetResourceGen) {
            m_exocetResourceGen = gen;
            m_exocetResourceGenValid = true;
            m_exocetBlockAddr = 0;
            std::vector<DebugResource> resources(kMaxDebugResources);
            const int count = GetDebugResources(resources.data(), kMaxDebugResources);
            for (int i = 0; i < count; ++i) {
                if (resources[i].type == static_cast<uint16_t>(DebugResourceType::Exocet)) {
                    m_exocetBlockAddr = resources[i].address;
                    break;
                }
            }
        }
    }

    // The PROFILE reader's memory path: RAM banks only, so ROM can never pass for a block.
    core::ArMemoryAccess mem;
    std::vector<core::ArMemoryAccess::Region> memRegions;
    std::vector<core::ExocetRam> ram;
    std::vector<MemoryRegion> banks;
    for (const auto& r : GetMemoryRegions()) {
        if (!r.hostPtr || !r.writable)
            continue;
        memRegions.push_back({r.hostPtr, r.amigaAddr, r.size, r.name, r.writable});
        ram.push_back({r.amigaAddr, r.size});
        banks.push_back(r);
    }
    if (ram.empty())
        return m_exocetBlock;
    mem.SetRegions(std::move(memRegions));

    auto read = [&mem](uint32_t addr, uint8_t* out, uint32_t len) {
        std::vector<bool> valid;
        mem.Read(addr, out, len, &valid);
        for (uint32_t i = 0; i < len; ++i) {
            if (i >= valid.size() || !valid[i])
                return false;
        }
        return true;
    };
    if (m_exocetBlockAddr != 0) {
        core::ReadExocetBlock(read, ram, m_exocetBlockAddr, m_exocetBlock);
        return m_exocetBlock;
    }

    // Unregistered: the address last found, while it still reads as a block over a real
    // exocet.bin; else a scan of RAM for one (core::FindExocetBlock), at most every 50 frames
    // while there is none.
    if (m_exocetScanAddr != 0) {
        core::ExocetBlockState s;
        if (core::ReadExocetBlock(read, ram, m_exocetScanAddr, s) && s.arena.valid) {
            m_exocetBlock = std::move(s);
            return m_exocetBlock;
        }
        m_exocetScanAddr = 0;
    }
    if (m_exocetScanRunValid && m_totalRunCount - m_exocetScanRun < 50)
        return m_exocetBlock;
    m_exocetScanRun = m_totalRunCount;
    m_exocetScanRunValid = true;
    std::vector<core::ExocetHostBank> hosts;
    for (const auto& r : banks)
        hosts.push_back({r.amigaAddr, r.size, r.hostPtr});
    m_exocetScanAddr = core::FindExocetBlock(read, ram, hosts, m_exocetBlock);
    return m_exocetBlock;
}

// The disk's memory map (core/CrassMemMap.h): the table the block points at, and exec's own
// account of memory - its MemHeaders and their free chunks - which the table is laid over.
const core::ExocetMemoryState& AmigaEmulator::GetExocetMemory() const
{
    if (m_exocetMemoryRun == m_totalRunCount)
        return m_exocetMemory;
    m_exocetMemoryRun = m_totalRunCount;
    m_exocetMemory = core::ExocetMemoryState{};

    const core::ExocetBlockState& block = GetExocetBlock();
    if (!block.present || block.memmapAddress == 0)
        return m_exocetMemory;

    const auto regions = GetMemoryRegions();
    std::vector<core::ExocetRam> ram;
    for (const auto& r : regions)
        if (r.hostPtr && r.writable)
            ram.push_back({r.amigaAddr, r.size});
    const auto find = [&regions](uint32_t addr) -> const uint8_t* {
        for (const auto& r : regions)
            if (r.hostPtr && addr >= r.amigaAddr && addr - r.amigaAddr < r.size)
                return r.hostPtr + (addr - r.amigaAddr);
        return nullptr;
    };
    const auto read = [&find](uint32_t addr, uint8_t* out, uint32_t len) {
        for (uint32_t i = 0; i < len; ++i) {
            const uint8_t* p = find(addr + i);
            if (!p)
                return false;
            out[i] = *p;
        }
        return true;
    };

    // A table caught mid-change is shown as it stood, marked: the machine may be stopped there
    core::CrassMemMapTable table;
    core::ReadCrassMemMap(read, ram, block.memmapAddress, table);
    if (!table.present)
        return m_exocetMemory;

    core::exec::Readers r;
    r.readByte = [&find](uint32_t addr, bool& ok) -> uint8_t {
        const uint8_t* p = find(addr);
        ok = p != nullptr;
        return ok ? *p : uint8_t{0};
    };
    r.readLong = [&find](uint32_t addr, bool& ok) -> uint32_t {
        uint32_t v = 0;
        ok = true;
        for (uint32_t i = 0; i < 4; ++i) {
            const uint8_t* p = find(addr + i);
            if (!p) {
                ok = false;
                return 0;
            }
            v = (v << 8) | *p;
        }
        return v;
    };
    r.isMapped = [&find](uint32_t addr) { return find(addr) != nullptr; };

    std::vector<core::CrassMemHeader> headers;
    if (const auto execBase = core::exec::ExecBase(r)) {
        for (const auto& mh : core::exec::ListMemHeaders(r, *execBase)) {
            core::CrassMemHeader h;
            h.name = mh.name;
            h.attributes = mh.attributes;
            h.lower = mh.lower;
            h.upper = mh.upper;
            h.free = mh.free;
            for (const auto& c : core::exec::ListFreeChunks(r, mh))
                h.chunks.push_back({c.addr, c.bytes});
            headers.push_back(std::move(h));
        }
    }
    m_exocetMemory.present = true;
    m_exocetMemory.owners = block.owners;
    m_exocetMemory.table = std::move(table);
    m_exocetMemory.regions = core::LayOutCrassMemory(headers, m_exocetMemory.table);
    return m_exocetMemory;
}

void AmigaEmulator::SetBartmanDebugSupport(bool enabled)
{
    if (m_bartmanDebugEnabled != enabled)
        ResetDemoPerformance();
    m_bartmanDebugEnabled = enabled;
    if (m_adm_set_force_boot_rom)
        m_adm_set_force_boot_rom(enabled ? 1 : 0);
}

bool AmigaEmulator::GetDebugGateInfo(DebugGateInfo& out) const
{
    // The export reads the gate's opcode out of Amiga memory, which only exists once a machine has
    // been built: a core that is loaded but never ran (Preferences or the setup wizard load one to
    // identify ROMs) crashed in memory_get_word with the Bartman window open.
    if (!m_adm_get_debug_gate_info || !m_coreLoaded || !m_gameLoaded)
        return false;
    m_adm_get_debug_gate_info(&out.gateAddr, &out.opcode, &out.bootRomType, &out.force);
    return true;
}

std::vector<std::string> AmigaEmulator::GetMissingBartmanSymbols() const
{
    std::vector<std::string> missing;
    if (!m_adm_get_debug_resources)
        missing.emplace_back("adm_get_debug_resources");
    if (!m_adm_get_debug_resource_generation)
        missing.emplace_back("adm_get_debug_resource_generation");
    if (!m_adm_clear_debug_resources)
        missing.emplace_back("adm_clear_debug_resources");
    if (!m_adm_set_force_boot_rom)
        missing.emplace_back("adm_set_force_boot_rom");
    if (!m_adm_get_debug_gate_info)
        missing.emplace_back("adm_get_debug_gate_info");
    return missing;
}

void AmigaEmulator::SetSystemDirectory(const std::string& path)
{
    m_systemDir = path;

    // PuAE keys content-specific configuration and redirected floppy writes by the
    // content basename. ADM intentionally uses stable temp names such as
    // adm_emulator_0_df0.adf, so using the shared system directory as the save
    // directory causes a previous process's *_save.adf.gz to be applied to an
    // unrelated disk on the next launch. Keep PuAE's save/cache files in a unique
    // directory for this emulator instance instead. The directory remains stable
    // across power cycles and is removed by the destructor.
    if (m_ownsSaveDir && !m_saveDir.empty())
        return;

    std::error_code ec;
    const auto tempDirectory = std::filesystem::temp_directory_path(ec);
    if (!ec) {
        const auto nonce = std::chrono::system_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 4; ++attempt) {
            const auto candidate =
                tempDirectory / ("adm_puae_session_" + std::to_string(nonce) + "_" +
                                 std::to_string(m_instanceId) + "_" + std::to_string(attempt));
            ec.clear();
            if (std::filesystem::create_directory(candidate, ec)) {
                m_saveDir = candidate.string();
                m_ownsSaveDir = true;
                return;
            }
            if (ec)
                break;
        }
    }

    // Falling back keeps the emulator usable on systems whose temp directory is
    // unavailable, although PuAE write redirects will once again be persistent.
    m_saveDir = path;
    core::Logger::Warn("Emulator: could not create an isolated PuAE save directory; using '{}'",
                       path);
}

void AmigaEmulator::SetModel(AmigaModel model)
{
    m_currentModel = model;
    m_variablesUpdated = true;
}

bool AmigaEmulator::SelectModelPreset(AmigaModel model)
{
    struct PresetVariables
    {
        const char* kickstart;
        const char* chipMemory;
        const char* slowMemory;
        const char* fastMemory;
        // uaegfx board VRAM in MB, "0" for no graphics card. Written for every preset, not just
        // the one that wants a board: the option is sticky like any other, so a model switch has
        // to be able to take the board away again.
        const char* rtgMemory = "0";
    };

    // These values use PuAE's current core-option encoding. Keeping them explicit makes the menu
    // descriptions authoritative even where PuAE's own defaults differ (notably A600/A2000 ROMs).
    PresetVariables preset{};
    switch (model) {
        case AmigaModel::A500OG:
            preset = {"kick34005.A500", "1", "0", "0"};
            break;
        case AmigaModel::A500:
            preset = {"kick34005.A500", "1", "2", "0"};
            break;
        case AmigaModel::A500Plus:
            preset = {"kick37175.A500", "2", "0", "0"};
            break;
        case AmigaModel::A600:
            preset = {"kick37350.A600", "4", "0", "8"};
            break;
        case AmigaModel::A1200OG:
            preset = {"kick40068.A1200", "4", "0", "0"};
            break;
        case AmigaModel::A1200:
            preset = {"kick40068.A1200", "4", "0", "8"};
            break;
        case AmigaModel::A2000OG:
            preset = {"kick34005.A500", "1", "2", "0"};
            break;
        case AmigaModel::A2000:
            preset = {"kick37175.A500", "2", "0", "0"};
            break;
        case AmigaModel::A4030:
        case AmigaModel::A4040:
            preset = {"kick40068.A4000", "4", "0", "8"};
            break;
        // The A4000 is the natural home for RTG: an 030/040 with a 32-bit address space, which
        // is what a Zorro III board needs. 8 MB is well beyond any real card of the era (a
        // Picasso IV had 4) and leaves Picasso96 plenty for offscreen bitmaps.
        case AmigaModel::A4040RTG:
            preset = {"kick40068.A4000", "4", "0", "8", "8"};
            break;
        case AmigaModel::CDTV:
            preset = {"kick34005.A500", "2", "0", "0"};
            break;
        // The core resolves kick40060.CD32 under its Amiga Forever and TOSEC names too, and adds
        // the extended ROM and the NVRAM file itself (retro_config_kickstart).
        case AmigaModel::CD32:
            preset = {"kick40060.CD32", "4", "0", "0"};
            break;
        case AmigaModel::CD32FR:
            preset = {"kick40060.CD32", "4", "0", "8"};
            break;
    }

    m_currentModel = model;

    // A legacy saved puae_model value would otherwise take priority in EnvironmentCallback.
    m_userVariables.erase("puae_model");
    m_userVariables.erase("puae_chip_memory");
    m_userVariables.erase("puae_slow_memory");
    m_userVariables.erase("puae_fast_memory");

    m_userVariables["puae_cpu_model"] = "auto";
    m_userVariables["puae_fpu_model"] = "auto";
    // The chipset revisions sit below the model: a new machine starts from its own chips.
    for (const auto& setting : core::kMachineSettingKeys)
        m_userVariables[setting.key] = "auto";
    m_userVariables["puae_kickstart"] = preset.kickstart;
    m_userVariables["puae_chipmem_size"] = preset.chipMemory;
    m_userVariables["puae_bogomem_size"] = preset.slowMemory;
    m_userVariables["puae_fastmem_size"] = preset.fastMemory;
    // "auto", not "0". The core answers ANY non-auto z3mem_size with cpu_24bit_addressing=false
    // (libretro-core.c, GET_VAR("z3mem_size")) - so asking for no Z3 RAM also took the address
    // space to 32 bits on every model. A 68EC020 has 24 address lines, so an A1200 or CD32 ran
    // software that leaves garbage in an address's top byte when the hardware would not.
    // "auto" gives the same 0 MB of Z3 RAM from preset_opt and leaves the addressing to the
    // model preset, which already has it right: 24-bit everywhere, 32-bit on the A4000s.
    m_userVariables["puae_z3mem_size"] = "auto";
    // Zorro III always: a Zorro II board caps at 8 MB and has to share the $200000-$A00000
    // window with Fast RAM, which every preset that has a board also fills. Presets without a
    // board still write the type, so it cannot be left over from a previous machine.
    m_userVariables["puae_gfxcard_size"] = preset.rtgMemory;
    m_userVariables["puae_gfxcard_type"] = "ZorroIII";
    m_variablesUpdated = true;

    // PuAE snapshots core options during retro_init(). An unload/load_game cycle inside the same
    // initialised core therefore rebuilds the old machine once before noticing these new values.
    // Reinitialising here makes a live menu selection equivalent to Stop followed by Power On.
    return !m_coreLoaded || ReinitCore();
}

void AmigaEmulator::SetDriveEnabled(int drive, bool enabled)
{
    if (drive >= 0 && drive <= 3)
        m_driveEnabled[drive] = enabled;
}

float AmigaEmulator::VolumeGain() const
{
    const float v = static_cast<float>(m_volume.load(std::memory_order_relaxed)) / 100.0f;
    return v * v;
}

void AmigaEmulator::SetVolume(int percent)
{
    m_volume.store(std::clamp(percent, 0, 100), std::memory_order_relaxed);
    // the pull device: miniaudio scales after the callback; the push device: PushAudio does
    if (m_audioDevice)
        ma_device_set_master_volume(m_audioDevice, VolumeGain());
}

void AmigaEmulator::SetFloppySound(bool enabled)
{
    SetFloppySoundVolume(enabled ? m_floppySoundVolume : 0);
}

void AmigaEmulator::SetFloppySoundVolume(int percent)
{
    percent = std::clamp(percent, 0, 100);
    // Silence is a toggle state, not a preference: only an audible level is worth remembering, so
    // the toolbar checkbox has a volume to come back to.
    if (percent > 0)
        m_floppySoundVolume = percent;

    // A user variable rather than a core one. PreferencesView pushes its whole page into the user
    // map and that map outranks m_coreVariables in EnvironmentCallback, so a core-side write here
    // would be silently outvoted by the last Apply.
    m_userVariables["puae_floppy_sound"] = std::to_string(100 - percent);
    // The only sample set guaranteed to exist is the one compiled into the core. Any other value
    // sends PuAE hunting for drive_click_<name>.wav under <system>/uae_data, and a miss doesn't
    // fall back - it clears dfxclick and drive sound stops entirely. emplace, so this only fills a
    // gap: a deliberate External choice from Preferences stays where the user put it.
    m_userVariables.emplace("puae_floppy_sound_type", "internal");
    m_variablesUpdated = true;
}

int AmigaEmulator::GetFloppySoundVolume() const
{
    // The value the core is actually given, by the same precedence GET_VARIABLE applies.
    const std::string attenuation = ResolveCoreVariable("puae_floppy_sound");
    if (attenuation.empty())
        return m_floppySoundVolume;
    return std::clamp(100 - std::atoi(attenuation.c_str()), 0, 100);
}

void AmigaEmulator::SetVideoResolution(VideoResolution resolution)
{
    m_videoResolution = resolution;

    const char* value = "lores";
    if (resolution == VideoResolution::Hires)
        value = "hires";
    else if (resolution == VideoResolution::SuperHires)
        value = "superhires";

    // User variables, not core ones, for the reason spelled out in SetFloppySoundVolume: the
    // Preferences dialog pushes its whole page into the user map, so a core-side write here was
    // outvoted by the next Apply and the Display window's resolution combo silently stopped
    // working.
    m_userVariables["puae_video_resolution"] = value;
    // Vertical doubling pairs with any horizontal resolution above lores, so square-ish pixels
    // and progressive interlace handling come along with it.
    m_userVariables["puae_video_vresolution"] =
        (resolution == VideoResolution::Lores) ? "single" : "double";
    m_variablesUpdated = true;
}

AmigaEmulator::VideoResolution AmigaEmulator::GetVideoResolution() const
{
    // Resolved rather than remembered, so a resolution chosen in Emulator Preferences is what the
    // Display window's combo reports.
    const std::string value = ResolveCoreVariable("puae_video_resolution");
    if (value == "lores")
        return VideoResolution::Lores;
    if (value == "hires")
        return VideoResolution::Hires;
    if (value == "superhires")
        return VideoResolution::SuperHires;
    // "auto" and the auto- variants leave the choice to the core; report the last explicit pick.
    return m_videoResolution;
}

std::string AmigaEmulator::ResolveCoreVariable(const std::string& key) const
{
    // 1. A user override beats everything.
    if (auto it = m_userVariables.find(key); it != m_userVariables.end())
        return it->second;

    // 2. Values derived from the selected machine or the attached controller. Guarded by the
    //    same predicate SET_VARIABLES uses, so the two cannot disagree about the membership.
    if (IsMachineDerivedVariable(key)) {
        const auto& info = GetAmigaModelInfo(m_currentModel);
        if (key == "puae_model")
            return info.coreId;
        // Legacy option names. The current core declares none of these three - it reads
        // puae_chipmem_size and friends, which SelectModelPreset() writes as user variables -
        // but an older core still understands them and answering costs nothing.
        if (key == "puae_chip_memory")
            return info.chipMemory;
        if (key == "puae_slow_memory")
            return info.slowMemory;
        if (key == "puae_fast_memory")
            return info.fastMemory;
        // Joystick port type - tells PuAE what controller is on port 0.
        if (key == "puae_joyport" || key == "puae_joyport_type")
            return m_controllerType == ControllerType::CD32Controller ? "cd32pad" : "joystick";
        return "disabled"; // puae_cd32pad_options, puae_retropad_options
    }

    // 3. Peekaboo's own default for the options the Preferences dialog manages. One table, so
    //    the dialog cannot show a default the core was never given.
    if (const auto def = core::EmulatorOptionDefault(key); !def.empty())
        return std::string(def);

    // 4. Whatever the core declared for itself in SET_VARIABLES.
    if (auto it = m_coreVariables.find(key); it != m_coreVariables.end())
        return it->second;

    return {};
}

void AmigaEmulator::SetUserVariable(const std::string& key, const std::string& value)
{
    m_userVariables[key] = value;
    m_variablesUpdated = true;
}

void AmigaEmulator::ClearUserVariable(const std::string& key)
{
    m_userVariables.erase(key);
    m_variablesUpdated = true;
}

void AmigaEmulator::SetUserVariables(const std::unordered_map<std::string, std::string>& vars)
{
    m_userVariables = vars;
    m_variablesUpdated = true;
}

void AmigaEmulator::ClearAllUserVariables()
{
    m_userVariables.clear();
    m_variablesUpdated = true;
}

void AmigaEmulator::LedStateCallback(int led, int state)
{
    if (!s_instance)
        return;
    // LED 0 = power (not functional — PuAE's gui_led() is a stub in libretro builds)
    // LED 1 = DF0 drive activity
    if (led == 1) {
        s_instance->m_driveLedState = (state != 0);
        if (state != 0)
            s_instance->m_driveActiveLatch = true;
    }
    // Libretro LED 7 is PuAE's aggregate hard-drive indicator. Older cores only expose this;
    // newer cores additionally provide adm_drain_hd_activity() for per-filesystem units.
    if (led == 7 && state != 0) {
        const int status = state == 2 ? 2 : 1;
        s_instance->m_aggregateHdActivityLatch =
            std::max(s_instance->m_aggregateHdActivityLatch, status);
        ++s_instance->m_aggregateHdActivityCount;
    }
}

void AmigaEmulator::PollHostMountActivity()
{
    if (m_adm_drain_hd_activity) {
        struct WireEvent
        {
            int unit;
            int status;
            uint32_t count;
        };
        WireEvent events[32]{};
        const int count = std::clamp(m_adm_drain_hd_activity(events, 32), 0, 32);
        for (int index = 0; index < count; ++index)
            m_hostMountService.RecordActivity(events[index].unit, events[index].status,
                                              events[index].count);
    }
    if (m_adm_get_active_filesystem_units) {
        int units[32]{};
        const int count = std::clamp(m_adm_get_active_filesystem_units(units, 32), 0, 32);
        m_hostMountService.ConfirmActiveUnits(std::vector<int>(units, units + count));
    }
    m_hostMountService.CheckConfirmationTimeout();
}

bool AmigaEmulator::EnvironmentCallback(unsigned cmd, void* data)
{
    if (!s_instance)
        return false;

    // Strip experimental flag for matching
    unsigned baseCmd = cmd & 0xFFFF;

    switch (baseCmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
            if (data)
                *(bool*)data = true;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const enum retro_pixel_format* fmt = (const enum retro_pixel_format*)data;
            if (*fmt == RETRO_PIXEL_FORMAT_RGB565 || *fmt == RETRO_PIXEL_FORMAT_XRGB8888 ||
                *fmt == RETRO_PIXEL_FORMAT_0RGB1555) {
                s_instance->m_pixelFormat = *fmt;
                return true;
            }
            return false;
        }
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            const char** dir = (const char**)data;
            *dir = s_instance->m_systemDir.c_str();
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            const char** dir = (const char**)data;
            *dir = s_instance->m_saveDir.c_str();
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable* var = (struct retro_variable*)data;
            if (!var || !var->key)
                return false;

            s_instance->m_variableScratch = s_instance->ResolveCoreVariable(var->key);
            if (s_instance->m_variableScratch.empty())
                return false;

            var->value = s_instance->m_variableScratch.c_str();
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            const struct retro_variable* vars = (const struct retro_variable*)data;
            if (vars) {
                for (; vars->key && vars->value; vars++) {
                    // "Description; default|other|other" - the core puts the default first.
                    std::string val = vars->value;
                    auto semi = val.find(';');
                    if (semi == std::string::npos)
                        continue;
                    std::string options = val.substr(semi + 1);
                    auto start = options.find_first_not_of(' ');
                    if (start != std::string::npos)
                        options = options.substr(start);

                    const std::string key = vars->key;
                    CoreOptionDeclaration declaration;
                    for (size_t pos = 0; pos <= options.size();) {
                        const auto pipe = options.find('|', pos);
                        const auto end = (pipe == std::string::npos) ? options.size() : pipe;
                        declaration.values.push_back(options.substr(pos, end - pos));
                        if (pipe == std::string::npos)
                            break;
                        pos = pipe + 1;
                    }
                    if (!declaration.values.empty())
                        declaration.defaultValue = declaration.values.front();
                    // Kept whole, overridden keys included: this is the record of what the
                    // loaded core will actually parse, which is what EmulatorOptions.h is
                    // validated against.
                    s_instance->m_coreOptionDeclarations[key] = declaration;

                    // A key Peekaboo has its own default for is answered from the option table,
                    // so storing the core's default here would only be misleading.
                    if (!IsCoreVariableOverridden(key))
                        s_instance->m_coreVariables[key] = declaration.defaultValue;
                }
            }
            s_instance->m_variablesUpdated = true;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            if (data) {
                *(bool*)data = s_instance->m_variablesUpdated;
                s_instance->m_variablesUpdated = false;
            }
            return true;
        }
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            struct retro_log_callback* cb = (struct retro_log_callback*)data;
            cb->log = CoreLog;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: {
            if (data)
                *(unsigned*)data = 0;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            return true;

        case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION: {
            if (data)
                *(unsigned*)data = 1;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_LED_INTERFACE: {
            struct retro_led_interface* iface = (struct retro_led_interface*)data;
            if (iface) {
                iface->set_led_state = LedStateCallback;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
            const auto* cb = static_cast<const retro_disk_control_callback*>(data);
            if (cb) {
                s_instance->m_diskControl = *cb;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: {
            const auto* cb = static_cast<const retro_disk_control_ext_callback*>(data);
            if (cb) {
                s_instance->m_diskControlExt = *cb;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_MESSAGE:
        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
            return true;

        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: {
            struct retro_keyboard_callback
            {
                retro_keyboard_event_t callback;
            };
            auto* kb = static_cast<retro_keyboard_callback*>(data);
            if (kb) {
                s_instance->m_keyboardCallback = kb->callback;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: {
            struct retro_controller_description
            {
                const char* desc;
                unsigned id;
            };
            struct retro_controller_info
            {
                const struct retro_controller_description* types;
                unsigned num_types;
            };
            const struct retro_controller_info* info = (const struct retro_controller_info*)data;
            if (info) {
                for (unsigned t = 0; t < info[0].num_types; t++) {
                    const auto& type = info[0].types[t];
                    if (type.desc && std::string(type.desc).find("CD32") != std::string::npos) {
                        s_instance->m_cd32DeviceId = type.id;
                        break;
                    }
                }
                core::Logger::Info("Emulator: Controller info: CD32 device ID={}",
                                   s_instance->m_cd32DeviceId);
            }
            return true;
        }

        default:
            return false;
    }
}

void AmigaEmulator::VideoRefreshCallback(const void* data, unsigned width, unsigned height,
                                         size_t pitch)
{
    // PuAE may announce its framebuffer during retro_init() with a non-null data pointer but
    // zero dimensions. There is no frame to upload in that case, and touching OpenGL here also
    // makes core initialization unnecessarily dependent on a current rendering context.
    if (!s_instance || !data || width == 0 || height == 0)
        return;

    if (width != s_instance->m_width || height != s_instance->m_height) {
        s_instance->m_width = width;
        s_instance->m_height = height;
    }

    // Digest first. Everything below this point is presentation, and a headless stepper
    // turns presentation off (m_acceptVideoRefresh = presentFrame) - hashing after the
    // early-out would produce no digest at all for exactly the runs that need one.
    if (s_instance->m_frameDigestEnabled) {
        unsigned bytesPerPixel = 2;
        if (s_instance->m_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888)
            bytesPerPixel = 4;
        // Several refreshes in one frame fold together rather than the last one winning.
        s_instance->m_videoDigestPending = core::Fnv1a64Value(
            core::HashFramebuffer(data, width, height, pitch, bytesPerPixel,
                                  static_cast<uint32_t>(s_instance->m_pixelFormat)),
            s_instance->m_videoDigestPending);
    }

    // A/V capture reads the core's framebuffer directly, and like the digest above it sits
    // ABOVE the presentation early-out: a run that is not presenting still has to record.
    if (s_instance->IsVideoRecording())
        s_instance->CaptureVideoFrame(data, width, height, pitch);

    if (!s_instance->m_acceptVideoRefresh)
        return;

    // Sample the scrub-preview thumbnail before anything touches OpenGL: it is a straight
    // read of the core's framebuffer, so it costs nothing on the GPU and still works when
    // there is no GL context at all.
    if (s_instance->m_thumbEnabled && --s_instance->m_thumbCountdown <= 0) {
        s_instance->m_thumbCountdown = s_instance->m_thumbInterval;
        s_instance->CaptureThumbnail(data, width, height, pitch);
    }

    // Everything below uploads the frame into an OpenGL texture. glad only resolves the GL
    // entry points once a context has been made current, so a headless embedder (unit tests,
    // batch tooling) would call straight through a null function pointer. Dimensions above are
    // still tracked; only the upload is skipped.
    if (!glad_glGenTextures)
        return;

    if (s_instance->m_textureID == 0) {
        glGenTextures(1, &s_instance->m_textureID);
        glBindTexture(GL_TEXTURE_2D, s_instance->m_textureID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_instance->m_texAllocW = s_instance->m_texAllocH = 0; // new texture has no storage yet
    }

    glBindTexture(GL_TEXTURE_2D, s_instance->m_textureID);

    GLenum format = GL_BGRA;
    GLenum type = GL_UNSIGNED_SHORT_1_5_5_5_REV;

    if (s_instance->m_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        format = GL_BGRA;
        type = GL_UNSIGNED_BYTE;
    } else if (s_instance->m_pixelFormat == RETRO_PIXEL_FORMAT_RGB565) {
        format = GL_RGB;
        type = GL_UNSIGNED_SHORT_5_6_5;
    } else if (s_instance->m_pixelFormat == RETRO_PIXEL_FORMAT_0RGB1555) {
        format = GL_BGRA;
        type = GL_UNSIGNED_SHORT_1_5_5_5_REV;
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / (type == GL_UNSIGNED_BYTE ? 4 : 2));

    // Reallocate texture storage only when the size or pixel format actually changed; otherwise
    // update in place. glTexImage2D reallocates the GPU texture every call — wasteful for the
    // hottest texture in the app, which keeps the same dimensions frame after frame.
    if (static_cast<int>(width) != s_instance->m_texAllocW ||
        static_cast<int>(height) != s_instance->m_texAllocH ||
        format != s_instance->m_texAllocFormat || type != s_instance->m_texAllocType) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, format, type, data);
        s_instance->m_texAllocW = static_cast<int>(width);
        s_instance->m_texAllocH = static_cast<int>(height);
        s_instance->m_texAllocFormat = format;
        s_instance->m_texAllocType = type;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void AmigaEmulator::SetThumbnailCapture(bool enabled, int everyNFrames)
{
    m_thumbInterval = std::max(1, everyNFrames);
    if (m_thumbEnabled == enabled)
        return;
    m_thumbEnabled = enabled;
    // Fire on the very next frame so a scrub has something to show immediately rather than
    // after a whole interval of nothing.
    m_thumbCountdown = enabled ? 1 : 0;
    if (!enabled) {
        m_thumbRGB.clear();
        m_thumbRGB.shrink_to_fit();
        m_thumbW = m_thumbH = 0;
    }
}

// The core's framebuffer at native size, as RGBA top row first - what the capture sinks take. The
// format handling mirrors CaptureThumbnail's, including its UNKNOWN-means-0RGB1555 default.
void AmigaEmulator::CaptureVideoFrame(const void* data, unsigned width, unsigned height,
                                      size_t pitch)
{
    if (!data || width == 0 || height == 0)
        return;
    m_captureFrameScratch.resize(static_cast<size_t>(width) * height * 4);
    m_captureFrameWidth = static_cast<int>(width);
    m_captureFrameHeight = static_cast<int>(height);

    const auto* base = static_cast<const uint8_t*>(data);
    uint8_t* out = m_captureFrameScratch.data();
    const bool xrgb8888 = m_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888;
    const bool rgb565 = m_pixelFormat == RETRO_PIXEL_FORMAT_RGB565;

    for (unsigned y = 0; y < height; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * pitch;
        for (unsigned x = 0; x < width; ++x) {
            if (xrgb8888) {
                const uint8_t* p = row + static_cast<size_t>(x) * 4;
                *out++ = p[2];
                *out++ = p[1];
                *out++ = p[0];
            } else {
                uint16_t v = 0;
                std::memcpy(&v, row + static_cast<size_t>(x) * 2, sizeof(v));
                if (rgb565) {
                    // Replicate the high bits into the low ones so full-scale stays full.
                    const uint8_t r = static_cast<uint8_t>((v >> 8) & 0xF8);
                    const uint8_t g = static_cast<uint8_t>((v >> 3) & 0xFC);
                    const uint8_t b = static_cast<uint8_t>((v << 3) & 0xF8);
                    *out++ = static_cast<uint8_t>(r | (r >> 5));
                    *out++ = static_cast<uint8_t>(g | (g >> 6));
                    *out++ = static_cast<uint8_t>(b | (b >> 5));
                } else { // 0RGB1555
                    const uint8_t r = static_cast<uint8_t>((v >> 7) & 0xF8);
                    const uint8_t g = static_cast<uint8_t>((v >> 2) & 0xF8);
                    const uint8_t b = static_cast<uint8_t>((v << 3) & 0xF8);
                    *out++ = static_cast<uint8_t>(r | (r >> 5));
                    *out++ = static_cast<uint8_t>(g | (g >> 5));
                    *out++ = static_cast<uint8_t>(b | (b >> 5));
                }
            }
            *out++ = 255; // opaque
        }
    }
    m_captureFramePending = true;
}

void AmigaEmulator::CaptureThumbnail(const void* data, unsigned width, unsigned height,
                                     size_t pitch)
{
    constexpr int tw = kThumbnailWidth;
    constexpr int th = kThumbnailHeight;
    m_thumbRGB.resize(static_cast<size_t>(tw) * th * 3);

    // The source x offsets are the same for every row, so computing them once turns the
    // inner loop into a table lookup plus a format conversion.
    uint32_t xs[tw];
    for (int x = 0; x < tw; ++x)
        xs[x] = (static_cast<uint32_t>(x) * width) / tw;

    const auto* base = static_cast<const uint8_t*>(data);
    uint8_t* out = m_thumbRGB.data();

    // Same format set the texture upload below handles; UNKNOWN follows its 0RGB1555
    // default rather than producing a blank preview.
    const bool xrgb8888 = m_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888;
    const bool rgb565 = m_pixelFormat == RETRO_PIXEL_FORMAT_RGB565;

    for (int y = 0; y < th; ++y) {
        const uint8_t* row = base + (static_cast<size_t>(y) * height / th) * pitch;
        for (int x = 0; x < tw; ++x) {
            if (xrgb8888) {
                const uint8_t* p = row + static_cast<size_t>(xs[x]) * 4;
                *out++ = p[2];
                *out++ = p[1];
                *out++ = p[0];
            } else {
                uint16_t v = 0;
                std::memcpy(&v, row + static_cast<size_t>(xs[x]) * 2, sizeof(v));
                if (rgb565) {
                    // Replicate the high bits into the low ones so full-scale stays full.
                    const uint8_t r = static_cast<uint8_t>((v >> 8) & 0xF8);
                    const uint8_t g = static_cast<uint8_t>((v >> 3) & 0xFC);
                    const uint8_t b = static_cast<uint8_t>((v << 3) & 0xF8);
                    *out++ = static_cast<uint8_t>(r | (r >> 5));
                    *out++ = static_cast<uint8_t>(g | (g >> 6));
                    *out++ = static_cast<uint8_t>(b | (b >> 5));
                } else { // 0RGB1555
                    const uint8_t r = static_cast<uint8_t>((v >> 7) & 0xF8);
                    const uint8_t g = static_cast<uint8_t>((v >> 2) & 0xF8);
                    const uint8_t b = static_cast<uint8_t>((v << 3) & 0xF8);
                    *out++ = static_cast<uint8_t>(r | (r >> 5));
                    *out++ = static_cast<uint8_t>(g | (g >> 5));
                    *out++ = static_cast<uint8_t>(b | (b >> 5));
                }
            }
        }
    }

    m_thumbW = tw;
    m_thumbH = th;
    ++m_thumbSeq;
}

bool AmigaEmulator::CopyThumbnail(std::vector<uint8_t>& out, int& w, int& h) const
{
    if (m_thumbRGB.empty() || m_thumbW <= 0 || m_thumbH <= 0)
        return false;
    out = m_thumbRGB;
    w = m_thumbW;
    h = m_thumbH;
    return true;
}

bool AmigaEmulator::CaptureScreenshotPNG(std::vector<uint8_t>& out) const
{
    out.clear();
    if (m_textureID == 0)
        return false;
    int w = m_texAllocW > 0 ? m_texAllocW : static_cast<int>(m_width);
    int h = m_texAllocH > 0 ? m_texAllocH : static_cast<int>(m_height);
    if (w <= 0 || h <= 0)
        return false;

    // Read the display texture back as 24-bit RGB. The texture's internal format is GL_RGB, so a
    // GL_RGB/GL_UNSIGNED_BYTE read is a straight copy with no alpha noise. Rows are top-first,
    // which is what PNG wants — no vertical flip needed.
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    glBindTexture(GL_TEXTURE_2D, m_textureID);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    auto writeCb = [](void* ctx, void* data, int size) {
        auto* vec = static_cast<std::vector<uint8_t>*>(ctx);
        const auto* bytes = static_cast<const uint8_t*>(data);
        vec->insert(vec->end(), bytes, bytes + size);
    };
    int rc = stbi_write_png_to_func(writeCb, &out, w, h, 3, rgb.data(), w * 3);
    return rc != 0 && !out.empty();
}

bool AmigaEmulator::CaptureFrameRGBA(std::vector<uint8_t>& out, int& w, int& h) const
{
    out.clear();
    w = h = 0;
    if (m_textureID == 0)
        return false;
    int tw = m_texAllocW > 0 ? m_texAllocW : static_cast<int>(m_width);
    int th = m_texAllocH > 0 ? m_texAllocH : static_cast<int>(m_height);
    if (tw <= 0 || th <= 0)
        return false;

    // Read the display texture back as 32-bit RGBA, top row first — exactly what gif-h expects.
    out.resize(static_cast<size_t>(tw) * th * 4);
    glBindTexture(GL_TEXTURE_2D, m_textureID);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    w = tw;
    h = th;
    return true;
}

void AmigaEmulator::AudioSampleCallback(int16_t left, int16_t right)
{
    if (!s_instance)
        return;
    int16_t pair[2] = {left, right};
    s_instance->WriteAudioSamples(pair, 1);
}

size_t AmigaEmulator::AudioSampleBatchCallback(const int16_t* data, size_t frames)
{
    if (!s_instance || !data)
        return frames;
    s_instance->WriteAudioSamples(data, frames);
    return frames;
}

void AmigaEmulator::InputPollCallback()
{
    if (!s_instance)
        return;

    // libretro polls once per frame and then queries; this is the one moment where the
    // input the core is about to see is fully resolved, so it is where a frame's input is
    // both captured and, during a replay, substituted.
    if (s_instance->m_replayActive) {
        // A replayed frame with no input set runs with nothing held rather than with
        // whatever the host happens to be doing.
        s_instance->ApplyInputState(s_instance->m_replayFramePending ? s_instance->m_replayInput
                                                                     : core::FrameInput{});
        s_instance->m_replayFramePending = false;
        return;
    }

    s_instance->PollGamepad();
    if (s_instance->m_inputCaptureEnabled)
        s_instance->CaptureFrameInput();
}

void AmigaEmulator::SetTotalFrameCount(uint64_t frame)
{
    m_totalFrameCount = frame;
    // Anything scheduled against an absolute frame number refers to a timeline that no
    // longer applies; releasing it now is better than having it fire at a random point.
    m_pendingKeys.clear();
    m_injectedKeys.clear();
    m_injectedMouseLeft = false;
    m_injectedMouseRight = false;
    m_injectedMouseLeftReleaseFrame = 0;
    m_injectedMouseRightReleaseFrame = 0;
}

void AmigaEmulator::SetInputCaptureEnabled(bool enabled)
{
    m_inputCaptureEnabled = enabled;
    if (!enabled) {
        m_lastFrameInput = core::FrameInput{};
        m_pendingKeyEvents.clear();
    }
}

void AmigaEmulator::SetFrameDigestEnabled(bool enabled)
{
    if (m_frameDigestEnabled == enabled)
        return;
    m_frameDigestEnabled = enabled;
    // Whatever was half-accumulated belonged to a frame nobody is going to compare.
    m_videoDigestPending = 0;
    m_audioDigestPending.Reset();
    m_lastFrameDigest = FrameDigest{};
}

uint64_t AmigaEmulator::ComputeRamDigest() const
{
    uint64_t hash = core::kFnvOffsetBasis;
    for (const MemoryRegion& region : GetMemoryRegions()) {
        // ROM is not state; a bank with no host pointer is not readable at all.
        if (!region.hostPtr || !region.writable || region.size == 0)
            continue;
        // The address and size go in as well as the contents, so a machine with different
        // memory fitted cannot collide with one that merely has different bytes in it.
        hash = core::Fnv1a64Value(region.amigaAddr, hash);
        hash = core::Fnv1a64Value(region.size, hash);
        hash = core::Fnv1a64(region.hostPtr, region.size, hash);
    }
    return hash;
}

void AmigaEmulator::CaptureFrameInput()
{
    core::FrameInput in;
    for (int i = 0; i < 16; ++i) {
        if (m_joypadButtons[i] || m_injectedJoyButtons[0][i])
            in.joypad[0] |= static_cast<uint16_t>(1u << i);
        if (m_injectedJoyButtons[1][i])
            in.joypad[1] |= static_cast<uint16_t>(1u << i);
    }

    in.mouseDeltaX = m_mouseDeltaX;
    in.mouseDeltaY = m_mouseDeltaY;
    if (m_mouseLeft || m_injectedMouseLeft)
        in.mouseButtons |= 0x01;
    if (m_mouseRight || m_injectedMouseRight)
        in.mouseButtons |= 0x02;
    if (m_mouseMiddle)
        in.mouseButtons |= 0x04;

    in.keysDown.reserve(m_pressedKeys.size() + m_injectedKeys.size());
    for (int k : m_pressedKeys)
        in.keysDown.push_back(static_cast<uint16_t>(k));
    for (const auto& [k, expiry] : m_injectedKeys) {
        if (m_pressedKeys.count(k) == 0)
            in.keysDown.push_back(static_cast<uint16_t>(k));
    }
    // The set iterates in bucket order; sorting keeps a frame's capture reproducible so a
    // recorded timeline compares equal to a replayed one.
    std::sort(in.keysDown.begin(), in.keysDown.end());

    in.keyEvents.swap(m_pendingKeyEvents);
    m_pendingKeyEvents.clear();

    m_lastFrameInput = std::move(in);
}

void AmigaEmulator::ApplyInputState(const core::FrameInput& in)
{
    for (int i = 0; i < 16; ++i) {
        m_joypadButtons[i] = ((in.joypad[0] >> i) & 1) != 0;
        m_injectedJoyButtons[0][i] = false;
        m_injectedJoyButtons[1][i] = ((in.joypad[1] >> i) & 1) != 0;
    }

    m_mouseDeltaX = in.mouseDeltaX;
    m_mouseDeltaY = in.mouseDeltaY;
    m_mouseLeft = (in.mouseButtons & 0x01) != 0;
    m_mouseRight = (in.mouseButtons & 0x02) != 0;
    m_mouseMiddle = (in.mouseButtons & 0x04) != 0;
    m_injectedMouseLeft = false;
    m_injectedMouseRight = false;

    // Injected holds carry a release frame that means nothing on a replayed frame, so the
    // recorded held-key set replaces both maps outright.
    m_pressedKeys.clear();
    m_injectedKeys.clear();
    for (uint16_t k : in.keysDown)
        m_pressedKeys.insert(static_cast<int>(k));

    if (m_keyboardCallback) {
        for (const auto& ev : in.keyEvents)
            m_keyboardCallback(ev.down, static_cast<unsigned>(ev.key), 0, 0);
    }
}

void AmigaEmulator::BeginInputReplay()
{
    if (m_replayActive)
        return;
    // Park the live input so a rewind cannot leave a key or button stuck down once the
    // machine is handed back to the user.
    CaptureFrameInput();
    m_savedLiveInput = m_lastFrameInput;
    // Only the held-key set is parked. The edges CaptureFrameInput just drained were
    // already delivered to the core when they happened; restoring them on the way out
    // would fire every keypress a second time.
    m_savedLiveInput.keyEvents.clear();
    m_replayActive = true;
    m_replayFramePending = false;
}

void AmigaEmulator::EndInputReplay()
{
    if (!m_replayActive)
        return;
    m_replayActive = false;
    m_replayFramePending = false;
    ApplyInputState(m_savedLiveInput);
    m_savedLiveInput = core::FrameInput{};
    m_pendingKeyEvents.clear();
}

void AmigaEmulator::SetReplayInput(const core::FrameInput& input)
{
    m_replayInput = input;
    m_replayFramePending = true;
}

int16_t AmigaEmulator::InputStateCallback(unsigned port, unsigned device, unsigned /*index*/,
                                          unsigned id)
{
    if (!s_instance)
        return 0;

    // Libretro port 1 carries a second joypad only (mouse/keyboard stay on port 0).
    // PuAE maps it to the Amiga's port 0 - the mouse port - see AmigaEmulator.h.
    if (port == 1) {
        if (device == RETRO_DEVICE_JOYPAD && id < 16)
            return s_instance->m_injectedJoyButtons[1][id] ? 1 : 0;
        return 0;
    }

    // Port 0: mouse + keyboard + joypad (PuAE uses port 0 for all input)
    if (port == 0) {
        if (device == RETRO_DEVICE_MOUSE) {
            switch (id) {
                case RETRO_DEVICE_ID_MOUSE_X: {
                    int16_t dx = s_instance->m_mouseDeltaX;
                    s_instance->m_mouseDeltaX = 0;
                    return dx;
                }
                case RETRO_DEVICE_ID_MOUSE_Y: {
                    int16_t dy = s_instance->m_mouseDeltaY;
                    s_instance->m_mouseDeltaY = 0;
                    return dy;
                }
                case RETRO_DEVICE_ID_MOUSE_LEFT:
                    return (s_instance->m_mouseLeft || s_instance->m_injectedMouseLeft) ? 1 : 0;
                case RETRO_DEVICE_ID_MOUSE_RIGHT:
                    return (s_instance->m_mouseRight || s_instance->m_injectedMouseRight) ? 1 : 0;
                case RETRO_DEVICE_ID_MOUSE_MIDDLE:
                    return s_instance->m_mouseMiddle ? 1 : 0;
                default:
                    return 0;
            }
        } else if (device == RETRO_DEVICE_KEYBOARD) {
            return (s_instance->m_pressedKeys.count(id) > 0 ||
                    s_instance->m_injectedKeys.count(id) > 0)
                       ? 1
                       : 0;
        } else if (device == RETRO_DEVICE_JOYPAD) {
            if (id < 16) {
                return (s_instance->m_joypadButtons[id] || s_instance->m_injectedJoyButtons[0][id])
                           ? 1
                           : 0;
            }
        }
    }

    return 0;
}

bool AmigaEmulator::StartEmpty()
{
    ActiveInstanceScope _g(this);

    if (!m_coreLoaded)
        return false;

    if (m_gameLoaded) {
        m_retro_unload_game();
        m_gameLoaded = false;
    }

    m_totalFrameCount = 0;
    ++m_sessionId; // frame numbering just rebased; see GetSessionId()
    ResetDemoPerformance();
    ClearInjectedMouseButtons();

    // Pass drive enabled state to PuAE patch via environment variables
    for (int i = 0; i < 4; i++) {
        std::string envName = "ADM_DRIVE" + std::to_string(i) + "_ENABLED";
#ifdef _WIN32
        _putenv_s(envName.c_str(), m_driveEnabled[i] ? "1" : "0");
#else
        setenv(envName.c_str(), m_driveEnabled[i] ? "1" : "0", 1);
#endif
    }

    if (m_retro_load_game(nullptr)) {
        m_gameLoaded = true;
        m_cdDriveInMachine = IsCdMachine();
        ReapplyCoreDebugState();

        struct retro_system_av_info av_info;
        m_retro_get_system_av_info(&av_info);
        m_maxWidth = av_info.geometry.max_width;
        m_maxHeight = av_info.geometry.max_height;
        if (av_info.geometry.aspect_ratio > 0.0f)
            m_aspectRatio = av_info.geometry.aspect_ratio;
        if (av_info.timing.fps > 0.0)
            m_targetFps = av_info.timing.fps;
        if (av_info.timing.sample_rate > 0.0)
            m_coreSampleRate = av_info.timing.sample_rate;

        m_lastFrameTime = std::chrono::steady_clock::now();
        m_frameAccumulator = 0.0;
        m_frameStartCycle = 0;
        m_prevFrameStartCycle = 0;
        // Start every game with the vpos-0 capture OFF and nothing latched. Arming it is
        // Application::UpdateFrameHistory()'s decision, re-made every redraw, because the
        // capture is a real per-frame perturbation of the running machine and not merely a
        // cost - see SetStateCaptureEnabled(). Told to the core explicitly rather than
        // assumed: a first load never goes through UnloadCore(), so the core would otherwise
        // keep whatever the previous session left armed.
        m_lastFrameStartStateSeq = 0;
        m_restorableStateRequested = false;
        m_stateCaptureArmed = false;
        if (m_adm_set_state_capture)
            m_adm_set_state_capture(0);

        // Flush stale ring buffer data from previous session
        if (m_adm_clear_bplpt_events)
            m_adm_clear_bplpt_events();
        if (m_adm_clear_sprpt_events)
            m_adm_clear_sprpt_events();
        ClearAudioTrace();
        if (m_adm_clear_blit_events)
            m_adm_clear_blit_events();
        ClearBlitterVisMap();
        if (m_adm_clear_disk_dma_events)
            m_adm_clear_disk_dma_events();
        if (m_adm_clear_idle_events)
            m_adm_clear_idle_events();
        if (m_adm_clear_copper_events)
            m_adm_clear_copper_events();
        if (m_adm_clear_reg_write_events)
            m_adm_clear_reg_write_events();
        if (m_adm_clear_cpu_trace)
            m_adm_clear_cpu_trace();

        InitAudio();

        return true;
    }

    return false;
}

bool AmigaEmulator::LoadUaeConfig(const std::string& configPath)
{
    ActiveInstanceScope _g(this);

    if (!m_coreLoaded)
        return false;

    if (!std::filesystem::exists(configPath)) {
        core::Logger::Warn("Emulator: UAE config not found: {}", configPath);
        return false;
    }

    if (m_gameLoaded) {
        m_retro_unload_game();
        m_gameLoaded = false;
    }

    m_totalFrameCount = 0;
    ++m_sessionId; // frame numbering just rebased; see GetSessionId()
    ResetDemoPerformance();
    ClearInjectedMouseButtons();

    // Pass drive enabled state to PuAE patch via environment variables
    for (int i = 0; i < 4; i++) {
        std::string envName = "ADM_DRIVE" + std::to_string(i) + "_ENABLED";
#ifdef _WIN32
        _putenv_s(envName.c_str(), m_driveEnabled[i] ? "1" : "0");
#else
        setenv(envName.c_str(), m_driveEnabled[i] ? "1" : "0", 1);
#endif
    }

    // Pass the .uae file directly to the core — PuAE natively parses UAE config files
    struct retro_game_info info = {};
    info.path = configPath.c_str();

    if (m_retro_load_game(&info)) {
        m_gameLoaded = true;
        m_cdDriveInMachine = IsCdMachine();
        ReapplyCoreDebugState();
        m_uaeConfigPath = configPath;

        struct retro_system_av_info av_info;
        m_retro_get_system_av_info(&av_info);
        m_maxWidth = av_info.geometry.max_width;
        m_maxHeight = av_info.geometry.max_height;
        if (av_info.geometry.aspect_ratio > 0.0f)
            m_aspectRatio = av_info.geometry.aspect_ratio;
        if (av_info.timing.fps > 0.0)
            m_targetFps = av_info.timing.fps;
        if (av_info.timing.sample_rate > 0.0)
            m_coreSampleRate = av_info.timing.sample_rate;

        m_lastFrameTime = std::chrono::steady_clock::now();
        m_frameAccumulator = 0.0;
        m_frameStartCycle = 0;
        m_prevFrameStartCycle = 0;
        // Start every game with the vpos-0 capture OFF and nothing latched. Arming it is
        // Application::UpdateFrameHistory()'s decision, re-made every redraw, because the
        // capture is a real per-frame perturbation of the running machine and not merely a
        // cost - see SetStateCaptureEnabled(). Told to the core explicitly rather than
        // assumed: a first load never goes through UnloadCore(), so the core would otherwise
        // keep whatever the previous session left armed.
        m_lastFrameStartStateSeq = 0;
        m_restorableStateRequested = false;
        m_stateCaptureArmed = false;
        if (m_adm_set_state_capture)
            m_adm_set_state_capture(0);

        if (m_adm_clear_bplpt_events)
            m_adm_clear_bplpt_events();
        if (m_adm_clear_sprpt_events)
            m_adm_clear_sprpt_events();
        ClearAudioTrace();
        if (m_adm_clear_blit_events)
            m_adm_clear_blit_events();
        ClearBlitterVisMap();
        if (m_adm_clear_disk_dma_events)
            m_adm_clear_disk_dma_events();
        if (m_adm_clear_idle_events)
            m_adm_clear_idle_events();
        if (m_adm_clear_copper_events)
            m_adm_clear_copper_events();
        if (m_adm_clear_reg_write_events)
            m_adm_clear_reg_write_events();
        if (m_adm_clear_cpu_trace)
            m_adm_clear_cpu_trace();
        // The registry is a static in the core, so neither retro_unload_game nor the ReinitCore
        // that precedes a host-mount launch empties it. Without this, an F5 run through the 7474
        // /api/run path inherits the previous program's Bartman resources - pointers into memory
        // the new program now owns. LoadADF has always cleared it; this path had not.
        if (m_adm_clear_debug_resources)
            m_adm_clear_debug_resources();

        InitAudio();

        core::Logger::Info("Emulator: Loaded UAE config: {}", configPath);
        return true;
    }

    return false;
}

void AmigaEmulator::ClearUaeConfig() { m_uaeConfigPath.clear(); }

std::string AmigaEmulator::GetUaeConfigName() const
{
    if (m_uaeConfigPath.empty())
        return {};
    return std::filesystem::path(m_uaeConfigPath).stem().string();
}

void AmigaEmulator::SetCapsImgEnv()
{
    // The core resolves capsimg from retro_system_directory + "capsimg.so", but we
    // ship "capsimg.dylib" (and the system dir may be a custom kickstart folder).
    // Probe the likely locations and export the first that exists as an absolute
    // path; the build_puae.sh patch makes load_capslib() honour ADM_CAPSIMG_PATH.
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    if (!m_systemDir.empty()) {
        candidates.emplace_back(fs::path(m_systemDir) / "capsimg.dylib");
        candidates.emplace_back(fs::path(m_systemDir) / "capsimg.so");
    }
    candidates.emplace_back("./emulation/capsimg.dylib");
    candidates.emplace_back("./emulation/capsimg.so");

    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) {
            std::string abs = fs::absolute(c, ec).string();
            if (ec)
                abs = c.string();
#ifdef _WIN32
            _putenv_s("ADM_CAPSIMG_PATH", abs.c_str());
#else
            setenv("ADM_CAPSIMG_PATH", abs.c_str(), 1);
#endif
            return;
        }
    }
    core::Logger::Warn("Emulator: capsimg plugin not found; IPF copy protection will not run");
}

bool AmigaEmulator::RunBootBlock(const std::vector<uint8_t>& sector0,
                                 const std::vector<uint8_t>& sector1)
{
    constexpr size_t kAdfSize = 80u * 2u * 11u * 512u;

    // Load the template ADF and patch its bootblock (first two sectors).
    static const std::vector<std::string> kTemplatePaths = {
        "images/bootblock_template.adf",
        "../images/bootblock_template.adf",
        "../../images/bootblock_template.adf",
    };

    std::vector<uint8_t> adf;
    for (const auto& path : kTemplatePaths) {
        std::ifstream f(path, std::ios::binary);
        if (f.is_open()) {
            adf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            break;
        }
    }

    if (adf.size() < kAdfSize) {
        // Fallback: blank disk.
        adf.assign(kAdfSize, 0);
    }

    std::copy_n(sector0.begin(), std::min<size_t>(512, sector0.size()), adf.begin());
    std::copy_n(sector1.begin(), std::min<size_t>(512, sector1.size()), adf.begin() + 512);
    return LoadADF(adf, "adf");
}

bool AmigaEmulator::LoadADF(const std::vector<uint8_t>& adfData, const std::string& fileExt)
{
    ActiveInstanceScope _g(this);

    if (!m_coreLoaded)
        return false;

    // For raw IPF images, point the core's CAPS plugin loader at our bundled
    // capsimg (it searches the system dir for "capsimg.so", but we ship
    // "capsimg.dylib"). The env override matches the ADM_DRIVEx_ENABLED pattern.
    if (fileExt == "ipf")
        SetCapsImgEnv();

    // Unload any previously loaded game first. PuAE opens the disk image itself
    // (need_fullpath) and keeps a handle on it; on Windows that handle can hold a
    // write lock on the temp path, so we must release it before rewriting the
    // same file below — otherwise the ofstream open fails with "permission denied".
    if (m_gameLoaded) {
        m_retro_unload_game();
        m_gameLoaded = false;
    }

    // PUAE requires need_fullpath — write the image to a temp file. The extension
    // mirrors the source format; PuAE detects IPF by the "CAPS" magic regardless,
    // but a correct extension keeps zfile's format handling unambiguous.
    std::error_code tmpEc;
    auto tempDir = std::filesystem::temp_directory_path(tmpEc);
    if (tmpEc) {
        core::Logger::Error("Emulator: Failed to resolve temp directory: {}", tmpEc.message());
        return false;
    }
    // Make sure the directory actually exists and is writable before we rely on it.
    std::filesystem::create_directories(tempDir, tmpEc);
    m_tempAdfPath = (tempDir / ("adm_emulator_" + TempImageTag() + "." + fileExt)).string();

    // A previous session's file may have been chmod'ed read-only by PuAE's
    // write-protect handling; clear it or the open below fails with EACCES.
    ClearReadOnlyAttribute(m_tempAdfPath);
    std::ofstream ofs(m_tempAdfPath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        core::Logger::Error("Emulator: Failed to open temp disk image for writing: {} ({})",
                            m_tempAdfPath, std::strerror(errno));
        return false;
    }
    if (!ofs.write(reinterpret_cast<const char*>(adfData.data()), adfData.size())) {
        core::Logger::Error("Emulator: Failed to write temp disk image: {} ({})", m_tempAdfPath,
                            std::strerror(errno));
        return false;
    }
    ofs.close();
    if (ofs.fail()) {
        core::Logger::Error("Emulator: Failed to flush temp disk image: {} ({})", m_tempAdfPath,
                            std::strerror(errno));
        return false;
    }

    struct retro_game_info info = {};
    info.path = m_tempAdfPath.c_str();
    info.data = nullptr;
    info.size = 0;

    m_totalFrameCount = 0;
    ++m_sessionId; // frame numbering just rebased; see GetSessionId()
    ResetDemoPerformance();
    ClearInjectedMouseButtons();
    m_driveLedState = false;
    m_driveActiveLatch = false;

    // Pass drive enabled state to PuAE patch via environment variables
    for (int i = 0; i < 4; i++) {
        std::string envName = "ADM_DRIVE" + std::to_string(i) + "_ENABLED";
#ifdef _WIN32
        _putenv_s(envName.c_str(), m_driveEnabled[i] ? "1" : "0");
#else
        setenv(envName.c_str(), m_driveEnabled[i] ? "1" : "0", 1);
#endif
    }

    // Bartman debug support: (re)assert force-boot-rom before the machine boots so the uaelib
    // gate ($F0FF60) is installed when rtarea is set up during load.
    if (m_adm_set_force_boot_rom)
        m_adm_set_force_boot_rom(m_bartmanDebugEnabled ? 1 : 0);

    if (m_retro_load_game(&info)) {
        m_gameLoaded = true;
        m_cdDriveInMachine = IsCdMachine();
        ReapplyCoreDebugState();

        struct retro_system_av_info av_info;
        m_retro_get_system_av_info(&av_info);
        m_maxWidth = av_info.geometry.max_width;
        m_maxHeight = av_info.geometry.max_height;
        if (av_info.geometry.aspect_ratio > 0.0f)
            m_aspectRatio = av_info.geometry.aspect_ratio;
        if (av_info.timing.fps > 0.0)
            m_targetFps = av_info.timing.fps;
        if (av_info.timing.sample_rate > 0.0)
            m_coreSampleRate = av_info.timing.sample_rate;

        m_lastFrameTime = std::chrono::steady_clock::now();
        m_frameAccumulator = 0.0;
        m_frameStartCycle = 0;
        m_prevFrameStartCycle = 0;
        // Start every game with the vpos-0 capture OFF and nothing latched. Arming it is
        // Application::UpdateFrameHistory()'s decision, re-made every redraw, because the
        // capture is a real per-frame perturbation of the running machine and not merely a
        // cost - see SetStateCaptureEnabled(). Told to the core explicitly rather than
        // assumed: a first load never goes through UnloadCore(), so the core would otherwise
        // keep whatever the previous session left armed.
        m_lastFrameStartStateSeq = 0;
        m_restorableStateRequested = false;
        m_stateCaptureArmed = false;
        if (m_adm_set_state_capture)
            m_adm_set_state_capture(0);

        // Flush stale ring buffer data from previous session
        if (m_adm_clear_bplpt_events)
            m_adm_clear_bplpt_events();
        if (m_adm_clear_sprpt_events)
            m_adm_clear_sprpt_events();
        ClearAudioTrace();
        if (m_adm_clear_blit_events)
            m_adm_clear_blit_events();
        ClearBlitterVisMap();
        if (m_adm_clear_disk_dma_events)
            m_adm_clear_disk_dma_events();
        if (m_adm_clear_idle_events)
            m_adm_clear_idle_events();
        if (m_adm_clear_copper_events)
            m_adm_clear_copper_events();
        if (m_adm_clear_reg_write_events)
            m_adm_clear_reg_write_events();
        if (m_adm_clear_cpu_trace)
            m_adm_clear_cpu_trace();
        if (m_adm_clear_debug_resources)
            m_adm_clear_debug_resources();

        InitAudio();

        return true;
    }

    return false;
}

bool AmigaEmulator::RunExecutable(const std::vector<uint8_t>& exeData, const std::string& filename)
{
    std::vector<uint8_t> adfData = CreateBootableADF(exeData, filename);
    if (adfData.empty())
        return false;

    return LoadADF(adfData);
}

bool AmigaEmulator::RunHostExecutable(const std::string& programPath, std::string& outError)
{
    ActiveInstanceScope _g(this);
    outError.clear();
    if (!m_coreLoaded) {
        outError = "PuAE core is not loaded";
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    if (m_hostLaunchWorkspacePath.empty()) {
        std::error_code ec;
        auto tempDirectory = std::filesystem::temp_directory_path(ec);
        if (ec) {
            outError = "unable to resolve temporary directory: " + ec.message();
            return false;
        }
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        m_hostLaunchWorkspacePath =
            (tempDirectory /
             ("peekaboo_host_run_" + std::to_string(m_instanceId) + "_" + std::to_string(nonce)))
                .string();
    }

    auto launch = core::HostExecutableLauncher::Prepare(
        std::filesystem::path(programPath), std::filesystem::path(m_hostLaunchWorkspacePath),
        outError);
    if (!launch)
        return false;

    if (!StartHostMountProfile(launch->profile, outError))
        return false;

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    core::Logger::Info("Emulator: host-launched '{}' from DH1 in {} ms",
                       launch->programPath.string(), elapsed.count());
    return true;
}

bool AmigaEmulator::StartConfiguredHostMounts(
    std::string& outError, const std::optional<std::filesystem::path>& baseConfig)
{
    return StartHostMountProfile(CreateLaunchProfile(baseConfig), outError);
}

core::EmulatorLaunchProfile
AmigaEmulator::CreateLaunchProfile(const std::optional<std::filesystem::path>& baseConfig) const
{
    auto profile = m_hostMountService.CreateConfiguredProfile(baseConfig);
    profile.modelId = GetAmigaModelInfo(m_currentModel).id;
    profile.cdImage = m_configuredCdImage;
    return profile;
}

std::optional<core::MachineConfig> AmigaEmulator::GetMachineConfig() const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_get_machine_config)
        return std::nullopt;
    int values[core::kMachineConfigValueCount]{};
    const int count = m_adm_get_machine_config(values, core::kMachineConfigValueCount);
    return core::ParseMachineConfig(values, count);
}

void AmigaEmulator::SetSerialPort(const core::SerialPortTarget& target)
{
    m_serialPort = target;
    m_serialPortSet = true;
    if (m_coreLoaded && m_adm_serial_set_port)
        m_adm_serial_set_port(target.ToCoreName().c_str());
}

void AmigaEmulator::SetParallelOutput(const std::filesystem::path& path)
{
    m_parallelOutput = path;
    m_parallelOutputSet = true;
    if (m_coreLoaded && m_adm_parallel_set_output)
        m_adm_parallel_set_output(reinterpret_cast<const char*>(path.u8string().c_str()));
}

void AmigaEmulator::SetMidiDevices(int outDevice, int inDevice)
{
    m_midiOutDevice = std::max(outDevice, -2);
    m_midiInDevice = std::max(inDevice, -1);
    m_midiDevicesSet = true;
    if (m_coreLoaded && m_adm_midi_set_devices)
        m_adm_midi_set_devices(m_midiOutDevice, m_midiInDevice);
}

std::optional<core::HostPortsStatus> AmigaEmulator::GetHostPortsStatus() const
{
    if (!m_coreLoaded || !m_adm_hostio_get_status)
        return std::nullopt;
    std::array<uint8_t, core::kHostPortsStatusWireSizeMidi> wire{};
    const int full = m_adm_hostio_get_status(wire.data(), static_cast<int>(wire.size()));
    return core::ParseHostPortsStatus(wire.data(), std::min(full, static_cast<int>(wire.size())));
}

bool AmigaEmulator::IsCdMachine() const
{
    return core::MachineRoms::IsCdMachine(GetAmigaModelInfo(m_currentModel).id);
}

bool AmigaEmulator::NeedsGeneratedConfig() const
{
    const auto& mounts = m_hostMountService.GetConfiguredMounts();
    return std::any_of(mounts.begin(), mounts.end(),
                       [](const auto& mount) { return mount.enabled; }) ||
           !m_configuredCdImage.empty() || IsCdMachine();
}

bool AmigaEmulator::StartHostMountProfile(const core::EmulatorLaunchProfile& profile,
                                          std::string& outError)
{
    ActiveInstanceScope _g(this);
    outError.clear();
    if (!m_coreLoaded) {
        outError = "PuAE core is not loaded";
        return false;
    }
    if (m_hostLaunchWorkspacePath.empty()) {
        std::error_code ec;
        const auto temporary = std::filesystem::temp_directory_path(ec);
        if (ec) {
            outError = "unable to resolve temporary directory: " + ec.message();
            return false;
        }
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        m_hostLaunchWorkspacePath =
            (temporary /
             ("peekaboo_host_run_" + std::to_string(m_instanceId) + "_" + std::to_string(nonce)))
                .string();
    }

    std::filesystem::path configPath;
    core::EmulatorLaunchProfile effectiveProfile;
    auto launchProfile = profile;
    if (launchProfile.modelId.empty())
        launchProfile.modelId = GetAmigaModelInfo(m_currentModel).id;
    if (!m_hostMountService.PrepareLaunch(launchProfile, m_hostLaunchWorkspacePath, configPath,
                                          outError, &effectiveProfile))
        return false;

    // PuAE keeps filesystem process state across retro_load_game calls. ReinitCore is the
    // established stable cold-start path and is required whenever a generated mount profile is
    // applied, including repeated Bartman F5 runs.
    if (m_gameLoaded && !ReinitCore()) {
        outError = "unable to reinitialise PuAE for the host mount profile";
        m_hostMountService.MarkFailed(outError);
        return false;
    }
    if (!LoadUaeConfig(configPath.string())) {
        outError = "PuAE rejected the generated host-directory configuration";
        core::Logger::Error("Host mounts: PuAE rejected generated config '{}'",
                            configPath.string());
        m_hostMountService.MarkFailed(outError, false);
        return false;
    }
    core::Logger::Info("Host mounts: PuAE accepted generated config '{}'", configPath.string());
    m_hostMountService.MarkEffective(effectiveProfile);
    m_cdDriveInMachine = !effectiveProfile.cdImage.empty() ||
                         core::MachineRoms::IsCdMachine(effectiveProfile.modelId.empty()
                                                            ? GetAmigaModelInfo(m_currentModel).id
                                                            : effectiveProfile.modelId);
    if (!m_adm_get_active_filesystem_units && !m_reportedAggregateHdFallback) {
        core::Logger::Warn("Host mounts: this PuAE core cannot confirm active filesystem units; "
                           "using generated-config state and aggregate HD activity");
        m_reportedAggregateHdFallback = true;
    }
    m_aggregateHdActivityLatch = 0;
    m_aggregateHdActivityCount = 0;
    return true;
}

// AmigaDOS caps a filename at 30 characters and reads ':' and '/' as path
// separators, so a name lifted straight out of a TOSEC archive
// ("3D Demo II (1992-12)(Anarchy)[TP#2].exe", 39 characters) is refused outright by
// adfFileOpen. Trim the stem rather than the tail so the extension survives.
static std::string ToAmigaFilename(const std::string& name)
{
    std::string base = name;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos)
        base = base.substr(slash + 1);

    for (char& ch : base) {
        if (ch == ':' || ch == '/' || ch == '\\' || ch == '"' ||
            static_cast<unsigned char>(ch) < 0x20)
            ch = '-';
    }

    // AmigaDOS accepts a trailing space and then nothing can address the file.
    while (!base.empty() && base.back() == ' ')
        base.pop_back();

    constexpr size_t kMaxNameLen = 30; // ADF_MAX_NAME_LEN
    if (base.size() > kMaxNameLen) {
        std::string ext;
        const size_t dot = base.find_last_of('.');
        if (dot != std::string::npos && dot > 0 && base.size() - dot <= 5)
            ext = base.substr(dot);
        std::string stem = base.substr(0, kMaxNameLen - ext.size());
        while (!stem.empty() && stem.back() == ' ')
            stem.pop_back();
        base = stem + ext;
    }

    return base.empty() ? std::string("Program") : base;
}

/*static*/ std::vector<uint8_t>
AmigaEmulator::CreateBootableADF(const std::vector<uint8_t>& exeData, const std::string& filename)
{
    const std::string amigaName = ToAmigaFilename(filename);

    // Try to load template from assets/run.adf
    // Check multiple paths relative to potential CWDs (executable dir, project root)
    const std::vector<std::string> templatePaths = {"assets/run.adf", "../assets/run.adf",
                                                    "../../assets/run.adf"};

    std::vector<uint8_t> diskData;
    bool loaded = false;

    for (const auto& path : templatePaths) {
        std::ifstream file(path, std::ios::binary);
        if (file.is_open()) {
            diskData.assign((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
            loaded = true;
            core::Logger::Info("Emulator: Loaded run template from: {}", path);
            break;
        }
    }

    core::AdfHandler handler;
    // A scratch disk this function formats and fills itself. AdfHandler defaults to
    // write-protected, which is right for images the user opens and wrong for one
    // built here.
    handler.SetWriteProtected(false);
    bool mountSuccess = false;

    if (loaded && !diskData.empty()) {
        mountSuccess = handler.Mount(std::move(diskData));
        if (!mountSuccess) {
            core::Logger::Error("Emulator: Failed to mount run.adf template.");
        }
    }

    // Fallback to blank disk if template fails
    if (!mountSuccess) {
        core::Logger::Warn("Emulator: Template not found or invalid. Creating blank OFS disk.");
        diskData.resize(901120, 0);
        if (!handler.Mount(std::move(diskData))) {
            core::Logger::Error("Emulator: Failed to mount blank memory buffer.");
            return {};
        }
        if (!handler.Format("InstantRun", true)) {
            core::Logger::Error("Emulator: Failed to format memory disk.");
            return {};
        }
    }

    // Import executable to root
    if (!handler.ImportFile(0, amigaName, exeData)) {
        core::Logger::Error("Emulator: Failed to inject executable '{}'.", amigaName);
        return {};
    }

    // Handle S directory (find or create)
    auto tree = handler.GetFileTree();
    int32_t sSector = 0;

    // Look for existing 'S' directory
    for (const auto& node : tree.children) {
        if (node.name == "S" && node.isDir) {
            sSector = node.startSector;
            break;
        }
    }

    // If not found, create it
    if (sSector == 0) {
        if (!handler.CreateDirectory(0, "S")) {
            core::Logger::Error("Emulator: Failed to create S directory.");
            return {};
        }
        // Find it again to get sector
        tree = handler.GetFileTree();
        for (const auto& node : tree.children) {
            if (node.name == "S" && node.isDir) {
                sSector = node.startSector;
                break;
            }
        }
    }

    if (sSector == 0) {
        core::Logger::Error("Emulator: Could not find/create S directory sector.");
        return {};
    }

    // Check if startup-sequence exists in S and delete it (to avoid duplication/errors)
    const adm::core::FileNode* sNode = nullptr;
    // Helper to find S node in fresh tree is safer but we can just scan children of root since S is
    // at root
    for (const auto& node : tree.children) {
        if (node.startSector == sSector) {
            sNode = &node;
            break;
        }
    }

    if (sNode) {
        for (const auto& child : sNode->children) {
            if (child.name == "startup-sequence") {
                handler.DeleteFile(sSector, "startup-sequence");
                break;
            }
        }
    }

    // Create new startup-sequence. Quote the command unconditionally: nearly every
    // scene release carries spaces in its name, and unquoted, AmigaDOS runs the first
    // word and passes the rest as arguments.
    std::string startupContent = "\"" + amigaName + "\"\n";
    std::vector<uint8_t> startupData(startupContent.begin(), startupContent.end());

    if (!handler.ImportFile(sSector, "startup-sequence", startupData)) {
        core::Logger::Error("Emulator: Failed to create startup-sequence.");
        return {};
    }

    return handler.GetImageContent();
}

void AmigaEmulator::HarvestDiskEventsAndState()
{
    m_diskWriteEventCount = 0;
    if (m_adm_get_disk_write_events) {
        m_diskWriteEventCount =
            m_adm_get_disk_write_events(m_diskWriteEventCache, kMaxDiskWriteEvents);
        m_diskWriteEventCount =
            std::min(m_diskWriteEventCount, kMaxDiskWriteEvents); // guard against ABI/version skew
        if (m_adm_clear_disk_write_events)
            m_adm_clear_disk_write_events();
    }
    // The disk DMA ring is deliberately NOT drained per frame: it is a session log,
    // and its payload arena must stay addressable until the profiler has copied the
    // bytes out. Events carry a monotonic seq, so the profiler dedups on that and
    // the ring is only rewound by an explicit clear (start / disk change / reset).
    m_diskDmaEventCount = 0;
    if (m_adm_get_disk_dma_events) {
        const int n = m_adm_get_disk_dma_events(m_diskDmaEventCache, kMaxDiskDmaEvents);
        m_diskDmaEventCount = std::clamp(n, 0, kMaxDiskDmaEvents); // guard ABI/version skew
    }
    PollDiskState();
}

// Fetches the Paula channel trace and filters it to the CCK window [winLo, winHi).
void AmigaEmulator::HarvestAudioTrace(bool doFilter, uint32_t winLo, uint32_t winHi)
{
    SyncAudioTraceEnabled();
    if (AudioTraceWanted() && m_adm_get_audio_trace) {
        for (int ch = 0; ch < kAudioChannels; ++ch) {
            if (m_audioTrace[ch].size() < static_cast<size_t>(kMaxAudioSamples))
                m_audioTrace[ch].resize(kMaxAudioSamples);
            int rawCount = m_adm_get_audio_trace(ch, m_audioTrace[ch].data(), kMaxAudioSamples);
            rawCount = std::min(rawCount, kMaxAudioSamples); // guard against ABI/version skew
            if (rawCount < 0)
                rawCount = 0;
            if (doFilter) {
                int n = 0;
                for (int i = 0; i < rawCount; i++) {
                    uint32_t cyc = m_audioTrace[ch][i].cycle;
                    if (cyc >= winLo && cyc < winHi)
                        m_audioTrace[ch][n++] = m_audioTrace[ch][i];
                }
                m_audioTraceCount[ch] = n;
            } else {
                m_audioTraceCount[ch] = rawCount;
            }
        }
    } else {
        for (int ch = 0; ch < kAudioChannels; ++ch)
            m_audioTraceCount[ch] = 0;
    }
}

bool AmigaEmulator::RunOneRetroFrame(bool countFrame, bool presentFrame, bool harvestFrame)
{
    ActiveInstanceScope _g(this);
    ++m_totalRunCount;

    m_prevFrameStartCycle = m_frameStartCycle;

    // Where the machine stands before this call, so the vsync anchor read back afterwards can
    // be tested for having been stamped *during* it. See the boundary test below.
    uint32_t runFromCycle = 0;
    bool haveRunFromCycle = false;
    if (m_adm_get_beampos) {
        int bvpos = 0, bhpos = 0;
        m_adm_get_beampos(&runFromCycle, &bvpos, &bhpos);
        haveRunFromCycle = true;
    }

    // Before the frame, not after: the guest can sample /WPRO at any point inside
    // retro_run(), so the flag has to be right going in.
    AssertFloppyWriteProtect();

    // Held silence: not every way of starting a machine passes ReapplyCoreDebugState(), and a
    // fresh core comes up with all four voices on.
    if (m_forceSilent && m_adm_get_audio_channel_mask && m_adm_set_audio_channel_mask &&
        m_adm_get_audio_channel_mask() != 0)
        m_adm_set_audio_channel_mask(0);

    auto frameStart = std::chrono::steady_clock::now();
    m_acceptVideoRefresh = presentFrame;
    m_retro_run();
    m_acceptVideoRefresh = true;
    auto frameEnd = std::chrono::steady_clock::now();

    // A bounded one-shot returns directly from the CPU dispatch loop at an arbitrary
    // point in the chipset frame. adm_redraw_frame() swaps PUAE's live drawing buffers
    // and calls draw_frame(); that is only safe for debugger-staged stops and can
    // dereference incomplete drawing state after a direct pre-instruction stop.
    bool boundedOneShotStop = false;
    if (m_adm_get_oneshot_breakpoint_status) {
        const auto status = static_cast<OneShotBreakpointStatus>(
            m_adm_get_oneshot_breakpoint_status(nullptr, nullptr));
        boundedOneShotStop = status == OneShotBreakpointStatus::Hit ||
                             status == OneShotBreakpointStatus::BudgetExpired;
    }

    DrainSerialOutput();
    PollHostMountActivity();

    float coreMs = std::chrono::duration<float, std::milli>(frameEnd - frameStart).count();
    m_lastCoreMs = coreMs;

    // Capture the frame-start cycle anchor set by adm_record_frame_start() inside retro_run()
    m_frameStartCycle = m_adm_get_frame_start_cycles();

    // A real frame boundary occurred if the vsync anchor was stamped during this call, i.e.
    // it lands inside the cycle window this retro_run() covered.
    //
    // Comparing the anchor against the previous call's anchor instead — the obvious test, and
    // what this used to do — is wrong across a LoadState. The anchor is a cycle count, and
    // cycle counts are restored with the save state, so re-running the same frame from the
    // same restored state stamps the *same* anchor every time. Restore frame N-1 and re-run N
    // twice in a row and the second run reported no boundary crossed, marked a whole frame
    // partial, and failed the seek with "did not re-run cleanly" — reproducible in Peekaboo by
    // seeking to the frame the playhead is already parked on. Timeline playback now restores a
    // state every frame, so this path runs constantly.
    //
    // Modular subtraction throughout, so the uint32 cycle counter wrapping (~20 minutes of
    // emulated time) costs nothing.
    bool frameBoundaryCrossed;
    if (haveRunFromCycle && m_adm_get_beampos) {
        uint32_t nowCycle = 0;
        int bvpos = 0, bhpos = 0;
        m_adm_get_beampos(&nowCycle, &bvpos, &bhpos);
        const uint32_t span = nowCycle - runFromCycle;
        const uint32_t offset = m_frameStartCycle - runFromCycle;
        frameBoundaryCrossed = offset <= span;
    } else {
        // Core too old to report the beam. Keep the original test rather than guess.
        frameBoundaryCrossed = (m_prevFrameStartCycle != m_frameStartCycle);
    }

    // Frame length in colour clocks, straight from the core's vpos=0 anchor. A PAL frame is
    // 312 lines x 227 CCK = 70,824 (71,051 for a long frame). This is the ground truth for
    // whether the emulated machine is really running short frames or whether the audio the core
    // produced per frame is simply wrong.
    m_diagRetroRunCalls.fetch_add(1, std::memory_order_relaxed);
    if (frameBoundaryCrossed) {
        m_diagFrameBoundaries.fetch_add(1, std::memory_order_relaxed);
        if (m_prevFrameStartCycle != 0 && m_frameStartCycle > m_prevFrameStartCycle) {
            const uint32_t cck = m_frameStartCycle - m_prevFrameStartCycle;
            m_diagFrameCckTotal.fetch_add(cck, std::memory_order_relaxed);
            m_diagFrameCckCount.fetch_add(1, std::memory_order_relaxed);
            uint32_t prevMin = m_diagFrameCckMin.load(std::memory_order_relaxed);
            while (cck < prevMin && !m_diagFrameCckMin.compare_exchange_weak(
                                        prevMin, cck, std::memory_order_relaxed)) {
            }
            uint32_t prevMax = m_diagFrameCckMax.load(std::memory_order_relaxed);
            while (cck > prevMax && !m_diagFrameCckMax.compare_exchange_weak(
                                        prevMax, cck, std::memory_order_relaxed)) {
            }
        }
    }

    if (frameBoundaryCrossed)
        UpdateDemoPerformance(m_prevFrameStartCycle, m_frameStartCycle);

    if (countFrame || frameBoundaryCrossed)
        m_totalFrameCount++;

    // Close the frame's digest where the frame counter turns over, which is ahead of both
    // paths that fire m_onFrameComplete - so a consumer reading it from that callback is
    // reading the frame it was just told about.
    if (m_frameDigestEnabled && (countFrame || frameBoundaryCrossed)) {
        m_lastFrameDigest.video = m_videoDigestPending;
        m_lastFrameDigest.audio = m_audioDigestPending.Hash();
        m_lastFrameDigest.audioSamples = m_audioDigestPending.Frames();
        m_videoDigestPending = 0;
        m_audioDigestPending.Reset();
    }

    if (m_injectedMouseLeft && m_totalFrameCount >= m_injectedMouseLeftReleaseFrame) {
        m_injectedMouseLeft = false;
        m_injectedMouseLeftReleaseFrame = 0;
    }
    if (m_injectedMouseRight && m_totalFrameCount >= m_injectedMouseRightReleaseFrame) {
        m_injectedMouseRight = false;
        m_injectedMouseRightReleaseFrame = 0;
    }
    ClearExpiredInjectedKeys();

    if (countFrame) {
        // Rolling 1-second FPS
        m_fpsFrameCount++;
        float fpsElapsed = std::chrono::duration<float>(frameEnd - m_fpsTimer).count();
        if (fpsElapsed >= 1.0f) {
            m_rollingFps = m_fpsFrameCount / fpsElapsed;
            m_fpsFrameCount = 0;
            m_fpsTimer = frameEnd;
        }

        // Push sample into circular buffer
        m_perfHistory[m_perfHistoryHead] = {coreMs, m_rollingFps};
        m_perfHistoryHead = (m_perfHistoryHead + 1) % kPerfHistorySize;
        if (m_perfHistoryCount < kPerfHistorySize)
            ++m_perfHistoryCount;
    }

    // An intermediate completed frame cannot be observed before the next retro_run(). Keep all
    // emulation-facing bookkeeping above and disk state below, but defer large debug-ring copies
    // until the final catch-up frame. A mid-frame return is always harvested in full.
    if (!harvestFrame && frameBoundaryCrossed) {
        HarvestDiskEventsAndState();
        // This return skips the block that recomputes m_partialFrame, so clear it here: a
        // boundary was crossed, which is the definition of a whole frame. Left stale from
        // an earlier breakpoint stop it would tell the recorder to skip a perfectly good
        // frame, and a skipped frame is the hole this hook exists to close.
        m_partialFrame = false;
        // A composed film draws each frame's scopes, the catch-up frames' too: their samples
        // are gone once the next retro_run() starts.
        if (m_audioTraceForCapture && IsVideoRecording())
            HarvestAudioTrace(true, m_prevFrameStartCycle, m_frameStartCycle);
        // An intermediate catch-up frame is still a whole frame, and the frame-history
        // recorder has to see it: this is the only moment its machine state can be
        // serialized before the next retro_run() overwrites it.
        if (m_onFrameComplete)
            m_onFrameComplete(m_totalFrameCount);
        WriteCapturedFrame();
        return true;
    }

    // Suppressed presentation is only requested for expected whole intermediate frames. If a
    // breakpoint stopped the core early, redraw its buffered partial image before returning.
    if (!presentFrame && !frameBoundaryCrossed && !boundedOneShotStop && m_adm_redraw_frame)
        m_adm_redraw_frame();

    // Determine the CCK window the harvested events are filtered to.
    //  - Normal frame boundary crossed: the just-completed whole frame [prevStart, start).
    //  - retro_run() stopped early (a breakpoint fired mid-frame, so no vpos==0 crossing):
    //    the partial frame so far, [frameStart, breakCycle]. Every event carries an absolute
    //    CCK stamp, so clipping is just a range change. (Pause / Step-Frame always cross a
    //    boundary, so they keep showing whole frames — only breakpoints land here.)
    bool doFilter = frameBoundaryCrossed;
    uint32_t winLo = m_prevFrameStartCycle;
    uint32_t winHi = m_frameStartCycle;
    m_partialFrame = false;
    if (!frameBoundaryCrossed && m_frameStartCycle != 0 && m_adm_get_beampos) {
        uint32_t nowCycle = 0;
        int bvpos = 0, bhpos = 0;
        m_adm_get_beampos(&nowCycle, &bvpos, &bhpos);
        if (nowCycle > m_frameStartCycle) {
            winLo = m_frameStartCycle;
            winHi = nowCycle + 1; // inclusive of the break cycle
            doFilter = true;
            m_partialFrame = true;
            m_breakCycle = nowCycle;
            m_breakVpos = bvpos;
            m_breakHpos = bhpos;
        }
    }

    // The rest of this harvest is pure UI-feed data for the debugger windows (bplpt/sprpt/blit/
    // copper/reg-write/CPU-trace) - nothing else in the emulator reads it. Skipped while turbo
    // is suppressing it (see SetDebugHarvestSuppressed): none of it is meaningful while the
    // machine is running flat out, so fetching/filtering it every host frame is cost paid for
    // nobody to look at.
    if (m_debugHarvestSuppressed) {
        m_bplptEventCount = 0;
        m_sprptEventCount = 0;
        m_blitEventCount = 0;
        m_blitVisSpanCount = 0;
        m_idleEventCount = 0;
        for (int ch = 0; ch < kAudioChannels; ++ch)
            m_audioTraceCount[ch] = 0;
        m_copperExecEventCount = 0;
        m_copperExecOverflow = false;
        m_regWriteEventCount = 0;
        m_regWriteOverflow = false;
        m_cpuTraceCount = 0;
        m_cpuTraceOverflow = false;
    } else {
        // Fetch and CCK-window-filter bplpt events
        {
            int rawCount = m_adm_get_bplpt_events(m_bplptEvents, kMaxBplptEvents);
            rawCount = std::min(rawCount, kMaxBplptEvents); // guard against ABI/version skew
            if (doFilter) {
                int n = 0;
                for (int i = 0; i < rawCount; i++) {
                    uint32_t cyc = m_bplptEvents[i].cycle;
                    if (cyc >= winLo && cyc < winHi)
                        m_bplptEvents[n++] = m_bplptEvents[i];
                }
                m_bplptEventCount = n;
            } else {
                m_bplptEventCount = rawCount;
            }
        }

        // Fetch and CCK-window-filter sprpt events.
        // Use member scratch buffers (m_sprptWireBuf / m_sprptColorSnapBuf) instead of
        // stack-allocated arrays to avoid blowing the 8 MB main-thread stack — UAE's own
        // emulation loop already uses most of that budget before we get here.
        {
            // m_sprptWireBuf is a SprptWireScratch[kMaxSprptEvents] member: 12 bytes × 256 = 3 KB
            int rawCount = m_adm_get_sprpt_events(m_sprptWireBuf, kMaxSprptEvents);
            rawCount = std::min(rawCount, kMaxSprptEvents); // guard against ABI/version skew

            // Expand wire events → SprptEvent (colours zeroed initially)
            for (int i = 0; i < rawCount; i++) {
                m_sprptEvents[i].vpos = m_sprptWireBuf[i].vpos;
                m_sprptEvents[i].spriteIdx = m_sprptWireBuf[i].spriteIdx;
                m_sprptEvents[i].pad = m_sprptWireBuf[i].pad;
                m_sprptEvents[i].ptr = m_sprptWireBuf[i].ptr;
                m_sprptEvents[i].cycle = m_sprptWireBuf[i].cycle;
                std::memset(m_sprptEvents[i].colors, 0, sizeof(m_sprptEvents[i].colors));
            }

            // Merge colour snapshots if the new DLL is loaded.
            // m_sprptColorSnapBuf is a SprptColorSnap[kMaxSprptEvents] member: 32 bytes × 256 = 8
            // KB
            if (m_adm_get_sprpt_color_snaps && rawCount > 0) {
                int snapCount = m_adm_get_sprpt_color_snaps(m_sprptColorSnapBuf, rawCount);
                for (int i = 0; i < snapCount; i++)
                    std::memcpy(m_sprptEvents[i].colors, m_sprptColorSnapBuf[i].colors,
                                sizeof(m_sprptColorSnapBuf[i].colors));
            }

            if (doFilter) {
                int n = 0;
                for (int i = 0; i < rawCount; i++) {
                    uint32_t cyc = m_sprptEvents[i].cycle;
                    if (cyc >= winLo && cyc < winHi)
                        m_sprptEvents[n++] = m_sprptEvents[i];
                }
                m_sprptEventCount = n;
            } else {
                m_sprptEventCount = rawCount;
            }
        }

        // Fetch and CCK-window-filter blit events
        {
            int rawCount = m_adm_get_blit_events(m_blitEvents, kMaxBlitEvents);
            rawCount = std::min(rawCount, kMaxBlitEvents); // guard against ABI/version skew
            if (doFilter) {
                int n = 0;
                for (int i = 0; i < rawCount; i++) {
                    uint32_t cyc = m_blitEvents[i].start_cycle;
                    if (cyc >= winLo && cyc < winHi)
                        m_blitEvents[n++] = m_blitEvents[i];
                }
                m_blitEventCount = n;
            } else {
                m_blitEventCount = rawCount;
            }
        }

        // Fetch blitter-vis provenance spans. Unlike the event rings these describe the whole
        // frame that was just drawn, so there is no CCK window to filter them against.
        if (m_blitVisEnabled && m_adm_get_blitter_vis_spans && !m_blitVisSpans.empty()) {
            int rawCount = m_adm_get_blitter_vis_spans(m_blitVisSpans.data(), kMaxBlitVisSpans);
            m_blitVisSpanCount = std::min(rawCount, kMaxBlitVisSpans); // guard against ABI skew
        } else {
            m_blitVisSpanCount = 0;
        }

        // Fetch and CCK-window-filter idle-region events (Bartman debug_start_idle/stop_idle).
        // Optional: only present when the loaded core exports idle profiling. A start/stop pair can
        // straddle the frame boundary (idle usually brackets the vblank wait), so the scrubber
        // pairs the windowed events and treats a lone start/stop as running to the frame edge.
        if (m_adm_get_idle_events) {
            int rawCount = m_adm_get_idle_events(m_idleEvents, kMaxIdleEvents);
            rawCount = std::min(rawCount, kMaxIdleEvents); // guard against ABI/version skew
            if (doFilter) {
                int n = 0;
                for (int i = 0; i < rawCount; i++) {
                    uint32_t cyc = m_idleEvents[i].cycle;
                    if (cyc >= winLo && cyc < winHi)
                        m_idleEvents[n++] = m_idleEvents[i];
                }
                m_idleEventCount = n;
            } else {
                m_idleEventCount = rawCount;
            }
        } else {
            m_idleEventCount = 0;
        }

        // Fetch and CCK-window-filter the Paula channel trace. Same window as every other event
        // source, so the scopes line up with the frame scrubber playhead instead of drifting.
        HarvestAudioTrace(doFilter, winLo, winHi);

        // Fetch and CCK-window-filter copper events
        {
            int overflow = 0;
            int rawCount =
                m_adm_get_copper_events(m_copperExecEvents, kMaxCopperExecEvents, &overflow);
            rawCount = std::min(rawCount, kMaxCopperExecEvents); // guard against ABI/version skew
            if (doFilter) {
                int n = 0;
                for (int i = 0; i < rawCount; i++) {
                    uint32_t cyc = m_copperExecEvents[i].absolute_cycle;
                    if (cyc >= winLo && cyc < winHi)
                        m_copperExecEvents[n++] = m_copperExecEvents[i];
                }
                m_copperExecEventCount = n;
            } else {
                m_copperExecEventCount = rawCount;
            }
            m_copperExecOverflow = (overflow != 0);
        }

        // Fetch and CCK-window-filter custom-register writes. Optional export: a core predating
        // the register-log patch leaves the pointer null and the log simply stays empty.
        if (m_adm_get_reg_write_events) {
            int overflow = 0;
            if (m_regWriteEvents.empty())
                m_regWriteEvents.resize(kMaxRegWriteEvents);
            int rawCount =
                m_adm_get_reg_write_events(m_regWriteEvents.data(), kMaxRegWriteEvents, &overflow);
            rawCount = std::min(rawCount, kMaxRegWriteEvents); // guard against ABI/version skew
            if (doFilter) {
                int n = 0;
                for (int i = 0; i < rawCount; i++) {
                    uint32_t cyc = m_regWriteEvents[i].absolute_cycle;
                    if (cyc >= winLo && cyc < winHi)
                        m_regWriteEvents[n++] = m_regWriteEvents[i];
                }
                m_regWriteEventCount = n;
            } else {
                m_regWriteEventCount = rawCount;
            }
            m_regWriteOverflow = (overflow != 0);
        } else {
            m_regWriteEventCount = 0;
            m_regWriteOverflow = false;
        }

        // Fetch and CCK-window-filter CPU trace
        {
            int overflow = 0;
            const bool exactCompletedFrame = frameBoundaryCrossed && m_adm_get_cpu_trace_frame;
            int rawCount =
                exactCompletedFrame
                    ? m_adm_get_cpu_trace_frame(m_cpuTrace, kMaxCpuTraceEntries, &overflow)
                    : m_adm_get_cpu_trace(m_cpuTrace, kMaxCpuTraceEntries, &overflow);
            rawCount = std::min(rawCount, kMaxCpuTraceEntries); // guard against ABI/version skew
            if (doFilter && !exactCompletedFrame) {
                int n = 0;
                for (int i = 0; i < rawCount; i++) {
                    uint32_t cyc = m_cpuTrace[i].absolute_cycle;
                    if (cyc >= winLo && cyc < winHi)
                        m_cpuTrace[n++] = m_cpuTrace[i];
                }
                m_cpuTraceCount = n;
            } else {
                m_cpuTraceCount = rawCount;
            }
            m_cpuTraceOverflow = (overflow != 0);
        }

    } // !m_debugHarvestSuppressed

    HarvestDiskEventsAndState();

    // Whole frames only. A mid-frame breakpoint stop is not a state anything can seek to;
    // the RunForStep() that carries the frame to its boundary fires this instead.
    if (m_onFrameComplete && (countFrame || frameBoundaryCrossed) && !m_partialFrame)
        m_onFrameComplete(m_totalFrameCount);

    // Whole frames only here too: a partial frame is the previous picture with a band of the
    // new one over it, which would show up in the film as a tear. After the frame hook, so a
    // composed picture shows what the hook recorded of this frame (the Exocet video's parts).
    // Whole frames only here too: a partial frame is the previous picture with a band of the
    // new one over it, which would show up in the film as a tear.
    if ((countFrame || frameBoundaryCrossed) && !m_partialFrame)
        WriteCapturedFrame();
    return frameBoundaryCrossed;
}

// The frame just completed, into the recording: called at every whole frame, the catch-up
// batch's intermediate ones included - each carries its sound into the film, so each needs its
// picture or the two drift apart. A frame the core did not draw again (a dupe: we answer
// RETRO_ENVIRONMENT_GET_CAN_DUPE yes) is the last picture again, for the same reason.
void AmigaEmulator::WriteCapturedFrame()
{
    if (!IsVideoRecording() || (!m_captureFramePending && m_captureFrameScratch.empty()))
        return;
    if (!m_captureFramePending)
        ++m_captureFramesRepeated;
    m_captureFramePending = false;
    bool written = false;
    if (m_videoCompose) {
        VideoCaptureOptions::ComposeSource source;
        source.rgba = m_captureFrameScratch.data();
        source.width = m_captureFrameWidth;
        source.height = m_captureFrameHeight;
        // Its shape now, not when the film started: a recording from a cold boot goes from
        // the Kickstart's screen to whatever mode the demo sets
        const double aspect = m_aspectRatio > 0.0f ? static_cast<double>(m_aspectRatio) : 4.0 / 3.0;
        core::AviRecorder::ChooseScale(m_captureFrameWidth, m_captureFrameHeight, aspect,
                                       source.scaleX, source.scaleY);
        if (!m_videoCompose(source, m_videoComposed)) {
            EndVideoCapture("The picture could not be composed.");
            return;
        }
        written = m_videoCapture->WriteVideoFrame(
            m_videoComposed.data(), m_videoCapture->OutputWidth(), m_videoCapture->OutputHeight());
    } else {
        // A mode change (hires to lores, interlace on) on an "auto" resolution: every later
        // frame would be refused and the picture freeze while the sound ran on. The film ends
        // here, kept, with the reason.
        if (m_captureFrameWidth != m_captureSourceWidth ||
            m_captureFrameHeight != m_captureSourceHeight) {
            EndVideoCapture(fmt::format("The picture changed size ({}x{} -> {}x{}); recording "
                                        "stopped.",
                                        m_captureSourceWidth, m_captureSourceHeight,
                                        m_captureFrameWidth, m_captureFrameHeight));
            return;
        }
        written = m_videoCapture->WriteVideoFrame(m_captureFrameScratch.data(), m_captureFrameWidth,
                                                  m_captureFrameHeight);
    }
    // Two refusals end the film: a full AVI, closed while every size in it is still valid,
    // and an FFmpeg encoder that has died, whose result says why.
    if (!written && m_videoCapture->IsFull()) {
        const uint32_t frames = m_videoCapture->FramesWritten();
        EndVideoCapture(fmt::format("Stopped at the 4 GB AVI limit after {} frames ({:.1f} s).",
                                    frames, m_targetFps > 0.0 ? frames / m_targetFps : 0.0));
    } else if (!written && m_videoCapture->HasFailed()) {
        EndVideoCapture("The recording stopped.");
    }
}

void AmigaEmulator::Update()
{
    if (!m_coreLoaded || !m_gameLoaded)
        return;

    // Time spent emulating, so a main-thread delivery gap can be attributed to emulation cost
    // rather than to host-side work elsewhere in the frame.
    const auto updateEntry = std::chrono::steady_clock::now();
    struct UpdateTimer
    {
        AmigaEmulator* self;
        std::chrono::steady_clock::time_point entry;
        ~UpdateTimer()
        {
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - entry)
                    .count();
            const uint32_t us = static_cast<uint32_t>(ms * 1000.0);
            uint32_t prev = self->m_audioUpdateMaxUs.load(std::memory_order_relaxed);
            while (us > prev && !self->m_audioUpdateMaxUs.compare_exchange_weak(
                                    prev, us, std::memory_order_relaxed)) {
            }
            self->m_audioUpdateTotalUs.fetch_add(us, std::memory_order_relaxed);
            self->m_audioUpdateCount.fetch_add(1, std::memory_order_relaxed);
            if (ms > 40.0)
                self->m_audioUpdateSlow.fetch_add(1, std::memory_order_relaxed);
        }
    } updateTimer{this, updateEntry};

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - m_lastFrameTime).count();
    m_lastFrameTime = now;

    // How far behind we are willing to catch up before declaring the time lost. Both limits below
    // used to be far tighter (100 ms, and a 2-frame accumulator collapsed to 1), which made any
    // host-side stall permanently delete the audio for it: a measured 200 ms delivery gap dropped
    // ~340 ms of audio in a 20 s capture, and no amount of buffering can cover samples that were
    // never generated. Catch-up frames are cheap — intermediate frames already skip presentation
    // via the presentFrame flag — so the bound only needs to stop a slow host spiralling.
    static constexpr double kMaxCatchUpFrames = 12.0;

    double frameDuration = 1.0 / m_targetFps;
    const double maxElapsed = kMaxCatchUpFrames * frameDuration;

    // Cap large time gaps (e.g. window was minimized) to avoid an unbounded burst of frames.
    if (elapsed > maxElapsed) {
        // Emulated time we are choosing never to run. The audio for it is never generated, so no
        // amount of host-side buffering can cover the resulting hole — count it as a distinct
        // cause from a ring-buffer underrun.
        RecordDiscardedEmulatedTime(elapsed - maxElapsed, 0);
        elapsed = maxElapsed;
    }

    // Hold the audio ring at its setpoint by running fractionally fast or slow (+-0.6%), as WinUAE
    // does. Only at normal speed with sound playing: turbo and mute drain the ring on purpose.
    m_audioPace = 1.0;
    if (m_audioInitialized && !m_isMuted && m_speedMultiplier == 1.0) {
        if (m_pushActive) {
            if (PushPlaying(now))
                m_audioPace =
                    m_audioCushion.PaceFactor(m_pushQueuedEma, m_audioCushion.PushSetpoint());
        } else if (m_audioPrimed.load(std::memory_order_acquire)) {
            m_audioPace =
                m_audioCushion.PaceFactor(m_audioRingLevelEma.load(std::memory_order_relaxed));
        }
    }
    // Follow the default output device, which miniaudio did by itself.
    if (m_pushActive && now - m_pushLastDeviceCheck > std::chrono::seconds(1)) {
        m_pushLastDeviceCheck = now;
        // closed: a failed reopen has no enumerator, so DefaultDeviceChanged says no
        if (!m_pushOut->IsOpen() || m_pushOut->DefaultDeviceChanged())
            ReopenPushOutput();
    }
    m_frameAccumulator += elapsed * m_speedMultiplier * m_audioPace;

    // Bound the backlog, but run it rather than discarding it. Collapsing to a single frame here
    // was the dominant source of audio holes after a stall.
    if (m_speedMultiplier <= 1.0 && m_frameAccumulator > kMaxCatchUpFrames * frameDuration) {
        RecordDiscardedEmulatedTime(m_frameAccumulator - kMaxCatchUpFrames * frameDuration, 1);
        m_frameAccumulator = kMaxCatchUpFrames * frameDuration;
    }

    int framesDue = static_cast<int>(m_frameAccumulator / frameDuration);
    // A run-until-frame target caps the batch: at warp speed the accumulator is worth tens of
    // frames, and running the whole batch sails past the target before anyone can look at it.
    bool capped = false;
    if (m_frameBreakTarget && m_frameBreakTarget <= m_totalFrameCount)
        m_frameBreakTarget = 0;
    uint64_t stopTarget = m_frameStopTarget > m_totalFrameCount ? m_frameStopTarget : 0;
    if (m_frameBreakTarget && (!stopTarget || m_frameBreakTarget < stopTarget))
        stopTarget = m_frameBreakTarget;
    if (stopTarget) {
        const uint64_t remaining = stopTarget - m_totalFrameCount;
        if (remaining < static_cast<uint64_t>(framesDue)) {
            framesDue = static_cast<int>(remaining);
            capped = true;
        }
    }
    // At turbo the accumulator isn't bounded above (the kMaxCatchUpFrames cap a few lines up
    // only applies at <=1x). Worse, this call happens once per host loop iteration no matter
    // what, so every emulated frame it runs is "free" against the fixed per-iteration cost
    // (ImGui frame build, window compositing, glfwSwapBuffers) that iteration pays regardless —
    // a big batch amortizes that fixed cost over many frames, a tiny one pays it almost per
    // frame. Measured: at content-dependent cost of ~5-6ms/frame and this app's fixed
    // per-iteration cost, an 8ms box only fits ~1-2 frames per call, so throughput collapsed to
    // roughly the fixed-cost-bound iteration rate instead of the CPU-bound retro_run() rate.
    // 32ms keeps most of that amortization (dozens of frames per call at this content's cost)
    // while still bounding worst-case blocking to a fraction of the ~660ms this cured — the
    // stall-recovery/steady-state-overload case that made turbo feel frozen in the first place.
    constexpr double kMaxBatchMs = 32.0;
    const auto batchStart = std::chrono::steady_clock::now();
    for (int frame = 0; frame < framesDue; ++frame) {
        bool finalFrame = (frame + 1 == framesDue);
        // Only the frame flagged final gets presented (see below), so the time box has to
        // promote its stop point to final rather than just breaking out of the loop, or the
        // display would freeze on screen while frames keep silently advancing underneath it.
        if (!finalFrame && m_speedMultiplier > 1.0) {
            const double elapsedMs = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - batchStart)
                                         .count();
            if (elapsedMs >= kMaxBatchMs)
                finalFrame = true;
        }
        // Older compatible cores without the redraw export keep their original presentation
        // cadence so a mid-frame breakpoint can never hide its last video callback.
        const bool presentFrame = finalFrame || !m_adm_redraw_frame;
        const bool completed = RunOneRetroFrame(true, presentFrame, finalFrame);
        m_frameAccumulator -= frameDuration;
        if (!completed || finalFrame)
            break;
        // A host break raised by a frame the batch did not expect to stop on: stop anyway, and
        // show the frame it stopped on, which the batch had not presented.
        if (m_hostBreakPending) {
            if (m_adm_redraw_frame)
                m_adm_redraw_frame();
            break;
        }
    }
    // Wall time we deliberately declined to spend, so it must not burst out on the next call.
    if (capped) {
        RecordDiscardedEmulatedTime(m_frameAccumulator, 2);
        m_frameAccumulator = 0.0;
    }
}

void AmigaEmulator::RecordDiscardedEmulatedTime(double seconds, int site)
{
    if (seconds <= 0.0)
        return;
    m_audioTimeDiscardEvents.fetch_add(1, std::memory_order_relaxed);
    m_audioTimeDiscardedFrames.fetch_add(static_cast<uint64_t>(seconds * m_coreSampleRate),
                                         std::memory_order_relaxed);
    if (site >= 0 && site < 3)
        m_audioTimeDiscardBySite[site].fetch_add(1, std::memory_order_relaxed);
}

void AmigaEmulator::StepOneFrame(bool presentFrame)
{
    if (!m_coreLoaded || !m_gameLoaded)
        return;
    RunOneRetroFrame(true, presentFrame);
    m_lastFrameTime = std::chrono::steady_clock::now();
    m_frameAccumulator = 0.0;
}

void AmigaEmulator::RunForStep()
{
    if (!m_coreLoaded || !m_gameLoaded)
        return;
    RunOneRetroFrame(false);
}

void AmigaEmulator::RunToFrameEnd()
{
    if (!m_coreLoaded || !m_gameLoaded)
        return;
    // retro_run() returns at the next frame boundary (vpos==0) OR when a breakpoint stops
    // it early. To guarantee the partial frame completes, loop until a real boundary is
    // crossed, re-arming the debug trace each time so the instruction at any breakpoint we
    // land on is stepped over rather than re-firing forever. Capped so a pathological
    // breakpoint (e.g. one that hits every scanline) can't spin indefinitely.
    constexpr int kMaxIters = 600; // a PAL frame is ~313 lines; this comfortably clears one
    for (int i = 0; i < kMaxIters; ++i) {
        if (m_adm_arm_debug)
            m_adm_arm_debug();
        if (RunOneRetroFrame(false))
            break; // crossed a real frame boundary — the frame is complete
    }
    // Intermediate iterations latched debug-break-pending in the core; clear it so the next
    // Play/Resume doesn't immediately re-pause on a stale flag.
    if (m_adm_get_debug_break_pending)
        (void)m_adm_get_debug_break_pending();
    m_lastFrameTime = std::chrono::steady_clock::now();
    m_frameAccumulator = 0.0;
}

size_t AmigaEmulator::GetStateSize() const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_retro_serialize_size)
        return 0;
    ActiveInstanceScope _g(const_cast<AmigaEmulator*>(this));
    return m_retro_serialize_size();
}

std::vector<uint8_t> AmigaEmulator::SaveState() const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_retro_serialize_size || !m_retro_serialize)
        return {};
    ActiveInstanceScope _g(const_cast<AmigaEmulator*>(this));
    size_t size = m_retro_serialize_size();
    if (size == 0)
        return {};
    std::vector<uint8_t> data(size);
    if (!m_retro_serialize(data.data(), data.size())) {
        core::Logger::Error("Emulator: SaveState: retro_serialize failed");
        return {};
    }
    return data;
}

bool AmigaEmulator::LoadState(const std::vector<uint8_t>& data) const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_retro_unserialize || data.empty())
        return false;
    ActiveInstanceScope _g(const_cast<AmigaEmulator*>(this));
    // Note: we deliberately do NOT pre-check retro_serialize_size() against data.size().
    // PUAE computes that size from currprefs, which can still be stale immediately after
    // retro_load_game (before the first frame reconfigures memory), so the sizes legitimately
    // differ even for a compatible snapshot. retro_unserialize() uses the data's own length
    // and validates the state internally, returning false if it cannot apply it.
    if (m_retro_serialize_size && m_retro_serialize_size() != data.size()) {
        core::Logger::Warn("Emulator: LoadState: state size differs (core reports {}, snapshot is "
                           "{}) — attempting restore anyway",
                           m_retro_serialize_size(), data.size());
    }
    if (!m_retro_unserialize(data.data(), data.size()))
        return false;

    // retro_unserialize() completes the restore before it returns (it pumps m68k_go() until
    // savestate_state clears), so the chipset is settled by the time we get here and the
    // shadow can be rebuilt straight away.
    RebuildCustomStorage(data);
    return true;
}

void AmigaEmulator::SetStateCaptureEnabled(bool enable)
{
    if (!m_adm_set_state_capture)
        return;
    if (enable == m_stateCaptureArmed)
        return;
    ActiveInstanceScope _g(this);
    m_adm_set_state_capture(enable ? 1 : 0);
    m_stateCaptureArmed = enable;
    // Disarming drops whatever the core was holding, so the next reader must not be able to
    // mistake the stale sequence for a fresh capture.
    if (!enable)
        m_lastFrameStartStateSeq = 0;
}

std::vector<uint8_t> AmigaEmulator::SaveStateForRestore()
{
    if (!m_coreLoaded || !m_gameLoaded)
        return {};
    // Latched only when the ask arrives with the capture unarmed, because that is the case
    // the arming policy got wrong: this caller is about to be handed a mid-field state, and
    // the next one should not be. An ask that the capture was already serving proves nothing
    // and must not latch - otherwise the timeline recorder, which asks once a frame, would
    // pin the capture on for the rest of the session the first time a debugger window opened.
    if (!m_stateCaptureArmed)
        m_restorableStateRequested = true;
    if (m_adm_get_frame_start_state) {
        ActiveInstanceScope _g(this);
        uint64_t seq = 0;
        const size_t need = m_adm_get_frame_start_state(nullptr, 0, &seq);
        if (need > 0) {
            std::vector<uint8_t> out(need);
            const size_t got = m_adm_get_frame_start_state(out.data(), out.size(), &seq);
            if (got == need) {
                m_lastFrameStartStateSeq = seq;
                return out;
            }
        }
        // Capture is compiled in but nothing has been taken yet - it is off, or no frame
        // boundary has gone past since it was turned on. Fall through rather than fail.
    }
    if (!m_warnedNoFrameStartCapture) {
        m_warnedNoFrameStartCapture = true;
        core::Logger::Warn(
            "Emulator: no vpos-0 save state available ({}); falling back to a mid-field "
            "serialize. Restoring it rewinds the chipset behind the CPU, so a program that "
            "races the beam may not resume cleanly.",
            m_adm_get_frame_start_state ? "capture not armed yet" : "core predates the capture");
    }
    return SaveState();
}

bool AmigaEmulator::RebuildCustomStorage(const std::vector<uint8_t>& state) const
{
    if (!m_customStoragePtr)
        return false;

    const auto regs = core::ExtractCustomRegisters(state.data(), state.size());
    if (regs.empty()) {
        core::Logger::Warn("Emulator: LoadState: could not read custom registers out of the "
                           "snapshot — write-only registers will read stale until the program "
                           "writes them again");
        return false;
    }

    struct CustomStore
    {
        uint16_t value;
        uint32_t pc;
    };
    auto* store = reinterpret_cast<CustomStore*>(m_customStoragePtr);

    // Clear first: anything the savestate does not account for was written by whatever ran
    // before the restore and is now a lie. A zero there reads as "nothing has written this
    // register", which is exactly the truth for the restored machine.
    for (int i = 0; i < 256; ++i)
        store[i] = CustomStore{0, 0};

    for (const auto& r : regs) {
        const size_t idx = (r.offset & 0x1FEu) >> 1;
        store[idx].value = r.value;
        // pc identifies the instruction that last wrote the register. No instruction wrote
        // these — they came out of a snapshot — and inventing an address would put a bogus
        // writer in the Custom Registers window.
        store[idx].pc = 0;
    }
    return true;
}

AmigaEmulator::PerfStats AmigaEmulator::GetPerfStats() const
{
    const bool demoAvailable = m_cpuTraceEnabled && m_bartmanDebugEnabled && m_demoPerfAvailable;
    return {m_rollingFps, m_lastCoreMs, m_demoFps, m_demoIdlePercent, demoAvailable};
}

const AmigaEmulator::PerfSample* AmigaEmulator::GetPerfHistory(int& outCount, int& outHead) const
{
    outCount = m_perfHistoryCount;
    outHead = m_perfHistoryHead;
    return m_perfHistory;
}

bool AmigaEmulator::Reset()
{
    ActiveInstanceScope _g(this);

    if (!m_coreLoaded)
        return false;

    // A warm boot has to leave the machine standing: rebuilding it (HardReset) re-creates
    // every drive from the config/DF0 image alone, which is what used to throw away the disk
    // the user had booted and hand the boot back to the hard drive. Ctrl+Amiga+Amiga is
    // handled inside UAE's keyboard layer (inputdevice_do_keyboard), not by the running
    // program, so it also works on a machine that has already crashed.
    if (!m_gameLoaded || !m_keyboardCallback) {
        HardReset();
        return false;
    }

    // Drop anything the previous session was holding down; a stuck key would otherwise be
    // part of the reset combination, and a stuck Alt turns the keyboard reset into a hard one.
    ClearInjectedMouseButtons();

    ResetDemoPerformance();

    // Stale ring data belongs to the pre-reset machine.
    if (m_adm_clear_bplpt_events)
        m_adm_clear_bplpt_events();
    if (m_adm_clear_sprpt_events)
        m_adm_clear_sprpt_events();
    ClearAudioTrace();
    if (m_adm_clear_blit_events)
        m_adm_clear_blit_events();
    ClearBlitterVisMap();
    if (m_adm_clear_disk_dma_events)
        m_adm_clear_disk_dma_events();
    if (m_adm_clear_idle_events)
        m_adm_clear_idle_events();
    if (m_adm_clear_copper_events)
        m_adm_clear_copper_events();
    if (m_adm_clear_reg_write_events)
        m_adm_clear_reg_write_events();
    if (m_adm_clear_cpu_trace)
        m_adm_clear_cpu_trace();

    // UAE holds the CPU in reset while the keys are down and performs the reset itself on
    // release, so the combination must span more than one frame. keyboard_reset_seq only runs
    // away past five seconds of holding (which would escalate to a hard reset), so a short
    // hold is exactly the soft, keyboard-initiated reset uae_reset(0, 1) we want.
    constexpr uint32_t kResetHoldFrames = 3;
    PressKey(static_cast<int>(RetroKey::RSuper), kResetHoldFrames,
             {static_cast<int>(RetroKey::LCtrl), static_cast<int>(RetroKey::LSuper)});

    core::Logger::Info("Emulator: Warm reset (Ctrl+Amiga+Amiga) — drive media left in place");
    return true;
}

void AmigaEmulator::HardReset()
{
    ActiveInstanceScope _g(this);

    if (!m_coreLoaded)
        return;

    // Use unload+reload instead of m_retro_reset() which routes through PuAE's
    // retro_reset_hard() → retro_create_config() + uae_reset() path, which intermittently crashes.
    // The unload+reload cycle is the same stable path used by LoadADF/StartEmpty/LoadUaeConfig.
    if (m_gameLoaded) {
        m_retro_unload_game();
        m_gameLoaded = false;
    }

    m_totalFrameCount = 0;
    ++m_sessionId; // frame numbering just rebased; see GetSessionId()
    ResetDemoPerformance();
    ClearInjectedMouseButtons();

    // Re-pass drive enabled state
    for (int i = 0; i < 4; i++) {
        std::string envName = "ADM_DRIVE" + std::to_string(i) + "_ENABLED";
#ifdef _WIN32
        _putenv_s(envName.c_str(), m_driveEnabled[i] ? "1" : "0");
#else
        setenv(envName.c_str(), m_driveEnabled[i] ? "1" : "0", 1);
#endif
    }

    struct retro_game_info info = {};
    bool loaded = false;

    if (!m_uaeConfigPath.empty()) {
        info.path = m_uaeConfigPath.c_str();
        loaded = m_retro_load_game(&info);
    } else if (!m_tempAdfPath.empty()) {
        info.path = m_tempAdfPath.c_str();
        loaded = m_retro_load_game(&info);
    } else {
        loaded = m_retro_load_game(nullptr);
    }

    if (loaded) {
        m_gameLoaded = true;
        m_cdDriveInMachine = IsCdMachine();
        ReapplyCoreDebugState();

        struct retro_system_av_info av_info;
        m_retro_get_system_av_info(&av_info);
        m_maxWidth = av_info.geometry.max_width;
        m_maxHeight = av_info.geometry.max_height;
        if (av_info.geometry.aspect_ratio > 0.0f)
            m_aspectRatio = av_info.geometry.aspect_ratio;
        if (av_info.timing.fps > 0.0)
            m_targetFps = av_info.timing.fps;
        if (av_info.timing.sample_rate > 0.0)
            m_coreSampleRate = av_info.timing.sample_rate;

        m_lastFrameTime = std::chrono::steady_clock::now();
        m_frameAccumulator = 0.0;
    }

    for (int i = 0; i < 4; i++)
        m_currentTracks[i] = 0;
    m_currentSide = 0;
    m_frameStartCycle = 0;
    m_prevFrameStartCycle = 0;

    if (m_adm_clear_bplpt_events)
        m_adm_clear_bplpt_events();
    if (m_adm_clear_sprpt_events)
        m_adm_clear_sprpt_events();
    ClearAudioTrace();
    if (m_adm_clear_blit_events)
        m_adm_clear_blit_events();
    ClearBlitterVisMap();
    if (m_adm_clear_disk_dma_events)
        m_adm_clear_disk_dma_events();
    if (m_adm_clear_idle_events)
        m_adm_clear_idle_events();
    if (m_adm_clear_copper_events)
        m_adm_clear_copper_events();
    if (m_adm_clear_reg_write_events)
        m_adm_clear_reg_write_events();
    if (m_adm_clear_cpu_trace)
        m_adm_clear_cpu_trace();

    InitAudio();
}

void AmigaEmulator::EjectDisk()
{
    bool ejected = false;
    if (m_diskControlExt.set_eject_state) {
        m_diskControlExt.set_eject_state(true);
        core::Logger::Info("Emulator: Disk ejected via disk control EXT interface");
        ejected = true;
    } else if (m_diskControl.set_eject_state) {
        m_diskControl.set_eject_state(true);
        core::Logger::Info("Emulator: Disk ejected via disk control interface");
        ejected = true;
    }

    if (!ejected) {
        core::Logger::Warn("Emulator: Cannot eject: disk control interface not available.");
        // Do not unload game as it shuts down the emulation
    }
}

unsigned AmigaEmulator::GetNumDriveSlots() const
{
    if (m_diskControlExt.get_num_images)
        return m_diskControlExt.get_num_images();
    if (m_diskControl.get_num_images)
        return m_diskControl.get_num_images();
    return 0;
}

bool AmigaEmulator::InsertDisk(unsigned driveIndex, const std::vector<uint8_t>& adfData,
                               const std::string& fileExt)
{
    if (!m_coreLoaded || !m_gameLoaded || driveIndex > 3)
        return false;

    // Need at least the basic disk control interface
    auto setEject = m_diskControlExt.set_eject_state ? m_diskControlExt.set_eject_state
                                                     : m_diskControl.set_eject_state;
    auto setIndex = m_diskControlExt.set_image_index ? m_diskControlExt.set_image_index
                                                     : m_diskControl.set_image_index;
    auto replaceImage = m_diskControlExt.replace_image_index ? m_diskControlExt.replace_image_index
                                                             : m_diskControl.replace_image_index;
    auto addImage = m_diskControlExt.add_image_index ? m_diskControlExt.add_image_index
                                                     : m_diskControl.add_image_index;
    auto getNumImages = m_diskControlExt.get_num_images ? m_diskControlExt.get_num_images
                                                        : m_diskControl.get_num_images;

    if (!setEject || !setIndex || !replaceImage || !addImage || !getNumImages) {
        core::Logger::Error("Emulator: InsertDisk: disk control interface not available");
        return false;
    }

    // Raw IPF reaches the core's CAPS plugin loader, which needs our bundled
    // capsimg pointed out to it - LoadADF does the same for the boot disk.
    if (fileExt == "ipf")
        SetCapsImgEnv();

    // Write the image to a temp file (unique per instance + drive). The extension
    // has to mirror the source format or the libretro loader routes the file to
    // the wrong reader - see LoadADF.
    auto tempDir = std::filesystem::temp_directory_path();
    std::string tempPath = (tempDir / ("adm_emulator_" + TempImageTag() + "_df" +
                                       std::to_string(driveIndex) + "." + fileExt))
                               .string();

    ClearReadOnlyAttribute(tempPath);
    std::ofstream ofs(tempPath, std::ios::binary);
    if (!ofs.write(reinterpret_cast<const char*>(adfData.data()), adfData.size())) {
        core::Logger::Error("Emulator: InsertDisk: failed to write temp ADF: {}", tempPath);
        return false;
    }
    ofs.close();
    m_tempAdfPaths[driveIndex] = tempPath;

    // Ensure enough image slots exist
    while (getNumImages() <= driveIndex) {
        addImage(nullptr);
    }

    setIndex(driveIndex);
    setEject(true);

    // Replace image with the new ADF
    struct retro_game_info info = {};
    info.path = tempPath.c_str();
    info.data = nullptr;
    info.size = 0;
    replaceImage(driveIndex, &info);

    setEject(false);

    // set_eject_state(false) runs floppy_open_redirect(), which calls
    // disk_setwriteprotect(..., 0) - so the insert we just did has cleared this
    // slot's write protection. Put it back.
    if (m_adm_set_floppy_write_protect)
        m_adm_set_floppy_write_protect(static_cast<int>(driveIndex),
                                       m_floppyWriteProtect[driveIndex] ? 1 : 0);

    core::Logger::Info("Emulator: InsertDisk: SUCCESS — mounted ADF in DF{} ({}){}", driveIndex,
                       tempPath, m_floppyWriteProtect[driveIndex] ? " [write-protected]" : "");
    return true;
}

// Presents a write-protect change to the guest as what it physically is: the disk
// coming out, the tab moving, and the disk going back in.
//
// Setting the core's forced flag alone is not enough in practice. Only drive_insert()
// leaves the drive in the state that sticks - it sets drv->wrprot as well as
// drv->forcedwrprot - and only a disk change makes AmigaDOS re-read the protection
// and drop the filesystem buffers it still holds for a disk the user has just locked.
// Without the re-insert those buffers are written to a disk PuAE now refuses, and the
// guest reads its own writes back as garbage: the repeating "read/write error"
// requester and the files that are gone after a reset.
//
// The image the core already has is what goes back in, NOT the host's copy: the
// guest's writes live in that temp file, and re-inserting from the document would
// silently roll the disk back to how it was mounted. The gap is UAE's own two-second
// disk-change delay, which is what gives every program time to notice.
bool AmigaEmulator::ReinsertDisk(unsigned driveIndex)
{
    if (!m_coreLoaded || !m_gameLoaded || driveIndex > 3)
        return false;
    // A live drive has a real disk in it; its tab is the hardware's business.
    if (m_bridgeDrive[driveIndex])
        return false;

    // DF0 booted through retro_load_game() is backed by m_tempAdfPath; every other
    // slot, and DF0 after a hot swap, by m_tempAdfPaths[].
    std::string path = m_tempAdfPaths[driveIndex];
    if (path.empty() && driveIndex == 0)
        path = m_tempAdfPath;
    if (path.empty())
        return false;

    auto setEject = m_diskControlExt.set_eject_state ? m_diskControlExt.set_eject_state
                                                     : m_diskControl.set_eject_state;
    auto setIndex = m_diskControlExt.set_image_index ? m_diskControlExt.set_image_index
                                                     : m_diskControl.set_image_index;
    auto replaceImage = m_diskControlExt.replace_image_index ? m_diskControlExt.replace_image_index
                                                             : m_diskControl.replace_image_index;
    if (!setEject || !setIndex || !replaceImage) {
        core::Logger::Error("Emulator: ReinsertDisk: disk control interface not available");
        return false;
    }

    setIndex(driveIndex);
    setEject(true);

    // Naming the file again rather than trusting the core's own record of the slot,
    // which retro_set_eject_state(true) has just cleared.
    struct retro_game_info info = {};
    info.path = path.c_str();
    info.data = nullptr;
    info.size = 0;
    replaceImage(driveIndex, &info);

    setEject(false);

    // The insert clears the slot's protection (floppy_open_redirect ->
    // disk_setwriteprotect(..., 0)); put it back now rather than waiting for the next
    // frame's assert, so the value is already right when the delayed drive_insert()
    // reads changed_prefs two seconds from here.
    if (m_adm_set_floppy_write_protect)
        m_adm_set_floppy_write_protect(static_cast<int>(driveIndex),
                                       m_floppyWriteProtect[driveIndex] ? 1 : 0);

    core::Logger::Info("Emulator: DF{} re-inserted so the guest re-reads the disk{}", driveIndex,
                       m_floppyWriteProtect[driveIndex] ? " [write-protected]" : " [writable]");
    return true;
}

void AmigaEmulator::EjectDisk(unsigned driveIndex)
{
    if (!m_coreLoaded || !m_gameLoaded || driveIndex > 3)
        return;

    auto setEject = m_diskControlExt.set_eject_state ? m_diskControlExt.set_eject_state
                                                     : m_diskControl.set_eject_state;
    auto setIndex = m_diskControlExt.set_image_index ? m_diskControlExt.set_image_index
                                                     : m_diskControl.set_image_index;
    auto replaceImage = m_diskControlExt.replace_image_index ? m_diskControlExt.replace_image_index
                                                             : m_diskControl.replace_image_index;

    if (!setEject || !setIndex || !replaceImage) {
        core::Logger::Error("Emulator: EjectDisk: disk control interface not available");
        return;
    }

    setEject(true);
    setIndex(driveIndex);
    replaceImage(driveIndex, nullptr);

    core::Logger::Info("Emulator: EjectDisk: ejected DF{}", driveIndex);

    // Clean up temp file
    RemoveTempDiskImage(m_tempAdfPaths[driveIndex]);
}

bool AmigaEmulator::PrepareWhdloadPayload(bool buttonWait, bool noWriteCache, int prefsMode,
                                          int theme, WhdloadPayload& out, std::string& outError)
{
    ActiveInstanceScope _g(this);
    outError.clear();
    if (!m_coreLoaded) {
        outError = "PuAE core is not loaded";
        return false;
    }
    if (!m_adm_whdload_prepare) {
        outError = "this PuAE core cannot prepare WHDLoad (needs core 1.84.0 or later)";
        return false;
    }
    std::array<char, 4096> whdload{};
    std::array<char, 4096> saves{};
    const int ok =
        m_adm_whdload_prepare(buttonWait ? 1 : 0, noWriteCache ? 1 : 0, prefsMode, theme,
                              whdload.data(), saves.data(), static_cast<int>(whdload.size()));
    out.whdloadDirectory = std::filesystem::u8path(whdload.data());
    out.savesDirectory = std::filesystem::u8path(saves.data());
    if (!ok) {
        outError = "the core could not install WHDLoad into '" + std::string(whdload.data()) + "'";
        return false;
    }
    return true;
}

bool AmigaEmulator::InsertCd(const std::string& imagePath)
{
    ActiveInstanceScope _g(this);
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_cd_insert)
        return false;
    if (!m_adm_cd_insert(imagePath.c_str()))
        return false;
    core::Logger::Info("Emulator: CD0 insert requested: {}", imagePath);
    return true;
}

void AmigaEmulator::EjectCd()
{
    ActiveInstanceScope _g(this);
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_cd_eject)
        return;
    m_adm_cd_eject();
    core::Logger::Info("Emulator: CD0 eject requested");
}

std::string AmigaEmulator::GetCdImage() const
{
    if (!m_coreLoaded || !m_gameLoaded || !m_adm_cd_get_image)
        return {};
    std::array<char, 4096> image{};
    m_adm_cd_get_image(image.data(), static_cast<int>(image.size()));
    return std::string(image.data());
}

int AmigaEmulator::GetCdLed() const
{
    if (!m_guiDataPtr || !m_gameLoaded)
        return 0;
    return static_cast<const puae_gui_info*>(m_guiDataPtr)->cd > 0 ? 1 : 0;
}

bool AmigaEmulator::ExtractArchive(const std::filesystem::path& archive,
                                   const std::filesystem::path& destination) const
{
    // Counted in, so UnloadCore() cannot free the DLL while this thread is inside it.
    {
        std::lock_guard<std::mutex> lock(m_offThreadCoreMutex);
        if (m_offThreadCoreBlocked || !m_adm_extract_archive)
            return false;
        ++m_offThreadCoreCalls;
    }
    const auto archiveUtf8 = archive.u8string();
    const auto destinationUtf8 = destination.u8string();
    const bool ok =
        m_adm_extract_archive(reinterpret_cast<const char*>(archiveUtf8.c_str()),
                              reinterpret_cast<const char*>(destinationUtf8.c_str())) != 0;
    {
        std::lock_guard<std::mutex> lock(m_offThreadCoreMutex);
        --m_offThreadCoreCalls;
    }
    m_offThreadCoreIdle.notify_all();
    return ok;
}

int AmigaEmulator::IdentifyRom(const std::filesystem::path& file,
                               const std::filesystem::path& keyDirectory,
                               core::RomIdentity& out) const
{
    if (!m_adm_identify_rom)
        return -1;

    char name[256] = {};
    char model[128] = {};
    int version = 0, revision = 0, romVersion = 0, romRevision = 0, cloanto = 0;
    unsigned crc = 0, size = 0, type = 0;

    const auto fileUtf8 = file.u8string();
    const auto keyUtf8 = keyDirectory.u8string();
    const int result = m_adm_identify_rom(
        reinterpret_cast<const char*>(fileUtf8.c_str()),
        keyDirectory.empty() ? nullptr : reinterpret_cast<const char*>(keyUtf8.c_str()), name,
        static_cast<int>(sizeof(name)), model, static_cast<int>(sizeof(model)), &version, &revision,
        &romVersion, &romRevision, &crc, &size, &type, &cloanto);

    out.name = name;
    out.model = model;
    out.version = version;
    out.revision = revision;
    out.romVersion = romVersion;
    out.romRevision = romRevision;
    out.crc32 = crc;
    out.size = size;
    out.type = type;
    // The database's cloanto flag says Cloanto distributes this ROM, not that this file is
    // encrypted - Amiga Forever's own ROMs have been plain for years. The file's header says.
    (void)cloanto;
    char header[11] = {};
    std::ifstream in(file, std::ios::binary);
    out.encrypted = in.read(header, sizeof(header)) && std::memcmp(header, "AMIROMTYPE1", 11) == 0;
    return result;
}

AmigaEmulator::RtgState AmigaEmulator::GetRtgState() const
{
    RtgState state;
    if (!m_adm_get_rtg_state)
        return state;

    uint32_t boardSize = 0, width = 0, height = 0, depth = 0;
    uint8_t active = 0;
    m_adm_get_rtg_state(&boardSize, &width, &height, &depth, &active);
    state.boardSize = boardSize;
    state.width = width;
    state.height = height;
    state.depth = depth;
    state.active = active != 0;
    return state;
}

void AmigaEmulator::PollDiskState()
{
    if (!m_guiDataPtr)
        return;

    auto* gui = static_cast<puae_gui_info*>(m_guiDataPtr);
    m_currentSide = gui->drive_side;
    for (int i = 0; i < 4; ++i) {
        m_currentTracks[i] = gui->drives[i].drive_track;
        m_driveMotorState[i] = gui->drives[i].drive_motor;
        m_driveWriteState[i] = gui->drives[i].drive_writing;
        m_diskInserted[i] = gui->drives[i].floppy_inserted;
        m_diskWriteProtected[i] = gui->drives[i].floppy_protected;

        // Detector, not the enforcement - AssertFloppyWriteProtect() re-pushes before
        // every retro_run(). Finding the core writable here therefore means a path
        // inside the frame that just ran cleared the flag, and the guest could have
        // written a track to a disk the user had locked. Count it, correct it now
        // rather than waiting for the next frame's assert, and say so once: a
        // non-zero drift names the core path that needs its own fix. Only this
        // direction is tested - the core legitimately reports "protected" for an
        // empty drive and under the global floppy_read_only option.
        if (m_floppyWriteProtect[i] && m_diskInserted[i] && !m_diskWriteProtected[i] &&
            !m_bridgeDrive[i] && m_adm_set_floppy_write_protect) {
            m_adm_set_floppy_write_protect(i, 1);
            m_diskWriteProtected[i] = true;
            const uint32_t n = ++m_floppyWriteProtectDrift[i];
            if (n == 1 || n % 250 == 0) {
                core::Logger::Warn("Emulator: DF{} lost its forced write protection inside the "
                                   "core and was writable for a frame (x{}); re-applied",
                                   i, n);
            }
        }
    }
}

// The host owns forced write protection, so the host has to keep saying so. Several
// core paths clear floppy[n].forcedwrprot without asking: drive_eject(), the delayed
// drive_insert() that DISK_reinsert() schedules two seconds after every libretro
// insert (it re-applies drv->newnamewriteprotected, which nothing here sets),
// restore_disk() on every save-state restore (drive_insert(..., false)), and
// disk_setwriteprotect() itself. Kickstart samples the /WPRO line fresh immediately
// before every track write - trackdisk.device does btst #3,$BFE001 and returns
// TDERR_WriteProt - so one frame with the flag dropped is not a cosmetic glitch: the
// guest writes the track, PuAE's drive_write_data() throws the data away because
// protection came back, and AmigaDOS reads its own write back as garbage. That is the
// repeating "read/write error" requester and the file that vanishes on reboot, in
// place of a clean "disk is write protected". Re-asserting is four stores per frame.
void AmigaEmulator::AssertFloppyWriteProtect()
{
    if (!m_adm_set_floppy_write_protect)
        return;
    for (int i = 0; i < 4; ++i) {
        // A live drive's protection is the tab on the physical disk, reported by the
        // bridge. Forcing the core's flag would override the hardware's own answer.
        if (m_bridgeDrive[i])
            continue;
        m_adm_set_floppy_write_protect(i, m_floppyWriteProtect[i] ? 1 : 0);
    }
}

void AmigaEmulator::SetFloppyWriteProtect(int driveIndex, bool writeProtected)
{
    if (driveIndex < 0 || driveIndex > 3)
        return;
    m_floppyWriteProtect[driveIndex] = writeProtected;
    if (m_adm_set_floppy_write_protect) {
        m_adm_set_floppy_write_protect(driveIndex, writeProtected ? 1 : 0);
        return;
    }
    // A padlock the emulated machine cannot see is worse than no padlock: the host
    // refuses its own edits, the guest writes happily, and the two disagree in
    // silence. Say so once per core, while a machine is actually running - with none
    // loaded there is nothing to protect yet.
    if (writeProtected && m_coreLoaded && !m_warnedNoWriteProtectSupport) {
        m_warnedNoWriteProtectSupport = true;
        core::Logger::Warn("Emulator: this core has no adm_set_floppy_write_protect "
                           "(needs PuAE >= 1.78.0) - the padlock protects the host's copy of "
                           "the image, but the emulated machine still sees a writable disk");
    }
}

void AmigaEmulator::SetMouseState(int16_t deltaX, int16_t deltaY, bool leftBtn, bool rightBtn,
                                  bool middleBtn)
{
    m_mouseDeltaX += deltaX;
    m_mouseDeltaY += deltaY;
    m_mouseLeft = leftBtn;
    m_mouseRight = rightBtn;
    m_mouseMiddle = middleBtn;
}

uint64_t AmigaEmulator::ClickMouseButton(MouseButton button, uint32_t durationFrames)
{
    const uint64_t releaseFrame = m_totalFrameCount + std::max<uint32_t>(durationFrames, 1);
    if (button == MouseButton::Left) {
        m_injectedMouseLeft = true;
        m_injectedMouseLeftReleaseFrame = releaseFrame;
    } else {
        m_injectedMouseRight = true;
        m_injectedMouseRightReleaseFrame = releaseFrame;
    }
    return releaseFrame;
}

void AmigaEmulator::ClearInjectedMouseButtons()
{
    m_injectedMouseLeft = false;
    m_injectedMouseRight = false;
    m_injectedMouseLeftReleaseFrame = 0;
    m_injectedMouseRightReleaseFrame = 0;
    // Injected keys and joypad buttons are cleared here too: every caller of this is a
    // session boundary (core unload, game load, reset) that also resets m_totalFrameCount,
    // which would otherwise leave key holds with release frames far in the future.
    ClearInjectedKeys();
    for (int p = 0; p < kJoypadPorts; ++p)
        ClearInjectedJoypad(p);
}

AmigaEmulator::KeyPress AmigaEmulator::PressKey(int retroKeyCode, uint32_t durationFrames,
                                                const std::vector<int>& modifiers)
{
    return PressKeyAt(retroKeyCode, m_totalFrameCount, durationFrames, modifiers);
}

AmigaEmulator::KeyPress AmigaEmulator::PressKeyAt(int retroKeyCode, uint64_t pressFrame,
                                                  uint32_t durationFrames,
                                                  const std::vector<int>& modifiers)
{
    const uint32_t hold = std::max<uint32_t>(durationFrames, 1);
    uint64_t start = std::max(pressFrame, m_totalFrameCount);

    // A press may not begin while the same key is still down from an earlier one, or
    // the guest never sees the key-up that separates them and two presses read as one.
    // Find the last frame on which any code involved in this press is still occupied -
    // by a live hold or by an already-queued press - and start after it. This is what
    // makes repeated press_key calls on a paused machine queue instead of collapsing.
    uint64_t busyUntil = 0;
    auto noteBusy = [&](int code) {
        if (auto it = m_injectedKeys.find(code); it != m_injectedKeys.end())
            busyUntil = std::max(busyUntil, it->second);
        for (const auto& pk : m_pendingKeys) {
            if (pk.code == code)
                busyUntil = std::max(busyUntil, pk.pressFrame + pk.durationFrames);
        }
    };
    for (int mod : modifiers)
        noteBusy(mod);
    noteBusy(retroKeyCode);

    // busyUntil is the release frame of the blocking press; +1 leaves one frame with the
    // key up between them, which is the gap a keyboard-scanning program needs to count
    // two distinct keystrokes.
    const bool queued = busyUntil > 0 && start <= busyUntil;
    if (queued)
        start = busyUntil + 1;

    const uint64_t releaseFrame = start + hold;

    // Modifiers go down first (and come up last) so a program sampling the keyboard
    // mid-hold never sees the base key without its Shift/Ctrl/Amiga qualifier.
    auto schedule = [&](int code) {
        if (start <= m_totalFrameCount) {
            auto& frame = m_injectedKeys[code];
            frame = std::max(frame, releaseFrame);
            if (m_keyboardCallback)
                m_keyboardCallback(true, static_cast<unsigned>(code), 0, 0);
        } else {
            m_pendingKeys.push_back({code, start, hold});
        }
    };

    for (int mod : modifiers)
        schedule(mod);
    schedule(retroKeyCode);
    return KeyPress{start, releaseFrame, queued};
}

bool AmigaEmulator::IsInjectedKeyHeld(int retroKeyCode) const
{
    return m_injectedKeys.count(retroKeyCode) > 0;
}

void AmigaEmulator::ClearInjectedKeys()
{
    for (const auto& [code, frame] : m_injectedKeys) {
        if (m_keyboardCallback)
            m_keyboardCallback(false, static_cast<unsigned>(code), 0, 0);
    }
    m_injectedKeys.clear();
    m_pendingKeys.clear();
}

// Per-frame injected-key housekeeping: start any press whose frame has arrived, then
// release any hold that has expired. Called once per emulated frame from RunOneRetroFrame.
void AmigaEmulator::ClearExpiredInjectedKeys()
{
    for (auto it = m_pendingKeys.begin(); it != m_pendingKeys.end();) {
        if (m_totalFrameCount >= it->pressFrame) {
            auto& frame = m_injectedKeys[it->code];
            frame = std::max(frame, m_totalFrameCount + it->durationFrames);
            if (m_keyboardCallback)
                m_keyboardCallback(true, static_cast<unsigned>(it->code), 0, 0);
            it = m_pendingKeys.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_injectedKeys.begin(); it != m_injectedKeys.end();) {
        if (m_totalFrameCount >= it->second) {
            if (m_keyboardCallback)
                m_keyboardCallback(false, static_cast<unsigned>(it->first), 0, 0);
            it = m_injectedKeys.erase(it);
        } else {
            ++it;
        }
    }
}

void AmigaEmulator::SetInjectedJoypadButton(int port, int retroPadId, bool down)
{
    if (port < 0 || port >= kJoypadPorts || retroPadId < 0 || retroPadId >= 16)
        return;
    m_injectedJoyButtons[port][retroPadId] = down;
}

void AmigaEmulator::ClearInjectedJoypad(int port)
{
    if (port < 0 || port >= kJoypadPorts)
        return;
    for (bool& b : m_injectedJoyButtons[port])
        b = false;
}

uint32_t AmigaEmulator::GetInjectedJoypadMask(int port) const
{
    if (port < 0 || port >= kJoypadPorts)
        return 0;
    uint32_t mask = 0;
    for (int i = 0; i < 16; ++i) {
        if (m_injectedJoyButtons[port][i])
            mask |= (1u << i);
    }
    return mask;
}

bool AmigaEmulator::IsJoypadPortActive(int port) const
{
    return port >= 0 && port < kJoypadPorts && m_joypadPortActive[port];
}

bool AmigaEmulator::EnsureJoypadPortDevice(int port)
{
    if (port < 0 || port >= kJoypadPorts)
        return false;
    if (m_joypadPortActive[port])
        return true;
    if (!m_coreLoaded || !m_retro_set_controller_port_device)
        return false;

    // Port 0 mirrors SetControllerType()'s device selection (a CD32 pad is still a
    // joypad as far as injection is concerned); port 1 is always a plain joystick,
    // because the Amiga's port 0 has no CD32 pad variant worth emulating here.
    unsigned device = RETRO_DEVICE_JOYPAD;
    if (port == 0 && m_controllerType == ControllerType::CD32Controller) {
        device =
            (m_cd32DeviceId != 0) ? m_cd32DeviceId : RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0);
    }
    core::Logger::Info("Emulator: Registering joypad on libretro port {} (device {})", port,
                       device);
    m_retro_set_controller_port_device(static_cast<unsigned>(port), device);
    m_joypadPortActive[port] = true;
    return true;
}

void AmigaEmulator::SetKeyState(RetroKey key, bool down)
{
    if (down) {
        m_pressedKeys.insert(static_cast<int>(key));
    } else {
        m_pressedKeys.erase(static_cast<int>(key));
    }
}

void AmigaEmulator::SendKeyEvent(bool down, RetroKey key)
{
    SetKeyState(key, down);
    // Host input is ignored while a recorded frame is being replayed - accepting it would
    // make the replayed frame diverge from the one that was recorded.
    if (m_replayActive)
        return;
    if (m_inputCaptureEnabled)
        m_pendingKeyEvents.push_back({static_cast<uint16_t>(key), down});
    if (m_keyboardCallback) {
        m_keyboardCallback(down, static_cast<unsigned>(key), 0, 0);
    }
}

void AmigaEmulator::SendInput(int port, int device, int /*index*/, int id, int16_t value)
{
    if (port == 0 && device == RETRO_DEVICE_JOYPAD && id >= 0 && id < 16) {
        m_joypadButtons[id] = (value != 0);
    }
}

void AmigaEmulator::SetControllerType(ControllerType type)
{
    m_controllerType = type;
    // Signal PuAE to re-read core options (puae_joyport will return the new type)
    m_variablesUpdated = true;

    if (m_coreLoaded && m_retro_set_controller_port_device) {
        unsigned device;
        if (type == ControllerType::CD32Controller) {
            // Use the device ID captured from SET_CONTROLLER_INFO if available,
            // otherwise fall back to RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0) = 257
            device = (m_cd32DeviceId != 0) ? m_cd32DeviceId
                                           : RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0);
        } else {
            device = RETRO_DEVICE_JOYPAD;
        }
        core::Logger::Info("Emulator: Setting controller port 0 to device {} (type: {})", device,
                           (type == ControllerType::CD32Controller) ? "CD32" : "Joystick");
        m_retro_set_controller_port_device(0, device);
    }
}

void AmigaEmulator::SetKeyboardJoystickEnabled(bool enabled)
{
    m_keyboardJoystickEnabled = enabled;
    if (!enabled) {
        std::memset(m_kbJoyButtons, 0, sizeof(m_kbJoyButtons));
    }
}

void AmigaEmulator::SetKeyboardJoyButton(int retroId, bool down)
{
    if (retroId >= 0 && retroId < 16) {
        m_kbJoyButtons[retroId] = down;
    }
}

void AmigaEmulator::PollGamepad()
{
    if (!s_instance || !m_retro_set_controller_port_device)
        return;

    // Clear all buttons first
    std::memset(m_joypadButtons, 0, sizeof(m_joypadButtons));

    // Keyboard joystick: merge in the keyboard-driven button state. Done up
    // front so a connected physical pad can still contribute below.
    if (m_keyboardJoystickEnabled) {
        for (int i = 0; i < 16; ++i) {
            if (m_kbJoyButtons[i])
                m_joypadButtons[i] = true;
        }
    }

    for (int joyId : m_connectedJoystickIds) {
        GLFWgamepadstate gpState;
        if (glfwGetGamepadState(joyId, &gpState)) {
            // Check for activity to handle type switching
            bool active = false;
            for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; b++) {
                if (gpState.buttons[b] == GLFW_PRESS) {
                    active = true;
                    break;
                }
            }
            if (!active) {
                for (int a = 0; a <= GLFW_GAMEPAD_AXIS_LAST; a++) {
                    if (std::abs(gpState.axes[a]) > 0.5f) {
                        active = true;
                        break;
                    }
                }
            }

            if (active) {
                const char* name = glfwGetJoystickName(joyId);
                if (name) {
                    std::string controllerName = name;

                    ControllerType preferredType = ControllerType::StandardJoystick;
                    auto it = m_controllerConfigs.find(controllerName);
                    if (it != m_controllerConfigs.end()) {
                        preferredType = it->second;
                    }

                    if (m_controllerType != preferredType) {
                        core::Logger::Info(
                            "Emulator: Input active from '{}'. Switching to {}", controllerName,
                            (preferredType == ControllerType::CD32Controller) ? "CD32"
                                                                              : "Joystick");
                        SetControllerType(preferredType);
                    }
                }
            }

            auto mapButton = [&](int retroId, int glfwId) {
                if (gpState.buttons[glfwId] == GLFW_PRESS)
                    m_joypadButtons[retroId] = true;
            };

            mapButton(RETRO_DEVICE_ID_JOYPAD_UP, GLFW_GAMEPAD_BUTTON_DPAD_UP);
            mapButton(RETRO_DEVICE_ID_JOYPAD_DOWN, GLFW_GAMEPAD_BUTTON_DPAD_DOWN);
            mapButton(RETRO_DEVICE_ID_JOYPAD_LEFT, GLFW_GAMEPAD_BUTTON_DPAD_LEFT);
            mapButton(RETRO_DEVICE_ID_JOYPAD_RIGHT, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT);

            if (gpState.axes[GLFW_GAMEPAD_AXIS_LEFT_X] < -0.5f)
                m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_LEFT] = true;
            if (gpState.axes[GLFW_GAMEPAD_AXIS_LEFT_X] > 0.5f)
                m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_RIGHT] = true;
            if (gpState.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] < -0.5f)
                m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_UP] = true;
            if (gpState.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] > 0.5f)
                m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_DOWN] = true;

            mapButton(RETRO_DEVICE_ID_JOYPAD_B, GLFW_GAMEPAD_BUTTON_A);
            mapButton(RETRO_DEVICE_ID_JOYPAD_A, GLFW_GAMEPAD_BUTTON_B);
            mapButton(RETRO_DEVICE_ID_JOYPAD_Y, GLFW_GAMEPAD_BUTTON_X);
            mapButton(RETRO_DEVICE_ID_JOYPAD_X, GLFW_GAMEPAD_BUTTON_Y);
            mapButton(RETRO_DEVICE_ID_JOYPAD_L, GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);
            mapButton(RETRO_DEVICE_ID_JOYPAD_R, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);
            mapButton(RETRO_DEVICE_ID_JOYPAD_L2, GLFW_GAMEPAD_BUTTON_LEFT_THUMB);
            mapButton(RETRO_DEVICE_ID_JOYPAD_R2, GLFW_GAMEPAD_BUTTON_RIGHT_THUMB);
            mapButton(RETRO_DEVICE_ID_JOYPAD_SELECT, GLFW_GAMEPAD_BUTTON_BACK);
            mapButton(RETRO_DEVICE_ID_JOYPAD_START, GLFW_GAMEPAD_BUTTON_START);
        }
    }
}

std::vector<AmigaEmulator::ConnectedController> AmigaEmulator::GetConnectedControllers() const
{
    std::vector<ConnectedController> controllers;
    controllers.reserve(m_connectedJoystickIds.size());
    for (int id : m_connectedJoystickIds) {
        ConnectedController c;
        c.id = id;
        const char* name = glfwGetJoystickName(id);
        c.name = name ? name : "Unknown Controller";

        auto it = m_controllerConfigs.find(c.name);
        if (it != m_controllerConfigs.end()) {
            c.type = it->second;
        } else {
            c.type = ControllerType::StandardJoystick;
        }
        controllers.push_back(c);
    }
    return controllers;
}

void AmigaEmulator::RefreshConnectedControllers()
{
    m_connectedJoystickIds.clear();
    for (int id = GLFW_JOYSTICK_1; id <= GLFW_JOYSTICK_LAST; ++id) {
        if (glfwJoystickPresent(id))
            m_connectedJoystickIds.push_back(id);
    }
}

void AmigaEmulator::SetControllerType(const std::string& controllerName, ControllerType type)
{
    m_controllerConfigs[controllerName] = type;
}

/*
    // Try gamepad API first (has standard mapping)
    GLFWgamepadstate gpState;
    if (glfwGetGamepadState(joyId, &gpState)) {
        // D-pad
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_UP] = gpState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP];
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_DOWN] =
            gpState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN];
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_LEFT] =
            gpState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT];
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_RIGHT] =
            gpState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT];

        // Also use left analog stick as d-pad (threshold 0.5)
        if (gpState.axes[GLFW_GAMEPAD_AXIS_LEFT_X] < -0.5f)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_LEFT] = true;
        if (gpState.axes[GLFW_GAMEPAD_AXIS_LEFT_X] > 0.5f)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_RIGHT] = true;
        if (gpState.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] < -0.5f)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_UP] = true;
        if (gpState.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] > 0.5f)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_DOWN] = true;

        // Face buttons: map Xbox/PS layout to libretro
        // GLFW A (south) -> libretro B (fire 1 / CD32 Red)
        // GLFW B (east)  -> libretro A (fire 2 / CD32 Blue)
        // GLFW X (west)  -> libretro Y (CD32 Green)
        // GLFW Y (north) -> libretro X (CD32 Yellow)
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_B] = gpState.buttons[GLFW_GAMEPAD_BUTTON_A];
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_A] = gpState.buttons[GLFW_GAMEPAD_BUTTON_B];
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_Y] = gpState.buttons[GLFW_GAMEPAD_BUTTON_X];
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_X] = gpState.buttons[GLFW_GAMEPAD_BUTTON_Y];

        // Shoulder buttons (CD32 Fwd/Rwd)
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_L] =
            gpState.buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER];
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_R] =
            gpState.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER];

        // Start/Select (CD32 Play/Pause)
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_START] = gpState.buttons[GLFW_GAMEPAD_BUTTON_START];
        m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_SELECT] = gpState.buttons[GLFW_GAMEPAD_BUTTON_BACK];

        return;
    }

    // Fallback: raw joystick API for unrecognized controllers
    int axisCount = 0;
    const float* axes = glfwGetJoystickAxes(joyId, &axisCount);
    int btnCount = 0;
    const unsigned char* buttons = glfwGetJoystickButtons(joyId, &btnCount);

    if (axes && axisCount >= 2) {
        if (axes[0] < -0.5f)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_LEFT] = true;
        if (axes[0] > 0.5f)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_RIGHT] = true;
        if (axes[1] < -0.5f)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_UP] = true;
        if (axes[1] > 0.5f)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_DOWN] = true;
    }

    if (buttons) {
        // Map first two buttons as fire 1 and fire 2
        if (btnCount > 0)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_B] = buttons[0];
        if (btnCount > 1)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_A] = buttons[1];
        if (btnCount > 2)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_Y] = buttons[2];
        if (btnCount > 3)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_X] = buttons[3];
        if (btnCount > 4)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_L] = buttons[4];
        if (btnCount > 5)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_R] = buttons[5];
        if (btnCount > 6)
            m_joypadButtons[RETRO_DEVICE_ID_JOYPAD_START] = buttons[6];
        if (btnCount > 7)
    }
}
*/

// --- Audio ---

namespace {

// Single teardown implementations for the audio device and resampler — the class used to carry
// four hand-written copies of these cleanup sequences that had to stay in sync.
void DestroyAudioDevice(ma_device*& dev)
{
    if (!dev)
        return;
    ma_device_stop(dev);
    ma_device_uninit(dev);
    delete dev;
    dev = nullptr;
}

void DestroyAudioResampler(void*& rs)
{
    if (!rs)
        return;
    ma_resampler_uninit(static_cast<ma_resampler*>(rs), nullptr);
    delete static_cast<ma_resampler*>(rs);
    rs = nullptr;
}

} // namespace

// Temp disk images are named per emulator instance - but m_instanceId is a process-local
// counter, so a second Peekaboo process starts at 0 again and lands on the same filename.
// The core reads sectors out of that file lazily while the guest boots, so a collision does
// not fail loudly: the disk simply changes underneath a running machine. Folding the process
// id in makes the name unique across processes as well as within one.
//
// Found by the scenario harness: two concurrent CTest jobs both mounting a disk produced a
// mid-run divergence at frame 319 that reproduced nowhere else.
std::string AmigaEmulator::TempImageTag() const
{
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    return std::to_string(pid) + "_" + std::to_string(m_instanceId);
}

void AmigaEmulator::InitAudio()
{
    // Reloading a game does not change the host audio device. Tearing down and
    // recreating the CoreAudio device on every reload exhausts the macOS audio HAL
    // after a handful of cycles (e.g. flicking through boot-block previews crashed
    // the emulator on ~the 9th load). Reuse the existing device when the core sample
    // rate is unchanged; just flush stale samples from the ring buffer.
    uint32_t coreRate = static_cast<uint32_t>(m_coreSampleRate);
    // (a push output that was lost and never reopened is not reused: it is opened again below)
    if (m_audioInitialized && m_audioInitCoreRate == coreRate && m_pushActive &&
        m_pushOut->IsOpen()) {
        // Nothing queued belongs to the new session.
        m_pushOut->Flush();
        m_pushLastWrite = {};
        m_pushQueuedEma = m_audioCushion.PushSetpoint();
        return;
    }
    if (m_audioInitialized && m_audioInitCoreRate == coreRate && !m_pushActive) {
        // Stop the device before touching the ring. AudioDeviceCallback reads m_audioRing and
        // both positions on the audio thread, so clearing them underneath a running device is a
        // data race - and the two unpaired position stores let a callback observe the old read
        // position against a zeroed write position, which reads as a nearly full ring of stale
        // zeros. Stopping is cheap and, unlike uninit, does not touch the host audio HAL, so the
        // device reuse this path exists for is preserved.
        const bool wasStarted = m_audioDevice && ma_device_is_started(m_audioDevice);
        if (wasStarted)
            ma_device_stop(m_audioDevice);

        m_audioWritePos.store(0);
        m_audioReadPos.store(0);
        std::memset(m_audioRing, 0, sizeof(m_audioRing));
        // The ring was just emptied, so the cushion has to be rebuilt before output resumes.
        m_audioPrimed.store(false, std::memory_order_release);
        m_audioLastOutL = 0.0f;
        m_audioLastOutR = 0.0f;

        if (wasStarted && ma_device_start(m_audioDevice) != MA_SUCCESS) {
            core::Logger::Error("Emulator: Failed to restart emulator audio device after flush");
            ShutdownAudio();
            return;
        }
        return;
    }

    ShutdownAudio();

#ifdef _WIN32
    if (OpenPushOutput(coreRate))
        return;
#endif

    m_audioDevice = new ma_device;

    // Open at native device rate — miniaudio handles conversion from our callback rate
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 0; // Use native device rate
    config.dataCallback = AudioDeviceCallback;
    config.pUserData = this;
    // 10 ms periods. Whatever the device queues is latency against the picture, and the old
    // 1024-frame period measured a mean of 58 ms queued in WASAPI (3 x 1024 frames); 10 ms
    // periods measure 19.5 ms. Producer jitter is the ring's job, not the device's.
    config.periodSizeInMilliseconds = 10;

    if (ma_device_init(nullptr, &config, m_audioDevice) != MA_SUCCESS) {
        core::Logger::Error("Emulator: Failed to initialize emulator audio device");
        delete m_audioDevice;
        m_audioDevice = nullptr;
        return;
    }

    ma_device_set_master_volume(m_audioDevice, VolumeGain());
    m_deviceSampleRate = m_audioDevice->sampleRate;
    uint32_t coreSampleRate = static_cast<uint32_t>(m_coreSampleRate);

    // What the device queues behind the ring. WASAPI's low-latency shared mode hands back a
    // buffer that is not period x periods, so ask it directly there.
    m_audioDevicePeriodFrames = m_audioDevice->playback.internalPeriodSizeInFrames;
    m_audioDeviceBufferFrames = m_audioDevice->playback.internalPeriodSizeInFrames *
                                m_audioDevice->playback.internalPeriods;
#ifdef MA_SUPPORT_WASAPI
    if (m_audioDevice->pContext->backend == ma_backend_wasapi &&
        m_audioDevice->wasapi.actualBufferSizeInFramesPlayback != 0)
        m_audioDeviceBufferFrames = m_audioDevice->wasapi.actualBufferSizeInFramesPlayback;
#endif

    // Set up resampler if rates differ
    m_needsResampling = (m_deviceSampleRate != coreSampleRate);
    if (m_needsResampling) {
        auto* resampler = new ma_resampler;
        ma_resampler_config resamplerConfig = ma_resampler_config_init(
            ma_format_f32, 2, coreSampleRate, m_deviceSampleRate, ma_resample_algorithm_linear);
        if (ma_resampler_init(&resamplerConfig, nullptr, resampler) != MA_SUCCESS) {
            core::Logger::Error("Emulator: Failed to initialize audio resampler");
            delete resampler;
            m_needsResampling = false;
        } else {
            m_audioResampler = resampler;
        }
    }

    m_audioWritePos.store(0);
    m_audioReadPos.store(0);
    std::memset(m_audioRing, 0, sizeof(m_audioRing));

    // Cushion the ring by two emulated frames' worth of audio, and never less than 2048 frames
    // (~46 ms at 44.1 kHz). The core hands over a whole frame's audio at once (882 frames at PAL,
    // 484-1600 on a demo that reprograms Paula every tick), up to a host refresh late, while the
    // device takes 10 ms at a time, so the ring has to ride out a block plus a refresh of jitter.
    // Every frame of cushion is also latency against the picture: four frames here, behind
    // 1024-frame device periods, played music-synced demos ~6 fields late. m_audioCushion holds
    // the ring at this target instead of letting a catch-up burst leave it deeper for good.
    const double framesPerAmigaFrame =
        (m_targetFps > 1.0) ? (m_coreSampleRate / m_targetFps) : 882.0;
    m_audioTargetFrames = static_cast<uint32_t>(framesPerAmigaFrame * 2.0);
    m_audioTargetFrames =
        std::clamp<uint32_t>(m_audioTargetFrames, 2048, static_cast<uint32_t>(kAudioRingSize / 4));
    // The floor is judged over a second of output.
    m_audioCushion.Reset(m_audioTargetFrames, static_cast<uint32_t>(framesPerAmigaFrame),
                         m_audioDevicePeriodFrames, m_deviceSampleRate);
    m_audioPrimed.store(false, std::memory_order_release);
    m_audioLastOutL = 0.0f;
    m_audioLastOutR = 0.0f;

    if (ma_device_start(m_audioDevice) != MA_SUCCESS) {
        core::Logger::Error("Emulator: Failed to start emulator audio device");
        DestroyAudioResampler(m_audioResampler);
        DestroyAudioDevice(m_audioDevice);
        m_needsResampling = false;
        return;
    }

    m_audioInitialized = true;
    m_audioInitCoreRate = coreRate;
}

void AmigaEmulator::ShutdownAudio()
{
    if (!m_audioInitialized)
        return;

    if (m_pushOut) {
        m_pushOut->Close();
        m_pushOut.reset();
    }
    m_pushActive = false;
    DestroyAudioDevice(m_audioDevice);
    DestroyAudioResampler(m_audioResampler);

    m_needsResampling = false;
    m_audioInitialized = false;
}

bool AmigaEmulator::StartVideoRecording(const std::string& path, const VideoCaptureOptions& options,
                                        std::string& outError)
{
    outError.clear();
    if (!m_coreLoaded || !m_gameLoaded) {
        outError = "the emulator is not running";
        return false;
    }
    if (IsVideoRecording()) {
        outError = "a recording is already running";
        return false;
    }

    CaptureGeometry geometry;
    if (!GetCaptureGeometry(geometry)) {
        outError = "no frame has been rendered yet";
        return false;
    }
    // A composed film is the composer's picture, at its size and shape, made from the frame.
    const bool composed = static_cast<bool>(options.compose);
    if (composed && (options.composeWidth <= 0 || options.composeHeight <= 0)) {
        outError = "the composed picture has no size";
        return false;
    }
    const int width = composed ? options.composeWidth : geometry.width;
    const int height = composed ? options.composeHeight : geometry.height;
    const int scaleX = composed ? 1 : geometry.scaleX;
    const int scaleY = composed ? 1 : geometry.scaleY;
    const double aspect = composed ? static_cast<double>(width) / height : geometry.aspect;
    // A composed picture has square pixels: its own size is its shape, exactly.
    const int aspectNum = composed ? width : static_cast<int>(std::lround(aspect * 10000.0));
    const int aspectDen = composed ? height : 10000;
    // Round rather than truncate: a 49.92 Hz PAL machine is a 50 fps film, not a 49 fps one.
    const int fps = static_cast<int>(m_targetFps + 0.5);
    // The core's own rate and its native int16 payload, the same lossless stream the Core audio
    // tap records - not the device rate, which is resampled.
    const uint32_t sampleRate = static_cast<uint32_t>(m_coreSampleRate);

    const auto& info = core::GetVideoCaptureCodecInfo(options.codec);
    std::unique_ptr<core::VideoCaptureSink> sink;
    if (info.needsFfmpeg) {
        core::FfmpegRecorder::Config config;
        config.path = path;
        config.executable = options.ffmpegExecutable;
        config.codec = options.codec;
        config.quality = options.quality >= 0 ? options.quality : info.defaultQuality;
        config.width = width;
        config.height = height;
        config.scaleX = scaleX;
        config.scaleY = scaleY;
        config.aspectNum = aspectNum;
        config.aspectDen = aspectDen;
        config.fps = fps;
        config.sampleRate = sampleRate;
        auto recorder = std::make_unique<core::FfmpegRecorder>();
        if (!recorder->Begin(config, outError))
            return false;
        sink = std::move(recorder);
    } else {
        core::AviRecorder::Config config;
        config.path = path;
        config.codec = options.codec == core::VideoCaptureCodec::Zmbv
                           ? core::AviRecorder::VideoCodec::Zmbv
                           : core::AviRecorder::VideoCodec::Uncompressed;
        config.width = width;
        config.height = height;
        config.scaleX = scaleX;
        config.scaleY = scaleY;
        config.aspectNum = aspectNum;
        config.aspectDen = aspectDen;
        config.fps = fps;
        config.sampleRate = sampleRate;
        auto recorder = std::make_unique<core::AviRecorder>();
        if (!recorder->Begin(config, outError))
            return false;
        sink = std::move(recorder);
    }

    m_videoCaptureStatus.clear();
    m_captureFramePending = false;
    m_captureFrameScratch.clear(); // nothing to repeat until the first frame of this film
    m_captureFramesRepeated = 0;
    m_videoCapture = std::move(sink);
    m_videoCompose = options.compose;
    m_captureSourceWidth = width;
    m_captureSourceHeight = height;
    core::Logger::Info("Capture: recording {} {}x{} (from {}x{}, shown at {:.4f}){} at {} fps, {} "
                       "Hz audio to '{}'",
                       info.label, m_videoCapture->OutputWidth(), m_videoCapture->OutputHeight(),
                       width, height, aspect, composed ? ", composed" : "", fps, sampleRate,
                       m_videoCapture->Path());
    return true;
}

bool AmigaEmulator::GetCaptureGeometry(CaptureGeometry& out) const
{
    // The size the core last announced through VideoRefreshCallback. Not CaptureFrameRGBA:
    // that reads back the GL texture, so it answers nothing without a context. A film cannot
    // change size part-way through, so this is fixed for the whole recording and a later frame
    // of another size is dropped rather than stretched.
    out.width = static_cast<int>(m_width);
    out.height = static_cast<int>(m_height);
    if (out.width <= 0 || out.height <= 0)
        return false;
    // The shape the emulator window shows, which is what the GIF recorder stretches to as well:
    // the core's own aspect, PAL/NTSC pixel shape included. The frame is enlarged by whole
    // numbers towards it (720x287 becomes 720x574, each lores pixel a sharp 2x2 block; a
    // superhires 1440x287 becomes 1440x1148) and the exact value goes in the file for players
    // that honour it.
    out.aspect = m_aspectRatio > 0.0f ? static_cast<double>(m_aspectRatio) : 4.0 / 3.0;
    core::AviRecorder::ChooseScale(out.width, out.height, out.aspect, out.scaleX, out.scaleY);
    return true;
}

bool AmigaEmulator::GetVideoCaptureSize(int& outWidth, int& outHeight, int& sourceWidth,
                                        int& sourceHeight) const
{
    CaptureGeometry geometry;
    if (!m_coreLoaded || !m_gameLoaded || !GetCaptureGeometry(geometry)) {
        outWidth = outHeight = sourceWidth = sourceHeight = 0;
        return false;
    }
    outWidth = geometry.width * geometry.scaleX;
    outHeight = geometry.height * geometry.scaleY;
    sourceWidth = geometry.width;
    sourceHeight = geometry.height;
    return true;
}

bool AmigaEmulator::StopVideoRecording()
{
    if (!m_videoCapture)
        return false;
    EndVideoCapture({});
    return true;
}

void AmigaEmulator::EndVideoCapture(const std::string& reason)
{
    if (!m_videoCapture)
        return;
    m_videoCapture->End();
    m_captureFramePending = false;
    if (m_captureFramesRepeated)
        core::Logger::Info("Capture: {} of {} frames repeat one the core did not draw again",
                           m_captureFramesRepeated, m_videoCapture->FramesWritten());
    m_videoCompose = nullptr; // the composer's GL objects go with it, here on the main thread
    m_videoComposed.clear();
    m_videoComposed.shrink_to_fit();
    // The taps the composer asked for, off again unless a window still wants them
    SetAudioTraceForCapture(false);
    if (m_captureArmedDebugDma) {
        SetDebugDma(0);
        m_captureArmedDebugDma = false;
    }
    if (m_videoCapture->IsFinishing()) {
        // The result is not known until the mux is done; PollVideoCapture reports it.
        m_videoCaptureStatus.clear();
        m_finishingCaptures.push_back({std::move(m_videoCapture), reason});
        return;
    }
    const std::string result = m_videoCapture->ResultMessage();
    m_videoCaptureStatus = reason.empty() ? result : reason + " " + result;
    m_lastCapture = std::move(m_videoCapture);
}

bool AmigaEmulator::IsVideoFinishing() const
{
    return std::any_of(m_finishingCaptures.begin(), m_finishingCaptures.end(),
                       [](const FinishingCapture& f) { return f.sink->IsFinishing(); });
}

void AmigaEmulator::PollVideoCapture()
{
    for (auto it = m_finishingCaptures.begin(); it != m_finishingCaptures.end();) {
        if (it->sink->IsFinishing()) {
            ++it;
            continue;
        }
        const std::string result = it->sink->ResultMessage();
        m_videoCaptureStatus = it->reason.empty() ? result : it->reason + " " + result;
        m_lastCapture = std::move(it->sink); // its finishing thread is done
        it = m_finishingCaptures.erase(it);
    }
}

// The recording running now, else the one still being written out, else the last one to end.
const core::VideoCaptureSink* AmigaEmulator::LatestVideoCapture() const
{
    if (m_videoCapture)
        return m_videoCapture.get();
    if (!m_finishingCaptures.empty())
        return m_finishingCaptures.back().sink.get();
    return m_lastCapture.get();
}

uint32_t AmigaEmulator::GetVideoFramesRecorded() const
{
    const auto* sink = LatestVideoCapture();
    return sink ? sink->FramesWritten() : 0;
}

uint64_t AmigaEmulator::GetVideoBytesWritten() const
{
    const auto* sink = LatestVideoCapture();
    return sink ? sink->BytesWritten() : 0;
}

std::string AmigaEmulator::GetVideoPath() const
{
    const auto* sink = LatestVideoCapture();
    return sink ? sink->Path() : std::string();
}

bool AmigaEmulator::StartAudioRecording(AudioTap tap, const std::string& path, double maxSeconds,
                                        std::string& outError)
{
    core::AudioRecorder::Config config;
    config.path = path;
    config.channels = 2;
    config.maxSeconds = maxSeconds;

    if (tap == AudioTap::Core) {
        // The core's own rate, and its native int16 payload — a lossless copy of what PuAE made.
        config.sampleRate = static_cast<uint32_t>(m_coreSampleRate);
        config.float32 = false;
        return m_tapCore.Start(config, outError);
    }

    if (!m_audioInitialized) {
        outError = "no host audio device — the device tap has nothing to capture";
        return false;
    }
    // The device tap runs at whatever rate the sound card opened at, in float, so it is a lossless
    // copy of the buffer miniaudio receives.
    config.sampleRate = m_deviceSampleRate;
    config.float32 = true;
    return m_tapDevice.Start(config, outError);
}

void AmigaEmulator::StopAudioRecording(AudioTap tap)
{
    if (tap == AudioTap::Core)
        m_tapCore.Stop();
    else
        m_tapDevice.Stop();
}

void AmigaEmulator::StopAllAudioRecording()
{
    m_tapCore.Stop();
    m_tapDevice.Stop();
}

bool AmigaEmulator::IsAudioRecording(AudioTap tap) const
{
    return (tap == AudioTap::Core) ? m_tapCore.IsRecording() : m_tapDevice.IsRecording();
}

core::AudioRecorder::Stats AmigaEmulator::GetAudioRecordingStats(AudioTap tap) const
{
    return (tap == AudioTap::Core) ? m_tapCore.GetStats() : m_tapDevice.GetStats();
}

void AmigaEmulator::DrainAudioRecorders()
{
    m_tapCore.Drain();
    m_tapDevice.Drain();
}

AmigaEmulator::AudioStats AmigaEmulator::GetAudioStats() const
{
    AudioStats stats;
    stats.initialized = m_audioInitialized;
    stats.muted = m_isMuted;
    stats.resampling = m_needsResampling;
    stats.backend = m_pushActive ? "wasapi-push" : (m_audioInitialized ? "miniaudio" : "");
    stats.coreSampleRate = m_coreSampleRate;
    stats.deviceSampleRate = m_deviceSampleRate;
    stats.targetFps = m_targetFps;

    stats.framesProduced = m_audioFramesProduced.load(std::memory_order_relaxed);
    stats.framesDroppedOverrun = m_audioFramesDroppedOverrun.load(std::memory_order_relaxed);
    stats.framesConsumed = m_audioFramesConsumed.load(std::memory_order_relaxed);
    stats.framesHeld = m_audioFramesHeld.load(std::memory_order_relaxed);
    stats.framesDroppedResampler = m_audioFramesDroppedResampler.load(std::memory_order_relaxed);
    stats.deviceCallbacks = m_audioDeviceCallbacks.load(std::memory_order_relaxed);
    stats.underrunCallbacks = m_audioUnderrunCallbacks.load(std::memory_order_relaxed);

    stats.ringSize = static_cast<uint32_t>(kAudioRingSize);
    stats.ringLevel = m_audioRingLevel.load(std::memory_order_relaxed);
    const uint32_t ringMin = m_audioRingLevelMin.load(std::memory_order_relaxed);
    stats.ringLevelMin = (ringMin == 0xFFFFFFFFu) ? 0 : ringMin;
    stats.ringLevelMax = m_audioRingLevelMax.load(std::memory_order_relaxed);
    // Push output adapts its target; the callback device's is fixed.
    stats.ringTarget = m_pushActive ? m_audioCushion.Target() : m_audioTargetFrames;
    const uint64_t levelSamples = m_audioRingLevelSamples.load(std::memory_order_relaxed);
    stats.ringLevelMean =
        levelSamples ? static_cast<double>(m_audioRingLevelTotal.load(std::memory_order_relaxed)) /
                           static_cast<double>(levelSamples)
                     : 0.0;
    stats.framesTrimmed = m_audioFramesTrimmed.load(std::memory_order_relaxed);
    stats.trimEvents = m_audioTrimEvents.load(std::memory_order_relaxed);
    const uint32_t minPlaying = m_audioRingLevelMinPlaying.load(std::memory_order_relaxed);
    stats.ringLevelMinPlaying = (minPlaying == 0xFFFFFFFFu) ? 0 : minPlaying;
    stats.pacePpm = (m_audioPace - 1.0) * 1e6;
    for (size_t i = 0; i < stats.produceGapHist.size(); ++i)
        stats.produceGapHist[i] = m_audioProduceGapHist[i].load(std::memory_order_relaxed);
    stats.devicePeriodFrames = m_audioDevicePeriodFrames;
    stats.deviceBufferFrames = m_audioDeviceBufferFrames;
    // Push output has no second queue: what is waiting in the device buffer is all of it.
    if (levelSamples && m_coreSampleRate > 0.0 && m_pushActive)
        stats.latencyMs = stats.ringLevelMean * 1000.0 / m_coreSampleRate;
    else if (levelSamples && m_coreSampleRate > 0.0 && m_deviceSampleRate > 0)
        stats.latencyMs = stats.ringLevelMean * 1000.0 / m_coreSampleRate +
                          m_audioDeviceBufferFrames * 1000.0 / m_deviceSampleRate;
    if (m_pushActive)
        stats.ringSize = m_audioDeviceBufferFrames;

    stats.lastBlockFrames = m_audioLastBlockFrames.load(std::memory_order_relaxed);
    const uint32_t blockMin = m_audioMinBlockFrames.load(std::memory_order_relaxed);
    stats.minBlockFrames = (blockMin == 0xFFFFFFFFu) ? 0 : blockMin;
    stats.maxBlockFrames = m_audioMaxBlockFrames.load(std::memory_order_relaxed);
    stats.blockCount = m_audioBlockCount.load(std::memory_order_relaxed);
    const uint64_t blockTotal = m_audioBlockFrameTotal.load(std::memory_order_relaxed);
    stats.meanBlockFrames =
        stats.blockCount ? static_cast<double>(blockTotal) / static_cast<double>(stats.blockCount)
                         : 0.0;

    stats.timeDiscardEvents = m_audioTimeDiscardEvents.load(std::memory_order_relaxed);
    stats.timeDiscardedFrames = m_audioTimeDiscardedFrames.load(std::memory_order_relaxed);
    stats.timeDiscardElapsedClamp = m_audioTimeDiscardBySite[0].load(std::memory_order_relaxed);
    stats.timeDiscardAccumulatorClamp = m_audioTimeDiscardBySite[1].load(std::memory_order_relaxed);
    stats.timeDiscardFrameStopCap = m_audioTimeDiscardBySite[2].load(std::memory_order_relaxed);

    const uint64_t gapCount = m_audioProduceGapCount.load(std::memory_order_relaxed);
    const uint64_t gapTotalUs = m_audioProduceGapTotalMs.load(std::memory_order_relaxed);
    stats.produceGapMeanMs =
        gapCount ? static_cast<double>(gapTotalUs) / static_cast<double>(gapCount) / 1000.0 : 0.0;
    stats.produceGapMaxMs =
        static_cast<double>(m_audioProduceGapMaxUs.load(std::memory_order_relaxed)) / 1000.0;
    stats.produceStalls = m_audioProduceStalls.load(std::memory_order_relaxed);

    const uint64_t updateCount = m_audioUpdateCount.load(std::memory_order_relaxed);
    const uint64_t updateTotalUs = m_audioUpdateTotalUs.load(std::memory_order_relaxed);
    stats.updateMeanMs =
        updateCount ? static_cast<double>(updateTotalUs) / static_cast<double>(updateCount) / 1000.0
                    : 0.0;
    stats.updateMaxMs =
        static_cast<double>(m_audioUpdateMaxUs.load(std::memory_order_relaxed)) / 1000.0;
    stats.updateSlow = m_audioUpdateSlow.load(std::memory_order_relaxed);

    stats.retroRunCalls = m_diagRetroRunCalls.load(std::memory_order_relaxed);
    stats.frameBoundaries = m_diagFrameBoundaries.load(std::memory_order_relaxed);
    const uint64_t cckCount = m_diagFrameCckCount.load(std::memory_order_relaxed);
    stats.frameCckMean =
        cckCount ? static_cast<double>(m_diagFrameCckTotal.load(std::memory_order_relaxed)) /
                       static_cast<double>(cckCount)
                 : 0.0;
    const uint32_t cckMin = m_diagFrameCckMin.load(std::memory_order_relaxed);
    stats.frameCckMin = (cckMin == 0xFFFFFFFFu) ? 0 : cckMin;
    stats.frameCckMax = m_diagFrameCckMax.load(std::memory_order_relaxed);
    return stats;
}

void AmigaEmulator::ResetAudioStats()
{
    m_audioFramesProduced.store(0, std::memory_order_relaxed);
    m_audioFramesDroppedOverrun.store(0, std::memory_order_relaxed);
    m_audioFramesConsumed.store(0, std::memory_order_relaxed);
    m_audioFramesHeld.store(0, std::memory_order_relaxed);
    m_audioFramesDroppedResampler.store(0, std::memory_order_relaxed);
    m_audioDeviceCallbacks.store(0, std::memory_order_relaxed);
    m_audioUnderrunCallbacks.store(0, std::memory_order_relaxed);
    m_audioRingLevelMin.store(0xFFFFFFFFu, std::memory_order_relaxed);
    m_audioRingLevelMax.store(0, std::memory_order_relaxed);
    m_audioRingLevelTotal.store(0, std::memory_order_relaxed);
    m_audioRingLevelSamples.store(0, std::memory_order_relaxed);
    m_audioFramesTrimmed.store(0, std::memory_order_relaxed);
    m_audioTrimEvents.store(0, std::memory_order_relaxed);
    m_audioRingLevelMinPlaying.store(0xFFFFFFFFu, std::memory_order_relaxed);
    for (auto& bucket : m_audioProduceGapHist)
        bucket.store(0, std::memory_order_relaxed);
    m_audioLastBlockFrames.store(0, std::memory_order_relaxed);
    m_audioMinBlockFrames.store(0xFFFFFFFFu, std::memory_order_relaxed);
    m_audioMaxBlockFrames.store(0, std::memory_order_relaxed);
    m_audioBlockCount.store(0, std::memory_order_relaxed);
    m_audioBlockFrameTotal.store(0, std::memory_order_relaxed);
    m_audioTimeDiscardEvents.store(0, std::memory_order_relaxed);
    m_audioTimeDiscardedFrames.store(0, std::memory_order_relaxed);
    for (auto& site : m_audioTimeDiscardBySite)
        site.store(0, std::memory_order_relaxed);
    m_audioProduceGapCount.store(0, std::memory_order_relaxed);
    m_audioProduceGapTotalMs.store(0, std::memory_order_relaxed);
    m_audioProduceGapMaxUs.store(0, std::memory_order_relaxed);
    m_audioProduceStalls.store(0, std::memory_order_relaxed);
    m_audioUpdateMaxUs.store(0, std::memory_order_relaxed);
    m_audioUpdateTotalUs.store(0, std::memory_order_relaxed);
    m_audioUpdateCount.store(0, std::memory_order_relaxed);
    m_audioUpdateSlow.store(0, std::memory_order_relaxed);
    m_diagRetroRunCalls.store(0, std::memory_order_relaxed);
    m_diagFrameBoundaries.store(0, std::memory_order_relaxed);
    m_diagFrameCckTotal.store(0, std::memory_order_relaxed);
    m_diagFrameCckCount.store(0, std::memory_order_relaxed);
    m_diagFrameCckMin.store(0xFFFFFFFFu, std::memory_order_relaxed);
    m_diagFrameCckMax.store(0, std::memory_order_relaxed);
    m_audioHaveLastProduce = false;
}

// --- Paula channel telemetry ---------------------------------------------------------------
// Everything above this line measures the host audio path. Everything below reads the chipset.

void AmigaEmulator::SetAudioTraceEnabled(bool enabled)
{
    m_audioTraceEnabled = enabled;
    if (AudioTraceWanted()) {
        for (auto& buf : m_audioTrace)
            buf.resize(kMaxAudioSamples);
    }
    SyncAudioTraceEnabled();
    if (!AudioTraceWanted()) {
        for (int i = 0; i < kAudioChannels; ++i)
            m_audioTraceCount[i] = 0;
    }
}

void AmigaEmulator::SetAudioTraceForCapture(bool enabled)
{
    m_audioTraceForCapture = enabled;
    SetAudioTraceEnabled(m_audioTraceEnabled);
}

// The tap lives in the core, so arming it before a core is loaded can only record the intent.
// Reconciling here rather than at the call site means every path that brings a core up - first
// load, reload, session restore - completes a pending request without the caller knowing.
void AmigaEmulator::SyncAudioTraceEnabled()
{
    if (!m_adm_set_audio_trace_enabled)
        return;
    if (m_audioTraceEnabledCore == AudioTraceWanted())
        return;
    m_adm_set_audio_trace_enabled(AudioTraceWanted() ? 1 : 0);
    m_audioTraceEnabledCore = AudioTraceWanted();
}

void AmigaEmulator::ClearAudioTrace()
{
    if (m_adm_clear_audio_trace)
        m_adm_clear_audio_trace();
    for (int i = 0; i < kAudioChannels; ++i)
        m_audioTraceCount[i] = 0;
}

const AmigaEmulator::AudioSampleEvent* AmigaEmulator::GetAudioTrace(int channel,
                                                                    int& outCount) const
{
    outCount = 0;
    if (channel < 0 || channel >= kAudioChannels)
        return nullptr;
    outCount = m_audioTraceCount[channel];
    return outCount > 0 ? m_audioTrace[channel].data() : nullptr;
}

void AmigaEmulator::SetAudioChannelMask(uint8_t mask)
{
    if (m_adm_set_audio_channel_mask)
        m_adm_set_audio_channel_mask(m_forceSilent ? 0 : static_cast<uint8_t>(mask & 0x0F));
}

void AmigaEmulator::SetForceSilent(bool on)
{
    m_forceSilent = on;
    if (on) {
        m_isMuted = true;
        if (m_adm_set_audio_channel_mask)
            m_adm_set_audio_channel_mask(0);
    }
}

uint8_t AmigaEmulator::GetAudioChannelMask() const
{
    // All four voices audible is the hardware default, and the right answer for a core that
    // cannot report — never tell the UI everything is muted just because the export is missing.
    if (!m_adm_get_audio_channel_mask)
        return 0x0F;
    return static_cast<uint8_t>(m_adm_get_audio_channel_mask() & 0x0F);
}

AmigaEmulator::AudioFilterState AmigaEmulator::GetAudioFilterState() const
{
    AudioFilterState state;
    if (!m_adm_get_audio_filter_state)
        return state;
    const int raw = m_adm_get_audio_filter_state();
    state.ledFilterOn = (raw & 1) != 0;
    state.model = raw >> 1;
    return state;
}

void AmigaEmulator::WriteAudioSamples(const int16_t* data, size_t frames)
{
    // Accumulated across every block the core delivers in a frame and closed at the frame
    // boundary. Like the core tap below, deliberately outside any audio-device guard: a
    // headless run has no device and still has to produce this.
    if (m_frameDigestEnabled)
        m_audioDigestPending.AddSamples(data, frames);

    // Tap B — exactly what the core synthesised, captured before Peekaboo's ring buffer can
    // perturb it. Deliberately outside the m_audioInitialized guard: a headless run has no audio
    // device, and that lane still needs the core's output.
    // A/V capture takes the same stream as the core tap: what PuAE synthesised, before the ring
    // buffer. Held by the recorder until the frame boundary closes it, so each audio chunk sits
    // beside the picture it belongs to.
    if (IsVideoRecording())
        m_videoCapture->WriteAudio(data, frames);

    if (m_tapCore.IsRecording()) {
        core::AudioRecorder::BlockInfo block;
        // Total frames the core has delivered so far — the tap's own timeline, independent of
        // whether a host audio device exists at all.
        block.frameIndex = m_audioBlockFrameTotal.load(std::memory_order_relaxed);
        block.frames = static_cast<uint32_t>(frames);
        m_tapCore.PushInt16(data, frames);
        m_tapCore.PushBlock(block);
    }

    // Producer-jitter statistics: how many frames each retro_run() handed us. Recorded even when
    // there is no device, since this is the metric that distinguishes a well-behaved guest from
    // one that delivers uneven blocks.
    if (frames > 0) {
        // Wall-clock gap since the previous delivery. The device drains ~1024 frames every ~23 ms,
        // so a gap much longer than that is the main thread stalling, which is a different fault
        // from the core simply delivering uneven block sizes.
        const auto now = std::chrono::steady_clock::now();
        if (m_audioHaveLastProduce) {
            const double gapMs =
                std::chrono::duration<double, std::milli>(now - m_audioLastProduceTime).count();
            m_audioProduceGapCount.fetch_add(1, std::memory_order_relaxed);
            m_audioProduceGapTotalMs.fetch_add(static_cast<uint64_t>(gapMs * 1000.0),
                                               std::memory_order_relaxed);
            const uint32_t gapUs = static_cast<uint32_t>(gapMs * 1000.0);
            uint32_t prevGap = m_audioProduceGapMaxUs.load(std::memory_order_relaxed);
            while (gapUs > prevGap && !m_audioProduceGapMaxUs.compare_exchange_weak(
                                          prevGap, gapUs, std::memory_order_relaxed)) {
            }
            // A gap this long cannot be covered by the priming cushion, so it is a direct
            // explanation for an underrun rather than background jitter.
            if (gapMs > 40.0)
                m_audioProduceStalls.fetch_add(1, std::memory_order_relaxed);
            size_t bucket = 0;
            while (bucket < kProduceGapEdgesMs.size() && gapMs >= kProduceGapEdgesMs[bucket])
                ++bucket;
            m_audioProduceGapHist[bucket].fetch_add(1, std::memory_order_relaxed);
        }
        m_audioLastProduceTime = now;
        m_audioHaveLastProduce = true;

        const uint32_t blockFrames = static_cast<uint32_t>(frames);
        m_audioLastBlockFrames.store(blockFrames, std::memory_order_relaxed);
        m_audioBlockCount.fetch_add(1, std::memory_order_relaxed);
        m_audioBlockFrameTotal.fetch_add(frames, std::memory_order_relaxed);
        uint32_t prevMin = m_audioMinBlockFrames.load(std::memory_order_relaxed);
        while (blockFrames < prevMin && !m_audioMinBlockFrames.compare_exchange_weak(
                                            prevMin, blockFrames, std::memory_order_relaxed)) {
        }
        uint32_t prevMax = m_audioMaxBlockFrames.load(std::memory_order_relaxed);
        while (blockFrames > prevMax && !m_audioMaxBlockFrames.compare_exchange_weak(
                                            prevMax, blockFrames, std::memory_order_relaxed)) {
        }
    }

    if (!m_audioInitialized)
        return;

    if (m_pushActive) {
        PushAudio(data, frames);
        return;
    }

    constexpr float scale = 1.0f / 32768.0f;

    size_t writePos = m_audioWritePos.load(std::memory_order_relaxed);
    size_t readPos = m_audioReadPos.load(std::memory_order_acquire);

    size_t written = 0;
    for (; written < frames; written++) {
        size_t nextWrite = (writePos + 1) % kAudioRingSize;
        if (nextWrite == readPos)
            break;
        m_audioRing[writePos * 2] = static_cast<float>(data[written * 2]) * scale;
        m_audioRing[writePos * 2 + 1] = static_cast<float>(data[written * 2 + 1]) * scale;
        writePos = nextWrite;
    }

    m_audioWritePos.store(writePos, std::memory_order_release);

    m_audioFramesProduced.fetch_add(written, std::memory_order_relaxed);
    if (written < frames)
        m_audioFramesDroppedOverrun.fetch_add(frames - written, std::memory_order_relaxed);
}

bool AmigaEmulator::OpenPushOutput(uint32_t coreRate)
{
    auto out = std::make_unique<core::WasapiPushOutput>();
    std::string error;
    // 200 ms of room: far more than the cushion ever holds, so a catch-up burst is clipped by the
    // cushion's cap rather than by the device.
    if (!out->Open(coreRate, 200, error)) {
        core::Logger::Warn("Emulator: WASAPI push output unavailable ({}); using miniaudio", error);
        return false;
    }
    m_pushOut = std::move(out);
    m_pushActive = true;
    m_needsResampling = false;
    m_deviceSampleRate = m_pushOut->DeviceSampleRate() ? m_pushOut->DeviceSampleRate() : coreRate;
    m_audioDeviceBufferFrames = m_pushOut->BufferFrames();
    m_audioDevicePeriodFrames = m_pushOut->EnginePeriodFrames();
    ConfigurePushCushion(coreRate);
    m_audioInitialized = true;
    m_audioInitCoreRate = coreRate;
    core::Logger::Info("Emulator: audio via WASAPI push - buffer {} frames, engine period {}, "
                       "cushion {} frames",
                       m_audioDeviceBufferFrames, m_audioDevicePeriodFrames, m_audioTargetFrames);
    return true;
}

void AmigaEmulator::ConfigurePushCushion(uint32_t coreRate)
{
    // The queue has to hold a block plus one engine pass: blocks land a whole emulated frame at a
    // time, up to a host refresh late, and the engine takes a period at a time. Measured with
    // 882-frame blocks on a 60 Hz loop and a 10 ms engine: 1323 never starved and kept 10 ms
    // spare, 882 starved on a third of the blocks.
    const double block = (m_targetFps > 1.0) ? (m_coreSampleRate / m_targetFps) : 882.0;
    const uint32_t period = m_audioDevicePeriodFrames ? m_audioDevicePeriodFrames : coreRate / 100;
    m_audioTargetFrames = static_cast<uint32_t>(block) + period;
    m_audioCushion.Reset(m_audioTargetFrames, static_cast<uint32_t>(block), period, coreRate);
    // Up to 60 ms (two blocks and two periods at PAL) on a host loop that starves it, back down
    // half a period per 30 s without a starve.
    m_audioCushion.SetPushRange(2 * static_cast<uint32_t>(block) + 2 * period, 30 * coreRate);
    m_pushQueuedEma = m_audioCushion.PushSetpoint();
    m_pushLastWrite = {};
}

void AmigaEmulator::ReopenPushOutput()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - m_pushLastReopen < std::chrono::seconds(1))
        return;
    m_pushLastReopen = now;
    std::string error;
    if (!m_pushOut->Open(m_audioInitCoreRate, 200, error)) {
        core::Logger::Warn("Emulator: WASAPI push output lost ({}), retrying", error);
        return;
    }
    m_deviceSampleRate =
        m_pushOut->DeviceSampleRate() ? m_pushOut->DeviceSampleRate() : m_audioInitCoreRate;
    m_audioDeviceBufferFrames = m_pushOut->BufferFrames();
    m_audioDevicePeriodFrames = m_pushOut->EnginePeriodFrames();
    // another device can have another engine period: the cushion is sized for it again
    ConfigurePushCushion(m_audioInitCoreRate);
    core::Logger::Info("Emulator: WASAPI push output reopened on the default device");
}

bool AmigaEmulator::PushPlaying(std::chrono::steady_clock::time_point now) const
{
    return m_pushLastWrite != std::chrono::steady_clock::time_point{} &&
           now - m_pushLastWrite < std::chrono::milliseconds(200);
}

void AmigaEmulator::PushAudio(const int16_t* data, size_t frames)
{
    // Muted (turbo mutes too): queue nothing, and the device plays silence.
    if (m_isMuted || frames == 0)
        return;
    // A reopen that failed (no endpoint yet, say) leaves the output closed: try again, at most
    // once a second, rather than going quiet for the rest of the session
    if (!m_pushOut->IsOpen()) {
        ReopenPushOutput();
        return;
    }
    // The volume, before anything else here reads the block - the crossfade, the tail's fade
    // and Tap C all hear what the sound card does
    if (const float gain = VolumeGain(); gain < 1.0f) {
        m_pushGainScratch.resize(frames * 2);
        for (size_t i = 0; i < frames * 2; ++i)
            m_pushGainScratch[i] = static_cast<int16_t>(data[i] * gain);
        data = m_pushGainScratch.data();
    }
    uint32_t queued = 0;
    if (!m_pushOut->Queued(queued)) {
        ReopenPushOutput();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    // An empty queue straight after a write is a starve; after a pause or a stop it is a start.
    const bool playing = PushPlaying(now);
    m_pushLastWrite = now;

    m_audioRingLevel.store(queued, std::memory_order_relaxed);
    if (playing) {
        if (queued > m_audioRingLevelMax.load(std::memory_order_relaxed))
            m_audioRingLevelMax.store(queued, std::memory_order_relaxed);
        if (queued < m_audioRingLevelMin.load(std::memory_order_relaxed))
            m_audioRingLevelMin.store(queued, std::memory_order_relaxed);
        if (queued < m_audioRingLevelMinPlaying.load(std::memory_order_relaxed))
            m_audioRingLevelMinPlaying.store(queued, std::memory_order_relaxed);
        m_audioRingLevelTotal.fetch_add(queued, std::memory_order_relaxed);
        m_audioRingLevelSamples.fetch_add(1, std::memory_order_relaxed);
        // ~0.4 s at a block per frame.
        m_pushQueuedEma += 0.05 * (static_cast<double>(queued) - m_pushQueuedEma);
    } else {
        m_pushQueuedEma = m_audioCushion.PushSetpoint();
    }

    const core::AudioCushion::PushPlan plan =
        m_audioCushion.PlanPush(queued, static_cast<uint32_t>(frames), playing);
    uint32_t silence = 0;
    if (plan.silence) {
        if (!m_pushOut->Write(nullptr, plan.silence, silence)) {
            ReopenPushOutput();
            return;
        }
        if (plan.starved) {
            m_audioUnderrunCallbacks.fetch_add(1, std::memory_order_relaxed);
            m_audioFramesHeld.fetch_add(silence, std::memory_order_relaxed);
        }
    }

    const int16_t* src = data;
    size_t count = frames;
    if (plan.skip) {
        // Keep the newest audio, the part that belongs with the picture on screen, and crossfade
        // into it from where the queue left off so the cut does not click.
        count = frames - plan.skip;
        const int16_t* kept = data + static_cast<size_t>(plan.skip) * 2;
        m_pushScratch.assign(kept, kept + count * 2);
        const size_t fade = std::min<size_t>({64, plan.skip, count});
        for (size_t i = 0; i < fade; ++i) {
            const float gain = static_cast<float>(i + 1) / static_cast<float>(fade + 1);
            for (size_t ch = 0; ch < 2; ++ch)
                m_pushScratch[i * 2 + ch] = static_cast<int16_t>(data[i * 2 + ch] * (1.0f - gain) +
                                                                 m_pushScratch[i * 2 + ch] * gain);
        }
        src = m_pushScratch.data();
        m_audioFramesTrimmed.fetch_add(plan.skip, std::memory_order_relaxed);
        m_audioTrimEvents.fetch_add(1, std::memory_order_relaxed);
    }

    uint32_t written = 0;
    if (count && !m_pushOut->Write(src, static_cast<uint32_t>(count), written)) {
        ReopenPushOutput();
        return;
    }
    if (written) {
        m_pushLastL = src[(written - 1) * 2];
        m_pushLastR = src[(written - 1) * 2 + 1];
        m_pushTail = true;
    }
    m_audioDeviceCallbacks.fetch_add(1, std::memory_order_relaxed);
    m_audioFramesProduced.fetch_add(written, std::memory_order_relaxed);
    m_audioFramesConsumed.fetch_add(written, std::memory_order_relaxed);
    if (written < count)
        m_audioFramesDroppedOverrun.fetch_add(count - written, std::memory_order_relaxed);

    // Tap C - exactly what was queued for the sound card, silence included.
    if (m_tapDevice.IsRecording()) {
        m_pushTapScratch.assign(static_cast<size_t>(silence + written) * 2, 0.0f);
        for (size_t i = 0; i < static_cast<size_t>(written) * 2; ++i)
            m_pushTapScratch[static_cast<size_t>(silence) * 2 + i] = src[i] / 32768.0f;
        core::AudioRecorder::BlockInfo block;
        block.frameIndex = m_audioDeviceCallbacks.load(std::memory_order_relaxed);
        block.frames = silence + written;
        block.ringLevel = queued;
        block.held = playing ? silence : 0;
        block.dropped = plan.skip;
        m_tapDevice.PushFloat(m_pushTapScratch.data(), silence + written);
        m_tapDevice.PushBlock(block);
    }
}

void AmigaEmulator::EndPushStream()
{
    // Whatever the queue does now is a stop, not a starve: the next write is a fresh start. A main
    // thread stall never comes through here, so its empty queue still counts.
    m_pushLastWrite = {};
    // The sound card would otherwise drop off the last sample when the queue runs out, which
    // clicks - FillUnderrun's fade, for the push device.
    const bool fade = m_pushActive && m_pushTail && !m_isMuted && m_pushOut->IsOpen();
    m_pushTail = false;
    if (!fade)
        return;
    constexpr uint32_t kFadeFrames = 64;
    int16_t tail[kFadeFrames * 2];
    for (uint32_t i = 0; i < kFadeFrames; ++i) {
        const float gain = 1.0f - static_cast<float>(i + 1) / static_cast<float>(kFadeFrames + 1);
        tail[i * 2] = static_cast<int16_t>(m_pushLastL * gain);
        tail[i * 2 + 1] = static_cast<int16_t>(m_pushLastR * gain);
    }
    uint32_t written = 0;
    m_pushOut->Write(tail, kFadeFrames, written);
}

uint32_t AmigaEmulator::FillUnderrun(AmigaEmulator* emu, float* out, uint32_t validFrames,
                                     uint32_t frameCount)
{
    // The ring ran dry mid-buffer. The old behaviour repeated the last sample for the rest of the
    // callback, which is a DC step followed by a flat line — that is what "crunch" sounded like,
    // and it was measured filling 23% of the output stream. Fade to silence over a short ramp
    // instead: it removes the click at the discontinuity without inventing audible signal.
    constexpr uint32_t kFadeFrames = 64;

    float fromL = emu->m_audioLastOutL;
    float fromR = emu->m_audioLastOutR;
    if (validFrames > 0) {
        fromL = out[(validFrames - 1) * 2];
        fromR = out[(validFrames - 1) * 2 + 1];
    }

    const uint32_t missing = frameCount - validFrames;
    const uint32_t fade = std::min(kFadeFrames, missing);

    for (uint32_t i = 0; i < fade; ++i) {
        const float gain = 1.0f - static_cast<float>(i + 1) / static_cast<float>(fade + 1);
        out[(validFrames + i) * 2] = fromL * gain;
        out[(validFrames + i) * 2 + 1] = fromR * gain;
    }
    if (missing > fade) {
        std::memset(&out[(validFrames + fade) * 2], 0, (missing - fade) * 2 * sizeof(float));
    }

    // Rebuild the cushion before consuming again, rather than limping along at zero occupancy and
    // underrunning on every subsequent callback.
    emu->m_audioPrimed.store(false, std::memory_order_release);
    emu->m_audioLastOutL = 0.0f;
    emu->m_audioLastOutR = 0.0f;
    return missing;
}

size_t AmigaEmulator::DropOldestAudio(AmigaEmulator* emu, size_t readPos, size_t available,
                                      size_t drop)
{
    drop = std::min(drop, available);
    // Crossfade the first frames after the cut with the ones being skipped, so the jump in the
    // waveform does not click. Only frames between the read and write positions are touched, and
    // the producer never writes there. fade <= drop keeps the two ranges apart.
    constexpr size_t kFadeFrames = 64;
    const size_t newPos = (readPos + drop) % kAudioRingSize;
    const size_t fade = std::min({kFadeFrames, drop, available - drop});
    for (size_t i = 0; i < fade; ++i) {
        const float gain = static_cast<float>(i + 1) / static_cast<float>(fade + 1);
        const size_t from = (readPos + i) % kAudioRingSize;
        const size_t to = (newPos + i) % kAudioRingSize;
        for (size_t ch = 0; ch < 2; ++ch) {
            float& dst = emu->m_audioRing[to * 2 + ch];
            dst = emu->m_audioRing[from * 2 + ch] * (1.0f - gain) + dst * gain;
        }
    }
    emu->m_audioFramesTrimmed.fetch_add(drop, std::memory_order_relaxed);
    emu->m_audioTrimEvents.fetch_add(1, std::memory_order_relaxed);
    return newPos;
}

void AmigaEmulator::AudioDeviceCallback(ma_device* device, void* output, const void* /*input*/,
                                        uint32_t frameCount)
{
    AmigaEmulator* emu = static_cast<AmigaEmulator*>(device->pUserData);
    float* out = static_cast<float*>(output);

    if (!emu || !emu->m_audioInitialized || emu->m_isMuted) {
        std::memset(out, 0, frameCount * 2 * sizeof(float));
        if (emu && emu->m_isMuted) {
            emu->m_audioReadPos.store(emu->m_audioWritePos.load(std::memory_order_acquire),
                                      std::memory_order_release);
        }
        return;
    }

    // Read available samples from ring buffer into a temp buffer at core sample rate
    size_t readPos = emu->m_audioReadPos.load(std::memory_order_relaxed);
    size_t writePos = emu->m_audioWritePos.load(std::memory_order_acquire);

    // Calculate available frames in ring buffer
    size_t available = 0;
    if (writePos >= readPos)
        available = writePos - readPos;
    else
        available = kAudioRingSize - readPos + writePos;

    // Ring occupancy at entry — the number that says whether this callback was starved.
    const uint32_t ringLevel = static_cast<uint32_t>(available);
    emu->m_audioDeviceCallbacks.fetch_add(1, std::memory_order_relaxed);
    emu->m_audioRingLevel.store(ringLevel, std::memory_order_relaxed);
    uint32_t prevMin = emu->m_audioRingLevelMin.load(std::memory_order_relaxed);
    while (ringLevel < prevMin && !emu->m_audioRingLevelMin.compare_exchange_weak(
                                      prevMin, ringLevel, std::memory_order_relaxed)) {
    }
    uint32_t prevMax = emu->m_audioRingLevelMax.load(std::memory_order_relaxed);
    while (ringLevel > prevMax && !emu->m_audioRingLevelMax.compare_exchange_weak(
                                      prevMax, ringLevel, std::memory_order_relaxed)) {
    }

    // Per-callback accounting, reported to the tap C sidecar so a glitch in the waveform can be
    // traced to the callback that produced it.
    uint32_t heldFrames = 0;
    uint32_t droppedFrames = 0;
    uint32_t trimmedFrames = 0;
    size_t consumedFrames = 0;

    // Latency priming. The producer is bursty — the core flushes audio whenever its internal
    // buffer fills, so block sizes vary with what the emulated program is doing (measured at
    // 484-1600 frames for a demo that drives Paula hard, against a steady 882 for one that does
    // not). The consumer is paced by the sound card. With no target occupancy the ring sits near
    // empty and starves on most callbacks, so we hold output until it has built up a cushion.
    if (!emu->m_audioPrimed.load(std::memory_order_acquire)) {
        if (available < emu->m_audioTargetFrames) {
            // Still filling: emit silence rather than consuming a buffer we cannot sustain.
            std::memset(out, 0, frameCount * 2 * sizeof(float));
            emu->m_audioLastOutL = 0.0f;
            emu->m_audioLastOutR = 0.0f;
            if (emu->m_tapDevice.IsRecording()) {
                core::AudioRecorder::BlockInfo block;
                block.frameIndex = emu->m_audioDeviceCallbacks.load(std::memory_order_relaxed);
                block.frames = frameCount;
                block.ringLevel = ringLevel;
                emu->m_tapDevice.PushFloat(out, frameCount);
                emu->m_tapDevice.PushBlock(block);
            }
            return;
        }
        emu->m_audioPrimed.store(true, std::memory_order_release);
        trimmedFrames = emu->m_audioCushion.OnPrime(ringLevel);
        emu->m_audioRingLevelEma.store(static_cast<float>(emu->m_audioCushion.Setpoint()),
                                       std::memory_order_relaxed);
    } else {
        trimmedFrames = emu->m_audioCushion.OnCallback(ringLevel, frameCount);
    }
    // Anything queued past the target is audio whose picture is already on screen. Playing it
    // would put the sound that far behind the picture for the rest of the session.
    if (trimmedFrames > 0) {
        readPos = DropOldestAudio(emu, readPos, available, trimmedFrames);
        available -= trimmedFrames;
    }
    emu->m_audioRingLevelTotal.fetch_add(available, std::memory_order_relaxed);
    emu->m_audioRingLevelSamples.fetch_add(1, std::memory_order_relaxed);
    {
        const uint32_t playing = static_cast<uint32_t>(available);
        uint32_t prev = emu->m_audioRingLevelMinPlaying.load(std::memory_order_relaxed);
        while (playing < prev && !emu->m_audioRingLevelMinPlaying.compare_exchange_weak(
                                     prev, playing, std::memory_order_relaxed)) {
        }
        // Smoothed over ~0.5 s of output for the pace controller.
        const float alpha = std::min(1.0f, static_cast<float>(frameCount) /
                                               (0.5f * static_cast<float>(device->sampleRate)));
        const float ema = emu->m_audioRingLevelEma.load(std::memory_order_relaxed);
        emu->m_audioRingLevelEma.store(ema + alpha * (static_cast<float>(available) - ema),
                                       std::memory_order_relaxed);
    }

    if (!emu->m_needsResampling) {
        // No resampling needed — copy directly
        uint32_t toCopy = std::min(static_cast<uint32_t>(available), frameCount);
        for (uint32_t i = 0; i < toCopy; i++) {
            out[i * 2] = emu->m_audioRing[readPos * 2];
            out[i * 2 + 1] = emu->m_audioRing[readPos * 2 + 1];
            readPos = (readPos + 1) % kAudioRingSize;
        }
        consumedFrames = toCopy;
        if (toCopy < frameCount) {
            heldFrames = FillUnderrun(emu, out, toCopy, frameCount);
        }
    } else {
        // Resampling needed: read from ring into contiguous temp buffer,
        // then resample into output
        // Estimate how many source frames we need for the requested output frames
        auto* resampler = static_cast<ma_resampler*>(emu->m_audioResampler);
        ma_uint64 requiredInput = 0;
        ma_resampler_get_required_input_frame_count(resampler, frameCount, &requiredInput);

        size_t toRead = std::min(static_cast<size_t>(requiredInput), available);

        // Use a stack buffer for reasonable sizes, otherwise limit
        constexpr size_t kMaxTempFrames = 4096;
        if (toRead > kMaxTempFrames)
            toRead = kMaxTempFrames;

        float tempBuf[kMaxTempFrames * 2];
        size_t scanPos = readPos;
        for (size_t i = 0; i < toRead; i++) {
            tempBuf[i * 2] = emu->m_audioRing[scanPos * 2];
            tempBuf[i * 2 + 1] = emu->m_audioRing[scanPos * 2 + 1];
            scanPos = (scanPos + 1) % kAudioRingSize;
        }

        ma_uint64 inFrames = toRead;
        ma_uint64 outFrames = frameCount;
        ma_resampler_process_pcm_frames(resampler, tempBuf, &inFrames, out, &outFrames);

        // Advance the read position by what the resampler actually consumed, not by what we
        // offered it. Advancing by toRead discarded the difference on every callback.
        readPos = (readPos + static_cast<size_t>(inFrames)) % kAudioRingSize;
        consumedFrames = static_cast<size_t>(inFrames);

        if (outFrames < frameCount) {
            heldFrames = FillUnderrun(emu, out, static_cast<uint32_t>(outFrames), frameCount);
        }
    }

    // Track the last real sample so the next underrun can fade from it rather than jumping.
    if (consumedFrames > 0 && heldFrames < frameCount) {
        const uint32_t lastReal = frameCount - heldFrames - 1;
        emu->m_audioLastOutL = out[lastReal * 2];
        emu->m_audioLastOutR = out[lastReal * 2 + 1];
    }

    emu->m_audioReadPos.store(readPos, std::memory_order_release);

    emu->m_audioFramesConsumed.fetch_add(consumedFrames, std::memory_order_relaxed);
    if (heldFrames > 0) {
        emu->m_audioFramesHeld.fetch_add(heldFrames, std::memory_order_relaxed);
        emu->m_audioUnderrunCallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    if (droppedFrames > 0)
        emu->m_audioFramesDroppedResampler.fetch_add(droppedFrames, std::memory_order_relaxed);

    // Tap C — the exact buffer handed to the sound card, plus the accounting that explains it.
    if (emu->m_tapDevice.IsRecording()) {
        core::AudioRecorder::BlockInfo block;
        block.frameIndex = emu->m_audioDeviceCallbacks.load(std::memory_order_relaxed);
        block.frames = frameCount;
        block.ringLevel = ringLevel;
        block.held = heldFrames;
        block.dropped = droppedFrames + trimmedFrames;
        emu->m_tapDevice.PushFloat(out, frameCount);
        emu->m_tapDevice.PushBlock(block);
    }
}

} // namespace adm
