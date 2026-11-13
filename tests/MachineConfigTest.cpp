#include "core/AmigaEmulator.h"
#include "core/EmulatorOptions.h"
#include "core/MachineConfig.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

using namespace adm::core;

TEST(MachineConfig, ParsesTheExportInItsDocumentedOrder)
{
    const int values[kMachineConfigValueCount] = {3, 5, 2, 1, 2, 68030, 68882, 68030, 1, 0};
    const auto config = ParseMachineConfig(values, kMachineConfigValueCount);
    ASSERT_TRUE(config);
    EXPECT_EQ(config->chipsetMask, 3);
    EXPECT_EQ(config->compatible, 5);
    EXPECT_EQ(config->agnusModel, 2);
    EXPECT_EQ(config->agnusSize, 1);
    EXPECT_EQ(config->deniseModel, 2);
    EXPECT_EQ(config->cpuModel, 68030);
    EXPECT_EQ(config->fpuModel, 68882);
    EXPECT_EQ(config->mmuModel, 68030);
    EXPECT_TRUE(config->mmuEc);
    EXPECT_FALSE(config->addressSpace24);
}

TEST(MachineConfig, RefusesAShortReport)
{
    const int values[4] = {0, 5, 0, 0};
    EXPECT_FALSE(ParseMachineConfig(values, 4));
    EXPECT_FALSE(ParseMachineConfig(nullptr, kMachineConfigValueCount));
}

TEST(MachineConfig, NamesTheChipsetMask)
{
    EXPECT_EQ(ChipsetName(0), "OCS");
    EXPECT_EQ(ChipsetName(1), "ECS Agnus");
    EXPECT_EQ(ChipsetName(2), "ECS Denise");
    EXPECT_EQ(ChipsetName(3), "ECS");
    EXPECT_EQ(ChipsetName(7), "AGA");
}

TEST(MachineConfig, NamesFollowTheCoreTables)
{
    EXPECT_STREQ(ChipsetCompatibleName(5), "A500");
    EXPECT_STREQ(ChipsetCompatibleName(9), "A1200");
    EXPECT_STREQ(ChipsetCompatibleName(15), "Velvet");
    EXPECT_STREQ(ChipsetCompatibleName(99), "?");
    EXPECT_STREQ(AgnusModelName(2), "A1000");
    EXPECT_STREQ(AgnusSizeName(3), "2M");
    EXPECT_STREQ(DeniseModelName(2), "A1000 (no EHB)");
}

TEST(MachineConfig, NamesTheMmu)
{
    MachineConfig config;
    EXPECT_EQ(MmuName(config), "None");
    config.mmuModel = 68040;
    EXPECT_EQ(MmuName(config), "68040");
    config.mmuEc = true;
    EXPECT_EQ(MmuName(config), "68EC040");
}

TEST(MachineConfig, EverySettingIsAManagedOptionDefaultingToTheModel)
{
    for (const auto& setting : kMachineSettingKeys) {
        const auto* option = FindEmulatorOption(setting.key);
        ASSERT_NE(option, nullptr) << setting.key;
        EXPECT_STREQ(option->defaultValue, "auto") << setting.key;
        EXPECT_EQ(FindMachineSettingByKeyword(setting.keyword), &setting);
    }
    EXPECT_EQ(FindMachineSettingByKeyword("denise"), FindMachineSettingByKeyword("DENISE"));
    EXPECT_EQ(FindMachineSettingByKeyword("PAULA"), nullptr);
}

TEST(MachineConfig, SelectingAModelPutsEverySettingBackToTheModel)
{
    // Heap allocated: AmigaEmulator is several megabytes.
    auto emulator = std::make_unique<adm::AmigaEmulator>();
    emulator->SetUserVariable("puae_chipset", "ecs");
    emulator->SetUserVariable("puae_mmu_model", "none");
    ASSERT_TRUE(emulator->SelectModelPreset(adm::AmigaModel::A4040));
    for (const auto& setting : kMachineSettingKeys)
        EXPECT_EQ(emulator->ResolveCoreVariable(setting.key), "auto") << setting.key;
}

} // namespace
