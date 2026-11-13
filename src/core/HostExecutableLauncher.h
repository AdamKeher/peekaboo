#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "HostMountService.h"

namespace adm::core {

struct HostExecutableLaunchFiles
{
    std::filesystem::path programPath;
    std::filesystem::path programDirectory;
    std::filesystem::path workspaceDirectory;
    std::filesystem::path bootDirectory;
    std::filesystem::path startupSequencePath;
    EmulatorLaunchProfile profile;
};

// Creates the small host-side boot environment used for Bartman-style fast launches. The
// executable itself is never copied: PuAE sees its build directory through a read-only DH1 mount.
class HostExecutableLauncher
{
public:
    static bool ValidateHunk(const std::filesystem::path& programPath, std::string& outError);

    static std::string BuildStartupSequence(const std::string& filename);
    static std::optional<HostExecutableLaunchFiles>
    Prepare(const std::filesystem::path& programPath,
            const std::filesystem::path& workspaceDirectory, std::string& outError);
};

} // namespace adm::core
