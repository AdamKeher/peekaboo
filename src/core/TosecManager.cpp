#include "TosecManager.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <thread>
#include "Logger.h"

namespace adm::core {

static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

static std::string Trim(const std::string& s)
{
    auto first = s.find_first_not_of(" \t\r\n");
    if (std::string::npos == first)
        return s;
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, (last - first + 1));
}

TosecManager& TosecManager::Instance()
{
    static TosecManager instance;
    return instance;
}

void TosecManager::Initialize()
{
    if (m_loaderThread.joinable())
        return; // already loading (or loaded)
    m_loaderThread = std::thread([this]() { LoadTosecJson(); });
}

void TosecManager::Shutdown()
{
    if (m_loaderThread.joinable())
        m_loaderThread.join();
}

void TosecManager::LoadTosecJson()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_loaded.load(std::memory_order_relaxed))
        return;

    namespace fs = std::filesystem;
    fs::path tosecDir = fs::current_path() / "tosec";
    fs::path jsonPath = tosecDir / "tosec.json";

    if (!fs::exists(jsonPath)) {
        tosecDir = fs::current_path() / ".." / "tosec";
        jsonPath = tosecDir / "tosec.json";
    }

    if (!fs::exists(jsonPath)) {
        Logger::Warn("TosecManager: 'tosec/tosec.json' not found.");
        m_loaded.store(true, std::memory_order_release);
        return;
    }

    Logger::Info("TosecManager: Loading tosec.json from {}", jsonPath.string());

    try {
        std::ifstream f(jsonPath, std::ios::binary);
        if (!f.is_open()) {
            Logger::Error("TosecManager: Could not open tosec.json");
            m_loaded.store(true, std::memory_order_release);
            return;
        }

        nlohmann::json data = nlohmann::json::parse(f);
        if (!data.is_array()) {
            Logger::Error("TosecManager: tosec.json is not an array");
            m_loaded.store(true, std::memory_order_release);
            return;
        }

        int entriesLoaded = 0;
        int gamesProcessed = 0;

        auto readStr = [](const nlohmann::json& obj, const std::string& key) -> std::string {
            if (obj.contains(key) && obj[key].is_string())
                return obj[key].get<std::string>();
            return "";
        };

        auto readBool = [](const nlohmann::json& obj, const std::string& key) -> bool {
            if (obj.contains(key) && obj[key].is_boolean())
                return obj[key].get<bool>();
            return false;
        };

        auto readInt = [](const nlohmann::json& obj, const std::string& key) -> int {
            if (obj.contains(key) && obj[key].is_number_integer())
                return obj[key].get<int>();
            return 0;
        };

        auto readStrArray = [](const nlohmann::json& obj,
                               const std::string& key) -> std::vector<std::string> {
            std::vector<std::string> res;
            if (obj.contains(key) && obj[key].is_array()) {
                for (const auto& item : obj[key]) {
                    if (item.is_string())
                        res.push_back(item.get<std::string>());
                }
            }
            return res;
        };

        for (auto& entry : data) {
            try {
                gamesProcessed++;
                std::string gameName = readStr(entry, "name");
                std::string description = readStr(entry, "description");
                std::string category = readStr(entry, "Category");
                std::string downloadUrl = readStr(entry, "Download URL");

                // Flat TOSEC metadata fields
                std::string title = readStr(entry, "Title");
                std::string version = readStr(entry, "Version");
                std::string demo = readStr(entry, "Demo");
                std::string year = readStr(entry, "Year");
                std::string publisher = readStr(entry, "Publisher");
                std::string releaseGroup = readStr(entry, "ReleaseGroup");
                std::string language = readStr(entry, "Language");
                std::string system = readStr(entry, "System");
                std::string video = readStr(entry, "Video");
                std::string region = readStr(entry, "Region");
                auto media = readStrArray(entry, "Media");

                bool verified = readBool(entry, "Verified");
                bool alternateDump = readBool(entry, "AlternateDump");
                bool badDump = readBool(entry, "BadDump");
                bool overdump = readBool(entry, "Overdump");
                bool virusInfected = readBool(entry, "VirusInfected");
                std::string virusName = readStr(entry, "VirusName");
                bool fixed = readBool(entry, "Fixed");

                bool cracked = readBool(entry, "Cracked");
                std::string crackGroup = readStr(entry, "CrackGroup");

                bool trained = readBool(entry, "Trained");
                int trainerCount = readInt(entry, "TrainerCount");
                std::string trainerLanguage = readStr(entry, "TrainerLanguage");
                std::string trainerGroup = readStr(entry, "TrainerGroup");

                bool hacked = readBool(entry, "Hacked");
                std::string hackType = readStr(entry, "HackType");
                std::string hackGroup = readStr(entry, "HackGroup");

                bool modified = readBool(entry, "Modified");
                std::string modType = readStr(entry, "ModType");
                std::string modGroup = readStr(entry, "ModGroup");
                std::string titleGroupId = readStr(entry, "TitleGroupId");

                if (entry.contains("roms") && entry["roms"].is_array()) {
                    for (auto& rom : entry["roms"]) {
                        std::string romMd5 = readStr(rom, "md5");
                        if (!romMd5.empty()) {
                            TosecEntry tosec;
                            tosec.gameTitle = title.empty() ? gameName : title;
                            tosec.description = description;
                            tosec.romName = readStr(rom, "name");
                            tosec.size = readStr(rom, "size");
                            tosec.crc = readStr(rom, "crc");
                            tosec.md5 = Trim(romMd5);
                            tosec.sha1 = Trim(readStr(rom, "sha1"));

                            tosec.category = category;
                            tosec.downloadUrl = downloadUrl;

                            tosec.version = version;
                            tosec.demo = demo;
                            tosec.year = year;
                            tosec.publisher = publisher;
                            tosec.releaseGroup = releaseGroup;
                            tosec.language = language;
                            tosec.system = system;
                            tosec.video = video;
                            tosec.region = region;
                            tosec.media = media;

                            tosec.verified = verified;
                            tosec.alternateDump = alternateDump;
                            tosec.badDump = badDump;
                            tosec.overdump = overdump;
                            tosec.virusInfected = virusInfected;
                            tosec.virusName = virusName;
                            tosec.fixed = fixed;

                            tosec.cracked = cracked;
                            tosec.crackGroup = crackGroup;

                            tosec.trained = trained;
                            tosec.trainerCount = trainerCount;
                            tosec.trainerLanguage = trainerLanguage;
                            tosec.trainerGroup = trainerGroup;

                            tosec.hacked = hacked;
                            tosec.hackType = hackType;
                            tosec.hackGroup = hackGroup;

                            tosec.modified = modified;
                            tosec.modType = modType;
                            tosec.modGroup = modGroup;
                            tosec.titleGroupId = titleGroupId;

                            // Own the entry once; index it from both maps by pointer.
                            m_entries.push_back(std::move(tosec));
                            const TosecEntry* ptr = &m_entries.back();
                            m_md5Map[ToLower(ptr->md5)] = ptr;
                            if (!ptr->sha1.empty())
                                m_sha1Map[ToLower(ptr->sha1)] = ptr;

                            entriesLoaded++;
                        }
                    }
                }
            } catch (const std::exception& e) {
                Logger::Error("TosecManager: Exception processing entry: {}", e.what());
            }
        }
        Logger::Info("TosecManager: Finished loading. Games: {}, ROMs mapped: {}", gamesProcessed,
                     entriesLoaded);
    } catch (const std::exception& e) {
        Logger::Error("TosecManager: Exception while loading JSON: {}", e.what());
    }

    m_loaded.store(true, std::memory_order_release);
}

