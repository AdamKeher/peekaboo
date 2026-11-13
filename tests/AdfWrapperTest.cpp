#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "core/AdfWrapper.h"

// Basic tests without a disk
TEST(AdfHandlerTest, ConstructorDoesNotCrash)
{
    adm::core::AdfHandler vol;
    EXPECT_FALSE(vol.IsMounted());
}

TEST(AdfHandlerTest, MountInvalidFileReturnsFalse)
{
    adm::core::AdfHandler vol;
    EXPECT_FALSE(vol.Mount("nonexistent.adf"));
}

TEST(AdfHandlerTest, UnmountWhenNotMountedIsNoOp)
{
    adm::core::AdfHandler vol;
    vol.Unmount();
    EXPECT_FALSE(vol.IsMounted());
}

// Tests with test.adf fixture
class AdfWrapperTestWithDisk : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // test.adf moved to tests/images

        // Try relative to project root (e.g. running from root)
        mounted = vol.Mount("tests/images/test.adf");

        if (!mounted) {
            // Try relative to build directory (one level deep)
            mounted = vol.Mount("../tests/images/test.adf");
        }
        if (!mounted) {
            // Try relative to build/debug directory (two levels deep)
            mounted = vol.Mount("../../tests/images/test.adf");
        }
        // Images mount write-protected; these tests exercise the mutators.
        vol.SetWriteProtected(false);
    }

    adm::core::AdfHandler vol;
    bool mounted = false;
};

// Tests with a fresh, clean ADF
class AdfWrapperTestWithCleanDisk : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create a blank formatted disk
        if (adm::core::AdfHandler::CreateBlank("clean_test.adf", "clean")) {
            mounted = vol.Mount("clean_test.adf");
        }
        vol.SetWriteProtected(false);
    }

    void TearDown() override
    {
        vol.Unmount();
        if (std::filesystem::exists("clean_test.adf")) {
            std::filesystem::remove("clean_test.adf");
        }
    }

    adm::core::AdfHandler vol;
    bool mounted = false;
};

TEST_F(AdfWrapperTestWithDisk, MountsSuccessfully)
{
    ASSERT_TRUE(mounted) << "Could not mount test.adf";
    EXPECT_TRUE(vol.IsMounted());
}

TEST_F(AdfWrapperTestWithDisk, HasFileSystem)
{
    if (!mounted)
        GTEST_SKIP() << "test.adf not available";
    EXPECT_TRUE(vol.HasFileSystem());
}

TEST_F(AdfWrapperTestWithDisk, GetDiskInfoReturnsValidData)
{
    if (!mounted)
        GTEST_SKIP() << "test.adf not available";

    auto info = vol.GetDiskInfo();

    EXPECT_FALSE(info.name.empty());
    EXPECT_GT(info.sizeBytes, 0u);
    EXPECT_GE(info.usedPercentage, 0.0f);
    EXPECT_LE(info.usedPercentage, 100.0f);
    EXPECT_EQ(info.cylinders, 80u);
    EXPECT_EQ(info.heads, 2u);
    EXPECT_EQ(info.sectors, 11u);
}

TEST_F(AdfWrapperTestWithDisk, GetFileTreeReturnsRootNode)
{
    if (!mounted)
        GTEST_SKIP() << "test.adf not available";

    auto root = vol.GetFileTree();

    // Root node should represent the volume
    EXPECT_FALSE(root.name.empty());
    EXPECT_TRUE(root.isDir);
}

TEST_F(AdfWrapperTestWithDisk, ReadBlockReturns512Bytes)
{
    if (!mounted)
        GTEST_SKIP() << "test.adf not available";

    auto block = vol.ReadBlock(0);

    EXPECT_EQ(block.size(), 512u);
}

TEST_F(AdfWrapperTestWithDisk, ReadBlockOutOfRangeReturnsEmpty)
{
    if (!mounted)
        GTEST_SKIP() << "test.adf not available";

    auto block = vol.ReadBlock(9999);

    EXPECT_TRUE(block.empty());
}

TEST_F(AdfWrapperTestWithDisk, AnalyzeBlockDetectsBootBlock)
{
    if (!mounted)
        GTEST_SKIP() << "test.adf not available";

    auto block = vol.ReadBlock(0);
    auto details = vol.AnalyzeBlock(0, block);

    EXPECT_TRUE(details.isBootBlock);
    EXPECT_FALSE(details.dosType.empty());
}

