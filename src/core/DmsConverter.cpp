#include "DmsConverter.h"
#include "Logger.h"

// xdms headers
extern "C" {
#include "cdata.h"
#include "pfile.h"
}

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace adm::core {

bool DmsConverter::ConvertToAdf(const std::string& dmsPath, const std::string& outputAdfPath)
{
    Logger::Info("Converting DMS '{}' to ADF '{}'", dmsPath, outputAdfPath);

    std::vector<char> inFile(dmsPath.begin(), dmsPath.end());
    inFile.push_back('\0');

    std::vector<char> outFile(outputAdfPath.begin(), outputAdfPath.end());
    outFile.push_back('\0');

    USHORT ret = Process_File(inFile.data(), outFile.data(), CMD_UNPACK, OPT_QUIET, 0, 0);

    if (ret != NO_PROBLEM) {
        Logger::Error("DMS conversion failed with error code: {}", ret);
        return false;
    }

    return true;
}

std::optional<std::vector<uint8_t>> DmsConverter::ConvertToBuffer(const std::string& dmsPath)
{
    Logger::Info("Decompressing DMS '{}' to memory buffer", dmsPath);

    // Decompress via Process_File to a temp file, then read into buffer
    auto tempPath = std::filesystem::temp_directory_path() / "adm_dms_temp.adf";
    std::string tempStr = tempPath.string();

    std::vector<char> inFile(dmsPath.begin(), dmsPath.end());
    inFile.push_back('\0');

    std::vector<char> outFile(tempStr.begin(), tempStr.end());
    outFile.push_back('\0');

    USHORT ret = Process_File(inFile.data(), outFile.data(), CMD_UNPACK, OPT_QUIET, 0, 0);

    if (ret != NO_PROBLEM) {
        Logger::Error("DMS decompression failed with error code: {}", ret);
        std::filesystem::remove(tempPath);
        return std::nullopt;
    }

    // Read temp ADF into buffer
    std::ifstream file(tempPath, std::ios::binary | std::ios::ate);
    if (!file) {
        Logger::Error("Failed to read temp ADF after DMS decompression");
        std::filesystem::remove(tempPath);
        return std::nullopt;
    }

    auto size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    file.close();

    // Clean up temp file
    std::filesystem::remove(tempPath);

    Logger::Info("DMS decompressed to {} byte buffer", buffer.size());
    return buffer;
}

std::optional<std::vector<uint8_t>>
DmsConverter::ConvertToBuffer(const std::vector<uint8_t>& dmsData)
{
    Logger::Info("Decompressing DMS data buffer ({} bytes) to memory buffer", dmsData.size());

    // Write DMS data to a temp file
    auto tempInPath = std::filesystem::temp_directory_path() / "adm_dms_in.dms";
    std::ofstream inFile(tempInPath, std::ios::binary);
    if (!inFile) {
        Logger::Error("Failed to create temporary input file for DMS decompression");
        return std::nullopt;
    }
    inFile.write(reinterpret_cast<const char*>(dmsData.data()), dmsData.size());
    inFile.close();

    // Call the file-based decompression
    auto result = ConvertToBuffer(tempInPath.string());

    // Clean up temp input file
    std::filesystem::remove(tempInPath);

    return result;
}

} // namespace adm::core
