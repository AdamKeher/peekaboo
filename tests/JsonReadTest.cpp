#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "../src/core/JsonRead.h"

namespace jr = adm::core::jsonread;
using nlohmann::json;

TEST(JsonReadTest, ReadsMatchingTypes)
{
    const json j = json::parse(R"({"i": 7, "u": 9, "neg": -3, "d": 1.5, "s": "x", "b": true})");
    EXPECT_EQ(jr::ReadOr(j, "i", 0), 7);
    EXPECT_EQ(jr::ReadOr(j, "u", 0u), 9u);
    EXPECT_EQ(jr::ReadOr(j, "neg", 0), -3);
    EXPECT_DOUBLE_EQ(jr::ReadOr(j, "d", 0.0), 1.5);
    EXPECT_DOUBLE_EQ(jr::ReadOr(j, "i", 0.0), 7.0);
    EXPECT_EQ(jr::ReadOr(j, "s", ""), "x");
    EXPECT_TRUE(jr::ReadOr(j, "b", false));
}

TEST(JsonReadTest, WrongTypesGiveTheDefault)
{
    const json j =
        json::parse(R"({"i": "7", "n": null, "d": 1.5, "s": 3, "b": 1, "arr": [1], "o": {}})");
    EXPECT_EQ(jr::ReadOr(j, "i", 4), 4);
    EXPECT_EQ(jr::ReadOr(j, "n", 4), 4);
    EXPECT_EQ(jr::ReadOr(j, "d", 4), 4); // a float is not an integer
    EXPECT_EQ(jr::ReadOr(j, "s", "def"), "def");
    EXPECT_FALSE(jr::ReadOr(j, "b", false));
    EXPECT_EQ(jr::ReadOr(j, "arr", 4), 4);
    EXPECT_EQ(jr::ReadOr(j, "o", 4), 4);
    EXPECT_EQ(jr::ReadOr(j, "missing", 4), 4);
}

TEST(JsonReadTest, NonObjectGivesTheDefault)
{
    EXPECT_EQ(jr::ReadOr(json::array({1, 2}), "x", 5), 5);
    EXPECT_EQ(jr::ReadOr(json(nullptr), "x", 5), 5);
    EXPECT_EQ(jr::ReadOr(json("text"), "x", 5), 5);
}

TEST(JsonReadTest, RangeIsCheckedForNarrowTypes)
{
    const json j = json::parse(R"({"big": 70000, "neg": -1, "huge": 99999999999})");
    EXPECT_EQ(jr::ReadOr<uint16_t>(j, "big", 3), 3);
    EXPECT_EQ(jr::ReadOr<uint32_t>(j, "neg", 3u), 3u);
    EXPECT_EQ(jr::ReadOr(j, "huge", 3), 3);
    EXPECT_EQ(jr::ReadOr<int64_t>(j, "huge", 3), 99999999999LL);
    EXPECT_EQ(jr::ReadOr<uint8_t>(j, "neg", 2), 2);
}

TEST(JsonReadTest, ReadIfLeavesOutAloneOnMismatch)
{
    const json j = json::parse(R"({"a": 1, "b": "no"})");
    int out = 9;
    EXPECT_FALSE(jr::ReadIf(j, "b", out));
    EXPECT_EQ(out, 9);
    EXPECT_TRUE(jr::ReadIf(j, "a", out));
    EXPECT_EQ(out, 1);
}

TEST(JsonReadTest, WorksWithOrderedJson)
{
    const nlohmann::ordered_json j = nlohmann::ordered_json::parse(R"({"exocet": "1"})");
    EXPECT_EQ(jr::ReadOr(j, "exocet", 0), 0);
}
