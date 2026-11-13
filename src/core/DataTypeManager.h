#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace adm::core {

/**
 * @brief Describes a single identification rule (byte pattern at offset).
 */
struct DataTypeRule
{
    uint32_t offset;
    std::vector<uint8_t> bytes;
};

/**
 * @brief Describes a data type with its identification rules.
 */
struct DataTypeDescriptor
{
    std::string name;
    std::string group;
    std::vector<DataTypeRule> rules;
};

/**
 * @brief Result of a data type identification.
 */
struct DataTypeResult
{
    std::string name;
    std::string group;

    bool IsValid() const { return !name.empty(); }
};

/**
 * @brief Manages data type identification from JSON descriptors.
 *
 * Crawls the datatypes/ directory at startup and provides identification
 * based on byte patterns in file headers.
 */
class DataTypeManager
{
public:
    DataTypeManager();

    /**
     * @brief Loads all data type descriptors from the datatypes/ directory.
     */
    void LoadDescriptors();

    /**
     * @brief Identifies a file based on its header bytes.
     * @param data The file header data (up to 2KB to support formats like MOD).
     * @return DataTypeResult with name and group, or empty if unidentified.
     */
    [[nodiscard]] DataTypeResult Identify(std::span<const uint8_t> data) const;

    /**
     * @brief Gets the number of loaded data type descriptors.
     */
    [[nodiscard]] size_t GetDescriptorCount() const { return m_descriptors.size(); }

private:
    /**
     * @brief Loads a single JSON descriptor file.
     * @param path Path to the JSON file.
     * @return true if loaded successfully.
     */
    bool LoadDescriptor(const std::string& path);

    /**
     * @brief Checks if all rules match the given data.
     * @param descriptor The data type descriptor.
     * @param data The file header data.
     * @return true if all rules match.
     */
    [[nodiscard]] bool MatchesRules(const DataTypeDescriptor& descriptor,
                                    std::span<const uint8_t> data) const;

    /**
     * @brief Parses a hex string into bytes.
     * @param hex Hex string (e.g., "464F524D").
     * @return Vector of bytes.
     */
    static std::vector<uint8_t> ParseHexString(const std::string& hex);

    std::vector<DataTypeDescriptor> m_descriptors;
};

} // namespace adm::core
