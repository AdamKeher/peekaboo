#include "HostMountService.h"

#include "CdMedia.h"
#include "HardfileInspector.h"
#include "Logger.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace adm::core {
namespace {

constexpr int kMinBootPriority = -128;
constexpr int kMaxBootPriority = 127;
constexpr auto kMountConfirmationTimeout = std::chrono::seconds(10);
constexpr auto kActivityDisplayDuration = std::chrono::milliseconds(180);

std::string PathUtf8(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path PathFromUtf8(const std::string& value)
{
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

bool HasUnsafeConfigCharacters(const std::string& value, bool mountField)
{
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7f || ch == '"')
            return true;
        if (mountField && (ch == ',' || ch == ':'))
            return true;
    }
    return false;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool IsSinglePathKey(const std::string& key)
{
    static constexpr const char* kKeys[] = {"kickstart_rom_file",
                                            "kickstart_ext_rom_file",
                                            "flash_file",
                                            "cart_file",
                                            "rtc_file",
                                            "floppy0",
                                            "floppy1",
                                            "floppy2",
                                            "floppy3",
                                            "statefile",
                                            "win32.rom_path",
                                            "win32.floppy_path",
                                            "win32.hardfile_path"};
    return std::find(std::begin(kKeys), std::end(kKeys), key) != std::end(kKeys);
}

std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

std::string MakeAbsoluteConfigPath(const std::string& raw,
                                   const std::filesystem::path& baseDirectory)
{
    bool quoted = raw.size() >= 2 && raw.front() == '"' && raw.back() == '"';
    std::string value = quoted ? raw.substr(1, raw.size() - 2) : raw;
    if (value.empty())
        return raw;
    std::filesystem::path path = PathFromUtf8(value);
    if (path.is_absolute())
        return raw;
    const std::string absolute = PathUtf8((baseDirectory / path).lexically_normal());
    return quoted ? '"' + absolute + '"' : absolute;
}

std::string RebaseConfigLine(const std::string& line, const std::filesystem::path& baseDirectory)
{
    const auto equals = line.find('=');
    if (equals == std::string::npos)
        return line;
    const std::string key = LowerAscii(Trim(line.substr(0, equals)));
    const std::string rawValue = Trim(line.substr(equals + 1));
    if (IsSinglePathKey(key))
        return line.substr(0, equals + 1) + MakeAbsoluteConfigPath(rawValue, baseDirectory);

    // UAE compound storage entries quote their host path. Rebase that quoted component while
    // leaving all controller, geometry, access, and priority fields byte-for-byte intact.
    if (key == "filesystem2" || key == "hardfile2") {
        const auto firstQuote = rawValue.find('"');
        const auto lastQuote = rawValue.find_last_of('"');
        if (firstQuote != std::string::npos && lastQuote > firstQuote) {
            const std::string hostPath =
                rawValue.substr(firstQuote + 1, lastQuote - firstQuote - 1);
            const std::string rebased = MakeAbsoluteConfigPath('"' + hostPath + '"', baseDirectory);
            return line.substr(0, equals + 1) + rawValue.substr(0, firstQuote) + rebased +
                   rawValue.substr(lastQuote + 1);
        }
    }
    return line;
}

bool CheckWritableDirectory(const std::filesystem::path& directory, std::string& outError)
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto probe = directory / (".peekaboo_write_probe_" + std::to_string(nonce));
    {
        std::ofstream stream(probe, std::ios::binary | std::ios::trunc);
        if (!stream) {
            outError = "directory is not writable: " + PathUtf8(directory);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::remove(probe, ec);
    if (ec) {
        outError = "directory write probe could not be removed: " + ec.message();
        return false;
    }
    return true;
}

const char* SourceName(HostMountSource source)
{
    switch (source) {
        case HostMountSource::Preferences:
            return "preferences";
        case HostMountSource::RestoredSession:
            return "restored session";
        case HostMountSource::TransientBartman:
            return "Bartman transient override";
        case HostMountSource::TransientWhdload:
            return "WHDLoad transient override";
        case HostMountSource::TransientDocument:
            return "hardfile document transient override";
    }
    return "unknown";
}

void LogMount(const char* prefix, const HostDirectoryMount& mount)
{
    if (!mount.enabled) {
        Logger::Info("Host mounts: {} {} disabled", prefix, HostMountDeviceName(mount.device));
        return;
    }
    if (mount.IsHardfile()) {
        Logger::Info("Host mounts: {} {} hardfile='{}' controller={} unit={} geometry={} mode={} "
                     "priority={} source={}",
                     prefix, HostMountDeviceName(mount.device), PathUtf8(mount.hardfilePath),
                     HardDriveControllerId(mount.controller), mount.controllerUnit,
                     HardfileGeometryModeId(mount.geometry.mode),
                     mount.readOnly ? "read-only" : "read-write", mount.bootPriority,
                     SourceName(mount.source));
        return;
    }
    Logger::Info("Host mounts: {} {} label='{}' path='{}' mode={} priority={} source={}", prefix,
                 HostMountDeviceName(mount.device), mount.volumeLabel,
                 PathUtf8(mount.hostDirectory), mount.readOnly ? "read-only" : "read-write",
                 mount.bootPriority, SourceName(mount.source));
}

bool CheckWritableFile(const std::filesystem::path& file, std::string& outError)
{
    // Opened for update and never truncated: this only asks whether the OS would let the core
    // write to it.
    std::fstream stream(file, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        outError =
            "hardfile is not writable (read-only attribute or no permission): " + PathUtf8(file);
        return false;
    }
    return true;
}

bool IsCompressedHardfile(const std::filesystem::path& path)
{
    return LowerAscii(path.extension().string()) == ".hdz";
}

bool ModelIs(const std::string& modelId, std::initializer_list<const char*> ids)
{
    return std::any_of(ids.begin(), ids.end(), [&](const char* id) { return modelId == id; });
}

} // namespace

const char* HostMountDeviceName(HostMountDevice device)
{
    return device == HostMountDevice::DH0 ? "DH0" : "DH1";
}

const char* HardDriveControllerName(HardDriveController controller)
{
    switch (controller) {
        case HardDriveController::Uae:
            return "UAE hardfile";
        case HardDriveController::IdeMainboard:
            return "Built-in IDE";
        case HardDriveController::ScsiA590A2091:
            return "A590/A2091 SCSI";
        case HardDriveController::ScsiA4091:
            return "A4091 SCSI";
    }
    return "unknown";
}

const char* HardDriveControllerId(HardDriveController controller)
{
    switch (controller) {
        case HardDriveController::Uae:
            return "uae";
        case HardDriveController::IdeMainboard:
            return "ide";
        case HardDriveController::ScsiA590A2091:
            return "a2091";
        case HardDriveController::ScsiA4091:
            return "a4091";
    }
    return "uae";
}

std::optional<HardDriveController> ParseHardDriveController(const std::string& value)
{
    const std::string lowered = LowerAscii(Trim(value));
    if (lowered == "uae")
        return HardDriveController::Uae;
    if (lowered == "ide")
        return HardDriveController::IdeMainboard;
    if (lowered == "a590" || lowered == "a2091" || lowered == "a590/a2091")
        return HardDriveController::ScsiA590A2091;
    if (lowered == "a4091")
        return HardDriveController::ScsiA4091;
    return std::nullopt;
}

const char* HardfileGeometryModeId(HardfileGeometryMode mode)
{
    switch (mode) {
        case HardfileGeometryMode::Auto:
            return "auto";
        case HardfileGeometryMode::Rdb:
            return "rdb";
        case HardfileGeometryMode::Manual:
            return "manual";
    }
    return "auto";
}

std::optional<HardfileGeometryMode> ParseHardfileGeometryMode(const std::string& value)
{
    const std::string lowered = LowerAscii(Trim(value));
    if (lowered == "auto")
        return HardfileGeometryMode::Auto;
    if (lowered == "rdb")
        return HardfileGeometryMode::Rdb;
    if (lowered == "manual")
        return HardfileGeometryMode::Manual;
    return std::nullopt;
}

std::string UaeConfigBuilder::ControllerToken(HardDriveController controller, int unit)
{
    switch (controller) {
        case HardDriveController::Uae:
            return fmt::format("uae{}", unit);
        case HardDriveController::IdeMainboard:
            return fmt::format("ide{}_mainboard", unit);
        case HardDriveController::ScsiA590A2091:
            return fmt::format("scsi{}_a2091", unit);
        case HardDriveController::ScsiA4091:
            return fmt::format("scsi{}_a4091", unit);
    }
    return "uae0";
}

int UaeConfigBuilder::MaxControllerUnit(HardDriveController controller)
{
    switch (controller) {
        case HardDriveController::Uae:
            return 0;
        case HardDriveController::IdeMainboard:
            return 1; // master and slave on the one built-in channel
        case HardDriveController::ScsiA590A2091:
        case HardDriveController::ScsiA4091:
            return 6; // SCSI IDs 0-6; 7 is the host adapter
    }
    return 0;
}

std::string UaeConfigBuilder::ControllerUnavailableReason(HardDriveController controller,
                                                          const std::string& modelId)
{
    switch (controller) {
        case HardDriveController::Uae:
        case HardDriveController::ScsiA590A2091:
            return {};
        case HardDriveController::IdeMainboard:
            if (ModelIs(modelId, {"A600", "A1200OG", "A1200", "A4030", "A4040"}))
                return {};
            return "only the A600, A1200 and A4000 have a built-in IDE controller";
        case HardDriveController::ScsiA4091:
            if (ModelIs(modelId, {"A4030", "A4040"}))
                return {};
            return "the A4091 is a Zorro III card and needs an A4000";
    }
    return {};
}

std::optional<std::string> UaeConfigBuilder::HardfileGeometryFields(const HostDirectoryMount& mount,
                                                                    std::string& outError)
{
    constexpr const char* kRdbFields = "0,0,0,512";
    switch (mount.geometry.mode) {
        case HardfileGeometryMode::Rdb:
            return std::string(kRdbFields);
        case HardfileGeometryMode::Manual: {
            const auto& g = mount.geometry;
            const bool blockSizeValid = g.blockSize >= 512 && g.blockSize <= 32768 &&
                                        (g.blockSize & (g.blockSize - 1)) == 0;
            if (g.sectors < 1 || g.sectors > 65535 || g.surfaces < 1 || g.surfaces > 255 ||
                g.reserved < 0 || g.reserved > 1024 || !blockSizeValid) {
                outError = std::string(HostMountDeviceName(mount.device)) +
                           ": manual geometry needs sectors 1-65535, surfaces 1-255, reserved "
                           "0-1024 and a power-of-two block size from 512";
                return std::nullopt;
            }
            return fmt::format("{},{},{},{}", g.sectors, g.surfaces, g.reserved, g.blockSize);
        }
        case HardfileGeometryMode::Auto: {
            std::string inspectError;
            const auto info = HardfileInspector::Inspect(mount.hardfilePath, inspectError);
            if (!info) {
                outError = std::string(HostMountDeviceName(mount.device)) +
                           ": cannot read the hardfile: " + inspectError;
                return std::nullopt;
            }
            if (info->hasRdb)
                return std::string(kRdbFields);
            return fmt::format("{},{},{},{}", info->guessSectors, info->guessSurfaces,
                               info->guessReserved, info->guessBlockSize);
        }
    }
    return std::string(kRdbFields);
}

bool UaeConfigBuilder::ValidateControllerRoms(const std::array<HostDirectoryMount, 2>& mounts,
                                              const HardDriveControllerRoms& roms,
                                              std::string& outError)
{
    for (const auto& mount : mounts) {
        if (!mount.enabled || !mount.IsHardfile())
            continue;
        const std::filesystem::path* rom = nullptr;
        if (mount.controller == HardDriveController::ScsiA590A2091)
            rom = &roms.a590A2091;
        else if (mount.controller == HardDriveController::ScsiA4091)
            rom = &roms.a4091;
        if (!rom)
            continue;
        std::error_code ec;
        if (rom->empty() || !std::filesystem::is_regular_file(*rom, ec)) {
            outError = fmt::format("{}: the {} card needs its boot ROM file",
                                   HostMountDeviceName(mount.device),
                                   HardDriveControllerName(mount.controller));
            return false;
        }
        if (HasUnsafeConfigCharacters(PathUtf8(*rom), false)) {
            outError = fmt::format("{}: the {} ROM path contains an unsafe character",
                                   HostMountDeviceName(mount.device),
                                   HardDriveControllerName(mount.controller));
            return false;
        }
    }
    return true;
}

bool UaeConfigBuilder::ValidateMounts(std::array<HostDirectoryMount, 2>& mounts,
                                      std::string& outError, bool testWritable)
{
    outError.clear();
    std::array<std::string, 2> canonicalPaths;
    std::array<std::string, 2> labels;

    for (size_t index = 0; index < mounts.size(); ++index) {
        auto& mount = mounts[index];
        if (mount.enabled && static_cast<size_t>(mount.device) != index) {
            outError = "duplicate or mismatched host-mount device assignment";
            return false;
        }
        mount.device = static_cast<HostMountDevice>(index);
        if (!mount.enabled)
            continue;
        if (mount.bootPriority < kMinBootPriority || mount.bootPriority > kMaxBootPriority) {
            outError = std::string(HostMountDeviceName(mount.device)) +
                       ": boot priority must be between -128 and 127";
            return false;
        }
        if (mount.backingType == HostMountBackingType::Archive) {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(mount.hostDirectory, ec);
            if (mount.hostDirectory.empty() || ec ||
                !std::filesystem::is_regular_file(canonical, ec) || ec) {
                outError = std::string(HostMountDeviceName(mount.device)) +
                           ": archive is missing or is not a regular file";
                return false;
            }
            mount.hostDirectory = canonical;
            const std::string path = PathUtf8(canonical);
            if (HasUnsafeConfigCharacters(path, false) || mount.volumeLabel.empty() ||
                HasUnsafeConfigCharacters(mount.volumeLabel, true)) {
                outError = std::string(HostMountDeviceName(mount.device)) +
                           ": archive path or volume label contains an unsafe character";
                return false;
            }
            // An archive filesystem cannot be written.
            mount.readOnly = true;
            canonicalPaths[index] = LowerAscii(path);
            labels[index] = LowerAscii(mount.volumeLabel);
            continue;
        }
        if (mount.IsHardfile()) {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(mount.hardfilePath, ec);
            if (mount.hardfilePath.empty() || ec ||
                !std::filesystem::is_regular_file(canonical, ec) || ec) {
                outError = std::string(HostMountDeviceName(mount.device)) +
                           ": hardfile is missing or is not a regular file";
                return false;
            }
            mount.hardfilePath = canonical;
            std::string path;
            try {
                path = PathUtf8(canonical);
            } catch (...) {
                outError = std::string(HostMountDeviceName(mount.device)) +
                           ": hardfile path cannot be encoded as UTF-8";
                return false;
            }
            if (path.empty() || HasUnsafeConfigCharacters(path, false)) {
                outError = std::string(HostMountDeviceName(mount.device)) +
                           ": hardfile path contains quotes, newlines, or control characters";
                return false;
            }
            if (mount.controllerUnit < 0 ||
                mount.controllerUnit > MaxControllerUnit(mount.controller)) {
                outError = fmt::format(
                    "{}: {} unit must be 0-{}", HostMountDeviceName(mount.device),
                    HardDriveControllerName(mount.controller), MaxControllerUnit(mount.controller));
                return false;
            }
            // UAE decompresses a .hdz into memory, so a guest write could never reach the file.
            if (IsCompressedHardfile(canonical))
                mount.readOnly = true;
            if (mount.geometry.mode == HardfileGeometryMode::Manual &&
                !HardfileGeometryFields(mount, outError))
                return false;
            if (!mount.readOnly && testWritable && !CheckWritableFile(canonical, outError))
                return false;
            canonicalPaths[index] = LowerAscii(path);
            continue;
        }

        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(mount.hostDirectory, ec);
        if (ec || !std::filesystem::is_directory(canonical, ec) || ec) {
            outError = std::string(HostMountDeviceName(mount.device)) +
                       ": host path is missing or is not a directory";
            return false;
        }
        mount.hostDirectory = canonical;

        std::string path;
        try {
            path = PathUtf8(canonical);
        } catch (...) {
            outError = std::string(HostMountDeviceName(mount.device)) +
                       ": host path cannot be encoded as UTF-8";
            return false;
        }
        if (path.empty() || HasUnsafeConfigCharacters(path, false)) {
            outError = std::string(HostMountDeviceName(mount.device)) +
                       ": host path contains quotes, newlines, or control characters";
            return false;
        }
        if (mount.volumeLabel.empty() || HasUnsafeConfigCharacters(mount.volumeLabel, true)) {
            outError = std::string(HostMountDeviceName(mount.device)) +
                       ": volume label is empty or contains an unsafe character";
            return false;
        }
        if (!mount.readOnly && testWritable && !CheckWritableDirectory(canonical, outError))
            return false;

        canonicalPaths[index] = LowerAscii(path);
        labels[index] = LowerAscii(mount.volumeLabel);
    }

    if (mounts[0].enabled && mounts[1].enabled) {
        if (canonicalPaths[0] == canonicalPaths[1]) {
            outError = mounts[0].IsHardfile()
                           ? "DH0 and DH1 cannot use the same hardfile"
                           : "DH0 and DH1 cannot use the same canonical host directory";
            return false;
        }
        if (mounts[0].IsHardfile() && mounts[1].IsHardfile() &&
            mounts[0].controller == mounts[1].controller &&
            mounts[0].controller != HardDriveController::Uae &&
            mounts[0].controllerUnit == mounts[1].controllerUnit) {
            outError = fmt::format("DH0 and DH1 are both on {} unit {}",
                                   HardDriveControllerName(mounts[0].controller),
                                   mounts[0].controllerUnit);
            return false;
        }
        if (!mounts[0].IsHardfile() && !mounts[1].IsHardfile() && !labels[0].empty() &&
            labels[0] == labels[1]) {
            outError = "DH0 and DH1 cannot use the same Amiga volume label";
            return false;
        }
    }
    return true;
}

std::optional<std::string> UaeConfigBuilder::SerializeMount(const HostDirectoryMount& mount,
                                                            std::string& outError)
{
    if (!mount.enabled)
        return std::string{};
    if (mount.IsHardfile()) {
        const std::string path = PathUtf8(mount.hardfilePath);
        if (HasUnsafeConfigCharacters(path, false)) {
            outError = std::string(HostMountDeviceName(mount.device)) +
                       ": hardfile path is unsafe for UAE configuration";
            return std::nullopt;
        }
        auto geometry = HardfileGeometryFields(mount, outError);
        if (!geometry)
            return std::nullopt;
        // hardfile2=access,DEV:"path",sectors,surfaces,reserved,blocksize,bootpri,filesys,controller
        return fmt::format("hardfile2={},{}:\"{}\",{},{},,{}\n", mount.readOnly ? "ro" : "rw",
                           HostMountDeviceName(mount.device), path, *geometry, mount.bootPriority,
                           ControllerToken(mount.controller, mount.controllerUnit));
    }
    const std::string path = PathUtf8(mount.hostDirectory);
    if (HasUnsafeConfigCharacters(path, false) ||
        HasUnsafeConfigCharacters(mount.volumeLabel, true)) {
        outError = std::string(HostMountDeviceName(mount.device)) +
                   ": mount contains a value unsafe for UAE configuration";
        return std::nullopt;
    }
    return std::string("filesystem2=") + (mount.readOnly ? "ro," : "rw,") +
           HostMountDeviceName(mount.device) + ":" + mount.volumeLabel + ":\"" + path + "\"," +
           std::to_string(mount.bootPriority) + "\n";
}

std::optional<std::string> UaeConfigBuilder::Build(const EmulatorLaunchProfile& sourceProfile,
                                                   std::string& outError)
{
    EmulatorLaunchProfile profile = sourceProfile;
    if (!ValidateMounts(profile.mounts, outError))
        return std::nullopt;

    std::string config;
    if (profile.baseUaeConfig) {
        std::error_code ec;
        const auto basePath = std::filesystem::weakly_canonical(*profile.baseUaeConfig, ec);
        if (ec || !std::filesystem::is_regular_file(basePath, ec) || ec) {
            outError = "base UAE configuration is missing or is not a regular file";
            return std::nullopt;
        }
        std::ifstream stream(basePath, std::ios::binary);
        if (!stream) {
            outError = "unable to open base UAE configuration";
            return std::nullopt;
        }
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            config += RebaseConfigLine(line, basePath.parent_path()) + "\n";
        }
        config += "\n; PEEKaboo directory-backed launch profile\n";
    } else {
        config = "config_description=PEEKaboo directory-backed launch\nuse_gui=no\n";
    }

    if (profile.minimalBootRom)
        config += "boot_rom_uae=min\n";
    if (!ValidateControllerRoms(profile.mounts, profile.controllerRoms, outError))
        return std::nullopt;
    const auto usesController = [&](HardDriveController controller) {
        return std::any_of(profile.mounts.begin(), profile.mounts.end(), [&](const auto& mount) {
            return mount.enabled && mount.IsHardfile() && mount.controller == controller;
        });
    };
    // A card is in the machine because its ROM line is in the config. Emit it only when a drive
    // is attached, so a remembered ROM path never adds a board nobody asked for.
    if (usesController(HardDriveController::ScsiA590A2091))
        config += "a2091_rom_file=" + PathUtf8(profile.controllerRoms.a590A2091) + "\n";
    if (usesController(HardDriveController::ScsiA4091))
        config += "a4091_rom_file=" + PathUtf8(profile.controllerRoms.a4091) + "\n";
    const auto serializeExtra = [&](const ExtraVolume& volume) -> bool {
        const std::string path = PathUtf8(volume.hostDirectory);
        if (volume.device.empty() || HasUnsafeConfigCharacters(volume.device, true) ||
            HasUnsafeConfigCharacters(volume.volumeLabel, true) ||
            HasUnsafeConfigCharacters(path, false) || path.empty()) {
            outError = "volume " + volume.device + " is unsafe for UAE configuration";
            return false;
        }
        config += std::string("filesystem2=") + (volume.readOnly ? "ro," : "rw,") + volume.device +
                  ":" + volume.volumeLabel + ":\"" + path + "\"," +
                  std::to_string(volume.bootPriority) + "\n";
        return true;
    };
    for (const auto& volume : profile.volumesBefore) {
        if (!serializeExtra(volume))
            return std::nullopt;
    }
    for (const auto& mount : profile.mounts) {
        auto line = SerializeMount(mount, outError);
        if (!line)
            return std::nullopt;
        config += *line;
    }
    for (const auto& volume : profile.volumesAfter) {
        if (!serializeExtra(volume))
            return std::nullopt;
    }
    if (!profile.cdImage.empty()) {
        const std::string image = PathUtf8(profile.cdImage);
        if (HasUnsafeConfigCharacters(image, false)) {
            outError = "the CD image path contains an unsafe character";
            return std::nullopt;
        }
        if (!MachineRoms::IsCdMachine(profile.modelId))
            config += "scsi=true\n";
        config += "cdimage0=\"" + image + "\",\n";
    }
    return config;
}

bool UaeConfigBuilder::WriteAtomic(const std::filesystem::path& path, const std::string& contents,
                                   std::string& outError)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        outError = "unable to create generated-config directory: " + ec.message();
        return false;
    }
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary =
        path.parent_path() / (path.filename().string() + ".tmp." + std::to_string(nonce));
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream ||
            !stream.write(contents.data(), static_cast<std::streamsize>(contents.size()))) {
            outError = "unable to write temporary UAE configuration";
            std::filesystem::remove(temporary, ec);
            return false;
        }
        stream.flush();
        if (!stream) {
            outError = "unable to flush temporary UAE configuration";
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        outError = "unable to replace generated UAE configuration (Windows error " +
                   std::to_string(GetLastError()) + ")";
        std::filesystem::remove(temporary, ec);
        return false;
    }
