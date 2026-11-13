#include "core/HardfileInspector.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using adm::core::HardfileInspector;

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

// Writes the RDB checksum longword so the `size` longs of the block sum to zero.
void SealBlock(std::vector<uint8_t>& image, uint32_t block, uint32_t sizeLongs)
{
    const size_t base = size_t(block) * 512;
    PutBe32(image, base + 4, sizeLongs);
    PutBe32(image, base + 8, 0);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < sizeLongs; ++i)
        sum += GetBe32(image, base + i * 4u);
    PutBe32(image, base + 8, static_cast<uint32_t>(0u - sum));
}

void WritePart(std::vector<uint8_t>& image, uint32_t block, uint32_t next, const char* name,
               uint32_t lowCyl, uint32_t highCyl, uint32_t dosType, int32_t bootPri, bool bootable)
{
    const size_t base = size_t(block) * 512;
    PutBe32(image, base, 0x50415254);
    PutBe32(image, base + 0x10, next);
    PutBe32(image, base + 0x14, bootable ? 1 : 0);
    image[base + 0x24] = static_cast<uint8_t>(std::strlen(name));
    std::memcpy(&image[base + 0x25], name, std::strlen(name));
    const size_t env = base + 0x80;
    PutBe32(image, env + 0 * 4, 16);  // table size
    PutBe32(image, env + 1 * 4, 128); // size block
    PutBe32(image, env + 3 * 4, 1);   // surfaces
    PutBe32(image, env + 4 * 4, 1);   // sectors per block
    PutBe32(image, env + 5 * 4, 32);  // blocks per track
    PutBe32(image, env + 6 * 4, 2);   // reserved
    PutBe32(image, env + 9 * 4, lowCyl);
    PutBe32(image, env + 10 * 4, highCyl);
    PutBe32(image, env + 15 * 4, static_cast<uint32_t>(bootPri));
    PutBe32(image, env + 16 * 4, dosType);
    SealBlock(image, block, 64);
}

// A 2-partition RDB image: RDSK at block 0, PART at 1 and 2, FSHD at 3.
std::vector<uint8_t> BuildRdbImage(uint32_t cylinders = 40)
{
    std::vector<uint8_t> image(size_t(cylinders) * 32 * 512, 0);
    PutBe32(image, 0, 0x5244534B);
    PutBe32(image, 0x10, 512);        // block bytes
    PutBe32(image, 0x18, 0xFFFFFFFF); // bad blocks
    PutBe32(image, 0x1C, 1);          // partition list
    PutBe32(image, 0x20, 3);          // filesystem header list
    PutBe32(image, 0x24, 0xFFFFFFFF); // drive init
    PutBe32(image, 0x40, cylinders);
    PutBe32(image, 0x44, 32);
    PutBe32(image, 0x48, 1);
    std::memcpy(&image[0xA0], "PEEKABOO", 8);
    std::memcpy(&image[0xA8], "TEST DISK       ", 16);
    SealBlock(image, 0, 64);

    WritePart(image, 1, 2, "DH0", 2, 19, 0x444F5303, 0, true);
    WritePart(image, 2, 0xFFFFFFFF, "DH1", 20, cylinders - 1, 0x50465303, -10, false);

    PutBe32(image, 3 * 512, 0x46534844);
    PutBe32(image, 3 * 512 + 0x10, 0xFFFFFFFF);
    PutBe32(image, 3 * 512 + 0x20, 0x50465303);
    PutBe32(image, 3 * 512 + 0x24, (19u << 16) | 2u);
    SealBlock(image, 3, 64);
    return image;
}

class HardfileInspectorFileTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("peekaboo_hardfile_inspector_" + std::to_string(nonce));
        std::filesystem::create_directories(root);
    }
    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path root;
};

