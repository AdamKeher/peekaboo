#include "core/AmigaEmulator.h"
#include "core/EmulatorOptions.h"
#include "core/UserPreferences.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using adm::core::EmulatorOptionInfo;
using adm::core::GetEmulatorOptions;

// Heap allocated on purpose: AmigaEmulator is several megabytes and a stack local overflows.
std::unique_ptr<adm::AmigaEmulator> MakeEmulator()
{
    return std::make_unique<adm::AmigaEmulator>();
}

std::filesystem::path BundledCorePath()
{
#ifdef _WIN32
    constexpr const char* coreName = "puae_libretro.dll";
#else
    constexpr const char* coreName = "puae_libretro.dylib";
#endif
    return std::filesystem::absolute("emulation") / coreName;
}

// A core loaded only far enough to have declared its options. retro_set_environment() calls
// retro_set_core_options(), so LoadCore() alone fills in GetCoreOptionDeclarations() - no
// machine is started and no audio device is opened.
std::unique_ptr<adm::AmigaEmulator> LoadCoreForOptions()
{
    const auto corePath = BundledCorePath();
    if (!std::filesystem::is_regular_file(corePath))
        return nullptr;

    auto emulator = MakeEmulator();
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(corePath.parent_path().string());
    if (!emulator->LoadCore(corePath.string()))
        return nullptr;
    return emulator;
}

// ---------------------------------------------------------------------------
// The option table itself
// ---------------------------------------------------------------------------

TEST(EmulatorOptions, EveryEntryIsWellFormedAndNamedOnlyOnce)
{
    std::set<std::string> keys;
    for (const auto& option : GetEmulatorOptions()) {
        ASSERT_NE(option.key, nullptr);
        EXPECT_TRUE(keys.insert(option.key).second) << option.key << " is listed twice";
        EXPECT_TRUE(std::string(option.key).starts_with("puae_")) << option.key;
        EXPECT_TRUE(option.HasDefault())
            << option.key
            << " has no default, so the dialog and the core would disagree about "
               "what the user is looking at";

        if (option.values == nullptr) {
            EXPECT_EQ(option.valueCount, 0u) << option.key;
            continue;
        }
        EXPECT_GT(option.valueCount, 0u) << option.key;
        for (std::size_t i = 0; i < option.valueCount; ++i) {
            EXPECT_NE(option.values[i].value, nullptr) << option.key << " value " << i;
            EXPECT_NE(option.values[i].label, nullptr) << option.key << " value " << i;
        }
    }
}

TEST(EmulatorOptions, EveryDefaultIsOneOfTheValuesTheDialogOffers)
{
    for (const auto& option : GetEmulatorOptions()) {
        EXPECT_TRUE(adm::core::IsEmulatorOptionValue(option.key, option.defaultValue))
            << option.key << " defaults to " << option.defaultValue
            << ", which is not a row in its own combo box";
    }
}

TEST(EmulatorOptions, LookupsAnswerForKnownKeysOnly)
{
    EXPECT_NE(adm::core::FindEmulatorOption("puae_collision_level"), nullptr);
    EXPECT_EQ(adm::core::FindEmulatorOption("puae_not_an_option"), nullptr);

    EXPECT_EQ(adm::core::EmulatorOptionDefault("puae_collision_level"), "full");
    EXPECT_TRUE(adm::core::EmulatorOptionDefault("puae_not_an_option").empty());

    EXPECT_TRUE(adm::core::IsEmulatorOptionValue("puae_collision_level", "sprites"));
    EXPECT_FALSE(adm::core::IsEmulatorOptionValue("puae_collision_level", "everything"));
    EXPECT_FALSE(adm::core::IsEmulatorOptionValue("puae_not_an_option", "anything"));

    // An option with no fixed list takes any string; the core runs it through atof().
    EXPECT_TRUE(adm::core::IsEmulatorOptionValue("puae_cpu_throttle", "-250.0"));
}

// The four flags the CPU & Chipset tab used to offer as live checkboxes. The core derives all of
// them from puae_cpu_compatibility and declares no option for any one of them, so a table entry
// here would be a control that cannot do anything.
TEST(EmulatorOptions, DoesNotOfferTheFlagsTheCoreDerivesFromCompatibilityMode)
{
    for (const char* key : {"puae_cpu_cycle_exact", "puae_cpu_memory_cycle_exact",
                            "puae_cpu_compatible", "puae_blitter_cycle_exact"}) {
        EXPECT_EQ(adm::core::FindEmulatorOption(key), nullptr)
            << key << " is not a PuAE option - puae_cpu_compatibility sets it";
    }
}

