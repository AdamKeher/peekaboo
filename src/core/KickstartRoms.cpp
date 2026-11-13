#include "KickstartRoms.h"

#include <algorithm>
#include <system_error>

#include <fmt/format.h>

#include "AmigaEmulator.h"
#include "Logger.h"

namespace adm::core {

std::string RomIdentity::DisplayName() const
{
    if (identified && !name.empty())
        return name;
    return path.filename().string();
}

std::string RomIdentity::VersionLabel() const
{
    if (!identified)
        return {};
    // Both numbers: "1.3" is what people call it, "34.5" is what the file is named after.
    return fmt::format("Kickstart {}.{} (rev {}.{})", version, revision, romVersion, romRevision);
}

std::string RomIdentity::DetailLine() const
{
    std::string detail;
    if (size > 0) {
        // Kickstarts are whole numbers of KB, so there is no need for a decimal here.
        detail = fmt::format("{} KB", size / 1024);
    }
    if (identified) {
        if (!detail.empty())
            detail += ", ";
        detail += fmt::format("CRC {:08X}", crc32);
        if (!model.empty())
            detail += fmt::format(", {}", model);
        if (encrypted)
            detail += ", encrypted";
    } else if (!detail.empty()) {
        detail += ", not in the ROM database";
    }
    return detail;
}

bool KickstartRoms::Available(const adm::AmigaEmulator& emulator)
{
    return emulator.HasRomIdentification();
}

std::optional<RomIdentity> KickstartRoms::Identify(const adm::AmigaEmulator& emulator,
                                                   const std::filesystem::path& file,
                                                   const std::filesystem::path& keyDirectory)
{
    if (!emulator.HasRomIdentification())
        return std::nullopt;

    RomIdentity identity;
    identity.path = file;
    const int result = emulator.IdentifyRom(file, keyDirectory, identity);
    if (result < 0)
        return std::nullopt; // unreadable: not a ROM answer at all
    identity.identified = result > 0;
    return identity;
}

std::vector<RomIdentity> KickstartRoms::Scan(const adm::AmigaEmulator& emulator,
                                             const std::filesystem::path& directory)
{
    std::vector<RomIdentity> found;
    if (!emulator.HasRomIdentification())
        return found;

    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec))
        return found;

    // Not recursive: the ROM directory is the core's system directory, which also holds save
    // states, NVRAM and the WHDLoad cache. Walking into those would cost far more than it found.
    for (const auto& entry : std::filesystem::directory_iterator(
             directory, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec)
            break;
        if (!entry.is_regular_file(ec))
            continue;
        // The export caps what it will read, but skipping the obvious non-ROMs first saves
        // hashing every disk image in the directory.
        const auto size = entry.file_size(ec);
        if (ec || size < 128u * 1024u || size > 2u * 1024u * 1024u)
            continue;
        auto identity = Identify(emulator, entry.path(), directory);
        if (identity && identity->identified)
            found.push_back(*identity);
    }

    // Oldest first, by the Kickstart version people know them by, then by ROM revision so two
    // builds of the same version keep a stable order.
    std::sort(found.begin(), found.end(), [](const RomIdentity& a, const RomIdentity& b) {
        if (a.version != b.version)
            return a.version < b.version;
        if (a.revision != b.revision)
            return a.revision < b.revision;
        if (a.romVersion != b.romVersion)
            return a.romVersion < b.romVersion;
        if (a.romRevision != b.romRevision)
            return a.romRevision < b.romRevision;
        return a.DisplayName() < b.DisplayName();
    });
    return found;
}

} // namespace adm::core
