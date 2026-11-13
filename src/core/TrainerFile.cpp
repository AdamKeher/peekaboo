#include "TrainerFile.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace adm::core {

namespace {

constexpr const char* kFormatTag = "peekaboo-trainer";

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

// RetroArch writes every value quoted; strip the quotes if they are there.
std::string Unquote(const std::string& s)
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

/// memory_search_size in a .cht is an enum, not a byte count: 0-2 are sub-byte bit widths we
/// cannot express, 3 = 8-bit, 4 = 16-bit, 5 = 32-bit. Returns 0 for the widths we drop.
uint8_t ChtSearchSizeToBytes(int v)
{
    switch (v) {
        case 3: return 1;
        case 4: return 2;
        case 5: return 4;
        default: return 0; // 1/2/4-bit cheats have no Amiga equivalent worth inventing
    }
}

int ChtInt(const std::map<std::string, std::string>& kv, const std::string& key, int fallback)
{
    const auto it = kv.find(key);
    if (it == kv.end())
        return fallback;
    try {
        return std::stoi(it->second, nullptr, 0);
    } catch (...) {
        return fallback;
    }
}

} // namespace

const char* ToString(TrainerMatch m)
{
    switch (m) {
        case TrainerMatch::None: return "none";
        case TrainerMatch::ExactMd5: return "exact md5";
        case TrainerMatch::TitleGroup: return "title group";
    }
    return "none";
}

// --- native JSON -------------------------------------------------------------------------

std::optional<TrainerDoc> TrainerFile::Load(const std::string& path, std::string& err)
{
    nlohmann::json j;
    try {
        std::ifstream f(path);
        if (!f.is_open()) {
            err = fmt::format("cannot open {}", path);
            return std::nullopt;
        }
        f >> j;
    } catch (const std::exception& e) {
        err = fmt::format("{} is not valid JSON ({})", path, e.what());
        return std::nullopt;
    }

    if (!j.is_object()) {
        err = "trainer file is not a JSON object";
        return std::nullopt;
    }
    if (j.contains("format") && j["format"].is_string() &&
        j["format"].get<std::string>() != kFormatTag) {
        err = fmt::format("unexpected format '{}' (expected '{}')", j["format"].get<std::string>(),
                          kFormatTag);
        return std::nullopt;
    }

    TrainerDoc doc;
    if (j.contains("version") && j["version"].is_number())
        doc.version = j["version"].get<int>();
    if (doc.version > 1) {
        err = fmt::format("trainer file is version {}, this build understands version 1",
                          doc.version);
        return std::nullopt;
    }
    if (j.contains("title") && j["title"].is_string())
        doc.title = j["title"].get<std::string>();
    if (j.contains("author") && j["author"].is_string())
        doc.author = j["author"].get<std::string>();
    if (j.contains("notes") && j["notes"].is_string())
        doc.notes = j["notes"].get<std::string>();

    if (j.contains("identity") && j["identity"].is_object()) {
        const auto& id = j["identity"];
        if (id.contains("md5")) {
            if (id["md5"].is_string()) {
                doc.identity.md5.push_back(Lower(id["md5"].get<std::string>()));
            } else if (id["md5"].is_array()) {
                for (const auto& h : id["md5"]) {
                    if (h.is_string())
                        doc.identity.md5.push_back(Lower(h.get<std::string>()));
                }
            }
        }
        if (id.contains("tosecTitleGroupId") && id["tosecTitleGroupId"].is_string())
            doc.identity.tosecTitleGroupId = id["tosecTitleGroupId"].get<std::string>();
        if (id.contains("authoredOn") && id["authoredOn"].is_object()) {
            const auto& a = id["authoredOn"];
            if (a.contains("kickstart") && a["kickstart"].is_string())
                doc.identity.kickstart = a["kickstart"].get<std::string>();
            if (a.contains("chipRam") && a["chipRam"].is_number())
                doc.identity.chipRamKb = a["chipRam"].get<uint32_t>();
            if (a.contains("fastRam") && a["fastRam"].is_number())
                doc.identity.fastRamKb = a["fastRam"].get<uint32_t>();
        }
    }

    if (!j.contains("entries") || !j["entries"].is_array()) {
        err = "trainer file has no entries array";
        return std::nullopt;
    }
    for (const auto& item : j["entries"]) {
        std::string entryErr;
        if (auto e = MemoryLocks::EntryFromJson(item, entryErr)) {
            doc.entries.push_back(std::move(*e));
        } else {
            // One bad entry should not cost the user the whole file; report it in err and
            // carry on with the rest.
            if (!err.empty())
                err += "; ";
            err += entryErr;
        }
    }
    return doc;
}

