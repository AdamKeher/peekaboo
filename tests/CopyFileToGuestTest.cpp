#include "core/HostMountService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

// "Drop a host file into the guest" - the host-only half of the host-guest integration item. It
// copies into a mounted DH directory, so the interesting cases are all about which mount, if any,
// is allowed to take the file.
namespace {

std::filesystem::path MakeDirectory(const char* name)
{
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "adm-drop-test" / name;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path MakeFile(const std::filesystem::path& dir, const char* name,
                               const std::string& contents)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    return path;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The service reports the mounts a launch made effective, so a test has to go through a launch
// rather than setting the configured mounts and hoping.
adm::core::HostMountService ServiceWithEffective(const adm::core::HostDirectoryMount& dh0,
                                                 const adm::core::HostDirectoryMount& dh1)
{
    adm::core::HostMountService service;
    adm::core::EmulatorLaunchProfile profile;
    profile.modelId = "A1200";
    profile.mounts[0] = dh0;
    profile.mounts[1] = dh1;
    service.MarkEffective(profile);
    return service;
}

adm::core::HostDirectoryMount WritableDirectory(adm::core::HostMountDevice device,
                                                const std::filesystem::path& dir)
{
    adm::core::HostDirectoryMount mount;
    mount.backingType = adm::core::HostMountBackingType::Directory;
    mount.device = device;
    mount.enabled = true;
    mount.hostDirectory = dir;
    mount.volumeLabel = adm::core::HostMountDeviceName(device);
    mount.readOnly = false;
    return mount;
}

TEST(CopyFileToGuest, CopiesIntoTheWritableMount)
{
    const auto dh0 = MakeDirectory("dh0");
    const auto source = MakeDirectory("src");
    const auto file = MakeFile(source, "readme.txt", "hello amiga");

    auto service = ServiceWithEffective(WritableDirectory(adm::core::HostMountDevice::DH0, dh0),
                                        adm::core::HostDirectoryMount{});
    std::string error;
    const auto guestPath = service.CopyFileToGuest(file, error);
    ASSERT_TRUE(guestPath.has_value()) << error;
    EXPECT_EQ(*guestPath, "DH0:readme.txt");
    EXPECT_EQ(ReadFile(dh0 / "readme.txt"), "hello amiga");
}

// DH0 first, but a DH0 that cannot take the file must not stop DH1 from doing so.
TEST(CopyFileToGuest, FallsBackToTheSecondDriveWhenTheFirstIsReadOnly)
{
    const auto dh0 = MakeDirectory("ro-dh0");
    const auto dh1 = MakeDirectory("rw-dh1");
    const auto file = MakeFile(MakeDirectory("src2"), "game.adf", "x");

    auto readOnly = WritableDirectory(adm::core::HostMountDevice::DH0, dh0);
    readOnly.readOnly = true;
    auto service =
        ServiceWithEffective(readOnly, WritableDirectory(adm::core::HostMountDevice::DH1, dh1));

    std::string error;
    const auto guestPath = service.CopyFileToGuest(file, error);
    ASSERT_TRUE(guestPath.has_value()) << error;
    EXPECT_EQ(*guestPath, "DH1:game.adf");
    EXPECT_TRUE(std::filesystem::exists(dh1 / "game.adf"));
    EXPECT_FALSE(std::filesystem::exists(dh0 / "game.adf")) << "the read-only drive was written to";
}

// Overwriting a file the guest may have open is worse than an odd name.
TEST(CopyFileToGuest, NeverOverwritesAnExistingFile)
{
    const auto dh0 = MakeDirectory("clash");
    const auto file = MakeFile(MakeDirectory("src3"), "notes.txt", "new");
    MakeFile(dh0, "notes.txt", "original");

    auto service = ServiceWithEffective(WritableDirectory(adm::core::HostMountDevice::DH0, dh0),
                                        adm::core::HostDirectoryMount{});
    std::string error;
    const auto guestPath = service.CopyFileToGuest(file, error);
    ASSERT_TRUE(guestPath.has_value()) << error;
    EXPECT_EQ(*guestPath, "DH0:notes.1.txt");
    EXPECT_EQ(ReadFile(dh0 / "notes.txt"), "original") << "the original must be untouched";
    EXPECT_EQ(ReadFile(dh0 / "notes.1.txt"), "new");
}

TEST(CopyFileToGuest, RefusesWhenNothingWritableIsMounted)
{
    const auto file = MakeFile(MakeDirectory("src4"), "thing.txt", "x");

    // Nothing mounted at all.
    auto none =
        ServiceWithEffective(adm::core::HostDirectoryMount{}, adm::core::HostDirectoryMount{});
    std::string error;
    EXPECT_FALSE(none.CopyFileToGuest(file, error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(none.DropTargetDevice().has_value());

    // A hardfile is an image whose filesystem only the guest understands.
    adm::core::HostDirectoryMount hardfile;
    hardfile.backingType = adm::core::HostMountBackingType::HardfileImage;
    hardfile.device = adm::core::HostMountDevice::DH0;
    hardfile.enabled = true;
    hardfile.readOnly = false;
    hardfile.hardfilePath = MakeFile(MakeDirectory("src5"), "disk.hdf", "not a directory");
    auto image = ServiceWithEffective(hardfile, adm::core::HostDirectoryMount{});
    EXPECT_FALSE(image.CopyFileToGuest(file, error).has_value());
    EXPECT_FALSE(error.empty());
}

TEST(CopyFileToGuest, RefusesSomethingThatIsNotAFile)
{
    const auto dh0 = MakeDirectory("nofile");
    auto service = ServiceWithEffective(WritableDirectory(adm::core::HostMountDevice::DH0, dh0),
                                        adm::core::HostDirectoryMount{});
    std::string error;
    EXPECT_FALSE(service.CopyFileToGuest(dh0, error).has_value()) << "a directory is not a file";
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(service.CopyFileToGuest({}, error).has_value());
}

} // namespace
