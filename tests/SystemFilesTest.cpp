#include "core/SystemFiles.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/AmigaEmulator.h"
#include "core/KickstartRoms.h"

// The setup wizard's half in core: finding the user's system files, naming ROMs as the presets do,
// decoding Amiga Forever's encrypted ones, copying without overwriting. ROM identification is a
// callback, so most of this runs on fakes; the last tests check the naming against real ROMs and
// the real core where they are present.
namespace {

namespace fs = std::filesystem;
using adm::core::RomCopy;
using adm::core::RomIdentity;

class TempDir
{
public:
    TempDir()
    {
        static int n = 0;
        m_path =
            fs::temp_directory_path() /
            ("adm-sysfiles-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "-" + std::to_string(++n));
        fs::remove_all(m_path);
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    const fs::path& Path() const { return m_path; }

private:
    fs::path m_path;
};

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> ReadBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A fake ROM: 256 KB whose first byte says which ROM it is.
std::vector<uint8_t> FakeRom(uint8_t which)
{
    std::vector<uint8_t> rom(256 * 1024, 0xA5);
    rom[0] = which;
    return rom;
}

// Identifies a fake ROM by its first byte, as the core would by its CRC.
RomIdentity Fake(const fs::path& path, uint8_t which)
{
    RomIdentity id;
    id.path = path;
    id.identified = true;
    id.size = 256 * 1024;
    id.crc32 = 0x1000 + which;
    id.type = adm::core::kRomTypeKick;
    if (which == 0x34) {
        id.version = 1, id.revision = 3, id.romVersion = 34, id.romRevision = 5;
        id.model = "A500/A1000/A2000/CDTV";
        id.name = "KS ROM v1.3 (A500,A1000,A2000)";
    } else if (which == 0x40) {
        id.version = 3, id.revision = 1, id.romVersion = 40, id.romRevision = 68;
        id.model = "A1200";
        id.name = "KS ROM v3.1 (A1200)";
    } else if (which == 0x47) {
        id.version = 3, id.revision = 2, id.romVersion = 47, id.romRevision = 96;
        id.model = "A1200";
        id.name = "KS ROM v3.2 (A1200)";
    } else {
        id.identified = false;
    }
    return id;
}

int g_identifyCalls = 0;

adm::core::RomIdentifier FakeIdentifier()
{
    return [](const fs::path& file, const fs::path&) -> std::optional<RomIdentity> {
        ++g_identifyCalls;
        const auto bytes = ReadBytes(file);
        if (bytes.empty())
            return std::nullopt;
        return Fake(file, bytes[0]);
    };
}

// A DD ADF with an AmigaDOS root block naming the volume.
void WriteAdf(const fs::path& path, const std::string& volume, bool dos = true)
{
    std::vector<uint8_t> adf(901120, 0);
    uint8_t* root = adf.data() + 880 * 512;
    if (dos) {
        root[3] = 2;   // T_HEADER
        root[511] = 1; // ST_ROOT
    }
    root[432] = static_cast<uint8_t>(volume.size());
    std::copy(volume.begin(), volume.end(), root + 433);
    WriteBytes(path, adf);
}

TEST(SystemFiles, CanonicalNamesFollowTheMachinePresets)
{
    using adm::core::CanonicalRomName;
    EXPECT_EQ(CanonicalRomName(Fake("x", 0x34)), "kick34005.A500");
    EXPECT_EQ(CanonicalRomName(Fake("x", 0x40)), "kick40068.A1200");

    RomIdentity a4000 = Fake("x", 0x40);
    a4000.model = "A4000";
    EXPECT_EQ(CanonicalRomName(a4000), "kick40068.A4000");

    // The same revision on another board is other bytes, and no preset's
    RomIdentity a3000 = Fake("x", 0x34);
    a3000.model = "A3000";
    EXPECT_EQ(CanonicalRomName(a3000), "");
    EXPECT_EQ(CanonicalRomName(Fake("x", 0x47)), "") << "3.2: no preset loads it";
    EXPECT_EQ(CanonicalRomName(Fake("x", 0x00)), "") << "not identified";

    RomIdentity ext;
    ext.identified = true;
    ext.type = adm::core::kRomTypeExtCd32;
    EXPECT_EQ(CanonicalRomName(ext), "kick40060.CD32.ext");
    RomIdentity cd32;
    cd32.identified = true;
    cd32.type = adm::core::kRomTypeKickCd32;
    cd32.romVersion = 40, cd32.romRevision = 60;
    EXPECT_EQ(CanonicalRomName(cd32), "kick40060.CD32");
    RomIdentity combined = cd32;
    combined.type |= adm::core::kRomTypeExtCd32 | adm::core::kRomTypeCd32;
    EXPECT_EQ(CanonicalRomName(combined), "") << "a 1 MB CD32 image is neither half";
    RomIdentity cdtv;
    cdtv.identified = true;
    cdtv.type = adm::core::kRomTypeExtCdtv;
    cdtv.model = "CDTV";
    cdtv.name = "CDTV extended ROM v1.0";
    EXPECT_EQ(CanonicalRomName(cdtv), "kick34005.CDTV");
    RomIdentity a570 = cdtv; // as the database has it: model CDTV too
    a570.name = "CDTV/A570 extended ROM v2.30";
    EXPECT_EQ(CanonicalRomName(a570), "") << "the preset's is v1.0";

    // The A3000's 2.04 is revision 37.175, as the A500+'s is
    RomIdentity a3000v204 = Fake("x", 0x34);
    a3000v204.romVersion = 37, a3000v204.romRevision = 175, a3000v204.model = "A3000";
    EXPECT_EQ(CanonicalRomName(a3000v204), "");
    a3000v204.model = "A500+";
    EXPECT_EQ(CanonicalRomName(a3000v204), "kick37175.A500");

    EXPECT_EQ(adm::core::AmigaForeverRomName("kick34005.A500"), "amiga-os-130.rom");
    EXPECT_EQ(adm::core::AmigaForeverRomName("nope"), "");
}

TEST(SystemFiles, AdfVolumeNameReadsTheRootBlock)
{
    TempDir tmp;
    WriteAdf(tmp.Path() / "wb.adf", "Workbench1.3");
    WriteAdf(tmp.Path() / "ndos.adf", "Workbench1.3", false);
    WriteBytes(tmp.Path() / "short.adf", std::vector<uint8_t>(1000, 0));

    EXPECT_EQ(adm::core::AdfVolumeName(tmp.Path() / "wb.adf").value_or("?"), "Workbench1.3");
    EXPECT_FALSE(adm::core::AdfVolumeName(tmp.Path() / "ndos.adf")) << "not an AmigaDOS disk";
    EXPECT_FALSE(adm::core::AdfVolumeName(tmp.Path() / "short.adf"));
    EXPECT_FALSE(adm::core::AdfVolumeName(tmp.Path() / "missing.adf"));

    EXPECT_TRUE(adm::core::IsSystemDiskVolume("Workbench3.1"));
    EXPECT_TRUE(adm::core::IsSystemDiskVolume("Extras 1.3"));
    EXPECT_TRUE(adm::core::IsSystemDiskVolume("install3.1"));
    EXPECT_FALSE(adm::core::IsSystemDiskVolume("Lemmings"));
    EXPECT_TRUE(adm::core::IsWorkbenchVolume("Workbench1.3"));
    EXPECT_FALSE(adm::core::IsWorkbenchVolume("Extras 1.3"));
}

TEST(SystemFiles, DecodesAnAmigaForeverEncryptedRom)
{
    const auto plain = FakeRom(0x34);
    const std::vector<uint8_t> key = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    std::vector<uint8_t> file = {'A', 'M', 'I', 'R', 'O', 'M', 'T', 'Y', 'P', 'E', '1'};
    for (size_t i = 0; i < plain.size(); ++i)
        file.push_back(plain[i] ^ key[i % key.size()]);

    const auto decoded = adm::core::DecodeCloantoRom(file, key);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, plain);
    EXPECT_FALSE(adm::core::DecodeCloantoRom(plain, key)) << "a plain ROM is not one";
    EXPECT_FALSE(adm::core::DecodeCloantoRom(file, {})) << "no key, nothing to decode with";
}

TEST(SystemFiles, ScanFindsRomsKeysDisksAndHardfiles)
{
    TempDir tmp;
    const auto& root = tmp.Path();
    WriteBytes(root / "rom" / "amiga-os-130.rom", FakeRom(0x34));
    WriteBytes(root / "rom" / "copy of 1.3.bin", FakeRom(0x34)); // the same CRC
    WriteBytes(root / "rom" / "a1200.rom", FakeRom(0x40));
    WriteBytes(root / "rom" / "something.bin", FakeRom(0x00)); // not a ROM
    WriteBytes(root / "rom" / "rom.key", std::vector<uint8_t>(2048, 1));
    WriteBytes(root / "rom" / "game.zip", FakeRom(0x40)); // an archive: never asked
    WriteAdf(root / "adf" / "wb13.adf", "Workbench1.3");
    WriteAdf(root / "adf" / "lemmings.adf", "Lemmings");
    WriteBytes(root / "hdf" / "workbench-311.hdf", std::vector<uint8_t>(4096, 0));

    g_identifyCalls = 0;
    const auto scan = adm::core::ScanForSystemFiles(root, FakeIdentifier());
    EXPECT_EQ(g_identifyCalls, 4) << "the three ROM-sized files and the stranger, not the zip";
    ASSERT_EQ(scan.roms.size(), 2u) << "one per CRC";
    EXPECT_EQ(scan.roms[0].romVersion, 34) << "oldest first";
    EXPECT_EQ(scan.roms[1].romVersion, 40);
    ASSERT_EQ(scan.romKeys.size(), 1u);
    ASSERT_EQ(scan.disks.size(), 1u) << "only AmigaOS disks";
    EXPECT_EQ(scan.disks[0].volume, "Workbench1.3");
    ASSERT_EQ(scan.hardfiles.size(), 1u);
    EXPECT_EQ(scan.hardfiles[0].size, 4096u);
    EXPECT_FALSE(scan.truncated);

    // A limit stops the walk and says so
    adm::core::ScanLimits tight;
    tight.maxEntries = 3;
    EXPECT_TRUE(adm::core::ScanForSystemFiles(root, FakeIdentifier(), tight).truncated);
    // So does depth, quietly: a ROM below it is not found
    adm::core::ScanLimits shallow;
    shallow.maxDepth = 0;
    EXPECT_TRUE(adm::core::ScanForSystemFiles(root, FakeIdentifier(), shallow).roms.empty());

    // A second folder adds to the first without repeating it
    auto merged = scan;
    adm::core::MergeScan(merged, scan);
    EXPECT_EQ(merged.roms.size(), 2u);
    EXPECT_EQ(merged.disks.size(), 1u);
}

TEST(SystemFiles, CopyNamesRomsForThePresetsAndNeverOverwrites)
{
    TempDir tmp;
    const auto source = tmp.Path() / "af";
    const auto romDir = tmp.Path() / "emulation";
    WriteBytes(source / "amiga-os-130.rom", FakeRom(0x34));
    WriteBytes(source / "kick32.rom", FakeRom(0x47)); // no preset's: copied under its own name
    const auto scan = adm::core::ScanForSystemFiles(source, FakeIdentifier());
    ASSERT_EQ(scan.roms.size(), 2u);

    auto results = adm::core::CopyRomsInto(romDir, scan.roms, scan.romKeys, FakeIdentifier());
    ASSERT_EQ(results.size(), 2u);
    for (const auto& r : results)
        EXPECT_EQ(r.outcome, RomCopy::Outcome::Copied) << r.source << ": " << r.detail;
    EXPECT_EQ(ReadBytes(romDir / "kick34005.A500"), FakeRom(0x34));
    EXPECT_TRUE(fs::exists(romDir / "kick32.rom"));
    EXPECT_TRUE(adm::core::FindPresetRom(romDir, "kick34005.A500", FakeIdentifier()));

    // Again: nothing to do
    results = adm::core::CopyRomsInto(romDir, scan.roms, scan.romKeys, FakeIdentifier());
    for (const auto& r : results)
        EXPECT_EQ(r.outcome, RomCopy::Outcome::AlreadyThere) << r.source;

    // A different file under the name stays
    WriteBytes(romDir / "kick34005.A500", FakeRom(0x40));
    results = adm::core::CopyRomsInto(romDir, scan.roms, scan.romKeys, FakeIdentifier());
    EXPECT_EQ(results[0].outcome, RomCopy::Outcome::KeptExisting);
    EXPECT_FALSE(results[0].detail.empty());
    EXPECT_EQ(ReadBytes(romDir / "kick34005.A500"), FakeRom(0x40));
    // ... and is not taken for 1.3 by its name
    EXPECT_FALSE(adm::core::FindPresetRom(romDir, "kick34005.A500", FakeIdentifier()));
}

TEST(SystemFiles, CopyDecodesEncryptedRomsWithTheKeyBesideThem)
{
    TempDir tmp;
    const auto source = tmp.Path() / "rom";
    const auto romDir = tmp.Path() / "emulation";
    const auto plain = FakeRom(0x34);
    const std::vector<uint8_t> key = {9, 8, 7, 6, 5};
    std::vector<uint8_t> file = {'A', 'M', 'I', 'R', 'O', 'M', 'T', 'Y', 'P', 'E', '1'};
    for (size_t i = 0; i < plain.size(); ++i)
        file.push_back(plain[i] ^ key[i % key.size()]);
    WriteBytes(source / "amiga-os-130.rom", file);
    WriteBytes(source / "rom.key", key);

    // The core identifies an encrypted ROM through its key; the fake says so outright.
    RomIdentity encrypted = Fake(source / "amiga-os-130.rom", 0x34);
    encrypted.encrypted = true;

    auto results = adm::core::CopyRomsInto(romDir, {encrypted}, {}, FakeIdentifier());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, RomCopy::Outcome::Failed) << "no key to decode with";
    EXPECT_FALSE(fs::exists(romDir / "kick34005.A500"));

