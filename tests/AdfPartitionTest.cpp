// Hard-disk images in the disk manager: one AdfHandler per image, browsing one RDB partition at a
// time, with every block number relative to the partition being shown.

#include "core/AdfWrapper.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

void PutBe32(std::vector<uint8_t>& image, size_t offset, uint32_t value)
{
    image[offset] = static_cast<uint8_t>(value >> 24);
    image[offset + 1] = static_cast<uint8_t>(value >> 16);
    image[offset + 2] = static_cast<uint8_t>(value >> 8);
    image[offset + 3] = static_cast<uint8_t>(value);
}

uint32_t GetBe32(const std::vector<uint8_t>& image, size_t offset)
{
    return (uint32_t(image[offset]) << 24) | (uint32_t(image[offset + 1]) << 16) |
           (uint32_t(image[offset + 2]) << 8) | uint32_t(image[offset + 3]);
}

// RDB checksum over the block's 64 longwords.
void Seal(std::vector<uint8_t>& image, uint32_t block)
{
    const size_t base = size_t(block) * 512;
    PutBe32(image, base + 4, 64);
    PutBe32(image, base + 8, 0);
    uint32_t sum = 0;
    for (size_t i = 0; i < 64; ++i)
        sum += GetBe32(image, base + i * 4);
    PutBe32(image, base + 8, 0u - sum);
}

