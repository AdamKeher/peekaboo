#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "LaunchOptions.h"

namespace adm::core {

// Named machine-state snapshots taken with AmigaEmulator::SaveState().
//
// The point is a repeatable starting condition: boot once, snapshot at the interesting
// moment, then restore that exact machine as many times as you like instead of re-booting
// and hoping the frame counts line up.
//
// Snapshots live in memory by default (fast enough for a tight save/restore loop) and are
// additionally written to disk when saved with persist=true, so a hard-won boot state
// survives an app restart. A snapshot is only valid for the same core build and the same
// mounted game, so both are recorded and checked on restore.
class EmulatorStateStore
{
public:
    struct StateInfo
    {
        std::string name;
        size_t bytes = 0;
        uint64_t frame = 0;
        bool persisted = false;
        std::string coreVersion;
        std::string document; // path of the document that was mounted when saved
        std::string created;  // "YYYY-MM-DD HH:MM:SS" local time
    };

    // `directory` is where persisted snapshots are written. Created on first use.
    explicit EmulatorStateStore(std::filesystem::path directory = ProfilePath("states"));

    // Store `data` under `name`. Overwrites an existing snapshot of the same name.
    // With persist, also writes <directory>/<name>.pkst. Returns false (and sets
    // outError) only on a disk failure — the in-memory copy is kept either way.
    bool Save(const std::string& name, std::vector<uint8_t> data, uint64_t frame,
              const std::string& coreVersion, const std::string& document, bool persist,
              std::string& outError);

    // Fetch a snapshot by name, loading it from disk if it is not in memory. Returns
    // nullopt if there is no such snapshot.
    std::optional<std::vector<uint8_t>> Load(const std::string& name, StateInfo& outInfo);

    // Metadata for a snapshot without reading its bytes.
    std::optional<StateInfo> Info(const std::string& name);

    // Every known snapshot, in-memory and on-disk, newest first.
    std::vector<StateInfo> List();

    // Forget a snapshot and delete its file. False if there was no such snapshot.
    bool Remove(const std::string& name);

    // Drop every in-memory snapshot (persisted files are left alone).
    void ClearMemory() { m_entries.clear(); }

    // True if the name is safe to use as a file name (letters, digits, '-', '_', '.').
    static bool IsValidName(const std::string& name);

    const std::filesystem::path& Directory() const { return m_directory; }

private:
    struct Entry
    {
        StateInfo info;
        std::vector<uint8_t> data;
        bool dataLoaded = false;
    };

    std::filesystem::path FilePath(const std::string& name) const;
    // Read a .pkst file into an Entry. False if missing or malformed.
    bool ReadFile(const std::string& name, Entry& out) const;
    // Fold any .pkst files on disk that are not already known into m_entries.
    void ScanDirectory();

    std::filesystem::path m_directory;
    std::map<std::string, Entry> m_entries;
    bool m_scanned = false;
};

} // namespace adm::core
