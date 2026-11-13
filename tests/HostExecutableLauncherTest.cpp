#include "core/AmigaEmulator.h"
#include "core/HostExecutableLauncher.h"
#include "core/HostMountService.h"
#include "core/UserPreferences.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

class HostExecutableLauncherTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("peekaboo_host_launcher_test_" + std::to_string(nonce));
        std::filesystem::create_directories(root);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    static void WriteHunk(const std::filesystem::path& path, unsigned char marker = 0x42)
    {
        std::filesystem::create_directories(path.parent_path());
        // One-segment Amiga HUNK executable: MOVEQ #marker,D0; RTS. Keeping the fixture genuinely
        // loadable lets the integration test advance through the startup-sequence as well as
        // checking the host-side header validation.
        const std::vector<unsigned char> bytes = {
            0x00, 0x00,   0x03, 0xF3, // HUNK_HEADER
            0x00, 0x00,   0x00, 0x00, // no resident-library names
            0x00, 0x00,   0x00, 0x01, // table size
            0x00, 0x00,   0x00, 0x00, // first hunk
            0x00, 0x00,   0x00, 0x00, // last hunk
            0x00, 0x00,   0x00, 0x01, // segment size in longwords
            0x00, 0x00,   0x03, 0xE9, // HUNK_CODE
            0x00, 0x00,   0x00, 0x01, // one code longword
            0x70, marker, 0x4E, 0x75, // MOVEQ #marker,D0; RTS
            0x00, 0x00,   0x03, 0xF2  // HUNK_END
        };
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    static std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    std::filesystem::path root;
};

// ---- Hardfile mounts ---------------------------------------------------------------------

static std::filesystem::path WriteBlankHardfile(const std::filesystem::path& path,
                                                size_t blocks = 2048, bool rdb = false)
{
    std::filesystem::create_directories(path.parent_path());
    std::vector<char> bytes(blocks * 512, 0);
    if (rdb) {
        bytes[0] = 'R';
        bytes[1] = 'D';
        bytes[2] = 'S';
        bytes[3] = 'K';
    }
    std::ofstream(path, std::ios::binary)
        .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
}

static adm::core::HostDirectoryMount HardfileMount(adm::core::HostMountDevice device,
                                                   const std::filesystem::path& path)
{
    adm::core::HostDirectoryMount mount;
    mount.device = device;
    mount.enabled = true;
    mount.backingType = adm::core::HostMountBackingType::HardfileImage;
    mount.hardfilePath = path;
    mount.volumeLabel = adm::core::HostMountDeviceName(device);
    mount.readOnly = false;
    mount.bootPriority = 0;
    return mount;
}

TEST_F(HostExecutableLauncherTest, BuildsQuotedStartupSequence)
{
    EXPECT_EQ(adm::core::HostExecutableLauncher::BuildStartupSequence("my demo.exe"),
              "\"DH1:my demo.exe\"\n");
}

TEST_F(HostExecutableLauncherTest, BuildsBootableDh0AndReadOnlyNonBootableDh1)
{
    const auto boot = root / "launcher files" / "dh0";
    const auto output = root / "build output";
    adm::core::EmulatorLaunchProfile profile;
    profile.transient = true;
    profile.minimalBootRom = true;
    profile.mounts[0] = {adm::core::HostMountBackingType::Directory,
                         adm::core::HostMountDevice::DH0,
                         true,
                         boot,
                         "PeekabooBoot",
                         false,
                         0,
                         adm::core::HostMountSource::TransientBartman};
    profile.mounts[1] = {adm::core::HostMountBackingType::Directory,
                         adm::core::HostMountDevice::DH1,
                         true,
                         output,
                         "PeekabooBuild",
                         true,
                         -128,
                         adm::core::HostMountSource::TransientBartman};
    std::filesystem::create_directories(boot);
    std::filesystem::create_directories(output);
    std::string error;
    const auto built = adm::core::UaeConfigBuilder::Build(profile, error);
    ASSERT_TRUE(built.has_value()) << error;
    const std::string& config = *built;

    EXPECT_NE(config.find("boot_rom_uae=min\n"), std::string::npos);
    EXPECT_NE(config.find("filesystem2=rw,DH0:PeekabooBoot:\"" + boot.generic_string() + "\",0\n"),
              std::string::npos);
    EXPECT_NE(
        config.find("filesystem2=ro,DH1:PeekabooBuild:\"" + output.generic_string() + "\",-128\n"),
        std::string::npos);
}

