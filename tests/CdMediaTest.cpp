#include "core/CdMedia.h"
#include "core/HostMountService.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using adm::core::CdImageResolver;
using adm::core::MachineRoms;

class CdMediaTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) /
               ("peekaboo_cdmedia_" + std::to_string(nonce));
        std::filesystem::create_directories(root);
    }
    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path Write(const std::filesystem::path& path, size_t size)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << std::string(size, '\0');
        return path;
    }
    std::filesystem::path root;
};

TEST_F(CdMediaTest, Cd32NeedsBothRomsUnderAnyAcceptedName)
{
    const auto roms = root / "roms";
    std::filesystem::create_directories(roms);
    EXPECT_TRUE(MachineRoms::MissingRoms("A1200", roms).empty());
    EXPECT_TRUE(MachineRoms::IsCdMachine("CD32"));
    EXPECT_FALSE(MachineRoms::IsCdMachine("A1200"));

    auto missing = MachineRoms::MissingRoms("CD32", roms);
    EXPECT_NE(missing.find("kick40060.CD32 "), std::string::npos) << missing;
    EXPECT_NE(missing.find("kick40060.CD32.ext"), std::string::npos) << missing;

    // Amiga Forever names, one of them encrypted (11 bytes longer).
    Write(roms / "amiga-os-310-cd32.rom", 524288);
    Write(roms / "amiga-os-310-cd32-ext.rom", 524288 + 11);
    EXPECT_TRUE(MachineRoms::MissingRoms("CD32", roms).empty());
    EXPECT_TRUE(MachineRoms::MissingRoms("CD32FR", roms).empty());

    // A wrong size does not count.
    std::filesystem::remove(roms / "amiga-os-310-cd32-ext.rom");
    Write(roms / "kick40060.CD32.ext", 1000);
    EXPECT_FALSE(MachineRoms::MissingRoms("CD32", roms).empty());
}

TEST_F(CdMediaTest, ACombinedOneMegabyteCd32KickstartNeedsNoExtendedRom)
{
    const auto roms = root / "roms";
    Write(roms / "kick40060.CD32", 1048576);
    EXPECT_TRUE(MachineRoms::MissingRoms("CD32", roms).empty());
}

TEST_F(CdMediaTest, CdtvNeedsItsExtendedRom)
{
    const auto roms = root / "roms";
    std::filesystem::create_directories(roms);
    EXPECT_NE(MachineRoms::MissingRoms("CDTV", roms).find("kick34005.CDTV"), std::string::npos);
    Write(roms / "amiga-os-130-cdtv-ext.rom", 262144);
    EXPECT_TRUE(MachineRoms::MissingRoms("CDTV", roms).empty());
}

TEST_F(CdMediaTest, ResolvesImagesDirectlyAndArchivesThroughTheCache)
{
    std::string error;
    const auto cue = Write(root / "Game (1994).cue", 100);
    auto direct = CdImageResolver::Resolve(cue, root / "cache", error);
    ASSERT_TRUE(direct.has_value()) << error;
    EXPECT_FALSE(direct->needsExtraction);
    EXPECT_EQ(direct->image, cue);

    const auto archive = Write(root / "Chuck Rock II [!].7z", 4096);
    auto fresh = CdImageResolver::Resolve(archive, root / "cache", error);
    ASSERT_TRUE(fresh.has_value()) << error;
    EXPECT_TRUE(fresh->needsExtraction);
    EXPECT_EQ(fresh->extractDirectory.parent_path(), root / "cache");
    const std::string folder = fresh->extractDirectory.filename().string();
    EXPECT_EQ(folder.rfind("Chuck Rock II __", 0), 0u) << folder; // readable prefix, 16 chars
    EXPECT_EQ(folder.size(), 16u + 9u) << folder;                 // plus _<8 hex>

    // After an extraction completes, the cache is reused and the .cue is preferred over tracks.
    Write(fresh->extractDirectory / "Chuck Rock II (Track 01).bin", 5000);
    Write(fresh->extractDirectory / "Chuck Rock II (Track 02).wav", 9000);
    Write(fresh->extractDirectory / "Chuck Rock II.cue", 300);
    ASSERT_TRUE(CdImageResolver::WriteExtractionMarker(archive, fresh->extractDirectory));
    auto cached = CdImageResolver::Resolve(archive, root / "cache", error);
    ASSERT_TRUE(cached.has_value()) << error;
    EXPECT_FALSE(cached->needsExtraction);
    EXPECT_EQ(cached->image.filename(), "Chuck Rock II.cue");

    // A different archive of the same name invalidates the cache.
    Write(archive, 8192);
    auto changed = CdImageResolver::Resolve(archive, root / "cache", error);
    ASSERT_TRUE(changed.has_value()) << error;
    EXPECT_TRUE(changed->needsExtraction);

    EXPECT_FALSE(CdImageResolver::Resolve(Write(root / "notes.txt", 1), root / "cache", error));
    EXPECT_NE(error.find("not a disc image"), std::string::npos);
}

TEST_F(CdMediaTest, FindImagePrefersCueThenLargestIso)
{
    const auto dir = root / "extracted";
    Write(dir / "small.iso", 10);
    Write(dir / "sub" / "big.iso", 50);
    auto image = CdImageResolver::FindImage(dir);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->filename(), "big.iso");
    Write(dir / "sub" / "disc.chd", 5);
    EXPECT_EQ(CdImageResolver::FindImage(dir)->filename(), "disc.chd");
    Write(dir / "disc.cue", 5);
    EXPECT_EQ(CdImageResolver::FindImage(dir)->filename(), "disc.cue");
}

TEST_F(CdMediaTest, LaunchProfileWritesTheCdAndScsiOnlyWhereNeeded)
{
    adm::core::EmulatorLaunchProfile profile;
    profile.modelId = "CD32";
    profile.cdImage = root / "Game.cue";
    std::string error;
    auto config = adm::core::UaeConfigBuilder::Build(profile, error);
    ASSERT_TRUE(config.has_value()) << error;
    EXPECT_NE(config->find("cdimage0=\"" + (root / "Game.cue").generic_string() + "\",\n"),
              std::string::npos)
        << *config;
    EXPECT_EQ(config->find("scsi=true"), std::string::npos);

    profile.modelId = "A1200";
    config = adm::core::UaeConfigBuilder::Build(profile, error);
    ASSERT_TRUE(config.has_value()) << error;
    EXPECT_NE(config->find("scsi=true\n"), std::string::npos);

    profile.cdImage.clear();
    config = adm::core::UaeConfigBuilder::Build(profile, error);
    ASSERT_TRUE(config.has_value()) << error;
    EXPECT_EQ(config->find("cdimage0"), std::string::npos);
}

} // namespace
