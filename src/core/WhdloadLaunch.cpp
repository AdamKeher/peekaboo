#include "WhdloadLaunch.h"

#include <algorithm>
#include <cctype>

namespace adm::core {
namespace {

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string Utf8(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

} // namespace

const char* WhdloadPrefsModeName(int mode)
{
    switch (mode) {
        case 1:
            return "config";
        case 2:
            return "splash";
        case 3:
            return "both";
        default:
            return "none";
    }
}

bool WhdloadLaunch::IsArchiveExtension(const std::filesystem::path& path)
{
    const std::string extension = Lower(path.extension().string());
    return extension == ".lha" || extension == ".lzh" || extension == ".lzx";
}

std::string WhdloadLaunch::SanitizeLabel(const std::string& name)
{
    std::string label;
    for (unsigned char ch : name) {
        if (ch < 0x20 || ch == 0x7f || ch == ':' || ch == ',' || ch == '"' || ch == '/' ||
            ch == '\\')
            label.push_back('_');
        else
            label.push_back(static_cast<char>(ch));
        if (label.size() == 30)
            break;
    }
    return label.empty() ? std::string("Game") : label;
}

std::optional<WhdloadSource> WhdloadLaunch::Resolve(const std::filesystem::path& input,
                                                    std::string& outError)
{
    std::error_code ec;
    const auto path = std::filesystem::weakly_canonical(input, ec);
    if (ec || input.empty()) {
        outError = "no WHDLoad archive or directory given";
        return std::nullopt;
    }

    WhdloadSource source;
    source.input = path;
    if (std::filesystem::is_directory(path, ec)) {
        source.mountPath = path;
    } else if (std::filesystem::is_regular_file(path, ec)) {
        const std::string extension = Lower(path.extension().string());
        if (IsArchiveExtension(path)) {
            source.mountPath = path;
            source.isArchive = true;
        } else if (extension == ".slave" || extension == ".info") {
            // As the core resolves a launched .slave/.info (retro_config_harddrives): the
            // directory named like the file when it exists (Game.info beside Game/), otherwise
            // the directory holding the file.
            const auto named = path.parent_path() / path.stem();
            source.mountPath =
                std::filesystem::is_directory(named, ec) ? named : path.parent_path();
        } else {
            outError = "not a WHDLoad archive (.lha, .lzx), game directory, .slave or .info: " +
                       Utf8(path);
            return std::nullopt;
        }
    } else {
        outError = "no such file or directory: " + Utf8(path);
        return std::nullopt;
    }

    const auto nameSource =
        source.isArchive ? source.mountPath.stem() : source.mountPath.filename();
    source.volumeLabel = SanitizeLabel(Utf8(nameSource));
    return source;
}

int WhdloadLaunch::InstallKickstarts(const std::filesystem::path& whdloadDirectory,
                                     const std::filesystem::path& romDirectory,
                                     std::vector<std::string>* outMissing)
{
    struct Kickstart
    {
        const char* name;
        const char* amigaForever;
        uintmax_t size;
    };
    // The images WHDLoad slaves ask for by name, as the core's whdload_kscopy lists them (plus
    // the A4000 3.1 its payload also carries a relocation table for).
    static constexpr Kickstart kKickstarts[] = {
        {"kick33180.A500", "amiga-os-120.rom", 262144},
        {"kick34005.A500", "amiga-os-130.rom", 262144},
        {"kick40063.A600", "amiga-os-310-a600.rom", 524288},
        {"kick40068.A1200", "amiga-os-310-a1200.rom", 524288},
        {"kick40068.A4000", "amiga-os-310-a4000.rom", 524288},
    };

    const auto destination = whdloadDirectory / "Devs" / "Kickstarts";
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    int present = 0;
    for (const auto& kickstart : kKickstarts) {
        const auto target = destination / kickstart.name;
        if (std::filesystem::is_regular_file(target, ec)) {
            ++present;
            continue;
        }
        bool copied = false;
        for (const char* candidate : {kickstart.name, kickstart.amigaForever}) {
            const auto source = romDirectory / candidate;
            // An Amiga Forever image that is encrypted is 11 bytes longer and needs rom.key,
            // which WHDLoad cannot use; only a plain image of the right size is copied.
            if (!std::filesystem::is_regular_file(source, ec) ||
                std::filesystem::file_size(source, ec) != kickstart.size)
                continue;
            if (std::filesystem::copy_file(source, target, ec) && !ec) {
                copied = true;
                break;
            }
        }
        if (copied)
            ++present;
        else if (outMissing)
            outMissing->push_back(kickstart.name);
    }
    return present;
}

EmulatorLaunchProfile WhdloadLaunch::BuildProfile(const WhdloadSource& source,
                                                  const std::filesystem::path& whdloadDirectory,
                                                  const std::filesystem::path& savesDirectory,
                                                  EmulatorLaunchProfile base)
{
    EmulatorLaunchProfile profile = std::move(base);
    profile.transient = true;
    profile.minimalBootRom = false;
    profile.bootIntent = EmulatorBootIntent::Directory;

    auto& game = profile.mounts[0];
    game = HostDirectoryMount{};
    game.device = HostMountDevice::DH0;
    game.enabled = true;
    game.backingType =
        source.isArchive ? HostMountBackingType::Archive : HostMountBackingType::Directory;
    game.hostDirectory = source.mountPath;
    game.volumeLabel = source.volumeLabel;
    // An archive is read-only by nature; a game directory is left writable, as the core does,
    // because some installs keep their high scores beside the slave.
    game.readOnly = source.isArchive;
    game.bootPriority = -128;
    game.source = HostMountSource::TransientWhdload;

    // Whatever else is attached must not boot ahead of WHDLoad:.
    auto& other = profile.mounts[1];
    if (other.enabled) {
        other.bootPriority = -128;
        other.source = HostMountSource::TransientWhdload;
        if (!other.IsHardfile() && other.volumeLabel == game.volumeLabel)
            other.volumeLabel += "_DH1";
    }

    profile.volumesBefore = {{"WHDLoad", "WHDLoad", whdloadDirectory, false, 0}};
    profile.volumesAfter = {{"WHDSaves", "WHDSaves", savesDirectory, false, -128}};
    return profile;
}

} // namespace adm::core