#else
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        outError = "unable to replace generated UAE configuration: " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
#endif
    return true;
}

HostMountService::HostMountService()
{
    for (size_t index = 0; index < m_configured.size(); ++index) {
        auto device = static_cast<HostMountDevice>(index);
        m_configured[index].device = device;
        m_configured[index].volumeLabel = HostMountDeviceName(device);
        m_effective[index] = m_configured[index];
    }
}

void HostMountService::SetConfiguredMount(HostMountDevice device, const HostDirectoryMount& mount)
{
    const size_t index = static_cast<size_t>(device);
    const auto previous = m_configured[index];
    m_configured[index] = mount;
    m_configured[index].device = device;
    m_configured[index].source = HostMountSource::Preferences;
    m_lastError.clear();
    if (previous != m_configured[index]) {
        if (previous.enabled && !m_configured[index].enabled)
            Logger::Info("Host mounts: {} unmount requested; restart pending",
                         HostMountDeviceName(device));
        else if (previous.enabled)
            Logger::Info("Host mounts: {} replacement requested; restart pending",
                         HostMountDeviceName(device));
        LogMount("requested", m_configured[index]);
    }
}

void HostMountService::SetConfiguredMounts(const std::array<HostDirectoryMount, 2>& mounts)
{
    for (size_t index = 0; index < mounts.size(); ++index)
        SetConfiguredMount(static_cast<HostMountDevice>(index), mounts[index]);
}

