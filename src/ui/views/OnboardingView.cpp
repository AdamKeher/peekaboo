#include "OnboardingView.h"

#include <imgui.h>

#include <algorithm>

#include <fmt/format.h>

#include "../../core/AmigaEmulator.h"
#include "../../core/HostMountService.h"
#include "../../core/Logger.h"
#include "../../core/UserPreferences.h"
#include "EmulatorView.h"

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

// The file's read-only attribute (or permission) is not set
bool IsWritable(const fs::path& file)
{
    std::error_code ec;
    const auto perms = fs::status(file, ec).permissions();
    return !ec && (perms & fs::perms::owner_write) != fs::perms::none;
}

constexpr const char* kPageNames[] = {"Welcome",   "System files", "Machine",
                                      "Workbench", "Controls",     "Done"};

} // namespace

void OnboardingView::SetEmulator(AmigaEmulator* emulator)
{
    m_emulator = emulator;
    m_finder.SetEmulator(emulator);
    m_finder.SetRomDirectory([this]() { return RomDirectory(); });
}

fs::path OnboardingView::RomDirectory() const
{
    // The folder the core loads ROMs from: Application sets it from the prefs' ROM folder.
    if (m_emulator && !m_emulator->GetSystemDirectory().empty())
        return fs::u8path(m_emulator->GetSystemDirectory());
    std::error_code ec;
    const auto dir = fs::absolute("emulation", ec);
    return ec ? fs::path("emulation") : dir;
}

bool OnboardingView::AnyMachineReady() const
{
    const auto dir = RomDirectory();
    for (const auto& machine : core::MachineRomNeeds()) {
        const bool all =
            std::all_of(machine.roms.begin(), machine.roms.end(), [&](const char* rom) {
                return core::FindPresetRom(dir, rom, {}).has_value();
            });
        if (all)
            return true;
    }
    return false;
}

void OnboardingView::Open()
{
    m_isOpen = true;
    m_page = Welcome;
    m_dh0.clear();
    m_dh0ReadOnly = false;
    // A pick from an earlier run is not this one's: the machine page starts from the one in use
    m_model.clear();
    m_applied = m_emulatorView ? m_emulatorView->GetCurrentModelId() : std::string();
}

void OnboardingView::EnterPage(int page)
{
    // Moving on from the machine page puts the machine picked on the emulator
    if (m_page == Machines && page > m_page)
        ApplyMachine();
    m_page = std::clamp(page, 0, PageCount - 1);
    if (m_page == Files && !m_finder.HasScanned() && !m_finder.IsScanning())
        m_finder.Scan();
    if (m_page == Machines || m_page == Done)
        RefreshMachines();
}

void OnboardingView::RefreshMachines()
{
    const auto dir = RomDirectory();
    const auto identify = m_finder.Identifier();
    m_machines.clear();
    for (const auto& machine : core::MachineRomNeeds()) {
        MachineRow row{machine.label, machine.modelId, machine.kickstart, {}};
        for (const char* rom : machine.roms) {
            if (!core::FindPresetRom(dir, rom, identify))
                row.missing.push_back(rom);
        }
        m_machines.push_back(std::move(row));
    }

    // A row stands for every preset with its ROMs; the pick keeps its exact preset (A1200OG, ...)
    const auto rowOf = [this](const std::string& modelId) -> const MachineRow* {
        const auto roms = core::PresetRomNames(modelId);
        for (const auto& row : m_machines) {
            if (core::PresetRomNames(row.modelId) == roms)
                return &row;
        }
        return nullptr;
    };
    if (const auto* row = rowOf(m_model); row && row->missing.empty())
        return;
    // Else the machine in use, if it is ready, else the first that is, else none (AROS)
    m_model.clear();
    const std::string current = m_emulatorView ? m_emulatorView->GetCurrentModelId() : "";
    if (const auto* row = rowOf(current); row && row->missing.empty()) {
        m_model = current;
        return;
    }
    for (const auto& row : m_machines) {
        if (row.missing.empty()) {
            m_model = row.modelId;
            return;
        }
    }
}

void OnboardingView::ApplyMachine()
{
    if (m_model.empty() || m_model == m_applied || !m_emulatorView)
        return;
    for (const auto& info : GetAllAmigaModels()) {
        if (m_model == info.id) {
            m_emulatorView->SetModel(info.model);
            m_applied = m_model;
            core::Logger::Info("Setup wizard: machine {}", m_model);
            return;
        }
    }
}

