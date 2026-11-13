#include "core/KickstartRoms.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/AmigaEmulator.h"

// Identification goes through the core's own ROM database (adm_identify_rom, core 1.96.0), so a
// ROM is recognised by its contents. The point of these tests is that the filename is not
// consulted: the same bytes under any name give the same answer, and a wrong name is not believed.
namespace {

#ifdef _WIN32
constexpr const char* kCoreName = "puae_libretro.dll";
#else
constexpr const char* kCoreName = "puae_libretro.dylib";
#endif

std::unique_ptr<adm::AmigaEmulator> LoadCoreOnly()
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / kCoreName;
    if (!std::filesystem::is_regular_file(corePath))
        return nullptr;
    auto emulator = std::make_unique<adm::AmigaEmulator>();
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    // No StartEmpty(): identification reads a file and hashes it, and needs no machine.
    if (!emulator->LoadCore(corePath.string()))
        return nullptr;
    return emulator;
}

std::filesystem::path StagingDirectory()
{
    return std::filesystem::temp_directory_path() / "adm-rom-id-test";
}

TEST(KickstartRoms, IdentifiesABundledKickstartByItsContents)
{
    auto emulator = LoadCoreOnly();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    if (!adm::core::KickstartRoms::Available(*emulator))
        GTEST_SKIP() << "this core predates adm_identify_rom (core 1.96.0)";

    const auto romDirectory = std::filesystem::absolute("emulation");
    const auto rom = romDirectory / "kick34005.A500";
    if (!std::filesystem::is_regular_file(rom))
        GTEST_SKIP() << "kick34005.A500 is not present in " << romDirectory;

    const auto identity = adm::core::KickstartRoms::Identify(*emulator, rom, romDirectory);
    ASSERT_TRUE(identity.has_value()) << "the file should at least have been readable";
    ASSERT_TRUE(identity->identified) << "Kickstart 1.3 should be in the ROM database";
    // Two different numbers, and this is the pair that is easy to get backwards: the database
    // calls it Kickstart 1.3, ROM revision 34.5 - which is what kick34005 is named after.
    EXPECT_EQ(identity->version, 1) << "Kickstart 1.3";
    EXPECT_EQ(identity->revision, 3);
    EXPECT_EQ(identity->romVersion, 34) << "kick34005 is ROM revision 34.5";
    EXPECT_EQ(identity->romRevision, 5);
    EXPECT_EQ(identity->size, 256u * 1024u) << "Kickstart 1.3 is a 256 KB ROM";
    EXPECT_EQ(identity->VersionLabel(), "Kickstart 1.3 (rev 34.5)");
    // The database lists every machine the ROM fits, not just the first.
    EXPECT_NE(identity->model.find("A500"), std::string::npos) << "model: " << identity->model;
    EXPECT_TRUE(identity->IsKickstart()) << "type mask " << identity->type;
    EXPECT_FALSE(identity->IsExtended());
    EXPECT_NE(identity->crc32, 0u);
    EXPECT_FALSE(identity->DisplayName().empty());
}

