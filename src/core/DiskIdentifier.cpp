#include "DiskIdentifier.h"

#include <cpr/cpr.h>
#include <fmt/format.h>
#include <TinySHA1.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "Logger.h"
#include "MD5.h"
#include "Secrets.h"
#include "TosecManager.h"
#include "ZipUtils.h"

namespace adm {
namespace core {

// The expanded request URL carries the screenscraper credentials as query parameters; strip
// their values before logging — log lines end up in screenshots and pasted bug reports.
static std::string RedactCredentials(std::string url)
{
    for (const char* key : {"devid=", "devpassword="}) {
        const size_t pos = url.find(key);
        if (pos == std::string::npos)
            continue;
        const size_t valueStart = pos + std::strlen(key);
        size_t valueEnd = url.find('&', valueStart);
        if (valueEnd == std::string::npos)
            valueEnd = url.size();
        url.replace(valueStart, valueEnd - valueStart, "<redacted>");
    }
    return url;
}

// Helper to find a localized text value from a JSON array of {langue, text} objects.
// Prefers "en", falls back to first available.
static std::string FindLocalizedText(const nlohmann::json& arr, const std::string& lang = "en")
{
    if (!arr.is_array() || arr.empty())
        return {};

    for (const auto& item : arr) {
        if (item.contains("langue") && item["langue"].get<std::string>() == lang &&
            item.contains("text")) {
            return item["text"].get<std::string>();
        }
    }
    // Fallback: return first entry with text
    if (arr[0].contains("text")) {
        return arr[0]["text"].get<std::string>();
    }
    return {};
}

// Copy all TOSEC fields from a TosecEntry into a result. No-op if entry is null.
static void ApplyTosecEntry(IdentifyResult& result, const TosecEntry* entry)
{
    if (!entry)
        return;
    result.isTosecKnown = true;
    result.tosecTitle = entry->gameTitle;
    result.tosecRomName = entry->romName;
    result.tosecCategory = entry->category;
    result.tosecDownloadUrl = entry->downloadUrl;

    result.tosecVersion = entry->version;
    result.tosecDemo = entry->demo;
    result.tosecYear = entry->year;
    result.tosecPublisher = entry->publisher;
    result.tosecReleaseGroup = entry->releaseGroup;
    result.tosecLanguage = entry->language;
    result.tosecSystem = entry->system;
    result.tosecVideo = entry->video;
    result.tosecRegion = entry->region;
    result.tosecMedia = entry->media;

    result.tosecVerified = entry->verified;
    result.tosecAlternateDump = entry->alternateDump;
    result.tosecBadDump = entry->badDump;
    result.tosecOverdump = entry->overdump;
    result.tosecVirusInfected = entry->virusInfected;
    result.tosecVirusName = entry->virusName;
    result.tosecFixed = entry->fixed;

    result.tosecCracked = entry->cracked;
    result.tosecCrackGroup = entry->crackGroup;
    result.tosecTrained = entry->trained;
    result.tosecTrainerCount = entry->trainerCount;
    result.tosecTrainerLanguage = entry->trainerLanguage;
    result.tosecTrainerGroup = entry->trainerGroup;
    result.tosecHacked = entry->hacked;
    result.tosecHackType = entry->hackType;
    result.tosecHackGroup = entry->hackGroup;
    result.tosecModified = entry->modified;
    result.tosecModType = entry->modType;
    result.tosecModGroup = entry->modGroup;
    result.tosecTitleGroupId = entry->titleGroupId;
}

// Copy the TOSEC metadata fields from one result to another (src is not consumed).
static void CopyTosecFields(IdentifyResult& dst, const IdentifyResult& src)
{
    dst.isTosecKnown = src.isTosecKnown;
    dst.tosecTitle = src.tosecTitle;
    dst.tosecRomName = src.tosecRomName;
    dst.tosecCategory = src.tosecCategory;
    dst.tosecDownloadUrl = src.tosecDownloadUrl;
    dst.tosecVersion = src.tosecVersion;
    dst.tosecDemo = src.tosecDemo;
    dst.tosecYear = src.tosecYear;
    dst.tosecPublisher = src.tosecPublisher;
    dst.tosecReleaseGroup = src.tosecReleaseGroup;
    dst.tosecLanguage = src.tosecLanguage;
    dst.tosecSystem = src.tosecSystem;
    dst.tosecVideo = src.tosecVideo;
    dst.tosecRegion = src.tosecRegion;
    dst.tosecMedia = src.tosecMedia;
    dst.tosecVerified = src.tosecVerified;
    dst.tosecAlternateDump = src.tosecAlternateDump;
    dst.tosecBadDump = src.tosecBadDump;
    dst.tosecOverdump = src.tosecOverdump;
    dst.tosecVirusInfected = src.tosecVirusInfected;
    dst.tosecVirusName = src.tosecVirusName;
    dst.tosecFixed = src.tosecFixed;
    dst.tosecCracked = src.tosecCracked;
    dst.tosecCrackGroup = src.tosecCrackGroup;
    dst.tosecTrained = src.tosecTrained;
    dst.tosecTrainerCount = src.tosecTrainerCount;
    dst.tosecTrainerLanguage = src.tosecTrainerLanguage;
    dst.tosecTrainerGroup = src.tosecTrainerGroup;
    dst.tosecHacked = src.tosecHacked;
    dst.tosecHackType = src.tosecHackType;
    dst.tosecHackGroup = src.tosecHackGroup;
    dst.tosecModified = src.tosecModified;
    dst.tosecModType = src.tosecModType;
    dst.tosecModGroup = src.tosecModGroup;
    dst.tosecTitleGroupId = src.tosecTitleGroupId;
}

// Move ScreenScraper metadata (incl. image buffers) from a discarded source into dst.
static void MoveScreenScraperFields(IdentifyResult& dst, IdentifyResult& src)
{
    dst.found = src.found;
    dst.screenScraperAttempted = src.screenScraperAttempted;
    dst.title = std::move(src.title);
    dst.developer = std::move(src.developer);
    dst.publisher = std::move(src.publisher);
    dst.synopsis = std::move(src.synopsis);
    dst.releaseDate = std::move(src.releaseDate);
    dst.genre = std::move(src.genre);
    dst.players = std::move(src.players);
    dst.rating = std::move(src.rating);
    dst.family = std::move(src.family);
    dst.system = std::move(src.system);
    dst.gameId = std::move(src.gameId);
    dst.screenshotData = std::move(src.screenshotData);
    dst.boxArtData = std::move(src.boxArtData);
    dst.wheelData = std::move(src.wheelData);
    dst.developerLogoData = std::move(src.developerLogoData);
    dst.publisherLogoData = std::move(src.publisherLogoData);
}

// Move Pouet metadata from a discarded source into dst. Does not touch
// pouetAttempted — callers manage that flag explicitly.
static void MovePouetFields(IdentifyResult& dst, IdentifyResult& src)
{
    dst.hasPouetInfo = true;
    dst.pouetName = std::move(src.pouetName);
    dst.pouetProdId = std::move(src.pouetProdId);
    dst.pouetGroupId = std::move(src.pouetGroupId);
    dst.pouetDownloadUrl = std::move(src.pouetDownloadUrl);
    dst.pouetDate = std::move(src.pouetDate);
    dst.pouetGroups = std::move(src.pouetGroups);
    dst.pouetTypes = std::move(src.pouetTypes);
    dst.pouetVoteUp = std::move(src.pouetVoteUp);
    dst.pouetVotePig = std::move(src.pouetVotePig);
    dst.pouetVoteDown = std::move(src.pouetVoteDown);
    dst.pouetVoteAvg = std::move(src.pouetVoteAvg);
    dst.pouetPartyName = std::move(src.pouetPartyName);
    dst.pouetPartyYear = std::move(src.pouetPartyYear);
    dst.pouetPartyCompo = std::move(src.pouetPartyCompo);
    dst.pouetPartyPlace = std::move(src.pouetPartyPlace);
}

DiskIdentifier::~DiskIdentifier()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void DiskIdentifier::Reset()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_result = {};
    m_statusMessage.clear();
    m_state.store(IdentifyState::Idle);
}

void DiskIdentifier::StartIdentify(const std::string& filePath)
{
    // Wait for any previous thread to finish
    if (m_thread.joinable()) {
        m_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result = {};
        m_statusMessage = "Starting identification...";
    }
    m_state.store(IdentifyState::Hashing);

    m_thread = std::thread(&DiskIdentifier::RunIdentify, this, filePath);
}

void DiskIdentifier::StartPouetIdentify(const std::string& filePath, const std::string& title)
{
    // Wait for any previous thread to finish
    if (m_thread.joinable()) {
        m_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_statusMessage = "Starting Pouet identification...";
    }
    m_state.store(IdentifyState::SearchingPouet);

    m_thread = std::thread(&DiskIdentifier::RunPouetIdentify, this, filePath, title);
}

void DiskIdentifier::StartTosecOnly(const std::string& filePath)
{
    if (m_thread.joinable()) {
        m_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result = {};
        m_statusMessage = "Computing MD5...";
    }
    m_state.store(IdentifyState::Hashing);

    m_thread = std::thread(&DiskIdentifier::RunTosecOnly, this, filePath);
}

void DiskIdentifier::RunTosecOnly(std::string filePath)
{
    std::string filename = std::filesystem::path(filePath).filename().string();
    Logger::Info("Identify: Computing TOSEC lookup for '{}'", filename);

    std::string effectiveFilename;
    std::string md5Hash = ComputeMD5(filePath, &effectiveFilename);
    Logger::Info("Identify: MD5={}", md5Hash);

    // Wait for TOSEC database to finish loading (it loads on a background thread at startup)
    while (!TosecManager::Instance().IsLoaded()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const TosecEntry* tosecEntry = nullptr;
    if (!md5Hash.empty()) {
        tosecEntry = TosecManager::Instance().FindByMD5(md5Hash);
        if (tosecEntry) {
            Logger::Info("Identify: Found in TOSEC: {}", tosecEntry->romName);
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_result.md5 = md5Hash;
    m_result.effectiveFilename = effectiveFilename;
    ApplyTosecEntry(m_result, tosecEntry);
    m_statusMessage = tosecEntry ? "TOSEC match found" : "Not in TOSEC database";
    m_state.store(IdentifyState::Done);
}

IdentifyResult DiskIdentifier::ComputeTosec(const std::string& filePath)
{
    IdentifyResult result;
    std::string effectiveFilename;
    result.md5 = ComputeMD5(filePath, &effectiveFilename);
    result.effectiveFilename = effectiveFilename;

    if (!result.md5.empty()) {
        const TosecEntry* tosecEntry = TosecManager::Instance().FindByMD5(result.md5);
        ApplyTosecEntry(result, tosecEntry);
    }
    return result;
}

bool DiskIdentifier::TryLoadCacheBySHA1(const std::string& sha1, IdentifyResult& out)
{
    return LoadFromCache(sha1, out);
}

bool DiskIdentifier::TryLoadCacheByTitleGroup(const std::string& titleGroupId, IdentifyResult& out)
{
    if (titleGroupId.empty())
        return false;
    return LoadFromCache(titleGroupId, out);
}

bool DiskIdentifier::RefreshFromCache()
{
    // Snapshot current result without holding the lock during file I/O
    IdentifyResult snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_result;
    }

    // Only useful if TOSEC is identified but SS/Pouet data is absent
    if (!snapshot.isTosecKnown || snapshot.tosecTitleGroupId.empty())
        return false;
    if (snapshot.found && snapshot.hasPouetInfo)
        return false;

    IdentifyResult cached;
    if (!LoadFromCache(snapshot.tosecTitleGroupId, cached))
        return false;
    if (!cached.found && !cached.hasPouetInfo)
        return false;

    // Merge newly available SS/Pouet data into the stored result (preserve all TOSEC fields)
    std::lock_guard<std::mutex> lock(m_mutex);
    if (cached.found && !m_result.found)
        MoveScreenScraperFields(m_result, cached);
    if (cached.hasPouetInfo && !m_result.hasPouetInfo) {
        MovePouetFields(m_result, cached);
        m_result.pouetAttempted = cached.pouetAttempted;
    }
    return true;
}

bool DiskIdentifier::IsInCache(const std::string& filePath)
{
    auto dir = GetCacheDir();
    if (dir.empty())
        return false;

    // Fast path: check titleGroupId-based cache via MD5+TOSEC (no SHA1 needed)
    std::string md5Hash = ComputeMD5(filePath);
    if (!md5Hash.empty() && TosecManager::Instance().IsLoaded()) {
        const TosecEntry* tosecEntry = TosecManager::Instance().FindByMD5(md5Hash);
        if (tosecEntry && !tosecEntry->titleGroupId.empty()) {
            auto base = std::filesystem::path(dir) / tosecEntry->titleGroupId;
            return std::filesystem::exists(base.string() + "_response.json") ||
                   std::filesystem::exists(base.string() + "_pouet.json");
        }
    }

    // Fallback: SHA1-based cache (for non-TOSEC files)
    std::string sha1Hash = ComputeSHA1(filePath);
    if (sha1Hash.empty())
        return false;

    auto base = std::filesystem::path(dir) / sha1Hash;
    return std::filesystem::exists(base.string() + "_response.json") ||
           std::filesystem::exists(base.string() + "_pouet.json");
}

std::string DiskIdentifier::GetStatusMessage() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_statusMessage;
}

IdentifyResult DiskIdentifier::GetResult() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_result;
}

void DiskIdentifier::RunIdentify(std::string filePath)
{
    std::string filename = std::filesystem::path(filePath).filename().string();
    Logger::Info("Identify: Starting identification for '{}'", filename);

    // Step 1: Compute MD5 and check TOSEC (fast, gives immediate feedback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_statusMessage = "Checking TOSEC database...";
    }
    std::string effectiveFilename;
    std::string md5Hash = ComputeMD5(filePath, &effectiveFilename);
    Logger::Info("Identify: MD5={}", md5Hash);