void OnboardingView::UseAsDh0(const fs::path& hardfile)
{
    if (!m_emulatorView)
        return;
    auto mounts = m_emulatorView->GetConfiguredHostMounts();
    auto& dh0 = mounts[0];
    dh0.device = core::HostMountDevice::DH0;
    dh0.backingType = core::HostMountBackingType::HardfileImage;
    dh0.hardfilePath = hardfile;
    dh0.enabled = true;
    // Amiga Forever ships some hardfiles read-only, and a read-write mount of one stops the
    // machine starting at all; mounted read-only, Workbench still boots from it
    dh0.readOnly = !IsWritable(hardfile);
    dh0.bootPriority = 0; // boots ahead of the floppy
    m_emulatorView->SetConfiguredHostMounts(mounts, true);
    m_dh0 = Utf8(hardfile);
    m_dh0ReadOnly = dh0.readOnly;
    core::Logger::Info("Setup wizard: DH0 is {}{}", m_dh0, dh0.readOnly ? " (read-only)" : "");
}

void OnboardingView::Finish()
{
    ApplyMachine();
    m_isOpen = false;
    if (m_userPrefs)
        m_userPrefs->SetOnboardingDone(true);
}

void OnboardingView::Draw()
{
    if (!m_isOpen)
        return;
    // The scan started on System files finishes whichever page is showing
    m_finder.Poll();

    // Modal: a machine restarting under it (the Machine page's Next does that) or any other window
    // must not come up in front of it. Opened again while it should be open, as the screen profile
    // prompt is - the first frames' layout settling can close a popup - but not while another
    // dialog is up: opening this one would close that one, which would close this one.
    constexpr const char* kId = "Setup###SetupWizard";
    if (!ImGui::IsPopupOpen(kId)) {
        if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            return;
        ImGui::OpenPopup(kId);
    }
    ImGui::SetNextWindowSize(ImVec2(Em(800), Em(560)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::BeginPopupModal(kId, &open,
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        DrawSteps();
        ImGui::Separator();
        ImGui::Spacing();

        const float bar = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
        if (ImGui::BeginChild("##page", ImVec2(0, -bar))) {
            ImGui::PushTextWrapPos(0.0f);
            switch (m_page) {
                case Welcome:
                    DrawWelcome();
                    break;
                case Files:
                    DrawFiles();
                    break;
                case Machines:
                    DrawMachines();
                    break;
                case Workbench:
                    DrawWorkbench();
                    break;
                case Controls:
                    DrawControls();
                    break;
                default:
                    DrawDone();
                    break;
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (m_page != Done && ImGui::Button("Skip setup"))
            Finish();
        const float navW = Em(90);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - navW * 2 - ImGui::GetStyle().ItemSpacing.x -
                             ImGui::GetStyle().WindowPadding.x);
        ImGui::BeginDisabled(m_page == Welcome);
        if (ImGui::Button("Back", ImVec2(navW, 0)))
            EnterPage(m_page - 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (m_page == Done) {
            if (ImGui::Button("Finish", ImVec2(navW, 0)))
                Finish();
        } else if (ImGui::Button("Next", ImVec2(navW, 0))) {
            EnterPage(m_page + 1);
        }
        if (!m_isOpen) // finished by a button this frame
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!open && m_isOpen)
        Finish(); // closed: done, as Skip is
}

void OnboardingView::DrawSteps()
{
    for (int i = 0; i < PageCount; ++i) {
        if (i > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
        }
        if (i == m_page)
            ImGui::TextUnformatted(kPageNames[i]);
        else
            ImGui::TextDisabled("%s", kPageNames[i]);
    }
}

void OnboardingView::DrawWelcome()
{
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextUnformatted("Welcome to Peekaboo");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextUnformatted("Peekaboo looks inside Amiga disks and runs them on an emulated Amiga, "
                           "with a debugger beside it.");
    ImGui::Spacing();
    ImGui::TextUnformatted(
        "It comes without the Amiga's own system software - the Kickstart ROMs and Workbench - "
        "because they are still sold. Bring your own: Amiga Forever, AmigaOS 3.2, or ROMs and "
        "disks read from your own Amiga.");
    ImGui::Spacing();
    ImGui::TextUnformatted("The next pages find them on this computer, set up a machine and show "
                           "the controls. Anything chosen here can be changed later in Emulator > "
                           "Preferences > System Files, and this wizard runs again from Help > "
                           "Setup Wizard.");
}

void OnboardingView::DrawFiles()
{
    ImGui::TextUnformatted(
        "Peekaboo looks in the folders of Amiga Forever, FS-UAE and WinUAE that it finds, and in "
        "any you add. Every ROM is recognised by its contents, whatever its file name; the ticked "
        "ones are copied into Peekaboo's ROM folder under the names its machines load. The "
        "originals are left as they are.");
    ImGui::Spacing();
    m_finder.Draw();
}

void OnboardingView::DrawMachines()
{
    ImGui::TextUnformatted("The machine Peekaboo starts. Each needs its Kickstart in the ROM "
                           "folder; the ones that have it can be picked.");
    ImGui::Spacing();

    const auto rowRoms = core::PresetRomNames(m_model);
    bool anyReady = false;
    if (ImGui::BeginTable("##machines", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Machine", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Kickstart", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("ROM folder", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < m_machines.size(); ++i) {
            const auto& row = m_machines[i];
            const bool ready = row.missing.empty();
            anyReady = anyReady || ready;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // Not ready: there, greyed, with what it is missing
            ImGui::BeginDisabled(!ready);
            const bool selected = !rowRoms.empty() && core::PresetRomNames(row.modelId) == rowRoms;
            if (ImGui::Selectable(row.label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                m_model = row.modelId;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.kickstart.c_str());
            ImGui::TableNextColumn();
            if (ready) {
                ImGui::TextUnformatted("ready");
            } else {
                std::string needs = "needs ";
                for (size_t k = 0; k < row.missing.size(); ++k)
                    needs += (k ? ", " : "") + row.missing[k];
                ImGui::TextUnformatted(needs.c_str());
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (!anyReady) {
        ImGui::TextUnformatted(
            "No machine has its Kickstart yet. Each one still starts, on AROS - the free "
            "Kickstart replacement built into Peekaboo - but most Amiga software wants the real "
            "one. Go back a page to look in another folder, or get one:");
        ImGui::Bullet();
        ImGui::TextLinkOpenURL("Amiga Forever", "https://www.amigaforever.com");
        ImGui::SameLine();
        ImGui::TextDisabled("- every Kickstart and Workbench, licensed");
        ImGui::Bullet();
        ImGui::TextLinkOpenURL("AmigaOS 3.2", "https://www.hyperion-entertainment.com");
        ImGui::SameLine();
        ImGui::TextDisabled("- the current Kickstart and Workbench");
        ImGui::Bullet();
        ImGui::TextUnformatted("the ROM of your own Amiga, read out with a ROM dumping tool");
    } else if (!m_model.empty()) {
        ImGui::TextDisabled("Next sets the machine to %s.", m_model.c_str());
    }
}

void OnboardingView::DrawWorkbench()
{
    const auto& scan = m_finder.Result();
    ImGui::TextUnformatted(
        "Workbench is the Amiga's desktop. Games and demos mostly boot from their own disks and "
        "need none; for the desktop, open a Workbench floppy or make a Workbench hardfile the "
        "first hard drive (DH0). Both are used where they are, not copied.");
    ImGui::Spacing();

    // Workbench disks first, then the other AmigaOS disks, then hardfiles
    std::vector<const core::FoundDisk*> disks;
    for (const auto& d : scan.disks)
        disks.push_back(&d);
    std::stable_sort(disks.begin(), disks.end(), [](const auto* a, const auto* b) {
        return core::IsWorkbenchVolume(a->volume) && !core::IsWorkbenchVolume(b->volume);
    });

    const bool empty = disks.empty() && scan.hardfiles.empty();
    const float height = ImGui::GetTextLineHeightWithSpacing() * 10.0f;
    ImGui::BeginDisabled(empty);
    if (ImGui::BeginTable("##workbench", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##do", ImGuiTableColumnFlags_WidthFixed, Em(100));
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, Em(70));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch, 0.65f);
        ImGui::TableHeadersRow();
        // The folder, with the whole path on hover
        auto folderCell = [](const fs::path& path) {
            ImGui::TextUnformatted(Utf8(path.parent_path()).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Utf8(path).c_str());
        };
        int id = 0;
        // Hardfiles first: there are few, and DH0 is what this page is for
        for (const auto& h : scan.hardfiles) {
            ImGui::PushID(id++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Use as DH0"))
                UseAsDh0(h.path);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("hardfile");
            ImGui::TableNextColumn();
            ImGui::Text("%s (%llu MB%s)", Utf8(h.path.filename()).c_str(),
                        static_cast<unsigned long long>(h.size / (1024 * 1024)),
                        IsWritable(h.path) ? "" : ", read-only");
            ImGui::TableNextColumn();
            folderCell(h.path);
            ImGui::PopID();
        }
        for (const auto* d : disks) {
            ImGui::PushID(id++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Open") && m_onOpenDisk)
                m_onOpenDisk(Utf8(d->path));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("floppy");
            ImGui::TableNextColumn();
            ImGui::Text("%s  %s", d->volume.c_str(), Utf8(d->path.filename()).c_str());
            ImGui::TableNextColumn();
            folderCell(d->path);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    if (empty) {
        ImGui::TextDisabled(m_finder.IsScanning() ? "Still looking..."
                            : m_finder.HasScanned()
                                ? "No Workbench disks or hardfiles were found. Floppies run "
                                  "without one: File > Open any ADF."
                                : "Nothing looked for yet - see System files.");
    }
    if (!m_dh0.empty()) {
        ImGui::Spacing();
        ImGui::Text("DH0 is %s. A Workbench 3.x hardfile wants an Amiga 1200 or 4000.",
                    m_dh0.c_str());
        if (m_dh0ReadOnly) {
            ImGui::TextDisabled("The file is read-only, so DH0 is too: Workbench starts, but "
                                "cannot save to it. Use a writable copy for that (Emulator > "
                                "Hard Drives).");
        }
    }
}

void OnboardingView::DrawControls()
{
    ImGui::TextUnformatted("Mouse");
    ImGui::Bullet();
    ImGui::TextUnformatted("Click the emulator's screen to hand the mouse to the Amiga. F12 or "
                           "the middle button takes it back.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Joystick");
    ImGui::Bullet();
    ImGui::TextUnformatted("A gamepad shows up under Emulator > Controller, to be used as a "
                           "joystick or as a CD32 pad. Without one, the keyboard can be the "
                           "joystick:");
    if (m_emulatorView) {
        bool keyboardJoystick = m_emulatorView->IsKeyboardJoystickEnabled();
        ImGui::Indent();
        if (ImGui::Checkbox("Keyboard joystick: the cursor keys, A and S to fire",
                            &keyboardJoystick)) {
            m_emulatorView->SetKeyboardJoystickEnabled(keyboardJoystick);
            if (m_userPrefs)
                m_userPrefs->SetKeyboardJoystick(keyboardJoystick);
        }
        ImGui::Unindent();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Disks");
    ImGui::Bullet();
    ImGui::TextUnformatted("File > Open an ADF, or drop one on the window, then run it.");
}

void OnboardingView::DrawDone()
{
    const auto dir = RomDirectory();
    int ready = 0;
    std::string machine = "none - AROS, the free Kickstart";
    for (const auto& row : m_machines) {
        ready += row.missing.empty() ? 1 : 0;
        if (!m_model.empty() && core::PresetRomNames(row.modelId) == core::PresetRomNames(m_model))
            machine = row.label + " (" + m_model + ")";
    }
    if (ImGui::BeginTable("##summary", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, Em(150));
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);
        auto row = [](const char* key, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", key);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.c_str());
        };
        row("ROM folder", Utf8(dir));
        row("Machines ready", fmt::format("{} of {}", ready, static_cast<int>(m_machines.size())));
        row("Machine", machine);
        const auto& mounts = m_emulatorView ? m_emulatorView->GetConfiguredHostMounts()
                                            : std::array<core::HostDirectoryMount, 2>{};
        const auto& dh0 = mounts[0];
        row("DH0", dh0.enabled ? Utf8(dh0.HostPath()) : std::string("none"));
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    const std::string start =
        m_model.empty() ? std::string("Start the machine") : "Start the " + m_model;
    if (ImGui::Button(start.c_str())) {
        Finish();
        if (m_onStart)
            m_onStart();
    }
    ImGui::SameLine();
    if (ImGui::Button("Preferences > System Files")) {
        Finish();
        if (m_onOpenSystemFilesPrefs)
            m_onOpenSystemFilesPrefs();
    }
}

} // namespace adm::ui
