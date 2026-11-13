#include "FloppyDiskView.h"

#include <fmt/format.h>
#include <imgui.h>
#include <algorithm>

#include "../IconsFontAwesome6.h"

namespace adm::ui {

FloppyDiskView::FloppyDiskView() = default;

void FloppyDiskView::Open(Mode mode)
{
    m_isOpen = true;
    m_mode = mode;
    m_forceTabSelect = true;

    if (mode == Mode::Write) {
        // Refresh cached data
        RefreshWriteSelection();
    }
}

void FloppyDiskView::Close() { m_isOpen = false; }

void FloppyDiskView::RunInfo()
{
    Open(Mode::Write);
    m_writer.RunInfo();
}

void FloppyDiskView::RunBandwidth()
{
    Open(Mode::Write);
    m_writer.RunBandwidth();
}

void FloppyDiskView::RunRPM()
{
    Open(Mode::Write);
    m_writer.RunRPM(s_drives[m_driveIndex]);
}

// Fire the read-complete callback exactly once per read. This lives outside Draw() on purpose:
// a read runs on a background thread and can finish at any point, including while the window is
// closed or collapsed - and when this ran from DrawConfigPanel(), that case silently threw away
// a disk that had already been pulled off the physical drive.
void FloppyDiskView::Poll()
{
    // A compare can finish while the window is shut just as a read can, and its result should
    // be waiting when the window comes back.
    if (!m_compareFinished && m_compareReader.GetState() == core::ReadState::Done) {
        m_compareFinished = true;
        FinishCompare();
    }

    if (m_reader.GetState() != core::ReadState::Done || m_readCompleteFired)
        return;
    m_readCompleteFired = true;
    if (m_onReadComplete)
        m_onReadComplete(m_reader.CopyBuffer());
}

void FloppyDiskView::Draw()
{
    if (!m_isOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(740, 760), ImGuiCond_FirstUseEver);

    // The "###Write to Disk" id is kept from this window's original Write-only incarnation so
    // saved dock layouts from before the Read/Write merge still find it.
    if (!ImGui::Begin(ICON_FA_FLOPPY_DISK " Floppy Disk I/O###Write to Disk", &m_isOpen)) {
        ImGui::End();
        return;
    }

    DrawConfigPanel();
    ImGui::Separator();
    DrawProgressSection();
    ImGui::Separator();
    DrawOutputLog();

    ImGui::End();
}

void FloppyDiskView::DrawConfigPanel()
{
    auto writeState = m_writer.GetState();
    auto readState = m_reader.GetState();
    bool writeBusy =
        (writeState == core::WriteState::Writing || writeState == core::WriteState::Verifying ||
         writeState == core::WriteState::Starting);
    bool readBusy = (readState == core::ReadState::Reading);
    bool compareBusy = (m_compareReader.GetState() == core::ReadState::Reading);
    bool anyBusy = writeBusy || readBusy || compareBusy;

    // Drive/format selection - shared by both modes.
    ImGui::BeginDisabled(anyBusy);

    ImGui::SetNextItemWidth(100);
    if (ImGui::BeginCombo("Drive", s_drives[m_driveIndex])) {
        for (int i = 0; i < s_numDrives; ++i) {
            bool selected = (m_driveIndex == i);
            if (ImGui::Selectable(s_drives[i], selected)) {
                m_driveIndex = i;
                SaveSettings();
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(300);
    if (ImGui::BeginCombo("Format", s_formats[m_formatIndex])) {
        for (int i = 0; i < s_numFormats; ++i) {
            bool selected = (m_formatIndex == i);
            if (ImGui::Selectable(s_formats[i], selected)) {
                m_formatIndex = i;
                SaveSettings();
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::EndDisabled();

    ImGui::Spacing();

    // Mode tabs. Each tab owns its own options/action button and, crucially, its own ID scope:
    // BeginTabItem() pushes the tab's id onto the ID stack for its content, so the "Read from
    // Disk" tab label and the "Read from Disk" action button inside it hash to different ImGuiIDs
    // instead of colliding (which a plain label-matched button/radio pair sitting in the same ID
    // scope would do). Locked while either backend is busy so you can't switch away mid-operation.
    if (ImGui::BeginTabBar("FloppyDiskModeTabs")) {
        ImGuiTabItemFlags writeFlags =
            (m_forceTabSelect && m_mode == Mode::Write) ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags readFlags =
            (m_forceTabSelect && m_mode == Mode::Read) ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags formatFlags =
            (m_forceTabSelect && m_mode == Mode::Format) ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags compareFlags =
            (m_forceTabSelect && m_mode == Mode::Compare) ? ImGuiTabItemFlags_SetSelected : 0;

        ImGui::BeginDisabled(anyBusy);
        bool writeTabOpen = ImGui::BeginTabItem("Write to Disk", nullptr, writeFlags);
        ImGui::EndDisabled();
        if (anyBusy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Mode is locked while an operation is running.");
        }
        if (writeTabOpen) {
            m_mode = Mode::Write;
            ImGui::Spacing();
            DrawWriteConfig(writeBusy);
            ImGui::EndTabItem();
        }

        ImGui::BeginDisabled(anyBusy);
        bool readTabOpen = ImGui::BeginTabItem("Read from Disk", nullptr, readFlags);
        ImGui::EndDisabled();
        if (anyBusy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Mode is locked while an operation is running.");
        }
        if (readTabOpen) {
            m_mode = Mode::Read;
            ImGui::Spacing();
            DrawReadConfig(readBusy);
            ImGui::EndTabItem();
        }

        ImGui::BeginDisabled(anyBusy);
        bool formatTabOpen = ImGui::BeginTabItem("Format Disk", nullptr, formatFlags);
        ImGui::EndDisabled();
        if (anyBusy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Mode is locked while an operation is running.");
        }
        if (formatTabOpen) {
            m_mode = Mode::Format;
            ImGui::Spacing();
            DrawFormatConfig(writeBusy);
            ImGui::EndTabItem();
        }

        ImGui::BeginDisabled(anyBusy);
        bool compareTabOpen = ImGui::BeginTabItem("Compare", nullptr, compareFlags);
        ImGui::EndDisabled();
        if (anyBusy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Mode is locked while an operation is running.");
        }
        if (compareTabOpen) {
            m_mode = Mode::Compare;
            ImGui::Spacing();
            DrawCompareConfig(compareBusy);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
        m_forceTabSelect = false; // consumed - the tab bar now owns selection until the next Open()
    }
}

// The pinned document can vanish between frames - closed, or never there in the first place
// because the selection was restored from preferences. Falling back to the active document
// keeps the Write tab pointed at something writable rather than at a stale id.
void FloppyDiskView::RefreshWriteSource()
{
    if (!m_onGetWriteSource) {
        m_writeSource = {};
        return;
    }
    m_writeSource = m_onGetWriteSource(m_writeDocId);
    if (m_writeDocId != 0 && m_writeSource.id == 0)
        m_writeDocId = 0; // the pinned document is gone; follow the active one again
}

// The NDOS scan behind "Fast write" walks every block of the image, so it is pulled only when
// the selection changes and once more immediately before a write - never per frame.
void FloppyDiskView::RefreshWriteSelection()
{
    RefreshWriteSource();
    m_cachedZeroFilledBlocks =
        m_onGetZeroFilledBlocks ? m_onGetZeroFilledBlocks(m_writeSource.id) : std::vector<bool>{};
}

// Which of the open images to write. Before this existed the Write tab could only ever write
// whichever document happened to be active, so switching images meant leaving this window,
// clicking a tab and coming back.
void FloppyDiskView::DrawImageSelector(bool busy)
{
    const std::vector<DiskDocumentInfo> docs =
        m_onGetDocuments ? m_onGetDocuments() : std::vector<DiskDocumentInfo>{};

    std::string preview;
    if (m_writeDocId == 0) {
        preview = m_writeSource.name.empty()
                      ? std::string("Active document (none open)")
                      : fmt::format("Active document ({})", m_writeSource.name);
    } else {
        preview = m_writeSource.name;
    }

    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(300);
    if (ImGui::BeginCombo("Image", preview.c_str())) {
        // "Active document" stays at the top: it is the default, and it is the only entry that
        // keeps tracking the app rather than pinning one image.
        const bool followActive = (m_writeDocId == 0);
        if (ImGui::Selectable("Active document", followActive)) {
            m_writeDocId = 0;
            RefreshWriteSelection();
        }
        if (followActive)
            ImGui::SetItemDefaultFocus();

        if (!docs.empty())
            ImGui::Separator();
        for (const auto& doc : docs) {
            const bool selected = (m_writeDocId == doc.id);
            std::string label = doc.name;
            if (doc.mountedOnDrive >= 0)
                label += fmt::format("  (DF{})", doc.mountedOnDrive);
            if (ImGui::Selectable(fmt::format("{}##doc{}", label, doc.id).c_str(), selected)) {
                m_writeDocId = doc.id;
                RefreshWriteSelection();
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}

void FloppyDiskView::DrawWriteConfig(bool busy)
{
    // Re-resolved every frame rather than cached at Open(): with "Active document" selected this
    // is what follows the app, and with a document pinned it is what notices a Save As.
    RefreshWriteSource();
    DrawImageSelector(busy);

    ImGui::Text("Source:");
    ImGui::SameLine();
    if (m_writeSource.id == 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "No disk image open");
    } else if (m_writeSource.hasFile) {
        ImGui::TextWrapped("%s", m_writeSource.filePath.c_str());
    } else if (m_writeSource.hasBytes) {
        // A disk read off a physical drive, a TOSEC download or a New Disk has no file behind
        // it; gw gets a temp file written from the image held in memory.
        ImGui::TextWrapped("%s (in memory)", m_writeSource.name.c_str());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Nothing to write");
    }

    ImGui::Spacing();

    ImGui::BeginDisabled(busy);

    if (ImGui::Checkbox("Verify after write", &m_verify)) {
        SaveSettings();
    }

    if (ImGui::Checkbox("Pre-erase (clean rewrite)", &m_preErase)) {
        SaveSettings();
    }

    if (ImGui::Checkbox("Erase empty tracks", &m_eraseEmpty)) {
        SaveSettings();
    }

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Skips writing tracks that are empty in the ADF.\n"
                          "Use '--pre-erase' to ensure the disk is clean first.");
    }

    bool hasNdosData = !m_cachedZeroFilledBlocks.empty();
    if (!hasNdosData) {
        ImGui::BeginDisabled();
    }
    ImGui::Checkbox("Fast write (skip empty tracks)", &m_fastWrite);
    if (!hasNdosData) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Requires NDOS block data (load an ADF first)");
        }
    }

    ImGui::EndDisabled(); // busy

    ImGui::Spacing();

    // Action buttons
    bool haveSource = m_writeSource.hasFile || m_writeSource.hasBytes;
    bool canWrite = haveSource && !busy;

    if (!canWrite) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Write to Disk", ImVec2(0, 0))) {
        // Refresh data right before write
        if (m_fastWrite)
            RefreshWriteSelection();
        else
            RefreshWriteSource();
        StartWrite();
    }
    if (!canWrite) {
        ImGui::EndDisabled();
    }
    if (!haveSource && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(m_writeSource.id == 0 ? "Open a disk image, or pick one above."
                                                : "This document has no image behind it yet.");
    }

    DrawWriterActionRow(busy);
}

// Info/Bandwidth/RPM/Cancel plus the writer's state and elapsed time. Write and Format both
// drive the same GreaseweazelWriter, so they show the same row rather than two copies of it.
void FloppyDiskView::DrawWriterActionRow(bool busy)
{
    ImGui::SameLine();
    if (ImGui::Button("Info")) {
        m_writer.RunInfo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Bandwidth")) {
        m_writer.RunBandwidth();
    }
    ImGui::SameLine();
    if (ImGui::Button("RPM")) {
        m_writer.RunRPM(s_drives[m_driveIndex]);
    }

    ImGui::SameLine();

    if (!busy) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        m_writer.Cancel();
    }
    if (!busy) {
        ImGui::EndDisabled();
    }

    auto state = m_writer.GetState();

    // Stop timer when the operation finishes (Done, Error, or Cancelled)
    if (m_timerRunning) {
        if (state == core::WriteState::Done || state == core::WriteState::Error ||
            state == core::WriteState::Cancelled) {
            m_timerRunning = false;
            m_finalElapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - m_timerStart)
                    .count();
        }
    }

    double elapsed =
        m_timerRunning
            ? std::chrono::duration<double>(std::chrono::steady_clock::now() - m_timerStart).count()
            : m_finalElapsed;
    int minutes = static_cast<int>(elapsed) / 60;
    int seconds = static_cast<int>(elapsed) % 60;

    ImGui::SameLine();
    switch (state) {
        case core::WriteState::Idle:
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Ready");
            break;
        case core::WriteState::Starting:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Starting... %dm:%02ds", minutes,
                               seconds);
            break;
        case core::WriteState::Writing:
            ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "Writing... %dm:%02ds", minutes,
                               seconds);
            break;
        case core::WriteState::Verifying:
            ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "Verifying... %dm:%02ds", minutes,
                               seconds);
            break;
        case core::WriteState::Done:
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Done! %dm:%02ds", minutes, seconds);
            break;
        case core::WriteState::Error:
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error %dm:%02ds", minutes, seconds);
            break;
        case core::WriteState::Cancelled:
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Cancelled %dm:%02ds", minutes,
                               seconds);
            break;
    }
}

