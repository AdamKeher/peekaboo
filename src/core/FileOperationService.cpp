#include "FileOperationService.h"
#include "AdfWrapper.h"
#include "Logger.h"

#include <fmt/core.h>
#include <nfd.h>
#include <fstream>

namespace adm {
namespace core {

namespace {

// Logs the standard cancel/error tail for an NFD result that was not NFD_OKAY.
void LogNfdTail(nfdresult_t result, const char* action)
{
    if (result == NFD_CANCEL)
        Logger::Info("{} cancelled.", action);
    else
        Logger::Error("NFD Error: {}", NFD_GetError());
}

// Runs an "open file" dialog and, on success, invokes fn(path) then frees the path.
// Logs the cancel/error tail otherwise.
void WithOpenDialog(const char* action, const std::function<void(const char*)>& fn)
{
    nfdchar_t* path = nullptr;
    nfdresult_t result = NFD_OpenDialog(&path, nullptr, 0, nullptr);
    if (result == NFD_OKAY) {
        fn(path);
        NFD_FreePath(path);
    } else {
        LogNfdTail(result, action);
    }
}

// Runs a "save file" dialog and, on success, invokes fn(path) then frees the path.
void WithSaveDialog(const char* action, const char* defaultName,
                    const std::function<void(const char*)>& fn)
{
    nfdchar_t* path = nullptr;
    nfdresult_t result = NFD_SaveDialog(&path, nullptr, 0, nullptr, defaultName);
    if (result == NFD_OKAY) {
        fn(path);
        NFD_FreePath(path);
    } else {
        LogNfdTail(result, action);
    }
}

// Runs a "pick folder" dialog and, on success, invokes fn(path) then frees the path.
void WithPickFolder(const char* action, const std::function<void(const char*)>& fn)
{
    nfdchar_t* path = nullptr;
    nfdresult_t result = NFD_PickFolder(&path, nullptr);
    if (result == NFD_OKAY) {
        fn(path);
        NFD_FreePath(path);
    } else {
        LogNfdTail(result, action);
    }
}

} // namespace

FileOperationService::FileOperationService(AdfHandler& handler) : m_handler(handler) {}

void FileOperationService::ImportFiles(uint32_t destSector)
{
    const nfdpathset_t* pathSet;
    nfdresult_t result = NFD_OpenDialogMultiple(&pathSet, nullptr, 0, nullptr);

    if (result == NFD_OKAY) {
        nfdpathsetsize_t count;
        NFD_PathSet_GetCount(pathSet, &count);

        // Pre-calculate space requirements
        std::vector<std::string> paths;
        for (size_t i = 0; i < count; ++i) {
            nfdchar_t* path;
            NFD_PathSet_GetPath(pathSet, i, &path);
            paths.push_back(path);
        }

        uint32_t requiredBlocks = m_handler.CalculateRequiredBlocks(paths);
        uint32_t freeBlocks = m_handler.GetFreeBlocks();

        if (requiredBlocks > freeBlocks) {
            std::string msg =
                AdfHandler::InsufficientSpaceMessage("Import", requiredBlocks, freeBlocks);
            Logger::Error("Import cancelled: {}", msg);
            if (m_errorCallback)
                m_errorCallback(msg);
            NFD_PathSet_Free(pathSet);
            return;
        }

        bool anyFailed = false;
        for (size_t i = 0; i < count; ++i) {
            nfdchar_t* path;
            NFD_PathSet_GetPath(pathSet, i, &path);
            if (m_handler.ImportFile(destSector, path)) {
                Logger::Info("Imported: {}", path);
            } else {
                Logger::Error("Failed to import: {}", path);
                if (m_errorCallback)
                    m_errorCallback(fmt::format(
                        "Import failed for file:\n{}\n\nPartial file was removed. Process stopped.",
                        path));
                anyFailed = true;
                break;
            }
        }
        NFD_PathSet_Free(pathSet);

        // Refresh the views even on partial failure so imported files show up. preserveNav keeps
        // the hex editor where it was (the bool is a nav hint, not a success flag).
        (void)anyFailed;
        if (m_updateCallback)
            m_updateCallback(/*preserveNav=*/true);
    } else {
        LogNfdTail(result, "Import");
    }
}

void FileOperationService::CreateDirectory(uint32_t parentSector, const std::string& name)
{
    if (m_handler.CreateDirectory(parentSector, name)) {
        Logger::Info("Created directory: {}", name);
        if (m_updateCallback)
            m_updateCallback(false);
    } else {
        Logger::Error("Failed to create directory: {}", name);
    }
}

void FileOperationService::DeleteFiles(const std::vector<std::pair<uint32_t, std::string>>& items)
{
    int successCount = 0;
    for (const auto& item : items) {
        if (m_handler.DeleteFile(item.first, item.second)) {
            Logger::Info("Deleted: {}", item.second);
            successCount++;
        } else {
            Logger::Error("Failed to delete: {}", item.second);
        }
    }

    if (successCount > 0 && m_updateCallback) {
        m_updateCallback(false);
    }
}

void FileOperationService::ScrubFiles(const std::vector<std::pair<uint32_t, std::string>>& items)
{
    int successCount = 0;
    for (const auto& item : items) {
        if (m_handler.ScrubFile(item.first)) {
            Logger::Info("Scrubbed: {}", item.second);
            successCount++;
        } else {
            Logger::Error("Failed to scrub: {}", item.second);
        }
    }

    if (successCount > 0 && m_updateCallback) {
        m_updateCallback(true);
    }
}

void FileOperationService::ExportFiles(const std::vector<FileNode>& nodes)
{
    if (nodes.empty()) {
        return;
    }

    WithPickFolder("Export", [&](const char* outPath) {
        auto exportResult = m_handler.ExportNodes(nodes, outPath, true);

        if (exportResult.errors == 0) {
            if (exportResult.filesExported == 1 && exportResult.directoriesCreated == 0) {
                Logger::Info("Exported 1 file to: {}", outPath);
            } else {
                Logger::Info("Exported {} files and {} directories to: {}",
                             exportResult.filesExported, exportResult.directoriesCreated, outPath);
            }
        } else {
            Logger::Warn("Export completed with {} errors. {} files exported.", exportResult.errors,
                         exportResult.filesExported);

            std::string errorSummary =
                fmt::format("Export completed with {} error(s):\n\n", exportResult.errors);
            int maxErrors = std::min(static_cast<int>(exportResult.errorMessages.size()), 5);
            for (int i = 0; i < maxErrors; ++i) {
                errorSummary += exportResult.errorMessages[i] + "\n";
            }
            if (exportResult.errorMessages.size() > 5) {
                errorSummary += fmt::format("\n... and {} more errors.", exportResult.errors - 5);
            }

            if (m_errorCallback)
                m_errorCallback(errorSummary);
        }
    });
}

void FileOperationService::ClearBlock(uint32_t block)
{
    if (m_handler.ClearBlock(block)) {
        Logger::Info("Block {} cleared (zeroed).", block);
        if (m_updateCallback)
            m_updateCallback(true);
    } else {
        Logger::Error("Failed to clear block {}.", block);
    }
}

void FileOperationService::SaveBlocks(const std::set<uint32_t>& blocks)
{
    if (blocks.empty())
        return;

    WithSaveDialog("Save", "blocks.bin", [&](const char* outPath) {
        std::ofstream outFile(outPath, std::ios::binary);
        if (outFile) {
            for (uint32_t block : blocks) {
                auto data = m_handler.ReadBlock(block);
                if (!data.empty()) {
                    outFile.write(reinterpret_cast<const char*>(data.data()), data.size());
                }
            }
            Logger::Info("Saved {} blocks ({} bytes) to: {}", blocks.size(), blocks.size() * 512,
                         outPath);
        } else {
            Logger::Error("Failed to write to file: {}", outPath);
        }
    });
}

void FileOperationService::InsertBinary(uint32_t startBlock)
{
    WithOpenDialog("Insert", [&](const char* inPath) {
        std::ifstream inFile(inPath, std::ios::binary | std::ios::ate);
        if (inFile) {
            std::streamsize fileSize = inFile.tellg();
            inFile.seekg(0, std::ios::beg);

            if (fileSize <= 0) {
                std::string errorMsg = "The selected file is empty — nothing to insert.";
                if (m_errorCallback)
                    m_errorCallback(errorMsg);
                Logger::Error("Insert aborted: file is empty or unreadable: {}", inPath);
                return;
            }

            uint32_t blocksNeeded = (static_cast<uint64_t>(fileSize) + 511) / 512;
            uint32_t totalBlocks = m_handler.GetTotalBlocks();

            if (startBlock + blocksNeeded > totalBlocks) {
                std::string errorMsg =
                    fmt::format("File too large!\n\n"
                                "The file needs {} blocks ({} bytes),\n"
                                "but only {} blocks are available\n"
                                "starting from block {}.",
                                blocksNeeded, fileSize, totalBlocks - startBlock, startBlock);
                if (m_errorCallback)
                    m_errorCallback(errorMsg);
                Logger::Error("File too large: needs {} blocks, but only {} blocks available "
                              "from block {}",
                              blocksNeeded, totalBlocks - startBlock, startBlock);
                return;
            }

            std::vector<uint8_t> buffer(512, 0);
            uint32_t blocksWritten = 0;

            for (uint32_t i = 0; i < blocksNeeded; ++i) {
                std::fill(buffer.begin(), buffer.end(), 0);
                inFile.read(reinterpret_cast<char*>(buffer.data()), 512);

                if (m_handler.WriteBlock(startBlock + i, buffer)) {
                    blocksWritten++;
                } else {
                    Logger::Error("Failed to write block {}", startBlock + i);
                    break;
                }
            }

            Logger::Info("Inserted {} bytes ({} blocks) starting at block {}", fileSize,
                         blocksWritten, startBlock);

            if (m_updateCallback)
                m_updateCallback(true);
        } else {
            Logger::Error("Failed to open file: {}", inPath);
        }
    });
}

} // namespace core
} // namespace adm
