#include "SessionSerializer.h"

#include "HostMountJson.h"

#include "Constants.h"
#include "FloppyBridgeLimits.h"
#include "Logger.h"

#include <zip.h>
#include <ctime>
#include <deque>
#include <optional>

namespace adm::core {

namespace {

std::string DocPrefix(uint32_t id) { return "documents/" + std::to_string(id) + "/"; }

std::string PathUtf8(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

nlohmann::json MountJson(const PeekMountReference& reference)
{
    const auto& mount = reference.descriptor;
    nlohmann::json json = {{"device", HostMountDeviceName(mount.device)},
                           {"enabled", mount.enabled},
                           {"canonicalPath", PathUtf8(mount.hostDirectory)},
                           {"originalPath", reference.originalPath},
                           {"volumeLabel", mount.volumeLabel},
                           {"readOnly", mount.readOnly},
                           {"bootPriority", mount.bootPriority},
                           {"documentId", reference.documentId},
                           {"fingerprint", reference.directoryFingerprint.ToString()}};
    WriteHardfileFields(json, mount);
    return json;
}

std::optional<HostMountDevice> ParseDevice(const nlohmann::json& value)
{
    if (value.is_number_integer()) {
        const int raw = value.get<int>();
        if (raw == 0 || raw == 1)
            return static_cast<HostMountDevice>(raw);
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text == "DH0")
            return HostMountDevice::DH0;
        if (text == "DH1")
            return HostMountDevice::DH1;
    }
    return std::nullopt;
}

std::optional<PeekMountReference> ParseMount(const nlohmann::json& json, HostMountDevice expected)
{
    if (!json.is_object())
        return std::nullopt;
    const std::string backing = json.value("backingType", std::string("directory"));
    if (backing != "directory" && backing != "hardfile" && backing != "archive")
        return std::nullopt;
    auto device = json.contains("device") ? ParseDevice(json["device"]) : expected;
    if (!device || *device != expected)
        return std::nullopt;
    PeekMountReference result;
    result.descriptor.device = *device;
    result.descriptor.enabled = json.value("enabled", false);
    result.descriptor.hostDirectory =
        std::filesystem::u8path(json.value("canonicalPath", std::string{}));
    result.descriptor.volumeLabel =
        json.value("volumeLabel", std::string(HostMountDeviceName(*device)));
    result.descriptor.readOnly = json.value("readOnly", true);
    result.descriptor.bootPriority = json.value("bootPriority", -128);
    result.descriptor.source = HostMountSource::RestoredSession;
    ReadHardfileFields(json, result.descriptor);
    result.originalPath = json.value("originalPath", std::string{});
    result.documentId = json.value("documentId", 0u);
    if (auto fingerprint = DirectoryFingerprint::Parse(json.value("fingerprint", std::string{})))
        result.directoryFingerprint = *fingerprint;
    return result;
}

std::string IsoTimestampUtc()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

// Adds one in-memory buffer to the archive under entryName. The backing bytes must outlive
// zip_close, so callers pass pointers into storage that stays alive for the whole write.
bool AddEntry(zip_t* archive, const std::string& entryName, const void* data, size_t size,
              std::string& outError)
{
    zip_source_t* s = zip_source_buffer(archive, data, size, 0);
    if (!s) {
        outError = "Failed to create ZIP source for " + entryName;
        return false;
    }
    if (zip_file_add(archive, entryName.c_str(), s, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0) {
        outError =
            std::string("Failed to add ZIP entry ") + entryName + ": " + zip_strerror(archive);
        zip_source_free(s);
        return false;
    }
    return true;
}

// Reads an archive entry by exact name. Returns std::nullopt if the entry is absent.
std::optional<std::vector<uint8_t>> ReadEntry(zip_t* archive, const std::string& name)
{
    zip_int64_t idx = zip_name_locate(archive, name.c_str(), 0);
    if (idx < 0)
        return std::nullopt;

    zip_stat_t sb;
    zip_stat_init(&sb);
    if (zip_stat_index(archive, idx, 0, &sb) != 0 || !(sb.valid & ZIP_STAT_SIZE))
        return std::nullopt;

    // The declared size comes from the archive's central directory — a crafted .peek file can
    // claim anything, so cap it before allocating.
    if (sb.size > core::constants::disk::MaxZipAdfSize)
        return std::nullopt;

    zip_file_t* zf = zip_fopen_index(archive, idx, 0);
    if (!zf)
        return std::nullopt;

    std::vector<uint8_t> out(static_cast<size_t>(sb.size));
    zip_int64_t read = zip_fread(zf, out.data(), sb.size);
    zip_fclose(zf);
    if (read != static_cast<zip_int64_t>(sb.size))
        return std::nullopt;
    return out;
}

std::optional<std::string> ReadEntryString(zip_t* archive, const std::string& name)
{
    auto bytes = ReadEntry(archive, name);
    if (!bytes)
        return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

} // namespace

bool SessionSerializer::Write(const std::string& path, const PeekSession& session,
                              std::string& outError)
{
    int err = 0;
    zip_t* archive = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (!archive) {
        zip_error_t error;
        zip_error_init_with_code(&error, err);
        outError = std::string("Failed to create .peek archive: ") + zip_error_strerror(&error);
        zip_error_fini(&error);
        return false;
    }

    // All generated strings must stay alive until zip_close; deque keeps element addresses
    // stable across appends (unlike vector).
    std::deque<std::string> blobs;
    auto keep = [&blobs](std::string s) -> const std::string& {
        blobs.push_back(std::move(s));
        return blobs.back();
    };

    bool ok = true;

    // manifest.json
    nlohmann::json manifest;
    manifest["format"] = "adm.peek";
    manifest["version"] = session.version;
    manifest["appVersion"] =
        session.appVersion.empty() ? std::string(constants::AppVersion) : session.appVersion;
    manifest["coreVersion"] = session.coreVersion;
    manifest["createdUtc"] = IsoTimestampUtc();
    manifest["model"] = session.modelId;
    manifest["activeDocumentId"] = session.activeDocumentId;
    manifest["snapshot"] = {
        {"present", session.snapshotPresent && !session.snapshot.empty()},
        {"file", "snapshot.bin"},
        {"size", session.snapshot.size()},
        {"ownerDocumentId", session.snapshotOwnerDocId},
        {"df0DocumentId", session.df0DocId},
        {"fileExt", session.snapshotFileExt},
        {"wasPaused", session.wasPaused},
    };
    manifest["frameHistory"] = {
        {"present", session.frameHistoryPresent && !session.frameHistory.empty()},
        {"file", "framehistory.bin"},
        {"size", session.frameHistory.size()},
    };
    if (!session.bridgedSlots.empty()) {
        manifest["bridgedSlots"] = nlohmann::json::array();
        for (const auto& slot : session.bridgedSlots) {
            manifest["bridgedSlots"].push_back({
                {"drive", slot.drive},
                {"hardwareType", slot.hardwareType},
                {"comPort", slot.comPort},
                {"bridgeMode", slot.bridgeMode},
                {"bridgeDensity", slot.bridgeDensity},
                {"driveCable", slot.driveCable},
                {"autoCache", slot.autoCache},
                {"smartSpeed", slot.smartSpeed},
                {"allowWrites", slot.allowWrites},
                {"hardwareName", slot.hardwareName},
            });
        }
    }
    if (session.version >= 2) {
        manifest["hostMounts"] = nlohmann::json::array();
        for (size_t index = 0; index < session.hostMounts.size(); ++index) {
            if (session.hostMounts[index])
                manifest["hostMounts"].push_back(MountJson(*session.hostMounts[index]));
            else
                manifest["hostMounts"].push_back(nullptr);
        }
        manifest["hardDriveControllerRoms"] = ControllerRomsJson(session.hardDriveControllerRoms);
        manifest["cdImage"] = session.cdImage;
    }

    {
        const std::string& s = keep(manifest.dump(2));
        if (ok && !AddEntry(archive, "manifest.json", s.data(), s.size(), outError))
            ok = false;
    }

    if (ok && session.snapshotPresent && !session.snapshot.empty()) {
        if (!AddEntry(archive, "snapshot.bin", session.snapshot.data(), session.snapshot.size(),
                      outError))
            ok = false;
    }

    if (ok && session.frameHistoryPresent && !session.frameHistory.empty()) {
        if (!AddEntry(archive, "framehistory.bin", session.frameHistory.data(),
                      session.frameHistory.size(), outError))
            ok = false;
    }

    if (ok) {
        const std::string& s = keep(session.breakpoints.dump(2));
        if (!AddEntry(archive, "breakpoints.json", s.data(), s.size(), outError))
            ok = false;
    }

    if (ok) {
        const std::string& s = keep(session.viewState.dump(2));
        if (!AddEntry(archive, "view_state.json", s.data(), s.size(), outError))
            ok = false;
    }

    if (ok) {
        const std::string& s = keep(session.notes);
        if (!AddEntry(archive, "notes.txt", s.data(), s.size(), outError))
            ok = false;
    }

    for (const auto& doc : session.documents) {
        if (!ok)
            break;
        const std::string prefix = DocPrefix(doc.id);

        nlohmann::json dj;
        dj["id"] = doc.id;
        dj["name"] = doc.name;
        dj["originalPath"] = doc.originalPath;
        dj["zipEntryName"] = doc.zipEntryName;
        dj["sourceKind"] = doc.sourceKind;
        dj["forceUnsaved"] = doc.forceUnsaved;
        dj["writeProtected"] = doc.writeProtected;
        dj["isIpf"] = doc.isIpf;
        dj["lastHexViewBlock"] = doc.lastHexViewBlock;
        dj["partition"] = doc.partition;
        dj["hasIpf"] = !doc.ipfBytes.empty();
        dj["kind"] = doc.kind == DocumentKind::HostDirectory ? "hostDirectory" : "diskImage";
        dj["windowState"] = doc.windowState;
        if (doc.kind == DocumentKind::HostDirectory) {
            dj["canonicalPath"] = doc.canonicalPath;
            dj["assignedDevice"] = doc.assignedDevice
                                       ? nlohmann::json(HostMountDeviceName(*doc.assignedDevice))
                                       : nlohmann::json(nullptr);
            dj["volumeLabel"] = doc.volumeLabel;
            dj["readOnly"] = doc.readOnly;
            dj["effectiveMount"] = doc.effectiveMount;
            dj["fingerprint"] = doc.directoryFingerprint.ToString();
        }

        const std::string& djs = keep(dj.dump(2));
        if (!AddEntry(archive, prefix + "doc.json", djs.data(), djs.size(), outError)) {
            ok = false;
            break;
        }
        // A hardfile document is a reference to its file; there is no disk.adf to write.
        if (doc.kind == DocumentKind::DiskImage && doc.sourceKind != "hardfile" &&
            !AddEntry(archive, prefix + "disk.adf", doc.adfBytes.data(), doc.adfBytes.size(),
                      outError)) {
            ok = false;
            break;
        }
        if (doc.kind == DocumentKind::DiskImage && !doc.ipfBytes.empty()) {
            if (!AddEntry(archive, prefix + "disk.ipf", doc.ipfBytes.data(), doc.ipfBytes.size(),
                          outError)) {
                ok = false;
                break;
            }
        }
    }

    if (!ok) {
        zip_discard(archive);
        return false;
    }

    if (zip_close(archive) < 0) {
        outError = std::string("Failed to write .peek archive: ") + zip_strerror(archive);
        zip_discard(archive);
        return false;
    }

    Logger::Info("Saved session to {}", path);
    return true;
}

bool RestoredHardfileMayWrite(const HostDirectoryMount& restored,
                              const HostDirectoryMount& configured)
{
    if (!restored.IsHardfile() || !configured.enabled || !configured.IsHardfile() ||
        configured.readOnly)
        return false;
    std::error_code ec;
    return std::filesystem::equivalent(restored.hardfilePath, configured.hardfilePath, ec) && !ec;
}

bool SessionSerializer::Read(const std::string& path, PeekSession& out, std::string& outError)
{
    out = {};
    int err = 0;
    zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &err);
    if (!archive) {
        zip_error_t error;
        zip_error_init_with_code(&error, err);
        outError = std::string("Failed to open .peek archive: ") + zip_error_strerror(&error);
        zip_error_fini(&error);
        return false;
    }

    auto manifestStr = ReadEntryString(archive, "manifest.json");
    if (!manifestStr) {
        outError = "Not a valid .peek file (missing manifest.json)";
        zip_close(archive);
        return false;
    }

    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(*manifestStr);
    } catch (const std::exception& e) {
        outError = std::string("Corrupt .peek manifest: ") + e.what();
        zip_close(archive);
        return false;
    }

