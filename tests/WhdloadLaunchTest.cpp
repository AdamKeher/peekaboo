#include "core/HostMountService.h"
#include "core/WhdloadLaunch.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using adm::core::WhdloadLaunch;

class WhdloadLaunchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) /
               ("peekaboo_whdload_" + std::to_string(nonce));
        std::filesystem::create_directories(root / "WHDLoad" / "C");
        std::filesystem::create_directories(root / "WHDSaves");
    }
    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path Touch(const std::filesystem::path& path)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << "x";
        return path;
    }
    std::filesystem::path root;
};

TEST_F(WhdloadLaunchTest, ResolvesArchivesDirectoriesSlavesAndInfos)
{
    std::string error;
    auto archive = WhdloadLaunch::Resolve(Touch(root / "Superfrog_v1.4_0035.lha"), error);
    ASSERT_TRUE(archive.has_value()) << error;
    EXPECT_TRUE(archive->isArchive);
    EXPECT_EQ(archive->volumeLabel, "Superfrog_v1.4_0035");
    EXPECT_EQ(archive->mountPath, root / "Superfrog_v1.4_0035.lha");

    const auto gameDir = root / "games" / "AnotherWorld";
    Touch(gameDir / "AnotherWorld.slave");
    Touch(root / "games" / "AnotherWorld.info");
    auto directory = WhdloadLaunch::Resolve(gameDir, error);
    ASSERT_TRUE(directory.has_value()) << error;
    EXPECT_FALSE(directory->isArchive);
    EXPECT_EQ(directory->mountPath, gameDir);
    EXPECT_EQ(directory->volumeLabel, "AnotherWorld");

    // A slave mounts the directory holding it; an .info mounts the directory named like it.
    auto slave = WhdloadLaunch::Resolve(gameDir / "AnotherWorld.slave", error);
    ASSERT_TRUE(slave.has_value()) << error;
    EXPECT_EQ(slave->mountPath, gameDir);
    auto info = WhdloadLaunch::Resolve(root / "games" / "AnotherWorld.info", error);
    ASSERT_TRUE(info.has_value()) << error;
    EXPECT_EQ(info->mountPath, gameDir);

    EXPECT_FALSE(WhdloadLaunch::Resolve(Touch(root / "readme.txt"), error).has_value());
    EXPECT_NE(error.find("not a WHDLoad archive"), std::string::npos);
    EXPECT_FALSE(WhdloadLaunch::Resolve(root / "missing.lha", error).has_value());
}

TEST_F(WhdloadLaunchTest, InstallsKickstartsFromEitherNamingWithoutOverwriting)
{
    const auto roms = root / "roms";
    std::filesystem::create_directories(roms);
    std::ofstream(roms / "kick34005.A500", std::ios::binary) << std::string(262144, 'A');
    std::ofstream(roms / "amiga-os-310-a1200.rom", std::ios::binary) << std::string(524288, 'B');
    // Encrypted Amiga Forever image: 11 bytes too long, not copied.
    std::ofstream(roms / "amiga-os-310-a600.rom", std::ios::binary) << std::string(524299, 'C');

    const auto whdload = root / "WHDLoad";
    std::filesystem::create_directories(whdload / "Devs" / "Kickstarts");
    std::ofstream(whdload / "Devs" / "Kickstarts" / "kick40068.A4000", std::ios::binary) << "kept";

    std::vector<std::string> missing;
    EXPECT_EQ(WhdloadLaunch::InstallKickstarts(whdload, roms, &missing), 3);
    const auto kickstarts = whdload / "Devs" / "Kickstarts";
    EXPECT_EQ(std::filesystem::file_size(kickstarts / "kick34005.A500"), 262144u);
    EXPECT_EQ(std::filesystem::file_size(kickstarts / "kick40068.A1200"), 524288u);
    EXPECT_EQ(std::filesystem::file_size(kickstarts / "kick40068.A4000"), 4u);
    EXPECT_FALSE(std::filesystem::exists(kickstarts / "kick40063.A600"));
    EXPECT_EQ(missing, (std::vector<std::string>{"kick33180.A500", "kick40063.A600"}));
}

