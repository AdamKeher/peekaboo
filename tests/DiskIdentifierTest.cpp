#include <gtest/gtest.h>
#include <zip.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "core/DiskIdentifier.h"
#include "core/MD5.h"

namespace {

void CreateDummyFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
}

void CreateZipWithAdf(const std::string& zipPath, const std::string& adfName,
                      const std::vector<uint8_t>& adfData)
{
    int error = 0;
    zip_t* archive = zip_open(zipPath.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!archive) {
        throw std::runtime_error("Failed to create zip");
    }

    zip_source_t* source = zip_source_buffer(archive, adfData.data(), adfData.size(), 0);
    if (!source) {
        zip_close(archive);
        throw std::runtime_error("Failed to create zip source");
    }

    if (zip_file_add(archive, adfName.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
        zip_source_free(source);
        zip_close(archive);
        throw std::runtime_error("Failed to add file to zip");
    }

    zip_close(archive);
}

} // namespace

TEST(DiskIdentifierTest, ComputeTosec_RegularFile)
{
    std::vector<uint8_t> data(512, 0xAA);
    std::string path = "test_regular.adf";
    CreateDummyFile(path, data);

    adm::core::DiskIdentifier identifier;
    auto result = identifier.ComputeTosec(path);

    std::string expectedMd5 = adm::core::MD5::Compute(data);
    EXPECT_EQ(result.md5, expectedMd5);
    EXPECT_EQ(result.effectiveFilename, "test_regular.adf");

    std::filesystem::remove(path);
}

TEST(DiskIdentifierTest, ComputeTosec_ZipFile)
{
    std::vector<uint8_t> data(512, 0xBB);
    std::string zipPath = "test_archive.zip";
    std::string adfName = "inner_game.adf";

    CreateZipWithAdf(zipPath, adfName, data);

    adm::core::DiskIdentifier identifier;
    auto result = identifier.ComputeTosec(zipPath);

    std::string expectedMd5 = adm::core::MD5::Compute(data);
    EXPECT_EQ(result.md5, expectedMd5);
    EXPECT_EQ(result.effectiveFilename, adfName);

    std::filesystem::remove(zipPath);
}

TEST(DiskIdentifierTest, ComputeTosec_ZipFile_CaseInsensitiveExt)
{
    std::vector<uint8_t> data(512, 0xCC);
    std::string zipPath = "test_archive_upper.ZIP";
    std::string adfName = "INNER_GAME.ADF";

    CreateZipWithAdf(zipPath, adfName, data);

    adm::core::DiskIdentifier identifier;
    auto result = identifier.ComputeTosec(zipPath);

    std::string expectedMd5 = adm::core::MD5::Compute(data);
    EXPECT_EQ(result.md5, expectedMd5);
    // effectiveFilename comes from zip_get_name which should preserve case
    EXPECT_EQ(result.effectiveFilename, adfName);

    std::filesystem::remove(zipPath);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hashing a disk that has no file behind it
//
// A downloaded document is mounted straight from memory: its currentFilePath is a
// display name, not a path. Identification used to open it to hash it, so every
// such disk died on "Failed to compute SHA1 hash" before any lookup ran.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// A path that cannot exist, standing in for a download's display name.
const char* kPhantomPath = "3D Demo II (1992-12)(Anarchy)[TP#2].zip";

std::shared_ptr<const std::vector<uint8_t>> Bytes(std::vector<uint8_t> v)
{
    return std::make_shared<const std::vector<uint8_t>>(std::move(v));
}

std::vector<uint8_t> ZipContaining(const std::string& entryName, const std::vector<uint8_t>& data)
{
    zip_error_t ze;
    zip_error_init(&ze);
    zip_source_t* src = zip_source_buffer_create(nullptr, 0, 0, &ze);
    EXPECT_NE(src, nullptr);
    zip_source_keep(src);
    zip_t* archive = zip_open_from_source(src, ZIP_TRUNCATE, &ze);
    EXPECT_NE(archive, nullptr);
    zip_source_t* s = zip_source_buffer(archive, data.data(), data.size(), 0);
    EXPECT_GE(zip_file_add(archive, entryName.c_str(), s, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8), 0);
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

} // namespace

// The failure as reported: no file, no bytes, no hash.
TEST(DiskIdentifierHashTest, PathThatDoesNotExistHasNoSha1)
{
    adm::core::DiskIdentifier id;
    EXPECT_TRUE(id.ComputeSHA1(kPhantomPath).empty());
}

TEST(DiskIdentifierHashTest, SourceBytesGiveTheDiskAnSha1)
{
    std::vector<uint8_t> raw(4096);
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<uint8_t>((i * 31 + 7) & 0xff);

    adm::core::DiskIdentifier id;
    id.SetSourceBytes(Bytes(raw));

    const std::string sha1 = id.ComputeSHA1(kPhantomPath);
    ASSERT_EQ(sha1.size(), 40u);
    // Same bytes through the file route must agree, so the two are interchangeable.
    const std::string tmp =
        (std::filesystem::temp_directory_path() / "adm_identify_hash_test.bin").string();
    {
        std::ofstream out(tmp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(raw.data()), raw.size());
    }
    adm::core::DiskIdentifier viaFile;
    EXPECT_EQ(viaFile.ComputeSHA1(tmp), sha1);
    std::filesystem::remove(tmp);
}

TEST(DiskIdentifierHashTest, EmptySourceBytesStillFail)
{
    adm::core::DiskIdentifier id;
    id.SetSourceBytes(Bytes({}));
    EXPECT_TRUE(id.ComputeSHA1(kPhantomPath).empty());
}

// TOSEC hashes the file inside the archive, so the MD5 has to be of the member -
// this is what decides whether a downloaded disk is recognised at all.
TEST(DiskIdentifierHashTest, Md5OfSourceBytesReachesIntoTheZip)
{
    std::vector<uint8_t> member(2048);
    for (size_t i = 0; i < member.size(); ++i)
        member[i] = static_cast<uint8_t>((i * 17 + 3) & 0xff);
    const std::string entry = "3D Demo II (1992-12)(Anarchy)[TP#2].exe";

    adm::core::DiskIdentifier id;
    id.SetSourceBytes(Bytes(ZipContaining(entry, member)));

    std::string effectiveName;
    const std::string md5 = id.ComputeMD5(kPhantomPath, &effectiveName);
    EXPECT_EQ(md5, adm::core::MD5::Compute(member));
    EXPECT_EQ(effectiveName, entry);
}

// A disk that is not an archive is hashed whole, and reports the display name.
TEST(DiskIdentifierHashTest, Md5OfBareSourceBytesHashesThemDirectly)
{
    std::vector<uint8_t> raw(1024, 0x5A);

    adm::core::DiskIdentifier id;
    id.SetSourceBytes(Bytes(raw));

    std::string effectiveName;
    EXPECT_EQ(id.ComputeMD5(kPhantomPath, &effectiveName), adm::core::MD5::Compute(raw));
    EXPECT_EQ(effectiveName, kPhantomPath);
}