TEST_F(AdfWrapperTestWithDisk, AnalyzeBlockDetectsRootBlock)
{
    if (!mounted)
        GTEST_SKIP() << "test.adf not available";

    // Root block is at block 880 for standard DD disks
    auto block = vol.ReadBlock(880);
    auto details = vol.AnalyzeBlock(880, block);

    EXPECT_EQ(details.secondaryType, 1); // ST_ROOT
    EXPECT_FALSE(details.name.empty());
}

TEST_F(AdfWrapperTestWithCleanDisk, FreeBlockCountTracksWritesThroughTheCache)
{
    ASSERT_TRUE(mounted);
    // GetFreeBlocks is cached (GetDiskInfo runs it once per frame per document); the cache
    // must invalidate on every mutation path or the usage bar goes stale.
    const uint32_t before = vol.GetFreeBlocks();
    ASSERT_GT(before, 0u);

    std::vector<uint8_t> data(4096, 0xAA);
    ASSERT_TRUE(vol.ImportFile(0, "cachetest.bin", data));
    const uint32_t afterWrite = vol.GetFreeBlocks();
    EXPECT_LT(afterWrite, before);

    ASSERT_TRUE(vol.DeleteFile(0, "cachetest.bin"));
    EXPECT_EQ(vol.GetFreeBlocks(), before);
}

TEST_F(AdfWrapperTestWithCleanDisk, GetBlockUsageReturnsCorrectSize)
{
    auto usage = vol.GetBlockUsage();
    EXPECT_EQ(usage.size(), 1760u); // 80 * 2 * 11
}

TEST_F(AdfWrapperTestWithCleanDisk, GetAllBlockTypesReturnsCorrectSize)
{
    auto types = vol.GetAllBlockTypes();
    EXPECT_EQ(types.size(), 1760u);

    // Check root block type (880)
    EXPECT_EQ(types[880], "ST_ROOT");
}

TEST_F(AdfWrapperTestWithDisk, GetZeroFilledBlocksReturnsCorrectSize)
{
    if (!mounted)
        GTEST_SKIP() << "test.adf not available";

    auto zeroFilled = vol.GetZeroFilledBlocks();

    // Standard DD ADF has 1760 blocks
    EXPECT_EQ(zeroFilled.size(), 1760u);

    // Boot block should not be zero-filled
    EXPECT_FALSE(zeroFilled[0]);
}

TEST_F(AdfWrapperTestWithDisk, GetRootBlockInfoReturnsValidData)
{
    if (!mounted)
        GTEST_SKIP() << "test.adf not available";

    auto root = vol.GetRootBlockInfo();

    EXPECT_TRUE(root.hasRoot);
    EXPECT_FALSE(root.volumeName.empty());
    EXPECT_EQ(root.blockSize, 512u);
    EXPECT_EQ(root.rootPtr, 880u);
}

// Move semantics tests
TEST(AdfHandlerTest, MoveConstructor)
{
    adm::core::AdfHandler vol1;
    vol1.Mount("tests/images/test.adf");

    adm::core::AdfHandler vol2(std::move(vol1));

    // vol1 should be in a valid but unspecified state
    // vol2 should have taken ownership
}

TEST(AdfHandlerTest, MoveAssignment)
{
    adm::core::AdfHandler vol1;
    adm::core::AdfHandler vol2;

    vol1.Mount("tests/images/test.adf");
    vol2 = std::move(vol1);

    // vol1 should be in a valid but unspecified state
    // vol2 should have taken ownership
}

// --- Space Calculation Tests ---

class AdfWrapperTestSpaceCalc : public AdfWrapperTestWithCleanDisk
{
};

TEST_F(AdfWrapperTestSpaceCalc, CalculateRequiredBlocks_SingleFile)
{
    // FFS Data Block Payload: 512 bytes.
    EXPECT_EQ(vol.CalculateRequiredBlocks(0), 1u);   // Just Header
    EXPECT_EQ(vol.CalculateRequiredBlocks(1), 2u);   // Header + 1 Data
    EXPECT_EQ(vol.CalculateRequiredBlocks(512), 2u); // Header + 1 Data
    EXPECT_EQ(vol.CalculateRequiredBlocks(513), 3u); // Header + 2 Data

    // Size 72*512 = 36864: Header + 72 Data. (Header holds 72 pointers).
    // Total = 1 + 72 = 73.
    EXPECT_EQ(vol.CalculateRequiredBlocks(36864), 73u);

    // Size 72*512 + 1: Header + 73 Data. Need Extension block.
    // Total = 1 + 73 + 1 = 75.
    EXPECT_EQ(vol.CalculateRequiredBlocks(36864 + 1), 75u);
}

