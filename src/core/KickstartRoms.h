#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace adm {
class AmigaEmulator;
}

namespace adm::core {

// ROMTYPE_* from the core's rommgr.h, only the ones the host needs to tell apart.
inline constexpr uint32_t kRomTypeKick = 0x00000100;
inline constexpr uint32_t kRomTypeKickCd32 = 0x00000200;
inline constexpr uint32_t kRomTypeExtCd32 = 0x00000400;
inline constexpr uint32_t kRomTypeExtCdtv = 0x00000800;
inline constexpr uint32_t kRomTypeCd32 = 0x20000000;
inline constexpr uint32_t kRomTypeAllKick = kRomTypeKick | kRomTypeKickCd32 | kRomTypeCd32;
inline constexpr uint32_t kRomTypeAllExt = kRomTypeExtCd32 | kRomTypeExtCdtv;

// What the core's ROM database says a file is. `identified` false means the file was read but is
// not in the database - it may still be a perfectly good ROM, just not one WinUAE knows.
struct RomIdentity
{
    std::filesystem::path path;
    bool identified = false;
    std::string name;  // "KS ROM v1.3 (A500,A1000,A2000)", as the database words it
    std::string model; // "A500/A1000/A2000" - the database lists every machine it fits
    // Two different numbers, and the database carries both because neither implies the other:
    // the Kickstart version people say ("1.3", "3.1") and the ROM revision the files are named
    // after ("34.5" -> kick34005, "40.68" -> kick40068).
    int version = 0;     // Kickstart major, 1 for 1.3
    int revision = 0;    // Kickstart minor, 3 for 1.3
    int romVersion = 0;  // ROM revision major, 34 for kick34005
    int romRevision = 0; // ROM revision minor, 5 for kick34005
    uint32_t crc32 = 0;
    uint32_t size = 0;
    uint32_t type = 0; // ROMTYPE_* mask
    // The file is Amiga Forever's encrypted form ("AMIROMTYPE1" header), decoded through rom.key.
    // Read from the file itself: the database's own "cloanto" flag only says Cloanto sells it.
    bool encrypted = false;

    bool IsKickstart() const { return (type & kRomTypeAllKick) != 0; }
    bool IsExtended() const { return (type & kRomTypeAllExt) != 0; }
    // What to show in a list: the database's name, or the filename when it is unknown to it.
    std::string DisplayName() const;
    // "Kickstart 1.3 (rev 34.5)" or empty when the ROM is not in the database.
    std::string VersionLabel() const;
    // "256 KB, CRC 5F93FF4D, A500/A1000/A2000" - the detail line under the name.
    std::string DetailLine() const;
};

// Identifies Kickstart and extended ROMs by content, through the core's own database
// (adm_identify_rom, core 1.96.0). Filenames are never trusted: an Amiga Forever ROM, a TOSEC
// one and a hand-renamed one all identify the same.
class KickstartRoms
{
public:
    // Needs a loaded core. Returns nullopt when this DLL predates the export, so a caller can
    // say "rebuild the core" rather than "not a ROM".
    static std::optional<RomIdentity> Identify(const adm::AmigaEmulator& emulator,
                                               const std::filesystem::path& file,
                                               const std::filesystem::path& keyDirectory);

    // Every file in `directory` that identifies as a ROM, sorted by name. Files that do not
    // identify are left out: a ROM directory also holds rom.key, NVRAM and disk images.
    static std::vector<RomIdentity> Scan(const adm::AmigaEmulator& emulator,
                                         const std::filesystem::path& directory);

    // Whether the loaded core can identify ROMs at all.
    static bool Available(const adm::AmigaEmulator& emulator);
};

} // namespace adm::core
