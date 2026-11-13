#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "KickstartRoms.h"

namespace adm::core {

// Finding the user's own Amiga system files - Kickstart ROMs, Workbench disks and hardfiles - and
// copying the ROMs into Peekaboo's ROM folder under the names the machine presets look for. None
// of these ship with Peekaboo; the setup wizard and Preferences >
// System Files are the two faces of this.
//
// ROMs are identified by content through a callback - in the app KickstartRoms::Identify, which
// needs the core loaded - so everything here is testable without one.
using RomIdentifier = std::function<std::optional<RomIdentity>(
    const std::filesystem::path& file, const std::filesystem::path& keyDir)>;

// A folder worth looking in, and whose it is ("Amiga Forever", "FS-UAE", "WinUAE").
struct SearchFolder
{
    std::filesystem::path path;
    std::string label;
};

// The usual homes of other emulators' system files, the ones that exist on this machine: Amiga
// Forever's Shared folder (Public Documents, then the user's own), FS-UAE's Kickstarts, WinUAE's
// Roms. `env` reads an environment variable (empty when unset); tests give their own.
std::vector<SearchFolder>
DefaultSearchFolders(const std::function<std::string(const char*)>& env = {});

// An ADF that is an AmigaOS disk, by its volume name.
struct FoundDisk
{
    std::filesystem::path path;
    std::string volume; // "Workbench1.3"
};

struct FoundHardfile
{
    std::filesystem::path path;
    uint64_t size = 0;
};

struct SystemFileScan
{
    std::vector<RomIdentity> roms; // Kickstarts and extended ROMs, one per CRC, oldest first
    std::vector<std::filesystem::path> romKeys; // every rom.key seen
    std::vector<FoundDisk> disks;               // AmigaOS disks (Workbench, Extras, Install, ...)
    std::vector<FoundHardfile> hardfiles;       // .hdf
    size_t entriesVisited = 0;
    bool truncated = false; // a limit stopped the walk before the end of the tree
};

struct ScanLimits
{
    int maxDepth = 6;          // below the folder given
    size_t maxEntries = 60000; // files and folders looked at; a whole drive stops here
};

// Walks `root` for system files. A ROM-sized file (128 KB - 2 MB, plus Amiga Forever's 11-byte
// header) is offered to `identify`, with its own folder as the rom.key folder; only Kickstarts and
// extended ROMs are kept. ADFs are kept when their volume is an AmigaOS disk's.
SystemFileScan ScanForSystemFiles(const std::filesystem::path& root, const RomIdentifier& identify,
                                  const ScanLimits& limits = {});

// Adds `more` to `into`: ROMs by CRC, the rest by path.
void MergeScan(SystemFileScan& into, const SystemFileScan& more);

// The file name the machine presets load this ROM by ("kick34005.A500", "kick40068.A1200",
// "kick40060.CD32.ext", ...). Empty for a ROM no preset uses (an A3000 ROM, Kickstart 3.2, ...):
// it is still copied, under its own name, and can be picked in Preferences.
std::string CanonicalRomName(const RomIdentity& rom);

// The Amiga Forever name the core also accepts for a canonical one ("amiga-os-130.rom"), or empty.
std::string AmigaForeverRomName(const std::string& canonical);

// The volume name in an ADF's root block, when it is a DD or HD AmigaDOS disk.
std::optional<std::string> AdfVolumeName(const std::filesystem::path& adf);
// An AmigaOS disk's volume: Workbench, Extras, Install, Storage, Locale, Fonts.
bool IsSystemDiskVolume(const std::string& volume);
// A disk the machine boots to a Workbench from.
bool IsWorkbenchVolume(const std::string& volume);

// Amiga Forever's encrypted ROM ("AMIROMTYPE1" + the image XORed with rom.key) as the plain
// image; nullopt when `file` is not one or the key is empty.
std::optional<std::vector<uint8_t>> DecodeCloantoRom(const std::vector<uint8_t>& file,
                                                     const std::vector<uint8_t>& key);

struct RomCopy
{
    enum class Outcome {
        Copied,
        AlreadyThere, // the same bytes under that name
        KeptExisting, // a different file has the name; it was not overwritten
        Failed,
    };
    std::filesystem::path source;
    std::filesystem::path target;
    Outcome outcome = Outcome::Failed;
    std::string detail; // why, for KeptExisting and Failed
};

// Copies `roms` into `romDir` under CanonicalRomName (or their own name), decoding Amiga
// Forever's encrypted ones with a rom.key from `romKeys` - the one beside the ROM first. Each
// copy is identified again and must match its source's CRC, or it is removed and reported. One
// copy per CRC. Never overwrites a different file.
std::vector<RomCopy> CopyRomsInto(const std::filesystem::path& romDir,
                                  const std::vector<RomIdentity>& roms,
                                  const std::vector<std::filesystem::path>& romKeys,
                                  const RomIdentifier& identify);

// What a family of machine presets loads, for the setup wizard's machine list.
struct MachineRomSet
{
    const char* label;   // "Amiga 500 / 2000"
    const char* modelId; // the preset to select for it (AmigaModelInfo::id)
    const char* kickstart;
    std::vector<const char*> roms; // canonical names, all needed
};
const std::vector<MachineRomSet>& MachineRomNeeds();

// The ROMs any preset (AmigaModelInfo::id: "A500OG", "A4040RTG", ...) loads, canonical names;
// empty for an unknown id. AmigaEmulator::SelectModelPreset's choices.
std::vector<std::string> PresetRomNames(const std::string& modelId);

// The file in `romDir` the core would load for a canonical ROM name - that name, its Amiga
// Forever one or a TOSEC one (libretro-core.h's uae_kickstarts[]) - when its content really is
// that ROM (with no `identify`, by name alone). nullopt when there is none.
std::optional<std::filesystem::path> FindPresetRom(const std::filesystem::path& romDir,
                                                   const std::string& canonical,
                                                   const RomIdentifier& identify);

// Whether `dir` holds an Amiga Forever encrypted ROM (an "AMIROMTYPE1" file), which only loads
// with a rom.key beside it.
bool HasEncryptedRoms(const std::filesystem::path& dir);

} // namespace adm::core
