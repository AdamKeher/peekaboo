#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "../src/ui/TableLayout.h"

using adm::ui::TableLayoutStore;

namespace {

// A layout as ImGui writes one: window sections, table sections, a docking section. The table
// sections are not contiguous, which is the point - they have to be picked out, not sliced.
constexpr const char* kLayout = R"([Window][Debug##Default]
Pos=60,60
Size=400,400

[Table][0x6A9A5C90,6]
RefScale=24
Column 0  Width=137
Column 1  Weight=1.0000

[Window][Block]
Pos=402,32
DockId=0x00000002,0

[Table][0xBE9496E9,2]
Column 0  Width=80 Visible=0 Order=1
Column 1  Width=90 Order=0

[Docking][Data]
DockSpace ID=0x08BD597D
)";

} // namespace

TEST(TableLayoutStore, ExtractKeepsOnlyTableSections)
{
    const std::string tables = TableLayoutStore::ExtractTables(kLayout);
    EXPECT_NE(tables.find("[Table][0x6A9A5C90,6]"), std::string::npos);
    EXPECT_NE(tables.find("[Table][0xBE9496E9,2]"), std::string::npos);
    EXPECT_NE(tables.find("Width=137"), std::string::npos);
    EXPECT_NE(tables.find("Visible=0 Order=1"), std::string::npos);
    EXPECT_EQ(tables.find("[Window]"), std::string::npos);
    EXPECT_EQ(tables.find("[Docking]"), std::string::npos);
    EXPECT_EQ(tables.find("DockSpace"), std::string::npos);
    EXPECT_EQ(tables.find("Pos=60,60"), std::string::npos);
}

TEST(TableLayoutStore, StripRemovesOnlyTableSections)
{
    const std::string rest = TableLayoutStore::StripTables(kLayout);
    EXPECT_EQ(rest.find("[Table]"), std::string::npos);
    EXPECT_EQ(rest.find("Width=137"), std::string::npos);
    EXPECT_NE(rest.find("[Window][Debug##Default]"), std::string::npos);
    EXPECT_NE(rest.find("[Window][Block]"), std::string::npos);
    EXPECT_NE(rest.find("DockId=0x00000002,0"), std::string::npos);
    EXPECT_NE(rest.find("[Docking][Data]"), std::string::npos);
}

TEST(TableLayoutStore, ExtractAndStripPartitionEveryNonBlankLine)
{
    const std::string tables = TableLayoutStore::ExtractTables(kLayout);
    const std::string rest = TableLayoutStore::StripTables(kLayout);
    auto countLines = [](const std::string& s) {
        int n = 0;
        for (size_t pos = 0; pos < s.size(); ++pos)
            if (s[pos] != '\n' && (pos == 0 || s[pos - 1] == '\n'))
                ++n;
        return n;
    };
    const std::string all(kLayout);
    EXPECT_EQ(countLines(tables) + countLines(rest), countLines(all));
}

TEST(TableLayoutStore, ApplyToReplacesTheLayoutsOwnTables)
{
    // A workspace saved before the store existed still carries its own columns. They must lose:
    // two [Table] sections with the same ID would otherwise merge field by field rather than one
    // cleanly replacing the other.
    const auto file = std::filesystem::temp_directory_path() / "adm_table_layout_apply.ini";
    {
        std::ofstream out(file, std::ios::binary);
        out << "[Table][0x6A9A5C90,6]\nColumn 0  Width=999\n";
    }

    TableLayoutStore store;
    store.Load(file);
    ASSERT_FALSE(store.Ini().empty());

    const std::string merged = store.ApplyTo(kLayout);
    EXPECT_NE(merged.find("Width=999"), std::string::npos);
    EXPECT_EQ(merged.find("Width=137"), std::string::npos);
    EXPECT_EQ(merged.find("[Table][0xBE9496E9,2]"), std::string::npos);
    EXPECT_NE(merged.find("[Docking][Data]"), std::string::npos);
    EXPECT_NE(merged.find("[Window][Block]"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(TableLayoutStore, SaveThenLoadKeepsOnlyTables)
{
    const auto file = std::filesystem::temp_directory_path() / "adm_table_layout_save.ini";
    std::error_code ec;
    std::filesystem::remove(file, ec);

    // An empty store must not write: a launch that failed before the first frame would
    // otherwise replace a good file with nothing.
    TableLayoutStore empty;
    empty.Save(file);
    EXPECT_FALSE(std::filesystem::exists(file));

    // A hand-edited stray [Window] section in our own file must not reach the ImGui loader.
    {
        std::ofstream out(file, std::ios::binary);
        out << kLayout;
    }
    TableLayoutStore loaded;
    loaded.Load(file);
    EXPECT_NE(loaded.Ini().find("Width=137"), std::string::npos);
    EXPECT_EQ(loaded.Ini().find("[Window]"), std::string::npos);

    loaded.Save(file);
    TableLayoutStore reloaded;
    reloaded.Load(file);
    EXPECT_EQ(reloaded.Ini(), loaded.Ini());

    std::filesystem::remove(file, ec);
}

TEST(TableLayoutStore, LoadOfAMissingFileLeavesTheStoreEmpty)
{
    TableLayoutStore store;
    store.Load(std::filesystem::temp_directory_path() / "adm_table_layout_absent.ini");
    EXPECT_TRUE(store.Ini().empty());
}

TEST(TableLayoutStore, ApplyToLeavesTheLayoutAloneWhileEmpty)
{
    // First run after the store is introduced: nothing is saved yet, so a workspace's own
    // [Table] sections have to survive - they are what seeds the store on the way out.
    TableLayoutStore store;
    EXPECT_EQ(store.ApplyTo(kLayout), std::string(kLayout));
}

TEST(TableLayoutStore, RoundTripsThroughExtract)
{
    const std::string tables = TableLayoutStore::ExtractTables(kLayout);
    EXPECT_EQ(TableLayoutStore::ExtractTables(tables), tables);
    EXPECT_TRUE(TableLayoutStore::StripTables(tables).empty());
}

TEST(TableLayoutStore, HandlesEmptyAndUnterminatedInput)
{
    EXPECT_TRUE(TableLayoutStore::ExtractTables("").empty());
    EXPECT_TRUE(TableLayoutStore::StripTables("").empty());
    // No trailing newline on the last line.
    EXPECT_EQ(TableLayoutStore::ExtractTables("[Table][0x1,1]\nColumn 0  Width=8"),
              "[Table][0x1,1]\nColumn 0  Width=8\n");
}
