#include "BootBlockView.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <set>
#include "../../core/AmigaEmulator.h"
#include "../../core/BlockAnalyzer.h"
#include "../IconsFontAwesome6.h"
#include "../utils/ImGuiWidgets.h"

namespace adm::ui {

namespace {
// Bucket label for an entry's class in the filter (unidentified -> "Unknown").
std::string ClassLabel(const core::BootBlockInfo& info)
{
    if (!info.identified || info.className.empty())
        return "Unknown";
    return info.className;
}

// Bucket label for an entry's Kickstart in the filter ("" -> "(none)").
std::string KsLabel(const core::BootBlockInfo& info)
{
    return info.kickstart.empty() ? "(none)" : info.kickstart;
}

bool ContainsCI(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
        return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}
} // namespace

BootBlockView::BootBlockView()
{
    // Configure for 32 bytes per line
    m_hexState.BytesPerLine = 32;
    m_hexState.AddressChars = 8;
    m_hexState.ReadOnly = true;
    m_hexState.UserData = this;

    // Custom address callback for bootblock (shows block:offset format)
    m_hexState.GetAddressNameCallback = [](ImGuiHexEditorState* state, int offset, char* buf,
                                           int size) -> bool {
        // Show as BB:OOOO where BB is block (0 or 1) and OOOO is offset within block
        int block = offset / 512;
        int blockOffset = offset % 512;
        snprintf(buf, size, "%02x:%04x", block, blockOffset);
        return true;
    };
}

void BootBlockView::DisplaySectors(const std::vector<uint8_t>& sector0,
                                   const std::vector<uint8_t>& sector1)
{
    m_sector0 = sector0;
    m_sector1 = sector1;

    // Combine into single buffer for hex display
    std::vector<uint8_t> combined;
    combined.reserve(sector0.size() + sector1.size());
    combined.insert(combined.end(), sector0.begin(), sector0.end());
    combined.insert(combined.end(), sector1.begin(), sector1.end());

    BlockEditorBase::SetData(combined);
}

void BootBlockView::SetData(const std::vector<uint8_t>& sector0,
                            const std::vector<uint8_t>& sector1)
{
    // This always reflects the currently loaded disk's boot block.
    m_currentS0 = sector0;
    m_currentS1 = sector1;

    if (m_selectedSource == Source::CurrentDisk)
        DisplaySectors(sector0, sector1);
}

void BootBlockView::SetBootBlockInfo(const core::BootBlockInfo& info)
{
    m_currentInfo = info;
    if (m_selectedSource == Source::CurrentDisk)
        m_info = info;
}

void BootBlockView::SelectCurrentDisk()
{
    bool changed = (m_selectedSource != Source::CurrentDisk);
    m_selectedSource = Source::CurrentDisk;
    m_selectedLibraryName.clear();
    DisplaySectors(m_currentS0, m_currentS1);
    m_info = m_currentInfo;
    if (changed)
        ++m_selectionVersion;
}

void BootBlockView::SelectLibraryEntry(const std::string& name)
{
    if (!m_library)
        return;

    std::vector<uint8_t> data = m_library->Read(name);
    if (data.size() < 1024)
        return;

    bool changed = (m_selectedSource != Source::Library || m_selectedLibraryName != name);
    m_selectedSource = Source::Library;
    m_selectedLibraryName = name;

    std::vector<uint8_t> s0(data.begin(), data.begin() + 512);
    std::vector<uint8_t> s1(data.begin() + 512, data.begin() + 1024);
    DisplaySectors(s0, s1);

    m_info = m_identify ? m_identify(s0, s1) : core::BootBlockInfo{};

    // Boot the Preview tab on the machine this boot block targets (derived from KS).
    ApplyPreviewModelForMachine(m_info.kickstart);

    if (changed)
        ++m_selectionVersion;
}

void BootBlockView::SetOnNavigate(std::function<void(uint32_t)> callback)
{
    m_onNavigate = std::move(callback);
}

void BootBlockView::Open() { m_isOpen = true; }

void BootBlockView::Close() { m_isOpen = false; }

std::string BootBlockView::GetFilesystemName(uint8_t flags) const
{
    switch (flags) {
        case 0:
            return "OFS (Old FileSystem)";
        case 1:
            return "FFS (Fast FileSystem)";
        case 2:
            return "OFS Intl";
        case 3:
            return "FFS Intl";
        case 4:
            return "OFS DC (DirCache)";
        case 5:
            return "FFS DC (DirCache)";
        case 6:
            return "FFS LN (Long Names)";
        default:
            return "Unknown";
    }
}

void BootBlockView::DrawBrainfileInfo()
{
    ImGui::Text("BRAINFILE IDENTIFICATION");
    ImGui::Separator();

    if (!m_info.identified) {
        ImGui::TextDisabled("Not identified in database");
        ImGui::Spacing();
        if (!m_info.crc32.empty()) {
            ImGui::Text("CRC32:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", m_info.crc32.c_str());
        }
        return;
    }

    // Name
    ImGui::Text("Name:");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", m_info.name.c_str());
    ImGui::Spacing();

    // Class (with color)
    ImGui::Text("Class:");
    ImGui::SameLine();
    ImU32 classColor = core::BrainFile::GetClassColor(m_info.classCode);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(classColor), "%s", m_info.className.c_str());

