#pragma once

// JSON encoding of the hardfile half of a hard-drive mount and of the SCSI card ROMs, shared by
// user preferences, .peek sessions and saved emulator configurations so the three cannot drift.
// Every reader tolerates the field being absent: files written before hardfiles existed load as
// directory mounts with the defaults.

#include "HostMountService.h"
#include "JsonRead.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace adm::core {

inline std::string MountPathToUtf8(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

inline std::filesystem::path MountPathFromUtf8(const std::string& value)
{
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

inline void WriteHardfileFields(nlohmann::json& json, const HostDirectoryMount& mount)
{
    json["backingType"] = mount.IsHardfile()                                   ? "hardfile"
                          : mount.backingType == HostMountBackingType::Archive ? "archive"
                                                                               : "directory";
    json["hardfile"] = MountPathToUtf8(mount.hardfilePath);
    json["controller"] = HardDriveControllerId(mount.controller);
    json["controllerUnit"] = mount.controllerUnit;
    json["geometry"] = {{"mode", HardfileGeometryModeId(mount.geometry.mode)},
                        {"sectors", mount.geometry.sectors},
                        {"surfaces", mount.geometry.surfaces},
                        {"reserved", mount.geometry.reserved},
                        {"blockSize", mount.geometry.blockSize}};
}

inline void ReadHardfileFields(const nlohmann::json& json, HostDirectoryMount& mount)
{
    if (!json.is_object())
        return;
    if (json.contains("backingType") && json["backingType"].is_string()) {
        const std::string backing = json["backingType"].get<std::string>();
        mount.backingType = backing == "hardfile"  ? HostMountBackingType::HardfileImage
                            : backing == "archive" ? HostMountBackingType::Archive
                                                   : HostMountBackingType::Directory;
    }
    if (json.contains("hardfile") && json["hardfile"].is_string())
        mount.hardfilePath = MountPathFromUtf8(json["hardfile"].get<std::string>());
    if (json.contains("controller") && json["controller"].is_string()) {
        if (auto controller = ParseHardDriveController(json["controller"].get<std::string>()))
            mount.controller = *controller;
    }
    if (json.contains("controllerUnit") && json["controllerUnit"].is_number_integer())
        mount.controllerUnit = json["controllerUnit"].get<int>();
    if (json.contains("geometry") && json["geometry"].is_object()) {
        const auto& geometry = json["geometry"];
        if (geometry.contains("mode") && geometry["mode"].is_string()) {
            if (auto mode = ParseHardfileGeometryMode(geometry["mode"].get<std::string>()))
                mount.geometry.mode = *mode;
        }
        jsonread::ReadIf(geometry, "sectors", mount.geometry.sectors);
        jsonread::ReadIf(geometry, "surfaces", mount.geometry.surfaces);
        jsonread::ReadIf(geometry, "reserved", mount.geometry.reserved);
        jsonread::ReadIf(geometry, "blockSize", mount.geometry.blockSize);
    }
}

inline nlohmann::json ControllerRomsJson(const HardDriveControllerRoms& roms)
{
    return {{"a590A2091", MountPathToUtf8(roms.a590A2091)}, {"a4091", MountPathToUtf8(roms.a4091)}};
}

inline HardDriveControllerRoms ControllerRomsFromJson(const nlohmann::json& json)
{
    HardDriveControllerRoms roms;
    if (!json.is_object())
        return roms;
    if (json.contains("a590A2091") && json["a590A2091"].is_string())
        roms.a590A2091 = MountPathFromUtf8(json["a590A2091"].get<std::string>());
    if (json.contains("a4091") && json["a4091"].is_string())
        roms.a4091 = MountPathFromUtf8(json["a4091"].get<std::string>());
    return roms;
}

} // namespace adm::core
