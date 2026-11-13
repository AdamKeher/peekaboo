#include "SystemFiles.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>

namespace adm::core {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kAdfDD = 901120;
constexpr uint64_t kAdfHD = 1802240;
constexpr size_t kCloantoHeader = 11; // "AMIROMTYPE1"

std::string Lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool StartsWithNoCase(const std::string& s, const char* prefix)
{
    const size_t n = std::strlen(prefix);
    return s.size() >= n && Lower(s.substr(0, n)) == Lower(prefix);
}

std::string GetEnv(const char* name)
{
    const char* v = std::getenv(name);
    return v ? v : "";
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// Sorted as KickstartRoms::Scan sorts: by the Kickstart version people know, then the revision.
bool OlderRom(const RomIdentity& a, const RomIdentity& b)
{
    if (a.version != b.version)
        return a.version < b.version;
    if (a.revision != b.revision)
        return a.revision < b.revision;
    if (a.romVersion != b.romVersion)
        return a.romVersion < b.romVersion;
    if (a.romRevision != b.romRevision)
        return a.romRevision < b.romRevision;
    return a.path < b.path;
}

// Whether `a` is the better file of one ROM: plain beats encrypted (which needs its key), then the
// smaller beats an overdump (Amiga Forever's 512 KB "large" 1.3 is the 256 KB ROM twice, and
// identifies the same; WHDLoad wants the 256 KB one).
bool BetterRomFile(const RomIdentity& a, const RomIdentity& b)
{
    if (a.encrypted != b.encrypted)
        return !a.encrypted;
    std::error_code ea, eb;
    const auto sa = fs::file_size(a.path, ea), sb = fs::file_size(b.path, eb);
    return !ea && !eb && sa < sb;
}

// One ROM per CRC, the best file of it.
void AddRom(std::vector<RomIdentity>& roms, const RomIdentity& rom)
{
    for (auto& have : roms) {
        if (have.crc32 != rom.crc32)
            continue;
        if (BetterRomFile(rom, have))
            have = rom;
        return;
    }
    roms.push_back(rom);
}

// Every name the core loads a preset's ROM by: libretro-core.h's uae_kickstarts[] - the
// canonical name, Amiga Forever's, and the two TOSEC spellings ("" where it has none).
struct RomAlias
{
    const char* canonical;
    const char* amigaForever;
    const char* tosecMod;
    const char* tosec;
};
constexpr RomAlias kRomAliases[] = {
    {"kick31034.A1000", "amiga-os-110-ntsc.rom", "",
     "Kickstart v1.1 rev 31.34 (1985)(Commodore)(A1000)(NTSC).rom"},
    {"kick32034.A1000", "amiga-os-110-pal.rom", "",
     "Kickstart v1.1 rev 32.34 (1986)(Commodore)(A1000)(PAL).rom"},
    {"kick33180.A500", "amiga-os-120.rom",
     "Kickstart v1.2 rev 33.180 (1986)(Commodore)(A500-A2000)[!].rom",
     "Kickstart v1.2 rev 33.180 (1986)(Commodore)(A500-A1000-A2000).rom"},
    {"kick34005.A500", "amiga-os-130.rom",
     "Kickstart v1.3 rev 34.5 (1987)(Commodore)(A500-A1000-A2000-CDTV)[!].rom",
     "Kickstart v1.3 rev 34.5 (1987)(Commodore)(A500-A1000-A2000-CDTV).rom"},
    {"kick37175.A500", "amiga-os-204.rom",
     "Kickstart v2.04 rev 37.175 (1991)(Commodore)(A500+)[!].rom",
     "Kickstart v2.04 rev 37.175 (1991)(Commodore)(A500+).rom"},
    {"kick37350.A600", "amiga-os-205-a600.rom",
     "Kickstart v2.05 rev 37.350 (1992)(Commodore)(A600HD)[!].rom", ""},
    {"kick40063.A600", "amiga-os-310-a600.rom",
     "Kickstart v3.1 rev 40.63 (1993)(Commodore)(A500-A600-A2000)[!].rom",
     "Kickstart v3.1 rev 40.63 (1993)(Commodore)(A500-A600-A2000).rom"},
    {"kick39106.A1200", "amiga-os-300-a1200.rom",
     "Kickstart v3.0 rev 39.106 (1992)(Commodore)(A1200)[!].rom", ""},
    {"kick40068.A1200", "amiga-os-310-a1200.rom",
     "Kickstart v3.1 rev 40.68 (1993)(Commodore)(A1200)[!].rom",
     "Kickstart v3.1 rev 40.68 (1993)(Commodore)(A1200).rom"},
    {"kick39106.A4000", "amiga-os-300-a4000.rom",
     "Kickstart v3.0 rev 39.106 (1992)(Commodore)(A4000)[!].rom", ""},
    {"kick40068.A4000", "amiga-os-310-a4000.rom", "",
     "Kickstart v3.1 rev 40.68 (1993)(Commodore)(A4000).rom"},
    {"kick34005.CDTV", "amiga-os-130-cdtv-ext.rom",
     "CDTV Extended-ROM v1.0 (1991)(Commodore)(CDTV)[!].rom",
     "CDTV Extended-ROM v1.0 (1992)(Commodore)(CDTV).rom"},
    {"kick40060.CD32", "amiga-os-310-cd32.rom", "",
     "Kickstart v3.1 rev 40.60 (1993)(Commodore)(CD32).rom"},
    {"kick40060.CD32.ext", "amiga-os-310-cd32-ext.rom", "",
     "CD32 Extended-ROM rev 40.60 (1993)(Commodore)(CD32).rom"},
};

} // namespace

std::vector<SearchFolder> DefaultSearchFolders(const std::function<std::string(const char*)>& env)
{
    const auto get = env ? env : GetEnv;
    std::vector<SearchFolder> folders;
    std::set<fs::path> seen;
    auto add = [&](const std::string& base, const fs::path& rest, const char* label) {
        if (base.empty())
            return;
        const fs::path path = fs::path(base) / rest;
        std::error_code ec;
        if (!fs::is_directory(path, ec) || !seen.insert(path).second)
            return;
        folders.push_back({path, label});
    };
    // Amiga Forever puts its ROMs, ADFs, hardfiles and System/Work folders in "Amiga Files\Shared"
    // under Public Documents (the default) or the user's own Documents.
    add(get("PUBLIC"), fs::path("Documents") / "Amiga Files" / "Shared", "Amiga Forever");
    add(get("USERPROFILE"), fs::path("Documents") / "Amiga Files" / "Shared", "Amiga Forever");
    // FS-UAE keeps a Kickstarts folder in its base directory, Documents/FS-UAE on every platform.
    add(get("USERPROFILE"), fs::path("Documents") / "FS-UAE" / "Kickstarts", "FS-UAE");
    add(get("HOME"), fs::path("Documents") / "FS-UAE" / "Kickstarts", "FS-UAE");
    add(get("HOME"), fs::path("FS-UAE") / "Kickstarts", "FS-UAE");
    // WinUAE's ROM folder beside its executable.
    add(get("ProgramFiles"), fs::path("WinUAE") / "Roms", "WinUAE");
    add(get("ProgramFiles(x86)"), fs::path("WinUAE") / "Roms", "WinUAE");
    return folders;
}

SystemFileScan ScanForSystemFiles(const fs::path& root, const RomIdentifier& identify,
                                  const ScanLimits& limits)
{
    SystemFileScan scan;
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return scan;

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (++scan.entriesVisited > limits.maxEntries) {
            scan.truncated = true;
            break;
        }
        const auto& entry = *it;
        std::error_code entryEc;
        if (entry.is_directory(entryEc)) {
            if (it.depth() >= limits.maxDepth)
                it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(entryEc))
            continue;

        const fs::path& path = entry.path();
        const std::string name = Lower(path.filename().string());
        const std::string ext = Lower(path.extension().string());
        const uint64_t size = entry.file_size(entryEc);
        if (entryEc)
            continue;

        if (name == "rom.key") {
            scan.romKeys.push_back(path);
        } else if (ext == ".adf") {
            if (size == kAdfDD || size == kAdfHD) {
                if (auto volume = AdfVolumeName(path); volume && IsSystemDiskVolume(*volume))
                    scan.disks.push_back({path, *volume});
            }
        } else if (ext == ".hdf") {
            scan.hardfiles.push_back({path, size});
        } else if (size >= 128u * 1024u && size <= 2u * 1024u * 1024u + kCloantoHeader &&
                   identify) {
            // Disk images and archives of that size are not ROMs; everything else is asked about.
            static const std::set<std::string> kNotRoms = {".zip", ".lha", ".lzx", ".dms", ".ipf",
                                                           ".adz", ".exe", ".dll", ".gz",  ".7z"};
            if (kNotRoms.count(ext))
                continue;
            auto identity = identify(path, path.parent_path());
            if (identity && identity->identified &&
                (identity->IsKickstart() || identity->IsExtended()))
                AddRom(scan.roms, *identity);
        }
    }
    if (ec && !scan.truncated)
        scan.truncated = scan.entriesVisited > 0; // the walk failed part way