    const TosecEntry* tosecEntry = TosecManager::Instance().FindByMD5(md5Hash);
    if (tosecEntry) {
        Logger::Info("Identify: Found in TOSEC: {}", tosecEntry->romName);
    }

    // Publish MD5 and TOSEC results immediately so UI shows progress
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result.md5 = md5Hash;
        m_result.effectiveFilename = effectiveFilename;
        ApplyTosecEntry(m_result, tosecEntry);
        if (tosecEntry) {
            m_result.found = true;
            if (m_result.title.empty())
                m_result.title = tosecEntry->gameTitle;
        }
    }

    // Determine primary cache key: titleGroupId for TOSEC entries, otherwise SHA1
    // Using titleGroupId means all versions of a title share one cached metadata entry.
    const std::string titleGroupId =
        (tosecEntry && !tosecEntry->titleGroupId.empty()) ? tosecEntry->titleGroupId : "";
    bool useTitleCache = !titleGroupId.empty();

    // Step 2: Check title-group cache (fast — no SHA1 needed on hit)
    if (useTitleCache) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_statusMessage = "Checking local cache...";
        }
        Logger::Info("Identify: Checking title-group cache (group: '{}')...", titleGroupId);
        IdentifyResult cached;
        if (LoadFromCache(titleGroupId, cached)) {
            Logger::Info("Identify: Cache hit for '{}'", filename);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_result = std::move(cached);
            m_result.md5 = md5Hash;
            m_result.effectiveFilename = effectiveFilename;
            ApplyTosecEntry(m_result, tosecEntry);
            m_statusMessage = "Loaded from cache";
            m_state.store(IdentifyState::Done);
            return;
        }
    }

    // Step 3: Compute SHA1 (needed for ScreenScraper lookup and SHA1-based cache fallback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_statusMessage = "Computing SHA1 hash...";
    }
    Logger::Info("Identify: Computing SHA1 hash...");

    std::string sha1Hash = ComputeSHA1(filePath);
    if (sha1Hash.empty()) {
        Logger::Error("Identify: Failed to compute SHA1 hash for '{}'", filename);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result.errorMessage = "Failed to compute SHA1 hash";
        m_statusMessage = "Error: Failed to compute SHA1 hash";
        m_state.store(IdentifyState::Error);
        return;
    }

    Logger::Info("Identify: SHA1={}", sha1Hash);

    // For non-TOSEC entries, check the SHA1-based cache
    if (!useTitleCache) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_statusMessage = "Checking local cache...";
        }
        Logger::Info("Identify: Checking SHA1 cache...");
        IdentifyResult cached;
        if (LoadFromCache(sha1Hash, cached)) {
            Logger::Info("Identify: Cache hit - '{}' identified as '{}'", filename,
                         cached.found ? cached.title
                                      : (cached.hasPouetInfo ? cached.pouetName : "(not found)"));
            std::lock_guard<std::mutex> lock(m_mutex);
            m_result = std::move(cached);
            m_result.md5 = md5Hash;
            m_result.effectiveFilename = effectiveFilename;
            m_statusMessage = "Loaded from cache";
            m_state.store(IdentifyState::Done);
            return;
        }
    }

    const std::string& cacheKey = useTitleCache ? titleGroupId : sha1Hash;

    // Step 4: Search ScreenScraper by SHA1
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_statusMessage = "Searching by SHA1 hash...";
    }
    m_state.store(IdentifyState::SearchingByHash);
    Logger::Info("Identify: Searching ScreenScraper by SHA1...");

    std::string rawJson;
    auto result = SearchByHash(sha1Hash, rawJson);
    if (result.found) {
        Logger::Info("Identify: Match found by SHA1 - '{}' identified as '{}'", filename,
                     result.title);
        result.md5 = md5Hash;
        SaveToCache(cacheKey, result, rawJson);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result = std::move(result);
        m_result.screenScraperAttempted = true;
        m_result.md5 = md5Hash;
        m_result.effectiveFilename = effectiveFilename;
        ApplyTosecEntry(m_result, tosecEntry);
        m_statusMessage = "Identification complete";
        m_state.store(IdentifyState::Done);
        return;
    }

    // Step 5: Fallback — search by filename
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_statusMessage = fmt::format("Searching by filename '{}'...", filename);
    }
    m_state.store(IdentifyState::SearchingByName);
    Logger::Info("Identify: No SHA1 match, searching by filename '{}'...", filename);

    rawJson.clear();
    result = SearchByName(filename, rawJson);
    if (result.found) {
        Logger::Info("Identify: Match found by filename - '{}' identified as '{}'", filename,
                     result.title);
        result.md5 = md5Hash;
        SaveToCache(cacheKey, result, rawJson);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result = std::move(result);
        m_result.screenScraperAttempted = true;
        m_result.md5 = md5Hash;
        m_result.effectiveFilename = effectiveFilename;
        ApplyTosecEntry(m_result, tosecEntry);
        m_statusMessage = "Identification complete (by filename)";
        m_state.store(IdentifyState::Done);
        return;
    }

    // No match found
    if (!result.errorMessage.empty()) {
        Logger::Error("Identify: Error identifying '{}': {}", filename, result.errorMessage);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result = result;
        m_result.screenScraperAttempted = true;
        m_statusMessage = "Error: " + result.errorMessage;
        m_state.store(IdentifyState::Error);
    } else {
        Logger::Info("Identify: No match found in database for '{}'", filename);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result.errorMessage = "No match found";
        m_result.screenScraperAttempted = true;
        m_result.md5 = md5Hash;
        m_result.effectiveFilename = effectiveFilename;
        // TOSEC data is still valid even when ScreenScraper has no match
        ApplyTosecEntry(m_result, tosecEntry);
        if (tosecEntry) {
            m_result.found = true;
            m_result.title = tosecEntry->gameTitle;
        }
        m_statusMessage = "No match found";
        m_state.store(IdentifyState::Done);
    }
}