    if (m_info.isVirus) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[VIRUS]");
    }
    ImGui::Spacing();

    // Kickstart
    if (!m_info.kickstart.empty()) {
        ImGui::Text("Kickstart:");
        ImGui::SameLine();
        ImGui::Text("%s", m_info.kickstart.c_str());
        ImGui::Spacing();
    }

    // CRC32
    ImGui::Text("CRC32:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", m_info.crc32.c_str());
    ImGui::Spacing();

    // Bootable
    ImGui::Text("Bootable:");
    ImGui::SameLine();
    if (m_info.bootable) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Yes");
    } else {
        ImGui::TextDisabled("No");
    }
    ImGui::Spacing();

    // Notes
    ImGui::Text("Notes:");
    if (!m_info.notes.empty()) {
        ImGui::TextWrapped("%s", m_info.notes.c_str());
    } else {
        ImGui::SameLine();
        ImGui::Text("None");
    }
    ImGui::Spacing();
}

void BootBlockView::DrawRootblockLink(uint32_t block)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", block);

    // Navigation only makes sense for the loaded disk's boot block.
    bool clickable = (m_selectedSource == Source::CurrentDisk) && m_onNavigate;

    ImGui::PushStyleColor(ImGuiCol_Text,
                          clickable ? IM_COL32(100, 180, 255, 255) : IM_COL32(200, 200, 200, 255));
    ImGui::Text("%s", buf);
    ImGui::PopStyleColor();

    if (clickable) {
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Click to go to block %u", block);
        }
        if (ImGui::IsItemClicked())
            m_onNavigate(block);
    }
}

void BootBlockView::DrawStructureDetails()
{
    if (m_sector0.size() < 12) {
        ImGui::TextDisabled("Insufficient data for structure details");
        return;
    }

    ImGui::Text("STRUCTURE DETAILS");
    ImGui::Separator();

    // Read values from bootblock
    // DiskType: bytes 0-2 (should be "DOS")
    std::string diskType;
    diskType += static_cast<char>(m_sector0[0]);
    diskType += static_cast<char>(m_sector0[1]);
    diskType += static_cast<char>(m_sector0[2]);

    // Flags: byte 3
    uint8_t flags = m_sector0[3];
    std::string fsName = GetFilesystemName(flags);

    // Checksum: bytes 4-7
    uint32_t checksum =
        (static_cast<uint32_t>(m_sector0[4]) << 24) | (static_cast<uint32_t>(m_sector0[5]) << 16) |
        (static_cast<uint32_t>(m_sector0[6]) << 8) | static_cast<uint32_t>(m_sector0[7]);

    // Rootblock: bytes 8-11
    uint32_t rootblock =
        (static_cast<uint32_t>(m_sector0[8]) << 24) | (static_cast<uint32_t>(m_sector0[9]) << 16) |
        (static_cast<uint32_t>(m_sector0[10]) << 8) | static_cast<uint32_t>(m_sector0[11]);

    // Display in table format
    if (ImGui::BeginTable("##bootblock_details", 3, ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);

        // DiskType
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("DiskType");
        ImGui::TableNextColumn();
        ImGui::Text("%s", diskType.c_str());
        ImGui::TableNextColumn();
        if (diskType == "DOS") {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Valid DOS disk");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Non-DOS disk");
        }

        // Flags
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("Flags");
        ImGui::TableNextColumn();
        ImGui::Text("%u", flags);
        ImGui::TableNextColumn();
        ImGui::Text("%s", fsName.c_str());

        // Checksum
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("Checksum");
        ImGui::TableNextColumn();
        ImGui::Text("0x%08X", checksum);
        ImGui::TableNextColumn();
        bool checksumValid = core::BlockAnalyzer::ValidateBootBlockChecksum(m_data);
        if (checksumValid) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[OK]");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[ERROR]");
        }

        // Rootblock
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("Rootblock");
        ImGui::TableNextColumn();
        DrawRootblockLink(rootblock);
        ImGui::TableNextColumn();
        if (rootblock == 880) {
            ImGui::TextDisabled("Standard DD/HD value");
        } else if (rootblock == 0) {
            ImGui::TextDisabled("Not set (non-bootable)");
        } else {
            ImGui::Text("Custom value");
        }

        ImGui::EndTable();
    }
}

