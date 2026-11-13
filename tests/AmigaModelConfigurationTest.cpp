#include "core/AmigaEmulator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>

namespace {

TEST(AmigaModelConfiguration, A500PresetReplacesConflictingMachineOverrides)
{
    auto emulator = std::make_unique<adm::AmigaEmulator>();
    emulator->SetUserVariables({{"puae_model", "A1200"},
                                {"puae_cpu_model", "68020"},
                                {"puae_fpu_model", "68881"},
                                {"puae_kickstart", "kick40063.A600"},
                                {"puae_chipmem_size", "2"},
                                {"puae_bogomem_size", "0"},
                                {"puae_fastmem_size", "8"},
                                {"puae_z3mem_size", "16"},
                                {"puae_video_standard", "NTSC"}});

    emulator->SelectModelPreset(adm::AmigaModel::A500);

    EXPECT_EQ(emulator->GetCurrentModel(), adm::AmigaModel::A500);
    const auto& variables = emulator->GetUserVariables();
    EXPECT_EQ(variables.count("puae_model"), 0u);
    EXPECT_EQ(variables.at("puae_cpu_model"), "auto");
    EXPECT_EQ(variables.at("puae_fpu_model"), "auto");
    EXPECT_EQ(variables.at("puae_kickstart"), "kick34005.A500");
    EXPECT_EQ(variables.at("puae_chipmem_size"), "1");
    EXPECT_EQ(variables.at("puae_bogomem_size"), "2");
    EXPECT_EQ(variables.at("puae_fastmem_size"), "0");
    EXPECT_EQ(variables.at("puae_z3mem_size"), "auto");
    EXPECT_EQ(variables.at("puae_video_standard"), "NTSC");
}

TEST(AmigaModelConfiguration, A500PlusUsesPuAesModelIdentifierWithoutChangingItsSavedId)
{
    const auto& info = adm::GetAmigaModelInfo(adm::AmigaModel::A500Plus);
    EXPECT_STREQ(info.id, "A500+");
    EXPECT_STREQ(info.coreId, "A500PLUS");
}

TEST(AmigaModelConfiguration, A600AndA2000PresetsMatchTheirDisplayedKickstartVersions)
{
    auto emulator = std::make_unique<adm::AmigaEmulator>();

    emulator->SelectModelPreset(adm::AmigaModel::A600);
    EXPECT_EQ(emulator->GetUserVariables().at("puae_kickstart"), "kick37350.A600");

    emulator->SelectModelPreset(adm::AmigaModel::A2000);
    EXPECT_EQ(emulator->GetUserVariables().at("puae_kickstart"), "kick37175.A500");
}

TEST(AmigaModelConfiguration, BundledCoreBootsSelectedA500HardwareInsteadOfSavedOverrides)
{
#ifdef _WIN32
    constexpr const char* coreName = "puae_libretro.dll";
#else
    constexpr const char* coreName = "puae_libretro.dylib";
#endif
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / coreName;
    if (!std::filesystem::is_regular_file(corePath))
        GTEST_SKIP() << "Bundled PuAE core is not present at " << corePath;

    auto emulator = std::make_unique<adm::AmigaEmulator>();
    // Muted: StartEmpty() opens a real host playback device. See EmulatorWarmResetTest.
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    emulator->SetUserVariables({{"puae_kickstart", "kick40063.A600"},
                                {"puae_chipmem_size", "2"},
                                {"puae_bogomem_size", "0"},
                                {"puae_fastmem_size", "8"}});
    emulator->SelectModelPreset(adm::AmigaModel::A500);

    ASSERT_TRUE(emulator->LoadCore(corePath.string()));
    ASSERT_TRUE(emulator->StartEmpty());

    const auto regions = emulator->GetMemoryRegions();
    const auto findRegion = [&](const char* name) {
        return std::find_if(regions.begin(), regions.end(),
                            [&](const auto& region) { return region.name == name; });
    };
    const auto chip = findRegion("Chip RAM");
    const auto slow = std::find_if(regions.begin(), regions.end(), [](const auto& region) {
        return region.name.starts_with("Slow RAM");
    });
    const auto kickstart = findRegion("Kickstart ROM");
    ASSERT_NE(chip, regions.end());
    ASSERT_NE(slow, regions.end());
    ASSERT_NE(kickstart, regions.end());
    EXPECT_EQ(chip->size, 512u * 1024u);
    EXPECT_EQ(slow->size, 512u * 1024u);
    EXPECT_EQ(findRegion("Fast RAM"), regions.end());
    ASSERT_NE(kickstart->hostPtr, nullptr);
    ASSERT_GE(kickstart->size, 14u);
    const uint16_t kickstartVersion =
        (static_cast<uint16_t>(kickstart->hostPtr[12]) << 8) | kickstart->hostPtr[13];
    EXPECT_EQ(kickstartVersion, 34u);
}

// The bug this guards: a Kickstart the CPU cannot run (here the A1200's 3.1 ROM, which needs a
// 68EC020, on an A500's 68000) made the core ask for a restart during load. It never built its
// opcode table and the first instruction called through NULL, killing the host process - this
// whole test binary, not just the test. Saved settings from before the model id was persisted
// restored exactly this pair. Core 1.94.0 reports the mismatch and boots the ROM anyway.
TEST(AmigaModelConfiguration, KickstartTheCpuCannotRunStartsInsteadOfCrashingTheHost)
{
#ifdef _WIN32
    constexpr const char* coreName = "puae_libretro.dll";
#else
    constexpr const char* coreName = "puae_libretro.dylib";
#endif
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / coreName;
    if (!std::filesystem::is_regular_file(corePath))
        GTEST_SKIP() << "Bundled PuAE core is not present at " << corePath;
    if (!std::filesystem::is_regular_file(systemDirectory / "kick40068.A1200"))
        GTEST_SKIP() << "kick40068.A1200 is not present in " << systemDirectory;

    auto emulator = std::make_unique<adm::AmigaEmulator>();
    // Muted: StartEmpty() opens a real host playback device. See EmulatorWarmResetTest.
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    ASSERT_TRUE(emulator->SelectModelPreset(adm::AmigaModel::A500));
    emulator->SetUserVariable("puae_kickstart", "kick40068.A1200");

    ASSERT_TRUE(emulator->LoadCore(corePath.string()));
    ASSERT_TRUE(emulator->StartEmpty());
    for (int i = 0; i < 50; ++i)
        emulator->StepOneFrame(false);

    EXPECT_TRUE(emulator->IsGameLoaded());
    EXPECT_GE(emulator->GetTotalFrameCount(), 50u);
    const auto regions = emulator->GetMemoryRegions();
    const auto kickstart = std::find_if(regions.begin(), regions.end(), [](const auto& region) {
        return region.name == "Kickstart ROM";
    });
    ASSERT_NE(kickstart, regions.end());
    ASSERT_NE(kickstart->hostPtr, nullptr);
    ASSERT_GE(kickstart->size, 14u);
    const uint16_t kickstartVersion =
        (static_cast<uint16_t>(kickstart->hostPtr[12]) << 8) | kickstart->hostPtr[13];
    EXPECT_EQ(kickstartVersion, 40u) << "the machine should hold the ROM it was given";
}

TEST(AmigaModelConfiguration, LiveSelectionColdReinitializesCoreBeforeLoadingNewMachine)
{
#ifdef _WIN32
    constexpr const char* coreName = "puae_libretro.dll";
#else
    constexpr const char* coreName = "puae_libretro.dylib";
#endif
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / coreName;
    if (!std::filesystem::is_regular_file(corePath))
        GTEST_SKIP() << "Bundled PuAE core is not present at " << corePath;

    auto emulator = std::make_unique<adm::AmigaEmulator>();
    // Muted: StartEmpty() opens a real host playback device. See EmulatorWarmResetTest.
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(systemDirectory.string());
    ASSERT_TRUE(emulator->SelectModelPreset(adm::AmigaModel::A1200));
    ASSERT_TRUE(emulator->LoadCore(corePath.string()));
    ASSERT_TRUE(emulator->StartEmpty());

    ASSERT_TRUE(emulator->SelectModelPreset(adm::AmigaModel::A500));
    ASSERT_FALSE(emulator->IsGameLoaded());
    ASSERT_TRUE(emulator->StartEmpty());

    const auto regions = emulator->GetMemoryRegions();
    const auto chip = std::find_if(regions.begin(), regions.end(),
                                   [](const auto& region) { return region.name == "Chip RAM"; });
    const auto slow = std::find_if(regions.begin(), regions.end(), [](const auto& region) {
        return region.name.starts_with("Slow RAM");
    });
    const auto kickstart = std::find_if(regions.begin(), regions.end(), [](const auto& region) {
        return region.name == "Kickstart ROM";
    });
    ASSERT_NE(chip, regions.end());
    ASSERT_NE(slow, regions.end());
    ASSERT_NE(kickstart, regions.end());
    EXPECT_EQ(chip->size, 512u * 1024u);
    EXPECT_EQ(slow->size, 512u * 1024u);
    ASSERT_NE(kickstart->hostPtr, nullptr);
    const uint16_t kickstartVersion =
        (static_cast<uint16_t>(kickstart->hostPtr[12]) << 8) | kickstart->hostPtr[13];
    EXPECT_EQ(kickstartVersion, 34u);
}

// The bug this guards: SelectModelPreset wrote puae_z3mem_size "0", and the core answers any
// non-auto z3mem_size with cpu_24bit_addressing=false. Every model therefore booted with a
// 32-bit address space, including the 68EC020 machines, whose CPU only has 24 address lines.
// Software that leaves garbage in an address's top byte then works here and not on the hardware.
TEST(AmigaModelConfiguration, PresetsBootWithTheAddressSpaceTheirCpuActuallyHas)
{
#ifdef _WIN32
    constexpr const char* coreName = "puae_libretro.dll";
#else
    constexpr const char* coreName = "puae_libretro.dylib";
#endif
    const auto systemDirectory = std::filesystem::absolute("emulation");
    const auto corePath = systemDirectory / coreName;
    if (!std::filesystem::is_regular_file(corePath))
        GTEST_SKIP() << "Bundled PuAE core is not present at " << corePath;

    // One model per address width and per CPU family that decides it. A4030 is the case that
    // must stay 32-bit, so this cannot pass by answering 24 to everything.
    struct Expectation
    {
        adm::AmigaModel model;
        const char* kickstart; // skipped when the ROM is not in emulation/
        int addressBits;
    };
    const Expectation expectations[] = {
        {adm::AmigaModel::A500, "kick34005.A500", 24},   // 68000
        {adm::AmigaModel::A1200, "kick40068.A1200", 24}, // 68EC020 - the bug
        {adm::AmigaModel::A4030, "kick40068.A4000", 32}, // 68030
    };

    for (const auto& expected : expectations) {
        if (!std::filesystem::is_regular_file(systemDirectory / expected.kickstart)) {
            GTEST_SKIP() << expected.kickstart << " is not present in " << systemDirectory;
            continue;
        }
        // One emulator per model: two live instances in one process share the core's static
        // callback pointer, so the previous one has to be released first.
        auto emulator = std::make_unique<adm::AmigaEmulator>();
        // Muted: StartEmpty() opens a real host playback device. See EmulatorWarmResetTest.
        emulator->SetMuted(true);
        emulator->SetSystemDirectory(systemDirectory.string());
        ASSERT_TRUE(emulator->SelectModelPreset(expected.model));
        ASSERT_TRUE(emulator->LoadCore(corePath.string()));
        ASSERT_TRUE(emulator->StartEmpty());
        if (!emulator->HasMachineConfig())
            GTEST_SKIP() << "the bundled core does not export adm_get_machine_config";

        const auto config = emulator->GetMachineConfig();
        ASSERT_TRUE(config.has_value())
            << adm::GetAmigaModelInfo(expected.model).id << ": no machine config";
        EXPECT_EQ(adm::core::AddressBits(*config), expected.addressBits)
            << adm::GetAmigaModelInfo(expected.model).id << " booted with a "
            << adm::core::AddressBits(*config) << "-bit address space (CPU " << config->cpuModel
            << ")";
    }
}

} // namespace
