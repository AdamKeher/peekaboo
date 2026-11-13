#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/core/DiskFileIndex.h"

using adm::core::AdfHandler;
using adm::core::DiskFileEntry;
using adm::core::DiskFileIndex;
using adm::core::FileNode;

namespace {

FileNode MakeNode(const std::string& name, bool isDir, uint32_t header, int32_t size = 0)
{
    FileNode n;
    n.name = name;
    n.isDir = isDir;
    n.startSector = static_cast<int32_t>(header);
    n.parentSector = 880;
    n.size = size;
    n.modified = {1992, 3, 15, 10, 30, 0};
    return n;
}

// root -> { s/ -> startup-sequence, loader }
FileNode MakeTree()
{
    FileNode root = MakeNode("Workbench", true, 880);
    FileNode s = MakeNode("s", true, 881);
    s.children.push_back(MakeNode("startup-sequence", false, 882, 64));
    root.children.push_back(std::move(s));
    root.children.push_back(MakeNode("loader", false, 883, 2048));
    return root;
}

const DiskFileEntry* Find(const DiskFileIndex& idx, const std::string& path)
{
    return idx.FindByPath(path);
}

} // namespace

TEST(DiskFileIndexTest, FlattensTheTreeWithFullPaths)
{
    DiskFileIndex idx;
    idx.Build(MakeTree());

    ASSERT_EQ(idx.Entries().size(), 3u);
    // Depth-first: the directory, then its child, then the sibling.
    EXPECT_EQ(idx.Entries()[0].path, "s");
    EXPECT_TRUE(idx.Entries()[0].isDir);
    EXPECT_EQ(idx.Entries()[0].depth, 0);
    EXPECT_EQ(idx.Entries()[1].path, "s/startup-sequence");
    EXPECT_EQ(idx.Entries()[1].depth, 1);
    EXPECT_EQ(idx.Entries()[2].path, "loader");
}

TEST(DiskFileIndexTest, TheVolumeNodeItselfIsNotAnEntry)
{
    DiskFileIndex idx;
    idx.Build(MakeTree());
    // Paths are volume-relative, so nothing starts with the disk's name.
    for (const auto& e : idx.Entries())
        EXPECT_EQ(e.path.rfind("Workbench", 0), std::string::npos);
}

TEST(DiskFileIndexTest, LooksUpByPathIgnoringCaseAndSeparators)
{
    DiskFileIndex idx;
    idx.Build(MakeTree());

    const auto* e = Find(idx, "s/startup-sequence");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->headerBlock, 882u);

    // AmigaDOS is case-insensitive, and all of these name the same file.
    for (const char* p :
         {"S/Startup-Sequence", "/s/startup-sequence", "Workbench:s/startup-sequence",
          ":s/startup-sequence", "s\\startup-sequence", "s/startup-sequence/"})
        EXPECT_EQ(Find(idx, p), e) << p;

    EXPECT_EQ(Find(idx, "s/nothing"), nullptr);
    EXPECT_EQ(Find(idx, ""), nullptr);
    EXPECT_EQ(Find(idx, "/"), nullptr);
}

TEST(DiskFileIndexTest, LooksUpByHeaderBlock)
{
    DiskFileIndex idx;
    idx.Build(MakeTree());

    const auto* e = idx.FindByHeader(883);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->path, "loader");
    EXPECT_EQ(e->size, 2048u);
    EXPECT_EQ(idx.FindByHeader(9999), nullptr);
    EXPECT_EQ(idx.FindByHeader(0), nullptr);
}

TEST(DiskFileIndexTest, CarriesMetadataThroughFromTheFileNode)
{
    DiskFileIndex idx;
    FileNode root = MakeNode("Vol", true, 880);
    FileNode f = MakeNode("readme", false, 890, 17);
    f.comment = "notes";
    f.access = 0x0F;
    f.dataType = "Amiga Executable";
    f.dataTypeGroup = "Executable";
    root.children.push_back(std::move(f));
    idx.Build(root);

    ASSERT_EQ(idx.Entries().size(), 1u);
    const auto& e = idx.Entries()[0];
    EXPECT_EQ(e.comment, "notes");
    EXPECT_EQ(e.protection, 0x0F);
    EXPECT_EQ(e.dataType, "Amiga Executable");
    EXPECT_EQ(e.dataTypeGroup, "Executable");
    EXPECT_EQ(e.modified, "1992-03-15 10:30:00");
    EXPECT_EQ(e.parentSector, 880u);
}

