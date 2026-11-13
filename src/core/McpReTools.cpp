#include "McpReTools.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <fmt/format.h>

#include "AdfWrapper.h"
#include "DiskFileIndex.h"
#include "DiskSearcher.h"
#include "HexUtil.h"
#include "HunkParser.h"
#include "McpToolHandler.h"
#include "MemoryAnnotations.h"
#include "SegListResolver.h"

namespace adm::core {

using nlohmann::json;

namespace {

// --- shared argument plumbing ----------------------------------------------

json Err(const std::string& msg) { return json{{"error", msg}}; }

json Unavailable(const std::string& hint) { return json{{"available", false}, {"hint", hint}}; }

std::string Hex(uint32_t v, int width = 8) { return fmt::format("${:0{}X}", v, width); }

// Accepts a JSON number, or a string parsed as hex ("0x1234", "$1234", "1234").
bool ParseAddr(const json& j, uint32_t& out)
{
    if (j.is_number_integer() || j.is_number_unsigned()) {
        out = j.get<uint32_t>();
        return true;
    }
    if (j.is_string()) {
        if (auto v = ParseHexAddress(j.get<std::string>())) {
            out = *v;
            return true;
        }
    }
    return false;
}

bool GetAddr(const json& args, const char* key, uint32_t& out)
{
    return args.contains(key) && ParseAddr(args[key], out);
}

// Distinguishes an absent addr-style argument from one that was supplied but didn't parse — a
// malformed value reported as a missing one sends the caller looking at the wrong thing.
json AddrArgError(const json& args, const char* key)
{
    if (!args.contains(key) || args[key].is_null())
        return Err(fmt::format("{} is required", key));
    return Err(fmt::format("could not parse {} {} — expected an integer or a hex string like "
                           "\"0x1234\" / \"$1234\"",
                           key, args[key].dump()));
}

json AddrSchema(const char* description)
{
    return json{{"type", json::array({"integer", "string"})}, {"description", description}};
}

json IntSchema(const char* description)
{
    return json{{"type", "integer"}, {"description", description}};
}

json BoolSchema(const char* description)
{
    return json{{"type", "boolean"}, {"description", description}};
}

json StrSchema(const char* description)
{
    return json{{"type", "string"}, {"description", description}};
}

json EnumSchema(const char* description, std::vector<std::string> values)
{
    return json{{"type", "string"}, {"enum", values}, {"description", description}};
}

// Parses [{addr, len}, ...] or ["$100:$300", ...] into ranges.
bool ParseRanges(const json& j, std::vector<MemoryRange>& out, std::string& err)
{
    if (!j.is_array()) {
        err = "ranges must be an array of {addr, len} objects";
        return false;
    }
    for (const auto& item : j) {
        MemoryRange r;
        if (!item.is_object() || !ParseAddr(item.value("addr", json{}), r.addr)) {
            err = "each range needs an addr";
            return false;
        }
        uint32_t len = 0;
        if (item.contains("len") && ParseAddr(item["len"], len))
            r.len = len;
        else {
            err = "each range needs a len";
            return false;
        }
        if (r.len == 0) {
            err = "range length must be at least 1";
            return false;
        }
        out.push_back(r);
    }
    return true;
}

std::vector<uint8_t> ParsePattern(const json& args, std::string& err)
{
    if (args.contains("text") && args["text"].is_string()) {
        const auto s = args["text"].get<std::string>();
        return {s.begin(), s.end()};
    }
    if (args.contains("pattern") && args["pattern"].is_string()) {
        auto bytes = DiskSearcher::ParseHex(args["pattern"].get<std::string>());
        if (bytes.empty())
            err = "pattern must be hex digit pairs, e.g. \"4E75\"";
        return bytes;
    }
    err = "pass either text (an ASCII string) or pattern (hex digit pairs)";
    return {};
}

std::string HexDump(const std::vector<uint8_t>& bytes)
{
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes)
        out += fmt::format("{:02X}", b);
    return out;
}

const char* kBase64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(const std::vector<uint8_t>& data)
{
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kBase64Chars[(n >> 18) & 63]);
        out.push_back(kBase64Chars[(n >> 12) & 63]);
        out.push_back(kBase64Chars[(n >> 6) & 63]);
        out.push_back(kBase64Chars[n & 63]);
    }
    if (i < data.size()) {
        uint32_t n = data[i] << 16;
        const bool two = (i + 1 < data.size());
        if (two)
            n |= data[i + 1] << 8;
        out.push_back(kBase64Chars[(n >> 18) & 63]);
        out.push_back(kBase64Chars[(n >> 12) & 63]);
        out.push_back(two ? kBase64Chars[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

// Renders bytes as text with non-printables escaped, so a caller can read a
// startup-sequence without decoding base64 first.
std::string AsText(const std::vector<uint8_t>& bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (uint8_t b : bytes) {
        if (b == '\n' || b == '\r' || b == '\t' || (b >= 32 && b < 127))
            out.push_back(static_cast<char>(b));
        else
            out += fmt::format("\\x{:02X}", b);
    }
    return out;
}

// The AR console's ROM test, reused so "is this in ROM?" means the same thing
// everywhere.
bool LooksLikeRom(const std::string& name)
{
    return name.find("Kickstart") != std::string::npos || name.find("ROM") != std::string::npos;
}

// --- snapshot_memory / diff_snapshots ---------------------------------------

json SnapshotInfoToJson(const MemorySnapshotInfo& info)
{
    json j{{"name", info.name},
           {"frame", info.frame},
           {"created", info.created},
           {"bytes", info.bytes},
           {"regionCount", info.regionCount}};
    json regions = json::array();
    for (const auto& r : info.regions)
        regions.push_back({{"addr", Hex(r.addr)}, {"len", r.len}});
    j["regions"] = regions;
    return j;
}

json HandleSnapshotMemory(const json& args, const McpHost& host)
{
    if (!host.memorySnapshots || !host.liveMemoryRegions)
        return Unavailable("Memory snapshots need a running emulator.");

    const auto name = args.value("name", std::string{});
    std::vector<MemoryRange> ranges;
    std::string err;
    if (args.contains("ranges") && !ParseRanges(args["ranges"], ranges, err))
        return Err(err);

    const uint64_t frame = host.getFrameCount ? host.getFrameCount() : 0;
    if (!host.memorySnapshots->Capture(name, ranges, host.liveMemoryRegions(), frame,
                                       args.value("replace", true), err))
        return Err(err);

    auto info = host.memorySnapshots->Info(name);
    json j = SnapshotInfoToJson(*info);
    j["ok"] = true;
    return j;
}

json HandleDiffSnapshots(const json& args, const McpHost& host)
{
    if (!host.memorySnapshots)
        return Unavailable("Memory snapshots need a running emulator.");

    MemoryDiffOptions opts;
    const auto mode = args.value("mode", std::string{"changed"});
    if (mode == "same")
        opts.wantSame = true;
    else if (mode != "changed")
        return Err("mode must be \"changed\" or \"same\"");

    opts.stableWith = args.value("stableWith", std::string{});
    opts.minRun = args.value("minRun", 1u);
    opts.maxRun = args.value("maxRun", 0u);
    opts.limit = args.value("limit", 256);
    opts.includeBytes = args.value("includeBytes", true);
    std::string err;
    if (args.contains("exclude") && !ParseRanges(args["exclude"], opts.exclude, err))
        return Err(err);

    const auto a = args.value("a", std::string{});
    const auto b = args.value("b", std::string{});
    auto res = host.memorySnapshots->Diff(a, b, opts);
    if (!res.ok)
        return Err(res.error);

    json runs = json::array();
    for (const auto& r : res.runs) {
        json jr{{"addr", Hex(r.addr)}, {"len", r.len}};
        if (!r.before.empty())
            jr["before"] = HexDump(r.before);
        if (!r.after.empty())
            jr["after"] = HexDump(r.after);
        runs.push_back(std::move(jr));
    }
    return json{{"available", true},
                {"a", a},
                {"b", b},
                {"mode", mode},
                {"stableWith", opts.stableWith},
                {"totalRuns", res.totalRuns},
                {"bytesMatching", res.bytesMatching},
                {"truncated", res.truncated},
                {"runs", runs}};
}

json HandleListMemorySnapshots(const McpHost& host)
{
    if (!host.memorySnapshots)
        return Unavailable("Memory snapshots need a running emulator.");
    json list = json::array();
    for (const auto& info : host.memorySnapshots->List())
        list.push_back(SnapshotInfoToJson(info));
    return json{{"available", true}, {"count", list.size()}, {"snapshots", list}};
}

json HandleDeleteMemorySnapshot(const json& args, const McpHost& host)
{
    if (!host.memorySnapshots)
        return Unavailable("Memory snapshots need a running emulator.");
    const auto name = args.value("name", std::string{});
    if (!host.memorySnapshots->Remove(name))
        return Err("no snapshot named '" + name + "'");
    return json{{"ok", true}, {"name", name}};
}

// --- describe_address --------------------------------------------------------

json HandleDescribeAddress(const json& args, const McpHost& host)
{
    uint32_t addr = 0;
    if (!GetAddr(args, "addr", addr))
        return AddrArgError(args, "addr");
    if (!host.execReaders || !host.memoryRegions)
        return Unavailable("Address description needs a running emulator.");

    AddressDescriber::Context ctx;
    ctx.readers = host.execReaders();
    ctx.regions = host.memoryRegions();
    ctx.customRegName = host.customRegName;
    ctx.customRegDesc = host.customRegDesc;
    ctx.resolveSymbol = host.resolveSymbol;
    ctx.inspectOs = args.value("inspectOs", true);
    if (host.annotations) {
        auto* ann = host.annotations;
        ctx.annotationLabel = [ann](uint32_t a) {
            return ann->GetLabel(a).value_or(std::string{});
        };
        ctx.annotationComment = [ann](uint32_t a) {
            return ann->GetComment(a).value_or(std::string{});
        };
    }

    auto d = AddressDescriber::Describe(addr, ctx);
    json layers = json::array();
    for (const auto& l : d.layers) {
        json jl{{"kind", l.kind}, {"name", l.name}, {"offset", l.offset}};
        if (!l.detail.empty())
            jl["detail"] = l.detail;
        if (l.base != 0 || l.size != 0) {
            jl["base"] = Hex(l.base);
            if (l.size != 0)
                jl["size"] = l.size;
        }
        layers.push_back(std::move(jl));
    }
    return json{{"available", true},
                {"addr", Hex(addr)},
                {"mapped", d.mapped},
                {"layerCount", layers.size()},
                {"layers", layers}};
}

// --- list_segments / map_file_to_memory -------------------------------------

std::vector<AmigaSegment> ResolveSegments(const McpHost& host)
{
    if (!host.execReaders)
        return {};
    auto readers = host.execReaders();
    if (!readers.readLong || !readers.readByte)
        return {};
    return SegListResolver::ResolveActiveProcess(readers.readLong, readers.readByte);
}

json SymbolsToJson(const McpHost& host)
{
    DebugSymbolsInfo info;
    if (host.debugSymbolsInfo)
        info = host.debugSymbolsInfo();
    return json{
        {"loaded", info.loaded}, {"path", info.path}, {"functionCount", info.functionCount}};
}

json HandleListSegments(const McpHost& host)
{
    auto segments = ResolveSegments(host);
    if (segments.empty())
        return Unavailable("No AmigaDOS process is running, or its segment list is unreadable. "
                           "Run a program from a booted disk and try again.");

    json arr = json::array();
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& s = segments[i];
        arr.push_back({{"index", i},
                       {"node", Hex(s.base - 4)},
                       {"base", Hex(s.base)},
                       {"size", s.size},
                       {"end", Hex(s.base + s.size)}});
    }
    return json{{"available", true},
                {"programBase", Hex(segments.front().base)},
                {"segmentCount", segments.size()},
                {"segments", arr},
                {"symbols", SymbolsToJson(host)}};
}

json HandleMapFileToMemory(const json& args, const McpHost& host)
{
    std::string path = args.value("path", std::string{});
    if (path.empty() && host.activeDocumentPath)
        path = host.activeDocumentPath();
    if (path.empty())
        return Err("path is required (no document is open to default to)");

    std::ifstream f(path, std::ios::binary);
    if (!f)
        return Err("cannot open '" + path + "'");
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (!HunkParser::IsAmigaExecutable(data))
        return Err("'" + path +
                   "' is not an AmigaDOS executable (a hunk file). Point this at the "
                   "program, not at the disk image it lives on.");

    auto parsed = HunkParser::Parse(data);
    auto segments = ResolveSegments(host);

    json hunks = json::array();
    for (size_t i = 0; i < parsed.hunks.size(); ++i) {
        const auto& h = parsed.hunks[i];
        json jh{{"index", i},
                {"type", HunkParser::GetHunkTypeName(h.type)},
                {"memoryFlags", HunkParser::GetMemoryFlagsString(h.memoryFlags)},
                {"fileOffset", h.dataOffset},
                {"fileSize", h.size},
                {"allocSize", h.allocSize}};
        // Hunks and resolved segments pair up positionally: AmigaDOS allocates
        // one segment per hunk, in file order.
        if (i < segments.size()) {
            jh["base"] = Hex(segments[i].base);
            jh["size"] = segments[i].size;
            jh["end"] = Hex(segments[i].base + segments[i].size);
        }
        hunks.push_back(std::move(jh));
    }

    json result{{"available", true},           {"path", path},   {"hunkCount", parsed.hunks.size()},
                {"loaded", !segments.empty()}, {"hunks", hunks}, {"symbols", SymbolsToJson(host)}};

    // Optional single-value translation, both directions.
    uint32_t addr = 0;
    if (GetAddr(args, "addr", addr)) {
        json t{{"addr", Hex(addr)}, {"found", false}};
        for (size_t i = 0; i < segments.size() && i < parsed.hunks.size(); ++i) {
            const auto& s = segments[i];
            if (addr < s.base || addr - s.base >= s.size)
                continue;
            t["found"] = true;
            t["hunk"] = i;
            t["hunkOffset"] = addr - s.base;
            t["fileOffset"] = parsed.hunks[i].dataOffset + (addr - s.base);
            break;
        }
        result["addressToFile"] = t;
    }
    uint32_t offset = 0;
    if (GetAddr(args, "offset", offset)) {
        json t{{"fileOffset", offset}, {"found", false}};
        for (size_t i = 0; i < segments.size() && i < parsed.hunks.size(); ++i) {
            const auto& h = parsed.hunks[i];
            if (offset < h.dataOffset || offset - h.dataOffset >= h.size)
                continue;
            t["found"] = true;
            t["hunk"] = i;
            t["hunkOffset"] = offset - h.dataOffset;
            t["addr"] = Hex(segments[i].base + (offset - h.dataOffset));
            break;
        }
        result["fileToAddress"] = t;
    }
    return result;
}

// --- ADF filesystem ----------------------------------------------------------

// Builds the path index for the mounted disk. Returns false with a hint when
// there is nothing mounted.
bool OpenDisk(const McpHost& host, DiskDocumentAccess& out, DiskFileIndex& index, json& fail)
{
    if (!host.activeDiskDocument) {
        fail = Unavailable("No disk document is open.");
        return false;
    }
    out = host.activeDiskDocument();
    if (!out.volume || !out.volume->IsMounted()) {
        fail = Unavailable("No disk document is open. Load one with load_document first.");
        return false;
    }
    if (!out.volume->HasFileSystem()) {
        fail = Unavailable("The mounted disk has no AmigaDOS filesystem (it is NDOS or a custom "
                           "format). Use read_disk_sectors / find_disk_bytes instead.");
        return false;
    }
    index.Build(out.volume->GetFileTree(), out.volume->GetDeletedFiles());
    return true;
}

json EntryToJson(const DiskFileEntry& e)
{
    json j{{"name", e.name},
           {"path", e.path},
           {"headerBlock", e.headerBlock},
           {"parentSector", e.parentSector},
           {"size", e.size},
           {"isDir", e.isDir},
           {"depth", e.depth}};
    if (e.isDeleted)
        j["deleted"] = true;
    if (!e.comment.empty())
        j["comment"] = e.comment;
    if (!e.modified.empty())
        j["modified"] = e.modified;
    if (!e.dataType.empty()) {
        j["dataType"] = e.dataType;
        j["dataTypeGroup"] = e.dataTypeGroup;
    }
    j["protection"] = e.protection;
    return j;
}

json HandleListDiskFiles(const json& args, const McpHost& host)
{
    DiskDocumentAccess disk;
    DiskFileIndex index;
    json fail;
    if (!OpenDisk(host, disk, index, fail))
        return fail;

    const auto prefix = DiskFileIndex::NormalisePath(args.value("path", std::string{}));
    const bool recursive = args.value("recursive", true);
    const bool includeDeleted = args.value("includeDeleted", false);
    const int limit = args.value("limit", 500);

    json files = json::array();
    uint32_t total = 0;
    bool truncated = false;
    for (const auto& e : index.Entries()) {
        if (e.isDeleted && !includeDeleted)
            continue;
        if (!prefix.empty()) {
            // Everything under "s" — the directory itself is not a result.
            if (e.path.size() <= prefix.size() || e.path.compare(0, prefix.size(), prefix) != 0 ||
                e.path[prefix.size()] != '/')
                continue;
        }
        if (!recursive) {
            const int wantDepth =
                prefix.empty()
                    ? 0
                    : static_cast<int>(std::count(prefix.begin(), prefix.end(), '/')) + 1;
            if (e.depth != wantDepth)
                continue;
        }
        ++total;
        if (limit > 0 && static_cast<int>(files.size()) >= limit) {
            truncated = true;
            continue;
        }
        files.push_back(EntryToJson(e));
    }

    return json{{"available", true},   {"volume", disk.volumeName}, {"image", disk.path},
                {"totalFiles", total}, {"truncated", truncated},    {"files", files}};
}

json HandleReadDiskFile(const json& args, const McpHost& host)
{
    DiskDocumentAccess disk;
    DiskFileIndex index;
    json fail;
    if (!OpenDisk(host, disk, index, fail))
        return fail;

    const DiskFileEntry* entry = nullptr;
    uint32_t headerBlock = 0;
    if (GetAddr(args, "headerBlock", headerBlock))
        entry = index.FindByHeader(headerBlock);
    else if (args.contains("path"))
        entry = index.FindByPath(args["path"].get<std::string>());
    else
        return Err("pass either path or headerBlock");
    if (!entry)
        return Err("no such file on the mounted disk");
    if (entry->isDir)
        return Err("'" + entry->path + "' is a directory");

    const std::string content = disk.volume->ReadFileContent(entry->headerBlock);
    std::vector<uint8_t> bytes(content.begin(), content.end());

    const uint32_t offset = args.value("offset", 0u);
    if (offset >= bytes.size())
        bytes.clear();
    else
        bytes.erase(bytes.begin(), bytes.begin() + offset);
    const uint32_t length = args.value("length", 0u);
    bool truncated = false;
    if (length > 0 && bytes.size() > length) {
        bytes.resize(length);
        truncated = true;
    }

    const auto encoding = args.value("encoding", std::string{"text"});
    json j{{"available", true},          {"path", entry->path}, {"headerBlock", entry->headerBlock},
           {"fileSize", content.size()}, {"offset", offset},    {"returnedBytes", bytes.size()},
           {"truncated", truncated},     {"encoding", encoding}};

    if (encoding == "hex")
        j["data"] = HexDump(bytes);
    else if (encoding == "base64")
        j["data"] = Base64(bytes);
    else if (encoding == "text")
        j["data"] = AsText(bytes);
    else
        return Err("encoding must be text, hex or base64");

    json blocks = json::array();
    for (uint32_t b : disk.volume->GetFileBlocks(entry->headerBlock))
        blocks.push_back(b);
    j["blocks"] = blocks;
    return j;
}

json HandleFindDiskBytes(const json& args, const McpHost& host)
{
    DiskDocumentAccess disk;
    DiskFileIndex index;
    json fail;
    if (!host.activeDiskDocument)
        return Unavailable("No disk document is open.");
    disk = host.activeDiskDocument();
    if (!disk.volume || !disk.volume->IsMounted())
        return Unavailable("No disk document is open. Load one with load_document first.");
    if (!disk.searcher)
        return Unavailable("The open document has no searcher attached.");

    std::string err;
    auto pattern = ParsePattern(args, err);
    if (pattern.empty())
        return Err(err);

    const bool caseSensitive = args.value("caseSensitive", true);
    const int limit = args.value("limit", 100);

    // Owner resolution needs both the path index and the block-to-header map,
    // and the latter only exists once GetAllBlockTypes has run.
    const bool wantOwners = args.value("resolveOwners", true) && disk.volume->HasFileSystem();
    if (wantOwners) {
        index.Build(disk.volume->GetFileTree(), disk.volume->GetDeletedFiles());
        disk.volume->GetAllBlockTypes();
    }

    auto hits = disk.searcher->FindAll(pattern, caseSensitive, DiskSearcher::SearchType::ASCII);

    json results = json::array();
    bool truncated = false;
    for (const auto& h : hits) {
        if (limit > 0 && static_cast<int>(results.size()) >= limit) {
            truncated = true;
            break;
        }
        json jr{{"block", h.block},
                {"offset", h.offset},
                {"length", h.length},
                {"imageOffset", static_cast<uint64_t>(h.block) * 512 + h.offset}};
        if (!h.context.empty())
            jr["context"] = h.context;
        if (wantOwners) {
            auto details = disk.volume->AnalyzeBlock(h.block, disk.volume->ReadBlock(h.block));
            jr["blockType"] = details.typeLabel;
            jr["blockFree"] = disk.volume->IsBlockFree(h.block);
            const uint32_t owner =
                details.fileHeader != 0 ? details.fileHeader : (details.isDataBlock ? 0 : h.block);
            if (const auto* e = index.FindByHeader(owner))
                jr["ownerPath"] = e->path;
            else
                jr["ownerPath"] = nullptr; // unowned: a spare or free sector
            jr["ownerHeaderBlock"] = owner;
        }
        results.push_back(std::move(jr));
    }

    return json{{"available", true},
                {"volume", disk.volumeName},
                {"totalMatches", hits.size()},
                {"truncated", truncated},
                {"matches", results}};
}

json HandleWhichFileOwns(const json& args, const McpHost& host)
{
    DiskDocumentAccess disk;
    DiskFileIndex index;
    json fail;
    if (!OpenDisk(host, disk, index, fail))
        return fail;

    uint32_t block = 0;
    if (!GetAddr(args, "block", block)) {
        uint32_t offset = 0;
        if (!GetAddr(args, "offset", offset))
            return Err("pass either block (a 512-byte block number) or offset (a byte offset into "
                       "the image)");
        block = offset / 512;
    }
    if (block >= disk.volume->GetTotalBlocks())
        return Err(fmt::format("block {} is past the end of the image ({} blocks)", block,
                               disk.volume->GetTotalBlocks()));

    // Populates the block-to-header map that AnalyzeBlock consults for FFS data
    // blocks, which carry no back-pointer of their own.
    disk.volume->GetAllBlockTypes();
    auto details = disk.volume->AnalyzeBlock(block, disk.volume->ReadBlock(block));

    json j{{"available", true},
           {"block", block},
           {"imageOffset", static_cast<uint64_t>(block) * 512},
           {"blockType", details.typeLabel},
           {"free", disk.volume->IsBlockFree(block)},
           {"isDataBlock", details.isDataBlock},
           {"isBootBlock", details.isBootBlock},
           {"isBitmapBlock", details.isBitmapBlock}};

    const uint32_t owner = details.fileHeader;
    if (owner != 0) {
        j["ownerHeaderBlock"] = owner;
        if (const auto* e = index.FindByHeader(owner)) {
            j["ownerPath"] = e->path;
            j["ownerSize"] = e->size;
            j["ownerDeleted"] = e->isDeleted;
        }
    } else if (const auto* self = index.FindByHeader(block)) {
        // The block is a file header in its own right.
        j["ownerHeaderBlock"] = block;
        j["ownerPath"] = self->path;
        j["ownerSize"] = self->size;
    } else {
        j["ownerPath"] = nullptr;
        j["note"] = "No file claims this block — it is free space, filesystem metadata, or a "
                    "spare sector something wrote to directly.";
    }
    return j;
}

// --- residents / reboot survivors -------------------------------------------

std::vector<ResidentScanner::Region> ScannerRegions(const McpHost& host)
{
    std::vector<ResidentScanner::Region> out;
    if (!host.memoryRegions)
        return out;
    for (const auto& r : host.memoryRegions())
        out.push_back({r.base, r.size, r.name, LooksLikeRom(r.name)});
    return out;
}

json ResidentToJson(const ResidentModule& m)
{
    json j{{"addr", Hex(m.addr)},   {"valid", m.valid},
           {"name", m.name},        {"type", m.typeName},
           {"typeCode", m.type},    {"version", m.version},
           {"pri", m.pri},          {"flags", fmt::format("${:02X}", m.flags)},
           {"init", Hex(m.initFn)}, {"endSkip", Hex(m.endSkip)}};
    if (m.indirect)
        j["indirect"] = true;
    if (!m.idString.empty())
        j["idString"] = m.idString;
    if (!m.valid)
        j["note"] = "matchword/matchtag do not check out — this pointer is not a real RomTag";
    return j;
}

json HandleListResidentModules(const McpHost& host)
{
    if (!host.execReaders)
        return Unavailable("Resident modules need a running emulator.");
    auto readers = host.execReaders();
    auto execBase = exec::ExecBase(readers);
    if (!execBase)
        return Unavailable("ExecBase unavailable — no AmigaOS is booted.");

    auto mods = ResidentScanner::ListResidentModules(readers, *execBase);
    json arr = json::array();
    int injected = 0;
    for (const auto& m : mods) {
        if (m.indirect)
            ++injected;
        arr.push_back(ResidentToJson(m));
    }
    return json{{"available", true},
                {"execBase", Hex(*execBase)},
                {"count", mods.size()},
                {"reachedByRedirect", injected},
                {"modules", arr}};
}

json HandleGetRebootSurvivors(const McpHost& host)
{
    if (!host.execReaders)
        return Unavailable("Reset survivors need a running emulator.");
    auto readers = host.execReaders();
    auto execBase = exec::ExecBase(readers);
    auto s =
        ResidentScanner::ScanRebootSurvivors(readers, execBase.value_or(0), ScannerRegions(host));
    if (!s.available)
        return Unavailable(s.hint);

    json captures = json::array();
    for (const auto& c : s.captures)
        captures.push_back({{"name", c.name},
                            {"field", Hex(c.addr)},
                            {"value", Hex(c.value)},
                            {"set", c.set},
                            {"pointsToRom", c.pointsToRom}});

    json tags = json::array();
    for (const auto& t : s.kickTags)
        tags.push_back(ResidentToJson(t));

    json mem = json::array();
    for (const auto& [addr, bytes] : s.kickMemEntries)
        mem.push_back({{"addr", Hex(addr)}, {"bytes", bytes}});

    json stolen = json::array();
    for (const auto& sm : s.stolenMemory)
        stolen.push_back({{"memHeader", Hex(sm.memHeader)},
                          {"name", sm.name},
                          {"upper", Hex(sm.upper)},
                          {"regionEnd", Hex(sm.regionEnd)},
                          {"stolenBytes", sm.stolenBytes}});

    return json{{"available", true},
                {"execBase", Hex(s.execBase)},
                {"anySurvivors", s.AnySurvivors()},
                {"captures", captures},
                {"kickMemPtr", Hex(s.kickMemPtr)},
                {"kickTagPtr", Hex(s.kickTagPtr)},
                {"kickCheckSum", Hex(s.kickCheckSum)},
                {"pointerSumCheckSum", Hex(s.pointerSumCheckSum)},
                {"matchesPointerSum", s.matchesPointerSum},
                {"kickTags", tags},
                {"kickMemEntries", mem},
                {"stolenMemory", stolen}};
}

// --- CPU trace history -------------------------------------------------------

json TraceRecordToJson(const CpuTraceMatch& m, bool includeRegs)
{
    const auto& r = m.record;
    json j{{"seq", r.seq},   {"frame", r.frame}, {"cycle", r.cycle}, {"lRef", r.lRef},
           {"vpos", r.vpos}, {"hpos", r.hpos},   {"pc", Hex(r.pc)}};
    std::string raw;
    for (int i = 0; i < r.rawLen; ++i)
        raw += fmt::format("{:02X}", r.raw[i]);
    j["bytes"] = raw;
    if (m.haveAccess)
        j["access"] = {{"addr", Hex(m.access.addr)},
                       {"size", m.access.size},
                       {"read", m.access.read},
                       {"write", m.access.write}};
    if (includeRegs) {
        json d = json::array(), a = json::array();
        for (int i = 0; i < 8; ++i) {
            d.push_back(Hex(r.d[i]));
            a.push_back(Hex(r.a[i]));
        }
        j["d"] = d;
        j["a"] = a;
        j["sr"] = fmt::format("${:04X}", r.sr);
    }
    return j;
}

json TraceSnapshotToJson(const CpuTraceSnapshot& snap, bool includeRegs)
{
    if (!snap.available)
        return Unavailable(snap.hint);
    json matches = json::array();
    for (const auto& m : snap.matches)
        matches.push_back(TraceRecordToJson(m, includeRegs));
    return json{{"available", true},
                {"recording", snap.enabled},
                {"historyRecords", snap.totalRecords},
                {"historyFrames", snap.frames},
                {"oldestSeq", snap.oldestSeq},
                {"newestSeq", snap.newestSeq},
                {"dropped", snap.dropped},
                {"totalMatches", snap.totalMatches},
                {"truncated", snap.truncated},
                {"matches", matches}};
}

json HandleConfigureTraceHistory(const json& args, const McpHost& host)
{
    if (!host.cpuTraceHistory)
        return Unavailable("Trace history needs a running emulator.");
    auto& h = *host.cpuTraceHistory;
    if (args.contains("maxRecords")) {
        const auto n = args["maxRecords"].get<int64_t>();
        if (n < 1000 || n > 5000000)
            return Err("maxRecords must be between 1000 and 5000000");
        h.SetCapacity(static_cast<size_t>(n));
    }
    if (args.contains("clear") && args["clear"].get<bool>())
        h.Clear();
    if (args.contains("enabled"))
        h.SetEnabled(args["enabled"].get<bool>());
    return json{{"ok", true},
                {"enabled", h.IsEnabled()},
                {"maxRecords", h.Capacity()},
                {"records", h.Size()},
                {"frames", h.FrameCount()},
                {"note", "Recording also needs the per-instruction CPU trace itself: call "
                         "set_cpu_trace with enabled:true."}};
}

json HandleGetCpuTrace(const json& args, const McpHost& host)
{
    if (!host.cpuTraceHistory)
        return Unavailable("Trace history needs a running emulator.");
    CpuTraceQuery q;
    q.limit = args.value("limit", 64);
    q.startSeq = args.value("startSeq", 0ull);
    q.newestFirst = args.value("newestFirst", false);
    if (args.contains("frame")) {
        q.haveFrame = true;
        q.frame = args["frame"].get<uint64_t>();
    }
    return TraceSnapshotToJson(host.cpuTraceHistory->Query(q), args.value("includeRegs", false));
}

json HandleFindInTrace(const json& args, const McpHost& host)
{
    if (!host.cpuTraceHistory)
        return Unavailable("Trace history needs a running emulator.");

    const auto mode = args.value("mode", std::string{"filter"});
    CpuTraceQuery q;
    if (mode == "filter")
        q.mode = TraceQueryMode::Filter;
    else if (mode == "accesses")
        q.mode = TraceQueryMode::Accesses;
    else if (mode == "lastWriter")
        q.mode = TraceQueryMode::LastWriter;
    else
        return Err("mode must be filter, accesses or lastWriter");

    if (q.mode != TraceQueryMode::Filter) {
        if (!args.contains("addr") || args["addr"].is_null())
            return Err(mode + " needs an addr");
        if (!GetAddr(args, "addr", q.addr))
            return AddrArgError(args, "addr");
        uint32_t len = 1;
        GetAddr(args, "len", len);
        q.len = len == 0 ? 1 : len;
        q.readsOnly = args.value("readsOnly", false);
        q.writesOnly = args.value("writesOnly", false);
        if (q.readsOnly && q.writesOnly)
            return Err("readsOnly and writesOnly are mutually exclusive");
    }

    uint32_t pcLo = 0, pcHi = 0;
    if (GetAddr(args, "pcLo", pcLo) && GetAddr(args, "pcHi", pcHi)) {
        if (pcHi <= pcLo)
            return Err("pcHi must be above pcLo");
        q.havePcRange = true;
        q.pcLo = pcLo;
        q.pcHi = pcHi;
    } else if (args.contains("pcLo") != args.contains("pcHi")) {
        return Err("pcLo and pcHi must be given together");
    }

    if (args.contains("frame")) {
        q.haveFrame = true;
        q.frame = args["frame"].get<uint64_t>();
    }
    if (args.contains("reg")) {
        q.reg = args["reg"].get<std::string>();
        if (!args.contains("regValue") || args["regValue"].is_null())
            return Err("reg needs a regValue to compare against");
        if (!GetAddr(args, "regValue", q.regValue))
            return AddrArgError(args, "regValue");
    }
    q.beforeSeq = args.value("beforeSeq", 0ull);
    q.limit = args.value("limit", 64);
    q.newestFirst = args.value("newestFirst", q.mode != TraceQueryMode::Filter);

    json j = TraceSnapshotToJson(host.cpuTraceHistory->Query(q), args.value("includeRegs", false));
    if (j.value("available", false))
        j["mode"] = mode;
    return j;
}

// --- library call tracing / DOS breakpoints ---------------------------------

json ValueToJson(const LibraryCallValue& v)
{
    json j{{"name", v.name}, {"reg", v.reg}, {"value", Hex(v.value)}};
    if (!v.text.empty())
        j["text"] = v.text;
    return j;
}

json HandleTraceLibraryCalls(const json& args, const McpHost& host)
{
    if (!host.libraryTracer || !host.cpuTraceHistory || !host.execReaders)
        return Unavailable("Library call tracing needs a running emulator.");

    auto readers = host.execReaders();
    auto execBase = exec::ExecBase(readers);

    LibraryCallQuery q;
    q.library = args.value("library", std::string{});
    if (args.contains("functions") && args["functions"].is_array())
        for (const auto& f : args["functions"])
            if (f.is_string())
                q.functions.push_back(f.get<std::string>());
    q.sinceSeq = args.value("sinceSeq", 0ull);
    q.limit = args.value("limit", 64);

    auto snap = host.libraryTracer->Trace(*host.cpuTraceHistory, readers, execBase.value_or(0), q);
    if (!snap.available)
        return Unavailable(snap.hint);

    json calls = json::array();
    for (const auto& c : snap.calls) {
        json jc{{"seq", c.seq},
                {"frame", c.frame},
                {"lRef", c.lRef},
                {"library", c.library},
                {"function", c.function},
                {"lvo", c.lvo},
                {"entryPc", Hex(c.entryPc)},
                {"returnsTo", Hex(c.callerPc)}};
        json jargs = json::array();
        for (const auto& a : c.args)
            jargs.push_back(ValueToJson(a));
        jc["args"] = jargs;
        if (c.haveResult)
            jc["result"] = ValueToJson(c.result);
        calls.push_back(std::move(jc));
    }
    return json{{"available", true},
                {"librariesScanned", snap.librariesScanned},
                {"totalCalls", snap.totalCalls},
                {"truncated", snap.truncated},
                {"calls", calls}};
}

json DosRuleToJson(const DosBreakRule& r)
{
    json j{{"id", r.id},         {"library", r.library},      {"function", r.function},
           {"lvo", r.lvo},       {"entryPc", Hex(r.entryPc)}, {"hits", r.hits},
           {"breaks", r.breaks}, {"oneShot", r.oneShot}};
    if (!r.nameMatch.empty()) {
        j["nameMatch"] = r.nameMatch;
        j["argReg"] = r.argReg;
        j["caseSensitive"] = r.caseSensitive;
    }
    if (!r.lastValue.empty())
        j["lastValue"] = r.lastValue;
    return j;
}

json HandleAddDosBreakpoint(const json& args, const McpHost& host)
{
    if (!host.dosBreakpoints || !host.libraryTracer || !host.execReaders || !host.addBreakpoint)
        return Unavailable("DOS breakpoints need a running emulator.");

    const auto function = args.value("function", std::string{});
    if (function.empty())
        return Err("function is required, e.g. \"Open\"");
    const auto library = args.value("library", std::string{"dos.library"});

    // Validate the arguments before touching the machine, so a typo reports the
    // typo rather than whatever the lookup happened to fail on.
    DosNameMatch matchMode = DosNameMatch::Contains;
    if (!DosBreakpoints::ParseMatchMode(args.value("matchMode", std::string{"contains"}),
                                        matchMode))
        return Err("matchMode must be contains, equals, prefix or suffix");

    auto readers = host.execReaders();
    auto execBase = exec::ExecBase(readers);
    if (!execBase)
        return Err("ExecBase unavailable — no AmigaOS is booted yet, so there is no jump table to "
                   "break on. Boot first, then set the breakpoint.");

    uint32_t lvo = 0;
    auto entry = host.libraryTracer->ResolveEntry(readers, *execBase, library, function, &lvo);
    if (!entry)
        return Err(fmt::format("cannot resolve {}/{}() — either the library is not open or the "
                               "function name is unknown",
                               library, function));

    DosBreakRule rule;
    rule.library = library;
    rule.function = function;
    rule.lvo = lvo;
    rule.entryPc = *entry;
    rule.oneShot = args.value("oneShot", false);
    rule.caseSensitive = args.value("caseSensitive", false);
    rule.nameMatch = args.value("nameMatch", std::string{});
    rule.match = matchMode;

    if (!rule.nameMatch.empty()) {
        // The predicate needs to know which register carries the string, which
        // comes from the argument metadata.
        if (const LvoSpec* spec = host.libraryTracer->FindSpec(library, lvo)) {
            for (const auto& a : spec->args)
                if (a.type == LvoArgType::CStr || a.type == LvoArgType::BStr) {
                    rule.argReg = a.reg;
                    rule.argType = a.type;
                    break;
                }
        }
    }

    std::string err;
    const int id = host.dosBreakpoints->Add(rule, err);
    if (id == 0)
        return Err(err);
    if (!host.addBreakpoint(*entry)) {
        host.dosBreakpoints->Remove(id);
        return Err("could not set a breakpoint at " + Hex(*entry));
    }
    if (host.onStateChanged)
        host.onStateChanged();

    json j = DosRuleToJson(host.dosBreakpoints->List().back());
    j["ok"] = true;
    if (!rule.nameMatch.empty() && rule.argReg.empty())
        j["warning"] = "no string argument is known for this function, so nameMatch was ignored";
    return j;
}

json HandleRemoveDosBreakpoint(const json& args, const McpHost& host)
{
    if (!host.dosBreakpoints)
        return Unavailable("DOS breakpoints need a running emulator.");
    const int id = args.value("id", 0);
    uint32_t entryPc = 0;
    for (const auto& r : host.dosBreakpoints->List())
        if (r.id == id)
            entryPc = r.entryPc;
    if (!host.dosBreakpoints->Remove(id))
        return Err(fmt::format("no DOS breakpoint with id {}", id));
    if (host.removeBreakpoint && entryPc)
        host.removeBreakpoint(entryPc);
    if (host.onStateChanged)
        host.onStateChanged();
    return json{{"ok", true}, {"id", id}};
}

json HandleListDosBreakpoints(const McpHost& host)
{
    if (!host.dosBreakpoints)
        return Unavailable("DOS breakpoints need a running emulator.");
    json arr = json::array();
    for (const auto& r : host.dosBreakpoints->List())
        arr.push_back(DosRuleToJson(r));
    return json{{"available", true}, {"count", arr.size()}, {"breakpoints", arr}};
}

// --- manifest ----------------------------------------------------------------

const std::vector<std::string> kToolNames = {
    "snapshot_memory",       "diff_snapshots",
    "list_memory_snapshots", "delete_memory_snapshot",
    "describe_address",      "list_segments",
    "map_file_to_memory",    "list_disk_files",
    "read_disk_file",        "find_disk_bytes",
    "which_file_owns",       "list_resident_modules",
    "get_reboot_survivors",  "configure_cpu_trace_history",
    "get_cpu_trace",         "find_in_trace",
    "trace_library_calls",   "add_dos_breakpoint",
    "remove_dos_breakpoint", "list_dos_breakpoints",
    "resolve_source_line",   "line_coverage",
};

} // namespace

const std::vector<std::string>& McpReToolNames() { return kToolNames; }

void AppendMcpReToolManifest(json& tools)
{
    auto tool = [&](const char* name, const std::string& desc, json properties = json::object(),
                    json required = json::array()) {
        // A call site that writes `{}` for a parameterless tool hands us a null, not an empty
        // object (nlohmann reads an empty initializer list as null). MCP clients validate
        // inputSchema.properties as a record and reject the whole tools/list over one null, so
        // normalise here rather than trusting every call site to spell it json::object().
        if (!properties.is_object())
            properties = json::object();
        json schema = {{"type", "object"}, {"properties", properties}};
        if (!required.empty())
            schema["required"] = required;
        tools.push_back({{"name", name}, {"description", desc}, {"inputSchema", schema}});
    };

    const json rangeArraySchema = {
        {"type", "array"},
        {"items",
         {{"type", "object"},
          {"properties",
           {{"addr", AddrSchema("start address")}, {"len", AddrSchema("length in bytes")}}}}},
        {"description", "list of {addr, len} ranges"}};

    // --- memory snapshots ---
    tool("snapshot_memory",
         "Record the current contents of emulated memory under a name, for later comparison with "
         "diff_snapshots. With no ranges it captures every writable region, which is what you "
         "want for a blind before/after. Snapshots are held server-side and survive load_state, "
         "so you can snapshot at a menu, restore, make a different choice and snapshot again — "
         "the pair then differ only where that choice was recorded. Distinct from save_state "
         "(a whole machine you can restore) and from trainer_search (which hunts for a value "
         "rather than recording bytes).",
         {{"name", StrSchema("snapshot name (letters, digits, - _ . ; max 64 chars)")},
          {"ranges", rangeArraySchema},
          {"replace", BoolSchema("overwrite an existing snapshot of this name (default true)")}},
         json::array({"name"}));

    tool("diff_snapshots",
         "Compare two memory snapshots and report the contiguous runs of bytes that changed (or, "
         "with mode:\"same\", that did not). stableWith names a third snapshot and narrows the "
         "result to bytes that changed between A and B and then stayed put — which is how you "
         "separate the one byte a menu selection wrote from everything that is merely busy. "
         "minRun/maxRun filter by run length, so maxRun:1 finds isolated single-byte differences. "
         "Refuses if the snapshots were taken over different memory layouts.",
         {{"a", StrSchema("earlier snapshot name")},
          {"b", StrSchema("later snapshot name")},
          {"mode", EnumSchema("changed (default) reports differing bytes; same reports identical "
                              "ones",
                              {"changed", "same"})},
          {"stableWith", StrSchema("third snapshot C: also require B == C over the whole run")},
          {"exclude", rangeArraySchema},
          {"minRun", IntSchema("ignore runs shorter than this (default 1)")},
          {"maxRun", IntSchema("ignore runs longer than this (default 0 = no limit)")},
          {"limit", IntSchema("max runs returned (default 256, 0 = all); totals cover everything")},
          {"includeBytes", BoolSchema("include the before/after bytes (default true)")}},
         json::array({"a", "b"}));

    tool("list_memory_snapshots",
         "Every memory snapshot taken this session, newest first, with the ranges each covers.");

    tool("delete_memory_snapshot", "Forget a memory snapshot.",
         {{"name", StrSchema("snapshot name")}}, json::array({"name"}));

    // --- describe_address ---
    tool("describe_address",
         "Explain what an address actually is, so a raw number from any other tool stops being a "
         "raw number. Reports every layer that applies at once: which memory region it is in, "
         "which custom or CIA register it is, which 68k exception vector (for anything below "
         "$400), which loaded segment and hunk offset, which function from the debug symbols, "
         "which ExecBase field, whether it sits in an allocated or a free chunk of a memory pool, "
         "whose task stack it belongs to, whether it is a library base or one of that library's "
         "jump-table entries (with the LVO), and any label or comment you set earlier.",
         {{"addr", AddrSchema("the address to explain")},
          {"inspectOs", BoolSchema("walk AmigaOS structures (default true; set false when no OS "
                                   "is booted or Exec has been torn down)")}},
         json::array({"addr"}));

    // --- segments ---
    tool("list_segments",
         "Where the running AmigaDOS program's hunks were loaded: the base, size and end of each "
         "segment, plus the program base to subtract from a PC to get a link-time address. Walks "
         "ExecBase -> ThisTask -> pr_CLI -> cli_Module in emulated memory, so it needs a program "
         "started from a booted disk.");

    tool("map_file_to_memory",
         "Pair an AmigaDOS executable's hunks with where they were loaded, and translate between "
         "a runtime address and a file offset in either direction. Defaults to the active "
         "document; point `path` at the executable itself, not at the disk image containing it. "
         "Works on the file alone when nothing is running — the hunk table is reported either "
         "way, just without bases.",
         {{"path", StrSchema("host path of the executable (defaults to the active document)")},
          {"addr", AddrSchema("runtime address to translate to a file offset")},
          {"offset", AddrSchema("file offset to translate to a runtime address")}});

    // --- ADF filesystem ---
    tool("list_disk_files",
         "The AmigaDOS directory tree of the mounted disk image, with each entry's full path, "
         "header block, size, protection bits, comment and date. This reads the image through its "
         "filesystem, unlike read_disk_sectors which reads the emulated drive's raw tracks. Pull "
         "s/startup-sequence and the loader out statically, before booting and again afterwards, "
         "to test whether anything on disk was rewritten.",
         {{"path", StrSchema("only list entries under this directory (default: the whole disk)")},
          {"recursive", BoolSchema("descend into subdirectories (default true)")},
          {"includeDeleted", BoolSchema("also list deleted files still recoverable from the "
                                        "bitmap (default false)")},
          {"limit", IntSchema("max entries returned (default 500, 0 = all)")}});

    tool("read_disk_file",
         "Read a file out of the mounted disk image by path or header block. Returns the content "
         "plus the list of blocks it occupies, so a later find_disk_bytes hit can be tied back to "
         "it. encoding:\"text\" escapes non-printable bytes, which is what you want for a "
         "startup-sequence; use hex or base64 for binaries.",
         {{"path", StrSchema("volume-relative path, e.g. \"s/startup-sequence\"")},
          {"headerBlock", AddrSchema("file header block, as reported by list_disk_files")},
          {"offset", IntSchema("byte offset to start at (default 0)")},
          {"length", IntSchema("max bytes to return (default 0 = the whole file)")},
          {"encoding",
           EnumSchema("how to render the bytes (default text)", {"text", "hex", "base64"})}});

    tool("find_disk_bytes",
         "Search the whole disk image, byte by byte and across block boundaries, and report which "
         "file owns each hit. A match in a block no file claims is the interesting case: that is "
         "a stash written into a spare or free sector. Searches the image directly, so it works "
         "without booting.",
         {{"text", StrSchema("ASCII string to look for")},
          {"pattern", StrSchema("hex digit pairs to look for, e.g. \"4E75\"")},
          {"caseSensitive", BoolSchema("default true; only affects text searches")},
          {"resolveOwners", BoolSchema("resolve the owning file for each hit (default true)")},
          {"limit", IntSchema("max matches returned (default 100, 0 = all)")}});

    tool("which_file_owns",
         "Name the file that owns a block of the mounted image, or say plainly that nothing does. "
         "Reports the block's type, whether the bitmap has it marked free, and the owning file's "
         "path and size. Accepts a block number or a byte offset into the image.",
         {{"block", AddrSchema("512-byte block number")},
          {"offset", AddrSchema("byte offset into the image (converted to a block)")}});

    // --- reset survivors ---
    tool("list_resident_modules",
         "Everything on ExecBase->ResModules: the ROM's own RomTags plus anything added to the "
         "list. Entries reached through the bit-31 redirect are flagged, which is how an injected "
         "module in RAM shows up next to the ROM ones. Each tag's matchword and matchtag are "
         "validated, so a bogus pointer is reported as bogus rather than as a module.");

    tool("get_reboot_survivors",
         "The specific list of things that outlive a reset: the three capture vectors "
         "(ColdCapture/CoolCapture/WarmCapture, flagged when they point outside ROM), KickTagPtr "
         "and KickMemPtr with the tags and memory blocks they name, and any memory pool whose "
         "mh_Upper sits below the end of its region — i.e. memory stolen off the top. Use this "
         "when a program reboots the machine rather than chaining into the DOS boot. Note a "
         "stock boot already shows a few KB missing off the top of a pool (Exec reserves it), so "
         "compare against a clean boot rather than reading any stolenMemory entry as tampering.");

    // --- CPU trace ---
    tool("configure_cpu_trace_history",
         "Turn on the rolling instruction history that get_cpu_trace and find_in_trace search. "
         "The core's own trace buffer only holds the current frame, so without this a provenance "
         "question can only be asked about the frame you are sitting in. Off by default because "
         "each record is about 90 bytes and a traced frame is 14-17k instructions. This is only "
         "the history; the per-instruction trace itself is switched on with set_cpu_trace.",
         {{"enabled", BoolSchema("start (true) or stop (false) recording; starting clears the "
                                 "existing history, stopping keeps it searchable")},
          {"maxRecords", IntSchema("ring capacity in instructions (default 500000, about 30 "
                                   "frames and 44 MB; 1000-5000000)")},
          {"clear", BoolSchema("discard the recorded history")}});

    tool("get_cpu_trace",
         "Page through the recorded instruction history: sequence number, frame, cycle and "
         "beam-relative position, PC and raw bytes, optionally the whole register file. Needs "
         "configure_cpu_trace_history and set_cpu_trace both on, and at least one frame run.",
         {{"frame", IntSchema("only this emulated frame")},
          {"startSeq", IntSchema("begin at this sequence number (default: the oldest kept)")},
          {"limit", IntSchema("max records (default 64, 0 = all)")},
          {"newestFirst", BoolSchema("walk backwards from the most recent (default false)")},
          {"includeRegs", BoolSchema("include D0-D7/A0-A7/SR per record (default false)")}});

    tool("find_in_trace",
         "Search the recorded instruction history. mode:\"filter\" matches on PC range, frame and "
         "a register value. mode:\"accesses\" decodes each instruction's effective address from "
         "the registers captured with it and returns everything that touched a range — \"who "
         "reads this?\" answered after the fact. mode:\"lastWriter\" returns the single most "
         "recent instruction to write the range; pass its seq back as beforeSeq to step to the "
         "writer before that, and keep walking the provenance backwards.",
         {{"mode",
           EnumSchema("which search (default filter)", {"filter", "accesses", "lastWriter"})},
          {"addr", AddrSchema("target address (required for accesses and lastWriter)")},
          {"len", AddrSchema("target length in bytes (default 1)")},
          {"readsOnly", BoolSchema("accesses mode: only reads")},
          {"writesOnly", BoolSchema("accesses mode: only writes")},
          {"beforeSeq", IntSchema("only consider records before this sequence number")},
          {"pcLo", AddrSchema("lowest PC to match (with pcHi)")},
          {"pcHi", AddrSchema("first PC above the range (with pcLo)")},
          {"frame", IntSchema("only this emulated frame")},
          {"reg", StrSchema("register name to test, e.g. \"d0\", \"a6\", \"pc\", \"sp\"")},
          {"regValue", AddrSchema("value that register must hold")},
          {"limit", IntSchema("max records (default 64, 0 = all)")},
          {"newestFirst", BoolSchema("walk backwards (defaults true for address searches)")},
          {"includeRegs", BoolSchema("include the register file per record (default false)")}});

    // --- library calls ---
    tool("trace_library_calls",
         "Every AmigaOS library call in the recorded instruction history, with its arguments "
         "decoded — including the strings they point at — and D0 on return. A call through a "
         "library executes the JMP at its jump-table entry, so the trace's PCs identify the call "
         "and the registers captured alongside give the arguments; nothing is hooked and nothing "
         "is patched. Watch dos.library for Open/Read/Seek/LoadSeg/Execute, and exec.library for "
         "the classic hiding places: AllocAbs, FindPort, FindResident, FindSemaphore, "
         "OpenResource. Needs configure_cpu_trace_history and set_cpu_trace on.",
         {{"library", StrSchema("Amiga node name, e.g. \"dos.library\" (default: every open "
                                "library, device and resource)")},
          {"functions",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "only these function names (default: all)"}}},
          {"sinceSeq", IntSchema("only calls at or after this trace sequence number")},
          {"limit", IntSchema("max calls returned (default 64, 0 = all)")}});

    tool("resolve_source_line",
         "Where a source line's machine code actually is, and whether it has any. This is the "
         "reverse of a symbol lookup, and on an optimised build it is the question that matters: "
         "under -Ofast -flto -fwhole-program many lines generate no code of their own (folded "
         "into a caller) while others generate several disjoint stretches (inlined or unrolled), "
         "so a breakpoint on them either never fires or covers only part of the line.\n"
         "`status` is the useful part: \"exact\" (the line has code), \"moved\" (it does not, "
         "but a later line within the search window does -- `resolvedLine` says which), "
         "\"none\" (the file is covered and this line genuinely produced nothing), "
         "\"file_unknown\" (no such file in this build's line table), or \"file_not_covered\" "
         "(the file is named but its compilation unit could not be read).\n"
         "`sites` lists every address range, link-time and relocated, with `isStmt` marking the "
         "locations the compiler recommends stopping at.",
         {{"file",
           {{"type", "string"},
            {"description", "Source path; matched by trailing components, so a bare "
                            "basename usually works."}}},
          {"line", {{"type", "integer"}, {"description", "1-based source line."}}},
          {"stmtOnly",
           {{"type", "boolean"},
            {"description", "Prefer is_stmt locations (default true). Set false to "
                            "see every address the line touched."}}}},
         {"file", "line"});

    tool("line_coverage",
         "Did this source line execute, in which of its copies, and with what register values? "
         "The retrospective counterpart to a breakpoint, and on a -Ofast -flto -fwhole-program "
         "build usually the only thing that can answer: most lines generate no code of their own "
         "to break on, and those that do are often scattered across several inlined or unrolled "
         "copies, so one breakpoint address covers only part of the line.\n"
         "Every stretch of code the line produced is listed, executed or not -- \"the line ran, "
         "but only two of its four copies did\" is usually the interesting answer, and it is "
         "invisible if unexecuted stretches are dropped. Each carries its hit count, its inline "
         "chain when the line was inlined, and the register file at its first execution.\n"
         "Needs CPU trace history (see configure_cpu_trace_history) and a relocated program; "
         "without either it returns the addresses and says so in `hint` rather than reporting "
         "zero hits, which would read as \"never ran\".",
         {{"file",
           {{"type", "string"},
            {"description", "Source path; matched by trailing components, so a bare "
                            "basename usually works."}}},
          {"line", {{"type", "integer"}, {"description", "1-based source line."}}},
          {"frame",
           {{"type", "integer"},
            {"description", "Only count hits in this emulated frame. Omit or 0 for "
                            "every frame still in the trace."}}}},
         {"file", "line"});

    tool("add_dos_breakpoint",
         "Break when a library function is called, optionally only when a string argument "
         "matches — \"stop when anything opens a file called loader\", without knowing an address "
         "in advance. The breakpoint sits on the library's jump-table entry; on each call the "
         "named argument is read out of emulated memory and execution silently resumes when it "
         "does not match. That costs a host round-trip per call, which is nothing for Open or "
         "LoadSeg and slow if you point it at Read. Needs the library to be open already.",
         {{"function", StrSchema("function name, e.g. \"Open\" or \"LoadSeg\"")},
          {"library", StrSchema("Amiga node name (default \"dos.library\")")},
          {"nameMatch", StrSchema("only break when the function's string argument matches this "
                                  "(default: break on every call)")},
          {"matchMode", EnumSchema("how nameMatch is applied (default contains)",
                                   {"contains", "equals", "prefix", "suffix"})},
          {"caseSensitive", BoolSchema("default false, matching AmigaDOS")},
          {"oneShot", BoolSchema("remove the rule after it stops once (default false)")}},
         json::array({"function"}));

    tool("remove_dos_breakpoint", "Remove a DOS breakpoint by id.",
         {{"id", IntSchema("id from add_dos_breakpoint / list_dos_breakpoints")}},
         json::array({"id"}));

    tool("list_dos_breakpoints",
         "Every DOS breakpoint currently set, with the calls each has seen, how many actually "
         "stopped, and the last string argument observed.");
}

