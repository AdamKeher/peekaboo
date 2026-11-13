#pragma once

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../core/HostPorts.h"
#include "../../core/ScreenProfile.h"
#include "SystemFilesFinder.h"
#include "core/KickstartRoms.h"

namespace adm {
class AmigaEmulator;
namespace core {
class UserPreferences;
}

namespace ui {

class EmulatorView;
class FontManager;

/**
 * @brief Tabbed preferences window for emulator core configuration.
 *
 * Exposes granular PuAE core options grouped into logical categories:
 * CPU & Chipset, RAM, Video, Audio, and Floppy. Settings are stored in
 * UserPreferences and applied to the AmigaEmulator as user variable
 * overrides (highest-priority, applied before hardcoded defaults).
 */
class PreferencesView
{
public:
    void Open();
    // Open() on the Appearance tab (View > Text Size > Appearance...).
    void OpenAppearance();
    // Open() on the System Files tab: the ROM folder and finding the user's own ROMs.
    void OpenSystemFiles();
    void Draw();

    void SetEmulator(AmigaEmulator* emu);
    void SetUserPreferences(core::UserPreferences* prefs) { m_userPrefs = prefs; }
    void SetEmulatorView(EmulatorView* view) { m_emulatorView = view; }
    void SetFontManager(const FontManager* fonts) { m_fontManager = fonts; }
    /// The live screen profile and how to change it (Application owns both).
    void SetScreenProfileHooks(std::function<core::ScreenProfile()> get,
                               std::function<void(core::ScreenProfile)> set)
    {
        m_getScreenProfile = std::move(get);
        m_setScreenProfile = std::move(set);
    }

    bool IsOpen() const { return m_isOpen; }

    // Number of known Kickstart ROM entries in the static table
    static constexpr int kNumKickstarts = 14;

private:
    bool m_isOpen = false;
    bool m_needsLoad = true;
    AmigaEmulator* m_emulator = nullptr;
    core::UserPreferences* m_userPrefs = nullptr;
    EmulatorView* m_emulatorView = nullptr;
    const FontManager* m_fontManager = nullptr;
    std::function<core::ScreenProfile()> m_getScreenProfile;
    std::function<void(core::ScreenProfile)> m_setScreenProfile;

    // Appearance tab
    bool m_selectAppearanceTab = false;
    bool m_selectSystemFilesTab = false;
    int m_fontRoleSel = 1; // the highlighted row of the text-role table (Code by default)
    // The interface-size slider mid-drag. Zooming while dragging would move the slider under
    // the mouse, so the zoom applies on release; until then this is what the slider shows.
    float m_draftUiScale = -1.0f;

    // Working copy of settings: puae variable key → string value
    std::map<std::string, std::string> m_vars;
    // The emulator's user variables as they were when the working copy was last in step with
    // them. Anything that changes out from under an open dialog - a model switch, the Display
    // window's resolution combo, the Audio Channels filter - is folded back in by
    // SyncExternalChanges() so Apply cannot push a stale machine back over it.
    std::unordered_map<std::string, std::string> m_emulatorVars;
    std::string m_kickstartRomDir;   // persisted separately (not a PuAE variable)
    bool m_hasRomKey = false;        // rom.key exists in m_kickstartRomDir
    bool m_hasEncryptedRoms = false; // an Amiga Forever encrypted ROM is in m_kickstartRomDir
    SystemFilesFinder m_finder;      // the System Files tab's "Find system files"
    // What the ROM directory actually holds, identified by content rather than by filename
    // (core::KickstartRoms, core 1.96.0). Refilled by ScanKickstartRoms. Empty either because
    // the directory holds no ROMs or because the loaded core is too old to identify them -
    // m_romScanSupported tells those two apart, so the table can say which.
    std::vector<core::RomIdentity> m_scannedRoms;
    bool m_romScanSupported = false;
    bool m_romScanRan = false;

    void LoadSettings();
    void SyncExternalChanges();
    void ApplySettings();
    void ResetToDefaults();
    void ScanKickstartRoms(); // scan m_kickstartRomDir (or default ./emulation/) for ROM files

    void DrawGeneralTab();
    void DrawAppearanceTab();
    void DrawSystemFilesTab();
    void DrawCpuChipsetTab();
    void DrawRamTab();
    void DrawVideoTab();
    void DrawAudioTab();
    void DrawFloppyTab();
    void DrawPortsTab();

    // Ports tab: what the fields hold before Apply Port, and the host's serial devices.
    core::SerialPortTarget m_serialDraft;
    std::string m_parallelDraft;
    bool m_portsDraftLoaded = false;
    std::vector<std::string> m_serialDevices;

    // Draw a labelled combo box backed by m_vars[key]. The entries, and the value used when
    // nothing is stored yet, come from the shared option table in core/EmulatorOptions.h - the
    // same table the emulator answers the core out of - so the dialog cannot offer a string the
    // core does not parse. Returns true if the value changed.
    bool Combo(const char* label, const std::string& key);

    // Draw a labelled checkbox backed by m_vars[key], for an option whose two values are the
    // core's "enabled"/"disabled". Returns true if the value changed.
    bool Checkbox(const char* label, const std::string& key);

    // Peekaboo's default for an option, from the shared table.
    static const char* OptionDefault(const std::string& key);
};

} // namespace ui
} // namespace adm