TEST_F(HostExecutableLauncherTest, RejectsInvalidOrUnsafePrograms)
{
    std::string error;
    EXPECT_FALSE(adm::core::HostExecutableLauncher::ValidateHunk("relative.exe", error));
    EXPECT_NE(error.find("absolute"), std::string::npos);

    const auto invalid = root / "invalid.exe";
    std::ofstream(invalid, std::ios::binary) << "not a hunk";
    EXPECT_FALSE(adm::core::HostExecutableLauncher::ValidateHunk(invalid, error));
    EXPECT_NE(error.find("HUNK_HEADER"), std::string::npos);

    adm::core::HostDirectoryMount unsafe;
    unsafe.enabled = true;
    unsafe.hostDirectory = root / "bad\"path";
    unsafe.volumeLabel = "Bad";
    std::string ignored;
    EXPECT_FALSE(adm::core::UaeConfigBuilder::SerializeMount(unsafe, ignored).has_value());
}

TEST_F(HostExecutableLauncherTest, PreparesLaunchWithoutCopyingOrChangingExecutable)
{
    const auto program = root / "build output" / "my demo.exe";
    const auto workspace = root / "runtime";
    WriteHunk(program);
    const auto before = ReadText(program);

    std::string error;
    auto launch = adm::core::HostExecutableLauncher::Prepare(program, workspace, error);
    ASSERT_TRUE(launch.has_value()) << error;

    EXPECT_EQ(ReadText(launch->startupSequencePath), "\"DH1:my demo.exe\"\n");
    EXPECT_EQ(launch->profile.mounts[1].hostDirectory, program.parent_path());
    EXPECT_EQ(ReadText(program), before);
    EXPECT_FALSE(std::filesystem::exists(workspace / program.filename()));
}

TEST_F(HostExecutableLauncherTest, ReusesWorkspaceAcrossRepeatedCompileAndRunCycles)
{
    const auto program = root / "build" / "a.exe";
    const auto workspace = root / "runtime";
    std::string error;

    for (unsigned char generation = 1; generation <= 25; ++generation) {
        WriteHunk(program, generation);
        auto launch = adm::core::HostExecutableLauncher::Prepare(program, workspace, error);
        ASSERT_TRUE(launch.has_value())
            << "generation " << static_cast<int>(generation) << ": " << error;
        EXPECT_EQ(ReadText(launch->startupSequencePath), "\"DH1:a.exe\"\n");
        EXPECT_EQ(launch->profile.mounts[0].bootPriority, 0);
        EXPECT_EQ(launch->profile.mounts[1].bootPriority, -128);
    }
}

TEST_F(HostExecutableLauncherTest, BuildsEveryDhCombinationAndTracksPendingState)
{
    adm::core::HostMountService service;
    auto mounts = service.GetConfiguredMounts();
    std::filesystem::create_directories(root / "dh0");
    std::filesystem::create_directories(root / "dh1");
    mounts[0].hostDirectory = root / "dh0";
    mounts[0].volumeLabel = "Work";
    mounts[0].readOnly = true;
    mounts[1].hostDirectory = root / "dh1";
    mounts[1].volumeLabel = "Data";
    mounts[1].readOnly = true;

    for (int mask = 0; mask < 4; ++mask) {
        mounts[0].enabled = (mask & 1) != 0;
        mounts[1].enabled = (mask & 2) != 0;
        service.SetConfiguredMounts(mounts);
        auto profile = service.CreateConfiguredProfile();
        std::string error;
        auto config = adm::core::UaeConfigBuilder::Build(profile, error);
        ASSERT_TRUE(config.has_value()) << error;
        EXPECT_EQ(config->find("filesystem2=") != std::string::npos, mask != 0);
        service.MarkEffective(profile);
        EXPECT_FALSE(service.HasPendingRestart());
    }
}

