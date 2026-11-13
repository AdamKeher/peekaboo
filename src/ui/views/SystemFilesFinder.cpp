#include "SystemFilesFinder.h"

#include <imgui.h>
#include <nfd.h>

#include <algorithm>
#include <chrono>

#include <fmt/format.h>

#include "../../core/AmigaEmulator.h"
#include "../../core/KickstartRoms.h"
#include "../../core/Logger.h"

namespace adm::ui {

namespace fs = std::filesystem;

namespace {

// A width designed at 16 px text, scaled to the text size in use now
float Em(float px16) { return px16 * ImGui::GetFontSize() / 16.0f; }

std::string Utf8(const fs::path& path)
{
    const auto s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

} // namespace

SystemFilesFinder::~SystemFilesFinder()
{
    // A scan still walking finishes first: it calls into the core through m_emulator.
    if (m_scanning.valid())
        m_scanning.wait();
}

core::RomIdentifier SystemFilesFinder::Identifier()
{
    if (!m_emulator)
        return {};
    // The same on-demand load Preferences does: LoadCore only resolves the exports.
    if (!m_emulator->IsCoreLoaded())
        m_emulator->LoadCore(AmigaEmulator::GetDefaultCorePath());
    if (!core::KickstartRoms::Available(*m_emulator))
        return {};
    AmigaEmulator* emulator = m_emulator;
    return [emulator](const fs::path& file, const fs::path& keyDir) {
        return core::KickstartRoms::Identify(*emulator, file, keyDir);
    };
}

void SystemFilesFinder::FindDefaultFolders()
{
    m_foldersFound = true;
    for (const auto& f : core::DefaultSearchFolders())
        m_folders.push_back({f.path, f.label, true});
}

void SystemFilesFinder::Scan()
{
    if (m_scanning.valid())
        return;
    if (!m_foldersFound)
        FindDefaultFolders();
    std::vector<fs::path> roots;
    for (const auto& f : m_folders) {
        if (f.use)
            roots.push_back(f.path);
    }
    // The identifier is made here, on the UI thread: it may load the core.
    auto identify = Identifier();
    m_scanning = std::async(std::launch::async, [roots, identify]() {
        core::SystemFileScan all;
        for (const auto& root : roots)
            core::MergeScan(all, core::ScanForSystemFiles(root, identify));
        return all;
    });
}

void SystemFilesFinder::Poll()
{
    if (!m_scanning.valid() ||
        m_scanning.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    m_scan = m_scanning.get();
    m_scanned = true;
    m_copied = false;
    m_lastCopy.clear();
    const fs::path romDir = m_romDirectory ? m_romDirectory() : fs::path();
    m_copy.assign(m_scan.roms.size(), 0);
    m_inRomFolder.assign(m_scan.roms.size(), 0);
    for (size_t i = 0; i < m_scan.roms.size(); ++i) {
        const std::string name = core::CanonicalRomName(m_scan.roms[i]);
        // Already there under a name the core loads: nothing to copy
        m_inRomFolder[i] = !name.empty() && core::FindPresetRom(romDir, name, {}).has_value();
        // A preset's ROM is copied unless it is there; one no preset loads only when ticked
        m_copy[i] = !name.empty() && !m_inRomFolder[i];
    }
    core::Logger::Info("System files: found {} ROMs, {} rom.key, {} AmigaOS disks, {} hardfiles "
                       "({} entries{})",
                       m_scan.roms.size(), m_scan.romKeys.size(), m_scan.disks.size(),
                       m_scan.hardfiles.size(), m_scan.entriesVisited,
                       m_scan.truncated ? ", stopped at the limit" : "");
}

void SystemFilesFinder::DrawFolders()
{
    ImGui::TextUnformatted("Look in");
    ImGui::SameLine();
    const float buttons = Em(110) + Em(70) + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(
        std::max(ImGui::GetCursorPosX(),
                 ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons));
    ImGui::BeginDisabled(IsScanning());
    if (ImGui::Button("Add folder...", ImVec2(Em(110), 0))) {
        nfdchar_t* out = nullptr;
        if (NFD_PickFolder(&out, nullptr) == NFD_OKAY) {
            const fs::path picked = fs::u8path(out);
            NFD_FreePath(out);
            const bool have = std::any_of(m_folders.begin(), m_folders.end(),
                                          [&](const Folder& f) { return f.path == picked; });
            if (!have)
                m_folders.push_back({picked, {}, true});
            Scan();
        }
        ImGui::GetIO().ClearInputKeys();
    }
    ImGui::SameLine();
    if (ImGui::Button(IsScanning() ? "Scanning" : "Scan", ImVec2(Em(70), 0)))
        Scan();
    ImGui::EndDisabled();

    // The folders as a table, the defaults found and any added; greyed, not hidden, when empty
    const int rows = std::max<int>(1, static_cast<int>(m_folders.size()));
    const float height = ImGui::GetFrameHeightWithSpacing() * (std::min(rows, 4) + 1) +
                         ImGui::GetStyle().CellPadding.y;
    ImGui::BeginDisabled(m_folders.empty() || IsScanning());
    if (ImGui::BeginTable("##sysfolders", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##use", ImGuiTableColumnFlags_WidthFixed, Em(24));
        ImGui::TableSetupColumn("Whose", ImGuiTableColumnFlags_WidthFixed, Em(110));
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        int remove = -1;
        for (int i = 0; i < static_cast<int>(m_folders.size()); ++i) {
            auto& f = m_folders[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Checkbox("##use", &f.use);
            ImGui::TableNextColumn();
            if (f.label.empty()) {
                // One the user added: theirs to take away again
                if (ImGui::SmallButton("Remove"))
                    remove = i;
            } else {
                ImGui::TextUnformatted(f.label.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Utf8(f.path).c_str());
            ImGui::PopID();
        }
        if (m_folders.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("-");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("No Amiga Forever, FS-UAE or WinUAE folder on this computer");
        }
        if (remove >= 0)
            m_folders.erase(m_folders.begin() + remove);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
}

bool SystemFilesFinder::DrawRoms()
{
    const auto& roms = m_scan.roms;
    ImGui::Spacing();
    if (IsScanning())
        ImGui::TextDisabled("Kickstart ROMs found - looking...");
    else
        ImGui::Text("Kickstart ROMs found: %d", static_cast<int>(roms.size()));

    const float height = ImGui::GetTextLineHeightWithSpacing() * 8.0f;
    ImGui::BeginDisabled(roms.empty() || IsScanning());
    if (ImGui::BeginTable("##sysroms", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Copy", ImGuiTableColumnFlags_WidthFixed, Em(40));
        ImGui::TableSetupColumn("ROM", ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupColumn("Loaded as", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Found", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(roms.size()); ++i) {
            const auto& rom = roms[i];
            const std::string name = core::CanonicalRomName(rom);
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool copy = m_copy[i] != 0;
            if (ImGui::Checkbox("##copy", &copy))
                m_copy[i] = copy ? 1 : 0;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(rom.DisplayName().c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s", rom.VersionLabel().c_str(), rom.DetailLine().c_str());
            ImGui::TableNextColumn();
            if (name.empty())
                ImGui::TextDisabled("its own name");
            else if (m_inRomFolder[i])
                ImGui::TextDisabled("%s (there)", name.c_str());
            else
                ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Utf8(rom.path).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Utf8(rom.path).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    if (m_scanned && !IsScanning()) {
        std::vector<std::string> also;
        if (!m_scan.disks.empty())
            also.push_back(fmt::format("{} AmigaOS disk{}", m_scan.disks.size(),
                                       m_scan.disks.size() == 1 ? "" : "s"));
        if (!m_scan.hardfiles.empty())
            also.push_back(fmt::format("{} hardfile{}", m_scan.hardfiles.size(),
                                       m_scan.hardfiles.size() == 1 ? "" : "s"));
        if (!m_scan.romKeys.empty())
            also.push_back("rom.key");
        std::string line;
        for (size_t i = 0; i < also.size(); ++i)
            line += (i ? ", " : "") + also[i];
        if (!line.empty())
            ImGui::TextDisabled("Also found: %s", line.c_str());
        if (m_scan.truncated)
            ImGui::TextDisabled("Stopped after %d files - pick a smaller folder to look further.",
                                static_cast<int>(m_scan.entriesVisited));
    }

    // Copy, into the ROM folder as it stands
    const fs::path romDir = m_romDirectory ? m_romDirectory() : fs::path();
    const int ticked = static_cast<int>(std::count(m_copy.begin(), m_copy.end(), 1));
    bool copiedNow = false;
    ImGui::Spacing();
    ImGui::TextDisabled("Peekaboo's ROM folder: %s", Utf8(romDir).c_str());
    ImGui::BeginDisabled(ticked == 0 || IsScanning() || romDir.empty());
    const std::string label = ticked == 0   ? std::string("Copy")
                              : ticked == 1 ? std::string("Copy 1 ROM")
                                            : fmt::format("Copy {} ROMs", ticked);
    if (ImGui::Button(label.c_str())) {
        std::vector<core::RomIdentity> chosen;
        for (size_t i = 0; i < roms.size(); ++i) {
            if (m_copy[i])
                chosen.push_back(roms[i]);
        }
        m_lastCopy = core::CopyRomsInto(romDir, chosen, m_scan.romKeys, Identifier());
        m_copied = true;
        copiedNow = true;
        for (size_t i = 0; i < roms.size(); ++i) {
            const std::string name = core::CanonicalRomName(roms[i]);
            m_inRomFolder[i] = !name.empty() && core::FindPresetRom(romDir, name, {}).has_value();
            if (m_inRomFolder[i])
                m_copy[i] = 0;
        }
        for (const auto& c : m_lastCopy) {
            if (c.outcome == core::RomCopy::Outcome::Failed ||
                c.outcome == core::RomCopy::Outcome::KeptExisting)
                core::Logger::Warn("System files: {}: {}", Utf8(c.source), c.detail);
        }
    }
    ImGui::EndDisabled();

    if (m_copied) {
        int copied = 0, there = 0;
        for (const auto& c : m_lastCopy) {
            copied += c.outcome == core::RomCopy::Outcome::Copied ? 1 : 0;
            there += c.outcome == core::RomCopy::Outcome::AlreadyThere ? 1 : 0;
        }
        ImGui::SameLine();
        ImGui::Text("Copied %d%s.", copied,
                    there ? fmt::format(", {} already there", there).c_str() : "");
        for (const auto& c : m_lastCopy) {
            if (c.outcome != core::RomCopy::Outcome::Failed &&
                c.outcome != core::RomCopy::Outcome::KeptExisting)
                continue;
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s %s",
                               Utf8(c.source.filename()).c_str(), c.detail.c_str());
        }
    }
    return copiedNow;
}

bool SystemFilesFinder::Draw()
{
    if (!m_foldersFound)
        FindDefaultFolders();
    Poll();
    DrawFolders();
    return DrawRoms();
}

} // namespace adm::ui
