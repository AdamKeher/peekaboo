#include "EmulatorOptions.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>

namespace adm::core {

namespace {

using Value = EmulatorOptionValue;

// CPU --------------------------------------------------------------------------------------

constexpr std::array kCpuModel{Value{"auto", "Auto"},   Value{"68000", "68000"},
                               Value{"68010", "68010"}, Value{"68020", "68020"},
                               Value{"68030", "68030"}, Value{"68040", "68040"},
                               Value{"68060", "68060"}};

constexpr std::array kFpuModel{Value{"auto", "Auto"}, Value{"0", "None"}, Value{"68881", "68881"},
                               Value{"68882", "68882"}, Value{"cpu", "CPU Internal"}};

// 68030 and up (core 1.87.0). Auto is the core's own choice: a full MMU for any of them.
constexpr std::array kMmuModel{Value{"auto", "Auto (Full)"}, Value{"full", "Full"},
                               Value{"ec", "EC (No Translation)"}, Value{"none", "None (Fastest)"}};

// The core derives cpu_compatible, cpu_cycle_exact, cpu_memory_cycle_exact and
// blitter_cycle_exact from this one option and reads no separate variable for any of them.
constexpr std::array kCpuCompatibility{
    Value{"normal", "Normal"}, Value{"compatible", "More Compatible"},
    Value{"memory", "Memory Cycle Exact"}, Value{"exact", "Cycle Exact"}};

// Chipset ----------------------------------------------------------------------------------

// "false" is the core's spelling for "no immediate blits, no waiting blits" - the accurate
// setting. It is not "disabled": the core compares the string and silently ignores anything it
// does not recognise.
constexpr std::array kImmediateBlits{Value{"false", "Disabled (Cycle-Accurate)"},
                                     Value{"waiting", "Wait for Blitter"},
                                     Value{"immediate", "Immediate"}};

constexpr std::array kCollisionLevel{Value{"none", "None"}, Value{"sprites", "Sprites Only"},
                                     Value{"playfields", "Playfields"}, Value{"full", "Full"}};

// Chipset revisions below the model preset (core 1.86.0). "auto" is the preset's own choice;
// every other value is cfgfile.c's spelling, which the core passes straight through.
constexpr std::array kChipset{Value{"auto", "Model Default"},
                              Value{"ocs", "OCS"},
                              Value{"ecs_agnus", "ECS Agnus + OCS Denise"},
                              Value{"ecs_denise", "OCS Agnus + ECS Denise"},
                              Value{"ecs", "ECS"},
                              Value{"aga", "AGA"}};

constexpr std::array kChipsetCompatible{
    Value{"auto", "Model Default"}, Value{"Generic", "Generic"},
    Value{"A500", "A500"},          Value{"A500+", "A500+"},
    Value{"A600", "A600"},          Value{"A1000", "A1000"},
    Value{"A1200", "A1200"},        Value{"A2000", "A2000"},
    Value{"A3000", "A3000"},        Value{"A3000T", "A3000T"},
    Value{"A4000", "A4000"},        Value{"A4000T", "A4000T"},
    Value{"CDTV", "CDTV"},          Value{"CDTV-CR", "CDTV-CR"},
    Value{"CD32", "CD32"},          Value{"Velvet", "A1000 Prototype (Velvet)"}};

constexpr std::array kAgnusModel{Value{"auto", "Follow Chipset"},
                                 Value{"velvet", "Velvet"},
                                 Value{"a1000", "A1000"},
                                 Value{"ocs", "OCS"},
                                 Value{"ecs", "ECS"},
                                 Value{"aga", "AGA"}};

constexpr std::array kAgnusSize{Value{"auto", "Follow Chipset"}, Value{"512k", "512K"},
                                Value{"1m", "1M"}, Value{"2m", "2M"}};

constexpr std::array kDeniseModel{Value{"auto", "Follow Chipset"},
                                  Value{"velvet", "Velvet"},
                                  Value{"a1000_noehb", "A1000 (No EHB)"},
                                  Value{"a1000", "A1000"},
                                  Value{"ocs", "OCS"},
                                  Value{"ecs", "ECS"},
                                  Value{"aga", "AGA"}};

// RAM --------------------------------------------------------------------------------------

constexpr std::array kChipMem{Value{"auto", "Automatic"}, Value{"1", "0.5 MB"}, Value{"2", "1 MB"},
                              Value{"3", "1.5 MB"}, Value{"4", "2 MB"}};

constexpr std::array kBogoMem{Value{"auto", "Automatic"}, Value{"0", "None"},
                              Value{"2", "0.5 MB"},       Value{"4", "1 MB"},
                              Value{"6", "1.5 MB"},       Value{"7", "1.8 MB"}};

constexpr std::array kFastMem{Value{"auto", "Automatic"}, Value{"0", "None"}, Value{"1", "1 MB"},
                              Value{"2", "2 MB"},         Value{"4", "4 MB"}, Value{"8", "8 MB"}};

constexpr std::array kZ3Mem{
    Value{"auto", "Automatic"}, Value{"0", "None"},     Value{"1", "1 MB"},
    Value{"2", "2 MB"},         Value{"4", "4 MB"},     Value{"8", "8 MB"},
    Value{"16", "16 MB"},       Value{"32", "32 MB"},   Value{"64", "64 MB"},
    Value{"128", "128 MB"},     Value{"256", "256 MB"}, Value{"512", "512 MB"}};

// RTG (Picasso96 / uaegfx), core 1.99.0. No "Automatic": every model preset leaves the board at
// zero, so an automatic setting would only ever mean "None" and the option exists to turn on
// something no preset turns on.
constexpr std::array kGfxCardSize{Value{"0", "Disabled"}, Value{"1", "1 MB"}, Value{"2", "2 MB"},
                                  Value{"4", "4 MB"},     Value{"8", "8 MB"}, Value{"16", "16 MB"},
                                  Value{"32", "32 MB"}};

// Zorro II caps the board at 8 MB and needs a 24-bit machine; Zorro III is what an 020+ wants,
// and the only way past 8 MB. These strings are matched against gfxboard_get_configname().
constexpr std::array kGfxCardType{Value{"ZorroIII", "Zorro III"}, Value{"ZorroII", "Zorro II"}};

// Video ------------------------------------------------------------------------------------

constexpr std::array kVideoStandard{Value{"PAL", "PAL (50 Hz)"}, Value{"NTSC", "NTSC (60 Hz)"},
                                    Value{"PAL auto", "Auto"}};

constexpr std::array kVideoResolution{Value{"auto", "Auto"}, Value{"lores", "Low (360 px)"},
                                      Value{"hires", "High (720 px)"},
                                      Value{"superhires", "Super-High (1440 px)"}};

constexpr std::array kVideoVResolution{Value{"auto", "Auto"}, Value{"single", "Single Line"},
                                       Value{"double", "Double Line"}};

constexpr std::array kVideoAspect{Value{"auto", "Auto"}, Value{"PAL", "PAL"}, Value{"NTSC", "NTSC"},
                                  Value{"1:1", "1:1 Pixel"}};

constexpr std::array kFramerate{Value{"disabled", "No Skip"}, Value{"1", "Skip 1 Frame"},
                                Value{"2", "Skip 2 Frames"}};

constexpr std::array kFlickerFixer{Value{"disabled", "Off"}, Value{"enabled", "On"}};

// Audio ------------------------------------------------------------------------------------

constexpr std::array kStereoSeparation{Value{"0%", "0% (Mono)"},
                                       Value{"10%", "10%"},
                                       Value{"20%", "20%"},
                                       Value{"30%", "30%"},
                                       Value{"40%", "40%"},
                                       Value{"50%", "50%"},
                                       Value{"60%", "60%"},
                                       Value{"70%", "70%"},
                                       Value{"80%", "80%"},
                                       Value{"90%", "90%"},
                                       Value{"100%", "100% (Full Stereo)"}};

constexpr std::array kInterpolation{Value{"none", "None"}, Value{"anti", "Anti"},
                                    Value{"sinc", "Sinc"}, Value{"rh", "RH"},
                                    Value{"crux", "Crux"}};

constexpr std::array kSoundFilter{Value{"emulated", "Emulated (Model-Specific)"},
                                  Value{"off", "Off"}, Value{"on", "Always On"}};

constexpr std::array kSoundFilterType{Value{"auto", "Auto"}, Value{"standard", "Standard (A500)"},
                                      Value{"enhanced", "Enhanced (A1200)"}};

// Core 1.93.0: the expansion board names in expansion.c.
constexpr std::array kSoundBoard{Value{"disabled", "None"},
                                 Value{"toccata", "Toccata"},
                                 Value{"prelude", "Prelude"},
                                 Value{"prelude1200", "Prelude 1200"},
                                 Value{"uaesnd_z2", "UAESND Zorro II"},
                                 Value{"uaesnd_z3", "UAESND Zorro III"}};

// Floppy -----------------------------------------------------------------------------------

constexpr std::array kFloppySpeed{Value{"100", "Normal (100%)"}, Value{"200", "Fast (200%)"},
                                  Value{"400", "Very Fast (400%)"},
                                  Value{"800", "Ultra Fast (800%)"}, Value{"0", "Turbo (Instant)"}};

constexpr std::array kOffOn{Value{"disabled", "Off"}, Value{"enabled", "On"}};

// ethernet.c's driver names: SLIRP, and SLIRP forwarding host ports 21-23 and 80.
constexpr std::array kA2065{Value{"disabled", "Off"}, Value{"slirp", "NAT"},
                            Value{"slirp_inbound", "NAT, open ports"}};

// The core takes exactly these three. "internal" plays samples compiled into the DLL; the other
// two read drive_click_<name>.wav and friends from <system>/uae_data, and a missing file silences
// drive sound altogether rather than falling back to the built-in set.
constexpr std::array kFloppySoundType{Value{"internal", "Internal (built in)"},
                                      Value{"A500", "External: A500"},
                                      Value{"LOUD", "External: LOUD"}};

// An attenuation, not a volume: 0 is full loudness and 100 is silence, so the values descend as
// the labels climb. Every step the core offers, so a value already in the settings file always
// has a row to land on.
constexpr std::array kFloppySoundVolume{
    Value{"100", "Off"}, Value{"95", "5%"},  Value{"90", "10%"}, Value{"85", "15%"},
    Value{"80", "20%"},  Value{"75", "25%"}, Value{"70", "30%"}, Value{"65", "35%"},
    Value{"60", "40%"},  Value{"55", "45%"}, Value{"50", "50%"}, Value{"45", "55%"},
    Value{"40", "60%"},  Value{"35", "65%"}, Value{"30", "70%"}, Value{"25", "75%"},
    Value{"20", "80%"},  Value{"15", "85%"}, Value{"10", "90%"}, Value{"5", "95%"},
    Value{"0", "100%"}};

// CD ---------------------------------------------------------------------------------------

constexpr std::array kCdSpeed{Value{"100", "Real drive speed"},
                              Value{"0", "Turbo (no seek delay)"}};

constexpr std::array kCdVolume{Value{"0%", "0%"},   Value{"5%", "5%"},   Value{"10%", "10%"},
                               Value{"15%", "15%"}, Value{"20%", "20%"}, Value{"25%", "25%"},
                               Value{"30%", "30%"}, Value{"35%", "35%"}, Value{"40%", "40%"},
                               Value{"45%", "45%"}, Value{"50%", "50%"}, Value{"55%", "55%"},
                               Value{"60%", "60%"}, Value{"65%", "65%"}, Value{"70%", "70%"},
                               Value{"75%", "75%"}, Value{"80%", "80%"}, Value{"85%", "85%"},
                               Value{"90%", "90%"}, Value{"95%", "95%"}, Value{"100%", "100%"}};

template <std::size_t N>
constexpr EmulatorOptionInfo Option(const char* key, const char* defaultValue,
                                    const std::array<Value, N>& values)
{
    return EmulatorOptionInfo{key, defaultValue, values.data(), values.size()};
}

std::vector<EmulatorOptionInfo> BuildOptions()
{
    return {
        // CPU. "exact" is Peekaboo's default rather than the core's "memory": a debugger that
        // draws DMA and blitter timing has to be cycle exact to be telling the truth.
        Option("puae_cpu_model", "auto", kCpuModel),
        Option("puae_fpu_model", "auto", kFpuModel),
        Option("puae_mmu_model", "auto", kMmuModel),
        Option("puae_cpu_compatibility", "exact", kCpuCompatibility),
        // Free numeric: -900.0 to 5000.0 in the dialog, atof() in the core.
        EmulatorOptionInfo{"puae_cpu_throttle", "0.0", nullptr, 0},

        // Chipset.
        Option("puae_immediate_blits", "false", kImmediateBlits),
        Option("puae_collision_level", "full", kCollisionLevel),
        Option("puae_chipset", "auto", kChipset),
        Option("puae_chipset_compatible", "auto", kChipsetCompatible),
        Option("puae_agnus_model", "auto", kAgnusModel),
        Option("puae_agnus_size", "auto", kAgnusSize),
        Option("puae_denise_model", "auto", kDeniseModel),

        // RAM. "auto" throughout: SelectModelPreset() writes the machine's real sizes as user
        // variables, which outrank these.
        Option("puae_chipmem_size", "auto", kChipMem),
        Option("puae_bogomem_size", "auto", kBogoMem),
        Option("puae_fastmem_size", "auto", kFastMem),
        Option("puae_z3mem_size", "auto", kZ3Mem),

        // RTG. Off by default: an uaegfx board with no Picasso96 installed in the guest is dead
        // weight, and the board eats Zorro address space whether or not anything uses it.
        Option("puae_gfxcard_size", "0", kGfxCardSize),
        Option("puae_gfxcard_type", "ZorroIII", kGfxCardType),

        // Video. PAL rather than the core's "PAL auto" so a disk that says nothing still boots
        // the machine most Amiga software was written for.
        Option("puae_video_standard", "PAL", kVideoStandard),
        Option("puae_video_resolution", "auto", kVideoResolution),
        Option("puae_video_vresolution", "auto", kVideoVResolution),
        Option("puae_video_aspect", "auto", kVideoAspect),
        Option("puae_gfx_framerate", "disabled", kFramerate),
        Option("puae_gfx_flickerfixer", "disabled", kFlickerFixer),

        // Audio.
        Option("puae_sound_stereo_separation", "70%", kStereoSeparation),
        Option("puae_sound_interpol", "anti", kInterpolation),
        Option("puae_sound_filter", "emulated", kSoundFilter),
        Option("puae_sound_filter_type", "auto", kSoundFilterType),
        Option("puae_sound_board", "disabled", kSoundBoard),

        // Floppy. write_redirect stays "disabled" because Peekaboo's own disk-write capture
        // depends on it: the core only calls adm_record_disk_write() when writes are allowed
        // through to the image, and that is what marks a document dirty.
        Option("puae_floppy_speed", "100", kFloppySpeed),
        Option("puae_floppy_write_protection", "disabled", kOffOn),
        Option("puae_floppy_write_redirect", "disabled", kOffOn),
        Option("puae_floppy_multidrive", "enabled", kOffOn),
        Option("puae_floppy_sound", "80", kFloppySoundVolume),
        Option("puae_floppy_sound_empty_mute", "enabled", kOffOn),
        Option("puae_floppy_sound_type", "internal", kFloppySoundType),

        // Network (core 1.91.0). bsdsocket.library through the host's TCP/IP stack.
        Option("puae_bsdsocket", "disabled", kOffOn),
        // Core 1.92.0, Windows builds: an A2065 card and uaenet.device, both on SLIRP.
        Option("puae_a2065", "disabled", kA2065),
        Option("puae_sana2", "disabled", kOffOn),

        // CD, for CD32/CDTV and a CD on any other machine. The core's own defaults.
        Option("puae_cd_speed", "100", kCdSpeed),
        Option("puae_cd_startup_delayed_insert", "disabled", kOffOn),
        Option("puae_shared_nvram", "disabled", kOffOn),
        Option("puae_sound_volume_cd", "100%", kCdVolume),
    };
}

const std::unordered_map<std::string_view, const EmulatorOptionInfo*>& OptionIndex()
{
    static const std::unordered_map<std::string_view, const EmulatorOptionInfo*> index = [] {
        std::unordered_map<std::string_view, const EmulatorOptionInfo*> map;
        for (const auto& option : GetEmulatorOptions())
            map.emplace(option.key, &option);
        return map;
    }();
    return index;
}

} // namespace

const std::vector<EmulatorOptionInfo>& GetEmulatorOptions()
{
    static const std::vector<EmulatorOptionInfo> options = BuildOptions();
    return options;
}

const EmulatorOptionInfo* FindEmulatorOption(std::string_view key)
{
    const auto& index = OptionIndex();
    auto it = index.find(key);
    return it == index.end() ? nullptr : it->second;
}

std::string_view EmulatorOptionDefault(std::string_view key)
{
    const auto* option = FindEmulatorOption(key);
    return (option && option->HasDefault()) ? std::string_view(option->defaultValue)
                                            : std::string_view();
}

bool IsEmulatorOptionValue(std::string_view key, std::string_view value)
{
    const auto* option = FindEmulatorOption(key);
    if (!option)
        return false;
    if (option->values == nullptr)
        return true;
    return std::any_of(
        option->values, option->values + option->valueCount,
        [&](const EmulatorOptionValue& candidate) { return value == candidate.value; });
}

} // namespace adm::core
