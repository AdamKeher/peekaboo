#include "HostDirectoryDocument.h"
#include "Logger.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace adm::core {
namespace {

std::string PathUtf8(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string LowerAmiga(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch);
    });
    return value;
}

void HashBytes(uint64_t& hash, const std::string& value)
{
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
}

uint64_t FileTimeValue(std::filesystem::file_time_type value)
{
    return static_cast<uint64_t>(value.time_since_epoch().count());
}

bool ValidLeafName(const std::string& name)
{
    if (name.empty() || name == "." || name == "..")
        return false;
    return name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

} // namespace

DocumentEntryId DocumentEntryId::AdfBlock(uint32_t block)
{
    return {"adf:" + std::to_string(block)};
}

std::optional<DocumentEntryId> DocumentEntryId::RelativePath(const std::filesystem::path& path,
                                                             std::string& outError)
{
    if (!HostDirectoryDocument::IsSafeRelativePath(path)) {
        outError = "document entry path must be a confined relative path";
        return std::nullopt;
    }
    outError.clear();
    return DocumentEntryId{PathUtf8(path.lexically_normal())};
}

std::string DirectoryFingerprint::ToString() const
{
    return std::to_string(entryCount) + ":" + std::to_string(totalBytes) + ":" +
           std::to_string(newestWriteTime) + ":" + std::to_string(nameHash);
}

std::optional<DirectoryFingerprint> DirectoryFingerprint::Parse(const std::string& value)
{
    DirectoryFingerprint result;
    uint64_t* fields[] = {&result.entryCount, &result.totalBytes, &result.newestWriteTime,
                          &result.nameHash};
    size_t begin = 0;
    for (size_t index = 0; index < std::size(fields); ++index) {
        const size_t end = index + 1 == std::size(fields) ? value.size() : value.find(':', begin);
        if (end == std::string::npos || end == begin)
            return std::nullopt;
        const char* first = value.data() + begin;
        const char* last = value.data() + end;
        if (auto parsed = std::from_chars(first, last, *fields[index]);
            parsed.ec != std::errc{} || parsed.ptr != last)
            return std::nullopt;
        begin = end + 1;
    }
    return result;
}

HostDirectoryDocument::~HostDirectoryDocument()
{
    if (m_pendingScan.valid())
        m_pendingScan.wait();
}

bool HostDirectoryDocument::Open(const std::filesystem::path& path, bool readOnly,
                                 std::string& outError,
                                 std::optional<HostMountDevice> assignedDevice,
                                 std::string volumeLabel)
{
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(path, ec);
    if (ec || !std::filesystem::is_directory(canonical, ec) || ec) {
        outError = "host directory is missing or is not a directory";
        return false;
    }
    m_originalRoot = path;
    m_canonicalRoot = canonical;
    m_assignedDevice = assignedDevice;
    m_volumeLabel = std::move(volumeLabel);
    m_readOnly = readOnly;
    m_available = true;
    return RefreshNow(outError);
}

void HostDirectoryDocument::SetPlaceholder(const std::filesystem::path& originalPath,
                                           std::optional<HostMountDevice> assignedDevice,
                                           std::string volumeLabel, bool readOnly)
{
    m_originalRoot = originalPath;
    m_canonicalRoot.clear();
    m_assignedDevice = assignedDevice;
    m_volumeLabel = std::move(volumeLabel);
    m_readOnly = readOnly;
    m_available = false;
    m_scan = {};
    m_scan.error = "host directory is missing";
}

bool HostDirectoryDocument::Locate(const std::filesystem::path& path, std::string& outError)
{
    return Open(path, m_readOnly, outError, m_assignedDevice, m_volumeLabel);
}

DocumentCapabilities HostDirectoryDocument::GetCapabilities() const
{
    DocumentCapabilities result;
    result.browse = m_available;
    result.importFiles = m_available && !m_readOnly;
    result.createDirectory = m_available && !m_readOnly;
    result.rename = m_available && !m_readOnly;
    result.deleteEntries = m_available && !m_readOnly;
    result.writeBlocks = m_available && !m_readOnly;
    return result;
}