    results = adm::core::CopyRomsInto(romDir, {encrypted}, {source / "rom.key"}, FakeIdentifier());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, RomCopy::Outcome::Copied) << results[0].detail;
    EXPECT_EQ(ReadBytes(romDir / "kick34005.A500"), plain) << "the copy is the plain image";

    // A key that decodes to something else is caught by identifying the copy
    fs::remove(romDir / "kick34005.A500");
    WriteBytes(source / "rom.key", std::vector<uint8_t>{1, 2, 3});
    results = adm::core::CopyRomsInto(romDir, {encrypted}, {source / "rom.key"}, FakeIdentifier());
    EXPECT_EQ(results[0].outcome, RomCopy::Outcome::Failed);
    EXPECT_FALSE(fs::exists(romDir / "kick34005.A500")) << "a wrong copy is removed";
}

TEST(SystemFiles, DefaultSearchFoldersAreTheOnesThatExist)
{
    TempDir tmp;
    fs::create_directories(tmp.Path() / "Documents" / "Amiga Files" / "Shared");
    fs::create_directories(tmp.Path() / "Documents" / "FS-UAE" / "Kickstarts");
    const std::string base = tmp.Path().string();
    const auto folders = adm::core::DefaultSearchFolders([&base](const char* name) -> std::string {
        const std::string n = name;
        if (n == "PUBLIC" || n == "USERPROFILE") // the same folder twice: listed once
            return base;
        if (n == "ProgramFiles")
            return base + "/nowhere";
        return "";
    });
    ASSERT_EQ(folders.size(), 2u);
    EXPECT_EQ(folders[0].label, "Amiga Forever");
    EXPECT_EQ(folders[1].label, "FS-UAE");
}