void DiskIdentifier::RunPouetIdentify(std::string filePath, std::string title)
{
    Logger::Info("Identify: Starting Pouet identification for '{}'", title);

    auto tosecInfo = ComputeTosec(filePath);

    // Determine cache key: titleGroupId for TOSEC entries, otherwise SHA1
    std::string cacheKey = tosecInfo.tosecTitleGroupId;
    if (cacheKey.empty()) {
        cacheKey = ComputeSHA1(filePath);
        if (cacheKey.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_result.errorMessage = "Failed to compute SHA1 hash for cache";
            m_statusMessage = "Error: Failed to compute SHA1 hash";
            m_state.store(IdentifyState::Error);
            return;
        }
    }

    // Load cache before acquiring the mutex to avoid file I/O under lock
    IdentifyResult cached;
    bool hasCached = LoadFromCache(cacheKey, cached);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result.md5 = tosecInfo.md5;
        m_result.effectiveFilename = tosecInfo.effectiveFilename;

        if (tosecInfo.isTosecKnown) {
            CopyTosecFields(m_result, tosecInfo);
            if (m_result.title.empty())
                m_result.title = tosecInfo.tosecTitle;
        }

        if (hasCached) {
            // Merge cached ScreenScraper data, preserving current MD5/TOSEC fields
            MoveScreenScraperFields(m_result, cached);
            m_result.screenScraperAttempted = true;
            if (cached.hasPouetInfo)
                MovePouetFields(m_result, cached);
        }
        m_statusMessage = "Searching Pouet database...";
    }

    IdentifyResult pouetResult;
    SearchPouet(title, pouetResult);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result.pouetAttempted = true;
        if (pouetResult.hasPouetInfo) {
            MovePouetFields(m_result, pouetResult);
            m_statusMessage = "Pouet Identification complete";
            SaveToCache(cacheKey, m_result, {});
        } else {
            m_statusMessage = "No match found on Pouet";
        }
    }

    m_state.store(IdentifyState::Done);
}

