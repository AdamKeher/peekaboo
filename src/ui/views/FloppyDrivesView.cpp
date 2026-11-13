#include "FloppyDrivesView.h"

#include <fmt/format.h>
#include <imgui.h>

#include "../DriveChips.h"
#include "../IconsFontAwesome6.h"

namespace adm::ui {

namespace {

const char* const kBridgeModes[] = {"Fast", "Compatible", "Turbo AmigaDOS", "Stalling"};

// Driver index of "XCopy Standalone (buffered)" in the core's driver list
// (patches/floppybridge/floppybridge_lib.cpp). Its measured-best configuration is
// Fast mode with read-ahead and smart speed OFF: raw per-track performance, no background
// caching. Choosing it applies those as the starting point; the other drivers keep whatever
// was set.
const int kXCopyBufferedDriver = 4;
constexpr int kNumBridgeModes = 4;

// Upstream's own descriptions, condensed. Worth having in the UI because the
// accuracy/speed trade-off is the main thing anyone will want to change.
const char* const kBridgeModeHelp[] = {
    "Reads a whole track as fast as the drive allows. Right for most disks and games.",
    "Streams in real time, matching the physical rotation. Slower, but copes with\n"
    "copy protection and anything that measures disk timing.",
    "Ignores rotation entirely and serves data on demand. Fastest, and wrong for\n"
    "anything that reads raw MFM or checks timing.",
    "Stalls the emulation while a track is fetched. Most compatible of all, at the\n"
    "cost of the machine visibly hitching on every seek."};

const char* const kDensities[] = {"Auto-detect", "DD only", "HD only"};
constexpr int kNumDensities = 3;

// FloppyBridge::DriveSelection. The default (drive A) is what an IBM PC cable
// with the twist selects; a straight cable or a Shugart drive needs one of the
// others, and getting it wrong is not obvious - the device opens, then fails
// with "Failed to find track 0" about a second later.
const char* const kDriveCables[] = {"Drive A (IBM PC, cable with twist)",
                                    "Drive B (IBM PC, before the twist)",
                                    "Shugart drive 0",
                                    "Shugart drive 1",
                                    "Shugart drive 2",
                                    "Shugart drive 3"};
constexpr int kNumDriveCables = 6;

const char* const kDriveSlots[] = {"DF0", "DF1", "DF2", "DF3"};
constexpr int kNumDriveSlots = 4;

// A slot's settings arrive from prefs and .peek files, and SetSlotConfig is public: a value
// past the end of its table shows as "?" rather than reading past the array.
template <size_t N> const char* NameAt(const char* const (&table)[N], int index)
{
    return index >= 0 && index < static_cast<int>(N) ? table[index] : "?";
}

// How long to keep saying "opening" after Attach before treating silence as a
// failure. A successful open against real hardware measured 0.8-1.2 s.
constexpr double kOpenTimeoutSeconds = 6.0;

// The live-drive briefing, shown on the (i) in the tab label. Everything that used to
// be a standing warning in the panel is folded in here instead.
const char* const kLiveDriveHelp =
    "Backed by a real, spinning floppy over FloppyBridge.\n\n"
    "Disk browsing and the block views need a file, and a live drive has none - use "
    "Floppy Disk I/O to image the disk first.\n\n"
    "Several slots can be live at once, one device per serial port. DF0 is the slot the "
    "machine boots from.\n\n"
    "Attach with the emulator stopped and the slot reads \"pending\": the device opens when "
    "the machine starts, and the assignment survives stop/start until you detach it.\n\n"
    "Save states, rewind and warp are unreliable while a drive is live. A platter cannot be "
    "rewound, a restored state cannot put the head back where it was, and the bridge has to "
    "be serviced in real time to stay in step with the disk's rotation.";

ImVec4 OkColor() { return ImVec4(0.35f, 0.80f, 0.35f, 1.0f); }
ImVec4 WarnColor() { return ImVec4(0.95f, 0.75f, 0.25f, 1.0f); }
ImVec4 ErrColor() { return ImVec4(0.95f, 0.40f, 0.40f, 1.0f); }
ImVec4 DimColor() { return ImVec4(0.65f, 0.65f, 0.65f, 1.0f); }
ImVec4 ImageColor() { return ImVec4(0.55f, 0.75f, 0.95f, 1.0f); }

void HelpMarker(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool ValidSlot(int slot) { return slot >= 0 && slot < kNumDriveSlots; }

} // namespace

FloppyDrivesView::FloppyDrivesView()
{
    for (int d = 0; d < kNumDriveSlots; ++d) {
        m_slots[d].driveSlot = d;
        m_awaitingSince[d] = -1.0;
        m_wasPending[d] = false;
        m_lastActionWasError[d] = false;
    }
}

void FloppyDrivesView::Open()
{
    m_isOpen = true;
    // Enumerate on open rather than in the constructor: a device plugged in
    // after startup should appear the first time the window is used.
    m_listsLoaded = false;
}

void FloppyDrivesView::SetSlotConfig(int slot, const Config& cfg)
{
    if (!ValidSlot(slot))
        return;
    m_slots[slot] = cfg;
    m_slots[slot].driveSlot = slot;
}

const FloppyDrivesView::Config& FloppyDrivesView::GetSlotConfig(int slot) const
{
    return m_slots[ValidSlot(slot) ? slot : 0];
}

void FloppyDrivesView::SetSelectedSlot(int slot)
{
    if (ValidSlot(slot))
        m_selectedSlot = slot;
}

void FloppyDrivesView::RefreshComPorts()
{
    if (m_onEnumComPorts)
        m_comPorts = m_onEnumComPorts();

    // Keep every remembered port selectable even when it is not currently present,
    // so unplugging a device does not silently rewrite the saved setting.
    for (int d = 0; d < kNumDriveSlots; ++d) {
        const std::string& want = m_slots[d].comPort;
        if (want.empty())
            continue;
        bool found = false;
        for (const auto& p : m_comPorts) {
            if (p == want) {
                found = true;
                break;
            }
        }
        if (!found)
            m_comPorts.push_back(want);
    }
}

void FloppyDrivesView::SaveSettings(int slot)
{
    if (m_onSettingsChanged && ValidSlot(slot))
        m_onSettingsChanged(m_slots[slot]);
}

int FloppyDrivesView::PortUsedByOtherSlot(int slot, const std::string& port) const
{
    if (port.empty())
        return -1;
    for (int d = 0; d < kNumDriveSlots; ++d) {
        if (d == slot || !m_status[d].assigned)
            continue;
        // The attached port is the authority; the slot's editable config is only
        // the fallback for a slot bridged from the console before it was recorded.
        const std::string& used =
            m_status[d].comPort.empty() ? m_slots[d].comPort : m_status[d].comPort;
        if (used == port)
            return d;
    }
    return -1;
}

const char* FloppyDrivesView::BackingLabel(int slot, bool* outIsWarn, bool* outIsError) const
{
    *outIsWarn = false;
    *outIsError = false;
    const Status& st = m_status[slot];
    if (!st.assigned)
        return st.imageMounted ? "image" : (st.enabled ? "empty" : "disabled");
    if (st.pending) {
        *outIsWarn = true;
        return "pending";
    }
    if (st.opened)
        return "live drive";
    if (m_awaitingSince[slot] >= 0.0 &&
        ImGui::GetTime() - m_awaitingSince[slot] < kOpenTimeoutSeconds) {
        *outIsWarn = true;
        return "opening...";
    }
    *outIsError = true;
    return "failed";
}

void FloppyDrivesView::Draw()
{
    if (!m_isOpen)
        return;

    // The core is loaded lazily, on the first emulator start. Until then it can be
    // asked nothing - which is not the same as it answering "unsupported", and the
    // window must not say so. Re-read the lists the moment it appears, because what
    // is shown before that is the host-side stand-in.
    const bool coreLoaded = m_onIsCoreLoaded ? m_onIsCoreLoaded() : false;
    if (coreLoaded != m_coreLoaded) {
        m_coreLoaded = coreLoaded;
        m_listsLoaded = false;
    }

    if (!m_listsLoaded) {
        if (m_onGetDriverList)
            m_drivers = m_onGetDriverList();
        RefreshComPorts();
        m_listsLoaded = true;
    }

    ImGui::SetNextWindowSize(ImVec2(680, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_FLOPPY_DISK " Floppy Drives###FloppyDrives", &m_isOpen)) {
        ImGui::End();
        return;
    }

    // Poll every slot once per frame and pass the snapshot down. Each call crosses into
    // the core, and the panels between them want each slot's status more than once.
    for (int d = 0; d < kNumDriveSlots; ++d) {
        m_status[d] = m_onGetStatus ? m_onGetStatus(d) : Status{};

        // A pending slot that the core has just been told about (the emulator
        // started) is now opening: start its grace period so it does not read as
        // "failed" for the second the device takes.
        const bool pendingNow = m_status[d].assigned && m_status[d].pending;
        if (m_wasPending[d] && !pendingNow && m_status[d].assigned)
            m_awaitingSince[d] = ImGui::GetTime();
        m_wasPending[d] = pendingNow;

        // Clear the "opening" grace period once the slot resolves either way.
        if (m_awaitingSince[d] >= 0.0) {
            const Status& st = m_status[d];
            if (st.opened || !st.assigned || st.pending || !st.lastError.empty())
                m_awaitingSince[d] = -1.0;
        }
    }

    DrawDriveList();
    ImGui::Separator();
    ImGui::Spacing();
    DrawSlotPanel(m_selectedSlot);

    ImGui::End();
}

void FloppyDrivesView::DrawDriveList()
{
    // One chip per slot, carrying everything the old nine-column table did: the enabled LED,
    // what backs the slot, the cylinder on a seven-segment readout, and motor / disk / write
    // protect. Five table rows collapse to one strip, and every drive's live state stays
    // visible at once - which is the only reason the table was there.
    std::vector<DriveChip> chips;
    chips.reserve(kNumDriveSlots);

    for (int d = 0; d < kNumDriveSlots; ++d) {
        const Status& st = m_status[d];
        bool warn = false, err = false;
        const char* backing = BackingLabel(d, &warn, &err);

        DriveChip chip;
        chip.name = kDriveSlots[d];
        chip.ledOn = st.enabled;

        // Backing: live hardware, a mounted image, or nothing. The glyph slot keeps its width
        // either way so the chips stay a fixed grid.
        if (st.assigned) {
            chip.backingGlyph = ICON_FA_HARD_DRIVE;
            chip.backingColor = err ? ErrColor() : warn ? WarnColor() : OkColor();
        } else if (st.imageMounted) {
            chip.backingGlyph = ICON_FA_FLOPPY_DISK;
            chip.backingColor = ImageColor();
        } else {
            // An empty slot still gets a glyph so the strip does not change width the moment
            // the first disk is mounted.
            chip.backingGlyph = ICON_FA_MINUS;
            chip.backingColor = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
        }

        // Cylinder. Application::GetLiveFloppyStatus falls back to gui_data for non-bridge
        // slots, so this is a real head position whatever backs the drive. Blank it only when
        // the slot is off, where "00" would read as "parked at cylinder 0".
        const bool hasPosition = st.enabled && (st.assigned || st.imageMounted);
        chip.digits = hasPosition ? st.cylinder : kNoDigits;
        // Same lamp rule as the emulator status bar: dim idle, bright reading, red writing.
        chip.digitsColor = !st.motor    ? ImVec4(0.28f, 0.50f, 0.28f, 1.0f)
                           : st.writing ? ImVec4(1.00f, 0.35f, 0.35f, 1.0f)
                                        : ImVec4(0.45f, 0.95f, 0.45f, 1.0f);

        // diskInDrive is only reported by the bridge; an image-backed slot has media whenever
        // one is mounted. The padlock is a button on image slots - a live drive's write
        // protection is the tab on the physical disk, which the bridge only reports.
        const bool wpClickable = !st.assigned && (st.imageMounted || st.enabled);
        ChipIndicator lock{ICON_FA_LOCK, st.writeProtected};
        lock.clickable = wpClickable;
        lock.litColor = ImVec4(0.95f, 0.72f, 0.25f, 1.0f);
        if (wpClickable)
            lock.tooltip =
                st.writeProtected
                    ? fmt::format("{} is write-protected - click to allow writes", kDriveSlots[d])
                    : fmt::format("{} is writable - click to write-protect it", kDriveSlots[d]);
        chip.indicators = {
            {ICON_FA_ARROW_ROTATE_RIGHT, st.motor},
            {ICON_FA_FLOPPY_DISK, st.diskInDrive || st.imageMounted},
            std::move(lock),
        };

        // One tooltip for the whole chip, in words - one hover explains every indicator, and it
        // avoids hit-testing each glyph inside the chip.
        std::string what;
        if (st.assigned)
            what = fmt::format("{} on {}",
                               st.driverName.empty() ? std::string("live drive") : st.driverName,
                               st.comPort.empty() ? std::string("its serial port") : st.comPort);
        else if (st.imageMounted)
            what = st.imageName.empty() ? std::string("disk image") : st.imageName;
        else
            what = backing;

        chip.tooltip =
            fmt::format("{} - {}\n{}\n{}\n{}", kDriveSlots[d], what,
                        st.motor ? "Motor running" : "Motor stopped",
                        (st.diskInDrive || st.imageMounted) ? "Disk inserted" : "No disk",
                        st.writeProtected ? "Write-protected" : "Writable");
        if (hasPosition)
            chip.tooltip += fmt::format("\nCylinder {}", st.cylinder);
        if (!st.lastError.empty())
            chip.tooltip += "\n" + st.lastError;

        chips.push_back(std::move(chip));
    }

    ChipIndicatorClick indicatorClick;
    const int clicked = DrawDriveChips("FloppyDriveChips", chips, m_selectedSlot, &indicatorClick);
    if (clicked >= 0) {
        m_selectedSlot = clicked;
        if (m_onSelectedSlotChanged)
            m_onSelectedSlotChanged(clicked);
    }
    // The padlock is the only clickable indicator, and it is always the last one.
    if (indicatorClick.chip >= 0 && m_onSetWriteProtect) {
        const int slot = indicatorClick.chip;
        m_onSetWriteProtect(slot, !m_status[slot].writeProtected);
    }
}

void FloppyDrivesView::DrawSlotPanel(int slot)
{
    if (!ValidSlot(slot))
        return;
    const Status& st = m_status[slot];

    ImGui::PushID(slot);

    // DF0 always exists; the others can be switched off in the machine configuration.
    // A live slot is enabled by definition - the bridge needs a drive to back.
    {
        bool enabled = st.enabled;
        const bool locked = (slot == 0) || st.assigned;
        ImGui::BeginDisabled(locked);
        if (ImGui::Checkbox("Enabled", &enabled) && m_onSetEnabled)
            m_onSetEnabled(slot, enabled);
        ImGui::EndDisabled();
        if (locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(slot == 0 ? "DF0 is always enabled."
                                        : "A live drive keeps its slot enabled.");
    }

    // Source. Each slot gets its own tab bar (PushID above), so switching drives keeps
    // each one's controls in view. A live slot is pinned to the live tab: one slot
    // cannot hold both a spinning platter and an image.
    if (ImGui::BeginTabBar("Source")) {
        ImGui::BeginDisabled(st.assigned);
        const bool imageTab = ImGui::BeginTabItem("Disk image");
        ImGui::EndDisabled();
        if (st.assigned && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Detach the live drive to put an image in this slot.");
        if (imageTab) {
            // A slot that has just gone live can still report this tab selected for one
            // frame: the tab bar acts on the Live tab's SetSelected below only on its next
            // layout pass. Draw nothing rather than image controls for a live slot.
            if (!st.assigned) {
                ImGui::Spacing();
                DrawImagePanel(slot);
            }
            ImGui::EndTabItem();
        }

        const ImGuiTabItemFlags liveFlags =
            st.assigned ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        const bool liveTab =
            ImGui::BeginTabItem("Live drive " ICON_FA_CIRCLE_INFO, nullptr, liveFlags);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::TextUnformatted(kLiveDriveHelp);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (liveTab) {
            ImGui::Spacing();
            DrawLivePanel(slot);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::PopID();
}

void FloppyDrivesView::DrawImagePanel(int slot)
{
    const Status& st = m_status[slot];

    ImGui::TextUnformatted("Mounted:");
    ImGui::SameLine();
    if (st.imageMounted)
        ImGui::TextColored(ImageColor(), "%s",
                           st.imageName.empty() ? "(image)" : st.imageName.c_str());
    else
        ImGui::TextColored(DimColor(), "nothing - the drive is empty");

    ImGui::Spacing();

    ImGui::BeginDisabled(!st.enabled);
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Mount file...", ImVec2(150, 0)) && m_onMountFile)
        m_onMountFile(slot);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!st.enabled)
            ImGui::SetTooltip("Enable the drive first.");
        else
            ImGui::SetTooltip("An ADF, extended ADF, DMS, IPF or SCP image, or a ZIP holding "
                              "one. The file opens as a document and is inserted here, so it "
                              "can be browsed while the machine reads it.");
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!st.imageMounted);
    if (ImGui::Button(ICON_FA_EJECT " Eject", ImVec2(120, 0)) && m_onEject)
        m_onEject(slot);
    ImGui::EndDisabled();

    {
        bool wp = st.writeProtected;
        ImGui::BeginDisabled(!st.enabled);
        if (ImGui::Checkbox("Write protect", &wp) && m_onSetWriteProtect)
            m_onSetWriteProtect(slot, wp);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Refuses writes from both the application and the emulated "
                              "Amiga, which sees a disk with its tab open. The same switch as "
                              "the padlock on the chip above and in the document window.");
    }

    // Open documents: the same list the emulator's Drives menu offers.
    if (m_onGetDocuments) {
        const auto docs = m_onGetDocuments();
        ImGui::Spacing();
        ImGui::TextUnformatted("Open documents");
        HelpMarker("Disk images open in the application. Mounting one here inserts the "
                   "document's current in-memory contents; edits you make to it are what the "
                   "machine sees on the next reset.");
        // The box is drawn whether or not anything is open: a list that appears and
        // disappears moves everything under it, and an empty box says "nothing open"
        // just as well as a label did.
        ImGui::BeginDisabled(!st.enabled);
        if (ImGui::BeginListBox("##OpenDocs",
                                ImVec2(-FLT_MIN, 6 * ImGui::GetTextLineHeightWithSpacing()))) {
            for (const auto& doc : docs) {
                const bool mountedHere = doc.mountedOnDrive == slot;
                const bool mountedElsewhere = doc.mountedOnDrive >= 0 && !mountedHere;
                std::string label = doc.name;
                if (mountedHere)
                    label += "  (mounted here)";
                else if (mountedElsewhere)
                    label += fmt::format("  (in DF{})", doc.mountedOnDrive);
                ImGui::BeginDisabled(mountedHere || mountedElsewhere);
                if (ImGui::Selectable(label.c_str(), mountedHere,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (m_onMountDocument)
                        m_onMountDocument(slot, doc.id);
                }
                ImGui::EndDisabled();
                if (mountedElsewhere && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Already mounted in DF%d. Eject it there first.",
                                      doc.mountedOnDrive);
            }
            ImGui::EndListBox();
        }
        ImGui::EndDisabled();
        ImGui::TextColored(DimColor(), "Click a document to mount it in %s.", kDriveSlots[slot]);
    }
}

void FloppyDrivesView::DrawLivePanel(int slot)
{
    Config& cfg = m_slots[slot];
    const Status& st = m_status[slot];
    const bool running = m_onIsRunning && m_onIsRunning();
    const bool slotAttached = st.assigned;

    // Only a core that is actually loaded can tell us it lacks support. Reporting
    // this before one is loaded is the bug this guard exists to prevent.
    if (m_coreLoaded && !st.supported) {
        ImGui::TextColored(ErrColor(), ICON_FA_TRIANGLE_EXCLAMATION
                           " This PuAE core has no FloppyBridge support.");
        ImGui::TextWrapped("Rebuild the core with scripts/build_puae.ps1 (or .sh). Live drive "
                           "support needs PuAE 1.36.0 or newer.");
        return;
    }

    if (!slotAttached && st.imageMounted) {
        ImGui::TextColored(WarnColor(), ICON_FA_CIRCLE_INFO
                           " Attaching replaces the mounted image: one slot cannot hold both.");
    }

    // The configuration is locked while the slot is attached: the core would
    // accept a change, but a half-edited config applied mid-session is a trap.
    // Detach, change, re-attach.
    ImGui::BeginDisabled(slotAttached);

    ImGui::SetNextItemWidth(220);
    const char* driverLabel =
        (cfg.hardwareType >= 0 && cfg.hardwareType < static_cast<int>(m_drivers.size()))
            ? m_drivers[cfg.hardwareType].c_str()
            : "(none)";
    if (ImGui::BeginCombo("Hardware", driverLabel)) {
        for (int i = 0; i < static_cast<int>(m_drivers.size()); ++i) {
            const bool selected = (cfg.hardwareType == i);
            if (ImGui::Selectable(m_drivers[i].c_str(), selected)) {
                cfg.hardwareType = i;
                if (i == kXCopyBufferedDriver) {
                    cfg.bridgeMode = 0; // Fast
                    cfg.autoCache = false;
                    cfg.smartSpeed = false;
                }
                // Changing hardware re-arms the safety: the flag was agreed for one
                // device and one disk, and driver 4 is the only one that honours it.
                cfg.allowWrites = false;
                SaveSettings(slot);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(220);
    const char* portLabel = cfg.comPort.empty() ? "(select a port)" : cfg.comPort.c_str();
    if (ImGui::BeginCombo("Serial port", portLabel)) {
        if (m_comPorts.empty())
            ImGui::TextDisabled("no serial ports found");
        for (const auto& port : m_comPorts) {
            const bool selected = (cfg.comPort == port);
            const int other = PortUsedByOtherSlot(slot, port);
            std::string item =
                other >= 0 ? fmt::format("{}  (live on {})", port, kDriveSlots[other]) : port;
            if (ImGui::Selectable(item.c_str(), selected)) {
                cfg.comPort = port;
                SaveSettings(slot);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_ROTATE_RIGHT " Refresh"))
        RefreshComPorts();

    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Drive select", NameAt(kDriveCables, cfg.driveCable))) {
        for (int i = 0; i < kNumDriveCables; ++i) {
            const bool selected = (cfg.driveCable == i);
            if (ImGui::Selectable(kDriveCables[i], selected)) {
                cfg.driveCable = i;
                SaveSettings(slot);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    HelpMarker("Which drive on the floppy cable to talk to.\n\n"
               "If the device opens and then fails with \"Failed to find track 0\", this is "
               "almost always the setting that is wrong - try Drive B next, then the Shugart "
               "entries. The wrong choice costs about a second before it reports the failure.");

    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Bridge mode", NameAt(kBridgeModes, cfg.bridgeMode))) {
        for (int i = 0; i < kNumBridgeModes; ++i) {
            const bool selected = (cfg.bridgeMode == i);
            if (ImGui::Selectable(kBridgeModes[i], selected)) {
                cfg.bridgeMode = i;
                SaveSettings(slot);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", kBridgeModeHelp[i]);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    HelpMarker(NameAt(kBridgeModeHelp, cfg.bridgeMode));

    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Density", NameAt(kDensities, cfg.bridgeDensity))) {
        for (int i = 0; i < kNumDensities; ++i) {
            const bool selected = (cfg.bridgeDensity == i);
            if (ImGui::Selectable(kDensities[i], selected)) {
                cfg.bridgeDensity = i;
                SaveSettings(slot);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (ImGui::Checkbox("Background read-ahead", &cfg.autoCache))
        SaveSettings(slot);
    HelpMarker("Cache tracks while the drive is otherwise idle, so the next track a trackloader "
               "asks for is already in memory.\n\n"
               "Without this every seek costs a full revolution before any data can be served - "
               "measured at about 2.5 revolutions per cylinder step against a real trackloading "
               "demo. The cost is that the motor keeps spinning and the head moves between "
               "loads, so the drive is audibly busier.\n\n"
               "Off by default for XCopy Standalone (buffered): its per-track read is the "
               "measured-best configuration without read-ahead.");

    if (ImGui::Checkbox("Smart speed", &cfg.smartSpeed))
        SaveSettings(slot);
    HelpMarker("Serve an already-captured track ten times faster than the platter turns.\n\n"
               "Applied per track, and only to tracks whose measured bitcell timing is even: "
               "cylinders 0-3 and anything with variable timing are always served at real speed, "
               "so timing-based copy protection still sees a real drive. Turn it off if a disk "
               "that used to load stops loading.");

    // The one destructive setting in this window, so it is last, it is off by
    // default, and it cannot be changed while the slot is attached - the driver reads
    // it once, when the device opens.
    ImGui::BeginDisabled(cfg.hardwareType != kXCopyBufferedDriver);
    if (ImGui::Checkbox("Allow writes to physical disk", &cfg.allowWrites))
        SaveSettings(slot);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(
            "Let the emulated Amiga write to the disk in the drive. OFF by default.\n\n"
            "With this off the machine sees a write-protected drive however the disk's own "
            "tab is set, and nothing it does can change the platter. With it on, the disk's "
            "tab decides - and a stray Format or Delete in Workbench is as permanent as it "
            "would be on real hardware.\n\n"
            "Takes effect when the device opens, so set it before Attach. XCopy Standalone "
            "(buffered) only: the other drivers have always followed the tab.");

    ImGui::EndDisabled();

    ImGui::Spacing();

    const int portClash = slotAttached ? -1 : PortUsedByOtherSlot(slot, cfg.comPort);
    const bool canAttach = !slotAttached && !cfg.comPort.empty() && portClash < 0;

    ImGui::BeginDisabled(!canAttach);
    if (ImGui::Button(ICON_FA_POWER_OFF " Attach", ImVec2(120, 0))) {
        m_lastActionMessage[slot].clear();
        m_lastActionWasError[slot] = false;
        if (m_onAttach && m_onAttach(cfg)) {
            if (running) {
                m_awaitingSince[slot] = ImGui::GetTime();
            } else {
                m_lastActionMessage[slot] =
                    fmt::format("{} is assigned to the live drive.", kDriveSlots[slot]);
            }
        } else {
            m_lastActionMessage[slot] =
                "Attach was rejected - check the hardware type and port, and that no other "
                "slot uses the same port.";
            m_lastActionWasError[slot] = true;
        }
    }
    ImGui::EndDisabled();
    if (!canAttach && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (slotAttached)
            ImGui::SetTooltip("%s is already attached. Detach first to change the configuration.",
                              kDriveSlots[slot]);
        else if (portClash >= 0)
            ImGui::SetTooltip("%s is already the live drive on %s. One device cannot be opened "
                              "twice - pick another port, or detach %s first.",
                              cfg.comPort.c_str(), kDriveSlots[portClash], kDriveSlots[portClash]);
        else
            ImGui::SetTooltip("Choose a serial port first.");
    }
    if (canAttach && !running && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The emulator is stopped: the slot is assigned now and the drive is "
                          "opened when the machine starts. The assignment survives stop/start "
                          "until you detach it.");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!slotAttached);
    if (ImGui::Button(ICON_FA_EJECT " Detach", ImVec2(120, 0))) {
        if (m_onDetach)
            m_onDetach(slot);
        m_awaitingSince[slot] = -1.0;
        m_lastActionMessage[slot] =
            fmt::format("{} is file-backed again, and empty.", kDriveSlots[slot]);
        m_lastActionWasError[slot] = false;
    }
    ImGui::EndDisabled();

    if (!m_lastActionMessage[slot].empty()) {
        ImGui::TextColored(m_lastActionWasError[slot] ? ErrColor() : DimColor(), "%s",
                           m_lastActionMessage[slot].c_str());
    }

    // A pending slot says nothing here: "pending" is already its Backing in the list
    // above, and the tab's (i) explains what it means.
    if (slotAttached && !st.pending && !st.opened && !st.lastError.empty()) {
        ImGui::TextColored(ErrColor(), ICON_FA_TRIANGLE_EXCLAMATION " %s", st.lastError.c_str());
        ImGui::TextWrapped("The slot stays assigned but the emulated drive is dead. Detach, fix "
                           "the setting, and attach again.");
    } else if (slotAttached && st.opened && !st.lastError.empty()) {
        // A message on a drive that did open is a compatibility warning, not a
        // failure - upstream uses the same channel for both.
        ImGui::TextColored(WarnColor(), ICON_FA_CIRCLE_INFO " %s", st.lastError.c_str());
    } else if (slotAttached && st.opened && !st.diskInDrive) {
        ImGui::TextColored(DimColor(),
                           "Drive is live and empty. Insert a disk - the guest will see it.");
    }
}

} // namespace adm::ui