TEST(HardfileInspectorTest, DecodesRdbPartitionsAndFilesystems)
{
    const auto image = BuildRdbImage();
    const auto info = HardfileInspector::InspectBytes(image, image.size());

    ASSERT_TRUE(info.hasRdb);
    EXPECT_TRUE(info.rdbChecksumValid);
    EXPECT_EQ(info.rdbBlock, 0u);
    EXPECT_EQ(info.cylinders, 40u);
    EXPECT_EQ(info.sectors, 32u);
    EXPECT_EQ(info.heads, 1u);
    EXPECT_EQ(info.diskVendor, "PEEKABOO");
    EXPECT_EQ(info.diskProduct, "TEST DISK");
    EXPECT_TRUE(info.warnings.empty());

    ASSERT_EQ(info.partitions.size(), 2u);
    const auto& dh0 = info.partitions[0];
    EXPECT_EQ(dh0.driveName, "DH0");
    EXPECT_TRUE(dh0.checksumValid);
    EXPECT_TRUE(dh0.bootable);
    EXPECT_EQ(dh0.lowCylinder, 2u);
    EXPECT_EQ(dh0.highCylinder, 19u);
    EXPECT_EQ(dh0.firstBlock, 64u);
    EXPECT_EQ(dh0.blockCount, 18u * 32u);
    EXPECT_EQ(HardfileInspector::DosTypeText(dh0.dosType), "DOS3");
    EXPECT_EQ(HardfileInspector::DosTypeDescription(dh0.dosType), "FFS International");

    const auto& dh1 = info.partitions[1];
    EXPECT_EQ(dh1.driveName, "DH1");
    EXPECT_FALSE(dh1.bootable);
    EXPECT_EQ(dh1.bootPriority, -10);
    EXPECT_EQ(HardfileInspector::DosTypeText(dh1.dosType), "PFS3");

    ASSERT_EQ(info.filesystems.size(), 1u);
    EXPECT_EQ(info.filesystems[0].dosType, 0x50465303u);
    EXPECT_EQ(info.filesystems[0].version, (19u << 16) | 2u);
}

TEST(HardfileInspectorTest, ReportsBadChecksumsAndOversizedPartitions)
{
    auto image = BuildRdbImage();
    image[1 * 512 + 0x30] ^= 0xFF; // corrupt PART block 1 without resealing
    // Claim more cylinders than the image holds for DH1.
    PutBe32(image, 2 * 512 + 0x80 + 10 * 4, 400);
    SealBlock(image, 2, 64);

    const auto info = HardfileInspector::InspectBytes(image, image.size());
    ASSERT_EQ(info.partitions.size(), 2u);
    EXPECT_FALSE(info.partitions[0].checksumValid);
    EXPECT_TRUE(info.partitions[1].checksumValid);
    bool sawChecksum = false;
    bool sawOversize = false;
    for (const auto& warning : info.warnings) {
        sawChecksum |= warning.find("PART block 1 has a bad checksum") != std::string::npos;
        sawOversize |= warning.find("DH1 extends past the end") != std::string::npos;
    }
    EXPECT_TRUE(sawChecksum);
    EXPECT_TRUE(sawOversize);
}

TEST(HardfileInspectorTest, AHugeChecksumSizeFieldIsRejectedNotSummed)
{
    // A corrupt SummedLongs of 2^30 + 1: multiplied by four it wraps to 4, which read as "fits
    // in the block" and sent the checksum loop round a billion times on the UI thread.
    auto image = BuildRdbImage();
    PutBe32(image, 0 * 512 + 4, 0x40000001u);

    const auto info = HardfileInspector::InspectBytes(image, image.size());
    ASSERT_TRUE(info.hasRdb);
    EXPECT_FALSE(info.rdbChecksumValid);
}

TEST(HardfileInspectorTest, StopsOnACyclicPartitionChain)
{
    auto image = BuildRdbImage();
    PutBe32(image, 2 * 512 + 0x10, 1); // DH1 points back at DH0
    SealBlock(image, 2, 64);
    const auto info = HardfileInspector::InspectBytes(image, image.size());
    EXPECT_EQ(info.partitions.size(), 2u);
}

TEST(HardfileInspectorTest, GuessesGeometryWithoutAnRdb)
{
    std::vector<uint8_t> image(1024 * 512, 0);
    PutBe32(image, 0, 0x444F5301);
    auto info = HardfileInspector::InspectBytes(image, image.size());
    EXPECT_FALSE(info.hasRdb);
    EXPECT_EQ(info.rawDosType, 0x444F5301u);
    EXPECT_EQ(info.guessSectors, 32);
    EXPECT_EQ(info.guessSurfaces, 1);
    EXPECT_EQ(info.guessReserved, 2);

    // From 1 GB the core switches to sixteen surfaces.
    info = HardfileInspector::InspectBytes(image, 1024ull * 1024ull * 1024ull);
    EXPECT_EQ(info.guessSurfaces, 16);
}