    std::sort(scan.roms.begin(), scan.roms.end(), OlderRom);
    return scan;
}

void MergeScan(SystemFileScan& into, const SystemFileScan& more)
{
    for (const auto& rom : more.roms)
        AddRom(into.roms, rom);
    std::sort(into.roms.begin(), into.roms.end(), OlderRom);
    auto addPaths = [](auto& to, const auto& from, auto pathOf) {
        for (const auto& item : from) {
            const bool have = std::any_of(to.begin(), to.end(),
                                          [&](const auto& t) { return pathOf(t) == pathOf(item); });
            if (!have)
                to.push_back(item);
        }
    };
    addPaths(into.romKeys, more.romKeys, [](const fs::path& p) { return p; });
    addPaths(into.disks, more.disks, [](const FoundDisk& d) { return d.path; });
    addPaths(into.hardfiles, more.hardfiles, [](const FoundHardfile& h) { return h.path; });
    into.entriesVisited += more.entriesVisited;
    into.truncated = into.truncated || more.truncated;
}

std::string CanonicalRomName(const RomIdentity& rom)
{
    if (!rom.identified)
        return {};
    const auto fits = [&rom](const char* machine) {
        return rom.model.find(machine) != std::string::npos;
    };
    const bool kickCd32 = (rom.type & (kRomTypeKickCd32 | kRomTypeCd32)) != 0;
    const bool extCd32 = (rom.type & kRomTypeExtCd32) != 0;
    // A combined 1 MB CD32 image is neither half, and no preset loads it.
    if (extCd32 && !kickCd32)
        return "kick40060.CD32.ext";
    // The preset's is the CDTV's own v1.0 (the core's TOSEC names say so); v2.30, the CDTV/A570
    // one, is the same type and also lists the CDTV
    if ((rom.type & kRomTypeExtCdtv) && fits("CDTV") && rom.name.find("v1.0") != std::string::npos)
        return "kick34005.CDTV";
    if (kickCd32 && !extCd32 && rom.romVersion == 40 && rom.romRevision == 60)
        return "kick40060.CD32";
    if (!rom.IsKickstart() || kickCd32)
        return {};

    // Every revision also exists as another machine's ROM (the A3000's 1.3, 2.04 and 3.1), with
    // other contents, so the machine the database lists has to be the preset's too.
    const int v = rom.romVersion, r = rom.romRevision;
    // The database gives both A1000 1.1 ROMs revision 31.34; only its name tells PAL from NTSC.
    if ((v == 31 || v == 32) && r == 34 && fits("A1000")) {
        const bool pal = v == 32 || rom.name.find("PAL") != std::string::npos;
        return pal ? "kick32034.A1000" : "kick31034.A1000";
    }
    if (v == 33 && r == 180 && fits("A500"))
        return "kick33180.A500";
    if (v == 34 && r == 5 && fits("A500"))
        return "kick34005.A500";
    if (v == 37 && r == 175 && fits("A500"))
        return "kick37175.A500";
    if (v == 37 && r == 350 && fits("A600"))
        return "kick37350.A600";
    if (v == 40 && r == 63 && fits("A600"))
        return "kick40063.A600";
    if ((v == 39 && r == 106) || (v == 40 && r == 68)) {
        const char* rev = v == 39 ? "39106" : "40068";
        if (fits("A1200"))
            return std::string("kick") + rev + ".A1200";
        if (fits("A4000"))
            return std::string("kick") + rev + ".A4000";
    }
    return {};
}