TEST(SystemFiles, MachineNeedsAreCanonicalNames)
{
    for (const auto& m : adm::core::MachineRomNeeds()) {
        ASSERT_FALSE(m.roms.empty()) << m.label;
        for (const char* rom : m.roms)
            EXPECT_FALSE(adm::core::AmigaForeverRomName(rom).empty()) << rom << " has no alias";
    }
}

// The core loads a preset's ROM under its canonical, Amiga Forever or TOSEC name; so does the
// check.
TEST(SystemFiles, PresetRomsAreFoundUnderEveryNameTheCoreAccepts)
{
    TempDir tmp;
    WriteBytes(tmp.Path() / "Kickstart v3.1 rev 40.68 (1993)(Commodore)(A1200)[!].rom",
               FakeRom(0x40));
    EXPECT_TRUE(adm::core::FindPresetRom(tmp.Path(), "kick40068.A1200", FakeIdentifier()));
    WriteBytes(tmp.Path() / "amiga-os-130.rom", FakeRom(0x34));
    EXPECT_TRUE(adm::core::FindPresetRom(tmp.Path(), "kick34005.A500", {}));
    EXPECT_FALSE(adm::core::FindPresetRom(tmp.Path(), "kick40068.A4000", {}));
}

// Every preset the emulator offers has its ROMs listed, so a new preset cannot slip past the
// setup wizard's machine list and the missing-Kickstart notice.
TEST(SystemFiles, EveryPresetHasItsRoms)
{
    for (const auto& info : adm::GetAllAmigaModels())
        EXPECT_FALSE(adm::core::PresetRomNames(info.id).empty()) << info.id;
    EXPECT_TRUE(adm::core::PresetRomNames("nope").empty());
    const auto cd32 = adm::core::PresetRomNames("CD32FR");
    ASSERT_EQ(cd32.size(), 2u);
    EXPECT_EQ(cd32[1], "kick40060.CD32.ext");
}