void DiskIdentifier::SearchPouet(const std::string& title, IdentifyResult& outResult)
{
    auto response = cpr::Get(cpr::Url{"https://api.pouet.net/v1/search/prod/"},
                             cpr::Parameters{{"q", title}}, cpr::Timeout{15000});

    Logger::Info("DiskIdentifier: GET {}", RedactCredentials(response.url.str()));

    if (response.error || response.status_code != 200) {
        Logger::Error("DiskIdentifier: Pouet API error: {}",
                      response.error ? response.error.message
                                     : std::to_string(response.status_code));
        return;
    }

    try {
        auto json = nlohmann::json::parse(response.text);
        if (!json.contains("success") || !json["success"].get<bool>() ||
            !json.contains("results")) {
            return;
        }

        const auto& results = json["results"];
        if (!results.is_object()) {
            return;
        }

        auto safeGetStr = [](const nlohmann::json& j, const std::string& key) -> std::string {
            if (!j.contains(key) || j[key].is_null())
                return "";
            if (j[key].is_string())
                return j[key].get<std::string>();
            if (j[key].is_number_integer())
                return std::to_string(j[key].get<int>());
            if (j[key].is_number_float())
                return std::to_string(j[key].get<double>());
            return "";
        };

        auto simplifyStr = [](const std::string& str) {
            std::string res;
            for (char c : str) {
                if (std::isalnum(static_cast<unsigned char>(c))) {
                    res += std::tolower(static_cast<unsigned char>(c));
                }
            }
            return res;
        };

        std::string bestId;
        const nlohmann::json* bestItem = nullptr;
        std::string simpleTitle = simplifyStr(title);

        for (auto it = results.begin(); it != results.end(); ++it) {
            std::string itemId = it.key();
            const auto& item = it.value();

            std::string itemName = safeGetStr(item, "name");
            std::string simpleName = simplifyStr(itemName);

            // Exact or substring match on simplified name
            if (simpleName.find(simpleTitle) != std::string::npos ||
                simpleTitle.find(simpleName) != std::string::npos) {
                // Must have Amiga platform
                if (item.contains("platforms") && item["platforms"].is_object()) {
                    bool isAmiga = false;
                    for (auto pIt = item["platforms"].begin(); pIt != item["platforms"].end();
                         ++pIt) {
                        std::string platName = pIt.value().value("name", "");
                        std::string lowerPlat = platName;
                        std::transform(
                            lowerPlat.begin(), lowerPlat.end(), lowerPlat.begin(),
                            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        if (lowerPlat.find("amiga") != std::string::npos) {
                            isAmiga = true;
                            break;
                        }
                    }

                    if (isAmiga) {
                        bestId = itemId;
                        bestItem = &item;
                        // Perfect name match?
                        if (simpleName == simpleTitle) {
                            break; // Stop looking, perfect match
                        }
                    }
                }
            }
        }

        if (bestItem != nullptr) {
            outResult.hasPouetInfo = true;
            outResult.pouetName = safeGetStr(*bestItem, "name");
            outResult.pouetProdId = bestId;
            outResult.pouetDownloadUrl = "https://www.pouet.net/prod.php?which=" + bestId;
            outResult.pouetDate = safeGetStr(*bestItem, "releaseDate");

            outResult.pouetVoteUp = safeGetStr(*bestItem, "voteup");
            outResult.pouetVotePig = safeGetStr(*bestItem, "votepig");
            outResult.pouetVoteDown = safeGetStr(*bestItem, "votedown");
            outResult.pouetVoteAvg = safeGetStr(*bestItem, "voteavg");

            // Groups
            if (bestItem->contains("groups") && (*bestItem)["groups"].is_array()) {
                std::string groupsStr;
                for (const auto& g : (*bestItem)["groups"]) {
                    if (!groupsStr.empty())
                        groupsStr += ", ";
                    groupsStr += safeGetStr(g, "name");
                    if (outResult.pouetGroupId.empty() && g.contains("id")) {
                        outResult.pouetGroupId = safeGetStr(g, "id");
                    }
                }
                outResult.pouetGroups = groupsStr;
            }

            // Party Info
            outResult.pouetPartyName = safeGetStr(*bestItem, "party_name"); // sometimes null
            if (outResult.pouetPartyName.empty() && bestItem->contains("party") &&
                (*bestItem)["party"].is_object()) {
                outResult.pouetPartyName = safeGetStr((*bestItem)["party"], "name");
            }
            outResult.pouetPartyYear = safeGetStr(*bestItem, "party_year");
            outResult.pouetPartyCompo = safeGetStr(*bestItem, "party_compo");
            outResult.pouetPartyPlace = safeGetStr(*bestItem, "party_place");

            // Types
            if (bestItem->contains("types") && (*bestItem)["types"].is_array()) {
                std::string typesStr;
                for (const auto& t : (*bestItem)["types"]) {
                    std::string typeName;
                    if (t.is_string())
                        typeName = t.get<std::string>();
                    else if (t.is_object() && t.contains("name") && t["name"].is_string())
                        typeName = t["name"].get<std::string>();
                    if (!typeName.empty()) {
                        if (!typesStr.empty())
                            typesStr += ", ";
                        typesStr += typeName;
                    }
                }
                outResult.pouetTypes = typesStr;
            }

            Logger::Info("DiskIdentifier: Pouet match found: {} (ID: {})", outResult.pouetName,
                         bestId);
        } else {
            Logger::Info("DiskIdentifier: No obvious Amiga match found on Pouet for '{}'", title);
        }

    } catch (const std::exception& e) {
        Logger::Error("DiskIdentifier: Pouet JSON parse error: {}", e.what());
    }
}

void DiskIdentifier::SetSourceBytes(std::shared_ptr<const std::vector<uint8_t>> bytes)
{
    std::lock_guard<std::mutex> lock(m_sourceMutex);
    m_sourceBytes = std::move(bytes);
}

std::shared_ptr<const std::vector<uint8_t>> DiskIdentifier::SourceBytes() const
{
    std::lock_guard<std::mutex> lock(m_sourceMutex);
    return m_sourceBytes;
}

std::string DiskIdentifier::ComputeSHA1(const std::string& filePath)
{
    sha1::SHA1 hasher;

    // Like the on-disk route, this hashes the container as it was delivered - a ZIP
    // is hashed whole. Only the MD5 below reaches inside, because that is the one
    // TOSEC is keyed on.
    if (auto bytes = SourceBytes()) {
        if (bytes->empty()) {
            Logger::Error("DiskIdentifier: No bytes to hash for '{}'", filePath);
            return {};
        }
        hasher.processBytes(bytes->data(), bytes->size());
    } else {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            Logger::Error("DiskIdentifier: Cannot open file for hashing: {}", filePath);
            return {};
        }

        char buffer[8192];
        while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
            hasher.processBytes(buffer, static_cast<size_t>(file.gcount()));
            if (file.eof())
                break;
        }
    }

    sha1::SHA1::digest8_t digest;
    hasher.getDigestBytes(digest);

    // Convert to lowercase hex string
    std::string result;
    result.reserve(40);
    for (int i = 0; i < 20; ++i) {
        result += fmt::format("{:02x}", digest[i]);
    }
    return result;
}

