#include "core/AmigaEmulator.h"
#include "core/UserPreferences.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

namespace {

// PuAE's puae_floppy_sound is an attenuation: "0" is full volume, "100" is silence. Reading it as
// a switch - "100" for on - is what silenced drive sound, so these pin the sense down. Measured
// against the boot scenario's goldens: at the core default the drive is audible from frame 164,
// and "100" produces the same audio digest as having no samples loaded at all.
std::string Var(const adm::AmigaEmulator& emulator, const std::string& key)
{
    const auto& vars = emulator.GetUserVariables();
    auto it = vars.find(key);
    return it == vars.end() ? std::string() : it->second;
}

// Heap allocated on purpose: AmigaEmulator is several megabytes and a stack local overflows.
std::unique_ptr<adm::AmigaEmulator> MakeEmulator()
{
    return std::make_unique<adm::AmigaEmulator>();
}

TEST(FloppySoundTest, EnablingAsksForVolumeNotSilence)
{
    auto emulator = MakeEmulator();
    emulator->SetFloppySound(true);

    const std::string attenuation = Var(*emulator, "puae_floppy_sound");
    ASSERT_FALSE(attenuation.empty());
    EXPECT_NE(attenuation, "100") << "100 is the core's mute value, not its enable value";
    EXPECT_EQ(attenuation, "80") << "the core's own default, 20% loudness";
    EXPECT_TRUE(emulator->GetFloppySound());
    EXPECT_EQ(emulator->GetFloppySoundVolume(), 20);
}

TEST(FloppySoundTest, DisablingAsksForSilence)
{
    auto emulator = MakeEmulator();
    emulator->SetFloppySound(false);

    EXPECT_EQ(Var(*emulator, "puae_floppy_sound"), "100");
    EXPECT_FALSE(emulator->GetFloppySound());
    EXPECT_EQ(emulator->GetFloppySoundVolume(), 0);
}

TEST(FloppySoundTest, SwitchingBackOnRestoresTheChosenVolume)
{
    auto emulator = MakeEmulator();
    emulator->SetFloppySoundVolume(60);
    EXPECT_EQ(Var(*emulator, "puae_floppy_sound"), "40");

    emulator->SetFloppySound(false);
    EXPECT_EQ(Var(*emulator, "puae_floppy_sound"), "100");

    emulator->SetFloppySound(true);
    EXPECT_EQ(Var(*emulator, "puae_floppy_sound"), "40") << "off is a state, not a new preference";
    EXPECT_EQ(emulator->GetFloppySoundVolume(), 60);
}

// The toolbar toggle has to outrank the preferences page, which pushes its whole tab into the user
// variable map - and that map wins in EnvironmentCallback.
TEST(FloppySoundTest, TheToggleWritesWhereTheCoreActuallyLooks)
{
    auto emulator = MakeEmulator();
    emulator->SetUserVariables({{"puae_floppy_sound", "100"}});
    EXPECT_FALSE(emulator->GetFloppySound());

    emulator->SetFloppySound(true);
    EXPECT_EQ(Var(*emulator, "puae_floppy_sound"), "80");
    EXPECT_TRUE(emulator->GetFloppySound());
}

// Anything but "internal" sends PuAE looking for drive_click_<name>.wav under <system>/uae_data,
// and a miss clears dfxclick outright rather than falling back to the built-in samples.
TEST(FloppySoundTest, DefaultsToTheSamplesCompiledIntoTheCore)
{
    auto emulator = MakeEmulator();
    emulator->SetFloppySound(true);
    EXPECT_EQ(Var(*emulator, "puae_floppy_sound_type"), "internal");
}

TEST(FloppySoundTest, ADeliberateExternalSampleSetIsLeftAlone)
{
    auto emulator = MakeEmulator();
    emulator->SetUserVariables({{"puae_floppy_sound_type", "LOUD"}});
    emulator->SetFloppySound(true);
    EXPECT_EQ(Var(*emulator, "puae_floppy_sound_type"), "LOUD");
}

TEST(FloppySoundTest, LegacySettingsAreTranslatedOnTheWayIn)
{
    // What the old preferences page wrote when both boxes were ticked: silence, an external sample
    // set with no files behind it, and a key the core has never read.
    std::map<std::string, std::string> settings{{"puae_floppy_sound", "100"},
                                                {"puae_floppy_sound_empty_drive", "false"},
                                                {"puae_floppy_sound_type", "A500"}};
    adm::core::UserPreferences::MigrateLegacyFloppySound(settings);

    EXPECT_EQ(settings["puae_floppy_sound"], "80");
    EXPECT_EQ(settings["puae_floppy_sound_type"], "internal");
    EXPECT_EQ(settings["puae_floppy_sound_empty_mute"], "enabled");
    EXPECT_EQ(settings.count("puae_floppy_sound_empty_drive"), 0u);
}

TEST(FloppySoundTest, LegacyOffStaysOff)
{
    std::map<std::string, std::string> settings{{"puae_floppy_sound", "0"},
                                                {"puae_floppy_sound_empty_drive", "true"},
                                                {"puae_floppy_sound_type", "A1200"}};
    adm::core::UserPreferences::MigrateLegacyFloppySound(settings);

    EXPECT_EQ(settings["puae_floppy_sound"], "100");
    EXPECT_EQ(settings["puae_floppy_sound_empty_mute"], "disabled");
    EXPECT_EQ(settings["puae_floppy_sound_type"], "internal");
}

// Only a block carrying the dead key is legacy; a current one must survive untouched.
TEST(FloppySoundTest, CurrentSettingsAreNotRewritten)
{
    std::map<std::string, std::string> settings{{"puae_floppy_sound", "40"},
                                                {"puae_floppy_sound_type", "LOUD"}};
    const auto before = settings;
    adm::core::UserPreferences::MigrateLegacyFloppySound(settings);
    EXPECT_EQ(settings, before);
}

} // namespace