std::string AmigaForeverRomName(const std::string& canonical)
{
    for (const auto& alias : kRomAliases) {
        if (canonical == alias.canonical)
            return alias.amigaForever;
    }
    return {};
}

std::optional<std::string> AdfVolumeName(const fs::path& adf)
{
    std::error_code ec;
    const uint64_t size = fs::file_size(adf, ec);
    if (ec || (size != kAdfDD && size != kAdfHD))
        return std::nullopt;
    // The root block sits mid-disk: block 880 on a DD disk, 1760 on an HD one.
    const uint64_t rootOffset = (size == kAdfDD ? 880u : 1760u) * 512u;
    std::ifstream in(adf, std::ios::binary);
    uint8_t block[512];
    if (!in.seekg(static_cast<std::streamoff>(rootOffset)) ||
        !in.read(reinterpret_cast<char*>(block), sizeof(block)))
        return std::nullopt;
    const auto be32 = [&block](size_t at) {
        return (uint32_t(block[at]) << 24) | (uint32_t(block[at + 1]) << 16) |
               (uint32_t(block[at + 2]) << 8) | uint32_t(block[at + 3]);
    };
    // T_HEADER and ST_ROOT, or it is not an AmigaDOS disk (a trackloader, NDOS)
    if (be32(0) != 2 || be32(508) != 1)
        return std::nullopt;
    const size_t length = std::min<size_t>(block[432], 30);
    return std::string(reinterpret_cast<const char*>(block + 433), length);
}