TEST_F(HostExecutableLauncherTest, RejectsDuplicateMountsAndInvalidPriority)
{
    std::filesystem::create_directories(root / "shared");
    std::array<adm::core::HostDirectoryMount, 2> mounts;
    for (int index = 0; index < 2; ++index) {
        mounts[index].device = static_cast<adm::core::HostMountDevice>(index);
        mounts[index].enabled = true;
        mounts[index].hostDirectory = root / "shared";
        mounts[index].volumeLabel = index == 0 ? "Same" : "same";
        mounts[index].readOnly = true;
    }
    std::string error;
    EXPECT_FALSE(adm::core::UaeConfigBuilder::ValidateMounts(mounts, error));
    EXPECT_NE(error.find("canonical host directory"), std::string::npos);

    mounts[1].enabled = false;
    mounts[0].bootPriority = 128;
    EXPECT_FALSE(adm::core::UaeConfigBuilder::ValidateMounts(mounts, error));
    EXPECT_NE(error.find("-128 and 127"), std::string::npos);

    mounts[0].bootPriority = 0;
    mounts[1] = mounts[0];
    mounts[1].enabled = true;
    EXPECT_FALSE(adm::core::UaeConfigBuilder::ValidateMounts(mounts, error));
    EXPECT_NE(error.find("device assignment"), std::string::npos);
}

TEST_F(HostExecutableLauncherTest, TransientProfileDoesNotMutateSavedMounts)
{
    adm::core::HostMountService service;
    auto configured = service.GetConfiguredMounts();
    configured[0].enabled = true;
    configured[0].hostDirectory = root / "saved";
    configured[0].volumeLabel = "Saved";
    std::filesystem::create_directories(configured[0].hostDirectory);
    service.SetConfiguredMounts(configured);

    adm::core::EmulatorLaunchProfile transient;
    transient.transient = true;
    transient.mounts[0] = configured[0];
    transient.mounts[0].hostDirectory = root / "bartman";
    transient.mounts[0].volumeLabel = "Bartman";
    std::filesystem::create_directories(transient.mounts[0].hostDirectory);
    service.MarkEffective(transient);

    EXPECT_EQ(service.GetConfiguredMounts(), configured);
    EXPECT_TRUE(service.IsTransientOverrideActive());
    EXPECT_TRUE(service.HasPendingRestart());
}

TEST_F(HostExecutableLauncherTest, MapsAndLatchesPerUnitActivity)
{
    const auto baseConfig = root / "base.uae";
    std::ofstream(baseConfig) << "filesystem2=ro,BASE:Base:\"base\",-128\n";

    adm::core::HostMountService service;
    auto profile = service.CreateConfiguredProfile(baseConfig);
    profile.mounts[0].enabled = true;
    profile.mounts[0].volumeLabel = "Work";
    profile.mounts[1].enabled = true;
    profile.mounts[1].volumeLabel = "Data";
    service.MarkEffective(profile);

    EXPECT_FALSE(service.DeviceForUnit(0).has_value());
    EXPECT_EQ(service.DeviceForUnit(1), adm::core::HostMountDevice::DH0);
    EXPECT_EQ(service.DeviceForUnit(2), adm::core::HostMountDevice::DH1);
    service.RecordActivity(1, 1, 3);
    service.RecordActivity(1, 2, 2);
    EXPECT_EQ(service.GetActivity()[0].reads, 3u);
    EXPECT_EQ(service.GetActivity()[0].writes, 2u);
    EXPECT_EQ(service.ConsumeActivity(adm::core::HostMountDevice::DH0), 2);
    EXPECT_EQ(service.ConsumeActivity(adm::core::HostMountDevice::DH0), 0);
    const auto now = std::chrono::steady_clock::now();
    EXPECT_EQ(service.GetRecentActivityStatus(adm::core::HostMountDevice::DH0, now), 2);
    EXPECT_EQ(service.GetRecentActivityStatus(adm::core::HostMountDevice::DH0,
                                              now + std::chrono::seconds(1)),
              0);
}