void BootBlockView::EnsureMetadata()
{
    if (!m_metaDirty)
        return;
    m_metaDirty = false;

    m_metaCache.clear();
    std::set<std::string> classes, kickstarts, machines;

    if (m_library) {
        for (const auto& entry : m_library->Entries()) {
            core::BootBlockInfo info =
                m_metadataProvider ? m_metadataProvider(entry.name) : core::BootBlockInfo{};
            classes.insert(ClassLabel(info));
            kickstarts.insert(KsLabel(info));
            machines.insert(core::BrainFile::GetMachineType(info.kickstart));
            m_metaCache.emplace(entry.name, std::move(info));
        }
    }

    auto buildOptions = [](const std::set<std::string>& src) {
        std::vector<std::string> out;
        out.reserve(src.size() + 1);
        out.push_back("All");
        for (const auto& s : src)
            out.push_back(s);
        return out;
    };
    m_classOptions = buildOptions(classes);
    m_ksOptions = buildOptions(kickstarts);
    m_machineOptions = buildOptions(machines);

    // Keep selected filters valid if the option set shrank.
    if (m_filterClassIdx >= static_cast<int>(m_classOptions.size()))
        m_filterClassIdx = 0;
    if (m_filterKsIdx >= static_cast<int>(m_ksOptions.size()))
        m_filterKsIdx = 0;
    if (m_filterMachineIdx >= static_cast<int>(m_machineOptions.size()))
        m_filterMachineIdx = 0;
}

const core::BootBlockInfo* BootBlockView::MetaFor(const std::string& name) const
{
    auto it = m_metaCache.find(name);
    return it == m_metaCache.end() ? nullptr : &it->second;
}

bool BootBlockView::PassesFilters(const std::string& name) const
{
    if (m_searchBuf[0] && !ContainsCI(name, m_searchBuf))
        return false;

    const core::BootBlockInfo* meta = MetaFor(name);
    core::BootBlockInfo empty;
    const core::BootBlockInfo& info = meta ? *meta : empty;

    if (m_filterClassIdx > 0 && m_filterClassIdx < static_cast<int>(m_classOptions.size()) &&
        ClassLabel(info) != m_classOptions[m_filterClassIdx])
        return false;
    if (m_filterKsIdx > 0 && m_filterKsIdx < static_cast<int>(m_ksOptions.size()) &&
        KsLabel(info) != m_ksOptions[m_filterKsIdx])
        return false;
    if (m_filterMachineIdx > 0 && m_filterMachineIdx < static_cast<int>(m_machineOptions.size()) &&
        core::BrainFile::GetMachineType(info.kickstart) != m_machineOptions[m_filterMachineIdx])
        return false;

    return true;
}

void BootBlockView::ApplyPreviewModelForMachine(const std::string& kickstart)
{
    if (m_previewModels.empty())
        return;
    std::string machine = core::BrainFile::GetMachineType(kickstart);
    if (machine == "Any")
        return; // leave the user's current model choice alone

    // Only record the preferred model index. DrawPreviewTab applies it (via
    // m_onSelectPreviewModel) when it (re)boots the preview, so we avoid poking the
    // emulator on every list selection.
    for (int i = 0; i < static_cast<int>(m_previewModels.size()); ++i) {
        if (ContainsCI(m_previewModels[i], machine)) {
            m_previewModelIdx = i;
            return;
        }
    }
}

void BootBlockView::DrawFilters()
{
    auto combo = [](const char* label, int& idx, const std::vector<std::string>& opts) {
        if (opts.empty())
            return;
        ImGui::SetNextItemWidth(-FLT_MIN);
        ComboFromVector(label, idx, opts);
    };

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##bb_search", "Search...", m_searchBuf, sizeof(m_searchBuf));
    combo("##bb_class", m_filterClassIdx, m_classOptions);
    combo("##bb_ks", m_filterKsIdx, m_ksOptions);
    combo("##bb_machine", m_filterMachineIdx, m_machineOptions);
}