bool TrainerFile::Save(const std::string& path, const TrainerDoc& doc, std::string& err)
{
    try {
        nlohmann::json j;
        j["format"] = kFormatTag;
        j["version"] = 1;
        if (!doc.title.empty())
            j["title"] = doc.title;
        if (!doc.author.empty())
            j["author"] = doc.author;
        if (!doc.notes.empty())
            j["notes"] = doc.notes;

        if (!doc.identity.Empty() || !doc.identity.kickstart.empty()) {
            nlohmann::json id;
            if (!doc.identity.md5.empty())
                id["md5"] = doc.identity.md5;
            if (!doc.identity.tosecTitleGroupId.empty())
                id["tosecTitleGroupId"] = doc.identity.tosecTitleGroupId;
            if (!doc.identity.kickstart.empty() || doc.identity.chipRamKb ||
                doc.identity.fastRamKb) {
                nlohmann::json a;
                if (!doc.identity.kickstart.empty())
                    a["kickstart"] = doc.identity.kickstart;
                a["chipRam"] = doc.identity.chipRamKb;
                a["fastRam"] = doc.identity.fastRamKb;
                id["authoredOn"] = a;
            }
            j["identity"] = id;
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : doc.entries)
            arr.push_back(MemoryLocks::EntryToJson(e));
        j["entries"] = arr;

        std::ofstream f(path);
        if (!f.is_open()) {
            err = fmt::format("cannot write {}", path);
            return false;
        }
        f << j.dump(2);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

// --- RetroArch .cht import ---------------------------------------------------------------

std::optional<TrainerDoc> TrainerFile::ImportCht(const std::string& path, std::string& err,
                                                 int* skipped)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        err = fmt::format("cannot open {}", path);
        return std::nullopt;
    }

    // A .cht is a flat "key = value" ini with every field prefixed cheatN_.
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        kv[Trim(line.substr(0, eq))] = Unquote(Trim(line.substr(eq + 1)));
    }
    if (kv.empty()) {
        err = "no key = value pairs found - is this a .cht file?";
        return std::nullopt;
    }

    TrainerDoc doc;
    doc.title = std::filesystem::path(path).stem().string();
    doc.notes = fmt::format("Imported from {}", std::filesystem::path(path).filename().string());

    int dropped = 0;
    const int count = ChtInt(kv, "cheats", 0);
    // `cheats` is not always present or accurate, so walk until a gap rather than trusting it.
    for (int i = 0; count == 0 || i < count; ++i) {
        const std::string pre = fmt::format("cheat{}_", i);
        if (kv.find(pre + "address") == kv.end() && kv.find(pre + "desc") == kv.end()) {
            if (count == 0)
                break;
            continue;
        }

        const int cheatType = ChtInt(kv, pre + "cheat_type", 1);
        const uint8_t size = ChtSearchSizeToBytes(ChtInt(kv, pre + "memory_search_size", 3));
        if (cheatType != 1 || size == 0) {
            // cheat_type 1 is "set to value"; the increase/decrease/conditional types have no
            // equivalent here, and sub-byte widths cannot be expressed.
            ++dropped;
            continue;
        }

        LockEntry e;
        const auto d = kv.find(pre + "desc");
        e.desc = d != kv.end() && !d->second.empty() ? d->second : fmt::format("cheat {}", i);
        e.mode = LockMode::Set;
        e.size = size;
        e.value = static_cast<uint32_t>(ChtInt(kv, pre + "value", 0));
        // Deliberately off: an imported address has no guard bytes to verify, so it must be
        // reviewed before it is trusted.
        e.enabled = false;
        e.anchor.kind = AnchorKind::Absolute;

        const uint32_t base = static_cast<uint32_t>(ChtInt(kv, pre + "address", 0));
        const int repeat = std::max(1, ChtInt(kv, pre + "repeat_count", 1));
        const int addrStep = ChtInt(kv, pre + "repeat_add_to_address", 1);
        const int valueStep = ChtInt(kv, pre + "repeat_add_to_value", 0);

        for (int r = 0; r < repeat; ++r) {
            LockEntry rep = e;
            rep.anchor.addr = base + static_cast<uint32_t>(r * addrStep);
            rep.value = e.value + static_cast<uint32_t>(r * valueStep);
            if (repeat > 1)
                rep.desc = fmt::format("{} [{}/{}]", e.desc, r + 1, repeat);
            doc.entries.push_back(std::move(rep));
        }
    }

    if (skipped)
        *skipped = dropped;
    if (doc.entries.empty()) {
        err = dropped ? fmt::format("all {} cheat(s) use features with no Amiga equivalent",
                                    dropped)
                      : "no usable cheats found";
        return std::nullopt;
    }
    return doc;
}

// --- identity matching --------------------------------------------------------------------

bool TrainerFile::MatchesMd5(const TrainerDoc& doc, const std::string& md5)
{
    if (md5.empty())
        return false;
    const std::string want = Lower(md5);
    return std::find(doc.identity.md5.begin(), doc.identity.md5.end(), want) !=
           doc.identity.md5.end();
}

std::string TrainerFile::FindForMedia(const std::string& dir, const std::string& md5,
                                      const std::string& titleGroupId, TrainerMatch& outMatch)
{
    outMatch = TrainerMatch::None;
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec))
        return {};

    // The cheap hit first: trainers/<md5>.json needs no parsing at all.
    if (!md5.empty()) {
        const std::string direct = (std::filesystem::path(dir) / (Lower(md5) + ".json")).string();
        if (std::filesystem::exists(direct, ec)) {
            outMatch = TrainerMatch::ExactMd5;
            return direct;
        }
    }

    std::string titleGroupHit;
    for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
        if (!de.is_regular_file() || de.path().extension() != ".json")
            continue;
        std::string err;
        auto doc = Load(de.path().string(), err);
        if (!doc)
            continue;
        if (MatchesMd5(*doc, md5)) {
            outMatch = TrainerMatch::ExactMd5;
            return de.path().string(); // an exact hash always wins
        }
        if (titleGroupHit.empty() && !titleGroupId.empty() &&
            doc->identity.tosecTitleGroupId == titleGroupId) {
            titleGroupHit = de.path().string();
        }
    }

    if (!titleGroupHit.empty()) {
        outMatch = TrainerMatch::TitleGroup;
        return titleGroupHit;
    }
    return {};
}

} // namespace adm::core