TEST_F(HostExecutableLauncherTest, MigratesMissingAndConflictingMountPreferences)
{
    struct CurrentPathGuard
    {
        std::filesystem::path original = std::filesystem::current_path();
        ~CurrentPathGuard() { std::filesystem::current_path(original); }
    } guard;
    std::filesystem::current_path(root);

    std::ofstream("user_prefs.json") << "{}";
    adm::core::UserPreferences legacy;
    EXPECT_FALSE(legacy.Get().hostDirectoryMounts[0].enabled);
    EXPECT_FALSE(legacy.Get().hostDirectoryMounts[1].enabled);
    EXPECT_TRUE(legacy.Get().hardDriveGroupsOpen[0]);
    EXPECT_TRUE(legacy.Get().hardDriveGroupsOpen[1]);

    legacy.SetHardDriveGroupOpen(1, false);
    adm::core::UserPreferences reloaded;
    EXPECT_TRUE(reloaded.Get().hardDriveGroupsOpen[0]);
    EXPECT_FALSE(reloaded.Get().hardDriveGroupsOpen[1]);

    const auto shared = root / "shared-prefs";
    std::filesystem::create_directories(shared);
    const auto sharedPath = shared.generic_string();
    nlohmann::json saved;
    saved["hostDirectoryMounts"] = {
        {{"enabled", true}, {"directory", sharedPath}, {"label", "Work"}},
        {{"enabled", true}, {"directory", sharedPath}, {"label", "work"}}};
    std::ofstream("user_prefs.json", std::ios::trunc) << saved.dump();

    adm::core::UserPreferences migrated;
    EXPECT_TRUE(migrated.Get().hostDirectoryMounts[0].enabled);
    EXPECT_FALSE(migrated.Get().hostDirectoryMounts[1].enabled);
}

TEST_F(HostExecutableLauncherTest, PreferencesRoundTripHardfileMountsAndRoms)
{
    struct CurrentPathGuard
    {
        std::filesystem::path original = std::filesystem::current_path();
        ~CurrentPathGuard() { std::filesystem::current_path(original); }
    } guard;
    std::filesystem::current_path(root);
    std::ofstream("user_prefs.json") << "{}";

    const auto image = WriteBlankHardfile(root / "prefs.hdf", 2048, true);
    {
        adm::core::UserPreferences prefs;
        auto mounts = prefs.Get().hostDirectoryMounts;
        mounts[1] = HardfileMount(adm::core::HostMountDevice::DH1, image);
        mounts[1].controller = adm::core::HardDriveController::IdeMainboard;
        mounts[1].controllerUnit = 1;
        mounts[1].geometry.mode = adm::core::HardfileGeometryMode::Rdb;
        prefs.SetHostDirectoryMounts(mounts);
        adm::core::HardDriveControllerRoms roms;
        roms.a4091 = root / "a4091.rom";
        prefs.SetHardDriveControllerRoms(roms);
    }

    adm::core::UserPreferences reloaded;
    const auto& mount = reloaded.Get().hostDirectoryMounts[1];
    EXPECT_TRUE(mount.enabled);
    EXPECT_TRUE(mount.IsHardfile());
    EXPECT_EQ(mount.hardfilePath, std::filesystem::weakly_canonical(image));
    EXPECT_EQ(mount.controller, adm::core::HardDriveController::IdeMainboard);
    EXPECT_EQ(mount.controllerUnit, 1);
    EXPECT_EQ(mount.geometry.mode, adm::core::HardfileGeometryMode::Rdb);
    EXPECT_EQ(reloaded.Get().hardDriveControllerRoms.a4091, root / "a4091.rom");

    // A hardfile that has since gone missing loads disabled rather than failing the next boot.
    std::filesystem::remove(image);
    adm::core::UserPreferences missing;
    EXPECT_FALSE(missing.Get().hostDirectoryMounts[1].enabled);
}

TEST_F(HostExecutableLauncherTest, RebasesRelativePathsWithoutModifyingBaseConfig)
{
    const auto baseDirectory = root / "base";
    std::filesystem::create_directories(baseDirectory / "roms");
    const auto baseConfig = baseDirectory / "machine.uae";
    const std::string original =
        "kickstart_rom_file=roms/kick.rom\nfloppy0=disks/boot.adf\nuse_gui=no\n";
    std::ofstream(baseConfig, std::ios::binary) << original;

    adm::core::EmulatorLaunchProfile profile;
    profile.baseUaeConfig = baseConfig;
    std::string error;
    auto config = adm::core::UaeConfigBuilder::Build(profile, error);
    ASSERT_TRUE(config.has_value()) << error;
    EXPECT_NE(config->find((baseDirectory / "roms/kick.rom").generic_string()), std::string::npos);
    EXPECT_NE(config->find((baseDirectory / "disks/boot.adf").generic_string()), std::string::npos);
    EXPECT_EQ(ReadText(baseConfig), original);
}

