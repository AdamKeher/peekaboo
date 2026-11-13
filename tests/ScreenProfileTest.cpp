#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/ScreenProfile.h"
#include "core/UserPreferences.h"
#include "core/WorkspaceManager.h"
#include "ui/TableLayout.h"

using adm::core::ScreenProfile;
using adm::core::UserPreferences;
using adm::core::WorkspaceManager;
using adm::ui::TableLayoutStore;
using nlohmann::json;

namespace {

// Trimmed from a real 4K workspace: a main dock space under an 80 px menu bar, a panel's own
// dock space inside it, a docked window and a floating one, and a [Table] section.
constexpr const char* kHiresIni = "[Window][WindowOverViewport_11111111]\n"
                                  "Pos=0,80\n"
                                  "Size=3840,1974\n"
                                  "Collapsed=0\n"
                                  "\n"
                                  "[Window][Floating]\n"
                                  "Pos=1920,1000\n"
                                  "Size=800,400\n"
                                  "Collapsed=0\n"
                                  "\n"
                                  "[Table][0x6A9A5C90,6]\n"
                                  "Column 0  Width=137\n"
                                  "\n"
                                  "[Docking][Data]\n"
                                  "DockSpace   ID=0x0043E547 Window=0xE9355DE7 Pos=1183,370 "
                                  "Size=2645,1672 Split=X Selected=0x81D3EE07\n"
                                  "  DockNode  ID=0x0000000F Parent=0x0043E547 SizeRef=702,1918 "
                                  "Selected=0xE661AECB\n"
                                  "DockSpace   ID=0x08BD597D Window=0x1BBC0F80 Pos=0,80 "
                                  "Size=3840,1974 Split=X\n"
                                  "  DockNode  ID=0x0000000B Parent=0x08BD597D SizeRef=602,1974 "
                                  "Selected=0xC1EAE4EA\n";

// UserPreferences and WorkspaceManager read and write their files in the working directory, so
// each test runs in a scratch directory of its own.
class ScreenProfileFiles : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("peekaboo_screen_profile_test_" + std::to_string(nonce));
        std::filesystem::create_directories(root);
        original = std::filesystem::current_path();
        std::filesystem::current_path(root);
    }

    void TearDown() override
    {
        std::filesystem::current_path(original);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path root;
    std::filesystem::path original;
};

WorkspaceManager::Layout MakeLayout(const std::string& ini)
{
    WorkspaceManager::Layout layout;
    layout.imguiLayout = ini;
    layout.windowStates["Memory"] = true;
    layout.blockBytesPerLine = 32;
    layout.panels.push_back({"Debug", 4449607});
    return layout;
}

} // namespace

// --- the profile type ---------------------------------------------------------

TEST(ScreenProfile, KeysRoundTripAndUnknownKeysAreRejected)
{
    for (ScreenProfile p : adm::core::kAllScreenProfiles)
        EXPECT_EQ(adm::core::ParseScreenProfile(adm::core::ScreenProfileKey(p)), p);
    EXPECT_FALSE(adm::core::ParseScreenProfile("").has_value());
    EXPECT_FALSE(adm::core::ParseScreenProfile("Hires").has_value());
}

TEST(ScreenProfile, ClassifiesByEffectiveWidth)
{
    EXPECT_EQ(adm::core::ClassifyScreen(3840.0f), ScreenProfile::Hires); // 4K at 100%
    EXPECT_EQ(adm::core::ClassifyScreen(2560.0f), ScreenProfile::Hires); // 1440p at 100%
    EXPECT_EQ(adm::core::ClassifyScreen(1920.0f), ScreenProfile::Lores); // 1080p
    EXPECT_EQ(adm::core::ClassifyScreen(1470.0f), ScreenProfile::Lores); // MacBook Air, points
}

// --- scaling a layout ------------------------------------------------------------

TEST(ScreenProfile, FindsTheLargestDockSpaceAsTheMainOne)
{
    float x = 0, y = 0, w = 0, h = 0;
    ASSERT_TRUE(adm::core::FindMainDockSpace(kHiresIni, x, y, w, h));
    EXPECT_FLOAT_EQ(x, 0.0f);
    EXPECT_FLOAT_EQ(y, 80.0f);
    EXPECT_FLOAT_EQ(w, 3840.0f);
    EXPECT_FLOAT_EQ(h, 1974.0f);
    EXPECT_FALSE(adm::core::FindMainDockSpace("[Window][A]\nPos=0,0\n", x, y, w, h));
}