TEST_F(WhdloadLaunchTest, SanitizesVolumeLabels)
{
    EXPECT_EQ(WhdloadLaunch::SanitizeLabel("Game: Part 1, \"Deluxe\""), "Game_ Part 1_ _Deluxe_");
    EXPECT_EQ(WhdloadLaunch::SanitizeLabel(""), "Game");
    EXPECT_EQ(WhdloadLaunch::SanitizeLabel(std::string(40, 'a')).size(), 30u);
}

TEST_F(WhdloadLaunchTest, BuildsWhdloadAheadOfTheGameAndSavesAfter)
{
    std::string error;
    const auto source = WhdloadLaunch::Resolve(Touch(root / "Superfrog.lha"), error);
    ASSERT_TRUE(source.has_value()) << error;

    const auto dh1 = root / "work";
    std::filesystem::create_directories(dh1);
    adm::core::EmulatorLaunchProfile base;
    base.mounts[0].device = adm::core::HostMountDevice::DH0;
    base.mounts[0].enabled = true;
    base.mounts[0].hostDirectory = root / "saved-dh0";
    base.mounts[1].device = adm::core::HostMountDevice::DH1;
    base.mounts[1].enabled = true;
    base.mounts[1].hostDirectory = dh1;
    base.mounts[1].volumeLabel = "Work";
    base.mounts[1].bootPriority = 5;

    auto profile = WhdloadLaunch::BuildProfile(*source, root / "WHDLoad", root / "WHDSaves", base);
    EXPECT_TRUE(profile.transient);
    EXPECT_FALSE(profile.minimalBootRom);
    EXPECT_EQ(profile.mounts[0].backingType, adm::core::HostMountBackingType::Archive);
    EXPECT_EQ(profile.mounts[0].source, adm::core::HostMountSource::TransientWhdload);
    EXPECT_EQ(profile.mounts[1].bootPriority, -128); // nothing boots ahead of WHDLoad:

    auto config = adm::core::UaeConfigBuilder::Build(profile, error);
    ASSERT_TRUE(config.has_value()) << error;
    const auto whdload = config->find("filesystem2=rw,WHDLoad:WHDLoad:\"" +
                                      (root / "WHDLoad").generic_string() + "\",0\n");
    const auto game = config->find("filesystem2=ro,DH0:Superfrog:\"" +
                                   (root / "Superfrog.lha").generic_string() + "\",-128\n");
    const auto work = config->find("filesystem2=ro,DH1:Work:");
    const auto saves = config->find("filesystem2=rw,WHDSaves:WHDSaves:\"" +
                                    (root / "WHDSaves").generic_string() + "\",-128\n");
    ASSERT_NE(whdload, std::string::npos) << *config;
    ASSERT_NE(game, std::string::npos) << *config;
    ASSERT_NE(work, std::string::npos) << *config;
    ASSERT_NE(saves, std::string::npos) << *config;
    EXPECT_LT(whdload, game);
    EXPECT_LT(game, work);
    EXPECT_LT(work, saves);

    // Units follow that order: WHDLoad 0, DH0 1, DH1 2, WHDSaves 3.
    adm::core::HostMountService service;
    service.MarkEffective(profile);
    EXPECT_FALSE(service.DeviceForUnit(0).has_value());
    EXPECT_EQ(service.DeviceForUnit(1), adm::core::HostMountDevice::DH0);
    EXPECT_EQ(service.DeviceForUnit(2), adm::core::HostMountDevice::DH1);
    EXPECT_FALSE(service.DeviceForUnit(3).has_value());
    EXPECT_TRUE(service.IsTransientOverrideActive());
}

} // namespace