std::string DiskIdentifier::ComputeMD5(const std::string& filePath,
                                       std::string* outEffectiveFilename)
{
    // In-memory document: hash what it was built from. The size/mtime memo below is
    // keyed on a real file, so it is skipped - these bytes never change anyway.
    if (auto bytes = SourceBytes()) {
        std::string effName = std::filesystem::path(filePath).filename().string();
        if (bytes->empty()) {
            if (outEffectiveFilename)
                *outEffectiveFilename = effName;
            return {};
        }

        // Same rule as the on-disk route: TOSEC hashed the file inside the archive,
        // not the archive, so a ZIP is identified by the member it holds.
        const std::vector<uint8_t>* payload = bytes.get();
        std::vector<uint8_t> member;
        if (bytes->size() > 3 && (*bytes)[0] == 'P' && (*bytes)[1] == 'K' && (*bytes)[2] == 0x03 &&
            (*bytes)[3] == 0x04) {
            std::string entryName, error;
            if (ExtractFromZipBuffer(*bytes, ZipPayload::AnyDiskImage, member, entryName, error)) {
                Logger::Info("DiskIdentifier: Computed MD5 from ZIP entry '{}'", entryName);
                payload = &member;
                effName = entryName;
            } else {
                // Fall through and hash the archive; it will not match TOSEC, but a
                // wrong hash is more useful to report than none.
                Logger::Error("DiskIdentifier: Failed to extract from ZIP for MD5: {}", error);
            }
        }

        if (outEffectiveFilename)
            *outEffectiveFilename = effName;
        return MD5::Compute(*payload);
    }

    // Fast path: return a memoized hash if the file is unchanged (same size + mtime).
    std::error_code ec;
    std::uintmax_t fsize = std::filesystem::file_size(filePath, ec);
    std::int64_t fmtime = 0;
    if (!ec) {
        std::error_code ec2;
        auto t = std::filesystem::last_write_time(filePath, ec2);
        if (!ec2)
            fmtime = t.time_since_epoch().count();
    }
    if (!ec) {
        std::lock_guard<std::mutex> lock(m_md5CacheMutex);
        auto it = m_md5Cache.find(filePath);
        if (it != m_md5Cache.end() && it->second.size == fsize && it->second.mtime == fmtime) {
            if (outEffectiveFilename)
                *outEffectiveFilename = it->second.effectiveFilename;
            return it->second.md5;
        }
    }

    std::string md5;
    std::string effName;

    // Check for ZIP extension
    std::string ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (ext == ".zip") {
        std::vector<uint8_t> zipData;
        std::string entryName;
        std::string error;
        // adm::ExtractFromZip (found in parent namespace). TOSEC hashes whatever the
        // archive holds — an IPF or a HUNK executable just as often as an ADF — so
        // take any member, not only ".adf", or those titles can never match.
        if (ExtractFromZip(filePath, ZipPayload::AnyDiskImage, zipData, entryName, error)) {
            Logger::Info("DiskIdentifier: Computed MD5 from ZIP entry '{}'", entryName);
            effName = entryName;
            md5 = MD5::Compute(zipData);
        } else {
            Logger::Error("DiskIdentifier: Failed to extract from ZIP for MD5: {}", error);
            // Fallback: with nothing extracted we can't identify it via TOSEC.
            effName = std::filesystem::path(filePath).filename().string();
            md5 = "";
        }
    } else {
        effName = std::filesystem::path(filePath).filename().string();
        md5 = MD5::ComputeFile(filePath);
    }

    if (outEffectiveFilename)
        *outEffectiveFilename = effName;

    // Only memoize successful hashes for files we could stat.
    if (!ec && !md5.empty()) {
        std::lock_guard<std::mutex> lock(m_md5CacheMutex);
        m_md5Cache[filePath] = {md5, effName, fsize, fmtime};
    }
    return md5;
}

