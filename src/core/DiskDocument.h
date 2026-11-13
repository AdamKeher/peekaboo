#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "AdfWrapper.h"
#include "BrainFile.h"
#include "DiskIdentifier.h"
#include "DownloadManager.h"
#include "HostDirectoryDocument.h"
#include "HostMountService.h"
#include "IpfTypes.h"

namespace adm {
namespace ui {
class FileListView;
class FileViewer;
class ImageViewer;
class AudioPlayer;
class HunkViewer;
} // namespace ui
} // namespace adm

namespace adm {
namespace core {

class FileOperationService;
class DiskSearcher;
class DebugSymbols;
class DebugLines;
class DebugInline;
class DebugFrame;
class ExocetPack;

struct DemosceneSource
{
    int releaseId = 0;
    std::string releaseName;
    std::string groupName;
    std::string source; // "tosec" | "demozoo" | "pouet" | "janeway" | ""
};

/**
 * @brief Bundles all per-disk state for multi-document support.
 *
 * Each open ADF file gets its own DiskDocument instance containing
 * the volume, file operation service, file list view, and per-document viewers.
 */
/**
 * @brief Load state for a document. Documents created from a remote download
 * exist as a placeholder tab (Downloading) before their volume is mounted.
 */
enum class DocLoadState {
    Ready,       // volume mounted, normal disk view
    Downloading, // remote download in progress; placeholder UI shown
    Failed       // download or mount failed; error UI with Retry/Close
};

struct DiskDocument
{
    uint32_t id = 0;
    uint32_t initialDockId = 0;
    AdfHandler volume;
    DocumentKind kind = DocumentKind::DiskImage;
    std::unique_ptr<HostDirectoryDocument> hostDirectory;
    std::unordered_map<uint32_t, DocumentEntryId> hostEntryIds;
    std::chrono::steady_clock::time_point nextHostRefresh{};
    std::optional<DirectoryFingerprint> savedDirectoryFingerprint;
    bool directoryFingerprintChanged = false;
    bool restoredWindowPlacement = false;

    // Remote-download placeholder state (Ready for locally-loaded documents)
    DocLoadState loadState = DocLoadState::Ready;
    std::shared_ptr<DownloadTask> download; // non-null while Downloading/Failed
    DownloadRequest downloadRequest;        // retained so the tab can Retry

    std::unique_ptr<FileOperationService> fileOpService;
    std::unique_ptr<DiskSearcher> diskSearcher;
    std::string currentFilePath;
    std::string loadedZipPath;
    std::string loadedZipEntryName;
    std::string loadedDmsPath;
    std::string loadedIpfPath;    // non-empty = loaded from an IPF preservation image
    std::string loadedExtAdfPath; // non-empty = loaded from a UAE-1ADF extended-ADF image
    std::string loadedScpPath;    // non-empty = loaded from a SuperCard Pro flux image
    std::string loadedHdzPath; // non-empty = gunzipped from a .hdz hardfile; save goes to Save As
    // What the document's own write protection was before a running emulator took its hardfile
    // read-write. Set while that lock is held, so the user's choice comes back afterwards.
    std::optional<bool> writeProtectBeforeEmulatorLock;
    // What Application::UpdateHardfileDocumentRuntime last worked out about this file's place in
    // the running machine, keyed by the mount service's generation, the run state and the path.
    // The per-frame path is then a lookup rather than a std::filesystem::equivalent (which opens
    // both files every time).
    struct HardfileRuntime
    {
        uint64_t generation = ~0ull;
        bool running = false;
        std::string path;
        std::optional<HostMountDevice> device;
        bool readWrite = false;
        std::string status;     // DiskInfo::runtimeStatus
        std::string lockReason; // the padlock's tooltip while the emulator holds the file
    };
    HardfileRuntime hardfileRuntime;
    // per-track preservation metadata (IPF / ext-ADF / SCP)
    std::vector<core::IpfTrackInfo> ipfTracks;
    // Original undecoded image (IPF / UAE-1ADF / SCP) handed to the emulator so it
    // reads the disk at flux level and copy protection works. Empty = run the
    // decoded ADF.
    std::vector<uint8_t> emulatorImageBytes;
    std::string loadedFromUrl; // non-empty = downloaded from remote URL; save redirects to save-as
    // Exactly the bytes this document was built from, kept only when there is no file
    // on disk to go back to (a download mounted straight from memory). The disk
    // identifier hashes these: currentFilePath is a display name for such a document,
    // so opening it to hash fails, and hashing the mounted image instead would match
    // nothing - TOSEC's hashes are over the file as distributed, not over the ADF a
    // HUNK executable gets wrapped in.
    std::vector<uint8_t> identifySourceBytes;
    std::optional<DemosceneSource> demosceneSource; // set when downloaded (TOSEC, the extension)
    std::string
        loadedExecutablePath; // non-empty = loaded from Amiga executable; save redirects to save-as
    // Source-level debug symbols auto-discovered from a sibling .elf next to the
    // loaded file (bartman toolchain output). Null when no matching .elf exists.
    std::unique_ptr<core::DebugSymbols> debugSymbols;
    std::unique_ptr<core::DebugLines>
        debugLines; // DWARF line info from the same .elf (may be null)
    std::unique_ptr<core::DebugInline> debugInline; // DWARF inline info (flame chart; may be null)
    // DWARF call-frame info from the same .elf: what lets the Call Stack window unwind the
    // live stack at a halted PC without a gdb session. May be null.
    std::unique_ptr<core::DebugFrame> debugFrame;
    std::string debugElfPath; // path the symbols were loaded from (for UI/logging)
    // The Exocet sync pack's `.exocet`, discovered beside the loaded file by the .elf's rules.
    // Null when there is none; exocetError says why
    // a file that was found did not load.
    std::shared_ptr<core::ExocetPack> exocet;
    std::string exocetError;
    bool forceUnsaved = false; // true for run documents with no backing file
    BootBlockInfo bootBlockInfo;
    uint32_t lastHexViewBlock = 0;
    std::unique_ptr<core::DiskIdentifier> identifier; // Per-document background disk identifier
    // Trainer auto-discovery runs once, after the identifier has produced an MD5. The flag is
    // set even when nothing matched, so a document with no trainer does not re-scan every frame.
    bool trainerLookupDone = false;
    std::unique_ptr<ui::FileListView> fileListView;

    // A hard-disk image (.hdf/.hdz): `volume` shows one RDB partition at a time.
    [[nodiscard]] bool IsHardfile() const
    {
        return kind == DocumentKind::DiskImage && volume.IsPartitionedDevice();
    }

    // Write protection. Enforced inside AdfHandler (every mutator checks it), so
    // this is only the document's view of the same flag. Host-directory documents
    // carry their own read-only switch instead.
    [[nodiscard]] bool IsWriteProtected() const
    {
        if (kind == DocumentKind::HostDirectory)
            return !hostDirectory || hostDirectory->IsReadOnly();
        return volume.IsWriteProtected();
    }

    [[nodiscard]] bool HasUnsavedChanges() const
    {
        if (kind == DocumentKind::HostDirectory)
            return false;
        return volume.HasUnsavedChanges() || forceUnsaved;
    }

    // Per-document multi-instance viewers
    std::vector<std::unique_ptr<ui::FileViewer>> fileViewers;
    std::vector<std::unique_ptr<ui::ImageViewer>> imageViewers;
    std::vector<std::unique_ptr<ui::AudioPlayer>> audioPlayers;
    std::vector<std::unique_ptr<ui::HunkViewer>> hunkViewers;
};

} // namespace core
} // namespace adm
