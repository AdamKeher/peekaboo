#include "MachineConfig.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>

namespace adm::core {
namespace {

const char* Lookup(int index, std::initializer_list<const char*> names)
{
    if (index < 0 || index >= static_cast<int>(names.size()))
        return "?";
    return *(names.begin() + index);
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) ==
                      std::toupper(static_cast<unsigned char>(y));
           });
}

} // namespace

std::optional<MachineConfig> ParseMachineConfig(const int* values, int count)
{
    if (!values || count < kMachineConfigValueCount)
        return std::nullopt;
    MachineConfig config;
    config.chipsetMask = values[0];
    config.compatible = values[1];
    config.agnusModel = values[2];
    config.agnusSize = values[3];
    config.deniseModel = values[4];
    config.cpuModel = values[5];
    config.fpuModel = values[6];
    config.mmuModel = values[7];
    config.mmuEc = values[8] != 0;
    config.addressSpace24 = values[9] != 0;
    return config;
}

const MachineSettingKey* FindMachineSettingByKeyword(std::string_view keyword)
{
    for (const auto& setting : kMachineSettingKeys) {
        if (EqualsIgnoreCase(keyword, setting.keyword))
            return &setting;
    }
    return nullptr;
}

std::string ChipsetName(int chipsetMask)
{
    constexpr int kEcsAgnus = 1;
    constexpr int kEcsDenise = 2;
    constexpr int kAga = 4;
    if (chipsetMask & kAga)
        return "AGA";
    const bool agnus = (chipsetMask & kEcsAgnus) != 0;
    const bool denise = (chipsetMask & kEcsDenise) != 0;
    if (agnus && denise)
        return "ECS";
    if (agnus)
        return "ECS Agnus";
    if (denise)
        return "ECS Denise";
    return "OCS";
}

const char* ChipsetCompatibleName(int compatible)
{
    return Lookup(compatible, {"-", "Generic", "CDTV", "CDTV-CR", "CD32", "A500", "A500+", "A600",
                               "A1000", "A1200", "A2000", "A3000", "A3000T", "A4000", "A4000T",
                               "Velvet", "Casablanca", "DraCo"});
}

const char* AgnusModelName(int model)
{
    return Lookup(model, {"Default", "Velvet", "A1000", "OCS", "ECS", "AGA"});
}

const char* AgnusSizeName(int size) { return Lookup(size, {"Default", "512K", "1M", "2M"}); }

const char* DeniseModelName(int model)
{
    return Lookup(model, {"Default", "Velvet", "A1000 (no EHB)", "A1000", "OCS", "ECS", "AGA"});
}

std::string MmuName(const MachineConfig& config)
{
    if (config.mmuModel == 0)
        return "None";
    if (config.mmuEc)
        return fmt::format("68EC0{}", config.mmuModel % 100);
    return std::to_string(config.mmuModel);
}

int AddressBits(const MachineConfig& config)
{
    return (config.cpuModel < 68020 || config.addressSpace24) ? 24 : 32;
}

} // namespace adm::core