// Formatting a physical disk is a write of a blank filesystem, so it runs through the same
// GreaseweazelWriter as the Write tab and shares its progress bar, track grid and log. What it
// does not share is the source: the image comes from the same blank ADF templates the New Disk
// menu offers, built fresh with the label typed here.
void FloppyDiskView::DrawFormatConfig(bool busy)
{
    const std::vector<std::string> templates =
        m_onGetDiskTemplates ? m_onGetDiskTemplates() : std::vector<std::string>{};

    // The list is scanned once at startup, so the first draw is where no selection at all - or
    // one naming a template that is no longer there - settles onto something real.
    if (!templates.empty() &&
        std::find(templates.begin(), templates.end(), m_formatTemplate) == templates.end()) {
        m_formatTemplate = templates.front();
    }

    ImGui::TextWrapped("Writes a blank filesystem to the disk in drive %s. Everything on that "
                       "disk is destroyed.",
                       s_drives[m_driveIndex]);

    ImGui::Spacing();

    ImGui::BeginDisabled(busy);

    ImGui::SetNextItemWidth(300);
    const char* preview = templates.empty() ? "No templates found" : m_formatTemplate.c_str();
    if (ImGui::BeginCombo("Filesystem", preview)) {
        for (const auto& name : templates) {
            const bool selected = (name == m_formatTemplate);
            if (ImGui::Selectable(name.c_str(), selected))
                m_formatTemplate = name;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The same blank images the New Disk menu creates from,\nread out of "
                          "images/.");
    }

    ImGui::SetNextItemWidth(300);
    ImGui::InputText("Volume label", m_formatLabel, sizeof(m_formatLabel));

    if (ImGui::Checkbox("Verify after write", &m_verify)) {
        SaveSettings();
    }
    if (ImGui::Checkbox("Pre-erase (clean rewrite)", &m_preErase)) {
        SaveSettings();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Erases each track before writing it. Slower, but it is what\n"
                          "clears whatever the disk held before - worth it on media\n"
                          "that was last written in another format.");
    }

    ImGui::EndDisabled();

    ImGui::Spacing();

    const bool canFormat = !templates.empty() && m_onBuildFormatImage && !busy;
    ImGui::BeginDisabled(!canFormat);
    if (ImGui::Button("Format Disk", ImVec2(0, 0))) {
        StartFormat();
    }
    ImGui::EndDisabled();
    if (templates.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("No blank images in images/ to format from.");
    }

    DrawWriterActionRow(busy);
}

