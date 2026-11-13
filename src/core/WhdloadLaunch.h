#pragma once

#include "HostMountService.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace adm::core {

// How WHDLoad itself behaves, written into WHDLoad.prefs by the core before each launch.
struct WhdloadSettings
{
    bool buttonWait = true; // slaves may wait for a button on their loading screens
    // On by default: with it off WHDLoad buffers saves in memory and only writes them when
    // QuitKey is pressed, and a machine that is simply stopped never presses it.
    bool noWriteCache = true;
    int prefsMode = 0; // 0 none, 1 config window, 2 splash window, 3 both
    int theme = 0;     // 0 default (black), 1 native (grey)

    bool operator==(const WhdloadSettings&) const = default;
};

// What a WHDLoad launch mounts as DH0.
struct WhdloadSource
{
    std::filesystem::path input;     // what the user picked
    std::filesystem::path mountPath; // the archive, or the directory that holds the slave
    std::string volumeLabel;
    bool isArchive = false; // .lha/.lzx, mounted read-only through UAE's archive filesystem
};

// A WHDLoad boot built from Peekaboo's own generated configuration, the way the core's content
// loader does it for an .lha: WHDLoad: (the embedded WHDLoad boot files, priority 0) boots and its
// startup-sequence runs the slave it finds on DH0:, with WHDSaves: beside it for saved games.
class WhdloadLaunch
{
public:
    // Accepts an .lha/.lzx archive, a game directory, or a .slave/.info inside one.
    static std::optional<WhdloadSource> Resolve(const std::filesystem::path& input,
                                                std::string& outError);

    // The launch profile: `base` (the configured profile, so DH1, the controller ROMs and any
    // base config survive) with WHDLoad: ahead of the drives, the game on DH0 and WHDSaves:
    // after them. Transient, like a Bartman run: saved DH0 settings are untouched.
    static EmulatorLaunchProfile BuildProfile(const WhdloadSource& source,
                                              const std::filesystem::path& whdloadDirectory,
                                              const std::filesystem::path& savesDirectory,
                                              EmulatorLaunchProfile base);

    // An AmigaDOS volume label: no ':', ',', quotes or control characters, at most 30 chars.
    static std::string SanitizeLabel(const std::string& name);

    static bool IsArchiveExtension(const std::filesystem::path& path);

    // Copies the Kickstart images slaves load (kick34005.A500 and friends) from `romDirectory`
    // into WHDLoad:Devs/Kickstarts, beside the .RTB relocation tables the payload ships. Takes
    // the core's names or the Amiga Forever ones, checks the size, and never overwrites. Returns
    // how many are in place afterwards; `outMissing` names the ones that could not be found.
    static int InstallKickstarts(const std::filesystem::path& whdloadDirectory,
                                 const std::filesystem::path& romDirectory,
                                 std::vector<std::string>* outMissing = nullptr);
};

const char* WhdloadPrefsModeName(int mode);

} // namespace adm::core
