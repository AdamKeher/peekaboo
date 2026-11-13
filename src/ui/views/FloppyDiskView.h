#pragma once

#include "../../core/DiskCompare.h"
#include "../../core/DiskDocumentInfo.h"
#include "../../core/GreaseweazelReader.h"
#include "../../core/GreaseweazelWriter.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace adm::ui {

// Single window for all GreaseWeazle floppy I/O: reading a physical disk into a new document and
// writing an ADF back out to one, plus the drive diagnostics (Info/Bandwidth/RPM). Read and Write
// share the drive/format pickers, the track-grid visualization and the output log — only the
// per-mode options and action button differ — so the two used to be separate, much thinner windows
// (Write had verify/pre-erase/track grid; Read was a bare progress popup) and are unified here.
// What the Write tab would write if you pressed the button right now. Cheap enough to ask for
// every frame: it answers "can this document be written, and from where", not "give me the
// 880KB". The bytes themselves are fetched once, at the moment the write starts.
struct WriteSourceInfo
{
    uint32_t id = 0;       // resolved document id; 0 when nothing is open
    std::string name;      // display name, as the Disk Images menu spells it
    std::string filePath;  // the document's path - may be a placeholder like "[Floppy Disk]"
    bool hasFile = false;  // filePath names a real file on disk, so gw can be pointed at it
    bool hasBytes = false; // the document can hand over its image from memory instead
};

class FloppyDiskView
{
public:
    enum class Mode { Write, Read, Format, Compare };

    FloppyDiskView();
    ~FloppyDiskView() = default;

    // Per-frame housekeeping that must run whether or not the window is being drawn - notably
    // firing the read-complete callback. Call once per frame, unconditionally, before Draw().
    void Poll();

    void Draw();

    // Opens the window on the given tab. Safe to call repeatedly (e.g. from both menu items) to
    // switch tabs on an already-open window.
    void Open(Mode mode);
    void Close();

    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    // Callbacks to get data from Application (write side). Each takes the id of the document to
    // write, which is 0 for "whichever one is active" - the only thing this window could write
    // before the image selector existed, and still the default.
    void SetOnGetDocuments(std::function<std::vector<DiskDocumentInfo>()> callback)
    {
        m_onGetDocuments = std::move(callback);
    }
    void SetOnGetWriteSource(std::function<WriteSourceInfo(uint32_t)> callback)
    {
        m_onGetWriteSource = std::move(callback);
    }
    void SetOnGetZeroFilledBlocks(std::function<std::vector<bool>(uint32_t)> callback)
    {
        m_onGetZeroFilledBlocks = std::move(callback);
    }
    void SetOnGetAdfData(std::function<std::vector<uint8_t>(uint32_t)> callback)
    {
        m_onGetAdfData = std::move(callback);
    }

    // Format tab. The template list is the one the New Disk menu offers - the blank ADFs in
    // images/ - so "OFS International" means the same thing in both places. The builder turns a
    // template name plus a volume label into the image that gets written to the physical disk.
    void SetOnGetDiskTemplates(std::function<std::vector<std::string>()> callback)
    {
        m_onGetDiskTemplates = std::move(callback);
    }
    void SetOnBuildFormatImage(std::function<std::vector<uint8_t>(const std::string& templateName,
                                                                  const std::string& label)>
                                   callback)
    {
        m_onBuildFormatImage = std::move(callback);
    }
    void SetOnSettingsChanged(std::function<void(int, int, bool, bool, bool, bool)> callback)
    {
        m_onSettingsChanged = std::move(callback);
    }

    // Fired once, the frame a full floppy read completes — Application mounts the buffer as a new
    // document. Not fired again until another read is started.
    void SetOnReadComplete(std::function<void(const std::vector<uint8_t>&)> callback)
    {
        m_onReadComplete = std::move(callback);
    }

    // Load saved settings from preferences
    void SetDriveIndex(int index) { m_driveIndex = index; }
    void SetFormatIndex(int index) { m_formatIndex = index; }
    void SetVerify(bool verify) { m_verify = verify; }
    void SetPreErase(bool preErase) { m_preErase = preErase; }
    void SetEraseEmpty(bool eraseEmpty) { m_eraseEmpty = eraseEmpty; }
    void SetReadVerify(bool verify) { m_readVerify = verify; }

    // Access to the backends for Application to call Start/Cancel directly if ever needed.
    core::GreaseweazelWriter& GetWriter() { return m_writer; }
    core::GreaseweazelReader& GetReader() { return m_reader; }