void BootBlockView::DrawSourceList()
{
    EnsureMetadata();

    ImGui::TextDisabled("SOURCE");
    ImGui::Separator();

    bool curSelected = (m_selectedSource == Source::CurrentDisk);
    if (ImGui::Selectable("Current Disk", curSelected))
        SelectCurrentDisk();

    ImGui::Spacing();
    ImGui::TextDisabled("LIBRARY");
    ImGui::Separator();

    if (!m_library || m_library->Entries().empty()) {
        ImGui::TextDisabled("(empty)");
        return;
    }

    DrawFilters();
    ImGui::Spacing();

    int shown = 0;
    for (const auto& entry : m_library->Entries()) {
        if (!PassesFilters(entry.name))
            continue;
        ++shown;
        bool sel = (m_selectedSource == Source::Library && m_selectedLibraryName == entry.name);
        ImGui::PushID(entry.name.c_str());
        if (ImGui::Selectable(entry.name.c_str(), sel))
            SelectLibraryEntry(entry.name);

        // Right-click context menu for rename/delete.
        if (ImGui::BeginPopupContextItem("##entry_ctx")) {
            if (ImGui::MenuItem("Rename...")) {
                m_renameOldName = entry.name;
                strncpy(m_renameBuf, entry.name.c_str(), sizeof(m_renameBuf) - 1);
                m_renameBuf[sizeof(m_renameBuf) - 1] = 0;
                m_showRenameDialog = true;
            }
            if (ImGui::MenuItem("Delete...")) {
                m_deleteName = entry.name;
                m_showDeleteConfirm = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    if (shown == 0)
        ImGui::TextDisabled("(no matches)");
}

void BootBlockView::DrawLibraryToolbar()
{
    bool isCurrent = (m_selectedSource == Source::CurrentDisk);
    bool isLibrary = (m_selectedSource == Source::Library);

    // Right-align the button row: shift the cursor so the whole group ends flush
    // with the right edge of the content region.
    const ImGuiStyle& style = ImGui::GetStyle();
    auto buttonWidth = [&style](const char* label) {
        return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
    };
    const char* kAdd = "Add to Library";
    const char* kInstall = "Install to Disk";
    const char* kRename = "Rename";
    const char* kDelete = "Delete";
    const float totalWidth = buttonWidth(kAdd) + buttonWidth(kInstall) + buttonWidth(kRename) +
                             buttonWidth(kDelete) + style.ItemSpacing.x * 3.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > totalWidth)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - totalWidth));

    // Add to Library (from the current disk).
    bool canAdd = isCurrent && m_documentLoaded && !m_currentS0.empty();
    if (!canAdd)
        ImGui::BeginDisabled();
    if (ImGui::Button(kAdd)) {
        std::string def = (m_info.identified && !m_info.name.empty()) ? m_info.name : std::string();
        strncpy(m_addNameBuf, def.c_str(), sizeof(m_addNameBuf) - 1);
        m_addNameBuf[sizeof(m_addNameBuf) - 1] = 0;
        m_showLibAddDialog = true;
    }
    if (!canAdd)
        ImGui::EndDisabled();

    ImGui::SameLine();

    // Install to Disk (from a library entry).
    bool canInstall = isLibrary && m_documentLoaded && m_onInstallBootBlock && !m_sector0.empty();
    if (!canInstall)
        ImGui::BeginDisabled();
    if (ImGui::Button(kInstall))
        m_showInstallConfirm = true;
    if (!canInstall)
        ImGui::EndDisabled();

    ImGui::SameLine();

    // Rename / Delete (library entries only).
    bool hasEntry = isLibrary && !m_selectedLibraryName.empty();
    if (!hasEntry)
        ImGui::BeginDisabled();
    if (ImGui::Button(kRename)) {
        m_renameOldName = m_selectedLibraryName;
        strncpy(m_renameBuf, m_selectedLibraryName.c_str(), sizeof(m_renameBuf) - 1);
        m_renameBuf[sizeof(m_renameBuf) - 1] = 0;
        m_showRenameDialog = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(kDelete)) {
        m_deleteName = m_selectedLibraryName;
        m_showDeleteConfirm = true;
    }
    if (!hasEntry)
        ImGui::EndDisabled();
}

