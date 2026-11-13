#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include "core/AdfWrapper.h"
#include "core/DiskValidator.h"

class BrokenChainTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create a blank formatted OFS disk
        if (adm::core::AdfHandler::CreateBlank("broken_chain_test.adf", "test", true)) {
            mounted = vol.Mount("broken_chain_test.adf");
        }
        vol.SetWriteProtected(false);
    }

    void TearDown() override
    {
        vol.Unmount();
        if (std::filesystem::exists("broken_chain_test.adf")) {
            std::filesystem::remove("broken_chain_test.adf");
        }
    }

    adm::core::AdfHandler vol;
    bool mounted = false;
};

TEST_F(BrokenChainTest, DetectsBrokenBlockChain)
{
    ASSERT_TRUE(mounted) << "Could not mount broken_chain_test.adf";

    // Verify it is OFS
    auto info = vol.GetDiskInfo();
    std::cout << "Disk Type: " << info.type << std::endl;
    ASSERT_EQ(info.type, "OFS") << "Test must be running on OFS disk to reproduce chain breakage";

    // 1. Create a file spanning multiple blocks
    // In OFS, data blocks are 488 bytes + 24 bytes header.
    // We want at least Header -> Data1 -> Data2 -> Data3 so we can break the link between Data1 and
    // Data2. 488 * 3 = 1464 bytes. Let's make it 2000 bytes.
    std::vector<uint8_t> fileData(2000, 0xAA);

    std::string tempFileName = "temp_large_file.bin";
    {
        std::ofstream tempFile(tempFileName, std::ios::binary);
        tempFile.write(reinterpret_cast<const char*>(fileData.data()), fileData.size());
    }

    ASSERT_TRUE(vol.ImportFile(0, tempFileName)); // Import to root
    std::filesystem::remove(tempFileName);

    // 2. Find the file header block
    auto root = vol.GetFileTree();
    ASSERT_FALSE(root.children.empty());

    int32_t headerSector = -1;
    for (const auto& node : root.children) {
        if (node.name == tempFileName) {
            headerSector = node.startSector;
            break;
        }
    }
    ASSERT_NE(headerSector, -1) << "File not found on disk";

    // 3. Get the block chain (should be valid initially)
    auto blocks = vol.GetFileBlocks(headerSector);
    ASSERT_GE(blocks.size(), 4u); // Header + at least 3 data blocks

    // 4. Break the chain!
    // Blocks[0] is Header. Blocks[1] is first Data block. Blocks[2] is second Data Block.
    // If we clear Blocks[2], the link from Blocks[1] to Blocks[2] will be broken
    // because Blocks[1] contains a pointer to Blocks[2].
    // Wait, simple "ClearBlock" just writes zeros.
    // In OFS data block:
    // 0x00: Primary Type usually? No, for Data block:
    // 0x00: Next Block Pointer (first longword)
    // 0x04: Header Block Pointer
    // ... data ...

    // We want to simulate the user scenario "zero filled the block halfway through the chain".
    // If we zero fill Blocks[2] (the middle data block):
    // - Blocks[1] points to Blocks[2].
    // - Blocks[2] is now all zeros.
    // - AdfLib (and AdfWrapper::GetFileBlocks) follows [1] to [2].
    // - It reads [2]. It sees it's not a valid data block (type is 0 maybe?).
    // - AdfLib might verify the block logic.

    // The user says: "I then clear / set to all zeros the block halfway through the chain... The
    // file structure was scanned OK and the block chains are valid." This implies AdfLib (or our
    // code) might just stop traversing or ignore the error.

    // Let's Zero out blocks[2].
    ASSERT_TRUE(vol.ClearBlock(blocks[2]));

    // 5. Run Validation
    adm::core::DiskValidator validator;
    auto result = validator.Validate(vol);

    // 6. Check results
    // We expect an ERROR in BlockChain or similar category because the chain is broken (file size
    // implies more data, or prev block points to invalid block).
    bool foundError = false;
    for (const auto& issue : result.issues) {
        // We look for errors related to this file or block chain
        if (issue.severity == adm::core::ValidationSeverity::Error &&
            (issue.category == adm::core::ValidationCategory::BlockChain ||
             issue.category == adm::core::ValidationCategory::DataBlock)) {
            // We might get specific error messages
            foundError = true;
            break;
        }
    }

    EXPECT_TRUE(foundError)
        << "Validator failed to report broken block chain for file with zeroed middle block";
}