TEST(SystemFiles, EncryptedRomsAreFoundByTheirHeader)
{
    TempDir tmp;
    WriteBytes(tmp.Path() / "plain.rom", FakeRom(0x34));
    EXPECT_FALSE(adm::core::HasEncryptedRoms(tmp.Path()));
    std::vector<uint8_t> encrypted = {'A', 'M', 'I', 'R', 'O', 'M', 'T', 'Y', 'P', 'E', '1'};
    encrypted.resize(11 + 256 * 1024, 0x5A);
    WriteBytes(tmp.Path() / "amiga-os-130.rom", encrypted);
    EXPECT_TRUE(adm::core::HasEncryptedRoms(tmp.Path()));
    EXPECT_FALSE(adm::core::HasEncryptedRoms(tmp.Path() / "missing"));
}

// --- With the real core ----------------------------------------------------------------------

#ifdef _WIN32
constexpr const char* kCoreName = "puae_libretro.dll";
#else
constexpr const char* kCoreName = "puae_libretro.dylib";
#endif

std::unique_ptr<adm::AmigaEmulator> LoadCoreOnly()
{
    const auto systemDirectory = fs::absolute("emulation");
    const auto corePath = systemDirectory / kCoreName;
    if (!fs::is_regular_file(corePath))
        return nullptr;
    auto emulator = std::make_unique<adm::AmigaEmulator>();
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    if (!emulator->LoadCore(corePath.string()) || !adm::core::KickstartRoms::Available(*emulator))
        return nullptr;
    return emulator;
}