TEST_F(HostExecutableLauncherTest, WritesGeneratedConfigAtomicallyAcrossRepeatedRuns)
{
    const auto configPath = root / "runtime" / "launch.uae";
    std::string error;
    for (int generation = 0; generation < 25; ++generation) {
        const std::string contents = "generation=" + std::to_string(generation) + "\n";
        ASSERT_TRUE(adm::core::UaeConfigBuilder::WriteAtomic(configPath, contents, error)) << error;
        EXPECT_EQ(ReadText(configPath), contents);
    }
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(configPath.parent_path()),
                            std::filesystem::directory_iterator()),
              1);
}

TEST_F(HostExecutableLauncherTest, BundledCoreLoadsHostMountAcrossRepeatedRuns)
{
#ifdef _WIN32
    const auto coreName = "puae_libretro.dll";
#else
    const auto coreName = "puae_libretro.dylib";
#endif
    const auto corePath = std::filesystem::absolute(std::filesystem::path("emulation") / coreName);
    if (!std::filesystem::exists(corePath))
        GTEST_SKIP() << "Bundled PuAE core is not present at " << corePath;

    const auto program = std::filesystem::absolute(root / "build" / "a.exe");
    WriteHunk(program);

    auto emulator = std::make_unique<adm::AmigaEmulator>();
    // Muted: the launcher boots a real machine, which opens a host playback device.
    emulator->SetMuted(true);
    emulator->SetSystemDirectory((std::filesystem::current_path() / "emulation").string());
    ASSERT_TRUE(emulator->LoadCore(corePath.string()));

    for (int generation = 0; generation < 12; ++generation) {
        WriteHunk(program, static_cast<unsigned char>(generation + 1));
        std::string error;
        ASSERT_TRUE(emulator->RunHostExecutable(program.string(), error))
            << "generation " << generation << ": " << error;
        ASSERT_TRUE(emulator->IsGameLoaded());

        // Advance far enough for the built-in boot ROM to mount DH0/DH1 and run the tiny HUNK.
        // Video presentation stays disabled because unit tests do not own an OpenGL context.
        const int frames = generation == 0 ? 600 : 120;
        for (int frame = 0; frame < frames; ++frame)
            emulator->StepOneFrame(false);
    }
}

TEST_F(HostExecutableLauncherTest, EmulatorCoreSaveDirectoryIsIsolatedPerSession)
{
    const auto systemDirectory = root / "system";
    std::filesystem::create_directories(systemDirectory);

    std::filesystem::path firstSaveDirectory;
    {
        auto emulator = std::make_unique<adm::AmigaEmulator>();
        emulator->SetSystemDirectory(systemDirectory.string());
        firstSaveDirectory = emulator->GetSaveDirectory();

        EXPECT_NE(std::filesystem::absolute(firstSaveDirectory),
                  std::filesystem::absolute(systemDirectory));
        auto expectedTempDirectory = std::filesystem::temp_directory_path().lexically_normal();
        if (expectedTempDirectory.filename().empty())
            expectedTempDirectory = expectedTempDirectory.parent_path();
        EXPECT_EQ(firstSaveDirectory.parent_path().lexically_normal(), expectedTempDirectory);
        EXPECT_TRUE(firstSaveDirectory.filename().string().starts_with("adm_puae_session_"));
        EXPECT_TRUE(std::filesystem::is_directory(firstSaveDirectory));
        std::ofstream(firstSaveDirectory / "adm_emulator_0_df0_save.adf.gz", std::ios::binary)
            << "stale media";
    }
    EXPECT_FALSE(std::filesystem::exists(firstSaveDirectory));

    auto nextSession = std::make_unique<adm::AmigaEmulator>();
    nextSession->SetSystemDirectory(systemDirectory.string());
    EXPECT_NE(std::filesystem::path(nextSession->GetSaveDirectory()), firstSaveDirectory);
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(nextSession->GetSaveDirectory()) /
                                         "adm_emulator_0_df0_save.adf.gz"));
}