bool IsSystemDiskVolume(const std::string& volume)
{
    for (const char* prefix : {"Workbench", "Extras", "Install", "Storage", "Locale", "Fonts"}) {
        if (StartsWithNoCase(volume, prefix))
            return true;
    }
    return false;
}

bool IsWorkbenchVolume(const std::string& volume) { return StartsWithNoCase(volume, "Workbench"); }

std::optional<std::vector<uint8_t>> DecodeCloantoRom(const std::vector<uint8_t>& file,
                                                     const std::vector<uint8_t>& key)
{
    if (key.empty() || file.size() <= kCloantoHeader ||
        std::memcmp(file.data(), "AMIROMTYPE1", kCloantoHeader) != 0)
        return std::nullopt;
    // The image after the header, XORed with the key over and over (WinUAE's decode_cloanto_rom)
    std::vector<uint8_t> plain(file.begin() + kCloantoHeader, file.end());
    for (size_t i = 0; i < plain.size(); ++i)
        plain[i] ^= key[i % key.size()];
    return plain;
}

std::vector<RomCopy> CopyRomsInto(const fs::path& romDir, const std::vector<RomIdentity>& roms,
                                  const std::vector<fs::path>& romKeys,
                                  const RomIdentifier& identify)
{
    std::vector<RomCopy> results;
    std::error_code ec;
    fs::create_directories(romDir, ec);
    std::set<uint32_t> done;
    std::set<std::string> namesTaken; // by an earlier ROM of this copy

    for (const auto& rom : roms) {
        if (!rom.identified || !done.insert(rom.crc32).second)
            continue;
        RomCopy copy;
        copy.source = rom.path;
        // A second ROM for the same preset name (two CDTV extended ROMs, say) keeps its own name
        std::string canonical = CanonicalRomName(rom);
        if (!canonical.empty() && !namesTaken.insert(canonical).second)
            canonical.clear();
        copy.target = romDir / (canonical.empty() ? rom.path.filename() : fs::path(canonical));
        auto fail = [&](std::string why) {
            copy.outcome = RomCopy::Outcome::Failed;
            copy.detail = std::move(why);
            results.push_back(copy);
        };

        std::vector<uint8_t> bytes;
        if (!ReadFile(rom.path, bytes)) {
            fail("could not be read");
            continue;
        }
        // Encrypted by the file's own header, whatever the identity says
        if (bytes.size() > kCloantoHeader &&
            std::memcmp(bytes.data(), "AMIROMTYPE1", kCloantoHeader) == 0) {
            // The key beside the ROM first: Amiga Forever keeps them together.
            std::vector<fs::path> keys;
            for (const auto& k : romKeys) {
                if (k.parent_path() == rom.path.parent_path())
                    keys.insert(keys.begin(), k);
                else
                    keys.push_back(k);
            }
            std::optional<std::vector<uint8_t>> plain;
            for (const auto& k : keys) {
                std::vector<uint8_t> key;
                if (ReadFile(k, key) && (plain = DecodeCloantoRom(bytes, key)))
                    break;
            }
            if (!plain) {
                fail("is encrypted, and there is no rom.key to decode it with");
                continue;
            }
            bytes = std::move(*plain);
        }

        if (fs::exists(copy.target, ec)) {
            std::vector<uint8_t> existing;
            if (ReadFile(copy.target, existing) && existing == bytes) {
                copy.outcome = RomCopy::Outcome::AlreadyThere;
            } else {
                copy.outcome = RomCopy::Outcome::KeptExisting;
                copy.detail = "a different file is already called " +
                              copy.target.filename().string() + "; it was left as it is";
            }
            results.push_back(copy);
            continue;
        }

        // Written beside the target and renamed, so a failed write never leaves half a ROM
        // under a name the presets load.
        const fs::path temp = copy.target.string() + ".part";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                out.close();
                fs::remove(temp, ec);
                fail("could not be written to " + romDir.string());
                continue;
            }
        }
        fs::rename(temp, copy.target, ec);
        if (ec) {
            fs::remove(temp, ec);
            fail("could not be written to " + romDir.string());
            continue;
        }
        // The copy has to be the ROM it came from: a decode with the wrong key is caught here.
        if (identify) {
            auto check = identify(copy.target, romDir);
            if (!check || !check->identified || check->crc32 != rom.crc32) {
                fs::remove(copy.target, ec);
                fail("the copy did not identify as " + rom.DisplayName());
                continue;
            }
        }
        copy.outcome = RomCopy::Outcome::Copied;
        results.push_back(copy);
    }
    return results;
}