std::vector<const TosecEntry*> TosecManager::GetAllEntries() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<const TosecEntry*> result;
    result.reserve(m_md5Map.size());
    for (const auto& [key, entry] : m_md5Map)
        result.push_back(entry);
    return result;
}

size_t TosecManager::GetEntryCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_md5Map.size();
}

const TosecEntry* TosecManager::FindByMD5(const std::string& md5) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_loaded.load(std::memory_order_relaxed))
        return nullptr;

    std::string key = ToLower(Trim(md5));
    auto it = m_md5Map.find(key);
    if (it != m_md5Map.end()) {
        Logger::Info("TosecManager: Match found for MD5 '{}'", key);
        return it->second;
    }

    return nullptr;
}

const TosecEntry* TosecManager::FindBySHA1(const std::string& sha1) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_loaded.load(std::memory_order_relaxed))
        return nullptr;

    std::string key = ToLower(Trim(sha1));
    auto it = m_sha1Map.find(key);
    if (it != m_sha1Map.end()) {
        return it->second;
    }
    return nullptr;
}

TosecFilenameInfo TosecManager::ParseFilename(const std::string& filename)
{
    TosecFilenameInfo info;
    info.valid = false;

    // Standard TOSEC format roughly: "Title
    // (Year)(Publisher)(System)(Video)(Region)(Language)[Status]..." We will simple regex to pull
    // out Title (Year)(Publisher). The Title is everything before the first '('.

    // Remove extension
    std::string name = filename;
    size_t extPos = name.find_last_of('.');
    if (extPos != std::string::npos) {
        name = name.substr(0, extPos);
    }

    // Find first '('
    size_t firstParen = name.find('(');
    if (firstParen == std::string::npos) {
        // Fallback: just title
        info.title = name;
        return info;
    }

    info.title = name.substr(0, firstParen);
    // Trim trailing spaces from title
    while (!info.title.empty() && std::isspace(info.title.back())) {
        info.title.pop_back();
    }

    std::string remainder = name.substr(firstParen);

    // Regex to iterate over (...) and [...] blocks
    std::regex blockRegex(R"([\(\[][^\)\]]+[\)\]])");
    auto blocks_begin = std::sregex_iterator(remainder.begin(), remainder.end(), blockRegex);
    auto blocks_end = std::sregex_iterator();

    int index = 0;
    for (std::sregex_iterator i = blocks_begin; i != blocks_end; ++i) {
        std::string block = i->str();
        // block includes parens, e.g. "(1990)"

        if (block.size() < 3)
            continue;
        std::string content = block.substr(1, block.size() - 2);

        // Heuristics for TOSEC:
        // 1st block usually Year
        // 2nd block usually Publisher

        if (index == 0) {
            // Check if year-ish (4 digits)
            if (std::regex_match(content, std::regex(R"(\d{4}|19xx|20xx)"))) {
                info.year = content;
            } else {
                // If not a year, maybe the title had parens in it?
                // Or "Disk 1 of 2".
                // For now, assume it's part of flags if not year.
                info.flags.push_back(block);
            }
        } else if (index == 1 && !info.year.empty()) {
            // If we found a year previously, this is likely Publisher
            info.publisher = content;
        } else {
            info.flags.push_back(block);
        }
        index++;
    }

    info.valid = true;
    return info;
}

} // namespace adm::core