adm::core::RomIdentifier RealIdentifier(const adm::AmigaEmulator& emulator)
{
    return [&emulator](const fs::path& file, const fs::path& keyDir) {
        return adm::core::KickstartRoms::Identify(emulator, file, keyDir);
    };
}

// The presets' own ROMs, named as they are, name themselves: the rules in CanonicalRomName are
// the database's view of every kick* file the presets load.
TEST(SystemFiles, EveryPresetRomNamesItself)
{
    auto emulator = LoadCoreOnly();
    if (!emulator)
        GTEST_SKIP() << "no PuAE core that identifies ROMs in emulation/";
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(fs::absolute("emulation"))) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("kick", 0) != 0)
            continue;
        const auto identity =
            adm::core::KickstartRoms::Identify(*emulator, entry.path(), entry.path().parent_path());
        ASSERT_TRUE(identity && identity->identified) << name;
        EXPECT_EQ(adm::core::CanonicalRomName(*identity), name)
            << name << ": " << identity->DisplayName() << " / " << identity->model << " / type "
            << std::hex << identity->type;
        ++checked;
    }
    if (checked == 0)
        GTEST_SKIP() << "no kick* ROMs in emulation/";
}

// A real Amiga Forever (or any) folder, named by PEEKABOO_TEST_SYSTEM_FILES: everything a preset
// needs is found and copied, and every copy loads as the ROM it came from.
TEST(SystemFiles, ScanAndCopyARealSystemFilesFolder)
{
    const char* folder = std::getenv("PEEKABOO_TEST_SYSTEM_FILES");
    if (!folder || !*folder)
        GTEST_SKIP() << "set PEEKABOO_TEST_SYSTEM_FILES to a folder of ROMs to run this";
    auto emulator = LoadCoreOnly();
    if (!emulator)
        GTEST_SKIP() << "no PuAE core that identifies ROMs in emulation/";
    const auto identify = RealIdentifier(*emulator);

    const auto scan = adm::core::ScanForSystemFiles(folder, identify);
    ASSERT_FALSE(scan.roms.empty()) << "no ROMs found in " << folder;
    TempDir tmp;
    const auto results = adm::core::CopyRomsInto(tmp.Path(), scan.roms, scan.romKeys, identify);
    int copied = 0;
    for (const auto& r : results) {
        EXPECT_NE(r.outcome, RomCopy::Outcome::Failed) << r.source << ": " << r.detail;
        copied += r.outcome == RomCopy::Outcome::Copied ? 1 : 0;
        std::cout << "  " << r.source.filename().string() << " -> " << r.target.filename().string()
                  << (r.outcome == RomCopy::Outcome::Copied ? "" : "  (" + r.detail + ")") << "\n";
    }
    for (const auto& rom : scan.roms) {
        std::cout << "    " << rom.DisplayName() << " | " << rom.model << " | type " << std::hex
                  << rom.type << std::dec << "\n";
    }
    EXPECT_EQ(copied, static_cast<int>(scan.roms.size()));
    if (fs::exists(tmp.Path() / "kick34005.A500")) {
        EXPECT_EQ(fs::file_size(tmp.Path() / "kick34005.A500"), 256u * 1024u)
            << "the 256 KB 1.3, not a 512 KB overdump of it (WHDLoad wants that one)";
    }
    for (const auto& m : adm::core::MachineRomNeeds()) {
        for (const char* rom : m.roms) {
            const bool have = adm::core::FindPresetRom(tmp.Path(), rom, identify).has_value();
            std::cout << "  " << m.label << ": " << rom << (have ? " ready" : " missing") << "\n";
        }
    }
}

} // namespace
