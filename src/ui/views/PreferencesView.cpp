#include "PreferencesView.h"

#include <imgui.h>
#include <nfd.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <random>

#include "../../core/AmigaEmulator.h"
#include "../../core/EmulatorOptions.h"
#include "../../core/UserPreferences.h"
#include "../FontManager.h"
#include "../FontRoles.h"
#include "../IconsFontAwesome6.h"
#include "EmulatorView.h"

namespace adm::ui {

// ---------------------------------------------------------------------------
// Static Kickstart ROM table (matches PuAE's uae_kickstarts[] in libretro-core.h)
// ---------------------------------------------------------------------------

struct KickstartRomEntry
{
    const char* filename;    // PuAE value (filename only, looked up in system dir)
    const char* alternate;   // Amiga Forever alternate filename
    const char* displayName; // Human-readable label
};

static const KickstartRomEntry kKickstarts[PreferencesView::kNumKickstarts] = {
    {"kick31034.A1000", "amiga-os-110-ntsc.rom", "v1.1 rev 31.034 (1985) \xe2\x80\x94 A1000 NTSC"},
    {"kick32034.A1000", "amiga-os-110-pal.rom", "v1.1 rev 32.034 (1986) \xe2\x80\x94 A1000 PAL"},
    {"kick33180.A500", "amiga-os-120.rom",
     "v1.2 rev 33.180 (1986) \xe2\x80\x94 A500 / A1000 / A2000"},
    {"kick34005.A500", "amiga-os-130.rom",
     "v1.3 rev 34.005 (1987) \xe2\x80\x94 A500 / A1000 / A2000"},
    {"kick37175.A500", "amiga-os-204.rom", "v2.04 rev 37.175 (1991) \xe2\x80\x94 A500+"},
    {"kick37350.A600", "amiga-os-205.rom", "v2.05 rev 37.350 (1992) \xe2\x80\x94 A600"},
    {"kick40063.A600", "amiga-os-310-a600.rom",
     "v3.1 rev 40.063 (1993) \xe2\x80\x94 A500 / A600 / A2000"},
    {"kick39106.A1200", "amiga-os-300-a1200.rom", "v3.0 rev 39.106 (1992) \xe2\x80\x94 A1200"},
    {"kick40068.A1200", "amiga-os-310-a1200.rom", "v3.1 rev 40.068 (1993) \xe2\x80\x94 A1200"},
    {"kick39106.A4000", "amiga-os-300-a4000.rom", "v3.0 rev 39.106 (1992) \xe2\x80\x94 A4000"},
    {"kick40068.A4000", "amiga-os-310-a4000.rom", "v3.1 rev 40.068 (1993) \xe2\x80\x94 A4000"},
    {"kick34005.CDTV", "amiga-os-130-cdtv.rom", "CDTV Extended ROM v1.0 (1991)"},
    {"kick40060.CD32", "amiga-os-310-cd32.rom", "v3.1 rev 40.060 (1993) \xe2\x80\x94 CD32"},
    {"kick40060.CD32.ext", "amiga-os-310-cd32-ext.rom", "CD32 Extended ROM rev 40.060 (1993)"},
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* PreferencesView::OptionDefault(const std::string& key)
{
    const auto* option = core::FindEmulatorOption(key);
    return (option && option->HasDefault()) ? option->defaultValue : "";
}

// A width designed at 16 px text, scaled to the text size in use now. Widths written as bare
// pixels stay put while the interface zoom (Preferences > Appearance) and the DPI factor grow the
// text inside them, which clips the labels.
static float Em(float px16) { return px16 * ImGui::GetFontSize() / 16.0f; }

// Draws a small "(?) " tooltip marker after the previous widget.
static void HelpMarker(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Draws a small orange restart indicator "↺" after the previous widget.
// Used for settings that require an emulator restart to take effect.
static void ResetMarker()
{
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.0f, 1.0f), "\xe2\x86\xba"); // UTF-8 ↺
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Requires emulator restart to take effect.");
}

bool PreferencesView::Combo(const char* label, const std::string& key)
{
    const auto* option = core::FindEmulatorOption(key);
    if (!option || option->valueCount == 0) {
        // A key with no value list has no combo to draw. Loud rather than silent: a typo here
        // used to show an empty dropdown that wrote nothing.
        ImGui::TextDisabled("%s: unknown option %s", label, key.c_str());
        return false;
    }

    std::string current = OptionDefault(key);
    if (auto it = m_vars.find(key); it != m_vars.end())
        current = it->second;

    const int count = static_cast<int>(option->valueCount);
    int idx = 0;
    for (int i = 0; i < count; ++i) {
        if (current == option->values[i].value) {
            idx = i;
            break;
        }
    }

    const auto getter = [](void* data, int i) -> const char* {
        return static_cast<const core::EmulatorOptionValue*>(data)[i].label;
    };
    if (ImGui::Combo(label, &idx, getter, const_cast<core::EmulatorOptionValue*>(option->values),
                     count)) {
        m_vars[key] = option->values[idx].value;
        return true;
    }
    return false;
}

bool PreferencesView::Checkbox(const char* label, const std::string& key)
{
    std::string current = OptionDefault(key);
    if (auto it = m_vars.find(key); it != m_vars.end())
        current = it->second;

    bool checked = (current == "enabled");
    if (ImGui::Checkbox(label, &checked)) {
        m_vars[key] = checked ? "enabled" : "disabled";
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void PreferencesView::Open()
{
    m_isOpen = true;
    m_needsLoad = true;
}

void PreferencesView::OpenAppearance()
{
    Open();
    m_selectAppearanceTab = true;
    ImGui::SetWindowFocus("Emulator Preferences");
}

void PreferencesView::OpenSystemFiles()
{
    Open();
    m_selectSystemFilesTab = true;
    ImGui::SetWindowFocus("Emulator Preferences");
}

void PreferencesView::SetEmulator(AmigaEmulator* emu)
{
    m_emulator = emu;
    m_finder.SetEmulator(emu);
    // Copies go to the folder in the box above, Applied or not: it is the one being looked at
    m_finder.SetRomDirectory([this]() { return std::filesystem::u8path(m_kickstartRomDir); });
}

void PreferencesView::Draw()
{
    if (!m_isOpen)
        return;

    if (m_needsLoad)
        LoadSettings();
    else
        SyncExternalChanges();

    ImGui::SetNextWindowSize(ImVec2(Em(640), Em(680)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Emulator Preferences", &m_isOpen)) {
        ImGui::End();
        return;
    }

    // Reserve height for the pinned bottom bar: separator + spacing + button row + padding
    const float bottomBarH = ImGui::GetStyle().ItemSpacing.y + 1.0f // separator
                             + ImGui::GetStyle().ItemSpacing.y +
                             ImGui::GetFrameHeight() // button row
                             + ImGui::GetStyle().WindowPadding.y;

    // Scrollable tab area
    if (ImGui::BeginChild("##prefs_scroll", ImVec2(0.0f, -bottomBarH), false)) {
        // Legend
        ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.0f, 1.0f), "\xe2\x86\xba");
        ImGui::SameLine();
        ImGui::TextDisabled("= requires emulator restart");

        ImGui::Spacing();

        if (ImGui::BeginTabBar("PrefsTabBar")) {
            if (ImGui::BeginTabItem("General")) {
                DrawGeneralTab();
                ImGui::EndTabItem();
            }
            const ImGuiTabItemFlags appearanceFlags =
                m_selectAppearanceTab ? ImGuiTabItemFlags_SetSelected : 0;
            m_selectAppearanceTab = false;
            if (ImGui::BeginTabItem("Appearance", nullptr, appearanceFlags)) {
                DrawAppearanceTab();
                ImGui::EndTabItem();
            }
            const ImGuiTabItemFlags systemFilesFlags =
                m_selectSystemFilesTab ? ImGuiTabItemFlags_SetSelected : 0;
            m_selectSystemFilesTab = false;
            if (ImGui::BeginTabItem("System Files", nullptr, systemFilesFlags)) {
                DrawSystemFilesTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("CPU & Chipset")) {
                DrawCpuChipsetTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("RAM")) {
                DrawRamTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Video")) {
                DrawVideoTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Audio")) {
                DrawAudioTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Floppy")) {
                DrawFloppyTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Ports")) {
                DrawPortsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Hard Drives")) {
                if (m_emulatorView)
                    m_emulatorView->DrawHardDriveEditor(true);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();

    // Pinned bottom action bar
    ImGui::Separator();
    ImGui::Spacing();

    // Sized to their labels, so they fit at every interface zoom.
    if (ImGui::Button("Apply", ImVec2(Em(90), 0)))
        ApplySettings();

    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults"))
        ResetToDefaults();

    // Right-aligned, but never over the buttons to its left when the window is narrow.
    const float closeW = Em(70);
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - closeW -
                                                              ImGui::GetStyle().WindowPadding.x));
    if (ImGui::Button("Close", ImVec2(closeW, 0)))
        m_isOpen = false;

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Settings load / apply / reset
// ---------------------------------------------------------------------------

void PreferencesView::ScanKickstartRoms()
{
    m_hasRomKey = std::filesystem::exists(std::filesystem::path(m_kickstartRomDir) / "rom.key");
    m_hasEncryptedRoms = core::HasEncryptedRoms(std::filesystem::u8path(m_kickstartRomDir));

    // What the directory holds, by content: a ROM is found whatever it is called, and reported
    // as what it really is. kKickstarts[] is no longer consulted for this - it only maps a
    // legacy saved filename onto its Amiga Forever twin in ApplySettings.
    //
    // Identification lives in the core, so the DLL has to be loaded for it - and Preferences is
    // usually opened long before anything has started the emulator, which made the page report
    // "this core cannot identify ROMs" on a core that can. Loading it here is the same on-demand
    // load EmulatorView::RunHardfile does, and it builds no machine: LoadCore only resolves the
    // exports.
    m_scannedRoms.clear();
    if (m_emulator && !m_emulator->IsCoreLoaded())
        m_emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath());
    m_romScanSupported = m_emulator && core::KickstartRoms::Available(*m_emulator);
    m_romScanRan = true;
    if (m_romScanSupported)
        m_scannedRoms = core::KickstartRoms::Scan(*m_emulator, m_kickstartRomDir);
}

void PreferencesView::LoadSettings()
{
    // Populate working vars from emulator user variables, falling back to defaults.
    auto get = [this](const std::string& key, const std::string& def) -> std::string {
        if (m_emulator) {
            const auto& uv = m_emulator->GetUserVariables();
            auto it = uv.find(key);
            if (it != uv.end())
                return it->second;
        }
        auto it2 = m_vars.find(key);
        if (it2 != m_vars.end())
            return it2->second;
        return def;
    };

    // Every option the dialog manages, with the same default the emulator hands the core while
    // the user has not chosen one. The two lists used to be written out separately here and in
    // AmigaEmulator's environment callback, which is how the dialog came to show a floppy
    // write-redirect default the core was never given: Apply then changed the machine without
    // the user touching the control.
    // ("80" for puae_floppy_sound is 20% loudness, not 80%: the option is an attenuation, so 0
    // is full volume and 100 is silence. The labels in the combo are the only place it reads the
    // right way up.)
    for (const auto& option : core::GetEmulatorOptions())
        m_vars[option.key] = get(option.key, option.defaultValue);

    // Not in the option table: the core builds the Kickstart list at runtime from the ROMs it
    // finds, so the combo is built from ScanKickstartRoms() instead.
    m_vars["puae_kickstart"] = get("puae_kickstart", "auto");
    // The extended ROM slot (CD32, CDTV). "auto" leaves the model preset's own.
    m_vars["puae_kickstart_ext"] = get("puae_kickstart_ext", "auto");

    // Kickstart ROM directory (not a PuAE variable — stored separately in UserPreferences).
    // Default to ./emulation when not explicitly configured.
    if (m_userPrefs)
        m_kickstartRomDir = m_userPrefs->Get().kickstartRomDir;

    if (m_kickstartRomDir.empty()) {
        try {
            m_kickstartRomDir = std::filesystem::canonical("./emulation").string();
        } catch (...) {
            m_kickstartRomDir = "./emulation";
        }
    }

    ScanKickstartRoms();

    if (m_emulator)
        m_emulatorVars = m_emulator->GetUserVariables();
    m_needsLoad = false;
}

void PreferencesView::SyncExternalChanges()
{
    if (!m_emulator)
        return;

    // Only keys somebody else moved since the last look, and only ones this dialog owns: the
    // user's own unapplied edits stay put, and puae_model stays out of the working copy so Apply
    // cannot write back the legacy override SelectModelPreset() goes out of its way to erase.
    // Switching model from the Emulator menu with Preferences still open used to leave the old
    // machine's RAM and Kickstart sitting here, so the next Apply quietly put it back.
    const auto owned = [](const std::string& key) {
        return core::FindEmulatorOption(key) != nullptr || key == "puae_kickstart" ||
               key == "puae_kickstart_ext";
    };

    const auto& live = m_emulator->GetUserVariables();
    for (const auto& [key, value] : live) {
        if (!owned(key))
            continue;
        auto previous = m_emulatorVars.find(key);
        if (previous == m_emulatorVars.end() || previous->second != value)
            m_vars[key] = value;
    }
    for (const auto& [key, value] : m_emulatorVars) {
        (void)value;
        if (owned(key) && live.find(key) == live.end())
            m_vars[key] = m_emulator->ResolveCoreVariable(key);
    }
    m_emulatorVars = live;
}

void PreferencesView::ApplySettings()
{
    if (m_emulator) {
        // Build user variable map. For puae_kickstart, resolve to full path
        // if a custom ROM directory is configured so PuAE can find the file.
        std::unordered_map<std::string, std::string> uvars(m_vars.begin(), m_vars.end());

        const std::string& ksFile = uvars["puae_kickstart"];
        if (!m_kickstartRomDir.empty() && ksFile != "auto" && ksFile != "aros") {
            // Check it's a bare filename (not already a full path)
            bool hasSlash =
                (ksFile.find('/') != std::string::npos || ksFile.find('\\') != std::string::npos);
            if (!hasSlash) {
                std::filesystem::path primaryPath =
                    std::filesystem::path(m_kickstartRomDir) / ksFile;

                if (std::filesystem::exists(primaryPath)) {
                    uvars["puae_kickstart"] = primaryPath.string();
                } else {
                    // Try to find the alternate name from our table
                    for (int i = 0; i < kNumKickstarts; ++i) {
                        if (ksFile == kKickstarts[i].filename) {
                            std::filesystem::path alternatePath =
                                std::filesystem::path(m_kickstartRomDir) / kKickstarts[i].alternate;
                            if (std::filesystem::exists(alternatePath)) {
                                uvars["puae_kickstart"] = alternatePath.string();
                            }
                            break;
                        }
                    }
                }
            }
        }

        m_emulator->SetUserVariables(uvars);

        // Update system directory so PuAE can find ROMs by filename too
        if (!m_kickstartRomDir.empty())
            m_emulator->SetSystemDirectory(m_kickstartRomDir);
    }
    if (m_userPrefs) {
        m_userPrefs->SetEmulatorSettings(m_vars);
        m_userPrefs->SetKickstartRomDir(m_kickstartRomDir);
    }
}

void PreferencesView::ResetToDefaults()
{
    if (m_emulator)
        m_emulator->ClearAllUserVariables();

    if (m_userPrefs) {
        m_userPrefs->SetEmulatorSettings({});
        m_userPrefs->SetKickstartRomDir("");
    }

    m_vars.clear();
    m_kickstartRomDir.clear();
    m_needsLoad = true;
}

// ---------------------------------------------------------------------------
// Tab: General
// ---------------------------------------------------------------------------

void PreferencesView::DrawGeneralTab()
{
    if (!m_userPrefs)
        return;

    ImGui::Spacing();
    ImGui::SeparatorText("Emulator");
    ImGui::Spacing();

    bool runWhenHidden = m_userPrefs->Get().emulatorRunWhenHidden;
    if (ImGui::Checkbox("Continue running when window is hidden", &runWhenHidden)) {
        m_userPrefs->SetEmulatorRunWhenHidden(runWhenHidden);
    }
    HelpMarker(
        "When enabled, the emulator keeps running even if the emulator window is closed or in "
        "an inactive tab. Disable this to automatically pause execution when the window is not "
        "visible.");

    bool bartman = m_userPrefs->Get().bartmanDebugEnabled;
    if (ImGui::Checkbox("Enable Bartman debug support", &bartman)) {
        m_userPrefs->SetBartmanDebugEnabled(bartman);
        if (m_emulator)
            m_emulator->SetBartmanDebugSupport(bartman);
    }
    HelpMarker("Installs the UAE debug gate ($F0FF60) used by Amiga programs built with Bartman's "
               "vscode-amiga-debug (debug_register_bitmap/palette). Enables live capture in the "
               "'Bartman Bitmaps' debugger window. Re-run your program (Instant Run / reset) after "
               "toggling for it to take effect. Leaves the UAE boot rom mapped while enabled.");

    const char* launchLabels[] = {"Automatic (fast with ADF fallback)", "Host directory (fast)",
                                  "Bootable ADF (compatible)"};
    const char* launchValues[] = {"automatic", "host_directory", "bootable_adf"};
    int launchMode = 0;
    const std::string& launchValue = m_userPrefs->Get().peekabooLaunchMode;
    if (launchValue == "host_directory")
        launchMode = 1;
    else if (launchValue == "bootable_adf")
        launchMode = 2;
    ImGui::SetNextItemWidth(Em(300));
    if (ImGui::Combo("Compile-and-run launch", &launchMode, launchLabels, 3)) {
        m_userPrefs->SetPeekabooLaunchMode(launchValues[launchMode]);
    }
    HelpMarker(
        "Automatic mounts the compiler output directory directly as read-only DH1 and falls back "
        "to the existing bootable-ADF runner if PuAE rejects the host mount. Host directory "
        "requires the fast path. Bootable ADF always uses the compatibility path.");

    ImGui::Spacing();
    ImGui::SeparatorText("Claude / MCP Server");
    ImGui::Spacing();

    const auto& p = m_userPrefs->Get();
    bool enabled = p.mcpEnabled;
    int port = p.mcpPort;
    std::string token = p.mcpToken;

    auto genToken = []() {
        static const char* hex = "0123456789abcdef";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 15);
        std::string out;
        for (int i = 0; i < 32; ++i)
            out += hex[dist(gen)];
        return out;
    };

    bool changed = false;
    if (ImGui::Checkbox("Enable MCP server (Claude Desktop integration)", &enabled)) {
        if (enabled && token.empty())
            token = genToken();
        changed = true;
    }
    HelpMarker("Exposes emulator state (registers, memory, disassembly, annotations, screenshots) "
               "to Claude Desktop via a local MCP bridge, so Claude can analyse and annotate code. "
               "Listens on 127.0.0.1 only. Restart Peekaboo to apply changes.");

    ImGui::SetNextItemWidth(Em(120));
    if (ImGui::InputInt("Port", &port)) {
        if (port < 1024)
            port = 1024;
        if (port > 65535)
            port = 65535;
        changed = true;
    }

    ImGui::Text("Token: %s", token.empty() ? "(none)" : token.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Regenerate")) {
        token = genToken();
        changed = true;
    }

    if (ImGui::Button("Copy Claude Desktop config")) {
        std::string bridgePath;
        try {
            bridgePath = std::filesystem::absolute("mcp/peekaboo_mcp_bridge.py").string();
        } catch (...) {
            bridgePath = "mcp/peekaboo_mcp_bridge.py";
        }
        std::string snippet = "{\n"
                              "  \"mcpServers\": {\n"
                              "    \"peekaboo\": {\n"
                              "      \"command\": \"python3\",\n"
                              "      \"args\": [\"" +
                              bridgePath +
                              "\"],\n"
                              "      \"env\": {\n"
                              "        \"PEEKABOO_MCP_URL\": \"http://127.0.0.1:" +
                              std::to_string(port) +
                              "\",\n"
                              "        \"PEEKABOO_MCP_TOKEN\": \"" +
                              token +
                              "\"\n"
                              "      }\n"
                              "    }\n"
                              "  }\n"
                              "}\n";
        ImGui::SetClipboardText(snippet.c_str());
    }
    HelpMarker("Copies a claude_desktop_config.json snippet to the clipboard. Paste it into your "
               "Claude Desktop config and restart Claude Desktop.");

    if (changed)
        m_userPrefs->SetMcpSettings(enabled, port, token);

    ImGui::Spacing();
    ImGui::SeparatorText("GDB Server (VS Code / source-level debugging)");
    ImGui::Spacing();

    bool gdbEnabled = p.gdbEnabled;
    int gdbPort = p.gdbPort;
    bool gdbChanged = false;

    if (ImGui::Checkbox("Enable GDB server", &gdbEnabled))
        gdbChanged = true;
    HelpMarker("Lets a real m68k-amiga-elf-gdb attach to the running program, so VS Code can show "
               "source-level breakpoints, variables, watches and call stacks while Peekaboo keeps "
               "its own debugger windows live. Listens on 127.0.0.1 only, but note it grants full "
               "read/write access to emulated memory, which is why it is off by default. Restart "
               "Peekaboo to apply changes.");

    ImGui::SetNextItemWidth(Em(120));
    if (ImGui::InputInt("Port##gdb", &gdbPort)) {
        if (gdbPort < 1024)
            gdbPort = 1024;
        if (gdbPort > 65535)
            gdbPort = 65535;
        gdbChanged = true;
    }
    HelpMarker("Default 2346. Deliberately not 2345: Bartman's amiga-debug extension hardcodes "
               "that port for its own WinUAE, and attaching to the wrong emulator is a confusing "
               "way to lose an afternoon.");

    if (gdbChanged)
        m_userPrefs->SetGdbSettings(gdbEnabled, gdbPort);
}

// ---------------------------------------------------------------------------
// Tab: Appearance
// ---------------------------------------------------------------------------

namespace {

// The Amiga ROM and demoscene faces are drawn on an 8 px grid: at a multiple of 8 px every stem
// lands on a whole pixel, anywhere between they go soft.
bool IsPixelFace(const std::string& face)
{
    for (const char* tag : {"Topaz", "MicroKnight", "P0T-NOoDLE", "mO'sOul", "pixel"}) {
        if (face.find(tag) != std::string::npos)
            return true;
    }
    return false;
}

constexpr const char* kFontSample = "MOVE.L D0,(A0)+ ; $C0FFEE";

int ToPercent(float scale) { return static_cast<int>(std::lround(scale * 100.0f)); }
float FromPercent(int pct) { return static_cast<float>((pct + 2) / 5 * 5) / 100.0f; }

} // namespace

void PreferencesView::DrawAppearanceTab()
{
    if (!m_userPrefs)
        return;
    using Prefs = core::UserPreferences;

    // --- Screen profile ---------------------------------------------------------------------
    // First, and before the font prefs are read below: switching swaps them.
    if (m_getScreenProfile && m_setScreenProfile) {
        ImGui::Spacing();
        ImGui::SeparatorText("Screen profile");
        ImGui::Spacing();
        const core::ScreenProfile current = m_getScreenProfile();
        for (core::ScreenProfile profile : core::kAllScreenProfiles) {
            if (profile != core::kAllScreenProfiles[0])
                ImGui::SameLine();
            const std::string label =
                std::string(core::ScreenProfileLabel(profile)) +
                (profile == core::ScreenProfile::Hires ? " (large screens)" : " (small screens)");
            if (ImGui::RadioButton(label.c_str(), profile == current) && profile != current)
                m_setScreenProfile(profile);
        }
        HelpMarker("Workspace layouts, the text sizes below, the main window's size and table "
                   "column widths are kept per profile. Switching swaps them all.\n"
                   "Also in View > Screen Profile.");
        ImGui::TextDisabled("The sizes below belong to the %s profile.",
                            core::ScreenProfileLabel(m_getScreenProfile()));
    }

    auto fonts = m_userPrefs->Get().fonts;
    const auto commit = [&](bool persist = true) { m_userPrefs->SetFontPrefs(fonts, persist); };

    // Sizes in pixels, from the live style so they match what the window is drawn at. The DPI
    // factor is baked into FontSizeBase at load; FontScaleMain is the interface zoom.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float unzoomedPx = style.FontSizeBase * style.FontScaleDpi;
    const float uiScaleShown = m_draftUiScale > 0.0f ? m_draftUiScale : fonts.uiScale;
    const float interfacePx = unzoomedPx * uiScaleShown;

    const auto faceFont = [this](const std::string& face) -> ImFont* {
        return (face.empty() || !m_fontManager) ? nullptr : m_fontManager->GetFont(face);
    };
    // The face a role really draws in: its own, or the interface's when it follows that.
    const auto effectiveFace = [&](int role) -> const std::string& {
        const auto& own = fonts.roles[role].face;
        return own.empty() ? fonts.roles[0].face : own;
    };
    const auto faceLabel = [](int role, const std::string& face) -> std::string {
        if (!face.empty())
            return face;
        return role == 0 ? "System monospace" : "Interface font";
    };

    // --- Interface size ---------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::SeparatorText("Interface size");
    ImGui::Spacing();

    if (ImGui::Button(ICON_FA_MINUS "##uismaller")) {
        fonts.uiScale = Prefs::StepUiScale(fonts.uiScale, -1);
        commit();
    }
    ImGui::SetItemTooltip("Smaller (Ctrl+-)");
    ImGui::SameLine();
    int uiPct = ToPercent(uiScaleShown);
    ImGui::SetNextItemWidth(Em(220));
    if (ImGui::SliderInt("##uiscale", &uiPct, ToPercent(Prefs::kMinUiScale),
                         ToPercent(Prefs::kMaxUiScale), "%d%%", ImGuiSliderFlags_AlwaysClamp))
        m_draftUiScale = FromPercent(uiPct);
    if (ImGui::IsItemDeactivatedAfterEdit() && m_draftUiScale > 0.0f) {
        fonts.uiScale = m_draftUiScale;
        m_draftUiScale = -1.0f;
        commit();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS "##uilarger")) {
        fonts.uiScale = Prefs::StepUiScale(fonts.uiScale, +1);
        commit();
    }
    ImGui::SetItemTooltip("Larger (Ctrl+=)");
    ImGui::SameLine();
    ImGui::BeginDisabled(fonts.uiScale == 1.0f && m_draftUiScale < 0.0f);
    if (ImGui::Button("100%##uireset")) {
        fonts.uiScale = 1.0f;
        m_draftUiScale = -1.0f;
        commit();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Reset (Ctrl+0)");
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f px", interfacePx);
    HelpMarker("Zooms the whole interface: text, padding, buttons and spacing grow together, so a "
               "larger size is never a cramped one. The slider applies when you let go.\n"
               "From anywhere: Ctrl+= / Ctrl+- / Ctrl+0, or View > Text Size.");

    // --- Text roles: the list, then the highlighted row's settings --------------------------
    ImGui::Spacing();
    ImGui::SeparatorText("Text in dense windows");
    ImGui::Spacing();

    if (ImGui::BeginTable("##fontroles", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthStretch, 0.24f);
        ImGui::TableSetupColumn("Font", ImGuiTableColumnFlags_WidthStretch, 0.24f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.16f);
        ImGui::TableSetupColumn("Sample", ImGuiTableColumnFlags_WidthStretch, 0.36f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < kFontRoleCount; ++i) {
            const auto role = static_cast<FontRole>(i);
            const auto& rp = fonts.roles[i];
            const float scale = (i == 0) ? uiScaleShown : rp.scale;
            const float px = (i == 0) ? interfacePx : interfacePx * rp.scale;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::Selectable(FontRoleName(role), m_fontRoleSel == i,
                                  ImGuiSelectableFlags_SpanAllColumns))
                m_fontRoleSel = i;
            ImGui::SetItemTooltip("%s", FontRoleWindows(role));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(faceLabel(i, rp.face).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d%%  %.0f px", ToPercent(scale), px);
            ImGui::TableNextColumn();
            // The sample is drawn exactly as the role will be: its face at its size. The
            // interface row divides out the zoom already applied, so a mid-drag value previews.
            const float sampleBase =
                style.FontSizeBase * ((i == 0) ? uiScaleShown / fonts.uiScale : rp.scale);
            ImGui::PushFont(faceFont(i == 0 ? rp.face : effectiveFace(i)), sampleBase);
            ImGui::TextUnformatted(kFontSample);
            ImGui::PopFont();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    const int sel = std::clamp(m_fontRoleSel, 0, kFontRoleCount - 1);
    const auto selRole = static_cast<FontRole>(sel);
    auto& rp = fonts.roles[sel];
    const float selPx = (sel == 0) ? interfacePx : interfacePx * rp.scale;

    ImGui::Spacing();
    ImGui::TextUnformatted(FontRoleName(selRole));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", FontRoleWindows(selRole));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(Em(300));
    if (ImGui::BeginCombo("Font##roleface", faceLabel(sel, rp.face).c_str())) {
        if (ImGui::Selectable(sel == 0 ? "System monospace" : "Interface font", rp.face.empty())) {
            rp.face.clear();
            commit();
        }
        if (m_fontManager) {
            ImGui::Separator();
            for (const auto& face : m_fontManager->GetSelectableFaces()) {
                // Each entry in its own face, so the list is its own preview.
                ImGui::PushFont(m_fontManager->GetFont(face), 0.0f);
                const bool picked = ImGui::Selectable(face.c_str(), rp.face == face);
                ImGui::PopFont();
                if (picked) {
                    rp.face = face;
                    commit();
                }
            }
        }
        ImGui::EndCombo();
    }
    HelpMarker("Topaz, MicroKnight, P0T-NOoDLE and mO'sOul are pixel fonts, drawn on an 8 px "
               "grid: they are sharpest at 8, 16, 24 or 32 px. Snap moves the size to the "
               "nearest of those.");

    // The Interface row's size is the zoom above; its slider is shown but not live.
    ImGui::BeginDisabled(sel == 0);
    int rolePct = (sel == 0) ? ToPercent(uiScaleShown) : ToPercent(rp.scale);
    ImGui::SetNextItemWidth(Em(300));
    if (ImGui::SliderInt("Size (% of interface)##rolesize", &rolePct,
                         ToPercent(Prefs::kMinRoleScale), ToPercent(Prefs::kMaxRoleScale), "%d%%",
                         ImGuiSliderFlags_AlwaysClamp) &&
        sel != 0) {
        rp.scale = FromPercent(rolePct);
        commit(false); // live while dragging; the listing does not move under the mouse
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && sel != 0)
        commit();
    ImGui::EndDisabled();
    if (sel == 0)
        ImGui::SetItemTooltip("The interface is sized by Interface size, above.");

    const bool pixelFace = IsPixelFace(sel == 0 ? rp.face : effectiveFace(sel));
    const float crispPx = std::max(8.0f, std::round(selPx / 8.0f) * 8.0f);
    const bool alreadyCrisp = std::fabs(selPx - crispPx) < 0.5f;
    ImGui::SameLine();
    ImGui::BeginDisabled(!pixelFace || alreadyCrisp);
    if (ImGui::Button("Snap to crisp size")) {
        if (sel == 0)
            fonts.uiScale = crispPx / unzoomedPx;
        else
            rp.scale = crispPx / interfacePx;
        m_draftUiScale = -1.0f;
        commit();
    }
    ImGui::EndDisabled();
    if (!pixelFace)
        ImGui::SetItemTooltip("For pixel fonts (Topaz, MicroKnight, ...).");
    else
        ImGui::SetItemTooltip("%.0f px -> %.0f px", selPx, crispPx);

    ImGui::Spacing();
    if (ImGui::Button("Reset this role")) {
        rp = {};
        commit();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset all appearance")) {
        fonts = {};
        m_draftUiScale = -1.0f;
        commit();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Ctrl+wheel over a dense window resizes its role's text.");
}

// ---------------------------------------------------------------------------
// Tab: System Files
// ---------------------------------------------------------------------------

void PreferencesView::DrawSystemFilesTab()
{
    ImGui::SeparatorText("Kickstart ROM");

    // --- ROM Directory row ---
    ImGui::TextUnformatted("ROM Directory:");
    HelpMarker("Directory containing Kickstart ROM files (e.g. kick34005.A500).\n"
               "Leave blank to use the default ./emulation/ directory.\n"
               "Click Scan after changing to refresh availability.");

    float btnW = Em(80);
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW * 2.0f - spacing * 2.0f);
    char romDirBuf[1024];
    snprintf(romDirBuf, sizeof(romDirBuf), "%s", m_kickstartRomDir.c_str());
    if (ImGui::InputText("##romdir", romDirBuf, sizeof(romDirBuf)))
        m_kickstartRomDir = romDirBuf;

    ImGui::SameLine();
    if (ImGui::Button("Browse...##romdir", ImVec2(btnW, 0))) {
        nfdchar_t* outPath = nullptr;
        nfdresult_t result = NFD_PickFolder(
            &outPath, m_kickstartRomDir.empty() ? nullptr : m_kickstartRomDir.c_str());
        ImGui::GetIO().ClearInputKeys();
        if (result == NFD_OKAY) {
            m_kickstartRomDir = outPath;
            NFD_FreePath(outPath);
            ScanKickstartRoms();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Scan##romscan", ImVec2(btnW, 0)))
        ScanKickstartRoms();

    // Only when it matters: an encrypted Amiga Forever ROM is here and its key is not. Modern Amiga
    // Forever ROMs are plain, and other people's ROMs never needed one.
    if (!m_hasRomKey && m_hasEncryptedRoms) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("This folder has encrypted Amiga Forever ROMs and no rom.key: they will "
                           "not load. Copy rom.key from Amiga Forever's rom folder beside them.");
        ImGui::PopStyleColor();
    }

    // --- What the ROM directory actually holds -----------------------------------------
    //
    // The table is the picker: a row is a ROM identified by its contents, so a file under any
    // name shows up as what it really is. The two slots below act on whatever is highlighted,
    // which is the shape Adam asked for - the list on top, the settings under it.
    ImGui::Spacing();
    if (!m_romScanRan)
        ScanKickstartRoms();

    std::string& ksVal = m_vars["puae_kickstart"];
    std::string& extVal = m_vars["puae_kickstart_ext"];

    // The empty state shows the real table greyed out rather than a wall of text.
    const bool haveRoms = !m_scannedRoms.empty();
    ImGui::BeginDisabled(!haveRoms);
    const ImVec2 tableSize(0.0f, ImGui::GetTextLineHeightWithSpacing() * 7.0f);
    if (ImGui::BeginTable("##romtable", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          tableSize)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ROM", ImGuiTableColumnFlags_WidthStretch, 0.44f);
        ImGui::TableSetupColumn("Revision", ImGuiTableColumnFlags_WidthStretch, 0.14f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.12f);
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(m_scannedRoms.size()); ++i) {
            const auto& rom = m_scannedRoms[i];
            const std::string path = rom.path.string();
            const std::string file = rom.path.filename().string();
            // A slot holds a full path once it has been picked here, so compare on that.
            const bool isKickstart = (ksVal == path);
            const bool isExtended = (extVal == path);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            // One selectable spanning the row: clicking anywhere in it assigns the ROM to the
            // slot it belongs in, which is the only sensible thing a double-click could mean.
            if (ImGui::Selectable("##rompick", isKickstart || isExtended,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap)) {
                if (rom.IsExtended())
                    extVal = path;
                else
                    ksVal = path;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n%s\nClick to use as the %s.", rom.DisplayName().c_str(),
                                  path.c_str(), rom.IsExtended() ? "extended ROM" : "Kickstart");
            }
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(rom.DisplayName().c_str());
            // Say which slot a row is currently filling, so the highlight is never ambiguous.
            if (isKickstart) {
                ImGui::SameLine();
                ImGui::TextDisabled("(Kickstart)");
            } else if (isExtended) {
                ImGui::SameLine();
                ImGui::TextDisabled("(extended)");
            }

            ImGui::TableNextColumn();
            ImGui::Text("%d.%d", rom.romVersion, rom.romRevision);
            ImGui::TableNextColumn();
            ImGui::Text("%u KB", rom.size / 1024u);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(file.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    if (!haveRoms) {
        if (!m_romScanSupported) {
            // Either the DLL would not load at all, or it predates adm_identify_rom.
            ImGui::TextDisabled(
                "This PuAE core cannot identify ROMs - rebuild it (needs core 1.96.0).");
        } else {
            ImGui::TextDisabled("No Kickstart ROMs found in the ROM directory.");
        }
    }

    // --- The two slots -------------------------------------------------------------------
    //
    // Each is a path, shown read-only with the ROM it resolves to. "Automatic" hands the choice
    // back to the model preset, which is what every machine starts on.
    const float slotBtnW = Em(80);
    const float slotSpacing = ImGui::GetStyle().ItemSpacing.x;

    // Names a slot's current value: a picked ROM by what it is, a bare filename as itself.
    const auto describeSlot = [this](const std::string& value) -> std::string {
        if (value.empty() || value == "auto")
            return "Automatic";
        if (value == "aros")
            return "AROS (Free Kickstart)";
        for (const auto& rom : m_scannedRoms) {
            if (rom.path.string() == value)
                return rom.DisplayName();
        }
        // Picked from outside the ROM directory, or a bare filename from a saved session that
        // predates the picker: identify it rather than showing something the user has to decode.
        if (m_emulator && core::KickstartRoms::Available(*m_emulator)) {
            const std::filesystem::path direct(value);
            for (const auto& candidate :
                 {direct, std::filesystem::path(m_kickstartRomDir) / direct.filename()}) {
                const auto identity =
                    core::KickstartRoms::Identify(*m_emulator, candidate, m_kickstartRomDir);
                if (identity && identity->identified)
                    return identity->DisplayName();
            }
        }
        return std::filesystem::path(value).filename().string();
    };

    // Opens a file dialog and writes the chosen path into `slot`.
    const auto browseForRom = [this](std::string& slot) {
        nfdchar_t* outPath = nullptr;
        nfdfilteritem_t filter[] = {{"Kickstart ROM", "rom,bin,A500,A600,A1200,A4000,CD32,ext"}};
        const nfdresult_t result = NFD_OpenDialog(
            &outPath, filter, 1, m_kickstartRomDir.empty() ? nullptr : m_kickstartRomDir.c_str());
        ImGui::GetIO().ClearInputKeys();
        if (result == NFD_OKAY) {
            slot = outPath;
            NFD_FreePath(outPath);
        }
    };

    ImGui::Spacing();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - slotBtnW * 2.0f -
                            slotSpacing * 3.0f);
    {
        const std::string label = describeSlot(ksVal);
        if (ImGui::BeginCombo("##kssel", label.c_str())) {
            if (ImGui::Selectable("Automatic", ksVal == "auto" || ksVal.empty()))
                ksVal = "auto";
            if (ImGui::Selectable("AROS (Free Kickstart)", ksVal == "aros"))
                ksVal = "aros";
            if (!m_scannedRoms.empty())
                ImGui::Separator();
            for (int i = 0; i < static_cast<int>(m_scannedRoms.size()); ++i) {
                const auto& rom = m_scannedRoms[i];
                if (rom.IsExtended())
                    continue; // an extended ROM is not a Kickstart; it belongs in the slot below
                ImGui::PushID(i);
                const std::string path = rom.path.string();
                if (ImGui::Selectable(rom.DisplayName().c_str(), ksVal == path))
                    ksVal = path;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...##ksrom", ImVec2(slotBtnW, 0)))
        browseForRom(ksVal);
    ImGui::SameLine();
    ImGui::BeginDisabled(ksVal == "auto" || ksVal.empty());
    if (ImGui::Button("Automatic##ksauto", ImVec2(slotBtnW, 0)))
        ksVal = "auto";
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted("Kickstart ROM");
    HelpMarker("The Kickstart the machine boots.\n"
               "Automatic: the model preset's own ROM.\n"
               "Browse picks any file on disk; it is identified by its contents, so the\n"
               "filename does not matter.");
    ResetMarker();
    if (ksVal != "auto" && ksVal != "aros" && !ksVal.empty())
        ImGui::SetItemTooltip("%s", ksVal.c_str());

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - slotBtnW * 2.0f -
                            slotSpacing * 3.0f);
    {
        const std::string label = describeSlot(extVal);
        if (ImGui::BeginCombo("##extsel", label.c_str())) {
            if (ImGui::Selectable("Automatic", extVal == "auto" || extVal.empty()))
                extVal = "auto";
            if (!m_scannedRoms.empty())
                ImGui::Separator();
            for (int i = 0; i < static_cast<int>(m_scannedRoms.size()); ++i) {
                const auto& rom = m_scannedRoms[i];
                if (!rom.IsExtended())
                    continue;
                ImGui::PushID(1000 + i);
                const std::string path = rom.path.string();
                if (ImGui::Selectable(rom.DisplayName().c_str(), extVal == path))
                    extVal = path;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...##extrom", ImVec2(slotBtnW, 0)))
        browseForRom(extVal);
    ImGui::SameLine();
    ImGui::BeginDisabled(extVal == "auto" || extVal.empty());
    if (ImGui::Button("Automatic##extauto", ImVec2(slotBtnW, 0)))
        extVal = "auto";
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted("Extended ROM");
    HelpMarker("The extended ROM a CD32 or CDTV needs alongside its Kickstart.\n"
               "Automatic: the model preset's own. Other machines ignore it.");
    ResetMarker();
    if (extVal != "auto" && !extVal.empty())
        ImGui::SetItemTooltip("%s", extVal.c_str());

    // Copying the user's own ROMs in, from Amiga Forever and the like - the setup wizard's finder
    ImGui::Spacing();
    ImGui::SeparatorText("Find system files");
    if (!m_finder.HasScanned() && !m_finder.IsScanning())
        m_finder.Scan();
    if (m_finder.Draw())
        ScanKickstartRoms(); // the table above shows what was copied in
}

// ---------------------------------------------------------------------------
// Tab: CPU & Chipset
// ---------------------------------------------------------------------------

void PreferencesView::DrawCpuChipsetTab()
{
    ImGui::SeparatorText("CPU");

    Combo("CPU Model##cpu", "puae_cpu_model");
    HelpMarker("CPU chip variant. 'Auto' matches the selected hardware model preset.");
    ResetMarker();

    Combo("FPU Model##fpu", "puae_fpu_model");
    HelpMarker("Floating-Point Unit. 'CPU Internal' is only valid for 68040/68060.");
    ResetMarker();

    Combo("MMU##mmu", "puae_mmu_model");
    HelpMarker("68030, 68040 and 68060 only. Auto gives them a full MMU.\n"
               "EC: a CPU without one, as in an A4000/030 - MMU instructions run but translate "
               "nothing.\n"
               "None: no MMU emulation at all, the fastest.\n"
               "With a full MMU, software that turns translation on (Enforcer, MuForce, VMM) "
               "makes the debugger's addresses logical ones.");
    ResetMarker();

    ImGui::SeparatorText("CPU Accuracy");

    Combo("Compatibility Mode##compat", "puae_cpu_compatibility");
    HelpMarker("Normal: fastest execution, least compatible.\n"
               "More Compatible: enables extra instruction compatibility flags.\n"
               "Memory Cycle Exact: accurate memory bus and blitter timing, approximate CPU.\n"
               "Cycle Exact: most accurate \xe2\x80\x94 required for timing-sensitive demos.");

    // The four UAE flags below are derived, not chosen: the core reads only
    // puae_cpu_compatibility and writes all four from it (libretro-core.c, GET_VAR
    // "cpu_compatibility"). They were four live checkboxes here until they were checked against
    // the core, which declares no option for any of them - ticking one changed nothing. Shown
    // read-only so the mode above still says what it does to the machine.
    {
        const std::string mode = m_vars.count("puae_cpu_compatibility")
                                     ? m_vars["puae_cpu_compatibility"]
                                     : OptionDefault("puae_cpu_compatibility");
        bool cpuCompatible = (mode != "normal");
        bool cpuCycleExact = (mode == "exact");
        bool memCycleExact = (mode == "exact" || mode == "memory");
        bool blitCycleExact = memCycleExact;

        ImGui::BeginDisabled();
        ImGui::Checkbox("CPU Compatible Mode##ccompat", &cpuCompatible);
        ImGui::Checkbox("Cycle-Exact CPU Timing##ce", &cpuCycleExact);
        ImGui::Checkbox("Memory Bus Cycle Exact##mce", &memCycleExact);
        ImGui::Checkbox("Blitter Cycle Exact##bce", &blitCycleExact);
        ImGui::EndDisabled();
    }
    ImGui::TextDisabled("Set by Compatibility Mode \xe2\x80\x94 the core has no separate option "
                        "for these.");

    ImGui::SeparatorText("CPU Speed");

    // Parse throttle value (-900.0 .. 5000.0) → integer percentage (-90 .. 500)
    auto& throttleStr = m_vars["puae_cpu_throttle"];
    if (throttleStr.empty())
        throttleStr = "0.0";
    int throttlePct = 0;
    try {
        throttlePct = static_cast<int>(std::stof(throttleStr) / 10.0f);
    } catch (...) {
        // Malformed saved value — fall back to 0% rather than crashing the dialog.
        throttleStr = "0.0";
    }

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 50.0f);
    if (ImGui::SliderInt("##throttle", &throttlePct, -90, 500, "%d%%")) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f", static_cast<float>(throttlePct) * 10.0f);
        throttleStr = buf;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Speed");
    HelpMarker("CPU speed offset relative to the hardware-accurate rate.\n"
               "0%% = default (real hardware speed).\n"
               "+500%% = 6\xc3\x97 faster than real hardware.\n"
               "-90%% = 10%% of normal speed.\n"
               "Ignored when Compatibility Mode is set to Cycle Exact.");

    ImGui::SeparatorText("Chipset (Blitter)");

    Combo("Immediate Blits##iblits", "puae_immediate_blits");
    HelpMarker("Disabled: the blitter takes the cycles it really takes and the CPU is not held "
               "back for it \xe2\x80\x94 what the hardware does.\n"
               "Wait for Blitter: stalls the CPU on a blit it has not waited for. A compatibility "
               "hack for software that forgets to poll BLTDONE.\n"
               "Immediate: blitter finishes instantly (faster, least accurate).");

    ImGui::SeparatorText("Chipset Collision");

    Combo("Collision Detection##coll", "puae_collision_level");
    HelpMarker("Hardware sprite and playfield collision detection.\n"
               "Full: emulates all collisions correctly. None: fastest, no collision.");

    ImGui::SeparatorText("Chipset Revision");

    // Below the model preset: selecting a model puts all five back to Model Default. An older
    // core has no options for them, so they stay visible but cannot be changed.
    const bool chipsetSupported =
        !m_emulator || !m_emulator->IsCoreLoaded() || m_emulator->HasMachineConfig();
    ImGui::BeginDisabled(!chipsetSupported);
    Combo("Chipset##chipset", "puae_chipset");
    HelpMarker("Which Agnus and Denise generation the machine has. The two mixed entries put one "
               "ECS chip beside an OCS one, as on an A500 with a Fat Agnus upgrade.");
    ResetMarker();
    Combo("Chipset Extra##csextra", "puae_chipset_compatible");
    HelpMarker("The motherboard around the chips: Gayle IDE and PCMCIA, the real-time clock, "
               "Kickstart mirrors and CIA quirks. Changing it away from the model can remove "
               "hardware the model's Kickstart expects (a CD32 without CD32 makes no CD).");
    ResetMarker();
    Combo("Agnus Model##agnus", "puae_agnus_model");
    HelpMarker("A1000 and Velvet Agnus behave as OCS whatever the chipset says, and change the "
               "blitter busy bug and mouse counter behaviour.");
    ResetMarker();
    Combo("Agnus Size##agnussize", "puae_agnus_size");
    HelpMarker("How much Chip RAM address space Agnus decodes. Chip RAM beyond it mirrors.");
    ResetMarker();
    Combo("Denise Model##denise", "puae_denise_model");
    HelpMarker("A1000 (No EHB) and Velvet Denise lack Extra Half-Brite: a sixth bitplane does "
               "not halve the colour.");
    ResetMarker();
    ImGui::EndDisabled();

    const auto running = m_emulator ? m_emulator->GetMachineConfig() : std::nullopt;
    ImGui::BeginDisabled(!running);
    if (running) {
        ImGui::Text("Running: %s on %s, Agnus %s (%s), Denise %s",
                    core::ChipsetName(running->chipsetMask).c_str(),
                    core::ChipsetCompatibleName(running->compatible),
                    core::AgnusModelName(running->agnusModel),
                    core::AgnusSizeName(running->agnusSize),
                    core::DeniseModelName(running->deniseModel));
    } else {
        ImGui::TextUnformatted("Running: -");
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextDisabled("Cycle-exact settings improve accuracy but increase CPU usage.");
}

// ---------------------------------------------------------------------------
// Tab: RAM
// ---------------------------------------------------------------------------

void PreferencesView::DrawRamTab()
{
    ImGui::SeparatorText("Chip Memory");

    Combo("Chip RAM##chipram", "puae_chipmem_size");
    HelpMarker("Chip RAM is shared between the CPU and custom chips (DMA).\n"
               "OCS/ECS machines support up to 1 MB; AGA machines up to 2 MB.\n"
               "Automatic defaults to the current model preset.");
    ResetMarker();

    ImGui::SeparatorText("Slow / Bogo Memory");

    Combo("Slow RAM##slowram", "puae_bogomem_size");
    HelpMarker("Slow (Bogo) RAM at 0xC00000 \xe2\x80\x94 accessible by CPU but not DMA.\n"
               "A500 trapdoor expansions typically provide 0.5 MB of slow RAM.");
    ResetMarker();

    ImGui::SeparatorText("Z2 Fast Memory");

    Combo("Z2 Fast RAM##fastram", "puae_fastmem_size");
    HelpMarker("Zorro II Fast RAM accessible only by the CPU (not custom chips).\n"
               "Speeds up programs that keep code and data in fast RAM.\n"
               "Not available on stock A500 without an expansion card.");
    ResetMarker();

    ImGui::SeparatorText("Z3 Fast Memory");

    Combo("Z3 Fast RAM##z3ram", "puae_z3mem_size");
    HelpMarker("Zorro III Fast RAM \xe2\x80\x94 32-bit wide, A3000/A4000/A1200 with expansion.\n"
               "Large Z3 RAM significantly improves performance for 32-bit software.");
    ResetMarker();

    ImGui::Spacing();
    ImGui::TextDisabled("All RAM changes require an emulator restart to take effect.");
}

// ---------------------------------------------------------------------------
// Tab: Video
// ---------------------------------------------------------------------------

void PreferencesView::DrawVideoTab()
{
    ImGui::SeparatorText("Display Standard");

    Combo("Video Standard##vstd", "puae_video_standard");
    HelpMarker("PAL: 50 Hz, 288 active lines (European standard).\n"
               "NTSC: 60 Hz, 240 active lines (North American standard).\n"
               "Auto: detected from loaded software.");
    ResetMarker();

    ImGui::SeparatorText("Resolution");

    Combo("Output Resolution##vres", "puae_video_resolution");
    HelpMarker("Horizontal output resolution.\n"
               "'Auto' adjusts to the current screen mode (recommended).");

    Combo("Line Mode##vvres", "puae_video_vresolution");
    HelpMarker("Vertical line rendering mode.\n"
               "Single: normal scanlines. Double: doubled lines for non-interlaced output.");

    Combo("Pixel Aspect Ratio##vasp", "puae_video_aspect");
    HelpMarker("Pixel aspect ratio for display.\n"
               "'Auto' uses the correct ratio for the video standard (recommended).");

    ImGui::SeparatorText("Graphics Card (RTG)");

    Combo("RTG Memory##gfxcard", "puae_gfxcard_size");
    HelpMarker("Picasso96 (uaegfx) board memory \xe2\x80\x94 high-colour Workbench screen modes,\n"
               "up to 1280x1024 at 8, 16, 24 or 32 bits.\n"
               "The board only does anything once Picasso96 is installed in the guest;\n"
               "without it AmigaOS never opens an RTG screen and the board sits idle.\n"
               "The A4040 RTG configuration comes with one fitted.");
    ResetMarker();

    Combo("RTG Board Type##gfxcardtype", "puae_gfxcard_type");
    HelpMarker("Zorro III needs a 32-bit address space (68020+), and is the only way past 8 MB.\n"
               "Zorro II is capped at 8 MB and shares the $200000-$A00000 window with Z2 Fast\n"
               "RAM \xe2\x80\x94 on a preset that fills it, a Zorro II board has nowhere to go.");
    ResetMarker();

    ImGui::SeparatorText("Performance");

    Combo("Frame Skip##gfxfps", "puae_gfx_framerate");
    HelpMarker("Skip frames to improve performance. Only recommended on slow hardware.");

    Checkbox("Flicker Fixer##flicker", "puae_gfx_flickerfixer");
    HelpMarker("Remove interlace flicker by blending fields.\n"
               "Affects interlaced screen modes (e.g. Workbench 2.0+ in interlaced mode).");

    ImGui::Spacing();
    ImGui::TextDisabled("Video standard and graphics card changes require an emulator restart.");
}

// ---------------------------------------------------------------------------
// Tab: Audio
// ---------------------------------------------------------------------------

void PreferencesView::DrawAudioTab()
{
    ImGui::SeparatorText("Paula Chip");

    Combo("Stereo Separation##stereo", "puae_sound_stereo_separation");
    HelpMarker("Paula audio channel panning.\n"
               "0%% = mono (channels mixed to centre).\n"
               "100%% = full stereo (left hard-left, right hard-right).");

    Combo("Interpolation##interp", "puae_sound_interpol");
    HelpMarker("Audio sample interpolation algorithm.\n"
               "None: no interpolation (raw samples).\n"
               "Anti: good balance of quality and performance (recommended).\n"
               "Sinc: highest quality, most CPU intensive.");

    ImGui::SeparatorText("Audio Filter");

    Combo("Filter Mode##filt", "puae_sound_filter");
    HelpMarker("Emulated: uses the low-pass filter appropriate for the hardware model.\n"
               "Off: no filter applied.\n"
               "On: filter always active regardless of model.");

    Combo("Filter Type##filttype", "puae_sound_filter_type");
    HelpMarker("Auto: matches the selected hardware model.\n"
               "Standard: A500-style capacitor filter.\n"
               "Enhanced: A1200-style improved filter.");

    ImGui::SeparatorText("Sound Card");

    // An expansion board, so unlike the settings above it needs Apply and a restart.
    ImGui::BeginDisabled(!m_emulator || !m_emulator->HasSoundBoard());
    Combo("Sound Card##soundboard", "puae_sound_board");
    ImGui::EndDisabled();
    HelpMarker("An expansion sound card beside Paula, mixed into the same output.\n"
               "Toccata, Prelude: Zorro II cards. Prelude 1200: the A1200 clock port. Each needs "
               "its Amiga driver or AHI.\n"
               "UAESND: WinUAE's card, for WinUAE's AHI driver. The Zorro III one needs a 32-bit "
               "CPU.\n"
               "Recording input is not connected. Takes effect after Apply and an emulator "
               "restart.");
    ResetMarker();

    ImGui::Spacing();
    ImGui::TextDisabled("Audio changes apply on the next emulated frame; the sound card on the "
                        "next power-on.");
}

// ---------------------------------------------------------------------------
// Tab: Ports
// ---------------------------------------------------------------------------

void PreferencesView::DrawPortsTab()
{
    using Mode = core::SerialPortTarget::Mode;
    if (!m_emulator || !m_emulatorView)
        return;
    if (!m_portsDraftLoaded) {
        m_serialDraft = m_emulator->GetSerialPort();
        const auto u8 = m_emulator->GetParallelOutput().u8string();
        m_parallelDraft.assign(u8.begin(), u8.end());
        m_serialDevices = core::EnumerateHostSerialDevices();
        // Opening the tab is the moment to ask the system for its MIDI devices; the combos
        // below draw from the cache every frame after that.
        core::EnumerateMidiOutputs(true);
        m_portsDraftLoaded = true;
    }
    // Ports apply to the running machine at its next frame; no restart, no Apply button below.
    const bool supported = !m_emulator->IsCoreLoaded() || m_emulator->HasHostPorts();
    const auto status = m_emulator->GetHostPortsStatus();

    ImGui::BeginDisabled(!supported);
    ImGui::SeparatorText("Serial Port");

    static constexpr const char* kModes[] = {"Off", "TCP Server (listen)", "TCP Client (connect)",
                                             "Host Serial Device"};
    int mode = static_cast<int>(m_serialDraft.mode);
    if (ImGui::Combo("Connection##sermode", &mode, kModes, IM_ARRAYSIZE(kModes)))
        m_serialDraft.mode = static_cast<Mode>(mode);
    HelpMarker("TCP Server: the Amiga's serial port listens for one client - a terminal "
               "(telnet, PuTTY raw), a remote debugger, or another emulator dialling in.\n"
               "TCP Client: dials a server, and redials whenever the link drops.\n"
               "Host Serial Device: a real COM port or USB serial adapter, at the baud rate "
               "the Amiga sets.");

    const bool tcp =
        m_serialDraft.mode == Mode::TcpListen || m_serialDraft.mode == Mode::TcpConnect;
    ImGui::BeginDisabled(!tcp);
    char hostBuf[256];
    snprintf(hostBuf, sizeof(hostBuf), "%s", m_serialDraft.host.c_str());
    if (ImGui::InputTextWithHint("Host##serhost", "127.0.0.1", hostBuf, sizeof(hostBuf)))
        m_serialDraft.host = hostBuf;
    HelpMarker("Server: the address to listen on. Empty or 127.0.0.1 accepts connections only "
               "from this computer; 0.0.0.0 from anywhere on the network.\nClient: the machine "
               "to connect to.");
    if (ImGui::InputInt("Port##serport", &m_serialDraft.port))
        m_serialDraft.port = std::clamp(m_serialDraft.port, 1, 65535);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(m_serialDraft.mode != Mode::Device);
    const char* devicePreview =
        m_serialDraft.device.empty() ? "(choose a device)" : m_serialDraft.device.c_str();
    if (ImGui::BeginCombo("Device##serdev", devicePreview)) {
        for (const auto& device : m_serialDevices) {
            if (ImGui::Selectable(device.c_str(), device == m_serialDraft.device))
                m_serialDraft.device = device;
        }
        if (m_serialDevices.empty())
            ImGui::TextDisabled("No serial devices found");
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan##serscan"))
        m_serialDevices = core::EnumerateHostSerialDevices();
    ImGui::EndDisabled();

    const bool serialChanged = !(m_serialDraft == m_emulator->GetSerialPort());
    const bool deviceMissing = m_serialDraft.mode == Mode::Device && m_serialDraft.device.empty();
    ImGui::BeginDisabled(!serialChanged || deviceMissing);
    if (ImGui::Button("Apply Serial Port##serapply"))
        m_emulatorView->SetSerialPort(m_serialDraft);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!status);
    ImGui::TextWrapped("Status: %s",
                       status ? core::DescribeSerialStatus(*status).c_str() : "not running");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Network");

    // A core option like the CPU's, so it goes through Apply and a restart, unlike the ports.
    ImGui::BeginDisabled(!m_emulator->HasBsdsocket());
    Checkbox("bsdsocket.library##bsdsocket", "puae_bsdsocket");
    ImGui::EndDisabled();
    HelpMarker("Amiga programs that open bsdsocket.library - web browsers, FTP and IRC clients, "
               "wget - reach the network through this computer's TCP/IP stack, with no Amiga "
               "TCP/IP stack installed. Adds the UAE Boot ROM.\n"
               "Takes effect after Apply and an emulator restart.");
    ResetMarker();

    ImGui::BeginDisabled(!m_emulator->HasNetworkCard());
    Combo("A2065 Ethernet##a2065", "puae_a2065");
    ImGui::EndDisabled();
    HelpMarker("A Commodore A2065 Zorro II Ethernet card behind a NAT inside the emulator "
               "(SLIRP). The Amiga is 10.0.2.15, the gateway 10.0.2.2 - which is also this "
               "computer - and DNS 10.0.2.3; DHCP answers. Needs a2065.device and a TCP/IP stack "
               "on the Amiga. 'NAT, open ports' also forwards this computer's ports 21-23 and 80 "
               "to the Amiga, on every network interface - other machines can reach them too.\n"
               "Windows builds only. Takes effect after Apply and an emulator restart.");
    ResetMarker();

    ImGui::BeginDisabled(!m_emulator->HasNetworkCard());
    Checkbox("uaenet.device##sana2", "puae_sana2");
    ImGui::EndDisabled();
    HelpMarker("A SANA-II network driver in the UAE Boot ROM, for a TCP/IP stack with no card: "
               "unit 0 is the same NAT as the A2065, unit 1 also has the open ports. Use this or "
               "the A2065, not both.\n"
               "Windows builds only. Takes effect after Apply and an emulator restart.");
    ResetMarker();

    ImGui::SeparatorText("MIDI");

    // Chosen devices apply at once; the Amiga only uses them while its UART runs at MIDI
    // speed, and the serial target gets the bytes the rest of the time.
    const auto midiCombo = [&](const char* label, bool output, int current) {
        int chosen = current;
        const std::string preview = core::MidiDeviceName(current, output);
        if (ImGui::BeginCombo(label, preview.c_str())) {
            if (ImGui::Selectable("None", output ? current < -1 : current < 0))
                chosen = output ? -2 : -1;
            if (output && ImGui::Selectable("System mapper", current == -1))
                chosen = -1;
            for (const auto& device :
                 output ? core::EnumerateMidiOutputs() : core::EnumerateMidiInputs()) {
                if (ImGui::Selectable((device.name + "##" + std::to_string(device.id)).c_str(),
                                      device.id == current))
                    chosen = device.id;
            }
            ImGui::EndCombo();
        }
        return chosen;
    };
    ImGui::BeginDisabled(m_emulator->IsCoreLoaded() && !m_emulator->HasMidi());
    const int midiOut = midiCombo("MIDI Out##midiout", true, m_emulator->GetMidiOutDevice());
    HelpMarker("Where MIDI from the Amiga plays. Amiga MIDI interfaces sit on the serial port: "
               "while software runs it at 31250 baud, bytes go here instead of to the serial "
               "connection above.");
    const int midiIn = midiCombo("MIDI In##midiin", false, m_emulator->GetMidiInDevice());
    HelpMarker("A host MIDI device (a keyboard, a loopback port) whose messages the Amiga "
               "receives on the serial port at MIDI speed.");
    if (midiOut != m_emulator->GetMidiOutDevice() || midiIn != m_emulator->GetMidiInDevice())
        m_emulatorView->SetMidiDevices(midiOut, midiIn);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!status || !status->hasMidi);
    if (status && status->hasMidi && !status->midiError.empty())
        ImGui::TextWrapped("Status: %s", status->midiError.c_str());
    else if (status && status->midiActive)
        ImGui::Text("Status: active - %u messages out, %u bytes in", status->midiOutMessages,
                    status->midiInBytes);
    else
        ImGui::TextUnformatted(status ? "Status: idle (the Amiga is not at MIDI speed)"
                                      : "Status: not running");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Rescan##midiscan"))
        core::EnumerateMidiOutputs(true); // one scan refreshes both lists
    ImGui::SetItemTooltip("Ask the system for its MIDI devices again; the lists above are "
                          "cached from when this tab opened.");

    ImGui::SeparatorText("Parallel Port");

    char pathBuf[1024];
    snprintf(pathBuf, sizeof(pathBuf), "%s", m_parallelDraft.c_str());
    const float browseWidth = 80.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseWidth -
                            ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputTextWithHint("##parpath", "capture file (empty: nothing connected)", pathBuf,
                                 sizeof(pathBuf)))
        m_parallelDraft = pathBuf;
    ImGui::SameLine();
    if (ImGui::Button("Browse...##parbrowse", ImVec2(browseWidth, 0))) {
        nfdchar_t* outPath = nullptr;
        nfdresult_t result = NFD_SaveDialog(&outPath, nullptr, 0, nullptr, "printer.prn");
        ImGui::GetIO().ClearInputKeys();
        if (result == NFD_OKAY) {
            m_parallelDraft = outPath;
            NFD_FreePath(outPath);
        }
    }
    HelpMarker("Every byte the Amiga sends to the parallel port - a printer driver's output, "
               "PRT: or PAR: - is appended to this file.");
    const auto currentParallel = m_emulator->GetParallelOutput().u8string();
    const bool parallelChanged =
        m_parallelDraft != std::string(currentParallel.begin(), currentParallel.end());
    ImGui::BeginDisabled(!parallelChanged);
    if (ImGui::Button("Apply Parallel Port##parapply"))
        m_emulatorView->SetParallelOutput(std::filesystem::u8path(m_parallelDraft));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!status);
    if (status && !status->parallelError.empty())
        ImGui::TextWrapped("Status: %s", status->parallelError.c_str());
    else if (status && !status->parallelPath.empty())
        ImGui::Text("Status: %u bytes written", status->parallelBytes);
    else
        ImGui::TextUnformatted(status ? "Status: nothing connected" : "Status: not running");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Tab: Floppy
// ---------------------------------------------------------------------------

void PreferencesView::DrawFloppyTab()
{
    ImGui::SeparatorText("Drive Speed");

    Combo("Drive Speed##fdspd", "puae_floppy_speed");
    HelpMarker("Floppy drive read/write speed multiplier.\n"
               "Normal: authentic A500 speed (recommended for demos/cracktros).\n"
               "Turbo: instant access \xe2\x80\x94 may break software with drive-timing tricks.");

    ImGui::SeparatorText("Floppy Sound");

    Combo("Floppy Sound Volume##fsnd", "puae_floppy_sound");
    HelpMarker("Emulate mechanical floppy drive seek and access sounds.\n"
               "Uses sampled audio to reproduce authentic drive noise.\n"
               "Off silences it; the toolbar's Disk Sound tick toggles\n"
               "between Off and the level set here.");

    Checkbox("Mute Seek Noise (No Disk)##fsndmute", "puae_floppy_sound_empty_mute");
    HelpMarker("Suppress seek noise on drives that have no disk inserted.\n"
               "Reduces noise when only some drive slots are in use.");

    Combo("Sound Type##fsndtype", "puae_floppy_sound_type");
    HelpMarker("Selects the sampled drive noise profile.\n"
               "Internal: the samples built into the core - always available.\n"
               "External: reads drive_click_A500.wav and friends from the\n"
               "system directory's uae_data folder. No files there means\n"
               "no drive sound at all.");

    ImGui::SeparatorText("Drive Options");

    // Not a PuAE variable - an application preference, stored in UserPreferences.
    if (m_userPrefs) {
        bool openProtected = m_userPrefs->Get().openImagesWriteProtected;
        if (ImGui::Checkbox("Open Disk Images Write-Protected##owp", &openProtected))
            m_userPrefs->SetOpenImagesWriteProtected(openProtected);
        HelpMarker("Every disk image opens write-protected, so neither the application "
                   "nor the emulated Amiga can change it by accident.\n\n"
                   "The padlock in a document window, on a drive chip, or in the "
                   "emulator's Drives menu unlocks one image at a time.\n\n"
                   "Unlike \"Write Protection\" below, this is per image "
                   "rather than all drives at once.");
    }

    Checkbox("Write Protection##fwp", "puae_floppy_write_protection");
    HelpMarker("Make all floppy drives read-only. Prevents writes to ADF images.");

    Checkbox("Write Redirect##fwr", "puae_floppy_write_redirect");
    HelpMarker("When DISABLED (default), writes flow directly into the in-memory ADF buffer. The\n"
               "document is marked dirty (\xe2\x97\x8f in title bar) and can be saved via File "
               "\xe2\x86\x92 Save.\n"
               "This is what captures changes the Amiga makes to its own disk (high scores,\n"
               "install routines, save files, etc.), so leave it off unless you want the "
               "opposite.\n"
               "When enabled, floppy writes are redirected to a separate save file and the\n"
               "document never sees them.");
    ResetMarker();

    Checkbox("Multi-Drive Mode##fmd", "puae_floppy_multidrive");
    HelpMarker("When loading multiple disk images, insert each into a separate drive slot\n"
               "(up to DF0:\u2013DF3:). Useful for multi-disk games without manual swapping.");

    ImGui::Spacing();
    ImGui::TextDisabled("Drive speed changes apply on the next disk access.");

    ImGui::SeparatorText("CD Drive (CD32, CDTV, or a CD on any machine)");

    Combo("CD Speed##cdspd", "puae_cd_speed");
    HelpMarker("Real: the CD32 reads at double speed and the CDTV at single speed, with seek\n"
               "delays. Turbo drops the seek delays.");

    Combo("CD Audio Volume##cdvol", "puae_sound_volume_cd");
    HelpMarker("Level of the CD's audio tracks, mixed with Paula.");

    Checkbox("Insert the Disc After Power-On##cddelay", "puae_cd_startup_delayed_insert");
    HelpMarker("Some titles refuse to start when the machine powers on with a disc already in\n"
               "the drive. This inserts it during the boot animation instead.");

    Checkbox("Shared NVRAM##cdnvram", "puae_shared_nvram");
    HelpMarker("Off: each disc keeps its own CD32/CDTV save memory. On: every disc shares one.\n"
               "Takes effect at the next power-on.");
}

} // namespace adm::ui