TEST_F(AdfWrapperTestSpaceCalc, CalculateRequiredBlocks_Directories)
{
    // Create a temporary directory structure on host
    std::filesystem::create_directory("test_import_dir");
    std::ofstream("test_import_dir/file1.txt") << "content"; // 7 bytes -> 2 blocks (header+data)
    std::ofstream("test_import_dir/file2.txt") << "content"; // 7 bytes -> 2 blocks

    // Directory itself: 1 block.
    // Total = 1 (Dir) + 2 (File1) + 2 (File2) = 5 blocks.

    std::vector<std::string> paths = {"test_import_dir"};
    EXPECT_EQ(vol.CalculateRequiredBlocks(paths), 5u);

    std::filesystem::remove_all("test_import_dir");
}

TEST_F(AdfWrapperTestSpaceCalc, GetFreeBlocks_CleanDisk)
{
    uint32_t free = vol.GetFreeBlocks();
    // Standard DD FFS disk usually has ~1750+ free blocks
    EXPECT_GT(free, 1750u);
    EXPECT_LE(free, 1760u);
}

TEST_F(AdfWrapperTestSpaceCalc, PartialWriteSafety)
{
    // Attempt to write a file larger than the entire disk
    uint32_t totalBytes = 1760 * 512 + 4096; // > 880KB
    std::vector<uint8_t> hugeData(totalBytes, 0xAA);

    // Import should fail (return false)
    EXPECT_FALSE(vol.ImportFile(0, "huge_file", hugeData));

    // Verify file does NOT exist (was cleaned up)
    EXPECT_EQ(vol.FindFile(0, "huge_file"), 0u);
}

// --- Write protection -------------------------------------------------------
// The guard lives in AdfHandler rather than the UI because FileOperationService,
// DocumentTransfer and ViewerLauncher all mutate through a bare AdfHandler&.
// This test is the safety net for all of those call sites at once.

TEST(AdfHandlerTest, NewHandlerIsWriteProtected)
{
    adm::core::AdfHandler vol;
    EXPECT_TRUE(vol.IsWriteProtected());
}

class AdfWrapperTestProtected : public AdfWrapperTestWithCleanDisk
{
protected:
    void SetUp() override
    {
        AdfWrapperTestWithCleanDisk::SetUp();
        // The base fixture unlocks; re-lock so the mutators are exercised against
        // a protected image.
        vol.SetWriteProtected(true);
    }
};

TEST_F(AdfWrapperTestProtected, EveryMutatorIsRefused)
{
    ASSERT_TRUE(mounted);

    const std::vector<uint8_t> payload(16, 0x5A);
    std::vector<uint8_t> block(512, 0xFF);

    EXPECT_FALSE(vol.Format("nope"));
    EXPECT_FALSE(vol.SetVolumeName("nope"));
    EXPECT_FALSE(vol.CreateDirectory(0, "dir"));
    EXPECT_FALSE(vol.ImportFile(0, "file", payload));
    EXPECT_FALSE(vol.ImportFile(0, "nonexistent_host_file.bin"));
    EXPECT_FALSE(vol.DeleteFile(0, "file"));
    EXPECT_FALSE(vol.RenameFile(0, 0, "a", "b"));
    EXPECT_FALSE(vol.MoveFile(0, "a", 0, "b"));
    EXPECT_FALSE(vol.SetPermissions(880, 0));
    EXPECT_FALSE(vol.RestoreDeletedFile(880));
    EXPECT_FALSE(vol.ScrubFile(880));
    EXPECT_FALSE(vol.ClearBlock(881));
    EXPECT_FALSE(vol.WriteBlock(881, block));
    EXPECT_FALSE(vol.RevertBlock(881));

    // Nothing reached the buffer.
    EXPECT_FALSE(vol.HasUnsavedChanges());
    EXPECT_EQ(vol.FindFile(0, "file"), 0u);
}

TEST_F(AdfWrapperTestProtected, UnlockingRestoresWrites)
{
    ASSERT_TRUE(mounted);

    const std::vector<uint8_t> payload(16, 0x5A);
    ASSERT_FALSE(vol.ImportFile(0, "file", payload));

    vol.SetWriteProtected(false);
    EXPECT_TRUE(vol.ImportFile(0, "file", payload));
    EXPECT_NE(vol.FindFile(0, "file"), 0u);
    EXPECT_TRUE(vol.HasUnsavedChanges());
}
