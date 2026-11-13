#include "CdMedia.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

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

constexpr uintmax_t k256K = 262144;
constexpr uintmax_t k512K = 524288;
constexpr uintmax_t k1M = 1048576;

// From the core's uae_kickstarts[] (libretro-core.h): normal, Amiga Forever, TOSEC-mod, TOSEC.
RomRequirement Cd32Kickstart()
{
    return {"Kickstart",
            {"kick40060.CD32", "amiga-os-310-cd32.rom",
             "Kickstart v3.1 rev 40.60 (1993)(Commodore)(CD32).rom"},
            {k512K, k1M}};
}

RomRequirement Cd32Extended()
{
    return {"extended ROM",
            {"kick40060.CD32.ext", "amiga-os-310-cd32-ext.rom",
             "CD32 Extended-ROM rev 40.60 (1993)(Commodore)(CD32).rom"},
            {k512K}};
}

std::optional<std::filesystem::path> FindRom(const RomRequirement& requirement,
                                             const std::filesystem::path& directory)
{
    std::error_code ec;
    for (const auto& name : requirement.names) {
        const auto candidate = directory / std::filesystem::u8path(name);
        if (!std::filesystem::is_regular_file(candidate, ec))
            continue;
        const auto size = std::filesystem::file_size(candidate, ec);
        // An Amiga Forever image may be encrypted (11 bytes longer); the core accepts it when
        // rom.key sits beside it, so allow that too.
        const bool sizeOk = requirement.sizes.empty() ||
                            std::any_of(requirement.sizes.begin(), requirement.sizes.end(),
                                        [&](uintmax_t s) { return size == s || size == s + 11; });
        if (sizeOk)
            return candidate;
    }
    return std::nullopt;
}

} // namespace

bool MachineRoms::IsCdMachine(const std::string& modelId)
{
    return modelId == "CD32" || modelId == "CD32FR" || modelId == "CDTV";
}

std::vector<RomRequirement> MachineRoms::ExtraRequirements(const std::string& modelId)
{
    if (modelId == "CD32" || modelId == "CD32FR")
        return {Cd32Kickstart(), Cd32Extended()};
    if (modelId == "CDTV")
        return {{"extended ROM",
                 {"kick34005.CDTV", "amiga-os-130-cdtv-ext.rom",
                  "CDTV Extended-ROM v1.0 (1991)(Commodore)(CDTV)[!].rom",
                  "CDTV Extended-ROM v1.0 (1992)(Commodore)(CDTV).rom"},
                 {k256K}}};
    return {};
}

std::string MachineRoms::MissingRoms(const std::string& modelId,
                                     const std::filesystem::path& romDirectory)
{
    auto requirements = ExtraRequirements(modelId);
    if (requirements.empty())
        return {};
    std::vector<std::string> missing;
    bool combinedCd32 = false;
    for (const auto& requirement : requirements) {
        const auto found = FindRom(requirement, romDirectory);
        // A 1 MB CD32 Kickstart already contains the extended ROM (the core decides by size).
        if (found && requirement.role == "Kickstart" &&
            (modelId == "CD32" || modelId == "CD32FR")) {
            std::error_code ec;
            combinedCd32 = std::filesystem::file_size(*found, ec) >= k1M;
        }
        if (!found && !(combinedCd32 && requirement.role == "extended ROM"))
            missing.push_back(
                fmt::format("{} {} (or {})", requirement.role, requirement.names[0],
                            requirement.names.size() > 1 ? requirement.names[1] : std::string()));
    }
    if (missing.empty())
        return {};
    std::string text = "needs ";
    for (size_t i = 0; i < missing.size(); ++i)
        text += (i ? " and " : "") + missing[i];
    return text + " in " + Utf8(romDirectory);
}

bool CdImageResolver::IsImageExtension(const std::filesystem::path& path)
{
    const std::string extension = Lower(path.extension().string());
    return extension == ".cue" || extension == ".ccd" || extension == ".mds" ||
           extension == ".nrg" || extension == ".chd" || extension == ".iso";
}