const std::vector<MachineRomSet>& MachineRomNeeds()
{
    // AmigaEmulator::SelectModelPreset's ROM for each preset, one row per ROM set.
    static const std::vector<MachineRomSet> kNeeds = {
        {"Amiga 500 / 2000", "A500", "Kickstart 1.3", {"kick34005.A500"}},
        {"Amiga 500+ / 2000", "A500+", "Kickstart 2.04", {"kick37175.A500"}},
        {"Amiga 600", "A600", "Kickstart 2.05", {"kick37350.A600"}},
        {"Amiga 1200", "A1200", "Kickstart 3.1", {"kick40068.A1200"}},
        {"Amiga 4000", "A4040", "Kickstart 3.1", {"kick40068.A4000"}},
        {"Amiga CDTV", "CDTV", "Kickstart 1.3 + CDTV", {"kick34005.A500", "kick34005.CDTV"}},
        {"Amiga CD32", "CD32", "Kickstart 3.1 CD32", {"kick40060.CD32", "kick40060.CD32.ext"}},
    };
    return kNeeds;
}

std::vector<std::string> PresetRomNames(const std::string& modelId)
{
    if (modelId == "A500OG" || modelId == "A500" || modelId == "A2000OG")
        return {"kick34005.A500"};
    if (modelId == "A500+" || modelId == "A2000")
        return {"kick37175.A500"};
    if (modelId == "A600")
        return {"kick37350.A600"};
    if (modelId == "A1200OG" || modelId == "A1200")
        return {"kick40068.A1200"};
    if (modelId == "A4030" || modelId == "A4040" || modelId == "A4040RTG")
        return {"kick40068.A4000"};
    if (modelId == "CDTV")
        return {"kick34005.A500", "kick34005.CDTV"};
    if (modelId == "CD32" || modelId == "CD32FR")
        return {"kick40060.CD32", "kick40060.CD32.ext"};
    return {};
}

bool HasEncryptedRoms(const fs::path& dir)
{
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code entryEc;
        const auto size = entry.file_size(entryEc);
        if (entryEc || !entry.is_regular_file(entryEc) || size < 128u * 1024u + kCloantoHeader ||
            size > 2u * 1024u * 1024u + kCloantoHeader)
            continue;
        char header[kCloantoHeader] = {};
        std::ifstream in(entry.path(), std::ios::binary);
        if (in.read(header, sizeof(header)) &&
            std::memcmp(header, "AMIROMTYPE1", kCloantoHeader) == 0)
            return true;
    }
    return false;
}

std::optional<fs::path> FindPresetRom(const fs::path& romDir, const std::string& canonical,
                                      const RomIdentifier& identify)
{
    std::vector<fs::path> candidates = {romDir / canonical};
    for (const auto& alias : kRomAliases) {
        if (canonical != alias.canonical)
            continue;
        for (const char* name : {alias.amigaForever, alias.tosecMod, alias.tosec}) {
            if (*name)
                candidates.push_back(romDir / fs::u8path(name));
        }
    }
    for (const auto& path : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
            continue;
        if (!identify)
            return path;
        // A file under the right name with the wrong contents is not believed.
        auto identity = identify(path, romDir);
        if (identity && identity->identified && CanonicalRomName(*identity) == canonical)
            return path;
    }
    return std::nullopt;
}

} // namespace adm::core