TEST(ScreenProfile, ScalesEveryPosSizeAndSizeRef)
{
    const std::string out = adm::core::ScaleImGuiIni(kHiresIni, 0.5f, 0.25f);
    EXPECT_NE(out.find("Pos=0,20\nSize=1920,494\n"), std::string::npos);
    EXPECT_NE(out.find("Pos=960,250\nSize=400,100\n"), std::string::npos);
    EXPECT_NE(out.find("Pos=592,93 Size=1323,418 Split=X"), std::string::npos);
    EXPECT_NE(out.find("SizeRef=351,480 "), std::string::npos);
    EXPECT_NE(out.find("SizeRef=301,494 "), std::string::npos);
}

TEST(ScreenProfile, ScalingLeavesEverythingElseAlone)
{
    const std::string out = adm::core::ScaleImGuiIni(kHiresIni, 0.5f, 0.25f);
    for (const char* kept :
         {"[Window][WindowOverViewport_11111111]\n", "Collapsed=0\n", "Column 0  Width=137\n",
          "ID=0x0043E547 Window=0xE9355DE7", "Parent=0x0043E547", "Selected=0xE661AECB\n"})
        EXPECT_NE(out.find(kept), std::string::npos) << kept;
    // Identity scaling is the identity.
    EXPECT_EQ(adm::core::ScaleImGuiIni(kHiresIni, 1.0f, 1.0f), kHiresIni);
}

TEST(ScreenProfile, ScalingNeverMakesASizeZero)
{
    const std::string out = adm::core::ScaleImGuiIni("Pos=3,3\nSize=3,3\n", 0.1f, 0.1f);
    EXPECT_EQ(out, "Pos=0,0\nSize=1,1\n");
}

// --- workspaces.json ---------------------------------------------------------------

TEST(ScreenProfileWorkspaces, AFileFromBeforeProfilesIsAllHires)
{
    const json legacy = json::parse(R"([{"name": "Debugger", "imguiLayout": "[Window][A]\n",
        "windowStates": {"Memory": true}, "blockBytesPerLine": 16,
        "panels": [{"name": "Debug", "id": 4449607}]}])");
    const auto workspaces = WorkspaceManager::FromJson(legacy);
    ASSERT_EQ(workspaces.size(), 1u);
    ASSERT_EQ(workspaces[0].layouts.size(), 1u);
    const auto& hires = workspaces[0].layouts.at(ScreenProfile::Hires);
    EXPECT_EQ(hires.imguiLayout, "[Window][A]\n");
    EXPECT_TRUE(hires.windowStates.at("Memory"));
    ASSERT_EQ(hires.panels.size(), 1u);
    EXPECT_EQ(hires.panels[0].id, 4449607u);
}

TEST(ScreenProfileWorkspaces, HiresStaysAtTheTopLevelAndLoresNestsBesideIt)
{
    WorkspaceManager::Workspace ws{"Debugger", {}};
    ws.layouts[ScreenProfile::Hires] = MakeLayout("hires ini");
    ws.layouts[ScreenProfile::Lores] = MakeLayout("lores ini");
    const json j = WorkspaceManager::ToJson({ws});

    // An older build reads exactly these top-level fields.
    EXPECT_EQ(j[0]["imguiLayout"], "hires ini");
    EXPECT_EQ(j[0]["lores"]["imguiLayout"], "lores ini");

    const auto back = WorkspaceManager::FromJson(j);
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].layouts.at(ScreenProfile::Hires).imguiLayout, "hires ini");
    EXPECT_EQ(back[0].layouts.at(ScreenProfile::Lores).imguiLayout, "lores ini");
    EXPECT_EQ(back[0].layouts.at(ScreenProfile::Lores).blockBytesPerLine, 32);
    EXPECT_EQ(back[0].layouts.at(ScreenProfile::Lores).panels.size(), 1u);
}

// A panel can share its name with a window (a panel "Exocet" beside the Exocet Grid, keyed
// "Exocet"): its visibility rides with the panel, so neither overwrites the other's.
TEST(ScreenProfileWorkspaces, APanelKeepsItsVisibilityApartFromAWindowOfItsName)
{
    WorkspaceManager::Workspace ws{"Exocet", {}};
    WorkspaceManager::Layout layout;
    layout.windowStates["Exocet"] = true;
    layout.panels.push_back({"Exocet", 599566493, 0});
    layout.panels.push_back({"Debug", 4449607});
    ws.layouts[ScreenProfile::Hires] = layout;

    const auto back = WorkspaceManager::FromJson(WorkspaceManager::ToJson({ws}));
    ASSERT_EQ(back.size(), 1u);
    const auto& hires = back[0].layouts.at(ScreenProfile::Hires);
    EXPECT_TRUE(hires.windowStates.at("Exocet"));
    ASSERT_EQ(hires.panels.size(), 2u);
    EXPECT_EQ(hires.panels[0].open, 0);
    EXPECT_EQ(hires.panels[1].open, -1); // not saved: windowStates under its name says
}

