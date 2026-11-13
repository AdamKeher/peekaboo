#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace adm::core {

class DmsConverter
{
public:
    /**
     * @brief Converts a DMS file to an ADF file.
     * @param dmsPath Absolute path to the .dms file.
     * @param outputAdfPath Absolute path to the output .adf file.
     * @return true if successful.
     */
    static bool ConvertToAdf(const std::string& dmsPath, const std::string& outputAdfPath);

    /**
     * @brief Decompresses a DMS file into an in-memory ADF buffer.
     * @param dmsPath Absolute path to the .dms file.
     * @return The ADF data as a byte vector, or std::nullopt on failure.
     */
    static std::optional<std::vector<uint8_t>> ConvertToBuffer(const std::string& dmsPath);

    /**
     * @brief Decompresses DMS data into an in-memory ADF buffer.
     * @param dmsData The DMS file content.
     * @return The ADF data as a byte vector, or std::nullopt on failure.
     */
    static std::optional<std::vector<uint8_t>> ConvertToBuffer(const std::vector<uint8_t>& dmsData);
};

} // namespace adm::core