TEST_F(HostExecutableLauncherTest, RunsExternalProgramWhenSmokePathIsProvided)
{
    const char* configuredProgram = std::getenv("PEEKABOO_SMOKE_PROGRAM");
    if (!configuredProgram || !*configuredProgram)
        GTEST_SKIP() << "Set PEEKABOO_SMOKE_PROGRAM for a real-program host-mount smoke test";

#ifdef _WIN32
    const auto coreName = "puae_libretro.dll";
#else
    const auto coreName = "puae_libretro.dylib";
#endif
    const auto corePath = std::filesystem::absolute(std::filesystem::path("emulation") / coreName);
    const auto program = std::filesystem::absolute(configuredProgram);
    ASSERT_TRUE(std::filesystem::is_regular_file(corePath));
    ASSERT_TRUE(std::filesystem::is_regular_file(program));

    auto emulator = std::make_unique<adm::AmigaEmulator>();
    // Muted: the launcher boots a real machine, which opens a host playback device.
    emulator->SetMuted(true);
    emulator->SetSystemDirectory((std::filesystem::current_path() / "emulation").string());
    ASSERT_TRUE(emulator->LoadCore(corePath.string()));

    std::string error;
    ASSERT_TRUE(emulator->RunHostExecutable(program.string(), error)) << error;
    for (int frame = 0; frame < 600; ++frame)
        emulator->StepOneFrame(false);
}

TEST_F(HostExecutableLauncherTest, SerializesHardfilesWithControllerAndGeometry)
{
    const auto rdbImage = WriteBlankHardfile(root / "images" / "rdb disk.hdf", 2048, true);
    const auto plainImage = WriteBlankHardfile(root / "images" / "plain.hdf");

    adm::core::EmulatorLaunchProfile profile;
    profile.mounts[0] = HardfileMount(adm::core::HostMountDevice::DH0, rdbImage);
    profile.mounts[0].controller = adm::core::HardDriveController::IdeMainboard;
    profile.mounts[0].controllerUnit = 1;
    profile.mounts[1] = HardfileMount(adm::core::HostMountDevice::DH1, plainImage);
    profile.mounts[1].readOnly = true;
    profile.mounts[1].bootPriority = -128;

    std::string error;
    auto built = adm::core::UaeConfigBuilder::Build(profile, error);
    ASSERT_TRUE(built.has_value()) << error;
    const auto canonicalRdb = std::filesystem::weakly_canonical(rdbImage).generic_string();
    const auto canonicalPlain = std::filesystem::weakly_canonical(plainImage).generic_string();
    // An image with an RDB gets 0,0,0,512 so UAE reads the geometry from it.
    EXPECT_NE(
        built->find("hardfile2=rw,DH0:\"" + canonicalRdb + "\",0,0,0,512,0,,ide1_mainboard\n"),
        std::string::npos)
        << *built;
    // No RDB: the core's 32/1/2/512 guess, on UAE's own controller.
    EXPECT_NE(built->find("hardfile2=ro,DH1:\"" + canonicalPlain + "\",32,1,2,512,-128,,uae0\n"),
              std::string::npos)
        << *built;
    EXPECT_EQ(built->find("_rom_file="), std::string::npos);

    profile.mounts[1].geometry.mode = adm::core::HardfileGeometryMode::Manual;
    profile.mounts[1].geometry.sectors = 63;
    profile.mounts[1].geometry.surfaces = 16;
    profile.mounts[1].geometry.reserved = 2;
    profile.mounts[1].geometry.blockSize = 512;
    built = adm::core::UaeConfigBuilder::Build(profile, error);
    ASSERT_TRUE(built.has_value()) << error;
    EXPECT_NE(built->find(",63,16,2,512,-128,,uae0\n"), std::string::npos) << *built;

    profile.mounts[1].geometry.mode = adm::core::HardfileGeometryMode::Rdb;
    built = adm::core::UaeConfigBuilder::Build(profile, error);
    ASSERT_TRUE(built.has_value()) << error;
    EXPECT_NE(built->find(canonicalPlain + "\",0,0,0,512,-128,,uae0\n"), std::string::npos);
}