// ---------------------------------------------------------------------------
// The table against the core that is actually shipped
// ---------------------------------------------------------------------------

TEST(EmulatorOptions, EveryManagedKeyIsAnOptionTheCoreDeclares)
{
    auto emulator = LoadCoreForOptions();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present at " << BundledCorePath();

    const auto& declared = emulator->GetCoreOptionDeclarations();
    ASSERT_FALSE(declared.empty()) << "the core declared no options at all";

    for (const auto& option : GetEmulatorOptions()) {
#ifndef _WIN32
        // The A2065 and uaenet.device are only compiled into Windows cores, where SLIRP's host
        // glue is; the host greys them out when the core does not declare them.
        if (std::string(option.key) == "puae_a2065" || std::string(option.key) == "puae_sana2")
            continue;
#endif
        EXPECT_NE(declared.find(option.key), declared.end())
            << option.key << " is not an option this core has; nothing would read it";
    }
}

TEST(EmulatorOptions, EveryValueTheDialogOffersIsOneTheCoreParses)
{
    auto emulator = LoadCoreForOptions();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present at " << BundledCorePath();

    const auto& declared = emulator->GetCoreOptionDeclarations();
    for (const auto& option : GetEmulatorOptions()) {
        auto it = declared.find(option.key);
        if (it == declared.end() || option.values == nullptr)
            continue;

        const auto& accepted = it->second.values;
        for (std::size_t i = 0; i < option.valueCount; ++i) {
            const std::string value = option.values[i].value;
            EXPECT_NE(std::find(accepted.begin(), accepted.end(), value), accepted.end())
                << option.key << " offers \"" << value
                << "\", which this core does not recognise - selecting it would change nothing";
        }
    }
}

TEST(EmulatorOptions, EveryDefaultIsAValueTheCoreParses)
{
    auto emulator = LoadCoreForOptions();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present at " << BundledCorePath();

    const auto& declared = emulator->GetCoreOptionDeclarations();
    for (const auto& option : GetEmulatorOptions()) {
        auto it = declared.find(option.key);
        if (it == declared.end() || it->second.values.empty())
            continue;
        const auto& accepted = it->second.values;
        EXPECT_NE(std::find(accepted.begin(), accepted.end(), option.defaultValue), accepted.end())
            << option.key << " is given the default \"" << option.defaultValue
            << "\", which this core does not recognise";
    }
}

// Where Peekaboo deliberately disagrees with PuAE's own default. Not a rule, a record: if one of
// these moves, it should move because someone decided to move it.
TEST(EmulatorOptions, DeliberateDeparturesFromTheCoreDefaultsAreTheOnlyOnes)
{
    auto emulator = LoadCoreForOptions();
    if (!emulator)
        GTEST_SKIP() << "Bundled PuAE core is not present at " << BundledCorePath();

    const std::map<std::string, std::string> kDeliberate = {
        // A debugger that draws DMA and blitter timing has to be cycle exact.
        {"puae_cpu_compatibility", "exact"},
        // The core waits for blits that software should have waited for itself.
        {"puae_immediate_blits", "false"},
        // Full collisions, because software that relies on them should behave.
        {"puae_collision_level", "full"},
        // A disk that says nothing boots the machine most Amiga software was written for.
        {"puae_video_standard", "PAL"},
        // Paula's channels are hard-panned on the hardware; 100% is unlistenable on headphones.
        {"puae_sound_stereo_separation", "70%"},
    };

    const auto& declared = emulator->GetCoreOptionDeclarations();
    for (const auto& option : GetEmulatorOptions()) {
        auto it = declared.find(option.key);
        if (it == declared.end())
            continue;

        const std::string ours = option.defaultValue;
        const std::string theirs = it->second.defaultValue;
        auto deliberate = kDeliberate.find(option.key);
        if (deliberate == kDeliberate.end()) {
            EXPECT_EQ(ours, theirs)
                << option.key
                << " quietly overrides the core's default; add it to the list above "
                   "with a reason, or drop the override";
        } else {
            EXPECT_EQ(ours, deliberate->second) << option.key;
            EXPECT_NE(ours, theirs) << option.key
                                    << " is listed as a deliberate departure but now matches the "
                                       "core; drop it from the list";
        }
    }
}

// ---------------------------------------------------------------------------
// What the core is told: the precedence the dialog relies on
// ---------------------------------------------------------------------------