// Empty when building from src/Secrets.h.example (fresh clone, no machine-local Secrets.h) —
// the screenscraper lookups are skipped rather than sent with blank credentials.
static bool HasScreenscraperCredentials()
{
    static bool warned = false;
    const bool have = SS_DEVID[0] != '\0' && SS_DEVPASS[0] != '\0';
    if (!have && !warned) {
        warned = true;
        Logger::Warn("DiskIdentifier: no screenscraper credentials configured "
                     "(src/Secrets.h) — online identification disabled");
    }
    return have;
}

IdentifyResult DiskIdentifier::SearchByHash(const std::string& sha1Hash, std::string& outRawJson)
{
    if (!HasScreenscraperCredentials())
        return {};
    auto response = cpr::Get(cpr::Url{"https://www.screenscraper.fr/api2/jeuInfos.php"},
                             cpr::Parameters{{"devid", SS_DEVID},
                                             {"devpassword", SS_DEVPASS},
                                             {"softname", SS_SOFTNAME},
                                             {"output", "json"},
                                             {"sha1", sha1Hash}},
                             cpr::Timeout{15000});

    Logger::Info("DiskIdentifier: GET {}", RedactCredentials(response.url.str()));

    if (response.error) {
        Logger::Error("DiskIdentifier: HTTP error (hash search): {}", response.error.message);
        IdentifyResult result;
        result.errorMessage = "Network error: " + response.error.message;
        return result;
    }

    if (response.status_code != 200) {
        Logger::Info("DiskIdentifier: Hash search returned status {}", response.status_code);
        return {};
    }

    outRawJson = response.text;
    return ParseResponse(response.text);
}

IdentifyResult DiskIdentifier::SearchByName(const std::string& filename, std::string& outRawJson)
{
    if (!HasScreenscraperCredentials())
        return {};
    auto response = cpr::Get(cpr::Url{"https://www.screenscraper.fr/api2/jeuInfos.php"},
                             cpr::Parameters{{"devid", SS_DEVID},
                                             {"devpassword", SS_DEVPASS},
                                             {"softname", SS_SOFTNAME},
                                             {"output", "json"},
                                             {"systemeid", "64"},
                                             {"romnom", filename}},
                             cpr::Timeout{15000});

    Logger::Info("DiskIdentifier: GET {}", RedactCredentials(response.url.str()));

    if (response.error) {
        Logger::Error("DiskIdentifier: HTTP error (name search): {}", response.error.message);
        IdentifyResult result;
        result.errorMessage = "Network error: " + response.error.message;
        return result;
    }

    if (response.status_code != 200) {
        Logger::Info("DiskIdentifier: Name search returned status {}", response.status_code);
        return {};
    }

    outRawJson = response.text;
    return ParseResponse(response.text);
}

std::vector<uint8_t> DiskIdentifier::DownloadImage(const std::string& url)
{
    if (url.empty())
        return {};

    auto response = cpr::Get(cpr::Url{url}, cpr::Timeout{15000});

    if (response.error || response.status_code != 200) {
        Logger::Error("DiskIdentifier: Failed to download image: {}", url);
        return {};
    }

    return std::vector<uint8_t>(response.text.begin(), response.text.end());
}