void BootBlockView::DrawLibraryAddDialog()
{
    if (!m_showLibAddDialog)
        return;

    ImGui::OpenPopup("Add Boot Block to Library");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Add Boot Block to Library", &m_showLibAddDialog,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save the current disk's boot block to the library.");
        ImGui::Spacing();
        ImGui::InputText("Name", m_addNameBuf, sizeof(m_addNameBuf));
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        std::string name = m_addNameBuf;
        bool valid = !name.empty() && m_library;
        if (!valid)
            ImGui::BeginDisabled();
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            if (m_library && m_library->Save(name, m_currentS0, m_currentS1)) {
                std::string stem = core::BootBlockLibrary::SanitizeName(name);
                if (m_onLibraryEntryAdded)
                    m_onLibraryEntryAdded(stem, m_currentS0, m_currentS1);
                m_metaDirty = true; // library + brainfile changed; rebuild metadata
                SelectLibraryEntry(stem);
            }
            m_showLibAddDialog = false;
            ImGui::CloseCurrentPopup();
        }
        if (!valid)
            ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_showLibAddDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void BootBlockView::DrawRenameDialog()
{
    if (!m_showRenameDialog)
        return;

    ImGui::OpenPopup("Rename Boot Block");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Rename Boot Block", &m_showRenameDialog,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("New name", m_renameBuf, sizeof(m_renameBuf));
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        std::string newName = m_renameBuf;
        bool valid = !newName.empty() && m_library;
        if (!valid)
            ImGui::BeginDisabled();
        if (ImGui::Button("Rename", ImVec2(120, 0))) {
            if (m_library && m_library->Rename(m_renameOldName, newName)) {
                m_metaDirty = true;
                SelectLibraryEntry(core::BootBlockLibrary::SanitizeName(newName));
            }
            m_showRenameDialog = false;
            ImGui::CloseCurrentPopup();
        }
        if (!valid)
            ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_showRenameDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void BootBlockView::DrawDeleteConfirm()
{
    if (!m_showDeleteConfirm)
        return;

    ImGui::OpenPopup("Delete Boot Block");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Delete Boot Block", &m_showDeleteConfirm,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete '%s' from the library?", m_deleteName.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Delete", ImVec2(120, 0))) {
            if (m_library)
                m_library->Delete(m_deleteName);
            m_metaDirty = true;
            if (m_selectedLibraryName == m_deleteName)
                SelectCurrentDisk();
            m_showDeleteConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_showDeleteConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void BootBlockView::DrawInstallConfirm()
{
    if (!m_showInstallConfirm)
        return;

    ImGui::OpenPopup("Install Boot Block");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Install Boot Block", &m_showInstallConfirm,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Install '%s' onto the current disk?", m_selectedLibraryName.c_str());
        ImGui::TextDisabled("This overwrites blocks 0 and 1 of the loaded disk.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Install", ImVec2(120, 0))) {
            if (m_onInstallBootBlock)
                m_onInstallBootBlock(m_sector0, m_sector1);
            m_showInstallConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_showInstallConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void BootBlockView::ReleasePreviewMouse()
{
    if (m_previewMouseCaptured) {
        m_previewMouseCaptured = false;
        if (m_window)
            glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        // Release any keys still held.
        if (m_previewSendKey) {
            for (int k : m_previewPrevKeys)
                m_previewSendKey(false, k);
        }
        m_previewPrevKeys.clear();
    }
}

void BootBlockView::HandlePreviewInput()
{
    using K = adm::AmigaEmulator::RetroKey;
    struct KeyMap
    {
        ImGuiKey imgui;
        K retro;
    };
    static const KeyMap kKeys[] = {
        {ImGuiKey_Backspace, K::Backspace},
        {ImGuiKey_Tab, K::Tab},
        {ImGuiKey_Enter, K::Return},
        {ImGuiKey_Escape, K::Escape},
        {ImGuiKey_Space, K::Space},
        {ImGuiKey_Delete, K::Delete},
        {ImGuiKey_UpArrow, K::Up},
        {ImGuiKey_DownArrow, K::Down},
        {ImGuiKey_LeftArrow, K::Left},
        {ImGuiKey_RightArrow, K::Right},
        {ImGuiKey_Insert, K::Insert},
        {ImGuiKey_Home, K::Help},
        {ImGuiKey_End, K::End},
        {ImGuiKey_PageUp, K::PageUp},
        {ImGuiKey_PageDown, K::PageDown},
        {ImGuiKey_F1, K::F1},
        {ImGuiKey_F2, K::F2},
        {ImGuiKey_F3, K::F3},
        {ImGuiKey_F4, K::F4},
        {ImGuiKey_F5, K::F5},
        {ImGuiKey_F6, K::F6},
        {ImGuiKey_F7, K::F7},
        {ImGuiKey_F8, K::F8},
        {ImGuiKey_F9, K::F9},
        {ImGuiKey_F10, K::F10},
        {ImGuiKey_F11, K::F11},
        {ImGuiKey_F12, K::F12},
        {ImGuiKey_GraveAccent, K::Backquote},
        {ImGuiKey_Minus, K::Minus},
        {ImGuiKey_Equal, K::Equals},
        {ImGuiKey_LeftBracket, K::LeftBracket},
        {ImGuiKey_RightBracket, K::RightBracket},
        {ImGuiKey_Backslash, K::Backslash},
        {ImGuiKey_Semicolon, K::Semicolon},
        {ImGuiKey_Apostrophe, K::Quote},
        {ImGuiKey_Comma, K::Comma},
        {ImGuiKey_Period, K::Period},
        {ImGuiKey_Slash, K::Slash},
        {ImGuiKey_0, K::Num0},
        {ImGuiKey_1, K::Num1},
        {ImGuiKey_2, K::Num2},
        {ImGuiKey_3, K::Num3},
        {ImGuiKey_4, K::Num4},
        {ImGuiKey_5, K::Num5},
        {ImGuiKey_6, K::Num6},
        {ImGuiKey_7, K::Num7},
        {ImGuiKey_8, K::Num8},
        {ImGuiKey_9, K::Num9},
        {ImGuiKey_A, K::A},
        {ImGuiKey_B, K::B},
        {ImGuiKey_C, K::C},
        {ImGuiKey_D, K::D},
        {ImGuiKey_E, K::E},
        {ImGuiKey_F, K::F},
        {ImGuiKey_G, K::G},
        {ImGuiKey_H, K::H},
        {ImGuiKey_I, K::I},
        {ImGuiKey_J, K::J},
        {ImGuiKey_K, K::K},
        {ImGuiKey_L, K::L},
        {ImGuiKey_M, K::M},
        {ImGuiKey_N, K::N},
        {ImGuiKey_O, K::O},
        {ImGuiKey_P, K::P},
        {ImGuiKey_Q, K::Q},
        {ImGuiKey_R, K::R},
        {ImGuiKey_S, K::S},
        {ImGuiKey_T, K::T},
        {ImGuiKey_U, K::U},
        {ImGuiKey_V, K::V},
        {ImGuiKey_W, K::W},
        {ImGuiKey_X, K::X},
        {ImGuiKey_Y, K::Y},
        {ImGuiKey_Z, K::Z},
        {ImGuiKey_CapsLock, K::CapsLock},
        {ImGuiKey_LeftShift, K::LShift},
        {ImGuiKey_RightShift, K::RShift},
        {ImGuiKey_LeftCtrl, K::LCtrl},
        {ImGuiKey_RightCtrl, K::RCtrl},
        {ImGuiKey_LeftAlt, K::LAlt},
        {ImGuiKey_RightAlt, K::RAlt},
        {ImGuiKey_LeftSuper, K::LSuper},
        {ImGuiKey_RightSuper, K::RSuper},
        {ImGuiKey_NumLock, K::NumLock},
        {ImGuiKey_Keypad0, K::Kp0},
        {ImGuiKey_Keypad1, K::Kp1},
        {ImGuiKey_Keypad2, K::Kp2},
        {ImGuiKey_Keypad3, K::Kp3},
        {ImGuiKey_Keypad4, K::Kp4},
        {ImGuiKey_Keypad5, K::Kp5},
        {ImGuiKey_Keypad6, K::Kp6},
        {ImGuiKey_Keypad7, K::Kp7},
        {ImGuiKey_Keypad8, K::Kp8},
        {ImGuiKey_Keypad9, K::Kp9},
        {ImGuiKey_KeypadDecimal, K::KpPeriod},
        {ImGuiKey_KeypadDivide, K::KpDivide},
        {ImGuiKey_KeypadMultiply, K::KpMultiply},
        {ImGuiKey_KeypadSubtract, K::KpMinus},
        {ImGuiKey_KeypadAdd, K::KpPlus},
        {ImGuiKey_KeypadEnter, K::KpEnter},
        {ImGuiKey_KeypadEqual, K::KpEquals},
    };

    ImGuiIO& io = ImGui::GetIO();
    bool isHovered = ImGui::IsWindowHovered();
    bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (m_previewMouseCaptured) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
            ImGui::IsKeyReleased(ImGuiKey_Escape) || !isFocused) {
            ReleasePreviewMouse();
        }
    } else {
        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_previewMouseCaptured = true;
            m_previewSuppressCaptureClick = true;
            ImGui::SetWindowFocus();
            if (m_window)
                glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }

    if (m_previewSendKey) {
        std::unordered_set<int> currentKeys;
        for (const auto& map : kKeys) {
            bool down = m_previewMouseCaptured && ImGui::IsKeyDown(map.imgui);
            int k = static_cast<int>(map.retro);
            if (down)
                currentKeys.insert(k);
            if (down && m_previewPrevKeys.count(k) == 0)
                m_previewSendKey(true, k);
            if (!down && m_previewPrevKeys.count(k) > 0)
                m_previewSendKey(false, k);
        }
        m_previewPrevKeys = std::move(currentKeys);
    }

    if (!m_previewMouseCaptured)
        return;

    int16_t dx = static_cast<int16_t>(io.MouseDelta.x);
    int16_t dy = static_cast<int16_t>(io.MouseDelta.y);
    bool left = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool right = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    bool middle = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    if (m_previewSuppressCaptureClick) {
        left = false;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            m_previewSuppressCaptureClick = false;
    }
    if (m_previewSetMouse)
        m_previewSetMouse(dx, dy, left, right, middle);
}

