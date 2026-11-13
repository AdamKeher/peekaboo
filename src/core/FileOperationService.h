#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// windows.h (via glad) #defines CreateDirectory; undef so the method name below is preserved.
#ifdef _WIN32
#undef CreateDirectory
#endif

namespace adm {
namespace core {

class AdfHandler;
struct FileNode;

/**
 * @brief Encapsulates file-level operations on the ADF disk (import, export, delete, etc.).
 *
 * Extracted from Application to reduce its size and separate concerns.
 * Communicates results via callbacks.
 */
class FileOperationService
{
public:
    explicit FileOperationService(AdfHandler& handler);

    void SetUpdateCallback(std::function<void(bool)> cb) { m_updateCallback = std::move(cb); }
    void SetErrorCallback(std::function<void(const std::string&)> cb)
    {
        m_errorCallback = std::move(cb);
    }

    void ImportFiles(uint32_t destSector);
    void CreateDirectory(uint32_t parentSector, const std::string& name);
    void DeleteFiles(const std::vector<std::pair<uint32_t, std::string>>& items);
    void ScrubFiles(const std::vector<std::pair<uint32_t, std::string>>& items);
    void ExportFiles(const std::vector<FileNode>& nodes);
    void ClearBlock(uint32_t block);
    void SaveBlocks(const std::set<uint32_t>& blocks);
    void InsertBinary(uint32_t startBlock);

private:
    AdfHandler& m_handler;
    std::function<void(bool)> m_updateCallback;
    std::function<void(const std::string&)> m_errorCallback;
};

} // namespace core
} // namespace adm