// The whole reason for identifying by content: the old code matched on filename, so a ROM under
// any other name was invisible and a mis-named one was believed.
TEST(KickstartRoms, TheFilenameIsNotConsulted)
{
    auto emulator = LoadCoreOnly();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    if (!adm::core::KickstartRoms::Available(*emulator))
        GTEST_SKIP() << "this core predates adm_identify_rom (core 1.96.0)";

    const auto romDirectory = std::filesystem::absolute("emulation");
    const auto source = romDirectory / "kick34005.A500";
    if (!std::filesystem::is_regular_file(source))
        GTEST_SKIP() << "kick34005.A500 is not present in " << romDirectory;

    const auto truth = adm::core::KickstartRoms::Identify(*emulator, source, romDirectory);
    ASSERT_TRUE(truth && truth->identified);

    // The same bytes under a name that claims to be a completely different ROM.
    std::error_code ec;
    const auto staging = StagingDirectory();
    std::filesystem::create_directories(staging, ec);
    const auto liar = staging / "kick40068.A1200";
    std::filesystem::copy_file(source, liar, std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "could not stage a renamed copy: " << ec.message();

    const auto identity = adm::core::KickstartRoms::Identify(*emulator, liar, romDirectory);
    ASSERT_TRUE(identity && identity->identified);
    EXPECT_EQ(identity->crc32, truth->crc32);
    EXPECT_EQ(identity->version, 1) << "the contents are 1.3 whatever the file is called";
    EXPECT_EQ(identity->revision, 3);
    EXPECT_EQ(identity->romVersion, 34) << "and not the 40.68 the filename claims";

    std::filesystem::remove(liar, ec);
}

TEST(KickstartRoms, AFileThatIsNotARomIsReportedAsUnidentifiedNotAsAnError)
{
    auto emulator = LoadCoreOnly();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    if (!adm::core::KickstartRoms::Available(*emulator))
        GTEST_SKIP() << "this core predates adm_identify_rom (core 1.96.0)";

    std::error_code ec;
    const auto staging = StagingDirectory();
    std::filesystem::create_directories(staging, ec);
    const auto junk = staging / "not-a-rom.bin";
    {
        std::ofstream out(junk, std::ios::binary);
        const std::vector<char> filler(256 * 1024, 0x5A);
        out.write(filler.data(), static_cast<std::streamsize>(filler.size()));
    }

    const auto identity = adm::core::KickstartRoms::Identify(*emulator, junk, staging);
    ASSERT_TRUE(identity.has_value()) << "a readable file is an answer, not a failure";
    EXPECT_FALSE(identity->identified);
    EXPECT_EQ(identity->size, 256u * 1024u) << "the size is known even when the ROM is not";
    // The list still has something to show for it.
    EXPECT_EQ(identity->DisplayName(), "not-a-rom.bin");

    std::filesystem::remove(junk, ec);
}

TEST(KickstartRoms, MissingFileIsDistinguishedFromAnUnknownRom)
{
    auto emulator = LoadCoreOnly();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    if (!adm::core::KickstartRoms::Available(*emulator))
        GTEST_SKIP() << "this core predates adm_identify_rom (core 1.96.0)";

    const auto absent = std::filesystem::absolute("emulation") / "no-such-rom-at-all.rom";
    EXPECT_FALSE(adm::core::KickstartRoms::Identify(*emulator, absent, {}).has_value());
}

TEST(KickstartRoms, ScanFindsTheBundledRoms)
{
    auto emulator = LoadCoreOnly();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    if (!adm::core::KickstartRoms::Available(*emulator))
        GTEST_SKIP() << "this core predates adm_identify_rom (core 1.96.0)";

    const auto romDirectory = std::filesystem::absolute("emulation");
    const auto found = adm::core::KickstartRoms::Scan(*emulator, romDirectory);
    if (found.empty())
        GTEST_SKIP() << "no ROMs in " << romDirectory;

    // Every hit is identified (Scan drops the rest) and has something to show.
    for (const auto& rom : found) {
        EXPECT_TRUE(rom.identified);
        EXPECT_FALSE(rom.DisplayName().empty());
        EXPECT_FALSE(rom.DetailLine().empty());
    }
    // Sorted oldest first, so a picker lists them in a sensible order.
    EXPECT_TRUE(std::is_sorted(found.begin(), found.end(),
                               [](const auto& a, const auto& b) { return a.version < b.version; }));
    // The A500 1.3 ROM is shipped, so it must be in there.
    const bool has13 = std::any_of(found.begin(), found.end(), [](const auto& rom) {
        return rom.romVersion == 34 && rom.romRevision == 5;
    });
    EXPECT_TRUE(has13) << "kick34005.A500 ships in emulation/ and should have been found";
}

// The CD32 extended ROM is a different ROMTYPE from the Kickstart beside it, which is what lets
// the picker offer the right files for each of its two slots.
TEST(KickstartRoms, ExtendedRomIsDistinguishedFromAKickstart)
{
    auto emulator = LoadCoreOnly();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    if (!adm::core::KickstartRoms::Available(*emulator))
        GTEST_SKIP() << "this core predates adm_identify_rom (core 1.96.0)";

    const auto romDirectory = std::filesystem::absolute("emulation");
    const auto ext = romDirectory / "kick40060.CD32.ext";
    if (!std::filesystem::is_regular_file(ext))
        GTEST_SKIP() << "kick40060.CD32.ext is not present in " << romDirectory;

    const auto identity = adm::core::KickstartRoms::Identify(*emulator, ext, romDirectory);
    ASSERT_TRUE(identity && identity->identified);
    EXPECT_TRUE(identity->IsExtended()) << "type mask " << identity->type;
    EXPECT_FALSE(identity->IsKickstart());
}

} // namespace