uint64_t HostDirectoryDocument::RequestRefresh()
{
    const uint64_t generation = ++m_requestedGeneration;
    if (!m_available)
        return generation;
    if (m_pendingScan.valid() &&
        m_pendingScan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return generation;
    if (m_pendingScan.valid())
        ApplyPendingRefresh();
    const auto root = m_canonicalRoot;
    m_pendingScan = std::async(std::launch::async,
                               [root, generation]() { return ScanDirectory(root, generation); });
    return generation;
}

bool HostDirectoryDocument::ApplyPendingRefresh()
{
    if (!m_pendingScan.valid() ||
        m_pendingScan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;
    HostDirectoryScan scan = m_pendingScan.get();
    if (scan.generation >= m_scan.generation) {
        if (!scan.error.empty() && m_available) {
            Logger::Warn("Host directory '{}' became unavailable: {}", m_canonicalRoot.string(),
                         scan.error);
            m_available = false;
        }
        m_scan = std::move(scan);
    }
    if (m_scan.generation < m_requestedGeneration)
        RequestRefresh();
    return true;
}

bool HostDirectoryDocument::RefreshNow(std::string& outError)
{
    if (!m_available) {
        outError = "host directory is unavailable";
        return false;
    }
    const uint64_t generation = ++m_requestedGeneration;
    auto scan = ScanDirectory(m_canonicalRoot, generation);
    if (!scan.error.empty()) {
        outError = scan.error;
        Logger::Warn("Host directory '{}' became unavailable: {}", m_canonicalRoot.string(),
                     scan.error);
        m_available = false;
        m_scan = std::move(scan);
        return false;
    }
    m_scan = std::move(scan);
    outError.clear();
    return true;
}

bool HostDirectoryDocument::IsSafeRelativePath(const std::filesystem::path& path)
{
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;
    for (const auto& part : path) {
        if (part == "..")
            return false;
    }
    return true;
}

bool HostDirectoryDocument::IsMetadataSidecar(const std::filesystem::path& path)
{
    const std::string name = LowerAmiga(PathUtf8(path.filename()));
    return name == "__uae___" || name == "_uaefsdb.___" || name == ".uaefsdb" ||
           name.ends_with(".uaem");
}

bool HostDirectoryDocument::IsWithinRoot(const std::filesystem::path& path) const
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        return false;
    auto rootIt = m_canonicalRoot.begin();
    auto pathIt = canonical.begin();
    for (; rootIt != m_canonicalRoot.end(); ++rootIt, ++pathIt) {
        if (pathIt == canonical.end())
            return false;
#ifdef _WIN32
        if (LowerAmiga(PathUtf8(*rootIt)) != LowerAmiga(PathUtf8(*pathIt)))
            return false;
#else
        if (*rootIt != *pathIt)
            return false;
#endif
    }
    return true;
}

std::optional<std::filesystem::path> HostDirectoryDocument::Resolve(const DocumentEntryId& id,
                                                                    std::string& outError) const
{
    const std::filesystem::path relative = std::filesystem::u8path(id.value);
    if (!m_available || !IsSafeRelativePath(relative)) {
        outError = "document entry is not a confined relative path";
        return std::nullopt;
    }
    const auto candidate = m_canonicalRoot / relative;
    if (!IsWithinRoot(candidate)) {
        outError = "document entry resolves outside the host directory";
        return std::nullopt;
    }
    outError.clear();
    return candidate;
}

bool HostDirectoryDocument::ReadFile(const DocumentEntryId& id, std::vector<uint8_t>& out,
                                     std::string& outError) const
{
    auto path = Resolve(id, outError);
    std::error_code ec;
    if (!path || !std::filesystem::is_regular_file(*path, ec) || ec) {
        if (outError.empty())
            outError = "document entry is not a regular file";
        return false;
    }
    std::ifstream stream(*path, std::ios::binary);
    if (!stream) {
        outError = "unable to open host file";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return static_cast<bool>(stream) || stream.eof();
}

bool HostDirectoryDocument::ReadFilePrefix(const DocumentEntryId& id, size_t maximumBytes,
                                           std::vector<uint8_t>& out, std::string& outError) const
{
    auto path = Resolve(id, outError);
    std::error_code ec;
    if (!path || !std::filesystem::is_regular_file(*path, ec) || ec) {
        if (outError.empty())
            outError = "document entry is not a regular file";
        return false;
    }
    std::ifstream stream(*path, std::ios::binary);
    if (!stream) {
        outError = "unable to open host file";
        return false;
    }
    out.resize(maximumBytes);
    stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(maximumBytes));
    out.resize(static_cast<size_t>(stream.gcount()));
    return static_cast<bool>(stream) || stream.eof();
}

bool HostDirectoryDocument::WriteFile(const DocumentEntryId& id, const std::vector<uint8_t>& data,
                                      std::string& outError)
{
    if (!RequireWritable(outError))
        return false;
    auto path = Resolve(id, outError);
    std::error_code ec;
    if (!path || !std::filesystem::is_regular_file(*path, ec) || ec) {
        if (outError.empty())
            outError = "document entry is not a regular file";
        return false;
    }
    std::ofstream stream(*path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        outError = "unable to open host file for writing";
        return false;
    }
    stream.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    if (!stream) {
        outError = "host file write failed";
        return false;
    }
    RequestRefresh();
    outError.clear();
    return true;
}

bool HostDirectoryDocument::CreateHostFile(const DocumentEntryId& parent, const std::string& name,
                                           const std::vector<uint8_t>& data, std::string& outError)
{
    if (!RequireWritable(outError))
        return false;
    auto destination = ResolveForCreate(parent, name, outError);
    if (!destination)
        return false;
    std::ofstream stream(*destination, std::ios::binary | std::ios::trunc);
    if (!stream) {
        outError = "unable to create host file";
        return false;
    }
    stream.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    if (!stream) {
        outError = "host file write failed";
        return false;
    }
    RequestRefresh();
    outError.clear();
    return true;
}

bool HostDirectoryDocument::RequireWritable(std::string& outError) const
{
    if (!m_available) {
        outError = "host directory is unavailable";
        return false;
    }
    if (m_readOnly) {
        outError = "host directory document is read-only";
        Logger::Warn("Blocked write to read-only host directory '{}'", m_canonicalRoot.string());
        return false;
    }
    return true;
}

std::optional<std::filesystem::path>
HostDirectoryDocument::ResolveForCreate(const DocumentEntryId& parent, const std::string& name,
                                        std::string& outError) const
{
    if (!ValidLeafName(name)) {
        outError = "entry name is empty or contains a path separator";
        return std::nullopt;
    }
    auto parentPath = Resolve(parent, outError);
    std::error_code ec;
    if (!parentPath || !std::filesystem::is_directory(*parentPath, ec) || ec) {
        if (outError.empty())
            outError = "destination parent is not a directory";
        return std::nullopt;
    }
    const auto candidate = *parentPath / std::filesystem::u8path(name);
    if (!IsWithinRoot(candidate.parent_path())) {
        outError = "destination resolves outside the host directory";
        return std::nullopt;
    }
    return candidate;
}

bool HostDirectoryDocument::Import(const DocumentEntryId& parent,
                                   const std::filesystem::path& source, std::string& outError)
{
    if (!RequireWritable(outError))
        return false;
    std::error_code ec;
    const auto sourceCanonical = std::filesystem::canonical(source, ec);
    if (ec || (!std::filesystem::is_regular_file(sourceCanonical, ec) &&
               !std::filesystem::is_directory(sourceCanonical, ec))) {
        outError = "import source is missing or unsupported";
        return false;
    }
    auto destination = ResolveForCreate(parent, PathUtf8(source.filename()), outError);
    if (!destination)
        return false;
    if (std::filesystem::is_directory(sourceCanonical, ec))
        std::filesystem::copy(sourceCanonical, *destination,
                              std::filesystem::copy_options::recursive, ec);
    else
        std::filesystem::copy_file(sourceCanonical, *destination,
                                   std::filesystem::copy_options::none, ec);
    if (ec) {
        outError = "host import failed: " + ec.message();
        return false;
    }
    RequestRefresh();
    return true;
}

bool HostDirectoryDocument::CreateDirectory(const DocumentEntryId& parent, const std::string& name,
                                            std::string& outError)
{
    if (!RequireWritable(outError))
        return false;
    auto destination = ResolveForCreate(parent, name, outError);
    if (!destination)
        return false;
    std::error_code ec;
    if (!std::filesystem::create_directory(*destination, ec) || ec) {
        outError = "unable to create host directory: " +
                   (ec ? ec.message() : std::string("entry already exists"));
        return false;
    }
    RequestRefresh();
    return true;
}

bool HostDirectoryDocument::Rename(const DocumentEntryId& id, const std::string& newName,
                                   std::string& outError)
{
    if (!RequireWritable(outError))
        return false;
    auto source = Resolve(id, outError);
    if (!source || *source == m_canonicalRoot)
        return false;
    std::string unused;
    auto parentId = DocumentEntryId::RelativePath(
        std::filesystem::relative(source->parent_path(), m_canonicalRoot), unused);
    auto destination = parentId ? ResolveForCreate(*parentId, newName, outError) : std::nullopt;
    if (!destination)
        return false;
    std::error_code ec;
    std::filesystem::rename(*source, *destination, ec);
    if (ec) {
        outError = "host rename failed: " + ec.message();
        return false;
    }
    RequestRefresh();
    return true;
}

bool HostDirectoryDocument::Move(const DocumentEntryId& id, const DocumentEntryId& newParent,
                                 std::string& outError)
{
    if (!RequireWritable(outError))
        return false;
    auto source = Resolve(id, outError);
    if (!source || *source == m_canonicalRoot)
        return false;
    auto destination = ResolveForCreate(newParent, PathUtf8(source->filename()), outError);
    if (!destination)
        return false;

    std::error_code ec;
    if (std::filesystem::is_directory(*source, ec) && !ec) {
        const auto relativeToSource =
            std::filesystem::relative(destination->parent_path(), *source, ec);
        if (!ec && IsSafeRelativePath(relativeToSource)) {
            outError = "a directory cannot be moved into itself";
            return false;
        }
        ec.clear();
    }
    std::filesystem::rename(*source, *destination, ec);
    if (ec) {
        outError = "host move failed: " + ec.message();
        return false;
    }
    RequestRefresh();
    return true;
}

bool HostDirectoryDocument::Remove(const DocumentEntryId& id, std::string& outError)
{
    if (!RequireWritable(outError))
        return false;
    auto path = Resolve(id, outError);
    if (!path || *path == m_canonicalRoot) {
        if (outError.empty())
            outError = "the document root cannot be deleted";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove_all(*path, ec);
    if (ec) {
        outError = "host delete failed: " + ec.message();
        return false;
    }
    RequestRefresh();
    return true;
}

HostDirectoryScan HostDirectoryDocument::ScanDirectory(const std::filesystem::path& root,
                                                       uint64_t generation)
{
    HostDirectoryScan result;
    result.generation = generation;
    std::error_code ec;
    const auto canonicalRoot = std::filesystem::canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonicalRoot, ec) || ec) {
        result.error = "host directory disappeared during refresh";
        return result;
    }

    const auto space = std::filesystem::space(canonicalRoot, ec);
    if (!ec) {
        result.capacityBytes = space.capacity;
        result.freeBytes = space.available;
    }
    ec.clear();

    std::unordered_map<std::string, std::vector<size_t>> namesByParent;
    std::filesystem::recursive_directory_iterator iterator(
        canonicalRoot, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && iterator != end) {
        const auto& item = *iterator;
        const auto relative = std::filesystem::relative(item.path(), canonicalRoot, ec);
        if (ec)
            break;
        if (IsMetadataSidecar(item.path())) {
            if (item.is_directory(ec))
                iterator.disable_recursion_pending();
            iterator.increment(ec);
            continue;
        }

        HostDirectoryEntry entry;
        entry.relativePath = relative.lexically_normal();
        std::string idError;
        auto id = DocumentEntryId::RelativePath(entry.relativePath, idError);
        auto parentId = DocumentEntryId::RelativePath(entry.relativePath.parent_path(), idError);
        if (!id || !parentId) {
            iterator.disable_recursion_pending();
            iterator.increment(ec);
            continue;
        }
        entry.id = std::move(*id);
        entry.parentId = std::move(*parentId);
        entry.name = PathUtf8(item.path().filename());
        const auto status = item.symlink_status(ec);
        if (ec)
            break;
        entry.isSymlink = std::filesystem::is_symlink(status);
        entry.isDirectory = std::filesystem::is_directory(status);
        if (entry.isSymlink) {
            iterator.disable_recursion_pending();
            const auto target = std::filesystem::weakly_canonical(item.path(), ec);
            if (ec) {
                entry.symlinkConfined = false;
                ec.clear();
            } else {
                auto rootIt = canonicalRoot.begin();
                auto targetIt = target.begin();
                for (; rootIt != canonicalRoot.end(); ++rootIt, ++targetIt) {
                    if (targetIt == target.end() || *rootIt != *targetIt) {
                        entry.symlinkConfined = false;
                        break;
                    }
                }
            }
        } else if (!entry.isDirectory) {
            entry.size = item.file_size(ec);
            if (ec) {
                entry.size = 0;
                ec.clear();
            }
        }
        entry.modified = item.last_write_time(ec);
        if (ec)
            ec.clear();

        result.fingerprint.entryCount++;
        result.fingerprint.totalBytes += entry.size;
        result.fingerprint.newestWriteTime =
            std::max(result.fingerprint.newestWriteTime, FileTimeValue(entry.modified));
        HashBytes(result.fingerprint.nameHash, entry.id.value);
        HashBytes(result.fingerprint.nameHash, std::to_string(entry.size));
        namesByParent[entry.parentId.value + "\n" + LowerAmiga(entry.name)].push_back(
            result.entries.size());
        result.entries.push_back(std::move(entry));
        iterator.increment(ec);
    }
    if (ec)
        result.error = "host directory refresh failed: " + ec.message();

    for (const auto& [key, indices] : namesByParent) {
        (void)key;
        if (indices.size() > 1)
            for (size_t index : indices)
                result.entries[index].amigaNameCollision = true;
    }
    std::sort(result.entries.begin(), result.entries.end(),
              [](const auto& left, const auto& right) {
                  if (left.parentId.value != right.parentId.value)
                      return left.parentId.value < right.parentId.value;
                  if (left.isDirectory != right.isDirectory)
                      return left.isDirectory > right.isDirectory;
                  return LowerAmiga(left.name) < LowerAmiga(right.name);
              });
    return result;
}

} // namespace adm::core