TEST(ScreenProfileWorkspaces, ALoresOnlyWorkspaceRoundTripsWithoutAHiresLayout)
{
    WorkspaceManager::Workspace ws{"Laptop", {}};
    ws.layouts[ScreenProfile::Lores] = MakeLayout("lores ini");
    const json j = WorkspaceManager::ToJson({ws});
    EXPECT_FALSE(j[0].contains("imguiLayout"));

    const auto back = WorkspaceManager::FromJson(j);
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].layouts.count(ScreenProfile::Hires), 0u);
    EXPECT_EQ(back[0].layouts.count(ScreenProfile::Lores), 1u);
}

TEST_F(ScreenProfileFiles, SavingOneProfileKeepsTheOther)
{
    {
        WorkspaceManager wm;
        wm.SaveLayout("Debugger", ScreenProfile::Hires, MakeLayout("hires ini"));
        wm.SaveLayout("Debugger", ScreenProfile::Lores, MakeLayout("lores ini"));
        wm.SaveLayout("Debugger", ScreenProfile::Lores, MakeLayout("lores ini 2"));
    }
    WorkspaceManager wm; // re-read from disk
    ASSERT_EQ(wm.GetWorkspaces().size(), 1u);
    EXPECT_EQ(wm.FindLayout("Debugger", ScreenProfile::Hires)->imguiLayout, "hires ini");
    EXPECT_EQ(wm.FindLayout("Debugger", ScreenProfile::Lores)->imguiLayout, "lores ini 2");
}

TEST_F(ScreenProfileFiles, RenameAndDeleteActOnBothProfiles)
{
    WorkspaceManager wm;
    wm.SaveLayout("Debugger", ScreenProfile::Hires, MakeLayout("hires ini"));
    wm.SaveLayout("Debugger", ScreenProfile::Lores, MakeLayout("lores ini"));
    wm.SaveLayout("Disk", ScreenProfile::Hires, MakeLayout("disk ini"));

    wm.RenameWorkspace("Debugger", "Debug");
    EXPECT_TRUE(wm.HasLayout("Debug", ScreenProfile::Hires));
    EXPECT_TRUE(wm.HasLayout("Debug", ScreenProfile::Lores));
    EXPECT_FALSE(wm.HasLayout("Debugger", ScreenProfile::Lores));

    wm.DeleteWorkspace("Debug");
    EXPECT_EQ(wm.FindWorkspace("Debug"), nullptr);
    EXPECT_FALSE(wm.HasLayout("Debug", ScreenProfile::Hires));
    EXPECT_FALSE(wm.HasLayout("Disk", ScreenProfile::Lores));
    EXPECT_TRUE(wm.HasLayout("Disk", ScreenProfile::Hires));
}

// --- user_prefs.json ---------------------------------------------------------------

TEST_F(ScreenProfileFiles, TheFirstChoiceKeepsTheLiveSettings)
{
    std::ofstream("user_prefs.json") << R"({"windowWidth": 3000, "fonts": {"uiScale": 1.5}})";
    UserPreferences prefs;
    EXPECT_TRUE(prefs.Get().screenProfile.empty());
    EXPECT_FALSE(prefs.SwitchScreenProfile("hires"));
    EXPECT_EQ(prefs.Get().screenProfile, "hires");
    EXPECT_EQ(prefs.Get().windowWidth, 3000);
    EXPECT_FLOAT_EQ(prefs.Get().fonts.uiScale, 1.5f);
    EXPECT_TRUE(prefs.Get().screenProfiles.empty());
}

TEST_F(ScreenProfileFiles, SwitchingSwapsTheScreenSettingsAndOnlyThose)
{
    std::ofstream("user_prefs.json") << "{}";
    {
        UserPreferences prefs;
        prefs.SwitchScreenProfile("hires");
        auto& p = prefs.GetMutable();
        p.windowWidth = 3000;
        p.fonts.uiScale = 1.0f;
        p.memoryBytesPerLine = 32;
        p.transitionDuration = 1.5f; // not a screen setting

        // A profile never used starts from the live values.
        EXPECT_FALSE(prefs.SwitchScreenProfile("lores"));
        EXPECT_EQ(prefs.Get().windowWidth, 3000);
        prefs.GetMutable().windowWidth = 1400;
        prefs.GetMutable().fonts.uiScale = 1.25f;
        prefs.GetMutable().memoryBytesPerLine = 16;
        prefs.SetFontPrefs(prefs.Get().fonts); // persists every live field
    }
    UserPreferences prefs; // re-read: the stashed profile survives the file
    EXPECT_EQ(prefs.Get().screenProfile, "lores");
    EXPECT_EQ(prefs.Get().windowWidth, 1400);

    EXPECT_TRUE(prefs.SwitchScreenProfile("hires"));
    EXPECT_EQ(prefs.Get().windowWidth, 3000);
    EXPECT_FLOAT_EQ(prefs.Get().fonts.uiScale, 1.0f);
    EXPECT_EQ(prefs.Get().memoryBytesPerLine, 32);
    EXPECT_FLOAT_EQ(prefs.Get().transitionDuration, 1.5f);

    EXPECT_TRUE(prefs.SwitchScreenProfile("lores"));
    EXPECT_EQ(prefs.Get().windowWidth, 1400);
    EXPECT_FLOAT_EQ(prefs.Get().fonts.uiScale, 1.25f);
    EXPECT_FLOAT_EQ(prefs.Get().transitionDuration, 1.5f);
}

