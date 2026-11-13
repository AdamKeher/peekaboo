#include <gtest/gtest.h>
#include <zip.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include "core/AdfWrapper.h"
#include "core/AmigaEmulator.h"
#include "core/MemoryDevice.h"
#include "core/ZipUtils.h"

// Basic tests for MemoryDevice open from vector
TEST(MemoryDeviceTest, OpenFromVector)
{
    adm::core::MemoryDevice memDevice;

    // Create a dummy buffer (not a valid ADF, but enough to open device)
    // 512 bytes * 1760 blocks = 901120 bytes for standard DD
    std::vector<uint8_t> buffer(901120, 0);

    // Mark a "boot block" sector
    buffer[0] = 'D';
    buffer[1] = 'O';
    buffer[2] = 'S';
    buffer[3] = 0;

    auto* dev = memDevice.Open(buffer);
    ASSERT_NE(dev, nullptr);
    EXPECT_TRUE(memDevice.IsOpen());
    EXPECT_EQ(memDevice.GetTotalBlocks(), 1760u);

    // Verify content read
    std::vector<uint8_t> readBack = memDevice.ReadBlockFromDisk(0);
    ASSERT_EQ(readBack.size(), 512u);
    EXPECT_EQ(readBack[0], 'D');
    EXPECT_EQ(readBack[1], 'O');
    EXPECT_EQ(readBack[2], 'S');
}

TEST(MemoryDeviceTest, OpenFromEmptyVectorFails)
{
    adm::core::MemoryDevice memDevice;
    std::vector<uint8_t> buffer;

    auto* dev = memDevice.Open(buffer);
    EXPECT_EQ(dev, nullptr);
    EXPECT_FALSE(memDevice.IsOpen());
}

TEST(MemoryDeviceTest, RevertBlockFromMemory)
{
    adm::core::MemoryDevice memDevice;
    std::vector<uint8_t> buffer(512 * 2, 0); // 2 blocks

    // Initialize block 1 with some data
    std::fill(buffer.begin() + 512, buffer.end(), 0xAA);

    memDevice.Open(buffer);
    ASSERT_TRUE(memDevice.IsOpen());

    // Modify block 1
    uint8_t newData[512];
    std::memset(newData, 0xBB, 512);
    memDevice.WriteSector(1, 1, newData);

    // Verify modification
    EXPECT_TRUE(memDevice.IsBlockModified(1));

    // Read back via sector read
    uint8_t readBuf[512];
    memDevice.ReadSector(1, 1, readBuf);
    EXPECT_EQ(readBuf[0], 0xBB);

    // Revert
    EXPECT_TRUE(memDevice.RevertBlock(1));
    EXPECT_FALSE(memDevice.IsBlockModified(1));

    // Read back - should be original 0xAA
    memDevice.ReadSector(1, 1, readBuf);
    EXPECT_EQ(readBuf[0], 0xAA);
}

// Tests for AdfHandler mounting from vector
TEST(AdfHandlerTest, MountFromVector)
{
    // We need a valid (or at least valid-enough) ADF image to mount
    // or else AdfWrapper::Mount might fail internally on adfVolMount
    // But for this test, we just want to ensure the plumbing works.

    // Creating a minimal valid boot block + root block structure is complex manually.
    // However, AdfHandler::Mount returns true even if volume mount fails (returns raw device).

    adm::core::AdfHandler handler;
    std::vector<uint8_t> buffer(901120, 0);

    bool result = handler.Mount(buffer);
    EXPECT_TRUE(result);
    EXPECT_TRUE(handler.IsMounted());

    // Verify we can read raw blocks
    auto block = handler.ReadBlock(0);
    EXPECT_EQ(block.size(), 512u);
}

TEST(AdfHandlerTest, GetFileContentReturnsData)
{
    adm::core::AdfHandler handler;
    std::vector<uint8_t> buffer(1024, 0xCC); // Small buffer

    handler.Mount(buffer);

    const auto& content = handler.GetFileContent();
    ASSERT_EQ(content.size(), 1024u);
    EXPECT_EQ(content[0], 0xCC);
}

