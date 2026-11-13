#include "HostExecutableLauncher.h"

#include <array>
#include <fstream>
#include <system_error>

namespace adm::core {
namespace {

bool WriteTextFile(const std::filesystem::path& path, const std::string& contents,
                   std::string& outError)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        outError = "unable to open " + path.string() + " for writing";
        return false;
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.close();
    if (stream.fail()) {
        outError = "unable to write " + path.string();
        return false;
    }
    return true;
}

} // namespace

bool HostExecutableLauncher::ValidateHunk(const std::filesystem::path& programPath,
                                          std::string& outError)
{
    outError.clear();
    if (!programPath.is_absolute()) {
        outError = "program path must be absolute";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(programPath, ec) || ec) {
        outError = "program does not exist or is not a regular file";
        return false;
    }

    std::ifstream stream(programPath, std::ios::binary);
    std::array<unsigned char, 4> header{};
    if (!stream.read(reinterpret_cast<char*>(header.data()), header.size())) {
        outError = "program is too small to be an Amiga HUNK executable";
        return false;
    }

    if (header != std::array<unsigned char, 4>{0x00, 0x00, 0x03, 0xF3}) {
        outError = "program does not begin with the HUNK_HEADER magic";
        return false;
    }

    const std::string filename = programPath.filename().string();
    if (filename.empty() || filename.find_first_of("\"\r\n:") != std::string::npos) {
        outError = "program filename contains a character that AmigaDOS cannot launch safely";
        return false;
    }
    return true;
}

std::string HostExecutableLauncher::BuildStartupSequence(const std::string& filename)
{
    return "\"DH1:" + filename + "\"\n";
}

std::optional<HostExecutableLaunchFiles>
HostExecutableLauncher::Prepare(const std::filesystem::path& programPath,
                                const std::filesystem::path& workspaceDirectory,
                                std::string& outError)
{
    if (!ValidateHunk(programPath, outError))
        return std::nullopt;

    std::error_code ec;
    const auto canonicalProgram = std::filesystem::weakly_canonical(programPath, ec);
    if (ec) {
        outError = "unable to resolve program path: " + ec.message();
        return std::nullopt;
    }

    if (!workspaceDirectory.is_absolute()) {
        outError = "launcher workspace path must be absolute";
        return std::nullopt;
    }

    const auto bootDirectory = workspaceDirectory / "dh0";
    const auto startupDirectory = bootDirectory / "S";
    std::filesystem::create_directories(startupDirectory, ec);
    if (ec) {
        outError = "unable to create launcher workspace: " + ec.message();
        return std::nullopt;
    }

    HostExecutableLaunchFiles files;
    files.programPath = canonicalProgram;
    files.programDirectory = canonicalProgram.parent_path();
    files.workspaceDirectory = workspaceDirectory;
    files.bootDirectory = bootDirectory;
    files.startupSequencePath = startupDirectory / "startup-sequence";
    files.profile.transient = true;
    files.profile.minimalBootRom = true;
    files.profile.bootIntent = EmulatorBootIntent::Directory;
    files.profile.mounts[0] = {HostMountBackingType::Directory,
                               HostMountDevice::DH0,
                               true,
                               files.bootDirectory,
                               "PeekabooBoot",
                               false,
                               0,
                               HostMountSource::TransientBartman};
    files.profile.mounts[1] = {HostMountBackingType::Directory,
                               HostMountDevice::DH1,
                               true,
                               files.programDirectory,
                               "PeekabooBuild",
                               true,
                               -128,
                               HostMountSource::TransientBartman};

    if (!WriteTextFile(files.startupSequencePath,
                       BuildStartupSequence(files.programPath.filename().string()), outError)) {
        return std::nullopt;
    }

    return files;
}

} // namespace adm::core
