#include <gtest/gtest.h>

#include "core/SessionSerializer.h"

#include <zip.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using namespace adm::core;

namespace {

std::filesystem::path TemporaryPeekPath()
{
    return std::filesystem::temp_directory_path() /
           ("adm-session-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".peek");
}

// A .peek written by hand: each entry is (name, text).
void WritePeek(const std::filesystem::path& path,
               const std::vector<std::pair<std::string, std::string>>& entries)
{
    int err = 0;
    zip_t* archive = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    ASSERT_NE(archive, nullptr);
    for (const auto& [name, text] : entries) {
        zip_source_t* s = zip_source_buffer(archive, text.data(), text.size(), 0);
        ASSERT_NE(s, nullptr);
        ASSERT_GE(zip_file_add(archive, name.c_str(), s, ZIP_FL_OVERWRITE), 0);
    }
    ASSERT_EQ(zip_close(archive), 0);
}

} // namespace

// APP-04: a wrong type in the manifest or a document fails the open with an error, no throw
TEST(SessionSerializer, WrongTypesAreRefusedNotThrown)
{
    const auto path = TemporaryPeekPath();
    const std::string goodManifest = R"({"format": "adm.peek", "version": 2})";
    const std::vector<std::vector<std::pair<std::string, std::string>>> cases = {
        {{"manifest.json", "[]"}},
        {{"manifest.json", R"({"format": "adm.peek", "version": "2"})"}},
        {{"manifest.json", goodManifest},
         {"documents/1/doc.json", R"({"id": 1, "sourceKind": "hardfile", "partition": "1"})"}},
        {{"manifest.json", R"({"format": "adm.peek", "version": 2, "bridgedSlots":
                              [{"drive": 0, "autoCache": 1}]})"}},
    };
    for (const auto& entries : cases) {
        WritePeek(path, entries);
        PeekSession restored;
        std::string error;
        bool ok = true;
        EXPECT_NO_THROW(ok = SessionSerializer::Read(path.string(), restored, error))
            << entries.back().second;
        EXPECT_FALSE(ok) << entries.back().second;
        EXPECT_FALSE(error.empty());
    }
    std::filesystem::remove(path);
}

// APP-06: the bridge settings index the Floppy Drives window's tables
TEST(SessionSerializer, BridgedSlotSettingsAreClamped)
{
    const auto path = TemporaryPeekPath();
    WritePeek(path, {{"manifest.json", R"({"format": "adm.peek", "version": 2, "bridgedSlots":
        [{"drive": 1, "bridgeMode": 40, "bridgeDensity": -3, "driveCable": 99}]})"}});
    PeekSession restored;
    std::string error;
    ASSERT_TRUE(SessionSerializer::Read(path.string(), restored, error)) << error;
    ASSERT_EQ(restored.bridgedSlots.size(), 1u);
    EXPECT_EQ(restored.bridgedSlots[0].bridgeMode, 3);
    EXPECT_EQ(restored.bridgedSlots[0].bridgeDensity, 0);
    EXPECT_EQ(restored.bridgedSlots[0].driveCable, 5);
    std::filesystem::remove(path);
}

// APP-07: a restored read-write hardfile keeps its writes only as the user's own mount
TEST(SessionSerializer, RestoredHardfileWritesOnlyAsTheUsersOwnMount)
{
    const auto dir = std::filesystem::temp_directory_path() /
                     ("adm-session-hdf-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "mine.hdf") << "x";
    std::ofstream(dir / "thesis.docx") << "y";

    HostDirectoryMount restored;
    restored.enabled = true;
    restored.backingType = HostMountBackingType::HardfileImage;
    restored.hardfilePath = dir / "mine.hdf";
    restored.readOnly = false;
    HostDirectoryMount configured = restored;

    EXPECT_TRUE(RestoredHardfileMayWrite(restored, configured));
    configured.readOnly = true; // the user mounts it read-only
    EXPECT_FALSE(RestoredHardfileMayWrite(restored, configured));
    configured.readOnly = false;
    configured.enabled = false; // the slot is not in use
    EXPECT_FALSE(RestoredHardfileMayWrite(restored, configured));
    configured.enabled = true;
    restored.hardfilePath = dir / "thesis.docx"; // any other file
    EXPECT_FALSE(RestoredHardfileMayWrite(restored, configured));
    EXPECT_FALSE(RestoredHardfileMayWrite(restored, HostDirectoryMount{}));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(SessionSerializer, RejectsEntriesWithOversizedDeclaredLength)
{
    const auto path = TemporaryPeekPath();
    PeekSession source;
    source.version = 2;

    std::string error;
    ASSERT_TRUE(SessionSerializer::Write(path.string(), source, error)) << error;

    // Forge the archive: a .peek file's central directory declares each entry's uncompressed
    // size, and ReadEntry allocates whatever it claims. Patch manifest.json's declared size to
    // ~4GB (not 0xFFFFFFFF — that would mean ZIP64) and the read must be refused, not attempted.
    std::vector<char> bytes;
    {
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.good());
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string sig = "PK\x01\x02";
    const std::string target = "manifest.json";
    bool patched = false;
    for (size_t i = 0; i + 46 + target.size() <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, sig.data(), sig.size()) != 0)
            continue;
        if (std::memcmp(bytes.data() + i + 46, target.data(), target.size()) != 0)
            continue;
        // Uncompressed size lives at offset 24 of the central-directory file header.
        bytes[i + 24] = static_cast<char>(0xF0);
        bytes[i + 25] = static_cast<char>(0xFF);
        bytes[i + 26] = static_cast<char>(0xFF);
        bytes[i + 27] = static_cast<char>(0xFF);
        patched = true;
        break;
    }
    ASSERT_TRUE(patched) << "manifest.json central-directory record not found";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    PeekSession restored;
    EXPECT_FALSE(SessionSerializer::Read(path.string(), restored, error));

    std::filesystem::remove(path);
}

TEST(SessionSerializer, VersionTwoStoresDirectoryReferencesWithoutContents)
{
    const auto path = TemporaryPeekPath();
    PeekSession source;
    source.version = 2;
    source.activeDocumentId = 7;
    PeekDocument document;
    document.id = 7;
    document.name = "Work";
    document.kind = DocumentKind::HostDirectory;
    document.originalPath = "original/work";
    document.canonicalPath = "canonical/work";
    document.assignedDevice = HostMountDevice::DH1;
    document.volumeLabel = "Work";
    document.readOnly = false;
    document.directoryFingerprint = {4, 100, 200, 300};
    document.windowState = {
        {"x", 12.5f}, {"y", 22.5f}, {"width", 640.0f}, {"height", 480.0f}, {"dockId", 42u}};
    source.documents.push_back(document);

    PeekMountReference reference;
    reference.descriptor.device = HostMountDevice::DH1;
    reference.descriptor.enabled = true;
    reference.descriptor.hostDirectory = document.canonicalPath;
    reference.descriptor.volumeLabel = document.volumeLabel;
    reference.descriptor.readOnly = false;
    reference.descriptor.bootPriority = -5;
    reference.documentId = document.id;
    reference.originalPath = document.originalPath;
    reference.directoryFingerprint = document.directoryFingerprint;
    source.hostMounts[1] = reference;

    std::string error;
    ASSERT_TRUE(SessionSerializer::Write(path.string(), source, error)) << error;
    PeekSession restored;
    ASSERT_TRUE(SessionSerializer::Read(path.string(), restored, error)) << error;
    ASSERT_EQ(restored.version, 2);
    ASSERT_EQ(restored.documents.size(), 1u);
    EXPECT_EQ(restored.documents[0].kind, DocumentKind::HostDirectory);
    EXPECT_TRUE(restored.documents[0].adfBytes.empty());
    EXPECT_EQ(restored.documents[0].directoryFingerprint, document.directoryFingerprint);
    EXPECT_EQ(restored.documents[0].windowState, document.windowState);
    ASSERT_TRUE(restored.hostMounts[1]);
    EXPECT_EQ(restored.hostMounts[1]->descriptor.device, HostMountDevice::DH1);
    EXPECT_EQ(restored.hostMounts[1]->descriptor.source, HostMountSource::RestoredSession);
    EXPECT_EQ(restored.hostMounts[1]->documentId, 7u);

    std::filesystem::remove(path);
}

TEST(SessionSerializer, VersionTwoCarriesHardfileMountsAndControllerRoms)
{
    const auto path = TemporaryPeekPath();
    PeekSession source;
    source.version = 2;
    PeekMountReference reference;
    reference.descriptor.device = HostMountDevice::DH0;
    reference.descriptor.enabled = true;
    reference.descriptor.backingType = HostMountBackingType::HardfileImage;
    reference.descriptor.hardfilePath = "C:/images/workbench-311.hdf";
    reference.descriptor.controller = HardDriveController::ScsiA590A2091;
    reference.descriptor.controllerUnit = 2;
    reference.descriptor.geometry.mode = HardfileGeometryMode::Manual;
    reference.descriptor.geometry.sectors = 63;
    reference.descriptor.geometry.surfaces = 16;
    reference.descriptor.readOnly = false;
    reference.descriptor.bootPriority = 0;
    reference.originalPath = "C:/images/workbench-311.hdf";
    source.hostMounts[0] = reference;
    source.hardDriveControllerRoms.a590A2091 = "C:/roms/amiga-boot-a590.rom";
    source.cdImage = "C:/cd32/Chuck Rock II (1994)(Core)[!].7z";

    std::string error;
    ASSERT_TRUE(SessionSerializer::Write(path.string(), source, error)) << error;
    PeekSession restored;
    ASSERT_TRUE(SessionSerializer::Read(path.string(), restored, error)) << error;
    ASSERT_TRUE(restored.hostMounts[0]);
    const auto& mount = restored.hostMounts[0]->descriptor;
    EXPECT_TRUE(mount.IsHardfile());
    EXPECT_EQ(mount.hardfilePath, source.hostMounts[0]->descriptor.hardfilePath);
    EXPECT_EQ(mount.controller, HardDriveController::ScsiA590A2091);
    EXPECT_EQ(mount.controllerUnit, 2);
    EXPECT_EQ(mount.geometry, reference.descriptor.geometry);
    EXPECT_FALSE(mount.readOnly);
    EXPECT_FALSE(restored.hostMounts[1]);
    EXPECT_EQ(restored.hardDriveControllerRoms, source.hardDriveControllerRoms);
    EXPECT_EQ(restored.cdImage, source.cdImage);

    std::filesystem::remove(path);
}

TEST(SessionSerializer, HardfileDocumentsAreReferencesWithTheirPartition)
{
    const auto path = TemporaryPeekPath();
    PeekSession source;
    source.version = 2;
    PeekDocument document;
    document.id = 3;
    document.name = "workbench-311.hdf";
    document.originalPath = "C:/images/workbench-311.hdf";
    document.sourceKind = "hardfile";
    document.partition = 1;
    document.writeProtected = false;
    source.documents.push_back(document);

    std::string error;
    ASSERT_TRUE(SessionSerializer::Write(path.string(), source, error)) << error;
    PeekSession restored;
    ASSERT_TRUE(SessionSerializer::Read(path.string(), restored, error)) << error;
    ASSERT_EQ(restored.documents.size(), 1u);
    const auto& doc = restored.documents[0];
    EXPECT_EQ(doc.sourceKind, "hardfile");
    EXPECT_EQ(doc.originalPath, document.originalPath);
    EXPECT_EQ(doc.partition, 1);
    EXPECT_FALSE(doc.writeProtected);
    EXPECT_TRUE(doc.adfBytes.empty());

    std::filesystem::remove(path);
}

TEST(SessionSerializer, StillReadsVersionOneDiskDocuments)
{
    const auto path = TemporaryPeekPath();
    PeekSession source;
    source.version = 1;
    PeekDocument document;
    document.id = 3;
    document.name = "Disk";
    document.adfBytes = {1, 2, 3, 4};
    source.documents.push_back(document);

    std::string error;
    ASSERT_TRUE(SessionSerializer::Write(path.string(), source, error)) << error;
    PeekSession restored;
    ASSERT_TRUE(SessionSerializer::Read(path.string(), restored, error)) << error;
    ASSERT_EQ(restored.version, 1);
    ASSERT_EQ(restored.documents.size(), 1u);
    EXPECT_EQ(restored.documents[0].kind, DocumentKind::DiskImage);
    EXPECT_EQ(restored.documents[0].adfBytes, document.adfBytes);
    EXPECT_FALSE(restored.hostMounts[0]);
    EXPECT_FALSE(restored.hostMounts[1]);

    std::filesystem::remove(path);
}

// The frame-history timeline rides along in the .peek archive so a rewind context can
// survive the session, but only when the user has opted in - hence the "absent by default"
// half of this.
TEST(SessionSerializer, CarriesTheFrameHistoryTimelineWhenPresent)
{
    const auto path = TemporaryPeekPath();

    PeekSession source;
    source.version = 2;
    source.snapshot = {1, 2, 3, 4, 5, 6, 7, 8};
    source.snapshotPresent = true;
    source.frameHistory = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33};
    source.frameHistoryPresent = true;

    std::string error;
    ASSERT_TRUE(SessionSerializer::Write(path.string(), source, error)) << error;

    PeekSession restored;
    ASSERT_TRUE(SessionSerializer::Read(path.string(), restored, error)) << error;
    EXPECT_TRUE(restored.frameHistoryPresent);
    EXPECT_EQ(restored.frameHistory, source.frameHistory);
    EXPECT_EQ(restored.snapshot, source.snapshot);

    std::filesystem::remove(path);
}

TEST(SessionSerializer, OmitsTheFrameHistoryTimelineWhenNotOptedIn)
{
    const auto path = TemporaryPeekPath();

    PeekSession source;
    source.version = 2;
    source.snapshot = {9, 9, 9, 9};
    source.snapshotPresent = true;
    // Recorded, but the user did not ask for it to be saved.
    source.frameHistory = {1, 2, 3, 4};
    source.frameHistoryPresent = false;

    std::string error;
    ASSERT_TRUE(SessionSerializer::Write(path.string(), source, error)) << error;

    PeekSession restored;
    ASSERT_TRUE(SessionSerializer::Read(path.string(), restored, error)) << error;
    EXPECT_FALSE(restored.frameHistoryPresent);
    EXPECT_TRUE(restored.frameHistory.empty());
    EXPECT_EQ(restored.snapshot, source.snapshot);

    std::filesystem::remove(path);
}
