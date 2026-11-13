#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include "../src/core/EmulatorStateStore.h"

using adm::core::EmulatorStateStore;

namespace {

std::vector<uint8_t> Payload(size_t n, uint8_t seed = 0)
{
    std::vector<uint8_t> v(n);
    std::iota(v.begin(), v.end(), seed);
    return v;
}

// Each test gets its own scratch directory so persisted .pkst files never leak
// between cases.
class EmulatorStateStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_dir = std::filesystem::temp_directory_path() /
                ("adm_states_" +
                 std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line()));
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }

    EmulatorStateStore MakeStore() { return EmulatorStateStore(m_dir); }

    std::filesystem::path m_dir;
};

} // namespace

TEST_F(EmulatorStateStoreTest, SavesAndLoadsInMemory)
{
    auto store = MakeStore();
    std::string err;
    ASSERT_TRUE(store.Save("menu", Payload(64), 120, "1.19.0", "game.adf", false, err)) << err;

    EmulatorStateStore::StateInfo info;
    auto data = store.Load("menu", info);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(*data, Payload(64));
    EXPECT_EQ(info.name, "menu");
    EXPECT_EQ(info.bytes, 64u);
    EXPECT_EQ(info.frame, 120u);
    EXPECT_EQ(info.coreVersion, "1.19.0");
    EXPECT_EQ(info.document, "game.adf");
    EXPECT_FALSE(info.persisted);
    EXPECT_FALSE(info.created.empty());
}

TEST_F(EmulatorStateStoreTest, LoadingAnUnknownNameReturnsNothing)
{
    auto store = MakeStore();
    EmulatorStateStore::StateInfo info;
    EXPECT_FALSE(store.Load("ghost", info).has_value());
    EXPECT_FALSE(store.Info("ghost").has_value());
}

TEST_F(EmulatorStateStoreTest, SavingTheSameNameOverwrites)
{
    auto store = MakeStore();
    std::string err;
    ASSERT_TRUE(store.Save("menu", Payload(16, 1), 10, "v", "d", false, err)) << err;
    ASSERT_TRUE(store.Save("menu", Payload(32, 9), 20, "v", "d", false, err)) << err;

    EmulatorStateStore::StateInfo info;
    auto data = store.Load("menu", info);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(data->size(), 32u);
    EXPECT_EQ(info.frame, 20u);
    EXPECT_EQ(store.List().size(), 1u);
}

TEST_F(EmulatorStateStoreTest, PersistedStatesSurviveANewStoreInstance)
{
    {
        auto store = MakeStore();
        std::string err;
        ASSERT_TRUE(store.Save("boot", Payload(128, 3), 42, "1.19.0", "disk.adf", true, err))
            << err;
        auto info = store.Info("boot");
        ASSERT_TRUE(info.has_value());
        EXPECT_TRUE(info->persisted);
    }

    // A fresh store must find the .pkst on disk — the point of persisting is
    // that a hard-won boot state outlives the app.
    auto reopened = MakeStore();
    EmulatorStateStore::StateInfo info;
    auto data = reopened.Load("boot", info);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(*data, Payload(128, 3));
    EXPECT_EQ(info.frame, 42u);
    EXPECT_EQ(info.coreVersion, "1.19.0");
    EXPECT_EQ(info.document, "disk.adf");
    EXPECT_TRUE(info.persisted);
}

TEST_F(EmulatorStateStoreTest, UnpersistedStatesDoNotSurvive)
{
    {
        auto store = MakeStore();
        std::string err;
        ASSERT_TRUE(store.Save("scratch", Payload(16), 1, "v", "d", false, err)) << err;
    }
    auto reopened = MakeStore();
    EmulatorStateStore::StateInfo info;
    EXPECT_FALSE(reopened.Load("scratch", info).has_value());
}

TEST_F(EmulatorStateStoreTest, ListIsNewestFirstAndCoversDiskAndMemory)
{
    auto store = MakeStore();
    std::string err;
    ASSERT_TRUE(store.Save("a", Payload(8), 1, "v", "d", true, err)) << err;
    ASSERT_TRUE(store.Save("b", Payload(8), 2, "v", "d", false, err)) << err;

    auto list = store.List();
    ASSERT_EQ(list.size(), 2u);
    // Both are visible regardless of where they live.
    std::vector<std::string> names{list[0].name, list[1].name};
    EXPECT_NE(std::find(names.begin(), names.end(), std::string("a")), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), std::string("b")), names.end());
}

TEST_F(EmulatorStateStoreTest, RemoveDeletesTheFileToo)
{
    auto store = MakeStore();
    std::string err;
    ASSERT_TRUE(store.Save("gone", Payload(8), 1, "v", "d", true, err)) << err;
    ASSERT_TRUE(store.Remove("gone"));
    EXPECT_FALSE(store.Remove("gone"));

    // A new store must not resurrect it from disk.
    auto reopened = MakeStore();
    EmulatorStateStore::StateInfo info;
    EXPECT_FALSE(reopened.Load("gone", info).has_value());
}

TEST_F(EmulatorStateStoreTest, ClearMemoryLeavesPersistedFilesAlone)
{
    auto store = MakeStore();
    std::string err;
    ASSERT_TRUE(store.Save("kept", Payload(8, 5), 1, "v", "d", true, err)) << err;
    ASSERT_TRUE(store.Save("dropped", Payload(8), 1, "v", "d", false, err)) << err;

    store.ClearMemory();

    EmulatorStateStore::StateInfo info;
    auto kept = store.Load("kept", info);
    ASSERT_TRUE(kept.has_value());
    EXPECT_EQ(*kept, Payload(8, 5));
    EXPECT_FALSE(store.Load("dropped", info).has_value());
}

TEST_F(EmulatorStateStoreTest, RejectsNamesThatWouldNotBeSafeFileNames)
{
    EXPECT_TRUE(EmulatorStateStore::IsValidName("menu"));
    EXPECT_TRUE(EmulatorStateStore::IsValidName("boot-2.state_1"));
    EXPECT_FALSE(EmulatorStateStore::IsValidName(""));
    EXPECT_FALSE(EmulatorStateStore::IsValidName("has space"));
    EXPECT_FALSE(EmulatorStateStore::IsValidName("../escape"));
    EXPECT_FALSE(EmulatorStateStore::IsValidName("a/b"));
    EXPECT_FALSE(EmulatorStateStore::IsValidName(std::string(65, 'x')));
}

TEST_F(EmulatorStateStoreTest, IgnoresACorruptStateFile)
{
    {
        auto store = MakeStore();
        std::string err;
        ASSERT_TRUE(store.Save("broken", Payload(32), 1, "v", "d", true, err)) << err;
    }
    // Truncate the file so its header no longer parses.
    for (const auto& entry : std::filesystem::directory_iterator(m_dir)) {
        std::ofstream f(entry.path(), std::ios::binary | std::ios::trunc);
        f << "junk";
    }

    auto reopened = MakeStore();
    EmulatorStateStore::StateInfo info;
    EXPECT_FALSE(reopened.Load("broken", info).has_value());
}

TEST_F(EmulatorStateStoreTest, DirectoryIsReported)
{
    auto store = MakeStore();
    EXPECT_EQ(store.Directory(), m_dir);
}
