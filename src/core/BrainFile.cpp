#include "BrainFile.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>

#include "Logger.h"

namespace adm {
namespace core {

// CRC32 lookup table (IEEE 802.3 polynomial)
static const uint32_t crc32Table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D};

BrainFile::BrainFile() = default;

bool BrainFile::LoadDatabase(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::Error("BrainFile: Failed to open {}", path);
        return false;
    }

    try {
        nlohmann::json root = nlohmann::json::parse(file);

        if (!root.contains("Bootblocks") || !root["Bootblocks"].is_array()) {
            Logger::Error("BrainFile: Invalid format - missing Bootblocks array");
            return false;
        }

        auto GetStringValue = [](const nlohmann::json& j, const std::string& key) -> std::string {
            if (!j.contains(key) || j[key].is_null()) {
                return "";
            }
            if (j[key].is_string()) {
                return j[key].get<std::string>();
            }
            if (j[key].is_number()) {
                if (j[key].is_number_integer()) {
                    return std::to_string(j[key].get<int64_t>());
                }
                return std::to_string(j[key].get<double>());
            }
            if (j[key].is_boolean()) {
                return j[key].get<bool>() ? "True" : "False";
            }
            return "";
        };

        m_entries.clear();
        for (const auto& entry : root["Bootblocks"]) {
            BrainEntry be;
            be.name = GetStringValue(entry, "Name");
            be.recog = GetStringValue(entry, "Recog");
            be.classCode = GetStringValue(entry, "Class");
            be.notes = GetStringValue(entry, "Notes");
            be.kickstart = GetStringValue(entry, "KS");
            be.file = GetStringValue(entry, "File");

            // CRC can be string or number in JSON
            if (entry.contains("CRC")) {
                if (entry["CRC"].is_string()) {
                    be.crc32 = entry["CRC"].get<std::string>();
                } else if (entry["CRC"].is_number()) {
                    // Convert number to hex string
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%X",
                             static_cast<unsigned int>(entry["CRC"].get<uint64_t>()));
                    be.crc32 = buf;
                }
            }

            // Bootable can be string "True"/"False" or boolean
            if (entry.contains("Bootable")) {
                if (entry["Bootable"].is_boolean()) {
                    be.bootable = entry["Bootable"].get<bool>();
                } else if (entry["Bootable"].is_string()) {
                    be.bootable = (entry["Bootable"].get<std::string>() == "True");
                }
            }

            m_entries.push_back(be);
        }

        m_loaded = true;
        m_loadedPath = path;
        Logger::Info("BrainFile: Loaded {} bootblock entries from {}", m_entries.size(), path);
        Logger::Info("BrainFile: Successfully loaded {} entries", m_entries.size());
        return true;

    } catch (const std::exception& e) {
        Logger::Error("BrainFile: JSON parse error: {}", e.what());
        return false;
    }
}

bool BrainFile::SaveDatabase(const std::string& path)
{
    std::string savePath = path.empty() ? m_loadedPath : path;
    if (savePath.empty()) {
        Logger::Error("BrainFile: No path specified for saving");
        return false;
    }

    nlohmann::json root;
    root["Bootblocks"] = nlohmann::json::array();

    for (const auto& entry : m_entries) {
        nlohmann::json node;
        node["Name"] = entry.name;
        if (!entry.recog.empty())
            node["Recog"] = entry.recog;
        node["Class"] = entry.classCode;
        if (!entry.notes.empty())
            node["Notes"] = entry.notes;
        if (!entry.kickstart.empty())
            node["KS"] = entry.kickstart;
        if (!entry.crc32.empty())
            node["CRC"] = entry.crc32;
        if (!entry.file.empty())
            node["File"] = entry.file;
        node["Bootable"] = entry.bootable ? "True" : "False";

        root["Bootblocks"].push_back(node);
    }

    std::ofstream file(savePath);
    if (!file.is_open()) {
        Logger::Error("BrainFile: Failed to open {} for writing", savePath);
        return false;
    }

    file << root.dump(4);
    Logger::Info("BrainFile: Saved {} entries to {}", m_entries.size(), savePath);
    return true;
}

bool BrainFile::AddEntry(const BootBlockInfo& info)
{
    // Check if CRC already exists
    for (const auto& entry : m_entries) {
        if (entry.crc32 == info.crc32 && !info.crc32.empty()) {
            Logger::Warn("BrainFile: Entry with CRC {} already exists ({})", info.crc32,
                         entry.name);
            return false;
        }
    }

    BrainEntry newEntry;
    newEntry.name = info.name;
    newEntry.classCode = info.classCode;
    newEntry.notes = info.notes;
    newEntry.kickstart = info.kickstart;
    newEntry.crc32 = info.crc32;
    newEntry.file = info.file;
    newEntry.bootable = info.bootable;
    // recog is empty for manually added entries (relies on CRC)

    m_entries.push_back(newEntry);
    Logger::Info("BrainFile: Added new entry '{}'", info.name);
    return true;
}