void HostMountService::SetControllerRoms(const HardDriveControllerRoms& roms)
{
    if (m_controllerRoms == roms)
        return;
    m_controllerRoms = roms;
    Logger::Info("Host mounts: controller ROMs set (A590/A2091 '{}', A4091 '{}')",
                 PathUtf8(roms.a590A2091), PathUtf8(roms.a4091));
}

bool HostMountService::IsGuestConfirmable(const HostDirectoryMount& mount)
{
    return !mount.IsHardfile() || mount.controller == HardDriveController::Uae;
}

EmulatorLaunchProfile HostMountService::CreateConfiguredProfile(
    const std::optional<std::filesystem::path>& baseConfig) const
{
    EmulatorLaunchProfile profile;
    profile.baseUaeConfig = baseConfig;
    profile.mounts = m_configured;
    profile.controllerRoms = m_controllerRoms;
    const bool directoryBoot =
        std::any_of(profile.mounts.begin(), profile.mounts.end(),
                    [](const auto& mount) { return mount.enabled && mount.bootPriority > -128; });
    profile.bootIntent = directoryBoot ? EmulatorBootIntent::Directory : EmulatorBootIntent::Floppy;
    return profile;
}

bool HostMountService::PrepareLaunch(const EmulatorLaunchProfile& profile,
                                     const std::filesystem::path& workspace,
                                     std::filesystem::path& outConfig, std::string& outError,
                                     EmulatorLaunchProfile* outEffectiveProfile)
{
    for (const auto& mount : profile.mounts)
        LogMount("requested", mount);
    EmulatorLaunchProfile effectiveProfile = profile;
    if (!UaeConfigBuilder::ValidateMounts(effectiveProfile.mounts, outError)) {
        Logger::Error("Host mounts: generated-config validation failed: {}", outError);
        MarkFailed(outError, false);
        return false;
    }
    auto config = UaeConfigBuilder::Build(effectiveProfile, outError);
    if (!config) {
        Logger::Error("Host mounts: generated-config build failed: {}", outError);
        MarkFailed(outError, false);
        return false;
    }
    outConfig = workspace / "launch.uae";
    if (!UaeConfigBuilder::WriteAtomic(outConfig, *config, outError)) {
        Logger::Error("Host mounts: generated-config write failed: {}", outError);
        MarkFailed(outError, false);
        return false;
    }
    if (outEffectiveProfile)
        *outEffectiveProfile = std::move(effectiveProfile);
    return true;
}

