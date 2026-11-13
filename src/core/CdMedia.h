#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace adm::core {

// One ROM a machine preset needs, under every name the core looks for it by
// (retro_config_kickstart: its own name, the Amiga Forever name, the TOSEC names).
struct RomRequirement
{
    std::string role; // "Kickstart", "extended ROM"
    std::vector<std::string> names;
    std::vector<uintmax_t> sizes; // accepted sizes; empty = any
};

// ROM checks for the presets whose ROMs Peekaboo does not ship: CD32, CD32FR and CDTV.
class MachineRoms
{
public:
    // The ROMs `modelId` ("CD32", "CD32FR", "CDTV") needs beyond what every preset needs; empty
    // for the floppy and hard-disk machines.
    static std::vector<RomRequirement> ExtraRequirements(const std::string& modelId);
    // Empty when every requirement is met in `romDirectory`, otherwise which files are missing.
    static std::string MissingRoms(const std::string& modelId,
                                   const std::filesystem::path& romDirectory);
    // Whether the preset has a CD drive of its own.
    static bool IsCdMachine(const std::string& modelId);
};

// A disc the CD drive can take: an image used as it is, or an archive (.7z/.zip, the TOSEC CD32
// sets) that has to be extracted into the cache first.
struct CdImageSource
{
    std::filesystem::path original;         // what the user picked
    std::filesystem::path image;            // the image the core is given (set once available)
    std::filesystem::path extractDirectory; // where an archive unpacks
    bool needsExtraction = false;
};

class CdImageResolver
{
public:
    static bool IsImageExtension(const std::filesystem::path& path);
    static bool IsArchiveExtension(const std::filesystem::path& path);

    // Resolves an image directly, or an archive to its cache directory under `cacheRoot`. A
    // cached extraction that is complete (its marker matches the archive's size and time) is
    // reused and needs no extraction.
    static std::optional<CdImageSource> Resolve(const std::filesystem::path& input,
                                                const std::filesystem::path& cacheRoot,
                                                std::string& outError);

    // The best image in an extracted directory tree: a .cue first (it carries the audio tracks),
    // then .ccd, .mds, .nrg, .chd, and the largest .iso last.
    static std::optional<std::filesystem::path> FindImage(const std::filesystem::path& directory);

    // Marks an extraction of `archive` into `directory` as complete.
    static bool WriteExtractionMarker(const std::filesystem::path& archive,
                                      const std::filesystem::path& directory);
    static bool HasValidExtractionMarker(const std::filesystem::path& archive,
                                         const std::filesystem::path& directory);
};

} // namespace adm::core