TEST_F(HostExecutableLauncherTest, ScsiCardsNeedTheirRomAndEmitItOnce)
{
    const auto image = WriteBlankHardfile(root / "scsi.hdf", 2048, true);
    const auto rom = root / "roms" / "amiga-boot-a590.rom";
    std::filesystem::create_directories(rom.parent_path());
    std::ofstream(rom, std::ios::binary) << std::string(16384, '\0');

    adm::core::EmulatorLaunchProfile profile;
    profile.mounts[0] = HardfileMount(adm::core::HostMountDevice::DH0, image);
    profile.mounts[0].controller = adm::core::HardDriveController::ScsiA590A2091;
    profile.mounts[0].controllerUnit = 3;

    std::string error;
    EXPECT_FALSE(adm::core::UaeConfigBuilder::Build(profile, error).has_value());
    EXPECT_NE(error.find("boot ROM"), std::string::npos) << error;

    profile.controllerRoms.a590A2091 = rom;
    profile.controllerRoms.a4091 = root / "unused.rom"; // not attached, so never checked
    auto built = adm::core::UaeConfigBuilder::Build(profile, error);
    ASSERT_TRUE(built.has_value()) << error;
    EXPECT_NE(built->find("a2091_rom_file=" + rom.generic_string() + "\n"), std::string::npos);
    EXPECT_EQ(built->find("a4091_rom_file="), std::string::npos);
    EXPECT_NE(built->find(",0,0,0,512,0,,scsi3_a2091\n"), std::string::npos) << *built;
}

TEST_F(HostExecutableLauncherTest, ValidatesHardfileMounts)
{
    const auto image = WriteBlankHardfile(root / "disk.hdf");
    std::array<adm::core::HostDirectoryMount, 2> mounts;
    mounts[0] = HardfileMount(adm::core::HostMountDevice::DH0, root / "missing.hdf");
    mounts[1].device = adm::core::HostMountDevice::DH1;
    std::string error;
    EXPECT_FALSE(adm::core::UaeConfigBuilder::ValidateMounts(mounts, error));
    EXPECT_NE(error.find("missing"), std::string::npos);

    mounts[0].hardfilePath = image;
    mounts[0].controller = adm::core::HardDriveController::IdeMainboard;
    mounts[0].controllerUnit = 2;
    EXPECT_FALSE(adm::core::UaeConfigBuilder::ValidateMounts(mounts, error));
    EXPECT_NE(error.find("unit must be 0-1"), std::string::npos) << error;

    // Two drives on the same IDE position collide; the same file twice is refused outright.
    const auto second = WriteBlankHardfile(root / "second.hdf");
    mounts[0].controllerUnit = 0;
    mounts[1] = HardfileMount(adm::core::HostMountDevice::DH1, second);
    mounts[1].controller = adm::core::HardDriveController::IdeMainboard;
    EXPECT_FALSE(adm::core::UaeConfigBuilder::ValidateMounts(mounts, error));
    EXPECT_NE(error.find("both on"), std::string::npos) << error;
    mounts[1].controllerUnit = 1;
    EXPECT_TRUE(adm::core::UaeConfigBuilder::ValidateMounts(mounts, error)) << error;
    mounts[1].hardfilePath = image;
    EXPECT_FALSE(adm::core::UaeConfigBuilder::ValidateMounts(mounts, error));
    EXPECT_NE(error.find("same hardfile"), std::string::npos) << error;

    // A hardfile and a directory may share nothing but still coexist.
    const auto directory = root / "dir";
    std::filesystem::create_directories(directory);
    mounts[1] = {};
    mounts[1].device = adm::core::HostMountDevice::DH1;
    mounts[1].enabled = true;
    mounts[1].hostDirectory = directory;
    mounts[1].volumeLabel = "DH0";
    EXPECT_TRUE(adm::core::UaeConfigBuilder::ValidateMounts(mounts, error)) << error;

    // Bad manual geometry.
    mounts[0].geometry.mode = adm::core::HardfileGeometryMode::Manual;
    mounts[0].geometry.blockSize = 700;
    EXPECT_FALSE(adm::core::UaeConfigBuilder::ValidateMounts(mounts, error));
    EXPECT_NE(error.find("manual geometry"), std::string::npos) << error;
}

TEST_F(HostExecutableLauncherTest, CompressedHardfilesAreForcedReadOnly)
{
    const auto image = WriteBlankHardfile(root / "disk.hdz");
    std::array<adm::core::HostDirectoryMount, 2> mounts;
    mounts[0] = HardfileMount(adm::core::HostMountDevice::DH0, image);
    mounts[0].geometry.mode = adm::core::HardfileGeometryMode::Rdb;
    mounts[1].device = adm::core::HostMountDevice::DH1;
    ASSERT_FALSE(mounts[0].readOnly);
    std::string error;
    ASSERT_TRUE(adm::core::UaeConfigBuilder::ValidateMounts(mounts, error)) << error;
    EXPECT_TRUE(mounts[0].readOnly);
}

