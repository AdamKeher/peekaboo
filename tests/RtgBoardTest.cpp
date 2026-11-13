#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "core/AmigaEmulator.h"
#include "core/HostMountService.h"

// RTG / Picasso96 (uaegfx), core 1.99.0.
//
// PICASSO96 was commented out in retrodep/sysconfig.h, so picasso96.c and gfxboard.c both
// compiled to empty objects and there was no graphics card of any kind. Turning it on fits an
// autoconfig board whose VRAM the guest's Picasso96 draws into; the core converts that buffer
// to the front-end's pixel format and hands it over as the frame.
//
// What these tests can prove without a guest: the board is really fitted and really sized, the
// Zorro II / Zorro III choice reaches autoconfig, and the display stays on the chipset until
// something in the guest opens an RTG screen. Proving a screen actually opens needs Picasso96
// installed on a bootable Workbench, which no fixture in this repo has.
namespace {

#ifdef _WIN32
constexpr const char* kCoreName = "puae_libretro.dll";
#else
constexpr const char* kCoreName = "puae_libretro.dylib";
#endif

std::unique_ptr<adm::AmigaEmulator> BootWithRtg(const std::string& sizeMb, const std::string& type,
                                                const char* fastMem = nullptr)
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / kCoreName;
    if (!std::filesystem::is_regular_file(corePath))
        return nullptr;
    auto emulator = std::make_unique<adm::AmigaEmulator>();
    // Muted: StartEmpty() opens a real host playback device. See EmulatorWarmResetTest.
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    // An 020 with a 32-bit address space, so a Zorro III board has somewhere to land.
    if (!emulator->SelectModelPreset(adm::AmigaModel::A1200))
        return nullptr;
    emulator->SetUserVariable("puae_gfxcard_size", sizeMb);
    emulator->SetUserVariable("puae_gfxcard_type", type);
    if (fastMem)
        emulator->SetUserVariable("puae_fastmem_size", fastMem);
    if (!emulator->LoadCore(corePath.string()) || !emulator->StartEmpty())
        return nullptr;
    return emulator;
}

// The board is a memory bank of its own, so "did the machine fit one" is answerable from the
// memory map without reading the core's log.
const adm::AmigaEmulator::MemoryRegion* FindRtgBank(const adm::AmigaEmulator& emulator)
{
    static adm::AmigaEmulator::MemoryRegion found;
    const auto regions = emulator.GetMemoryRegions();
    const auto it = std::find_if(regions.begin(), regions.end(),
                                 [](const auto& region) { return region.name == "RTG VRAM"; });
    if (it == regions.end())
        return nullptr;
    found = *it;
    return &found;
}

// For the failure message: which banks the machine does have. A missing RTG bank means either
// the board was not configured or the core is not exporting gfxmem_bank, and the two look the
// same until you see the list.
std::string RegionNames(const adm::AmigaEmulator& emulator)
{
    std::string out;
    for (const auto& region : emulator.GetMemoryRegions()) {
        if (!out.empty())
            out += ", ";
        out += region.name + "@" + std::to_string(region.amigaAddr) + "+" +
               std::to_string(region.size);
    }
    return out.empty() ? "(none)" : out;
}

TEST(RtgBoard, NoBoardIsFittedWhenTheOptionIsDisabled)
{
    auto emulator = BootWithRtg("0", "ZorroIII");
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";

    ASSERT_TRUE(emulator->HasRtg()) << "core is older than 1.99.0 (no adm_get_rtg_state)";
    const auto state = emulator->GetRtgState();
    EXPECT_EQ(state.boardSize, 0u);
    EXPECT_FALSE(state.active);
    EXPECT_EQ(FindRtgBank(*emulator), nullptr);
}

TEST(RtgBoard, AZorroIiiBoardIsFittedAtTheRequestedSize)
{
    auto emulator = BootWithRtg("8", "ZorroIII");
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    if (!emulator->HasRtg())
        GTEST_SKIP() << "core is older than 1.99.0";

    const auto state = emulator->GetRtgState();
    EXPECT_EQ(state.boardSize, 8u * 1024 * 1024);

    const auto* bank = FindRtgBank(*emulator);
    ASSERT_NE(bank, nullptr) << "an 8 MB board should appear in the memory map; banks: "
                             << RegionNames(*emulator);
    EXPECT_EQ(bank->size, 8u * 1024 * 1024);
    // Zorro III space starts at $10000000. A board that ended up below it was autoconfigured as
    // Zorro II, which is the failure the board-type option exists to avoid.
    EXPECT_GE(bank->amigaAddr, 0x10000000u)
        << "a ZorroIII board landed in Zorro II space at " << std::hex << bank->amigaAddr;
}