    // Public actions - drive diagnostics, mode-independent (run on the writer's `gw` runner).
    void RunInfo();
    void RunBandwidth();
    void RunRPM();

private:
    void DrawConfigPanel();
    void DrawWriteConfig(bool busy);
    void DrawReadConfig(bool busy);
    void DrawFormatConfig(bool busy);
    void DrawCompareConfig(bool busy);
    // The gw diagnostics row (Info/Bandwidth/RPM/Cancel) plus the elapsed-time status, shared by
    // every tab that drives the writer. Read has its own because it reports the reader's state.
    void DrawWriterActionRow(bool busy);
    // The same row for a tab that drives a reader: Read drives m_reader, Compare its own.
    void DrawReaderActionRow(core::GreaseweazelReader& reader, bool busy);
    void DrawProgressSection();
    void DrawTrackGrid();
    void DrawOutputLog();

    bool m_isOpen = false;
    Mode m_mode = Mode::Write;
    // One-shot: Open(mode) sets this so the next Draw() forces the ImGui tab bar's selection to
    // match m_mode (the tab bar otherwise tracks its own selected tab independent of m_mode).
    bool m_forceTabSelect = false;

    core::GreaseweazelWriter m_writer;
    core::GreaseweazelReader m_reader;
    bool m_readCompleteFired = true; // edge-detects Reading -> Done exactly once per read

    // Compare reads the disk with a reader of its own, so a compare never fires the Read tab's
    // read-complete callback (which mounts the disk as a new document) and leaves that tab's
    // last result alone. The image is snapshotted when the compare starts: the comparison is
    // against what was selected at that moment, whatever happens to the document meanwhile.
    core::GreaseweazelReader m_compareReader;
    std::vector<uint8_t> m_compareImage;
    std::string m_compareImageName;
    std::vector<core::TrackCompareResult> m_compareResults; // empty until a compare finishes
    std::string m_compareSummary;                           // one line, under the Compare button
    std::string m_compareDetail;   // the differing tracks, one per line, for the output log
    std::string m_compareError;    // why the last Compare press did not start
    bool m_compareFinished = true; // edge-detects the compare reader reaching Done

    // Shared configuration
    int m_driveIndex = 0;
    int m_formatIndex = 0;

    // Write-only configuration
    bool m_verify = true;
    bool m_fastWrite = false;
    bool m_preErase = false;
    bool m_eraseEmpty = false;

    // Read-only configuration
    bool m_readVerify = false;

    // Format-only configuration. The template index is held by name, not by position: the
    // images/ directory is rescanned at startup and an inserted file would otherwise silently
    // shift the selection onto a different filesystem.
    std::string m_formatTemplate;
    char m_formatLabel[32] = "Empty";

    // Write source. m_writeDocId is what the image selector holds: 0 follows the active
    // document, anything else pins one open image so the write survives clicking around the
    // app. m_writeSource is re-resolved every frame the Write tab is drawn, because a pinned
    // document can be closed, saved under a new name or edited behind this window's back.
    uint32_t m_writeDocId = 0;
    WriteSourceInfo m_writeSource;
    std::vector<bool> m_cachedZeroFilledBlocks;
    bool m_autoScroll = true;

    // Timer - tracks whichever operation (read or write) is currently running.
    bool m_timerRunning = false;
    std::chrono::steady_clock::time_point m_timerStart;
    double m_finalElapsed = 0.0;

    // Callbacks
    std::function<std::vector<DiskDocumentInfo>()> m_onGetDocuments;
    std::function<WriteSourceInfo(uint32_t)> m_onGetWriteSource;
    std::function<std::vector<bool>(uint32_t)> m_onGetZeroFilledBlocks;
    std::function<std::vector<uint8_t>(uint32_t)> m_onGetAdfData;
    std::function<void(int driveIndex, int formatIndex, bool verify, bool preErase, bool eraseEmpty,
                       bool readVerify)>
        m_onSettingsChanged;
    std::function<void(const std::vector<uint8_t>&)> m_onReadComplete;
    std::function<std::vector<std::string>()> m_onGetDiskTemplates;
    std::function<std::vector<uint8_t>(const std::string&, const std::string&)>
        m_onBuildFormatImage;

    void SaveSettings();
    // Re-reads m_writeSource for the current selection, falling back to the active document if
    // the pinned one has gone away. Cheap: safe to call every frame.
    void RefreshWriteSource();
    // RefreshWriteSource plus the NDOS block scan behind "Fast write", which is not cheap -
    // call it when the selection changes, not per frame.
    void RefreshWriteSelection();
    void DrawImageSelector(bool busy);
    void StartWrite();
    void StartRead();
    void StartFormat();
    void StartCompare();
    void FinishCompare();

    static constexpr const char* s_drives[] = {"A", "B"};
    static constexpr const char* s_formats[] = {"amiga.amigados", "amiga.amigados_hd"};
    static constexpr int s_numDrives = 2;
    static constexpr int s_numFormats = 2;
};

} // namespace adm::ui