TEST(EmulatorOptions, ADialogChoiceOutranksPeekaboosOwnDefault)
{
    auto emulator = MakeEmulator();
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_collision_level"), "full");

    // What Apply does: the whole page goes in as user variables.
    emulator->SetUserVariables({{"puae_collision_level", "sprites"},
                                {"puae_sound_filter", "off"},
                                {"puae_floppy_speed", "0"}});

    EXPECT_EQ(emulator->ResolveCoreVariable("puae_collision_level"), "sprites");
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_sound_filter"), "off");
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_floppy_speed"), "0");
    // Untouched keys still come back from the table, not from nowhere.
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_sound_interpol"), "anti");
}

TEST(EmulatorOptions, ResetToDefaultsPutsEveryOptionBackToTheTableValue)
{
    auto emulator = MakeEmulator();
    emulator->SetUserVariables({{"puae_collision_level", "none"},
                                {"puae_video_standard", "NTSC"},
                                {"puae_floppy_sound_type", "LOUD"}});
    emulator->ClearAllUserVariables(); // the dialog's "Reset to Defaults"

    for (const auto& option : GetEmulatorOptions()) {
        EXPECT_EQ(emulator->ResolveCoreVariable(option.key), option.defaultValue) << option.key;
    }
}

TEST(EmulatorOptions, EveryDialogOptionIsAnsweredWithoutALoadedCore)
{
    // The dialog can be opened, changed and applied before the machine is ever powered on, so
    // the table has to answer on its own rather than leaning on what a core declared.
    auto emulator = MakeEmulator();
    for (const auto& option : GetEmulatorOptions()) {
        EXPECT_FALSE(emulator->ResolveCoreVariable(option.key).empty())
            << option.key << " would make GET_VARIABLE answer false";
    }
    EXPECT_TRUE(emulator->ResolveCoreVariable("puae_not_an_option").empty());
}

TEST(EmulatorOptions, ModelDerivedValuesStillOutrankTheTable)
{
    auto emulator = MakeEmulator();
    emulator->SetModel(adm::AmigaModel::A1200);
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_model"), "A1200");

    // ...and a user override still outranks the model.
    emulator->SetUserVariable("puae_model", "CD32");
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_model"), "CD32");
}

// The Display window's resolution combo used to write core variables, which the Preferences
// dialog's user variables outranked: one Apply and the combo stopped doing anything.
TEST(EmulatorOptions, ResolutionChosenOutsideThePreferencesDialogReachesTheCore)
{
    auto emulator = MakeEmulator();
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_video_resolution"), "auto");

    emulator->SetVideoResolution(adm::AmigaEmulator::VideoResolution::SuperHires);
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_video_resolution"), "superhires");
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_video_vresolution"), "double");
    EXPECT_EQ(emulator->GetVideoResolution(), adm::AmigaEmulator::VideoResolution::SuperHires);

    // An unrelated preference applied afterwards must not undo it.
    auto vars = emulator->GetUserVariables();
    vars["puae_sound_filter"] = "off";
    emulator->SetUserVariables(vars);
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_video_resolution"), "superhires");
}

TEST(EmulatorOptions, ResolutionChosenInThePreferencesDialogIsReportedBack)
{
    auto emulator = MakeEmulator();
    emulator->SetVideoResolution(adm::AmigaEmulator::VideoResolution::Lores);

    // What Apply does with the Video tab's "Output Resolution" combo.
    auto vars = emulator->GetUserVariables();
    vars["puae_video_resolution"] = "hires";
    emulator->SetUserVariables(vars);

    EXPECT_EQ(emulator->GetVideoResolution(), adm::AmigaEmulator::VideoResolution::Hires)
        << "the Display window would still be showing Lores";
}

// ---------------------------------------------------------------------------
// Persisted settings
// ---------------------------------------------------------------------------

TEST(EmulatorOptions, SettingsRestoredFromASavedSessionReachTheCore)
{
    // What Application does on startup with prefs.emulatorSettings.
    const std::map<std::string, std::string> saved = {{"puae_cpu_compatibility", "compatible"},
                                                      {"puae_chipmem_size", "4"},
                                                      {"puae_floppy_write_redirect", "enabled"}};

    auto emulator = MakeEmulator();
    emulator->SetUserVariables(
        std::unordered_map<std::string, std::string>(saved.begin(), saved.end()));

    EXPECT_EQ(emulator->ResolveCoreVariable("puae_cpu_compatibility"), "compatible");
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_chipmem_size"), "4");
    EXPECT_EQ(emulator->ResolveCoreVariable("puae_floppy_write_redirect"), "enabled");
}

