#include "DataTypeManager.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "Logger.h"

namespace adm::core {

DataTypeManager::DataTypeManager() { LoadDescriptors(); }

void DataTypeManager::LoadDescriptors()
{
    m_descriptors.clear();

    // Search paths for datatypes directory
    std::vector<std::string> searchPaths = {"datatypes", "../../datatypes"};

    for (const auto& basePath : searchPaths) {
        if (!std::filesystem::exists(basePath) || !std::filesystem::is_directory(basePath)) {
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(basePath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                LoadDescriptor(entry.path().string());
            }
        }

        // If we found and loaded descriptors from this path, don't check others
        if (!m_descriptors.empty()) {
            Logger::Info("Loaded {} data type descriptors from {}", m_descriptors.size(), basePath);
            return;
        }
    }

    if (m_descriptors.empty()) {
        Logger::Warn("No data type descriptors found in datatypes/ directory");
    }
}

bool DataTypeManager::LoadDescriptor(const std::string& path)
{
    try {
        std::ifstream f(path);
        if (!f.is_open()) {
            return false;
        }

        nlohmann::json j = nlohmann::json::parse(f);

        DataTypeDescriptor desc;
        desc.name = j.value("name", "");
        desc.group = j.value("group", "");

        if (desc.name.empty()) {
            Logger::Warn("Data type descriptor {} missing 'name' field", path);
            return false;
        }

        if (j.contains("rules") && j["rules"].is_array()) {
            for (const auto& rule : j["rules"]) {
                DataTypeRule r;
                r.offset = rule.value("offset", 0u);

                std::string hexBytes = rule.value("bytes", "");
                if (!hexBytes.empty()) {
                    r.bytes = ParseHexString(hexBytes);
                }

                if (!r.bytes.empty()) {
                    desc.rules.push_back(std::move(r));
                }
            }
        }

        if (desc.rules.empty()) {
            Logger::Warn("Data type descriptor {} has no valid rules", path);
            return false;
        }

        m_descriptors.push_back(std::move(desc));
        return true;

    } catch (const std::exception& e) {
        Logger::Error("Failed to load data type descriptor {}: {}", path, e.what());
        return false;
    }
}

DataTypeResult DataTypeManager::Identify(std::span<const uint8_t> data) const
{
    for (const auto& desc : m_descriptors) {
        if (MatchesRules(desc, data)) {
            return {desc.name, desc.group};
        }
    }
    return {};
}

bool DataTypeManager::MatchesRules(const DataTypeDescriptor& descriptor,
                                   std::span<const uint8_t> data) const
{
    for (const auto& rule : descriptor.rules) {
        // Check if we have enough data to match this rule
        if (rule.offset + rule.bytes.size() > data.size()) {
            return false;
        }

        // Compare bytes
        for (size_t i = 0; i < rule.bytes.size(); ++i) {
            if (data[rule.offset + i] != rule.bytes[i]) {
                return false;
            }
        }
    }
    return true;
}

std::vector<uint8_t> DataTypeManager::ParseHexString(const std::string& hex)
{
    std::vector<uint8_t> bytes;

    // Process pairs of hex characters
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        std::string byteStr = hex.substr(i, 2);
        try {
            uint8_t byte = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
            bytes.push_back(byte);
        } catch (...) {
            // Invalid hex, skip
        }
    }

    return bytes;
}

} // namespace adm::core
