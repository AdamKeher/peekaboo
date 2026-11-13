#pragma once

#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <vector>

#include "../../core/SystemFiles.h"

namespace adm {
class AmigaEmulator;
}

namespace adm::ui {

// Finding the user's own Amiga system files and copying the ROMs into Peekaboo's ROM folder: the
// folders to look in, what was found, Copy. Preferences > System
// Files and the setup wizard each draw one.
class SystemFilesFinder
{
public:
    ~SystemFilesFinder();

    void SetEmulator(AmigaEmulator* emulator) { m_emulator = emulator; }
    // Where the ROMs are copied to: the ROM folder as it stands
    void SetRomDirectory(std::function<std::filesystem::path()> get)
    {
        m_romDirectory = std::move(get);
    }

    // Looks in every ticked folder, on a worker thread; the defaults are found on the first call.
    void Scan();
    bool IsScanning() const { return m_scanning.valid(); }
    bool HasScanned() const { return m_scanned; }
    const core::SystemFileScan& Result() const { return m_scan; }

    // Takes a finished scan's results. Draw does it; a host showing them elsewhere (the wizard's
    // Workbench page) calls it every frame too.
    void Poll();

    // Draws it all. True on the frame a copy put ROMs into the ROM folder.
    bool Draw();

    // The core's identification, loading the core first if nothing has (it builds no machine).
    // Empty when the core cannot identify ROMs.
    core::RomIdentifier Identifier();

private:
    struct Folder
    {
        std::filesystem::path path;
        std::string label; // "Amiga Forever"; empty for one the user added
        bool use = true;
    };

    void FindDefaultFolders();
    void DrawFolders();
    bool DrawRoms();

    AmigaEmulator* m_emulator = nullptr;
    std::function<std::filesystem::path()> m_romDirectory;
    std::vector<Folder> m_folders;
    bool m_foldersFound = false;

    std::future<core::SystemFileScan> m_scanning;
    core::SystemFileScan m_scan;
    bool m_scanned = false;
    std::vector<char> m_copy;        // per ROM found: ticked to copy
    std::vector<char> m_inRomFolder; // per ROM found: its preset name is in the ROM folder already
    std::vector<core::RomCopy> m_lastCopy;
    bool m_copied = false; // the last Copy has run (m_lastCopy is its answer)
};

} // namespace adm::ui
