#include "core/AmigaEmulator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace {

#ifdef _WIN32
constexpr const char* kCoreName = "puae_libretro.dll";
#else
constexpr const char* kCoreName = "puae_libretro.dylib";
#endif

// TRAP #0's exception vector. Kickstart rewrites the whole vector table while it comes up, so a
// sentinel written here is proof of a reboot: it survives ordinary running and is gone once the
// machine has reset.
constexpr uint32_t kVectorTrap0 = 0x80;
constexpr uint32_t kSentinel = 0x5A5A5A5Au;

uint8_t* ChipRam(adm::AmigaEmulator& emulator)
{
    for (const auto& region : emulator.GetMemoryRegions()) {
        if (region.name == "Chip RAM")
            return region.hostPtr;
    }
    return nullptr;
}

void PokeLong(uint8_t* chip, uint32_t address, uint32_t value)
{
    chip[address + 0] = static_cast<uint8_t>(value >> 24);
    chip[address + 1] = static_cast<uint8_t>(value >> 16);
    chip[address + 2] = static_cast<uint8_t>(value >> 8);
    chip[address + 3] = static_cast<uint8_t>(value);
}

uint32_t PeekLong(const uint8_t* chip, uint32_t address)
{
    return (static_cast<uint32_t>(chip[address + 0]) << 24) |
           (static_cast<uint32_t>(chip[address + 1]) << 16) |
           (static_cast<uint32_t>(chip[address + 2]) << 8) | chip[address + 3];
}

void RunFrames(adm::AmigaEmulator& emulator, int frames)
{
    for (int i = 0; i < frames; ++i)
        emulator.StepOneFrame(false);
}

std::vector<uint8_t> ReadFixtureAdf(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
}

} // namespace

// The bug this guards: resetting used to rebuild the machine, which dropped the disk the user had
// booted and handed the boot back to the hard drive. A reset must warm-boot the machine that is
// already running and leave DF0's media exactly where it is.
TEST(EmulatorWarmReset, KeepsDiskInDriveAndRebootsMachine)
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / kCoreName;
    if (!std::filesystem::is_regular_file(corePath))
        GTEST_SKIP() << "Bundled PuAE core is not present at " << corePath;

    const auto adfPath = std::filesystem::absolute("images/OFS.adf");
    const auto adf = ReadFixtureAdf(adfPath);
    if (adf.empty())
        GTEST_SKIP() << "Test ADF is not present at " << adfPath;

    // Heap, not stack: AmigaEmulator is ~5 MB of inline event caches and screen buffers, which
    // overflows the 2 MB main-thread stack the moment it is declared as a local.
    auto emulatorOwner = std::make_unique<adm::AmigaEmulator>();
    adm::AmigaEmulator& emulator = *emulatorOwner;
    // Muted: LoadADF() below opens a real host playback device and the boot plays out of the
    // speakers. Nothing this test looks at comes from the device.
    emulator.SetMuted(true);
    emulator.SetSystemDirectory(systemDirectory.string());
    ASSERT_TRUE(emulator.SelectModelPreset(adm::AmigaModel::A500));
    ASSERT_TRUE(emulator.LoadCore(corePath.string()));
    ASSERT_TRUE(emulator.LoadADF(adf));

    RunFrames(emulator, 240); // long enough for Kickstart to come up and read the disk
    ASSERT_TRUE(emulator.IsDiskInserted(0)) << "DF0 should hold the loaded disk before the reset";

    uint8_t* chip = ChipRam(emulator);
    ASSERT_NE(chip, nullptr);
    PokeLong(chip, kVectorTrap0, kSentinel);
    ASSERT_EQ(PeekLong(chip, kVectorTrap0), kSentinel);

    const uint64_t framesBefore = emulator.GetTotalFrameCount();

    ASSERT_TRUE(emulator.Reset()) << "a loaded machine must take the warm-reset path";

    // Warm means warm: the game is never unloaded, so the frame counter keeps running rather
    // than restarting from a fresh session.
    EXPECT_TRUE(emulator.IsGameLoaded());
    EXPECT_EQ(emulator.GetTotalFrameCount(), framesBefore);

    // Ctrl + Left-Amiga + Right-Amiga is held across the next few frames; UAE resets on release.
    EXPECT_TRUE(emulator.IsInjectedKeyHeld(static_cast<int>(adm::AmigaEmulator::RetroKey::LCtrl)));
    EXPECT_TRUE(emulator.IsInjectedKeyHeld(static_cast<int>(adm::AmigaEmulator::RetroKey::LSuper)));
    EXPECT_TRUE(emulator.IsInjectedKeyHeld(static_cast<int>(adm::AmigaEmulator::RetroKey::RSuper)));

    RunFrames(emulator, 10);
    EXPECT_FALSE(emulator.IsInjectedKeyHeld(static_cast<int>(adm::AmigaEmulator::RetroKey::LCtrl)));

    RunFrames(emulator, 240);
    EXPECT_NE(PeekLong(chip, kVectorTrap0), kSentinel)
        << "Kickstart should have rebuilt the exception vectors, i.e. the machine rebooted";
    EXPECT_TRUE(emulator.IsGameLoaded());
    EXPECT_TRUE(emulator.IsDiskInserted(0)) << "the disk must still be in DF0 after a reset";
}