TEST(EmulatorOptions, SavedSettingsOnlyEverHoldValuesTheDialogCanShow)
{
    // Anything the dialog writes has to be selectable again when it is read back, or reopening
    // Preferences silently moves the setting to whatever row it lands on.
    for (const auto& option : GetEmulatorOptions()) {
        if (option.values == nullptr)
            continue;
        for (std::size_t i = 0; i < option.valueCount; ++i)
            EXPECT_TRUE(adm::core::IsEmulatorOptionValue(option.key, option.values[i].value));
    }
}

TEST(EmulatorOptions, TheLegacyFloppySoundMigrationLandsOnValidValues)
{
    std::map<std::string, std::string> settings = {{"puae_floppy_sound_empty_drive", "false"},
                                                   {"puae_floppy_sound", "100"}};
    adm::core::UserPreferences::MigrateLegacyFloppySound(settings);

    EXPECT_EQ(settings.count("puae_floppy_sound_empty_drive"), 0u);
    for (const auto& [key, value] : settings) {
        EXPECT_TRUE(adm::core::IsEmulatorOptionValue(key, value))
            << key << " migrated to \"" << value << "\", which the dialog cannot show";
    }
}

TEST(EmulatorOptions, SettingsFilesStopCarryingTheOptionsTheCoreDoesNotHave)
{
    std::map<std::string, std::string> settings = {
        {"puae_cpu_compatibility", "exact"},  {"puae_cpu_compatible", "true"},
        {"puae_cpu_cycle_exact", "true"},     {"puae_cpu_memory_cycle_exact", "true"},
        {"puae_blitter_cycle_exact", "true"}, {"puae_kickstart", "kick34005.A500"}};
    adm::core::UserPreferences::DropOptionsTheCoreDoesNotHave(settings);

    EXPECT_EQ(settings.size(), 2u);
    EXPECT_EQ(settings.at("puae_cpu_compatibility"), "exact")
        << "the one option the core does read must survive";
    EXPECT_EQ(settings.at("puae_kickstart"), "kick34005.A500")
        << "puae_kickstart is outside the option table but is still a real option";
}

// ---------------------------------------------------------------------------
// End to end: a preference chosen in the dialog changes the machine that boots
// ---------------------------------------------------------------------------

TEST(EmulatorOptions, RamChosenInTheDialogOverridesTheModelPresetOnTheRealCore)
{
    const auto corePath = BundledCorePath();
    if (!std::filesystem::is_regular_file(corePath))
        GTEST_SKIP() << "Bundled PuAE core is not present at " << corePath;

    auto emulator = MakeEmulator();
    // Muted: StartEmpty() opens a real host playback device. See EmulatorWarmResetTest.
    emulator->SetMuted(true);
    emulator->SetSystemDirectory(corePath.parent_path().string());
    ASSERT_TRUE(emulator->SelectModelPreset(adm::AmigaModel::A500));

    // The A500 preset is 512K chip + 512K slow + no fast RAM. Now do what the RAM tab does:
    // read the current variables, change two rows, push the whole page back.
    auto page = emulator->GetUserVariables();
    ASSERT_EQ(page["puae_chipmem_size"], "1");
    page["puae_chipmem_size"] = "2"; // 1 MB
    page["puae_fastmem_size"] = "2"; // 2 MB
    emulator->SetUserVariables(page);

    ASSERT_TRUE(emulator->LoadCore(corePath.string()));
    ASSERT_TRUE(emulator->StartEmpty());

    const auto regions = emulator->GetMemoryRegions();
    std::string listing;
    for (const auto& region : regions)
        listing += "\n  " + region.name + " " + std::to_string(region.size);

    const auto find = [&](const char* name) {
        return std::find_if(regions.begin(), regions.end(),
                            [&](const auto& region) { return region.name == name; });
    };
    const auto chip = find("Chip RAM");
    // "Fast RAM" or "Fast RAM (*)" - UAE names an autoconfigured Z2 bank "*" until the board
    // that claimed it identifies itself.
    const auto fast = std::find_if(regions.begin(), regions.end(), [](const auto& region) {
        return region.name.starts_with("Fast RAM");
    });
    ASSERT_NE(chip, regions.end()) << listing;
    EXPECT_EQ(chip->size, 1024u * 1024u)
        << "the Chip RAM row of the dialog did not reach the core:" << listing;
    ASSERT_NE(fast, regions.end())
        << "the Z2 Fast RAM row of the dialog did not reach the core:" << listing;
    EXPECT_EQ(fast->size, 2u * 1024u * 1024u) << listing;
}

} // namespace
