#include <gtest/gtest.h>

#include "core/HostDirectoryDocument.h"

#include <chrono>
#include <fstream>

using namespace adm::core;

namespace {

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        path = std::filesystem::temp_directory_path() /
               ("adm-host-document-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

void WriteFile(const std::filesystem::path& path, const std::string& contents)
{
    std::ofstream output(path, std::ios::binary);
    output << contents;
}

} // namespace

TEST(HostDirectoryEntryId, RejectsTraversalAndAbsolutePaths)
{
    std::string error;
    EXPECT_FALSE(DocumentEntryId::RelativePath("../escape", error));
    EXPECT_FALSE(DocumentEntryId::RelativePath(std::filesystem::current_path(), error));
    auto valid = DocumentEntryId::RelativePath("drawer/file.txt", error);
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->value, "drawer/file.txt");
    EXPECT_EQ(DocumentEntryId::AdfBlock(42).value, "adf:42");
}

TEST(HostDirectoryDocument, ScansRecursivelyHidesSidecarsAndReportsCollisions)
{
    TemporaryDirectory temp;
    std::filesystem::create_directory(temp.path / "Drawer");
    WriteFile(temp.path / "Drawer" / "ReadMe", "one");
#ifndef _WIN32
    WriteFile(temp.path / "Drawer" / "README", "two");
#endif
    WriteFile(temp.path / "_uaefsdb.___", "metadata");

    HostDirectoryDocument document;
    std::string error;
    ASSERT_TRUE(document.Open(temp.path, true, error));
    const auto& entries = document.GetScan().entries;
    EXPECT_EQ(entries.size(),
#ifdef _WIN32
              2u
#else
              3u
#endif
    );
#ifndef _WIN32
    EXPECT_EQ(std::count_if(entries.begin(), entries.end(),
                            [](const auto& entry) { return entry.amigaNameCollision; }),
              2);
    EXPECT_EQ(document.GetScan().fingerprint.entryCount, 3u);
#else
    EXPECT_EQ(document.GetScan().fingerprint.entryCount, 2u);
#endif
}

TEST(HostDirectoryDocument, EnforcesReadOnlyCapabilitiesAndMutations)
{
    TemporaryDirectory temp;
    HostDirectoryDocument readOnly;
    std::string error;
    ASSERT_TRUE(readOnly.Open(temp.path, true, error));
    EXPECT_FALSE(readOnly.GetCapabilities().createDirectory);
    EXPECT_FALSE(readOnly.CreateDirectory({}, "Nope", error));
    EXPECT_NE(error.find("read-only"), std::string::npos);

    HostDirectoryDocument writable;
    ASSERT_TRUE(writable.Open(temp.path, false, error));
    EXPECT_TRUE(writable.GetCapabilities().createDirectory);
    ASSERT_TRUE(writable.CreateDirectory({}, "New Drawer", error));
    ASSERT_TRUE(writable.RefreshNow(error));
    EXPECT_TRUE(std::filesystem::is_directory(temp.path / "New Drawer"));
}

TEST(HostDirectoryDocument, CreatesFilesAndMovesEntriesWithOpaqueIds)
{
    TemporaryDirectory temp;
    HostDirectoryDocument document;
    std::string error;
    ASSERT_TRUE(document.Open(temp.path, false, error));
    ASSERT_TRUE(document.CreateDirectory({}, "Destination", error));
    ASSERT_TRUE(document.CreateHostFile({}, "hello.txt", {'h', 'i'}, error));
    ASSERT_TRUE(document.RefreshNow(error));

    auto file = DocumentEntryId::RelativePath("hello.txt", error);
    auto destination = DocumentEntryId::RelativePath("Destination", error);
    ASSERT_TRUE(file);
    ASSERT_TRUE(destination);
    ASSERT_TRUE(document.Move(*file, *destination, error));
    EXPECT_FALSE(std::filesystem::exists(temp.path / "hello.txt"));
    EXPECT_TRUE(std::filesystem::exists(temp.path / "Destination" / "hello.txt"));
}

TEST(HostDirectoryDocument, DoesNotResolveDirectorySymlinkOutsideRoot)
{
    TemporaryDirectory root;
    TemporaryDirectory outside;
    WriteFile(outside.path / "secret", "secret");
    std::error_code errorCode;
    std::filesystem::create_directory_symlink(outside.path, root.path / "outside-link", errorCode);
    if (errorCode)
        GTEST_SKIP() << "Directory symlinks unavailable: " << errorCode.message();

    HostDirectoryDocument document;
    std::string error;
    ASSERT_TRUE(document.Open(root.path, true, error));
    auto id = DocumentEntryId::RelativePath("outside-link/secret", error);
    ASSERT_TRUE(id);
    EXPECT_FALSE(document.Resolve(*id, error));
    EXPECT_NE(error.find("outside"), std::string::npos);
}

TEST(DirectoryFingerprint, RoundTrips)
{
    DirectoryFingerprint original{12, 3456, 789, 123456789};
    auto parsed = DirectoryFingerprint::Parse(original.ToString());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, original);
    EXPECT_FALSE(DirectoryFingerprint::Parse("broken"));
}
