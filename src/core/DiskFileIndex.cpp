#include "DiskFileIndex.h"

#include <algorithm>
#include <cctype>

#include <fmt/core.h>

namespace adm::core {

namespace {

std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string FormatDate(const DateTime& d)
{
    if (d.year <= 0)
        return {};
    return fmt::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}", d.year, d.month, d.day, d.hour,
                       d.min, d.sec);
}

} // namespace

std::string DiskFileIndex::NormalisePath(const std::string& path)
{
    std::string out;
    out.reserve(path.size());
    for (char c : path)
        out.push_back(c == '\\' ? '/' : c);

    // A leading "Volume:" or ":" both mean "from the root".
    const size_t colon = out.find(':');
    if (colon != std::string::npos)
        out.erase(0, colon + 1);
    while (!out.empty() && out.front() == '/')
        out.erase(0, 1);
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    return out;
}

void DiskFileIndex::Clear() { m_entries.clear(); }

void DiskFileIndex::AddNode(const FileNode& node, const std::string& parentPath, int depth)
{
    DiskFileEntry e;
    e.name = node.name;
    e.path = parentPath.empty() ? node.name : parentPath + "/" + node.name;
    e.headerBlock = static_cast<uint32_t>(node.startSector);
    e.parentSector = static_cast<uint32_t>(node.parentSector);
    e.size = node.size > 0 ? static_cast<uint32_t>(node.size) : 0;
    e.isDir = node.isDir;
    e.isDeleted = node.isDeleted;
    e.depth = depth;
    e.protection = node.access;
    e.comment = node.comment;
    e.modified = FormatDate(node.modified);
    e.dataType = node.dataType;
    e.dataTypeGroup = node.dataTypeGroup;
    const std::string path = e.path;
    m_entries.push_back(std::move(e));

    for (const auto& child : node.children)
        AddNode(child, path, depth + 1);
}

void DiskFileIndex::Build(const FileNode& root, const std::vector<FileNode>& deleted)
{
    m_entries.clear();
    // The root node is the volume itself, so its children start at the top level
    // rather than under a directory named after the disk.
    for (const auto& child : root.children)
        AddNode(child, {}, 0);

    // Deleted entries have no surviving parent chain, so they are listed by name
    // at the top level. GetDeletedFiles already sets isDeleted on each of them.
    for (const auto& node : deleted)
        AddNode(node, {}, 0);
}

const DiskFileEntry* DiskFileIndex::FindByPath(const std::string& path) const
{
    const std::string want = ToLower(NormalisePath(path));
    if (want.empty())
        return nullptr;
    for (const auto& e : m_entries)
        if (!e.isDeleted && ToLower(e.path) == want)
            return &e;
    // Fall back to deleted entries so a caller can still reach a scrubbed file
    // by name once the live tree has been checked.
    for (const auto& e : m_entries)
        if (ToLower(e.path) == want)
            return &e;
    return nullptr;
}

const DiskFileEntry* DiskFileIndex::FindByHeader(uint32_t headerBlock) const
{
    if (headerBlock == 0)
        return nullptr;
    for (const auto& e : m_entries)
        if (e.headerBlock == headerBlock)
            return &e;
    return nullptr;
}

} // namespace adm::core