TEST(HardfileInspectorTest, FindsAnRdbAfterBlockZero)
{
    auto image = BuildRdbImage();
    std::vector<uint8_t> shifted(image.size() + 512 * 3, 0);
    std::memcpy(shifted.data() + 512 * 3, image.data(), 512); // RDSK at block 3
    PutBe32(shifted, 3 * 512 + 0x1C, 0xFFFFFFFF);
    PutBe32(shifted, 3 * 512 + 0x20, 0xFFFFFFFF);
    SealBlock(shifted, 3, 64);
    const auto info = HardfileInspector::InspectBytes(shifted, shifted.size());
    EXPECT_TRUE(info.hasRdb);
    EXPECT_EQ(info.rdbBlock, 3u);
    EXPECT_TRUE(info.partitions.empty());
}

TEST(HardfileInspectorTest, DosTypeTextFallsBackToHex)
{
    EXPECT_EQ(HardfileInspector::DosTypeText(0x444F5300), "DOS0");
    EXPECT_EQ(HardfileInspector::DosTypeText(0x53465300), "SFS0");
    EXPECT_EQ(HardfileInspector::DosTypeText(0x00010203), "0x00010203");
}

TEST_F(HardfileInspectorFileTest, InspectsPlainAndCompressedFiles)
{
    const auto image = BuildRdbImage();
    const auto plain = root / "disk.hdf";
    {
        std::ofstream stream(plain, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
    }
    std::string error;
    auto info = HardfileInspector::Inspect(plain, error);
    ASSERT_TRUE(info.has_value()) << error;
    EXPECT_FALSE(info->compressed);
    EXPECT_EQ(info->sizeBytes, image.size());
    EXPECT_EQ(info->partitions.size(), 2u);

    const auto compressed = root / "disk.hdz";
    gzFile gz = gzopen(compressed.string().c_str(), "wb");
    ASSERT_NE(gz, nullptr);
    gzwrite(gz, image.data(), static_cast<unsigned>(image.size()));
    gzclose(gz);
    info = HardfileInspector::Inspect(compressed, error);
    ASSERT_TRUE(info.has_value()) << error;
    EXPECT_TRUE(info->compressed);
    EXPECT_EQ(info->sizeBytes, image.size());
    EXPECT_EQ(info->partitions.size(), 2u);

    EXPECT_FALSE(HardfileInspector::Inspect(root / "missing.hdf", error).has_value());

    // APP-10: the whole image back, and a stream cut short is damaged, not a shorter image
    std::vector<uint8_t> bytes;
    EXPECT_EQ(HardfileInspector::Gunzip(compressed, 1ull << 30, bytes, error),
              HardfileInspector::GunzipResult::Ok);
    EXPECT_EQ(bytes, image);
    EXPECT_EQ(HardfileInspector::Gunzip(compressed, image.size() - 1, bytes, error),
              HardfileInspector::GunzipResult::TooLarge);
    // a stream long enough to cut in the middle: 256 KB that does not compress away
    std::vector<uint8_t> noise(256 * 1024);
    uint32_t x = 0x12345678;
    for (auto& b : noise) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(x >> 24);
    }
    const auto cut = root / "cut.hdz";
    gz = gzopen(cut.string().c_str(), "wb");
    ASSERT_NE(gz, nullptr);
    gzwrite(gz, noise.data(), static_cast<unsigned>(noise.size()));
    gzclose(gz);
    std::filesystem::resize_file(cut, std::filesystem::file_size(cut) - 100);
    EXPECT_EQ(HardfileInspector::Gunzip(cut, 1ull << 30, bytes, error),
              HardfileInspector::GunzipResult::Damaged)
        << bytes.size() << " bytes, \"" << error << "\"";
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(bytes.empty());
    EXPECT_EQ(HardfileInspector::Gunzip(root / "missing.hdz", 1ull << 30, bytes, error),
              HardfileInspector::GunzipResult::CannotOpen);
}

} // namespace
