#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include "core/DmsParser.h"

TEST(DmsParserTest, ParseNonExistentFile)
{
    auto info = adm::core::DmsParser::Parse("non_existent_file.dms");
    EXPECT_FALSE(info.isValid);
}

// We can assume test.adf exists as other tests use it?
// Actually AdfWrapperTest uses it from 'tests/images/test.adf' or creates it?
// Let's rely on a file we create or know exists.
TEST(DmsParserTest, ParseInvalidFile)
{
    // Create a dummy file
    std::string path = "dummy_invalid.dms";
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite("NOTADMS", 1, 7, f);
        fclose(f);
    }

    auto info = adm::core::DmsParser::Parse(path);
    EXPECT_FALSE(info.isValid);

    std::filesystem::remove(path);
}
