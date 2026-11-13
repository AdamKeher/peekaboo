#include "EmulatorStateStore.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "Logger.h"

namespace adm::core {

namespace {

using nlohmann::json;

// .pkst layout: "PKST", uint32 version, uint32 metadata length, metadata JSON, raw
// serialized machine state. The metadata is kept out of the payload so List() can read
// it without pulling multi-megabyte snapshots into memory.
constexpr char kMagic[4] = {'P', 'K', 'S', 'T'};
constexpr uint32_t kVersion = 1;

std::string NowString()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

void WriteU32(std::ostream& os, uint32_t v)
{
    char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                 static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
    os.write(b, 4);
}

bool ReadU32(std::istream& is, uint32_t& v)
{
    unsigned char b[4];
    if (!is.read(reinterpret_cast<char*>(b), 4))
        return false;
    v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
        (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

} // namespace

EmulatorStateStore::EmulatorStateStore(std::filesystem::path directory)
    : m_directory(std::move(directory))
{
}

bool EmulatorStateStore::IsValidName(const std::string& name)
{
    if (name.empty() || name.size() > 64)
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
    });
}

std::filesystem::path EmulatorStateStore::FilePath(const std::string& name) const
{
    return m_directory / (name + ".pkst");
}

bool EmulatorStateStore::Save(const std::string& name, std::vector<uint8_t> data, uint64_t frame,
                              const std::string& coreVersion, const std::string& document,
                              bool persist, std::string& outError)
{
    Entry entry;
    entry.info.name = name;
    entry.info.bytes = data.size();
    entry.info.frame = frame;
    entry.info.coreVersion = coreVersion;
    entry.info.document = document;
    entry.info.created = NowString();
    entry.info.persisted = persist;
    entry.data = std::move(data);
    entry.dataLoaded = true;

    // Keep the in-memory copy regardless of what the disk does, so a read-only or full
    // volume costs you persistence but not the snapshot itself.
    const bool wasPersisted = m_entries.count(name) && m_entries[name].info.persisted;
    m_entries[name] = entry;

    if (!persist) {
        // A non-persisted save over a persisted one leaves the old file behind and stale;
        // remove it so List() cannot report two different snapshots under one name.
        if (wasPersisted) {
            std::error_code ec;
            std::filesystem::remove(FilePath(name), ec);
        }
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    if (ec) {
        outError = "cannot create state directory: " + ec.message();
        m_entries[name].info.persisted = false;
        return false;
    }

    json meta{{"name", name},         {"bytes", entry.info.bytes},
              {"frame", frame},       {"coreVersion", coreVersion},
              {"document", document}, {"created", entry.info.created}};
    const std::string metaText = meta.dump();

    std::ofstream os(FilePath(name), std::ios::binary | std::ios::trunc);
    if (!os) {
        outError = "cannot write " + FilePath(name).string();
        m_entries[name].info.persisted = false;
        return false;
    }
    os.write(kMagic, 4);
    WriteU32(os, kVersion);
    WriteU32(os, static_cast<uint32_t>(metaText.size()));
    os.write(metaText.data(), static_cast<std::streamsize>(metaText.size()));
    os.write(reinterpret_cast<const char*>(m_entries[name].data.data()),
             static_cast<std::streamsize>(m_entries[name].data.size()));
    if (!os) {
        outError = "write failed for " + FilePath(name).string();
        m_entries[name].info.persisted = false;
        return false;
    }
    return true;
}

bool EmulatorStateStore::ReadFile(const std::string& name, Entry& out) const
{
    std::ifstream is(FilePath(name), std::ios::binary);
    if (!is)
        return false;

    char magic[4];
    if (!is.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0)
        return false;
    uint32_t version = 0;
    uint32_t metaLen = 0;
    if (!ReadU32(is, version) || version != kVersion || !ReadU32(is, metaLen))
        return false;
    if (metaLen > 64 * 1024)
        return false;

    std::string metaText(metaLen, '\0');
    if (metaLen && !is.read(metaText.data(), metaLen))
        return false;

    json meta = json::parse(metaText, nullptr, false);
    if (meta.is_discarded() || !meta.is_object())
        return false;

    out.info.name = name;
    out.info.frame = meta.value("frame", 0ull);
    out.info.coreVersion = meta.value("coreVersion", std::string{});
    out.info.document = meta.value("document", std::string{});
    out.info.created = meta.value("created", std::string{});
    out.info.bytes = meta.value("bytes", size_t{0});
    out.info.persisted = true;

    out.data.assign(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
    out.dataLoaded = true;
    out.info.bytes = out.data.size();
    return true;
}

void EmulatorStateStore::ScanDirectory()
{
    m_scanned = true;
    std::error_code ec;
    if (!std::filesystem::exists(m_directory, ec))
        return;

    for (const auto& de : std::filesystem::directory_iterator(m_directory, ec)) {
        if (ec)
            break;
        if (!de.is_regular_file() || de.path().extension() != ".pkst")
            continue;
        const std::string name = de.path().stem().string();
        if (m_entries.count(name))
            continue; // in-memory copy wins; it is at least as fresh

        // Read the header only: the payload is pulled in on demand by Load().
        Entry entry;
        if (!ReadFile(name, entry)) {
            Logger::Warn("State store: ignoring malformed snapshot '{}'", de.path().string());
            continue;
        }
        entry.data.clear();
        entry.dataLoaded = false;
        m_entries[name] = std::move(entry);
    }
}

std::optional<std::vector<uint8_t>> EmulatorStateStore::Load(const std::string& name,
                                                             StateInfo& outInfo)
{
    if (!m_scanned)
        ScanDirectory();

    auto it = m_entries.find(name);
    if (it == m_entries.end())
        return std::nullopt;

    if (!it->second.dataLoaded) {
        Entry fromDisk;
        if (!ReadFile(name, fromDisk))
            return std::nullopt;
        it->second.data = std::move(fromDisk.data);
        it->second.dataLoaded = true;
    }
    outInfo = it->second.info;
    return it->second.data;
}

std::optional<EmulatorStateStore::StateInfo> EmulatorStateStore::Info(const std::string& name)
{
    if (!m_scanned)
        ScanDirectory();
    auto it = m_entries.find(name);
    if (it == m_entries.end())
        return std::nullopt;
    return it->second.info;
}

std::vector<EmulatorStateStore::StateInfo> EmulatorStateStore::List()
{
    if (!m_scanned)
        ScanDirectory();
    std::vector<StateInfo> out;
    out.reserve(m_entries.size());
    for (const auto& [name, entry] : m_entries)
        out.push_back(entry.info);
    std::sort(out.begin(), out.end(),
              [](const StateInfo& a, const StateInfo& b) { return a.created > b.created; });
    return out;
}

bool EmulatorStateStore::Remove(const std::string& name)
{
    if (!m_scanned)
        ScanDirectory();
    auto it = m_entries.find(name);
    if (it == m_entries.end())
        return false;
    m_entries.erase(it);
    std::error_code ec;
    std::filesystem::remove(FilePath(name), ec);
    return true;
}

} // namespace adm::core