bool CdImageResolver::IsArchiveExtension(const std::filesystem::path& path)
{
    const std::string extension = Lower(path.extension().string());
    return extension == ".7z" || extension == ".zip";
}

std::optional<std::filesystem::path>
CdImageResolver::FindImage(const std::filesystem::path& directory)
{
    static const char* kPreference[] = {".cue", ".ccd", ".mds", ".nrg", ".chd", ".iso"};
    std::vector<std::filesystem::path> images;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(directory, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && IsImageExtension(it->path()))
            images.push_back(it->path());
    }
    for (const char* extension : kPreference) {
        std::optional<std::filesystem::path> best;
        uintmax_t bestSize = 0;
        for (const auto& image : images) {
            if (Lower(image.extension().string()) != extension)
                continue;
            const auto size = std::filesystem::file_size(image, ec);
            if (!best || size > bestSize) {
                best = image;
                bestSize = size;
            }
        }
        if (best)
            return best;
    }
    return std::nullopt;
}

namespace {

std::string MarkerText(const std::filesystem::path& archive)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(archive, ec);
    const auto time = std::filesystem::last_write_time(archive, ec).time_since_epoch().count();
    return fmt::format("{}\n{}\n{}\n", Utf8(archive.filename()), size, time);
}

constexpr const char* kMarkerName = ".peekaboo-extracted";

} // namespace

bool CdImageResolver::WriteExtractionMarker(const std::filesystem::path& archive,
                                            const std::filesystem::path& directory)
{
    std::ofstream stream(directory / kMarkerName, std::ios::binary | std::ios::trunc);
    stream << MarkerText(archive);
    return static_cast<bool>(stream);
}

bool CdImageResolver::HasValidExtractionMarker(const std::filesystem::path& archive,
                                               const std::filesystem::path& directory)
{
    std::ifstream stream(directory / kMarkerName, std::ios::binary);
    if (!stream)
        return false;
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str() == MarkerText(archive);
}

std::optional<CdImageSource> CdImageResolver::Resolve(const std::filesystem::path& input,
                                                      const std::filesystem::path& cacheRoot,
                                                      std::string& outError)
{
    std::error_code ec;
    if (input.empty() || !std::filesystem::is_regular_file(input, ec)) {
        outError = "no such disc image: " + Utf8(input);
        return std::nullopt;
    }
    CdImageSource source;
    source.original = std::filesystem::weakly_canonical(input, ec);
    if (IsImageExtension(input)) {
        source.image = source.original;
        return source;
    }
    if (!IsArchiveExtension(input)) {
        outError = "not a disc image (.cue, .ccd, .mds, .nrg, .chd, .iso) or an archive of one "
                   "(.7z, .zip): " +
                   Utf8(input);
        return std::nullopt;
    }
    // One cache directory per archive: a short readable prefix and a hash of the full name. Short
    // matters - the core writes each extracted file with plain fopen, so the cache path plus a
    // TOSEC track name (60-odd characters) has to stay inside MAX_PATH.
    std::string folder;
    for (unsigned char ch : Utf8(input.stem())) {
        if (folder.size() == 16)
            break;
        folder.push_back(
            std::isalnum(ch) || ch == '-' || ch == '_' || ch == ' ' ? static_cast<char>(ch) : '_');
    }
    while (!folder.empty() && folder.back() == ' ')
        folder.pop_back();
    uint32_t hash = 2166136261u; // FNV-1a over the file name
    for (unsigned char ch : Utf8(input.filename())) {
        hash ^= ch;
        hash *= 16777619u;
    }
    folder += fmt::format("_{:08x}", hash);
    source.extractDirectory = cacheRoot / std::filesystem::u8path(folder);
    if (HasValidExtractionMarker(source.original, source.extractDirectory)) {
        if (const auto image = FindImage(source.extractDirectory)) {
            source.image = *image;
            return source;
        }
    }
    source.needsExtraction = true;
    return source;
}

} // namespace adm::core