void FloppyDiskView::DrawReadConfig(bool busy)
{
    ImGui::TextWrapped(
        "Reads the full 880KB disk in drive %s and opens it as a new document, block by block.",
        s_drives[m_driveIndex]);

    ImGui::Spacing();

    ImGui::BeginDisabled(busy);
    if (ImGui::Checkbox("Verify (read twice, compare)", &m_readVerify)) {
        SaveSettings();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Re-reads every cylinder and compares it against the first read.\n"
                          "Roughly doubles read time but flags tracks that decoded\n"
                          "differently between the two passes - a sign of a flaky\n"
                          "drive or marginal media rather than a clean read.");
    }

    ImGui::Spacing();

    if (busy) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Read from Disk", ImVec2(0, 0))) {
        StartRead();
    }
    if (busy) {
        ImGui::EndDisabled();
    }

    DrawReaderActionRow(m_reader, busy);
}

// Info/Bandwidth/RPM/Cancel plus the reader's state and elapsed time: the reader-side twin of
// DrawWriterActionRow, shared by Read and Compare (each of which drives a reader of its own).
void FloppyDiskView::DrawReaderActionRow(core::GreaseweazelReader& reader, bool busy)
{
    ImGui::SameLine();
    if (ImGui::Button("Info")) {
        m_writer.RunInfo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Bandwidth")) {
        m_writer.RunBandwidth();
    }
    ImGui::SameLine();
    if (ImGui::Button("RPM")) {
        m_writer.RunRPM(s_drives[m_driveIndex]);
    }

    ImGui::SameLine();

    if (!busy) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        reader.Cancel();
    }
    if (!busy) {
        ImGui::EndDisabled();
    }

    auto state = reader.GetState();

    if (m_timerRunning) {
        if (state == core::ReadState::Done || state == core::ReadState::Error ||
            state == core::ReadState::Cancelled) {
            m_timerRunning = false;
            m_finalElapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - m_timerStart)
                    .count();
        }
    }

    double elapsed =
        m_timerRunning
            ? std::chrono::duration<double>(std::chrono::steady_clock::now() - m_timerStart).count()
            : m_finalElapsed;
    int minutes = static_cast<int>(elapsed) / 60;
    int seconds = static_cast<int>(elapsed) % 60;

    ImGui::SameLine();
    switch (state) {
        case core::ReadState::Idle:
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Ready");
            break;
        case core::ReadState::Reading:
            ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "Reading... %dm:%02ds", minutes,
                               seconds);
            break;
        case core::ReadState::Done:
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Done! %dm:%02ds", minutes, seconds);
            break;
        case core::ReadState::Error:
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error %dm:%02ds", minutes, seconds);
            break;
        case core::ReadState::Cancelled:
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Cancelled %dm:%02ds", minutes,
                               seconds);
            break;
    }

    if (state == core::ReadState::Error) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
                           "Check that GreaseWeazle is connected and a disk is inserted.");
    }
}