TEST(RtgBoard, AZorroIiBoardLandsInZorroIiSpace)
{
    // 2 MB, and the preset's 8 MB of Zorro II Fast RAM turned off. That is not tidying up: the
    // A1200 preset fills the whole $200000-$A00000 Zorro II window with Fast RAM, so a Z2
    // graphics board has nowhere to autoconfigure to and silently stays unplaced. It is a real
    // constraint of the machine, not of this port - which is why Zorro III is the default.
    auto emulator = BootWithRtg("2", "ZorroII", "0");
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    if (!emulator->HasRtg())
        GTEST_SKIP() << "core is older than 1.99.0";

    EXPECT_EQ(emulator->GetRtgState().boardSize, 2u * 1024 * 1024);

    const auto* bank = FindRtgBank(*emulator);
    ASSERT_NE(bank, nullptr) << "a 2 MB board should appear in the memory map; banks: "
                             << RegionNames(*emulator);
    EXPECT_EQ(bank->size, 2u * 1024 * 1024) << RegionNames(*emulator);
    EXPECT_GE(bank->amigaAddr, 0x00200000u);
    EXPECT_LT(bank->amigaAddr, 0x00A00000u)
        << "a ZorroII board should be inside the Z2 window, not at " << std::hex << bank->amigaAddr;
}

// The board being fitted must not put the display on it. Kickstart alone never opens an RTG
// screen - that takes Picasso96 in the guest - so the picture has to stay the chipset's, and
// with it every raster overlay that reads beam geometry.
TEST(RtgBoard, TheDisplayStaysOnTheChipsetUntilTheGuestOpensAnRtgScreen)
{
    auto emulator = BootWithRtg("8", "ZorroIII");
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";
    if (!emulator->HasRtg())
        GTEST_SKIP() << "core is older than 1.99.0";

    for (int i = 0; i < 100; i++)
        emulator->StepOneFrame(false);

    const auto state = emulator->GetRtgState();
    EXPECT_FALSE(state.active);
    EXPECT_EQ(state.width, 0u);
    EXPECT_EQ(state.height, 0u);
    EXPECT_FALSE(emulator->IsRtgDisplayActive());
    // And the chipset frame is still the one being presented, at its usual size.
    EXPECT_GT(emulator->GetWidth(), 0u);
    EXPECT_GT(emulator->GetHeight(), 0u);
}

// Turning the board on must not disturb the machine that was there before it. This is the
// regression that matters most: PICASSO96 adds uaegfx.card to the boot ROM and runs
// picasso96_alloc() on every boot, whether or not a board is fitted.
TEST(RtgBoard, AMachineWithNoBoardBootsExactlyAsItDidBefore)
{
    auto withBoard = BootWithRtg("0", "ZorroIII");
    if (!withBoard)
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";

    for (int i = 0; i < 100; i++)
        withBoard->StepOneFrame(false);

    // Chip RAM and the Kickstart are where they always were, and the machine is drawing.
    const auto regions = withBoard->GetMemoryRegions();
    EXPECT_TRUE(std::any_of(regions.begin(), regions.end(),
                            [](const auto& r) { return r.name == "Chip RAM"; }));
    EXPECT_TRUE(std::any_of(regions.begin(), regions.end(),
                            [](const auto& r) { return r.name == "Kickstart ROM"; }));
    EXPECT_GT(withBoard->GetWidth(), 0u);
    EXPECT_GT(withBoard->GetHeight(), 0u);
}

// The A4040 RTG preset (core 1.99.0) is the A4040 with a board already fitted, so selecting it
// has to produce one without anybody touching an option.
TEST(RtgBoard, TheA4040RtgPresetFitsItsOwnBoard)
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / kCoreName;
    if (!std::filesystem::is_regular_file(corePath))
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";

    auto emulator = std::make_unique<adm::AmigaEmulator>();
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    ASSERT_TRUE(emulator->SelectModelPreset(adm::AmigaModel::A4040RTG));
    ASSERT_TRUE(emulator->LoadCore(corePath.string()));
    ASSERT_TRUE(emulator->StartEmpty());
    if (!emulator->HasRtg())
        GTEST_SKIP() << "core is older than 1.99.0";

    EXPECT_EQ(emulator->GetRtgState().boardSize, 8u * 1024 * 1024);
    const auto* bank = FindRtgBank(*emulator);
    ASSERT_NE(bank, nullptr) << "the preset should fit a board; banks: " << RegionNames(*emulator);
    EXPECT_GE(bank->amigaAddr, 0x10000000u) << "the preset's board should be Zorro III";
}