TEST_F(ScreenProfileFiles, AShippedProfileAppliesOnTheFirstChoice)
{
    // The package stores every profile and chooses none.
    std::ofstream("user_prefs.json") << R"({"windowWidth": 1600, "screenProfile": "",
               "screenProfiles": {"lores": {"fonts": {"uiScale": 0.9}, "windowWidth": 1200}}})";
    UserPreferences prefs;
    EXPECT_TRUE(prefs.SwitchScreenProfile("lores"));
    EXPECT_FLOAT_EQ(prefs.Get().fonts.uiScale, 0.9f);
    EXPECT_EQ(prefs.Get().windowWidth, 1200);
    EXPECT_TRUE(prefs.Get().screenProfiles.empty());
}

// APP-04: one wrong-typed preference is ignored, not a reset of every one
TEST_F(ScreenProfileFiles, OneBadPreferenceKeepsTheOthers)
{
    std::ofstream("user_prefs.json") << R"({"windowWidth": 3000, "showExocet": "yes",
        "mcpToken": "secret", "recentFiles": ["a.adf"], "frameHistoryBudgetMB": "lots",
        "displayFilters": {"gamma": "bright", "scanlines": true}})";
    UserPreferences prefs;
    EXPECT_EQ(prefs.Get().windowWidth, 3000);
    EXPECT_EQ(prefs.Get().mcpToken, "secret");
    ASSERT_EQ(prefs.Get().recentFiles.size(), 1u);
    EXPECT_FALSE(prefs.Get().showExocet);
    EXPECT_TRUE(prefs.Get().displayFilters.scanlines);
}

TEST_F(ScreenProfileFiles, AnUnreadablePreferencesFileIsKeptAsBad)
{
    std::ofstream("user_prefs.json") << "{ not json";
    UserPreferences prefs;
    EXPECT_TRUE(std::filesystem::exists("user_prefs.json.bad"));
}

TEST_F(ScreenProfileFiles, BridgeSlotSettingsAreClamped)
{
    std::ofstream("user_prefs.json")
        << R"({"fbSlots": [{"bridgeMode": 40, "bridgeDensity": 7, "driveCable": -1}]})";
    UserPreferences prefs;
    EXPECT_EQ(prefs.Get().fbSlots[0].bridgeMode, 3);
    EXPECT_EQ(prefs.Get().fbSlots[0].bridgeDensity, 2);
    EXPECT_EQ(prefs.Get().fbSlots[0].driveCable, 0);
}

// --- table_layout.ini ---------------------------------------------------------------

TEST_F(ScreenProfileFiles, EachProfileKeepsItsOwnColumnFile)
{
    EXPECT_EQ(TableLayoutStore::FileFor(ScreenProfile::Hires), "table_layout.ini");
    EXPECT_EQ(TableLayoutStore::FileFor(ScreenProfile::Lores), "table_layout_lores.ini");

    std::ofstream("table_layout.ini", std::ios::binary) << "[Table][0x1,1]\nColumn 0  Width=300\n";
    TableLayoutStore store;
    store.Load();
    ASSERT_NE(store.Ini().find("Width=300"), std::string::npos);

    // No Lores file yet: the columns carry over, and are written to it on the way back.
    EXPECT_FALSE(store.SwitchFile(TableLayoutStore::FileFor(ScreenProfile::Lores)));
    EXPECT_NE(store.Ini().find("Width=300"), std::string::npos);
    EXPECT_TRUE(store.SwitchFile(TableLayoutStore::FileFor(ScreenProfile::Hires)));
    EXPECT_TRUE(std::filesystem::exists("table_layout_lores.ini"));

    std::ofstream("table_layout_lores.ini", std::ios::binary)
        << "[Table][0x1,1]\nColumn 0  Width=120\n";
    EXPECT_TRUE(store.SwitchFile(TableLayoutStore::FileFor(ScreenProfile::Lores)));
    EXPECT_NE(store.Ini().find("Width=120"), std::string::npos);
    EXPECT_EQ(store.Ini().find("Width=300"), std::string::npos);
}