// Reads the disk in the drive and compares it, sector by sector, with an open image. Each sector
// is hashed on both sides (core::CompareDiskImages); a track is the same only when all eleven of
// its sectors are.
void FloppyDiskView::DrawCompareConfig(bool busy)
{
    // The Write tab's image selector: "the image I mean" is one choice, not one per tab.
    RefreshWriteSource();
    DrawImageSelector(busy);

    ImGui::TextWrapped("Reads the disk in drive %s and compares every sector with the selected "
                       "image. Tracks that match turn green, tracks that differ turn red.",
                       s_drives[m_driveIndex]);

    ImGui::Spacing();

    const bool haveImage = m_writeSource.id != 0 && m_onGetAdfData;
    ImGui::BeginDisabled(!haveImage || busy);
    if (ImGui::Button("Compare", ImVec2(0, 0))) {
        StartCompare();
    }
    ImGui::EndDisabled();
    if (!haveImage && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Open a disk image, or pick one above.");
    }

    DrawReaderActionRow(m_compareReader, busy);

    if (!m_compareError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_compareError.c_str());
    } else if (!m_compareSummary.empty()) {
        ImGui::TextColored(m_compareDetail.empty() ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                                                   : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "%s", m_compareSummary.c_str());
    }
}

void FloppyDiskView::StartWrite()
{
    core::WriteConfig config;
    config.adfPath = m_writeSource.hasFile ? m_writeSource.filePath : std::string();
    config.drive = s_drives[m_driveIndex];
    config.format = s_formats[m_formatIndex];
    config.verify = m_verify;
    config.fastWrite = m_fastWrite;
    config.preErase = m_preErase;
    config.eraseEmpty = m_eraseEmpty;
    config.zeroFilledBlocks = m_cachedZeroFilledBlocks;
    // Only fetched here, not per frame: this is the 880KB. RunWrite prefers adfData when it is
    // non-empty, spilling it to a temp file and pointing gw at that - which is how a container
    // (ZIP/DMS) and now an image with no file behind it at all both get written.
    if (m_writeSource.hasBytes && m_onGetAdfData) {
        config.adfData = m_onGetAdfData(m_writeSource.id);
    }

    m_timerRunning = true;
    m_timerStart = std::chrono::steady_clock::now();
    m_finalElapsed = 0.0;

    m_writer.Start(config);
}