std::optional<json> DispatchMcpReTool(const std::string& name, const json& args,
                                      const McpHost& host)
{
    if (name == "snapshot_memory")
        return HandleSnapshotMemory(args, host);
    if (name == "diff_snapshots")
        return HandleDiffSnapshots(args, host);
    if (name == "list_memory_snapshots")
        return HandleListMemorySnapshots(host);
    if (name == "delete_memory_snapshot")
        return HandleDeleteMemorySnapshot(args, host);
    if (name == "describe_address")
        return HandleDescribeAddress(args, host);
    if (name == "list_segments")
        return HandleListSegments(host);
    if (name == "map_file_to_memory")
        return HandleMapFileToMemory(args, host);
    if (name == "list_disk_files")
        return HandleListDiskFiles(args, host);
    if (name == "read_disk_file")
        return HandleReadDiskFile(args, host);
    if (name == "find_disk_bytes")
        return HandleFindDiskBytes(args, host);
    if (name == "which_file_owns")
        return HandleWhichFileOwns(args, host);
    if (name == "list_resident_modules")
        return HandleListResidentModules(host);
    if (name == "get_reboot_survivors")
        return HandleGetRebootSurvivors(host);
    if (name == "configure_cpu_trace_history")
        return HandleConfigureTraceHistory(args, host);
    if (name == "get_cpu_trace")
        return HandleGetCpuTrace(args, host);
    if (name == "find_in_trace")
        return HandleFindInTrace(args, host);
    if (name == "trace_library_calls")
        return HandleTraceLibraryCalls(args, host);
    if (name == "resolve_source_line") {
        if (!host.resolveSourceLine)
            return Err("no debug line table is loaded (build with -g and launch through "
                       "Peekaboo's VS Code target)");
        const std::string file = args.value("file", std::string{});
        if (file.empty())
            return Err("file is required");
        if (!args.contains("line"))
            return Err("line is required");
        return host.resolveSourceLine(file, args.value("line", 0u), args.value("stmtOnly", true));
    }
    if (name == "line_coverage") {
        if (!host.lineCoverage)
            return Err("line coverage is unavailable (no debug line table loaded)");
        const std::string file = args.value("file", std::string{});
        if (file.empty())
            return Err("file is required");
        if (!args.contains("line"))
            return Err("line is required");
        return host.lineCoverage(file, args.value("line", 0u), args.value("frame", 0ull));
    }
    if (name == "add_dos_breakpoint")
        return HandleAddDosBreakpoint(args, host);
    if (name == "remove_dos_breakpoint")
        return HandleRemoveDosBreakpoint(args, host);
    if (name == "list_dos_breakpoints")
        return HandleListDosBreakpoints(host);
    return std::nullopt;
}

} // namespace adm::core
