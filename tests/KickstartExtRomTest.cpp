#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "core/AmigaEmulator.h"

// puae_kickstart_ext (core 1.97.0) is the extended ROM slot the Preferences ROM page writes;
// the "Extended ROM" memory region it is checked through is exported from core 1.98.0.
// Before it, the extended ROM was whatever the model preset named, and the only way past that
// was a hand-written .uae - which does not cover every launch, because StartEmpty() boots with
// no config file at all. These tests go through StartEmpty() for exactly that reason.
namespace {

#ifdef _WIN32
constexpr const char* kCoreName = "puae_libretro.dll";
#else
constexpr const char* kCoreName = "puae_libretro.dylib";
#endif

std::unique_ptr<adm::AmigaEmulator> BootCd32(const std::string& extendedRom)
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / kCoreName;
    if (!std::filesystem::is_regular_file(corePath))
        return nullptr;
    auto emulator = std::make_unique<adm::AmigaEmulator>();
    // Muted: StartEmpty() opens a real host playback device. See EmulatorWarmResetTest.
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    if (!emulator->SelectModelPreset(adm::AmigaModel::CD32))
        return nullptr;
    if (!extendedRom.empty())
        emulator->SetUserVariable("puae_kickstart_ext", extendedRom);
    if (!emulator->LoadCore(corePath.string()) || !emulator->StartEmpty())
        return nullptr;
    return emulator;
}

// The extended ROM is a memory bank of its own, so "did the machine take it" is answerable
// without reading the core's log.
bool HasExtendedRomBank(const adm::AmigaEmulator& emulator)
{
    const auto regions = emulator.GetMemoryRegions();
    return std::any_of(regions.begin(), regions.end(),
                       [](const auto& region) { return region.name == "Extended ROM"; });
}

TEST(KickstartExtRom, AutoLeavesTheModelPresetsOwnExtendedRom)
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    if (!std::filesystem::is_regular_file(systemDirectory / "kick40060.CD32"))
        GTEST_SKIP() << "kick40060.CD32 is not present in " << systemDirectory;

    auto emulator = BootCd32("auto");
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    EXPECT_TRUE(HasExtendedRomBank(*emulator))
        << "a CD32 on 'auto' should still get the preset's extended ROM";
}

// The point of the option: an absolute path, used as it is rather than joined onto the system
// directory.
//
// The ROM is copied OUT of the system directory first, under a name nothing looks for. Pointing
// at the copy in emulation/ would prove nothing - the preset would have found that file by
// itself, so the test would pass with the option ignored entirely.
TEST(KickstartExtRom, AnAbsolutePathOutsideTheSystemDirectoryIsUsedAsGiven)
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto source = systemDirectory / "kick40060.CD32.ext";
    if (!std::filesystem::is_regular_file(source))
        GTEST_SKIP() << "kick40060.CD32.ext is not present in " << systemDirectory;

    std::error_code ec;
    const auto staging = std::filesystem::temp_directory_path() / "adm-ext-rom-test";
    std::filesystem::create_directories(staging, ec);
    const auto elsewhere = staging / "some-extended-rom.bin";
    std::filesystem::copy_file(source, elsewhere, std::filesystem::copy_options::overwrite_existing,
                               ec);
    ASSERT_FALSE(ec) << "could not stage the ROM outside the system directory: " << ec.message();

    auto emulator = BootCd32(elsewhere.string());
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    EXPECT_TRUE(HasExtendedRomBank(*emulator))
        << "the machine should have taken the extended ROM at " << elsewhere.string();

    emulator.reset(); // release the core before removing the file it read
    std::filesystem::remove(elsewhere, ec);
}

// A path that does not resolve must not take the machine down: the core logs the miss and boots
// without an extended ROM, the same as any other missing-ROM case.
TEST(KickstartExtRom, AnUnresolvablePathDoesNotKillTheMachine)
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    if (!std::filesystem::is_regular_file(systemDirectory / "kick40060.CD32"))
        GTEST_SKIP() << "kick40060.CD32 is not present in " << systemDirectory;

    auto emulator = BootCd32((systemDirectory / "no-such-extended-rom.ext").string());
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    for (int i = 0; i < 20; ++i)
        emulator->StepOneFrame(false);
    EXPECT_TRUE(emulator->IsGameLoaded());
    EXPECT_GE(emulator->GetTotalFrameCount(), 20u);
}

} // namespace