TEST_F(HostExecutableLauncherTest, ControllersReportWhichModelsCanTakeThem)
{
    using adm::core::HardDriveController;
    using adm::core::UaeConfigBuilder;
    EXPECT_TRUE(
        UaeConfigBuilder::ControllerUnavailableReason(HardDriveController::Uae, "A500").empty());
    EXPECT_FALSE(
        UaeConfigBuilder::ControllerUnavailableReason(HardDriveController::IdeMainboard, "A500")
            .empty());
    EXPECT_TRUE(
        UaeConfigBuilder::ControllerUnavailableReason(HardDriveController::IdeMainboard, "A1200")
            .empty());
    EXPECT_FALSE(
        UaeConfigBuilder::ControllerUnavailableReason(HardDriveController::ScsiA4091, "A1200")
            .empty());
    EXPECT_TRUE(
        UaeConfigBuilder::ControllerUnavailableReason(HardDriveController::ScsiA4091, "A4040")
            .empty());
    EXPECT_EQ(adm::core::ParseHardDriveController("A590"), HardDriveController::ScsiA590A2091);
    EXPECT_EQ(adm::core::ParseHardDriveController("ide"), HardDriveController::IdeMainboard);
    EXPECT_FALSE(adm::core::ParseHardDriveController("sata").has_value());
}

TEST_F(HostExecutableLauncherTest, OnlyUaeUnitsWaitForGuestConfirmation)
{
    const auto image = WriteBlankHardfile(root / "ide.hdf", 2048, true);
    const auto directory = root / "work";
    std::filesystem::create_directories(directory);

    adm::core::HostMountService service;
    adm::core::EmulatorLaunchProfile profile;
    profile.mounts[0] = HardfileMount(adm::core::HostMountDevice::DH0, image);
    profile.mounts[0].controller = adm::core::HardDriveController::IdeMainboard;
    profile.mounts[1].device = adm::core::HostMountDevice::DH1;
    service.MarkEffective(profile);
    // The IDE unit is not visible to the guest-confirmation query, so nothing is awaited.
    EXPECT_FALSE(service.CheckConfirmationTimeout(std::chrono::steady_clock::now() +
                                                  std::chrono::minutes(1)));
    EXPECT_EQ(service.DeviceForUnit(0), adm::core::HostMountDevice::DH0);
    EXPECT_FALSE(service.DeviceForUnit(1).has_value());

    profile.mounts[1].enabled = true;
    profile.mounts[1].hostDirectory = directory;
    profile.mounts[1].volumeLabel = "Work";
    service.MarkEffective(profile);
    EXPECT_EQ(service.DeviceForUnit(1), adm::core::HostMountDevice::DH1);
    EXPECT_TRUE(service.CheckConfirmationTimeout(std::chrono::steady_clock::now() +
                                                 std::chrono::minutes(1)));
}

TEST_F(HostExecutableLauncherTest, ControllerRomChangeIsPendingOnlyWhenACardIsInUse)
{
    const auto image = WriteBlankHardfile(root / "scsi.hdf", 2048, true);
    adm::core::HostMountService service;
    adm::core::HardDriveControllerRoms roms;
    roms.a590A2091 = root / "a590.rom";
    service.SetControllerRoms(roms);

    auto profile = service.CreateConfiguredProfile();
    profile.mounts[0] = HardfileMount(adm::core::HostMountDevice::DH0, image);
    service.SetConfiguredMounts(profile.mounts);
    profile = service.CreateConfiguredProfile();
    service.MarkEffective(profile);
    EXPECT_FALSE(service.HasPendingRestart());

    roms.a590A2091 = root / "other.rom";
    service.SetControllerRoms(roms);
    EXPECT_FALSE(service.HasPendingRestart()); // DH0 is on the UAE controller

    auto mounts = service.GetConfiguredMounts();
    mounts[0].controller = adm::core::HardDriveController::ScsiA590A2091;
    service.SetConfiguredMounts(mounts);
    service.MarkEffective(service.CreateConfiguredProfile());
    EXPECT_FALSE(service.HasPendingRestart());
    roms.a590A2091 = root / "third.rom";
    service.SetControllerRoms(roms);
    EXPECT_TRUE(service.HasPendingRestart());
}

} // namespace