IdentifyResult DiskIdentifier::ParseResponse(const std::string& jsonBody, bool downloadImages)
{
    IdentifyResult result;

    // The ScreenScraper API returns invalid JSON with backslash-escaped underscores
    // (e.g. "amiga\_disk\_manager") which are not valid JSON escape sequences.
    // Strip backslashes that precede non-JSON-escape characters.
    std::string sanitized;
    sanitized.reserve(jsonBody.size());
    for (size_t i = 0; i < jsonBody.size(); ++i) {
        if (jsonBody[i] == '\\' && i + 1 < jsonBody.size()) {
            char next = jsonBody[i + 1];
            // Valid JSON escapes: " \ / b f n r t u
            if (next == '"' || next == '\\' || next == '/' || next == 'b' || next == 'f' ||
                next == 'n' || next == 'r' || next == 't' || next == 'u') {
                sanitized += jsonBody[i]; // keep the backslash
            }
            // else: skip the invalid backslash
        } else {
            sanitized += jsonBody[i];
        }
    }

    try {
        auto json = nlohmann::json::parse(sanitized);

        // Check for success
        if (!json.contains("response") || !json["response"].contains("jeu")) {
            return result;
        }

        const auto& jeu = json["response"]["jeu"];

        // Game ID (for external links)
        result.gameId = jeu.value("id", "");

        // Parse title - prefer us/wor/eu regions, then ss, then first
        if (jeu.contains("noms") && jeu["noms"].is_array()) {
            for (const auto& nom : jeu["noms"]) {
                if (!nom.contains("region"))
                    continue;
                std::string region = nom["region"].get<std::string>();
                if (region == "us" || region == "wor" || region == "eu") {
                    result.title = nom.value("text", "");
                    break;
                }
            }
            if (result.title.empty()) {
                for (const auto& nom : jeu["noms"]) {
                    if (nom.contains("region") && nom["region"].get<std::string>() == "ss") {
                        result.title = nom.value("text", "");
                        break;
                    }
                }
            }
            if (result.title.empty() && !jeu["noms"].empty()) {
                result.title = jeu["noms"][0].value("text", "");
            }
        }

        // Developer
        if (jeu.contains("developpeur") && jeu["developpeur"].contains("text")) {
            result.developer = jeu["developpeur"]["text"].get<std::string>();
        }

        // Publisher
        if (jeu.contains("editeur") && jeu["editeur"].contains("text")) {
            result.publisher = jeu["editeur"]["text"].get<std::string>();
        }

        // Synopsis
        if (jeu.contains("synopsis") && jeu["synopsis"].is_array()) {
            result.synopsis = FindLocalizedText(jeu["synopsis"], "en");
        }

        // Release date - prefer wor, then eu, then first
        if (jeu.contains("dates") && jeu["dates"].is_array()) {
            for (const auto& d : jeu["dates"]) {
                if (d.contains("region") && d.contains("text")) {
                    std::string region = d["region"].get<std::string>();
                    if (region == "wor" || region == "eu" || region == "us") {
                        result.releaseDate = d["text"].get<std::string>();
                        break;
                    }
                }
            }
            if (result.releaseDate.empty() && !jeu["dates"].empty()) {
                result.releaseDate = jeu["dates"][0].value("text", "");
            }
        }

        // Genre
        if (jeu.contains("genres") && jeu["genres"].is_array() && !jeu["genres"].empty()) {
            const auto& firstGenre = jeu["genres"][0];
            if (firstGenre.contains("noms")) {
                result.genre = FindLocalizedText(firstGenre["noms"], "en");
            }
        }

        // Players
        if (jeu.contains("joueurs") && jeu["joueurs"].contains("text")) {
            result.players = jeu["joueurs"]["text"].get<std::string>();
        }

        // Rating (out of 20)
        if (jeu.contains("note") && jeu["note"].contains("text")) {
            result.rating = jeu["note"]["text"].get<std::string>();
        }

        // Family / series
        if (jeu.contains("familles") && jeu["familles"].is_array() && !jeu["familles"].empty()) {
            const auto& firstFamily = jeu["familles"][0];
            if (firstFamily.contains("noms")) {
                result.family = FindLocalizedText(firstFamily["noms"], "en");
                // familles often only have French names, fallback
                if (result.family.empty()) {
                    result.family = FindLocalizedText(firstFamily["noms"], "fr");
                }
            }
        }

        // System
        if (jeu.contains("systeme") && jeu["systeme"].contains("text")) {
            result.system = jeu["systeme"]["text"].get<std::string>();
        }

        result.found = !result.title.empty();

        if (result.found) {
            Logger::Info("DiskIdentifier: Found - {} (Dev: {}, Pub: {})", result.title,
                         result.developer, result.publisher);

            // Download images from the medias array
            if (downloadImages && jeu.contains("medias") && jeu["medias"].is_array()) {
                std::string screenshotUrl, boxArtUrl, wheelUrl;
                std::string devLogoUrl, pubLogoUrl;

                for (const auto& media : jeu["medias"]) {
                    if (!media.contains("type") || !media.contains("url"))
                        continue;

                    std::string type = media["type"].get<std::string>();
                    std::string parent = media.value("parent", "");

                    if (parent == "jeu") {
                        if (type == "ss" && screenshotUrl.empty()) {
                            screenshotUrl = media["url"].get<std::string>();
                        } else if (type == "box-2D" && boxArtUrl.empty()) {
                            boxArtUrl = media["url"].get<std::string>();
                        } else if (type == "wheel" && wheelUrl.empty()) {
                            wheelUrl = media["url"].get<std::string>();
                        }
                    } else if (parent == "developpeur" && type == "pictocouleur" &&
                               devLogoUrl.empty()) {
                        devLogoUrl = media["url"].get<std::string>();
                    } else if (parent == "editeur" && type == "pictocouleur" &&
                               pubLogoUrl.empty()) {
                        pubLogoUrl = media["url"].get<std::string>();
                    }
                }

                // Download images (still on background thread)
                if (!screenshotUrl.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_statusMessage = "Downloading screenshot...";
                    }
                    result.screenshotData = DownloadImage(screenshotUrl);
                }
                if (!boxArtUrl.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_statusMessage = "Downloading box art...";
                    }
                    result.boxArtData = DownloadImage(boxArtUrl);
                }
                if (!wheelUrl.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_statusMessage = "Downloading logo...";
                    }
                    result.wheelData = DownloadImage(wheelUrl);
                }
                if (!devLogoUrl.empty()) {
                    result.developerLogoData = DownloadImage(devLogoUrl);
                }
                if (!pubLogoUrl.empty()) {
                    result.publisherLogoData = DownloadImage(pubLogoUrl);
                }
            }
        }

    } catch (const nlohmann::json::exception& e) {
        Logger::Error("DiskIdentifier: JSON parse error: {}", e.what());
        result.errorMessage = "Failed to parse API response";
    }

    return result;
}

std::string DiskIdentifier::GetCacheDir()
{
    const char* homeEnv = std::getenv("HOME");
#ifdef _WIN32
    if (!homeEnv)
        homeEnv = std::getenv("USERPROFILE");
#endif
    if (!homeEnv)
        return {};

    auto cacheDir = std::filesystem::path(homeEnv) / ".peekaboo" / "cache";
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    return cacheDir.string();
}

std::vector<uint8_t> DiskIdentifier::ReadBinaryFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        return {};
    auto size = f.tellg();
    if (size <= 0)
        return {};
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