void HostMountService::MarkEffective(const EmulatorLaunchProfile& profile)
{
    const bool wasTransient = m_transientOverride;
    m_effectiveProfile = profile;
    m_effective = profile.mounts;
    ++m_effectiveGeneration;
    const bool preferenceProfile =
        !profile.transient &&
        std::all_of(profile.mounts.begin(), profile.mounts.end(),
                    [](const auto& mount) { return mount.source == HostMountSource::Preferences; });
    if (preferenceProfile)
        m_configured = profile.mounts;
    m_transientOverride = profile.transient;
    m_lastError.clear();
    m_activity = {};
    m_confirmed = {};
    m_effectiveControllerRoms = profile.controllerRoms;
    if (!profile.transient)
        m_controllerRoms = profile.controllerRoms;
    // Unit numbers follow config order: whatever the base config mounts, then the generated
    // lines in the order UaeConfigBuilder::Build writes them.
    m_unitDevices.clear();
    if (profile.baseUaeConfig) {
        std::ifstream stream(*profile.baseUaeConfig, std::ios::binary);
        std::string line;
        while (std::getline(stream, line)) {
            const std::string lowered = LowerAscii(Trim(line));
            if (lowered.starts_with("filesystem2=") || lowered.starts_with("hardfile2="))
                m_unitDevices.push_back(std::nullopt);
        }
    }
    m_unitDevices.insert(m_unitDevices.end(), profile.volumesBefore.size(), std::nullopt);
    for (size_t index = 0; index < m_effective.size(); ++index) {
        if (m_effective[index].enabled)
            m_unitDevices.push_back(static_cast<HostMountDevice>(index));
    }
    m_unitDevices.insert(m_unitDevices.end(), profile.volumesAfter.size(), std::nullopt);
    for (const auto& mount : m_effective)
        LogMount("effective", mount);
    for (const auto& volume : profile.volumesBefore)
        Logger::Info("Host mounts: effective {} '{}' path='{}' priority={}", volume.device,
                     volume.volumeLabel, PathUtf8(volume.hostDirectory), volume.bootPriority);
    for (const auto& volume : profile.volumesAfter)
        Logger::Info("Host mounts: effective {} '{}' path='{}' priority={}", volume.device,
                     volume.volumeLabel, PathUtf8(volume.hostDirectory), volume.bootPriority);
    if (!wasTransient && m_transientOverride)
        Logger::Info("Host mounts: transient override activated");
    else if (wasTransient && !m_transientOverride)
        Logger::Info("Host mounts: transient override restored saved configuration");
    m_waitingForConfirmation =
        std::any_of(m_effective.begin(), m_effective.end(),
                    [](const auto& mount) { return mount.enabled && IsGuestConfirmable(mount); });
    m_confirmationDeadline = std::chrono::steady_clock::now() + kMountConfirmationTimeout;
    m_confirmationTimeoutReported = false;
}