// ─────────────────────────────────────────────────────────────────────────────
// ZIP member selection
//
// TOSEC wraps exactly one file per archive and it is frequently not an ADF: the
// "Demos - Various - [EXE]" category ships a HUNK executable and thousands of
// entries are IPF. Extraction used to look for ".adf" only, so those downloads
// failed with "No ADF file found in ZIP archive".
// ─────────────────────────────────────────────────────────────────────────────

namespace {

using ZipEntries = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

// Builds an in-memory ZIP. Names ending in '/' are added as directory entries.
std::vector<uint8_t> MakeZip(const ZipEntries& entries)
{
    zip_error_t ze;
    zip_error_init(&ze);

    zip_source_t* src = zip_source_buffer_create(nullptr, 0, 0, &ze);
    EXPECT_NE(src, nullptr);
    zip_source_keep(src); // survives zip_close so we can read the result back

    zip_t* archive = zip_open_from_source(src, ZIP_TRUNCATE, &ze);
    EXPECT_NE(archive, nullptr);

    for (const auto& [name, data] : entries) {
        if (!name.empty() && name.back() == '/') {
            EXPECT_GE(zip_dir_add(archive, name.substr(0, name.size() - 1).c_str(), 0), 0);
            continue;
        }
        // freep = 0: the caller's buffers must outlive zip_close, and they do.
        zip_source_t* s = zip_source_buffer(archive, data.data(), data.size(), 0);
        EXPECT_NE(s, nullptr);
        EXPECT_GE(zip_file_add(archive, name.c_str(), s, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8), 0);
    }
    EXPECT_EQ(zip_close(archive), 0);

    zip_source_open(src);
    zip_source_seek(src, 0, SEEK_END);
    zip_int64_t size = zip_source_tell(src);
    zip_source_seek(src, 0, SEEK_SET);
    std::vector<uint8_t> out(static_cast<size_t>(std::max<zip_int64_t>(size, 0)));
    if (!out.empty())
        zip_source_read(src, out.data(), out.size());
    zip_source_close(src);
    zip_source_free(src);
    zip_error_fini(&ze);
    return out;
}

// A HUNK executable header is all ConvertToAdfBytes looks at to route a member.
std::vector<uint8_t> FakeExecutable(size_t size = 4096)
{
    std::vector<uint8_t> exe(size, 0x11);
    exe[0] = 0x00;
    exe[1] = 0x00;
    exe[2] = 0x03;
    exe[3] = 0xF3;
    return exe;
}

} // namespace

TEST(ZipPayloadTest, ExecutableOnlyArchiveIsExtractedAsAnyDiskImage)
{
    // The exact shape of the 3D Demo II download that failed.
    const std::string entry = "3D Demo II (1992-12)(Anarchy)[TP#2].exe";
    auto zipBytes = MakeZip({{entry, FakeExecutable()}});

    std::vector<uint8_t> out;
    std::string name, error;
    ASSERT_TRUE(
        adm::ExtractFromZipBuffer(zipBytes, adm::ZipPayload::AnyDiskImage, out, name, error))
        << error;
    EXPECT_EQ(name, entry);
    EXPECT_EQ(out.size(), 4096u);
    EXPECT_EQ(out[3], 0xF3);
}

TEST(ZipPayloadTest, ExecutableOnlyArchiveIsRejectedAsAdfOnly)
{
    auto zipBytes = MakeZip({{"demo.exe", FakeExecutable()}});

    std::vector<uint8_t> out;
    std::string name, error;
    EXPECT_FALSE(adm::ExtractFromZipBuffer(zipBytes, adm::ZipPayload::AdfOnly, out, name, error));
    EXPECT_EQ(error, "No ADF file found in ZIP archive");
}

TEST(ZipPayloadTest, IpfMemberIsExtracted)
{
    auto zipBytes =
        MakeZip({{"Some Game (1991)(Team 17)[cr].ipf", std::vector<uint8_t>(64, 0x5A)}});

    std::vector<uint8_t> out;
    std::string name, error;
    ASSERT_TRUE(
        adm::ExtractFromZipBuffer(zipBytes, adm::ZipPayload::AnyDiskImage, out, name, error))
        << error;
    EXPECT_EQ(name, "Some Game (1991)(Team 17)[cr].ipf");
    EXPECT_EQ(out.size(), 64u);
}