void DiskIdentifier::WriteBinaryFile(const std::string& path, const std::vector<uint8_t>& data)
{
    if (data.empty())
        return;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

bool DiskIdentifier::LoadFromCache(const std::string& sha1Hash, IdentifyResult& result)
{
    auto dir = GetCacheDir();
    if (dir.empty())
        return false;

    auto base = std::filesystem::path(dir) / sha1Hash;
    auto responsePath = base.string() + "_response.json";
    auto pouetPath = base.string() + "_pouet.json";

    bool responseExists = std::filesystem::exists(responsePath);
    bool pouetExists = std::filesystem::exists(pouetPath);

    if (!responseExists && !pouetExists)
        return false;

    bool loadedSomething = false;

    try {
        if (responseExists) {
            // Read the complete raw API response and re-parse it (without downloading images)
            std::ifstream responseFile(responsePath);
            std::string rawJson((std::istreambuf_iterator<char>(responseFile)),
                                std::istreambuf_iterator<char>());

            if (!rawJson.empty()) {
                IdentifyResult ssResult = ParseResponse(rawJson, false);
                result.screenScraperAttempted = true;
                if (ssResult.found) {
                    result.found = true;
                    result.title = ssResult.title;
                    result.developer = ssResult.developer;
                    result.publisher = ssResult.publisher;
                    result.synopsis = ssResult.synopsis;
                    result.releaseDate = ssResult.releaseDate;
                    result.genre = ssResult.genre;
                    result.players = ssResult.players;
                    result.rating = ssResult.rating;
                    result.family = ssResult.family;
                    result.system = ssResult.system;
                    result.gameId = ssResult.gameId;

                    // Load cached image binaries
                    result.screenshotData = ReadBinaryFile(base.string() + "_ss.bin");
                    result.boxArtData = ReadBinaryFile(base.string() + "_box.bin");
                    result.wheelData = ReadBinaryFile(base.string() + "_whl.bin");
                    result.developerLogoData = ReadBinaryFile(base.string() + "_dev.bin");
                    result.publisherLogoData = ReadBinaryFile(base.string() + "_pub.bin");
                    loadedSomething = true;
                }
            }
        }

        if (pouetExists) {
            try {
                std::ifstream pouetFile(pouetPath);
                auto pouetJson = nlohmann::json::parse(pouetFile);
                result.hasPouetInfo = true;
                result.pouetName = pouetJson.value("pouetName", "");
                result.pouetProdId = pouetJson.value("pouetProdId", "");
                result.pouetGroupId = pouetJson.value("pouetGroupId", "");
                result.pouetDownloadUrl = pouetJson.value("pouetDownloadUrl", "");
                result.pouetDate = pouetJson.value("pouetDate", "");
                result.pouetGroups = pouetJson.value("pouetGroups", "");
                result.pouetTypes = pouetJson.value("pouetTypes", "");
                result.pouetVoteUp = pouetJson.value("pouetVoteUp", "");
                result.pouetVotePig = pouetJson.value("pouetVotePig", "");
                result.pouetVoteDown = pouetJson.value("pouetVoteDown", "");
                result.pouetVoteAvg = pouetJson.value("pouetVoteAvg", "");
                result.pouetPartyName = pouetJson.value("pouetPartyName", "");
                result.pouetPartyYear = pouetJson.value("pouetPartyYear", "");
                result.pouetPartyCompo = pouetJson.value("pouetPartyCompo", "");
                result.pouetPartyPlace = pouetJson.value("pouetPartyPlace", "");
                result.pouetAttempted = true;
                loadedSomething = true;
            } catch (const std::exception& e) {
                Logger::Error("DiskIdentifier: Failed to load Pouet cache: {}", e.what());
            }
        }

        return loadedSomething;
    } catch (const std::exception& e) {
        Logger::Error("DiskIdentifier: Cache load error: {}", e.what());
        return false;
    }
}

void DiskIdentifier::SaveToCache(const std::string& sha1Hash, const IdentifyResult& result,
                                 const std::string& rawJson)
{
    auto dir = GetCacheDir();
    if (dir.empty())
        return;

    auto base = std::filesystem::path(dir) / sha1Hash;

    try {
        // Save the complete raw API response JSON so cache hits re-parse rather than duplicate
        // fields
        if (!rawJson.empty()) {
            std::ofstream responseFile(base.string() + "_response.json");
            responseFile << rawJson;
        }

        // Save image binaries separately (URLs in JSON may expire)
        WriteBinaryFile(base.string() + "_ss.bin", result.screenshotData);
        WriteBinaryFile(base.string() + "_box.bin", result.boxArtData);
        WriteBinaryFile(base.string() + "_whl.bin", result.wheelData);
        WriteBinaryFile(base.string() + "_dev.bin", result.developerLogoData);
        WriteBinaryFile(base.string() + "_pub.bin", result.publisherLogoData);

        if (result.hasPouetInfo) {
            nlohmann::json pouetJson = {{"pouetName", result.pouetName},
                                        {"pouetProdId", result.pouetProdId},
                                        {"pouetGroupId", result.pouetGroupId},
                                        {"pouetDownloadUrl", result.pouetDownloadUrl},
                                        {"pouetDate", result.pouetDate},
                                        {"pouetGroups", result.pouetGroups},
                                        {"pouetTypes", result.pouetTypes},
                                        {"pouetVoteUp", result.pouetVoteUp},
                                        {"pouetVotePig", result.pouetVotePig},
                                        {"pouetVoteDown", result.pouetVoteDown},
                                        {"pouetVoteAvg", result.pouetVoteAvg},
                                        {"pouetPartyName", result.pouetPartyName},
                                        {"pouetPartyYear", result.pouetPartyYear},
                                        {"pouetPartyCompo", result.pouetPartyCompo},
                                        {"pouetPartyPlace", result.pouetPartyPlace}};
            std::ofstream pouetFile(base.string() + "_pouet.json");
            pouetFile << pouetJson.dump(4);
        }

        // Write MD5→SHA1 index so TryLoadCacheByMD5 can locate this entry by ADF content MD5
        if (!result.md5.empty()) {
            auto indexPath = std::filesystem::path(dir) / (result.md5 + "_sha1.dat");
            std::ofstream indexFile(indexPath);
            indexFile << sha1Hash;
        }

        Logger::Info("DiskIdentifier: Cached result for {}", sha1Hash);
    } catch (const std::exception& e) {
        Logger::Error("DiskIdentifier: Cache save error: {}", e.what());
    }
}

bool DiskIdentifier::TryLoadCacheByMD5(const std::string& md5, IdentifyResult& out)
{
    if (md5.empty())
        return false;
    auto dir = GetCacheDir();
    if (dir.empty())
        return false;

    // Read the SHA1 that was the actual cache key for this ADF MD5
    auto indexPath = std::filesystem::path(dir) / (md5 + "_sha1.dat");
    if (!std::filesystem::exists(indexPath))
        return false;

    std::ifstream f(indexPath);
    std::string sha1;
    f >> sha1;
    if (sha1.empty())
        return false;

    return LoadFromCache(sha1, out);
}

} // namespace core
} // namespace adm
