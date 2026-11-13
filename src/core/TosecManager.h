#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace adm::core {

struct TosecEntry
{
    // Core ROM data
    std::string gameTitle;   // Parsed title from name
    std::string romName;     // From "roms[].name"
    std::string description; // From "description"
    std::string size;        // From "roms[].size"
    std::string crc;         // From "roms[].crc"
    std::string md5;         // From "roms[].md5"
    std::string sha1;        // From "roms[].sha1"

    // Classification
    std::string category;
    std::string downloadUrl;

    // TOSEC parsed metadata
    std::string version;
    std::string demo;
    std::string year;
    std::string publisher;
    std::string releaseGroup;
    std::string language;
    std::string system;
    std::string video;
    std::string region;
    std::vector<std::string> media;

    // Dump integrity flags
    bool verified = false;
    bool alternateDump = false;
    bool badDump = false;
    bool overdump = false;
    bool virusInfected = false;
    std::string virusName; // Virus name from [v NAME] TOSEC flag
    bool fixed = false;

    // Scene modification flags
    bool cracked = false;
    std::string crackGroup;

    bool trained = false;
    int trainerCount = 0;
    std::string trainerLanguage;
    std::string trainerGroup;

    bool hacked = false;
    std::string hackType;  // Functional change type (music, sfx, keyboard …)
    std::string hackGroup; // Scene group that made the hack

    bool modified = false;
    std::string modType;  // Modification descriptor (doscopy, bootblock …)
    std::string modGroup; // Scene group (if any)

    // Normalized title used as shared cache key across all versions of a title
    std::string titleGroupId;
};

// Parsed metadata from a filename based on TOSEC convention
// Convention: "Title (Year)(Publisher)(System)(Options...)[Status]..."
struct TosecFilenameInfo
{
    std::string title;
    std::string year;
    std::string publisher;
    std::vector<std::string> flags; // (System), [Status], etc.
    std::string category;
    bool valid = false;
};

class TosecManager
{
public:
    static TosecManager& Instance();

    void Initialize();
    // Joins the loader thread. Must be called before static destruction (Application::Shutdown)
    // — a detached loader still parsing during exit would race the destruction of m_mutex and
    // m_entries.
    void Shutdown();
    // Deliberately lock-free: this is polled from the UI thread every frame while the loader
    // thread is still running, so m_loaded has to be atomic rather than a plain bool written
    // under m_mutex and read without it.
    bool IsLoaded() const { return m_loaded.load(std::memory_order_acquire); }

    // Thread-safe lookups
    const TosecEntry* FindByMD5(const std::string& md5) const;
    const TosecEntry* FindBySHA1(const std::string& sha1) const;

    // Browser support – returns raw pointers into the internal map (valid for app lifetime)
    std::vector<const TosecEntry*> GetAllEntries() const;
    size_t GetEntryCount() const;

    // Utility to parse filename according to TOSEC rules
    static TosecFilenameInfo ParseFilename(const std::string& filename);

private:
    TosecManager() = default;
    ~TosecManager() { Shutdown(); }
    TosecManager(const TosecManager&) = delete;
    TosecManager& operator=(const TosecManager&) = delete;

    void LoadTosecJson();

    std::atomic<bool> m_loaded{false};
    std::thread m_loaderThread;
    mutable std::mutex m_mutex;
    // Entries are owned once here (deque keeps pointers stable across growth);
    // the lookup maps index into them by hash rather than storing copies.
    std::deque<TosecEntry> m_entries;
    std::unordered_map<std::string, const TosecEntry*> m_md5Map;
    std::unordered_map<std::string, const TosecEntry*> m_sha1Map;
};

} // namespace adm::core