void FloppyDiskView::StartFormat()
{
    core::WriteConfig config;
    config.drive = s_drives[m_driveIndex];
    config.format = s_formats[m_formatIndex];
    config.verify = m_verify;
    config.preErase = m_preErase;
    // Every one of the 160 tracks has to be written: a blank filesystem is mostly empty tracks,
    // and skipping them would leave whatever the disk held before sitting under the new root
    // block. So neither of the Write tab's two "skip empty tracks" options applies here.
    config.fastWrite = false;
    config.eraseEmpty = false;
    config.adfData = m_onBuildFormatImage
                         ? m_onBuildFormatImage(m_formatTemplate, std::string(m_formatLabel))
                         : std::vector<uint8_t>{};
    if (config.adfData.empty())
        return; // the builder reported the failure; there is nothing to write

    m_timerRunning = true;
    m_timerStart = std::chrono::steady_clock::now();
    m_finalElapsed = 0.0;

    m_writer.Start(config);
}

void FloppyDiskView::StartRead()
{
    m_readCompleteFired = false;

    core::ReadConfig config;
    config.drive = s_drives[m_driveIndex];
    config.format = s_formats[m_formatIndex];
    config.verify = m_readVerify;

    m_timerRunning = true;
    m_timerStart = std::chrono::steady_clock::now();
    m_finalElapsed = 0.0;

    m_reader.Start(config);
}

void FloppyDiskView::StartCompare()
{
    using Reader = core::GreaseweazelReader;

    m_compareError.clear();
    m_compareSummary.clear();
    m_compareDetail.clear();
    m_compareResults.clear();

    // Snapshotted now rather than when the read finishes: a minute of disk reading is plenty of
    // time for the document to be edited or closed.
    m_compareImage = m_onGetAdfData ? m_onGetAdfData(m_writeSource.id) : std::vector<uint8_t>{};
    m_compareImageName = m_writeSource.name;
    if (m_compareImage.size() != static_cast<size_t>(Reader::kTotalBytes)) {
        m_compareError =
            m_compareImage.empty()
                ? fmt::format("{} has no image to compare against.", m_compareImageName)
                : fmt::format("{} is {} bytes; the compare reads an {}-byte DD disk.",
                              m_compareImageName, m_compareImage.size(), Reader::kTotalBytes);
        m_compareImage.clear();
        return;
    }

    core::ReadConfig config;
    config.drive = s_drives[m_driveIndex];
    config.format = s_formats[m_formatIndex];

    m_compareFinished = false;
    m_timerRunning = true;
    m_timerStart = std::chrono::steady_clock::now();
    m_finalElapsed = 0.0;

    m_compareReader.Start(config);
}

void FloppyDiskView::FinishCompare()
{
    using Reader = core::GreaseweazelReader;

    m_compareResults =
        core::CompareDiskImages(m_compareReader.CopyBuffer(), m_compareImage, Reader::kTotalTracks,
                                Reader::kSectorsPerTrack, Reader::kBytesPerSector);
    m_compareImage.clear();
    m_compareImage.shrink_to_fit();

    int tracksDiffering = 0;
    int sectorsDiffering = 0;
    m_compareDetail.clear();
    for (int track = 0; track < static_cast<int>(m_compareResults.size()); ++track) {
        const uint32_t mask = m_compareResults[track].differingSectors;
        if (mask == 0)
            continue;
        ++tracksDiffering;
        std::string blocks;
        for (int sector = 0; sector < Reader::kSectorsPerTrack; ++sector) {
            if (!(mask & (1u << sector)))
                continue;
            ++sectorsDiffering;
            blocks += fmt::format("{}{}", blocks.empty() ? "" : ", ",
                                  track * Reader::kSectorsPerTrack + sector);
        }
        m_compareDetail += fmt::format("Cylinder {} head {} (track {}) differs: blocks {}\n",
                                       track / Reader::kHeadsPerCylinder,
                                       track % Reader::kHeadsPerCylinder, track, blocks);
    }

    m_compareSummary =
        tracksDiffering == 0
            ? fmt::format("The disk matches {}: all {} tracks are the same.", m_compareImageName,
                          Reader::kTotalTracks)
            : fmt::format("{} of {} tracks differ from {} ({} sectors).", tracksDiffering,
                          Reader::kTotalTracks, m_compareImageName, sectorsDiffering);
}

