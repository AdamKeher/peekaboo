#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "SystemFilesFinder.h"

namespace adm {
class AmigaEmulator;
namespace core {
class UserPreferences;
}
} // namespace adm

namespace adm::ui {

class EmulatorView;

// The setup wizard: on a first start without Kickstart ROMs, and from Help > Setup Wizard. It finds
// the user's own ROMs and Workbench (Peekaboo ships neither), copies the ROMs in, picks a machine
// that has its Kickstart, offers a Workbench hardfile as DH0, and shows the controls. Everything
// it sets lives in Preferences afterwards.
class OnboardingView
{
public:
    void Open();
    bool IsOpen() const { return m_isOpen; }
    void Draw();

    void SetEmulator(AmigaEmulator* emulator);
    void SetEmulatorView(EmulatorView* view) { m_emulatorView = view; }
    void SetUserPreferences(core::UserPreferences* prefs) { m_userPrefs = prefs; }

    // Whether any machine preset's Kickstart is in the ROM folder, by name: the first start that
    // finds one has nothing to set up.
    bool AnyMachineReady() const;

    std::function<void(const std::string& path)> m_onOpenDisk; // an ADF, as a document
    std::function<void()> m_onStart;                           // run the machine
    std::function<void()> m_onOpenSystemFilesPrefs;

private:
    enum Page { Welcome, Files, Machines, Workbench, Controls, Done, PageCount };

    std::filesystem::path RomDirectory() const;
    void EnterPage(int page);
    void RefreshMachines();
    void ApplyMachine();
    void Finish();
    void UseAsDh0(const std::filesystem::path& hardfile);

    void DrawSteps();
    void DrawWelcome();
    void DrawFiles();
    void DrawMachines();
    void DrawWorkbench();
    void DrawControls();
    void DrawDone();

    bool m_isOpen = false;
    int m_page = Welcome;
    AmigaEmulator* m_emulator = nullptr;
    EmulatorView* m_emulatorView = nullptr;
    core::UserPreferences* m_userPrefs = nullptr;
    SystemFilesFinder m_finder;

    // The machines page: each set of presets, ready or not, and the one picked
    struct MachineRow
    {
        std::string label, modelId, kickstart;
        std::vector<std::string> missing; // its ROMs not in the ROM folder
    };
    std::vector<MachineRow> m_machines;
    std::string m_model;   // the preset chosen (AmigaModelInfo::id), empty for none
    std::string m_applied; // the one last set on the emulator
    std::string m_dh0;     // the hardfile made DH0 here, for the summary
    bool m_dh0ReadOnly = false;
};

} // namespace adm::ui