std::string BrainFile::CalculateCRC32(const std::vector<uint8_t>& sector0,
                                      const std::vector<uint8_t>& sector1)
{
    // Use standard CRC32 (IEEE 802.3 polynomial) to match brainfile.json checksums
    uint32_t crc = 0xFFFFFFFF;

    // Process sector0
    for (uint8_t byte : sector0) {
        crc = crc32Table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }

    // Process sector1
    for (uint8_t byte : sector1) {
        crc = crc32Table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }

    crc ^= 0xFFFFFFFF; // Final XOR

    // Convert to uppercase hex string
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", crc);
    return std::string(buf);
}

bool BrainFile::MatchRecognitionPattern(const std::string& recog,
                                        const std::vector<uint8_t>& sector0,
                                        const std::vector<uint8_t>& sector1)
{
    if (recog.empty()) {
        return false;
    }

    std::string pattern = recog;

    while (pattern.find(',') != std::string::npos) {
        // Extract offset
        size_t commaPos = pattern.find(',');
        if (commaPos == std::string::npos)
            break;

        int offset = std::stoi(pattern.substr(0, commaPos));
        pattern = pattern.substr(commaPos + 1);

        // Extract value
        commaPos = pattern.find(',');
        std::string valueStr =
            (commaPos != std::string::npos) ? pattern.substr(0, commaPos) : pattern;

        int value = std::stoi(valueStr);

        // Check byte at offset
        uint8_t actualValue;
        if (offset < 512) {
            if (static_cast<size_t>(offset) >= sector0.size())
                return false;
            actualValue = sector0[offset];
        } else {
            int sector1Offset = offset - 512;
            if (static_cast<size_t>(sector1Offset) >= sector1.size())
                return false;
            actualValue = sector1[sector1Offset];
        }

        if (actualValue != static_cast<uint8_t>(value)) {
            return false;
        }

        // Move to next pair
        if (commaPos != std::string::npos) {
            pattern = pattern.substr(commaPos + 1);
        } else {
            break;
        }
    }

    return true;
}

BootBlockInfo BrainFile::IdentifyBootBlock(const std::vector<uint8_t>& sector0,
                                           const std::vector<uint8_t>& sector1) const
{
    BootBlockInfo info;

    // Calculate CRC32 of bootblock (always do this so we have CRC even if not identified)
    std::string bootCrc = CalculateCRC32(sector0, sector1);
    info.crc32 = bootCrc;

    Logger::Info("BrainFile: Bootblock CRC32 = {}", bootCrc);

    if (!m_loaded || m_entries.empty()) {
        Logger::Warn("BrainFile: Database not loaded or empty");
        return info;
    }

    // Search entries - first try CRC match, then recognition pattern
    for (const auto& entry : m_entries) {
        bool matched = false;

        // Try CRC match first (more reliable)
        // Normalize both to uppercase for comparison
        // Check CRC (if present and non-zero in DB)
        // Some entries might not have CRC or it might be 0
        if (!entry.crc32.empty() && entry.crc32 != "0") {
            uint32_t entryCrc = 0;
            try {
                // CRC values in brainfile.json are hex strings (with or without 0x prefix)
                if (entry.crc32.size() > 2 && entry.crc32.substr(0, 2) == "0x") {
                    entryCrc = std::stoul(entry.crc32, nullptr, 16);
                } else {
                    // Try hex first (most CRC values are hex like "35685AEA")
                    entryCrc = std::stoul(entry.crc32, nullptr, 16);
                }
            } catch (...) {
                // Ignore parsing errors
            }

            if (entryCrc != 0) {
                // Parse bootCrc as hex to compare
                uint32_t calculatedCrc = std::stoul(bootCrc, nullptr, 16);

                if (calculatedCrc != entryCrc) {
                    continue; // CRC mismatch
                }
                // If CRC matches, we consider it a very strong match
                matched = true;
            }
        }
        // Try recognition pattern if no CRC match
        if (!matched && !entry.recog.empty() &&
            MatchRecognitionPattern(entry.recog, sector0, sector1)) {
            matched = true;
        }

        if (matched) {
            info.name = entry.name;
            info.classCode = entry.classCode;
            info.className = GetClassName(entry.classCode);
            info.notes = entry.notes;
            info.kickstart = entry.kickstart;
            info.file = entry.file;
            info.bootable = entry.bootable;
            info.identified = true;
            info.isVirus = (entry.classCode == "v");
            Logger::Info("BrainFile: Identified bootblock as '{}'", entry.name);
            return info;
        }
    }

    // Not identified - return info with CRC for reference
    Logger::Info("BrainFile: Bootblock not identified in database");
    return info;
}

