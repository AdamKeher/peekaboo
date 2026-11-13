#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace adm::core {

// The machine the core is running, as adm_get_machine_config() reports it from currprefs (core
// 1.86.0). What an override produced, not what it asked for.
struct MachineConfig
{
    int chipsetMask = 0; // CSMASK_ECS_AGNUS 1, CSMASK_ECS_DENISE 2, CSMASK_AGA 4
    int compatible = 0;  // CP_* index into the chipset-extra vocabulary
    int agnusModel = 0;  // AGNUSMODEL_*
    int agnusSize = 0;   // AGNUSSIZE_*
    int deniseModel = 0; // DENISEMODEL_*
    int cpuModel = 0;
    int fpuModel = 0;
    int mmuModel = 0;
    bool mmuEc = false;
    bool addressSpace24 = true;

    bool operator==(const MachineConfig&) const = default;
};

// adm_get_machine_config's int array, in its documented order.
inline constexpr int kMachineConfigValueCount = 10;
std::optional<MachineConfig> ParseMachineConfig(const int* values, int count);

// The core options below the model preset: the chipset revisions (core 1.86.0) and the MMU
// (1.87.0). SelectModelPreset() puts each back to "auto".
struct MachineSettingKey
{
    const char* key;     // core option, e.g. "puae_chipset"
    const char* keyword; // console keyword, e.g. "CHIPSET"
    const char* label;   // "Chipset"
};
inline constexpr std::array kMachineSettingKeys{
    MachineSettingKey{"puae_chipset", "CHIPSET", "Chipset"},
    MachineSettingKey{"puae_chipset_compatible", "EXTRA", "Chipset extra"},
    MachineSettingKey{"puae_agnus_model", "AGNUS", "Agnus model"},
    MachineSettingKey{"puae_agnus_size", "AGNUSSIZE", "Agnus size"},
    MachineSettingKey{"puae_denise_model", "DENISE", "Denise model"},
    MachineSettingKey{"puae_mmu_model", "MMU", "MMU"},
};
const MachineSettingKey* FindMachineSettingByKeyword(std::string_view keyword);

// Display names for the values the core reports.
std::string ChipsetName(int chipsetMask);          // "OCS", "ECS Agnus", "ECS Denise", "ECS", "AGA"
const char* ChipsetCompatibleName(int compatible); // "A500", "A1200", ... or "-"
const char* AgnusModelName(int model);             // "Default", "Velvet", "A1000", "OCS", ...
const char* AgnusSizeName(int size);               // "Default", "512K", "1M", "2M"
const char* DeniseModelName(int model);            // "Default", "Velvet", "A1000 (no EHB)", ...
std::string MmuName(const MachineConfig& config);  // "None", "68030", "68EC030"
// 24 or 32. A 68000 or 68010 has 24 address lines whatever the prefs flag says. The flag itself
// comes from the model preset, which is why SelectModelPreset leaves puae_z3mem_size on "auto":
// any other value makes the core write cpu_24bit_addressing=false whatever the CPU is.
int AddressBits(const MachineConfig& config);

} // namespace adm::core