void BootBlockView::DrawPreviewTab()
{
    m_previewFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    bool canRun = !m_sector0.empty() && !m_sector1.empty() && m_onStartPreview;

    // Auto-(re)load when the selected boot block changes while this tab is open (also fires on
    // first open), so flicking through entries previews each one with no extra clicks.
    if (canRun && m_selectionVersion != m_previewedVersion) {
        if (m_onSelectPreviewModel)
            m_onSelectPreviewModel(m_previewModelIdx);
        m_onStartPreview(m_sector0, m_sector1);
        m_previewRunning = true;
        m_previewedVersion = m_selectionVersion;
    }

    // Toolbar: Run / Stop, model selector, hint.
    if (!m_previewRunning) {
        if (!canRun)
            ImGui::BeginDisabled();
        if (ImGui::Button("Run")) {
            if (m_onSelectPreviewModel)
                m_onSelectPreviewModel(m_previewModelIdx);
            m_onStartPreview(m_sector0, m_sector1);
            m_previewRunning = true;
            m_previewedVersion = m_selectionVersion;
        }
        if (!canRun)
            ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Stop")) {
            ReleasePreviewMouse();
            if (m_onStopPreview)
                m_onStopPreview();
            m_previewRunning = false;
        }
    }

    if (!m_previewModels.empty()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        const char* modelPreview = m_previewModels[m_previewModelIdx].c_str();
        if (ImGui::BeginCombo("##preview_model", modelPreview)) {
            for (int i = 0; i < static_cast<int>(m_previewModels.size()); ++i) {
                bool sel = (i == m_previewModelIdx);
                if (ImGui::Selectable(m_previewModels[i].c_str(), sel) && i != m_previewModelIdx) {
                    m_previewModelIdx = i;
                    ++m_selectionVersion; // re-run preview with the new model
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Boots the boot block in an isolated emulator.\n"
                          "Self-contained boot blocks (boot tools, intros, viruses) run;\n"
                          "track-loaders that need their original disk will hang.");

    ImGui::Separator();

    // Display area: render the preview emulator's framebuffer, letterboxed.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!m_previewRunning) {
        ImGui::TextDisabled("Press Run to preview this boot block.");
        return;
    }

    PreviewFrame frame = m_getPreviewFrame ? m_getPreviewFrame() : PreviewFrame{};
    if (!frame.valid || frame.texture == 0) {
        ImGui::TextDisabled("Starting emulator...");
        return;
    }

    float aspect = (frame.aspect > 0.0f) ? frame.aspect : (4.0f / 3.0f);
    float drawW = avail.x;
    float drawH = drawW / aspect;
    if (drawH > avail.y) {
        drawH = avail.y;
        drawW = drawH * aspect;
    }

    ImGui::BeginChild("##bb_preview_display", avail, false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoNavInputs);

    ImVec2 cursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(cursor.x + (avail.x - drawW) * 0.5f, cursor.y));
    ImGui::Image(frame.texture, ImVec2(drawW, drawH));

    // Capture hint overlay drawn over the image.
    ImVec2 imgMin = ImGui::GetItemRectMin();
    ImVec2 imgMax = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!m_previewMouseCaptured) {
        const char* hint = "Click to capture input";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        float px = imgMin.x + (imgMax.x - imgMin.x - ts.x) * 0.5f;
        float py = imgMax.y - ts.y - 6.0f;
        dl->AddRectFilled(ImVec2(px - 4, py - 2), ImVec2(px + ts.x + 4, py + ts.y + 2),
                          IM_COL32(0, 0, 0, 160), 3.0f);
        dl->AddText(ImVec2(px, py), IM_COL32(220, 220, 220, 255), hint);
    } else {
        const char* hint = "ESC / MMB to release";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        float px = imgMin.x + (imgMax.x - imgMin.x - ts.x) * 0.5f;
        float py = imgMax.y - ts.y - 6.0f;
        dl->AddRectFilled(ImVec2(px - 4, py - 2), ImVec2(px + ts.x + 4, py + ts.y + 2),
                          IM_COL32(0, 0, 0, 160), 3.0f);
        dl->AddText(ImVec2(px, py), IM_COL32(255, 200, 80, 255), hint);
    }

    HandlePreviewInput();
    m_previewFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::EndChild();
}