TEST(ZipPayloadTest, AdfWinsOverOtherMembers)
{
    // Preference order, not archive order: the .exe is listed first.
    auto zipBytes = MakeZip({{"readme.exe", FakeExecutable(16)},
                             {"disk.ipf", std::vector<uint8_t>(16, 0x01)},
                             {"disk.adf", std::vector<uint8_t>(32, 0x02)}});

    std::vector<uint8_t> out;
    std::string name, error;
    ASSERT_TRUE(
        adm::ExtractFromZipBuffer(zipBytes, adm::ZipPayload::AnyDiskImage, out, name, error))
        << error;
    EXPECT_EQ(name, "disk.adf");
    EXPECT_EQ(out.size(), 32u);
}

TEST(ZipPayloadTest, DirectoryEntriesAreSkipped)
{
    auto zipBytes = MakeZip({{"Demos/", {}}, {"Demos/disk.adf", std::vector<uint8_t>(24, 0x03)}});

    std::vector<uint8_t> out;
    std::string name, error;
    ASSERT_TRUE(
        adm::ExtractFromZipBuffer(zipBytes, adm::ZipPayload::AnyDiskImage, out, name, error))
        << error;
    EXPECT_EQ(name, "Demos/disk.adf");
    EXPECT_EQ(out.size(), 24u);
}

TEST(ZipPayloadTest, LoneUnknownMemberIsTakenAnyway)
{
    // Content detection downstream gets the final say; refusing on the name alone
    // would drop archives whose single member has an odd or missing extension.
    auto zipBytes = MakeZip({{"kickstart", std::vector<uint8_t>(48, 0x04)}});

    std::vector<uint8_t> out;
    std::string name, error;
    ASSERT_TRUE(
        adm::ExtractFromZipBuffer(zipBytes, adm::ZipPayload::AnyDiskImage, out, name, error))
        << error;
    EXPECT_EQ(name, "kickstart");
    EXPECT_EQ(out.size(), 48u);
}

TEST(ZipPayloadTest, SeveralUnknownMembersAreRejected)
{
    auto zipBytes = MakeZip({{"readme.txt", std::vector<uint8_t>(8, 0x05)},
                             {"cover.jpg", std::vector<uint8_t>(8, 0x06)}});

    std::vector<uint8_t> out;
    std::string name, error;
    EXPECT_FALSE(
        adm::ExtractFromZipBuffer(zipBytes, adm::ZipPayload::AnyDiskImage, out, name, error));
    EXPECT_EQ(error, "No Amiga disk image or executable found in ZIP archive");
}

// ─────────────────────────────────────────────────────────────────────────────
// Bootable ADF from a TOSEC-named executable
// ─────────────────────────────────────────────────────────────────────────────

TEST(BootableAdfTest, TosecNameIsTrimmedToAnAmigaFilename)
{
    // 39 characters, well past AmigaDOS's 30 — adfFileOpen refuses it verbatim.
    const std::string tosecName = "3D Demo II (1992-12)(Anarchy)[TP#2].exe";

    auto adfBytes = adm::AmigaEmulator::CreateBootableADF(FakeExecutable(20000), tosecName);
    ASSERT_FALSE(adfBytes.empty());

    adm::core::AdfHandler disk;
    ASSERT_TRUE(disk.Mount(adfBytes));

    auto tree = disk.GetFileTree();
    const adm::core::FileNode* program = nullptr;
    const adm::core::FileNode* sDir = nullptr;
    for (const auto& node : tree.children) {
        if (node.isDir && node.name == "S")
            sDir = &node;
        else if (!node.isDir)
            program = &node;
    }

    ASSERT_NE(program, nullptr);
    EXPECT_LE(program->name.size(), 30u);
    EXPECT_EQ(program->name, "3D Demo II (1992-12)(Anarc.exe");

    // The startup-sequence has to quote it, or AmigaDOS runs "3D" with arguments.
    ASSERT_NE(sDir, nullptr);
    const adm::core::FileNode* startup = nullptr;
    for (const auto& child : sDir->children) {
        if (child.name == "startup-sequence")
            startup = &child;
    }
    ASSERT_NE(startup, nullptr);
    EXPECT_EQ(disk.ReadFileContent(startup->startSector), "\"3D Demo II (1992-12)(Anarc.exe\"\n");
}
