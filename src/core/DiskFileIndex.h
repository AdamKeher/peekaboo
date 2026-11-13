#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "AdfWrapper.h"

namespace adm::core {

// A flattened filesystem entry with the full path FileNode does not carry.
struct DiskFileEntry
{
    std::string name;
    std::string path; // "s/startup-sequence" — volume-relative, '/' separated
    uint32_t headerBlock = 0;
    uint32_t parentSector = 0;
    uint32_t size = 0;
    bool isDir = false;
    bool isDeleted = false;
    int depth = 0;
    int32_t protection = 0;
    std::string comment;
    std::string modified; // "YYYY-MM-DD HH:MM:SS", empty when the date is unset
    std::string dataType;
    std::string dataTypeGroup;
};

/**
 * @brief Path index over a mounted ADF's file tree.
 *
 * AdfHandler::GetFileTree() gives a tree of FileNode, and every disk API keys off
 * a header block — but nothing in between knows a file's path. This builds that
 * mapping once so a caller can ask for "s/startup-sequence" by name, and so a raw
 * block hit can be reported as the file it belongs to.
 */
class DiskFileIndex
{
public:
    // Flattens `root` (the value AdfHandler::GetFileTree() returns) in
    // depth-first order. `deleted` may be AdfHandler::GetDeletedFiles(); those
    // entries have no parent path so they are listed at the top level.
    void Build(const FileNode& root, const std::vector<FileNode>& deleted = {});
    void Clear();

    const std::vector<DiskFileEntry>& Entries() const { return m_entries; }
    bool Empty() const { return m_entries.empty(); }

    // Path lookup. Matching is case-insensitive (AmigaDOS is), and accepts
    // '/' or ':' separators plus an optional leading separator or volume name.
    const DiskFileEntry* FindByPath(const std::string& path) const;
    const DiskFileEntry* FindByHeader(uint32_t headerBlock) const;

    // Normalises a user-supplied path to the form stored in `path`.
    static std::string NormalisePath(const std::string& path);

private:
    void AddNode(const FileNode& node, const std::string& parentPath, int depth);

    std::vector<DiskFileEntry> m_entries;
};

} // namespace adm::core
