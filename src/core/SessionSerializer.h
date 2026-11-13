#pragma once

#include "HostDirectoryDocument.h"

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace adm::core {

// One open disk document captured in a .peek session. The ADF bytes are the decoded,
// in-memory image (including any unsaved in-session edits) so a reopened session is fully
// self-contained. ipfBytes is populated only for IPF-sourced documents so copy-protected
// boot still works after restore.
struct PeekDocument
{
    uint32_t id = 0;
    std::string name;
    std::string originalPath;       // advisory only (for a future "save back" — not used on load)
    std::string zipEntryName;       // advisory only
    std::string sourceKind = "adf"; // adf | dms | zip | ipf | executable
    bool forceUnsaved = false;
    // Write protection for a disk-image document. Defaults true so a session written
    // before this existed restores protected rather than writable.
    bool writeProtected = true;
    bool isIpf = false;
    uint32_t lastHexViewBlock = 0;
    // Hardfile documents (sourceKind "hardfile"): which partition was shown, -1 = whole image.
    int partition = 0;
    std::vector<uint8_t> adfBytes;
    std::vector<uint8_t> ipfBytes; // optional

    DocumentKind kind = DocumentKind::DiskImage;
    std::string canonicalPath; // HostDirectory reference; contents are never embedded
    std::optional<HostMountDevice> assignedDevice;
    std::string volumeLabel;
    bool readOnly = true;
    bool effectiveMount = false;
    bool missing = false;
    DirectoryFingerprint directoryFingerprint;
    nlohmann::json windowState;
};

struct PeekMountReference
{
    HostDirectoryMount descriptor;
    std::string originalPath;
    uint32_t documentId = 0;
    DirectoryFingerprint directoryFingerprint;
};

// A .peek can come from anyone, and its hardfile mount names any host file: restored
// read-write, a snapshot that writes raw blocks to DH0 would overwrite it. A restored hardfile
// keeps its writes only when it is the user's own mount for that slot - the same file, enabled
// and read-write in their configuration; anything else is restored read-only.
bool RestoredHardfileMayWrite(const HostDirectoryMount& restored,
                              const HostDirectoryMount& configured);

// A complete debug session. Mirrors the .peek zip layout; consumers read/write the JSON
// sidecars (breakpoints, viewState) directly so each can evolve independently.
struct PeekSession
{
    int version = 1;
    std::string appVersion;
    std::string coreVersion;
    std::string modelId; // AmigaModelInfo id string

    uint32_t activeDocumentId = 0;

    // Machine snapshot (retro_serialize blob). Empty when not captured.
    std::vector<uint8_t> snapshot;
    bool snapshotPresent = false;
    uint32_t snapshotOwnerDocId = 0; // document mounted in DF0 when the snapshot was taken
    uint32_t df0DocId = 0;
    std::string snapshotFileExt = "adf";
    bool wasPaused = true;

    // Serialized frame-history timeline (FrameHistory::Serialize). Opt-in and empty by
    // default: a full timeline runs to hundreds of megabytes, which is not something to
    // put in every saved session without being asked.
    std::vector<uint8_t> frameHistory;
    bool frameHistoryPresent = false;

    // JSON sidecars (parsed/written by callers; stored verbatim here).
    nlohmann::json breakpoints;
    nlohmann::json viewState;
    std::string notes;

    // Which drive slots were backed by live FloppyBridge hardware, and with what
    // configuration. Only the configuration is stored - never that the drive was
    // spinning, and never any of its data. On restore the slot is left empty with a
    // note asking the user to reconnect; re-attaching hardware without being asked
    // would be wrong, and the platter's state cannot be restored in any case.
    struct BridgedSlot
    {
        int drive = 0;
        int hardwareType = 1;
        std::string comPort;
        int bridgeMode = 0;
        int bridgeDensity = 0;
        int driveCable = 0;
        bool autoCache = true;
        bool smartSpeed = true;
        bool allowWrites = false;
        std::string hardwareName; // for the reconnect message
    };
    std::vector<BridgedSlot> bridgedSlots;

    std::vector<PeekDocument> documents;
    std::array<std::optional<PeekMountReference>, 2> hostMounts;
    HardDriveControllerRoms hardDriveControllerRoms;
    // The disc CD0 held: an image, or the archive it was extracted from. Referenced, never
    // embedded.
    std::string cdImage;
};

// Reads and writes the .peek archive (a standard ZIP). All ZIP/JSON plumbing lives here so
// the Application layer only deals with PeekSession structs.
class SessionSerializer
{
public:
    static constexpr int kSupportedVersion = 2;

    // Serialize a session to a .peek file. Returns false and sets outError on failure.
    static bool Write(const std::string& path, const PeekSession& session, std::string& outError);

    // Parse a .peek file into outSession. Missing optional entries (snapshot, notes,
    // view_state) are tolerated. Returns false and sets outError on a fatal error
    // (unreadable archive, missing manifest, or unsupported version).
    static bool Read(const std::string& path, PeekSession& outSession, std::string& outError);
};

} // namespace adm::core