TEST(DiskFileIndexTest, DeletedEntriesAreListedAndFlagged)
{
    DiskFileIndex idx;
    FileNode gone = MakeNode("scratched", false, 900, 10);
    gone.isDeleted = true;
    idx.Build(MakeTree(), {gone});

    ASSERT_EQ(idx.Entries().size(), 4u);
    const auto* e = idx.FindByHeader(900);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->isDeleted);
    EXPECT_EQ(e->path, "scratched");
}

TEST(DiskFileIndexTest, ALiveFileWinsOverADeletedOneOfTheSameName)
{
    DiskFileIndex idx;
    FileNode root = MakeNode("Vol", true, 880);
    root.children.push_back(MakeNode("loader", false, 883, 100));
    FileNode gone = MakeNode("loader", false, 900, 50);
    gone.isDeleted = true;
    idx.Build(root, {gone});

    const auto* e = Find(idx, "loader");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->headerBlock, 883u); // the surviving file, not the scrubbed one
    EXPECT_FALSE(e->isDeleted);
}

TEST(DiskFileIndexTest, ClearEmptiesTheIndex)
{
    DiskFileIndex idx;
    idx.Build(MakeTree());
    ASSERT_FALSE(idx.Empty());
    idx.Clear();
    EXPECT_TRUE(idx.Empty());
    EXPECT_EQ(idx.FindByHeader(882), nullptr);
}

TEST(DiskFileIndexTest, NormalisePathIsIdempotent)
{
    EXPECT_EQ(DiskFileIndex::NormalisePath("DF0:s/startup-sequence"), "s/startup-sequence");
    EXPECT_EQ(DiskFileIndex::NormalisePath("s/startup-sequence"), "s/startup-sequence");
    EXPECT_EQ(DiskFileIndex::NormalisePath("//s//"), "s");
    EXPECT_EQ(DiskFileIndex::NormalisePath(":"), "");
}

// --- Against a real ADF ------------------------------------------------------
//
// The index is only half the answer for "which file owns this sector?" — the
// other half is AdfHandler's block-to-header map, which is populated as a side
// effect of GetAllBlockTypes(). This checks both together.

class DiskFileIndexRealAdfTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_path = (std::filesystem::temp_directory_path() / "adm_diskfileindex_test.adf").string();
        std::filesystem::remove(m_path);
        if (!AdfHandler::CreateBlank(m_path, "Test")) {
            m_ready = false;
            return;
        }
        if (!m_vol.Mount(m_path)) {
            m_ready = false;
            return;
        }
        m_vol.SetWriteProtected(false);
        // A file big enough to occupy several data blocks, inside a directory.
        m_ready = m_vol.CreateDirectory(880, "s");
        const uint32_t sSector = m_vol.FindFile(880, "s");
        std::vector<uint8_t> payload(4096, 0xAB);
        m_ready = m_ready && sSector != 0 && m_vol.ImportFile(sSector, "startup-sequence", payload);
    }

    void TearDown() override
    {
        m_vol.Unmount();
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    AdfHandler m_vol;
    std::string m_path;
    bool m_ready = false;
};

TEST_F(DiskFileIndexRealAdfTest, IndexesAFileWrittenToARealDisk)
{
    ASSERT_TRUE(m_ready) << "could not build the scratch ADF";

    DiskFileIndex idx;
    idx.Build(m_vol.GetFileTree());

    const auto* e = idx.FindByPath("s/startup-sequence");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->size, 4096u);
    EXPECT_FALSE(e->isDir);
    EXPECT_NE(e->headerBlock, 0u);
}

TEST_F(DiskFileIndexRealAdfTest, MapsADataBlockBackToItsOwningFile)
{
    ASSERT_TRUE(m_ready) << "could not build the scratch ADF";

    DiskFileIndex idx;
    idx.Build(m_vol.GetFileTree());
    const auto* e = idx.FindByPath("s/startup-sequence");
    ASSERT_NE(e, nullptr);

    // The reverse map is only built as a side effect of this call; without it
    // an FFS data block reports no owner at all.
    m_vol.GetAllBlockTypes();

    auto blocks = m_vol.GetFileBlocks(e->headerBlock);
    ASSERT_GE(blocks.size(), 2u) << "expected a header plus several data blocks";

    // Pick a block that is not the header itself.
    uint32_t dataBlock = 0;
    for (uint32_t b : blocks)
        if (b != e->headerBlock) {
            dataBlock = b;
            break;
        }
    ASSERT_NE(dataBlock, 0u);

    auto details = m_vol.AnalyzeBlock(dataBlock, m_vol.ReadBlock(dataBlock));
    EXPECT_EQ(details.fileHeader, e->headerBlock);

    const auto* owner = idx.FindByHeader(details.fileHeader);
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(owner->path, "s/startup-sequence");
}