void HostMountService::MarkFailed(const std::string& error, bool logError)
{
    if (logError && m_lastError != error)
        Logger::Error("Host mounts: launch failed: {}", error);
    m_lastError = error;
    m_waitingForConfirmation = false;
}

void HostMountService::ClearEffective()
{
    const bool hadEffective = std::any_of(m_effective.begin(), m_effective.end(),
                                          [](const auto& mount) { return mount.enabled; });
    const bool wasTransient = m_transientOverride;
    m_effectiveProfile.reset();
    m_effective = {};
    ++m_effectiveGeneration;
    for (size_t index = 0; index < m_effective.size(); ++index) {
        m_effective[index].device = static_cast<HostMountDevice>(index);
        m_effective[index].volumeLabel = HostMountDeviceName(m_effective[index].device);
    }
    m_transientOverride = false;
    m_activity = {};
    m_confirmed = {};
    m_unitDevices.clear();
    m_effectiveControllerRoms = {};
    m_waitingForConfirmation = false;
    m_confirmationTimeoutReported = false;
    if (hadEffective)
        Logger::Info("Host mounts: effective hard drives unmounted");
    if (wasTransient)
        Logger::Info("Host mounts: transient override ended; saved configuration restored");
}

bool HostMountService::HasPendingRestart() const
{
    if (m_transientOverride)
        return std::any_of(m_configured.begin(), m_configured.end(),
                           [](const auto& mount) { return mount.enabled; });
    if (m_configured != m_effective)
        return true;
    // A ROM change only matters to a running machine that has a drive on that card.
    const auto onCard = [&](HardDriveController controller) {
        return std::any_of(m_effective.begin(), m_effective.end(), [&](const auto& mount) {
            return mount.enabled && mount.IsHardfile() && mount.controller == controller;
        });
    };
    return (onCard(HardDriveController::ScsiA590A2091) &&
            m_controllerRoms.a590A2091 != m_effectiveControllerRoms.a590A2091) ||
           (onCard(HardDriveController::ScsiA4091) &&
            m_controllerRoms.a4091 != m_effectiveControllerRoms.a4091);
}