void FloppyDiskView::DrawProgressSection()
{
    float progressPercent = 0.0f;
    std::string statusMessage;

    // Read and Compare each run on a reader of their own; Write and Format share the writer.
    if (m_mode == Mode::Read || m_mode == Mode::Compare) {
        auto progress = (m_mode == Mode::Read ? m_reader : m_compareReader).GetProgress();
        progressPercent = progress.progressPercent;
        statusMessage = progress.statusMessage;
    } else {
        auto progress = m_writer.GetProgress();
        progressPercent = progress.progressPercent;
        statusMessage = progress.statusMessage;
    }

    // Progress bar
    ImGui::ProgressBar(progressPercent, ImVec2(-1, 0),
                       fmt::format("{:.0f}%", progressPercent * 100.0f).c_str());

    // Status text
    if (!statusMessage.empty()) {
        ImGui::TextWrapped("%s", statusMessage.c_str());
    }

    ImGui::Spacing();

    // Track grid
    DrawTrackGrid();
}

static ImU32 GetWriteTrackColor(core::TrackStatus status, float animTime)
{
    switch (status) {
        case core::TrackStatus::Pending:
            return IM_COL32(60, 60, 60, 255);
        case core::TrackStatus::Writing: {
            float pulse = 0.5f + 0.5f * sinf(animTime * 6.0f);
            int b = static_cast<int>(120 + pulse * 135);
            return IM_COL32(50, 80, b, 255);
        }
        case core::TrackStatus::Written:
            return IM_COL32(50, 200, 50, 255);
        case core::TrackStatus::Verifying: {
            float pulse = 0.5f + 0.5f * sinf(animTime * 6.0f);
            int p = static_cast<int>(100 + pulse * 155);
            return IM_COL32(120, 60, p, 255);
        }
        case core::TrackStatus::Verified:
            return IM_COL32(50, 255, 150, 255);
        case core::TrackStatus::Failed:
            return IM_COL32(255, 60, 60, 255);
        case core::TrackStatus::Skipped:
            return IM_COL32(30, 30, 30, 255);
    }
    return IM_COL32(60, 60, 60, 255);
}

static const char* GetWriteTrackStatusStr(core::TrackStatus status)
{
    switch (status) {
        case core::TrackStatus::Pending:
            return "Pending";
        case core::TrackStatus::Writing:
            return "Writing";
        case core::TrackStatus::Written:
            return "Written";
        case core::TrackStatus::Verifying:
            return "Verifying";
        case core::TrackStatus::Verified:
            return "Verified";
        case core::TrackStatus::Failed:
            return "Failed";
        case core::TrackStatus::Skipped:
            return "Skipped";
    }
    return "Unknown";
}

static ImU32 GetReadTrackColor(core::TrackReadStatus status, float animTime)
{
    switch (status) {
        case core::TrackReadStatus::Unloaded:
        case core::TrackReadStatus::Queued:
            return IM_COL32(60, 60, 60, 255);
        case core::TrackReadStatus::Reading: {
            float pulse = 0.5f + 0.5f * sinf(animTime * 6.0f);
            int b = static_cast<int>(120 + pulse * 135);
            return IM_COL32(50, 80, b, 255);
        }
        case core::TrackReadStatus::Loaded:
            return IM_COL32(50, 200, 50, 255);
        case core::TrackReadStatus::Verifying: {
            float pulse = 0.5f + 0.5f * sinf(animTime * 6.0f);
            int p = static_cast<int>(100 + pulse * 155);
            return IM_COL32(120, 60, p, 255);
        }
        case core::TrackReadStatus::Verified:
            return IM_COL32(50, 255, 150, 255);
        case core::TrackReadStatus::Failed:
            return IM_COL32(255, 60, 60, 255);
    }
    return IM_COL32(60, 60, 60, 255);
}

static const char* GetReadTrackStatusStr(core::TrackReadStatus status)
{
    switch (status) {
        case core::TrackReadStatus::Unloaded:
            return "Pending";
        case core::TrackReadStatus::Queued:
            return "Queued";
        case core::TrackReadStatus::Reading:
            return "Reading";
        case core::TrackReadStatus::Loaded:
            return "Loaded";
        case core::TrackReadStatus::Verifying:
            return "Verifying";
        case core::TrackReadStatus::Verified:
            return "Verified";
        case core::TrackReadStatus::Failed:
            return "Failed";
    }
    return "Unknown";
}