    if (!manifest.is_object() || manifest.value("format", std::string{}) != "adm.peek") {
        outError = "Not a PEEKaboo session file";
        zip_close(archive);
        return false;
    }

    // Every field below comes from a file that may have been hand-edited or made by another
    // build: a wrong type fails the open with a message instead of terminating the app.
    try {
        out.version = manifest.value("version", 1);
        if (out.version > kSupportedVersion) {
            outError = "This .peek was made by a newer version of PEEKaboo (format v" +
                       std::to_string(out.version) + ").";
            zip_close(archive);
            return false;
        }

        out.appVersion = manifest.value("appVersion", std::string{});
        out.coreVersion = manifest.value("coreVersion", std::string{});
        out.modelId = manifest.value("model", std::string{});
        out.activeDocumentId = manifest.value("activeDocumentId", 0u);
        if (out.version >= 2 && manifest.contains("hostMounts") &&
            manifest["hostMounts"].is_array()) {
            const auto& mounts = manifest["hostMounts"];
            for (size_t index = 0; index < out.hostMounts.size() && index < mounts.size();
                 ++index) {
                if (!mounts[index].is_null())
                    out.hostMounts[index] =
                        ParseMount(mounts[index], static_cast<HostMountDevice>(index));
            }
            out.cdImage = manifest.value("cdImage", std::string{});
            if (manifest.contains("hardDriveControllerRoms"))
                out.hardDriveControllerRoms =
                    ControllerRomsFromJson(manifest["hardDriveControllerRoms"]);
        }

        if (manifest.contains("snapshot") && manifest["snapshot"].is_object()) {
            const auto& snap = manifest["snapshot"];
            out.snapshotPresent = snap.value("present", false);
            out.snapshotOwnerDocId = snap.value("ownerDocumentId", 0u);
            out.df0DocId = snap.value("df0DocumentId", 0u);
            out.snapshotFileExt = snap.value("fileExt", std::string("adf"));
            out.wasPaused = snap.value("wasPaused", true);
        }

        if (manifest.contains("bridgedSlots") && manifest["bridgedSlots"].is_array()) {
            for (const auto& node : manifest["bridgedSlots"]) {
                if (!node.is_object())
                    continue;
                PeekSession::BridgedSlot slot;
                slot.drive = node.value("drive", 0);
                if (slot.drive < 0 || slot.drive > 3)
                    continue;
                slot.hardwareType = node.value("hardwareType", 1);
                slot.comPort = node.value("comPort", std::string());
                slot.bridgeMode = node.value("bridgeMode", 0);
                slot.bridgeDensity = node.value("bridgeDensity", 0);
                slot.driveCable = node.value("driveCable", 0);
                // indices into the Floppy Drives window's tables: clamp what the file says
                slot.bridgeMode = std::clamp(slot.bridgeMode, 0, kMaxBridgeMode);
                slot.bridgeDensity = std::clamp(slot.bridgeDensity, 0, kMaxBridgeDensity);
                slot.driveCable = std::clamp(slot.driveCable, 0, kMaxDriveCable);
                slot.autoCache = node.value("autoCache", true);
                slot.smartSpeed = node.value("smartSpeed", true);
                slot.allowWrites = node.value("allowWrites", false);
                slot.hardwareName = node.value("hardwareName", std::string());
                out.bridgedSlots.push_back(std::move(slot));
            }
        }

        if (out.snapshotPresent) {
            if (auto blob = ReadEntry(archive, "snapshot.bin"))
                out.snapshot = std::move(*blob);
            else
                out.snapshotPresent = false; // tolerate a missing/truncated snapshot
        }

        if (manifest.contains("frameHistory") && manifest["frameHistory"].is_object())
            out.frameHistoryPresent = manifest["frameHistory"].value("present", false);
        if (out.frameHistoryPresent) {
            if (auto blob = ReadEntry(archive, "framehistory.bin"))
                out.frameHistory = std::move(*blob);
            else
                out.frameHistoryPresent = false; // an absent timeline is not a broken session
        }

        // Optional sidecars — degrade gracefully on parse failure.
        if (auto s = ReadEntryString(archive, "breakpoints.json")) {
            try {
                out.breakpoints = nlohmann::json::parse(*s);
            } catch (const std::exception&) {
                Logger::Warn("Ignoring corrupt breakpoints.json in {}", path);
            }
        }
        if (auto s = ReadEntryString(archive, "view_state.json")) {
            try {
                out.viewState = nlohmann::json::parse(*s);
            } catch (const std::exception&) {
                Logger::Warn("Ignoring corrupt view_state.json in {}", path);
            }
        }
        if (auto s = ReadEntryString(archive, "notes.txt"))
            out.notes = std::move(*s);

        // Walk every documents/<id>/doc.json entry.
        zip_int64_t numEntries = zip_get_num_entries(archive, 0);
        for (zip_int64_t i = 0; i < numEntries; ++i) {
            const char* nameC = zip_get_name(archive, i, 0);
            if (!nameC)
                continue;
            std::string name(nameC);
            const std::string suffix = "/doc.json";
            if (name.size() <= suffix.size() ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            if (name.rfind("documents/", 0) != 0)
                continue;

            auto docStr = ReadEntryString(archive, name);
            if (!docStr)
                continue;
            nlohmann::json dj;
            try {
                dj = nlohmann::json::parse(*docStr);
            } catch (const std::exception&) {
                Logger::Warn("Ignoring corrupt {} in {}", name, path);
                continue;
            }

            PeekDocument doc;
            doc.id = dj.value("id", 0u);
            doc.name = dj.value("name", std::string{});
            doc.originalPath = dj.value("originalPath", std::string{});
            doc.zipEntryName = dj.value("zipEntryName", std::string{});
            doc.sourceKind = dj.value("sourceKind", std::string("adf"));
            doc.forceUnsaved = dj.value("forceUnsaved", false);
            doc.writeProtected = dj.value("writeProtected", true);
            doc.isIpf = dj.value("isIpf", false);
            doc.lastHexViewBlock = dj.value("lastHexViewBlock", 0u);
            doc.partition = dj.value("partition", 0);
            doc.kind = dj.value("kind", std::string("diskImage")) == "hostDirectory"
                           ? DocumentKind::HostDirectory
                           : DocumentKind::DiskImage;
            doc.windowState = dj.value("windowState", nlohmann::json{});

            if (doc.kind == DocumentKind::HostDirectory) {
                doc.canonicalPath = dj.value("canonicalPath", std::string{});
                if (dj.contains("assignedDevice") && !dj["assignedDevice"].is_null())
                    doc.assignedDevice = ParseDevice(dj["assignedDevice"]);
                doc.volumeLabel = dj.value("volumeLabel", std::string{});
                doc.readOnly = dj.value("readOnly", true);
                doc.effectiveMount = dj.value("effectiveMount", false);
                if (auto fingerprint =
                        DirectoryFingerprint::Parse(dj.value("fingerprint", std::string{})))
                    doc.directoryFingerprint = *fingerprint;
                std::error_code ec;
                doc.missing = doc.canonicalPath.empty() ||
                              !std::filesystem::is_directory(
                                  std::filesystem::u8path(doc.canonicalPath), ec) ||
                              ec;
                out.documents.push_back(std::move(doc));
                continue;
            }

            const std::string prefix = DocPrefix(doc.id);
            if (doc.sourceKind == "hardfile") {
                out.documents.push_back(std::move(doc));
                continue;
            }
            if (auto adf = ReadEntry(archive, prefix + "disk.adf"))
                doc.adfBytes = std::move(*adf);
            else {
                Logger::Warn("Skipping document {} — missing disk.adf", doc.id);
                continue;
            }
            if (dj.value("hasIpf", false) || doc.isIpf) {
                if (auto ipf = ReadEntry(archive, prefix + "disk.ipf"))
                    doc.ipfBytes = std::move(*ipf);
            }
            out.documents.push_back(std::move(doc));
        }
    } catch (const nlohmann::json::exception& e) {
        out = {};
        outError = std::string("Corrupt .peek session: ") + e.what();
        zip_close(archive);
        return false;
    }

    zip_close(archive);
    return true;
}

} // namespace adm::core