std::vector<uint8_t> BlankVolume(const std::filesystem::path& scratch, bool ofs)
{
    adm::core::AdfHandler init; // CreateBlank is static; a handler runs adfLibInit() first
    EXPECT_TRUE(
        adm::core::AdfHandler::CreateBlank(scratch.string(), ofs ? "Second" : "First", ofs));
    std::ifstream stream(scratch, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

class AdfPartitionTest : public ::testing::Test
{
protected:
    // 112 cylinders x 1 head x 32 sectors = 3584 blocks. The RDB lives in cylinders 0-1; DH0 is
    // an FFS volume on cylinders 2-56 and DH1 an OFS volume on 57-111 - 1760 blocks each, so a
    // blank floppy's filesystem drops straight in. (adflib's own adfCreateHd is not used: its
    // bitmap setup mixes absolute and partition-relative block numbers, so a partition that
    // does not start at block 0 comes out with no free blocks.)
    static constexpr uint32_t kCylBlocks = 32;

    void SetUp() override
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("peekaboo_adf_partition_" + std::to_string(nonce));
        std::filesystem::create_directories(root);
        image = root / "two-partitions.hdf";

        std::vector<uint8_t> bytes(112 * kCylBlocks * 512, 0);
        PutBe32(bytes, 0, 0x5244534B); // RDSK
        PutBe32(bytes, 0x0C, 7);       // host id
        PutBe32(bytes, 0x10, 512);     // block bytes
        PutBe32(bytes, 0x18, 0xFFFFFFFF);
        PutBe32(bytes, 0x1C, 1); // partition list
        PutBe32(bytes, 0x20, 0xFFFFFFFF);
        PutBe32(bytes, 0x24, 0xFFFFFFFF);
        PutBe32(bytes, 0x40, 112); // cylinders
        PutBe32(bytes, 0x44, 32);  // sectors
        PutBe32(bytes, 0x48, 1);   // heads
        PutBe32(bytes, 0x80, 0);   // RDB blocks lo
        PutBe32(bytes, 0x84, 63);  // RDB blocks hi
        PutBe32(bytes, 0x88, 2);   // lo cylinder
        PutBe32(bytes, 0x8C, 111); // hi cylinder
        PutBe32(bytes, 0x90, kCylBlocks);
        Seal(bytes, 0);

        const auto part = [&](uint32_t block, uint32_t next, const char* name, uint32_t lowCyl,
                              uint32_t highCyl, uint32_t dosType) {
            const size_t base = size_t(block) * 512;
            PutBe32(bytes, base, 0x50415254); // PART
            PutBe32(bytes, base + 0x0C, 7);
            PutBe32(bytes, base + 0x10, next);
            PutBe32(bytes, base + 0x14, 1); // bootable
            bytes[base + 0x24] = static_cast<uint8_t>(std::strlen(name));
            std::memcpy(&bytes[base + 0x25], name, std::strlen(name));
            const size_t env = base + 0x80;
            PutBe32(bytes, env + 0 * 4, 16);
            PutBe32(bytes, env + 1 * 4, 128);
            PutBe32(bytes, env + 3 * 4, 1);
            PutBe32(bytes, env + 4 * 4, 1);
            PutBe32(bytes, env + 5 * 4, 32);
            PutBe32(bytes, env + 6 * 4, 2);
            PutBe32(bytes, env + 9 * 4, lowCyl);
            PutBe32(bytes, env + 10 * 4, highCyl);
            PutBe32(bytes, env + 16 * 4, dosType);
            Seal(bytes, block);
        };
        part(1, 2, "DH0", 2, 56, 0x444F5301);
        part(2, 0xFFFFFFFF, "DH1", 57, 111, 0x444F5300);

        const auto ffs = BlankVolume(root / "ffs.adf", false);
        const auto ofs = BlankVolume(root / "ofs.adf", true);
        ASSERT_EQ(ffs.size(), 1760u * 512u);
        ASSERT_EQ(ofs.size(), 1760u * 512u);
        std::copy(ffs.begin(), ffs.end(), bytes.begin() + 2 * kCylBlocks * 512);
        std::copy(ofs.begin(), ofs.end(), bytes.begin() + 57 * kCylBlocks * 512);
        std::ofstream(image, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path root;
    std::filesystem::path image;
};

TEST_F(AdfPartitionTest, ListsAndSwitchesPartitions)
{
    adm::core::AdfHandler handler;
    ASSERT_TRUE(handler.Mount(image.string()));
    ASSERT_TRUE(handler.IsPartitionedDevice());

    const auto partitions = handler.GetPartitions();
    ASSERT_EQ(partitions.size(), 2u);
    EXPECT_EQ(partitions[0].name, "DH0");
    EXPECT_EQ(partitions[0].firstBlock, 2u * kCylBlocks);
    EXPECT_EQ(partitions[0].lastBlock, 57u * kCylBlocks - 1u);
    EXPECT_TRUE(partitions[0].dosFilesystem);
    EXPECT_EQ(partitions[0].filesystem, "DOS1");
    EXPECT_EQ(partitions[1].name, "DH1");
    EXPECT_EQ(partitions[1].firstBlock, 57u * kCylBlocks);
    EXPECT_EQ(partitions[1].filesystem, "DOS0");

    // Partition 0 is mounted by default and its blocks are counted from its own start.
    EXPECT_EQ(handler.GetSelectedPartition(), 0);
    ASSERT_TRUE(handler.HasFileSystem());
    EXPECT_EQ(handler.GetPartitionOffset(), 2u * kCylBlocks);
    EXPECT_EQ(handler.GetTotalBlocks(), 1760u);
    const auto info0 = handler.GetDiskInfo();
    EXPECT_EQ(info0.cylinders * info0.heads * info0.sectors, 1760u);
    EXPECT_EQ(info0.type, "FFS");
    EXPECT_EQ(info0.name, "First");

    ASSERT_TRUE(handler.SelectPartition(1));
    EXPECT_EQ(handler.GetSelectedPartition(), 1);
    EXPECT_EQ(handler.GetTotalBlocks(), 1760u);
    EXPECT_EQ(handler.GetDiskInfo().type, "OFS");
    EXPECT_EQ(handler.GetDiskInfo().name, "Second");

    // Whole image, raw.
    ASSERT_TRUE(handler.SelectPartition(-1));
    EXPECT_FALSE(handler.HasFileSystem());
    EXPECT_EQ(handler.GetPartitionOffset(), 0u);
    EXPECT_EQ(handler.GetTotalBlocks(), 112u * kCylBlocks);
    const auto rdsk = handler.ReadBlock(0);
    ASSERT_GE(rdsk.size(), 4u);
    EXPECT_EQ(std::string(rdsk.begin(), rdsk.begin() + 4), "RDSK");

    EXPECT_FALSE(handler.SelectPartition(2));
}

TEST_F(AdfPartitionTest, WritesStayInsideTheSelectedPartition)
{
    adm::core::AdfHandler handler;
    ASSERT_TRUE(handler.Mount(image.string()));
    handler.SetWriteProtected(false);
    ASSERT_TRUE(handler.SelectPartition(1));

    const std::vector<uint8_t> payload(1500, 0x5A);
    ASSERT_TRUE(handler.ImportFile(0, "hello.bin", payload));
    EXPECT_NE(handler.FindFile(0, "hello.bin"), 0u);
    EXPECT_TRUE(handler.HasUnsavedChanges());

    // Modified-block tracking is reported partition-relative.
    const auto& modified = handler.GetModifiedBlocks();
    ASSERT_FALSE(modified.empty());
    for (uint32_t block : modified)
        EXPECT_LT(block, handler.GetTotalBlocks());
    EXPECT_EQ(handler.GetBlockModificationStatus().size(), handler.GetTotalBlocks());

    // The other partition never saw the file.
    ASSERT_TRUE(handler.SelectPartition(0));
    EXPECT_EQ(handler.FindFile(0, "hello.bin"), 0u);
    ASSERT_TRUE(handler.SelectPartition(1));
    EXPECT_NE(handler.FindFile(0, "hello.bin"), 0u);

    // Saved to disk and reopened, it is still only in DH1.
    ASSERT_TRUE(handler.Save());
    adm::core::AdfHandler reopened;
    ASSERT_TRUE(reopened.Mount(image.string()));
    EXPECT_EQ(reopened.FindFile(0, "hello.bin"), 0u);
    ASSERT_TRUE(reopened.SelectPartition(1));
    EXPECT_NE(reopened.FindFile(0, "hello.bin"), 0u);
}

TEST(AdfPartitionFloppyTest, FloppiesAreNotPartitioned)
{
    const auto path =
        std::filesystem::temp_directory_path() /
        ("peekaboo_floppy_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".adf");
    adm::core::AdfHandler handler;
    ASSERT_TRUE(adm::core::AdfHandler::CreateBlank(path.string(), "Empty"));
    ASSERT_TRUE(handler.Mount(path.string()));
    EXPECT_FALSE(handler.IsPartitionedDevice());
    EXPECT_TRUE(handler.GetPartitions().empty());
    EXPECT_EQ(handler.GetTotalBlocks(), 1760u);
    EXPECT_EQ(handler.GetPartitionOffset(), 0u);
    EXPECT_TRUE(handler.SelectPartition(0));
    EXPECT_FALSE(handler.SelectPartition(1));
    handler.Unmount();
    std::filesystem::remove(path);
}

} // namespace
