#include "core/UserPreferences.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

using adm::core::UserPreferences;

namespace {

// UserPreferences reads and writes user_prefs.json in the working directory, so each test runs
// in a scratch directory of its own.
class FontPrefsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("peekaboo_font_prefs_test_" + std::to_string(nonce));
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

} // namespace

TEST_F(FontPrefsTest, DefaultsWhenTheFileHasNoFonts)
{
    std::ofstream("user_prefs.json") << "{}";
    UserPreferences prefs;
    const auto& f = prefs.Get().fonts;
    EXPECT_FLOAT_EQ(f.uiScale, 1.0f);
    for (const auto& role : f.roles) {
        EXPECT_TRUE(role.face.empty());
        EXPECT_FLOAT_EQ(role.scale, 1.0f);
    }
}

TEST_F(FontPrefsTest, RoundTripsThroughTheFile)
{
    std::ofstream("user_prefs.json") << "{}";
    {
        UserPreferences prefs;
        auto f = prefs.Get().fonts;
        f.uiScale = 1.25f;
        f.roles[0].face = "Topaz_a1200_v1.0";
        f.roles[1].scale = 0.85f;
        f.roles[2].face = "MicroKnight_v1.0";
        f.roles[3].scale = 1.5f;
        prefs.SetFontPrefs(f);
    }
    UserPreferences reloaded;
    const auto& f = reloaded.Get().fonts;
    EXPECT_FLOAT_EQ(f.uiScale, 1.25f);
    EXPECT_EQ(f.roles[0].face, "Topaz_a1200_v1.0");
    EXPECT_FLOAT_EQ(f.roles[1].scale, 0.85f);
    EXPECT_EQ(f.roles[2].face, "MicroKnight_v1.0");
    EXPECT_FLOAT_EQ(f.roles[3].scale, 1.5f);
}

TEST_F(FontPrefsTest, UnsavedChangeIsNotWritten)
{
    std::ofstream("user_prefs.json") << "{}";
    {
        UserPreferences prefs;
        auto f = prefs.Get().fonts;
        f.roles[1].scale = 1.4f;
        prefs.SetFontPrefs(f, false); // a slider mid-drag
        EXPECT_FLOAT_EQ(prefs.Get().fonts.roles[1].scale, 1.4f);
    }
    UserPreferences reloaded;
    EXPECT_FLOAT_EQ(reloaded.Get().fonts.roles[1].scale, 1.0f);
}

TEST_F(FontPrefsTest, ClampsOutOfRangeAndIgnoresMalformedValues)
{
    std::ofstream("user_prefs.json") << R"({"fonts": {"uiScale": 9.0, "roles": [
        {"face": "X", "scale": 3.0},
        {"face": 42, "scale": 0.1},
        "not an object",
        {"scale": "big"}
    ]}})";
    UserPreferences prefs;
    const auto& f = prefs.Get().fonts;
    EXPECT_FLOAT_EQ(f.uiScale, UserPreferences::kMaxUiScale);
    EXPECT_EQ(f.roles[0].face, "X");
    EXPECT_FLOAT_EQ(f.roles[0].scale, 1.0f); // the Interface row has no scale of its own
    EXPECT_TRUE(f.roles[1].face.empty());
    EXPECT_FLOAT_EQ(f.roles[1].scale, UserPreferences::kMinRoleScale);
    EXPECT_FLOAT_EQ(f.roles[2].scale, 1.0f);
    EXPECT_FLOAT_EQ(f.roles[3].scale, 1.0f);
}

TEST(FontPrefsStepTest, WalksTheZoomLadder)
{
    EXPECT_FLOAT_EQ(UserPreferences::StepUiScale(1.0f, +1), 1.1f);
    EXPECT_FLOAT_EQ(UserPreferences::StepUiScale(1.0f, -1), 0.9f);
    EXPECT_FLOAT_EQ(UserPreferences::StepUiScale(1.5f, 0), 1.0f);
    // Between stops: to the neighbour, never past it.
    EXPECT_FLOAT_EQ(UserPreferences::StepUiScale(1.3f, +1), 1.5f);
    EXPECT_FLOAT_EQ(UserPreferences::StepUiScale(1.3f, -1), 1.25f);
    // Ends of the ladder hold.
    EXPECT_FLOAT_EQ(UserPreferences::StepUiScale(2.0f, +1), UserPreferences::kMaxUiScale);
    EXPECT_FLOAT_EQ(UserPreferences::StepUiScale(0.75f, -1), UserPreferences::kMinUiScale);
}