void FloppyDiskView::DrawTrackGrid()
{
    float animTime = static_cast<float>(ImGui::GetTime());

    // Resolve this frame's per-track color/status lookup for the active mode, so the grid geometry
    // below stays mode-agnostic.
    auto writeProgress = m_writer.GetProgress();
    auto readProgress = (m_mode == Mode::Compare ? m_compareReader : m_reader).GetProgress();

    // Compare draws its own result once there is one; until then (and after a read that failed
    // or was cancelled) it shows the read in progress, like the Read tab. A track the read gave
    // up on is amber there, not red: red means "read fine, and different".
    constexpr ImU32 kSameColor = IM_COL32(50, 200, 50, 255);
    constexpr ImU32 kDifferColor = IM_COL32(255, 60, 60, 255);
    constexpr ImU32 kUnreadColor = IM_COL32(255, 170, 40, 255);
    const bool compareMode = (m_mode == Mode::Compare);
    const bool haveCompareResult = compareMode && !m_compareResults.empty();

    auto trackColor = [&](int track) -> ImU32 {
        if (haveCompareResult) {
            if (track >= static_cast<int>(m_compareResults.size()))
                return IM_COL32(60, 60, 60, 255);
            return m_compareResults[track].Matches() ? kSameColor : kDifferColor;
        }
        if (compareMode && track < static_cast<int>(readProgress.trackStatuses.size()) &&
            readProgress.trackStatuses[track] == core::TrackReadStatus::Failed)
            return kUnreadColor;
        if (m_mode == Mode::Write || m_mode == Mode::Format) {
            auto status = track < static_cast<int>(writeProgress.trackStatuses.size())
                              ? writeProgress.trackStatuses[track]
                              : core::TrackStatus::Pending;
            return GetWriteTrackColor(status, animTime);
        }
        auto status = track < static_cast<int>(readProgress.trackStatuses.size())
                          ? readProgress.trackStatuses[track]
                          : core::TrackReadStatus::Unloaded;
        return GetReadTrackColor(status, animTime);
    };
    auto trackStatusStr = [&](int track) -> std::string {
        if (haveCompareResult && track < static_cast<int>(m_compareResults.size())) {
            const uint32_t mask = m_compareResults[track].differingSectors;
            if (mask == 0)
                return "Same";
            std::string sectors;
            for (int sector = 0; sector < core::GreaseweazelReader::kSectorsPerTrack; ++sector) {
                if (mask & (1u << sector))
                    sectors += fmt::format("{}{}", sectors.empty() ? "" : ", ", sector);
            }
            return fmt::format("Different (sectors {})", sectors);
        }
        if (m_mode == Mode::Write || m_mode == Mode::Format) {
            auto status = track < static_cast<int>(writeProgress.trackStatuses.size())
                              ? writeProgress.trackStatuses[track]
                              : core::TrackStatus::Pending;
            return GetWriteTrackStatusStr(status);
        }
        auto status = track < static_cast<int>(readProgress.trackStatuses.size())
                          ? readProgress.trackStatuses[track]
                          : core::TrackReadStatus::Unloaded;
        return GetReadTrackStatusStr(status);
    };

    // Legend
    auto drawLegendItem = [](ImU32 color, const char* label) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(pos, ImVec2(pos.x + 10, pos.y + 10), color);
        drawList->AddRect(pos, ImVec2(pos.x + 10, pos.y + 10), IM_COL32(80, 80, 80, 255));
        ImGui::Dummy(ImVec2(10, 10));
        ImGui::SameLine();
        ImGui::Text("%s", label);
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(8, 0));
        ImGui::SameLine();
    };

    if (compareMode) {
        drawLegendItem(IM_COL32(60, 60, 60, 255), "Pending");
        drawLegendItem(IM_COL32(50, 120, 255, 255), "Reading");
        drawLegendItem(kSameColor, "Same");
        drawLegendItem(kDifferColor, "Different");
        drawLegendItem(kUnreadColor, "Unread");
    } else if (m_mode != Mode::Read) {
        drawLegendItem(IM_COL32(60, 60, 60, 255), "Pending");
        drawLegendItem(IM_COL32(50, 120, 255, 255), "Writing");
        if (m_verify) {
            drawLegendItem(IM_COL32(50, 255, 150, 255), "Verified");
        } else {
            drawLegendItem(IM_COL32(50, 200, 50, 255), "Written");
        }
        drawLegendItem(IM_COL32(255, 60, 60, 255), "Failed");
        drawLegendItem(IM_COL32(30, 30, 30, 255), "Skipped");
    } else {
        drawLegendItem(IM_COL32(60, 60, 60, 255), "Pending");
        drawLegendItem(IM_COL32(50, 120, 255, 255), "Reading");
        if (m_readVerify) {
            drawLegendItem(IM_COL32(50, 255, 150, 255), "Verified");
        } else {
            drawLegendItem(IM_COL32(50, 200, 50, 255), "Loaded");
        }
        drawLegendItem(IM_COL32(255, 60, 60, 255), "Failed");
    }
    ImGui::NewLine();

    ImGui::Spacing();

    // Grid layout constants (shared by both sides)
    constexpr int numCylinders = 80;
    constexpr int cols = 10;
    constexpr int rows = numCylinders / cols; // 8
    constexpr float cellSize = 24.0f;
    constexpr float cellPadding = 2.0f;
    constexpr float gridStep = cellSize + cellPadding;
    constexpr float labelMarginX = 32.0f;   // Increased relative to previous
    constexpr float titleHeight = 20.0f;    // space for "Side N" title
    constexpr float colLabelHeight = 24.0f; // Increased from 16.0f
    constexpr float gapBetweenSides = 28.0f;

    float sideGridWidth = labelMarginX + cols * gridStep;
    float totalWidth = sideGridWidth + gapBetweenSides + sideGridWidth;
    float totalHeight = titleHeight + colLabelHeight + rows * gridStep;

    ImVec2 baseOrigin = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImU32 labelColor = IM_COL32(180, 180, 180, 255);
    ImU32 titleColor = IM_COL32(220, 220, 220, 255);

    // Draw both sides side-by-side
    for (int head = 0; head < 2; ++head) {
        float sideOffsetX = head * (sideGridWidth + gapBetweenSides);
        ImVec2 sideOrigin(baseOrigin.x + sideOffsetX, baseOrigin.y);

        // Side title
        auto title = fmt::format("Side {} (Head {})", head, head);
        drawList->AddText(ImVec2(sideOrigin.x + labelMarginX, sideOrigin.y), titleColor,
                          title.c_str());

        // Grid cells - Draw RECTANGLES FIRST (background)
        for (int cyl = 0; cyl < numCylinders; ++cyl) {
            int col = cyl % cols;
            int row = cyl / cols;
            int trackIndex = cyl * 2 + head;

            float x = sideOrigin.x + labelMarginX + col * gridStep;
            float y = sideOrigin.y + titleHeight + colLabelHeight + row * gridStep;

            ImU32 color = trackColor(trackIndex);

            ImVec2 p0(x, y);
            ImVec2 p1(x + cellSize, y + cellSize);
            drawList->AddRectFilled(p0, p1, color);
            drawList->AddRect(p0, p1, IM_COL32(80, 80, 80, 255));
        }

        // Draw LABELS ON TOP
        // Column labels (cylinder offset within row: 0-9)
        for (int c = 0; c < cols; ++c) {
            float x = sideOrigin.x + labelMarginX + c * gridStep + cellSize * 0.5f;
            float y = sideOrigin.y + titleHeight;
            auto label = fmt::format("{}", c);
            ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
            // Center text and align vertically
            drawList->AddText(
                ImVec2(x - textSize.x * 0.5f, y + (colLabelHeight - textSize.y) * 0.5f), labelColor,
                label.c_str());
        }

        // Row labels (on the left) and Tooltips
        for (int cyl = 0; cyl < numCylinders; ++cyl) {
            int col = cyl % cols;
            int row = cyl / cols;
            int trackIndex = cyl * 2 + head;

            float x = sideOrigin.x + labelMarginX + col * gridStep;
            float y = sideOrigin.y + titleHeight + colLabelHeight + row * gridStep;

            // Row label on the left (first cylinder of each row)
            if (col == 0) {
                auto rowLabel = fmt::format("{}", cyl);
                ImVec2 textSize = ImGui::CalcTextSize(rowLabel.c_str());
                drawList->AddText(ImVec2(x - textSize.x - 4.0f, y + (cellSize - textSize.y) * 0.5f),
                                  labelColor, rowLabel.c_str());
            }

            // Tooltip
            ImVec2 p0(x, y);
            ImVec2 p1(x + cellSize, y + cellSize);
            if (ImGui::IsMouseHoveringRect(p0, p1)) {
                bool isHD = (m_formatIndex == 1);
                const char* bitCell = isHD ? "1.0" : "2.0";
                const char* bitRate = isHD ? "500" : "250";
                int sectors = isHD ? 22 : 11;
                int dataBytes = sectors * 512;
                int rawMfmBits = isHD ? 200000 : 100000;

                ImGui::SetTooltip("Cylinder %d, Head %d (Track %d)\n"
                                  "Status: %s\n"
                                  "---\n"
                                  "Bit cell: %s us\n"
                                  "Bit rate: %s kbit/s\n"
                                  "Rotation: 300 RPM (200 ms/rev)\n"
                                  "Sectors: %d x 512 = %d bytes\n"
                                  "Raw MFM: ~%d bits/track",
                                  cyl, head, trackIndex, trackStatusStr(trackIndex).c_str(),
                                  bitCell, bitRate, sectors, dataBytes, rawMfmBits);
            }
        }
    }

    // Reserve space for the entire dual-grid area
    ImGui::Dummy(ImVec2(totalWidth, totalHeight));
}

void FloppyDiskView::DrawOutputLog()
{
    ImGui::Text("Output Log:");

    std::string output;
    if (m_mode == Mode::Read) {
        output = m_reader.GetOutput();
    } else if (m_mode == Mode::Compare) {
        // The gw read, then the verdict and the tracks behind it.
        output = m_compareReader.GetOutput();
        if (!m_compareSummary.empty())
            output += "\n" + m_compareSummary + "\n" + m_compareDetail;
    } else {
        output = m_writer.GetOutput();
    }

    float footerHeight = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("OutputLog", ImVec2(0, -footerHeight), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::TextUnformatted(output.c_str());

        // Auto-scroll
        if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
}

void FloppyDiskView::SaveSettings()
{
    if (m_onSettingsChanged) {
        m_onSettingsChanged(m_driveIndex, m_formatIndex, m_verify, m_preErase, m_eraseEmpty,
                            m_readVerify);
    }
}

} // namespace adm::ui