void BootBlockView::Draw()
{
    // Reset per-frame preview flags; DrawPreviewTab() re-asserts them when its tab is active.
    m_previewTabActive = false;
    m_previewFocused = false;

    if (!m_isOpen) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(1200, 800), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(ICON_FA_POWER_OFF " Boot Block Viewer###Boot Block Viewer", &m_isOpen)) {
        static constexpr float kSplitterW = 6.0f;
        static constexpr float kMinLeftW = 120.0f;
        static constexpr float kMinRightW = 200.0f;

        const ImVec2 outerAvail = ImGui::GetContentRegionAvail();

        // Reserve space at the bottom for the action toolbar (separator + button row).
        const float footerHeight =
            ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        float bodyHeight = outerAvail.y - footerHeight;
        if (bodyHeight < 100.0f)
            bodyHeight = 100.0f;

        m_leftWidth = std::clamp(m_leftWidth, kMinLeftW,
                                 std::max(kMinLeftW, outerAvail.x - kSplitterW - kMinRightW));

        // Left: source list (Current Disk + library entries).
        if (ImGui::BeginChild("##bb_sources", ImVec2(m_leftWidth, bodyHeight), true)) {
            DrawSourceList();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Splitter gripper: drag to resize the source list.
        {
            ImVec2 splitterPos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##bb_splitter", ImVec2(kSplitterW, bodyHeight));
            if (ImGui::IsItemActive())
                m_leftWidth =
                    std::clamp(m_leftWidth + ImGui::GetIO().MouseDelta.x, kMinLeftW,
                               std::max(kMinLeftW, outerAvail.x - kSplitterW - kMinRightW));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            const float midX = splitterPos.x + kSplitterW * 0.5f;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(midX, splitterPos.y),
                                                ImVec2(midX, splitterPos.y + bodyHeight),
                                                IM_COL32(110, 110, 110, 180), 1.0f);
        }

        ImGui::SameLine();

        // Right: hex + structure details + brainfile identification.
        if (ImGui::BeginChild("##bb_content", ImVec2(0, bodyHeight), false)) {
            bool noData =
                m_data.empty() || (m_selectedSource == Source::CurrentDisk && !m_documentLoaded);
            if (noData) {
                if (m_selectedSource == Source::CurrentDisk) {
                    ImGui::TextDisabled("No disk loaded.");
                    ImGui::TextDisabled("Open a disk, or select a boot block from the library.");
                } else {
                    ImGui::TextDisabled("No boot block data.");
                }
            } else {
                ImVec2 availSize = ImGui::GetContentRegionAvail();

                // Fixed-height bottom panel; only the hex view grows/shrinks on resize.
                float bottomHeight = 280.0f;
                float topHeight = availSize.y - bottomHeight - ImGui::GetStyle().ItemSpacing.y;
                if (topHeight < 100.0f)
                    topHeight = 100.0f;

                // Top section: tabbed Hex view + live emulator Preview
                if (ImGui::BeginChild("##hex_panel", ImVec2(0, topHeight), true)) {
                    if (ImGui::BeginTabBar("##bb_view_tabs")) {
                        if (ImGui::BeginTabItem("Hex")) {
                            ImVec2 hexSize = ImGui::GetContentRegionAvail();
                            DrawHexEditor(hexSize, "##bootblock_hex");
                            ImGui::EndTabItem();
                        }
                        if (ImGui::BeginTabItem("Preview")) {
                            m_previewTabActive = true;
                            DrawPreviewTab();
                            ImGui::EndTabItem();
                        }
                        ImGui::EndTabBar();
                    }
                }
                ImGui::EndChild();

                ImGui::Separator();

                // Bottom section: Structure details (Left) + Brainfile info (Right)
                if (ImGui::BeginChild("##bottom_container", ImVec2(0, 0), false)) {
                    ImVec2 bottomAvail = ImGui::GetContentRegionAvail();
                    float structWidth = bottomAvail.x * 0.45f;

                    if (ImGui::BeginChild("##structure_panel", ImVec2(structWidth, 0), true)) {
                        DrawStructureDetails();
                    }
                    ImGui::EndChild();

                    ImGui::SameLine();

                    if (ImGui::BeginChild("##brainfile_panel", ImVec2(0, 0), true)) {
                        DrawBrainfileInfo();
                    }
                    ImGui::EndChild();
                }
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();

        // Footer: action buttons, right-aligned at the bottom of the window.
        ImGui::Separator();
        DrawLibraryToolbar();
    }
    ImGui::End();

    // Modal dialogs (drawn regardless of window collapse so they can complete).
    DrawLibraryAddDialog();
    DrawRenameDialog();
    DrawDeleteConfirm();
    DrawInstallConfirm();
}

} // namespace adm::ui