std::optional<HostMountDevice> HostMountService::DropTargetDevice() const
{
    // DH0 first, then DH1: a file dropped on the machine belongs on the boot volume unless that
    // one cannot take it.
    for (const auto& mount : m_effective) {
        if (!mount.enabled || mount.readOnly)
            continue;
        // Only a plain directory. A hardfile is an image whose filesystem the host has no
        // business writing into behind the guest's back, and an archive is read-only by nature.
        if (mount.backingType != HostMountBackingType::Directory)
            continue;
        std::error_code ec;
        if (!std::filesystem::is_directory(mount.hostDirectory, ec))
            continue;
        return mount.device;
    }
    return std::nullopt;
}

std::optional<std::string> HostMountService::CopyFileToGuest(const std::filesystem::path& file,
                                                             std::string& outError) const
{
    outError.clear();

    std::error_code ec;
    if (file.empty() || !std::filesystem::is_regular_file(file, ec)) {
        outError = "that is not a file on this machine";
        return std::nullopt;
    }

    const auto device = DropTargetDevice();
    if (!device) {
        outError = "no writable directory is mounted - give DH0 or DH1 a host directory and turn "
                   "off read-only in Hard Drives";
        return std::nullopt;
    }
    const auto& mount = m_effective[static_cast<size_t>(*device)];

    // AmigaDOS has no use for a host path's directory part, and a name with a colon or a slash
    // in it would be read as a path by the guest.
    std::string name = file.filename().string();
    if (name.empty() || name.find_first_of(":/\\") != std::string::npos) {
        outError = "'" + name + "' is not a name AmigaDOS can hold";
        return std::nullopt;
    }

    // Never overwrite. A file the guest has open, or one the user forgot about, is worse to lose
    // than to end up with "picture.1.iff" beside the original.
    std::filesystem::path destination = mount.hostDirectory / name;
    if (std::filesystem::exists(destination, ec)) {
        const std::string stem = file.stem().string();
        const std::string extension = file.extension().string();
        for (int suffix = 1; suffix < 1000; ++suffix) {
            name = stem + "." + std::to_string(suffix) + extension;
            destination = mount.hostDirectory / name;
            if (!std::filesystem::exists(destination, ec))
                break;
        }
        if (std::filesystem::exists(destination, ec)) {
            outError = "there are already too many files by that name on " +
                       std::string(HostMountDeviceName(*device));
            return std::nullopt;
        }
    }

    std::filesystem::copy_file(file, destination, ec);
    if (ec) {
        outError = "could not copy onto " + std::string(HostMountDeviceName(*device)) + ": " +
                   ec.message();
        return std::nullopt;
    }

    const std::string guestPath = std::string(HostMountDeviceName(*device)) + ":" + name;
    Logger::Info("Host mounts: copied '{}' to {}", file.string(), guestPath);
    return guestPath;
}

