#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "core/DefinitionManager.h"

namespace adm::core {

class DefinitionManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create a temporary definition file
        std::ofstream f("test_def.json");
        f << R"({"test": "value", "number": 42})";
        f.close();
    }

    void TearDown() override
    {
        if (std::filesystem::exists("test_def.json")) {
            std::filesystem::remove("test_def.json");
        }
    }

    DefinitionManager manager;
};

TEST_F(DefinitionManagerTest, GetDefinitionReturnsEmptyForEmptyPath)
{
    auto json = manager.GetDefinition("");
    EXPECT_TRUE(json.empty());
}

TEST_F(DefinitionManagerTest, GetDefinitionLoadsValidJson)
{
    auto json = manager.GetDefinition("test_def.json");
    EXPECT_FALSE(json.empty());
    EXPECT_EQ(json["test"], "value");
    EXPECT_EQ(json["number"], 42);
}

TEST_F(DefinitionManagerTest, GetDefinitionReturnsCachedVersion)
{
    auto json1 = manager.GetDefinition("test_def.json");

    // Modify file on disk (should not affect cached version)
    std::ofstream f("test_def.json");
    f << R"({"test": "changed"})";
    f.close();

    auto json2 = manager.GetDefinition("test_def.json");

    EXPECT_EQ(json2["test"], "value"); // Should still be "value" from cache
}

TEST_F(DefinitionManagerTest, GetDefinitionReturnsEmptyForInvalidFile)
{
    auto json = manager.GetDefinition("non_existent.json");
    EXPECT_TRUE(json.empty());
}

} // namespace adm::core
