#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace adm {
namespace core {

enum class IdentifyState {
    Idle,
    Hashing,
    SearchingByHash,
    SearchingByName,
    SearchingPouet,
    Done,
    Error
};

struct IdentifyResult
{
    std::string genre;
    std::string players;
    std::string rating; // out of 20
    std::string family;
    std::string system;
    std::string gameId; // ScreenScraper game ID for constructing links
    std::string errorMessage;
    bool found = false;
    bool screenScraperAttempted = false;
    bool pouetAttempted = false;

    // TOSEC Info
    bool isTosecKnown = false;
    std::string tosecTitle;
    std::string tosecRomName;
    std::string tosecCategory;
    std::string tosecVersion;
    std::string tosecDemo;
    std::string tosecYear;
    std::string tosecPublisher;
    std::string tosecReleaseGroup;
    std::string tosecLanguage;
    std::string tosecSystem;
    std::string tosecVideo;
    std::string tosecRegion;
    std::vector<std::string> tosecMedia;
    std::string tosecDownloadUrl;

    // Dump integrity
    bool tosecVerified = false;
    bool tosecAlternateDump = false;
    bool tosecBadDump = false;
    bool tosecOverdump = false;
    bool tosecVirusInfected = false;
    std::string tosecVirusName;
    bool tosecFixed = false;

    // Scene modifications
    bool tosecCracked = false;
    std::string tosecCrackGroup;
    bool tosecTrained = false;
    int tosecTrainerCount = 0;
    std::string tosecTrainerLanguage;
    std::string tosecTrainerGroup;
    bool tosecHacked = false;
    std::string tosecHackType;
    std::string tosecHackGroup;
    bool tosecModified = false;
    std::string tosecModType;
    std::string tosecModGroup;
    // Normalized title shared across all versions of a title (for cache grouping)
    std::string tosecTitleGroupId;

    std::string md5;
    std::string effectiveFilename; // Used for matching (e.g. inner ADF name for ZIPs)

    // Scraped Info
    std::string title;
    std::string developer;
    std::string publisher;
    std::string synopsis;
    std::string releaseDate;

    // Pouet Info
    bool hasPouetInfo = false;
    std::string pouetName;
    std::string pouetGroups;
    std::string pouetTypes;
    std::string pouetDate;
    std::string pouetDownloadUrl;
    std::string pouetProdId;
    std::string pouetGroupId;

    // Votes
    std::string pouetVoteUp;
    std::string pouetVotePig;
    std::string pouetVoteDown;
    std::string pouetVoteAvg;

    // Party Info
    std::string pouetPartyName;
    std::string pouetPartyYear;
    std::string pouetPartyCompo;
    std::string pouetPartyPlace;

    // Image data (downloaded raw bytes - decoded on UI thread)
    std::vector<uint8_t> screenshotData;
    std::vector<uint8_t> boxArtData;
    std::vector<uint8_t> wheelData;
    std::vector<uint8_t> developerLogoData;
    std::vector<uint8_t> publisherLogoData;
};

class DiskIdentifier
{
public:
    DiskIdentifier() = default;
    ~DiskIdentifier();

    DiskIdentifier(const DiskIdentifier&) = delete;
    DiskIdentifier& operator=(const DiskIdentifier&) = delete;

    // Hash these bytes instead of opening filePath. For a document with no file on
    // disk - a download mounted straight from memory - filePath is only a display
    // name, so every hash of it fails. Pass the bytes the document was built from;
    // pass nullptr (the default) and hashing reads the file as it always has.
    // Applies to every entry point below, including the per-source re-searches.
    void SetSourceBytes(std::shared_ptr<const std::vector<uint8_t>> bytes);

    // The two hashes identification runs on, honouring SetSourceBytes. Empty means the
    // disk could not be hashed at all, which is the state that used to end an identify
    // with "Failed to compute SHA1 hash". SHA1 covers the container as delivered; MD5
    // reaches inside a ZIP for the member, because that is what TOSEC is keyed on.
    std::string ComputeSHA1(const std::string& filePath);
    std::string ComputeMD5(const std::string& filePath,
                           std::string* outEffectiveFilename = nullptr);

    void StartIdentify(const std::string& filePath);
    void StartTosecOnly(const std::string& filePath);
    void StartPouetIdentify(const std::string& filePath, const std::string& title);
    void Reset();
    bool IsInCache(const std::string& filePath);
    IdentifyResult ComputeTosec(const std::string& filePath);
    bool TryLoadCacheBySHA1(const std::string& sha1, IdentifyResult& out);
    // Look up cache by the ADF content MD5 (works for both ZIP and raw ADF identifications)
    bool TryLoadCacheByMD5(const std::string& md5, IdentifyResult& out);
    // Look up cache by pre-computed title group ID (shared across all versions of a title)
    bool TryLoadCacheByTitleGroup(const std::string& titleGroupId, IdentifyResult& out);
    // If the current result has TOSEC data but missing SS/Pouet metadata, check the title-group
    // cache and merge any newly available data into the stored result. Returns true if updated.
    bool RefreshFromCache();
    IdentifyState GetState() const { return m_state.load(); }
    std::string GetStatusMessage() const;
    IdentifyResult GetResult() const;

private:
    void RunIdentify(std::string filePath);
    void RunTosecOnly(std::string filePath);
    void RunPouetIdentify(std::string filePath, std::string title);
    IdentifyResult SearchByHash(const std::string& sha1Hash, std::string& outRawJson);
    IdentifyResult SearchByName(const std::string& filename, std::string& outRawJson);
    void SearchPouet(const std::string& title, IdentifyResult& outResult);
    IdentifyResult ParseResponse(const std::string& jsonBody, bool downloadImages = true);
    std::vector<uint8_t> DownloadImage(const std::string& url);

    static std::string GetCacheDir();
    bool LoadFromCache(const std::string& sha1Hash, IdentifyResult& result);
    void SaveToCache(const std::string& sha1Hash, const IdentifyResult& result,
                     const std::string& rawJson);
    static std::vector<uint8_t> ReadBinaryFile(const std::string& path);
    static void WriteBinaryFile(const std::string& path, const std::vector<uint8_t>& data);

    // The document's own bytes, when it has no file on disk. Null = hash the path.
    std::shared_ptr<const std::vector<uint8_t>> SourceBytes() const;
    mutable std::mutex m_sourceMutex;
    std::shared_ptr<const std::vector<uint8_t>> m_sourceBytes;

    std::atomic<IdentifyState> m_state{IdentifyState::Idle};
    mutable std::mutex m_mutex;
    IdentifyResult m_result;
    std::string m_statusMessage;
    std::thread m_thread;

    // MD5 memo keyed by (path, size, mtime) so repeated identify/cache lookups for
    // the same unchanged file don't re-hash. A re-written file is detected via the
    // size/mtime change and recomputed.
    struct Md5CacheEntry
    {
        std::string md5;
        std::string effectiveFilename;
        std::uintmax_t size = 0;
        std::int64_t mtime = 0;
    };
    mutable std::mutex m_md5CacheMutex;
    std::unordered_map<std::string, Md5CacheEntry> m_md5Cache;
};

} // namespace core
} // namespace adm