std::optional<HostMountDevice> HostMountService::DeviceForUnit(int unit) const
{
    if (unit < 0 || static_cast<size_t>(unit) >= m_unitDevices.size())
        return std::nullopt;
    return m_unitDevices[static_cast<size_t>(unit)];
}

void HostMountService::RecordActivity(int unit, int status, uint32_t count)
{
    if (status != 1 && status != 2)
        return;
    auto device = DeviceForUnit(unit);
    if (!device)
        return;
    const size_t index = static_cast<size_t>(*device);
    auto& activity = m_activity[index];
    if (status == 2 && m_effective[index].readOnly && !activity.readOnlyViolationReported) {
        Logger::Warn("Host mounts: guest write reported for read-only {} ('{}')",
                     HostMountDeviceName(*device), m_effective[index].volumeLabel);
        activity.readOnlyViolationReported = true;
    }
    if (status == 1)
        activity.reads += count;
    else
        activity.writes += count;
    activity.latchedStatus = std::max(activity.latchedStatus, status);
    activity.recentStatus = status;
    activity.recentUntil = std::chrono::steady_clock::now() + kActivityDisplayDuration;
}

int HostMountService::ConsumeActivity(HostMountDevice device)
{
    auto& activity = m_activity[static_cast<size_t>(device)];
    const int status = activity.latchedStatus;
    activity.latchedStatus = 0;
    return status;
}