// And the plain A4040 must not, or the board would be a surprise on every A4000. This is the
// half that is easy to get wrong: SelectModelPreset() has to write puae_gfxcard_size for every
// preset, not just the one that wants a board, or switching away leaves the card behind.
TEST(RtgBoard, SwitchingToAPresetWithoutABoardTakesTheBoardAway)
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / kCoreName;
    if (!std::filesystem::is_regular_file(corePath))
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";

    auto emulator = std::make_unique<adm::AmigaEmulator>();
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    ASSERT_TRUE(emulator->SelectModelPreset(adm::AmigaModel::A4040RTG));
    ASSERT_TRUE(emulator->LoadCore(corePath.string()));
    ASSERT_TRUE(emulator->StartEmpty());
    if (!emulator->HasRtg())
        GTEST_SKIP() << "core is older than 1.99.0";
    ASSERT_EQ(emulator->GetRtgState().boardSize, 8u * 1024 * 1024);

    ASSERT_TRUE(emulator->SelectModelPreset(adm::AmigaModel::A4040));
    ASSERT_TRUE(emulator->StartEmpty());
    EXPECT_EQ(emulator->GetRtgState().boardSize, 0u)
        << "the board followed the model switch; banks: " << RegionNames(*emulator);
}

// THE regression test for this feature.
//
// Every test above boots through StartEmpty(), and they all passed while RTG was completely
// broken on the launch path anyone would actually use. retro_create_config() has five branches;
// four append uae_config and the fifth - the one that boots a .uae config file - reads the file
// row by row and goes straight to retro_config_kickstart(). Peekaboo boots every hard-disk,
// WHDLoad and CD machine through a generated launch.uae, so a board written into uae_config was
// fitted on an empty boot and silently absent on any machine with a hard drive.
//
// StartHostMountProfile() is that path. Fixed by emitting the rows from
// adm_machine_overrides_config(), which runs after all five branches.
TEST(RtgBoard, TheBoardSurvivesABootThroughTheGeneratedConfig)
{
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / kCoreName;
    if (!std::filesystem::is_regular_file(corePath))
        GTEST_SKIP() << "Bundled PuAE core is not present in emulation/";

    std::error_code ec;
    const auto dh0 = std::filesystem::temp_directory_path() / "adm-rtg-generated-config" / "dh0";
    std::filesystem::remove_all(dh0, ec);
    std::filesystem::create_directories(dh0, ec);

    auto emulator = std::make_unique<adm::AmigaEmulator>();
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    ASSERT_TRUE(emulator->SelectModelPreset(adm::AmigaModel::A4040RTG));
    ASSERT_TRUE(emulator->LoadCore(corePath.string()));

    // A host directory on DH0 is enough to force the generated-config path - it is the presence
    // of a hard drive that decides, not what kind.
    adm::core::EmulatorLaunchProfile profile = emulator->CreateLaunchProfile();
    profile.modelId = "A4040RTG";
    adm::core::HostDirectoryMount mount;
    mount.backingType = adm::core::HostMountBackingType::Directory;
    mount.device = adm::core::HostMountDevice::DH0;
    mount.enabled = true;
    mount.hostDirectory = dh0;
    mount.volumeLabel = "DH0";
    mount.readOnly = false;
    profile.mounts[0] = mount;

    std::string error;
    ASSERT_TRUE(emulator->StartHostMountProfile(profile, error)) << error;
    if (!emulator->HasRtg())
        GTEST_SKIP() << "core is older than 1.99.0";

    EXPECT_EQ(emulator->GetRtgState().boardSize, 8u * 1024 * 1024)
        << "the board did not survive the generated-config boot";
    EXPECT_NE(FindRtgBank(*emulator), nullptr)
        << "no RTG bank after a generated-config boot; banks: " << RegionNames(*emulator);
}

} // namespace