BootBlockInfo BrainFile::FindByName(const std::string& name) const
{
    // 'name' is a library .bin file stem. The authoritative link is the stored
    // File path (the on-disk name may differ from the entry Name by case or by
    // filesystem-sanitised characters such as '/'), so match that first and only
    // fall back to the entry Name.
    auto fileStem = [](const std::string& path) -> std::string {
        size_t slash = path.find_last_of("/\\");
        std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
        if (base.size() >= 4) {
            std::string ext = base.substr(base.size() - 4);
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".bin")
                base.resize(base.size() - 4);
        }
        return base;
    };

    const BrainEntry* match = nullptr;
    for (const auto& entry : m_entries) {
        if (!entry.file.empty() && fileStem(entry.file) == name) {
            match = &entry;
            break;
        }
    }
    if (!match) {
        for (const auto& entry : m_entries) {
            if (entry.name == name) {
                match = &entry;
                break;
            }
        }
    }

    BootBlockInfo info;
    if (!match)
        return info; // identified == false

    info.name = match->name;
    info.classCode = match->classCode;
    info.className = GetClassName(match->classCode);
    info.notes = match->notes;
    info.kickstart = match->kickstart;
    info.file = match->file;
    info.bootable = match->bootable;
    info.crc32 = match->crc32;
    info.identified = true;
    info.isVirus = (match->classCode == "v");
    return info;
}

std::string BrainFile::GetMachineType(const std::string& kickstart)
{
    // Strip a leading "KS" (case-insensitive) and look at the major version digit.
    size_t i = 0;
    if (kickstart.size() >= 2 && (kickstart[0] == 'K' || kickstart[0] == 'k') &&
        (kickstart[1] == 'S' || kickstart[1] == 's')) {
        i = 2;
    }
    while (i < kickstart.size() && std::isspace(static_cast<unsigned char>(kickstart[i])))
        ++i;
    if (i >= kickstart.size())
        return "Any";

    switch (kickstart[i]) {
        case '1':
            return "A500";
        case '2':
            return "A600";
        case '3':
            return "A1200";
        default:
            return "Any";
    }
}

std::string BrainFile::GetClassName(const std::string& classCode)
{
    if (classCode == "s")
        return "Standard";
    if (classCode == "u")
        return "Utility";
    if (classCode == "v")
        return "Virus";
    if (classCode == "g")
        return "Boot Game";
    if (classCode == "bl")
        return "Loader";
    if (classCode == "l")
        return "Logo";
    if (classCode == "i")
        return "Intro";
    if (classCode == "p")
        return "Copy Protection";
    if (classCode == "xc")
        return "XCopy";
    if (classCode == "sc")
        return "Scroller";
    if (classCode == "bm")
        return "Bootmenu";
    if (classCode == "rs")
        return "Ram Switch";
    if (classCode == "vfm")
        return "Virus Free Memory";
    if (classCode == "ds")
        return "Demoscene";
    if (classCode == "ga")
        return "Game";
    if (classCode == "bom")
        return "Boot Message";

    return "Unknown";
}

uint32_t BrainFile::GetClassColor(const std::string& classCode)
{
    // IM_COL32(R, G, B, A) format
    if (classCode == "s")
        return 0xFF80FF80; // Green - ABGR format
    if (classCode == "u")
        return 0xFF80FF80; // Green
    if (classCode == "v")
        return 0xFF5050FF; // Red (virus!)
    if (classCode == "g")
        return 0xFF50FFFF; // Yellow
    if (classCode == "bl")
        return 0xFF50FFFF; // Yellow
    if (classCode == "l")
        return 0xFFFF80FF; // Purple
    if (classCode == "i")
        return 0xFF50FFFF; // Yellow
    if (classCode == "p")
        return 0xFF80FF80; // Green
    if (classCode == "xc")
        return 0xFF80FF80; // Green
    if (classCode == "sc")
        return 0xFF50FFFF; // Yellow
    if (classCode == "bm")
        return 0xFF80FF80; // Green
    if (classCode == "rs")
        return 0xFF80FF80; // Green
    if (classCode == "vfm")
        return 0xFF80FF80; // Green
    if (classCode == "ds")
        return 0xFF80FF80; // Green
    if (classCode == "ga")
        return 0xFF80FF80; // Green
    if (classCode == "bom")
        return 0xFF50FFFF; // Yellow

    return 0xFFAAAAAA; // Gray for unknown
}

} // namespace core
} // namespace adm