int HostMountService::GetRecentActivityStatus(HostMountDevice device,
                                              std::chrono::steady_clock::time_point now) const
{
    const auto& activity = m_activity[static_cast<size_t>(device)];
    return now < activity.recentUntil ? activity.recentStatus : 0;
}

void HostMountService::ConfirmActiveUnits(const std::vector<int>& units)
{
    for (int unit : units) {
        const auto device = DeviceForUnit(unit);
        if (!device)
            continue;
        const size_t index = static_cast<size_t>(*device);
        if (!m_confirmed[index]) {
            m_confirmed[index] = true;
            Logger::Info("Host mounts: guest confirmed {} mounted as '{}'",
                         HostMountDeviceName(*device), m_effective[index].volumeLabel);
        }
    }
    m_waitingForConfirmation = false;
    for (size_t index = 0; index < m_effective.size(); ++index) {
        if (m_effective[index].enabled && IsGuestConfirmable(m_effective[index]) &&
            !m_confirmed[index]) {
            m_waitingForConfirmation = true;
            break;
        }
    }
}

bool HostMountService::CheckConfirmationTimeout(std::chrono::steady_clock::time_point now)
{
    if (!m_waitingForConfirmation || m_confirmationTimeoutReported || now < m_confirmationDeadline)
        return false;
    m_confirmationTimeoutReported = true;
    for (size_t index = 0; index < m_effective.size(); ++index) {
        if (m_effective[index].enabled && IsGuestConfirmable(m_effective[index]) &&
            !m_confirmed[index])
            Logger::Warn("Host mounts: timed out waiting for guest confirmation of {} ('{}')",
                         HostMountDeviceName(static_cast<HostMountDevice>(index)),
                         m_effective[index].volumeLabel);
    }
    return true;
}

HostMountRuntimeState HostMountService::GetRuntimeState(HostMountDevice device) const
{
    const size_t index = static_cast<size_t>(device);
    if (!m_lastError.empty() && m_configured[index].enabled)
        return HostMountRuntimeState::Failed;
    if (m_confirmed[index])
        return HostMountRuntimeState::Confirmed;
    if (m_effective[index].enabled)
        return m_configured[index] == m_effective[index] ? HostMountRuntimeState::Effective
                                                         : HostMountRuntimeState::PendingRestart;
    if (m_configured[index].enabled)
        return m_effectiveProfile ? HostMountRuntimeState::PendingRestart
                                  : HostMountRuntimeState::Configured;
    return HostMountRuntimeState::Disabled;
}

} // namespace adm::core
