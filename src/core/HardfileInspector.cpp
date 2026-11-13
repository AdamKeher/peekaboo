#include "HardfileInspector.h"

#include <fmt/format.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>

namespace adm::core {
namespace {

// The RDSK block must sit within the first 16 blocks (RDB_LOCATION_LIMIT), and the PART/FSHD
// chains it points at are, in every image seen in practice, inside the first couple of hundred.
constexpr uint32_t kRdbLocationLimit = 16;
constexpr size_t kInspectBytes = 1024 * 512;
constexpr uint32_t kIdRdsk = 0x5244534B; // RDSK
constexpr uint32_t kIdPart = 0x50415254; // PART
constexpr uint32_t kIdFshd = 0x46534844; // FSHD
constexpr uint32_t kEndOfList = 0xFFFFFFFF;

uint32_t Be32(std::span<const uint8_t> data, size_t offset)
{
    if (offset + 4 > data.size())
        return 0;
    return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
           (uint32_t(data[offset + 2]) << 8) | uint32_t(data[offset + 3]);
}

std::span<const uint8_t> Block(std::span<const uint8_t> data, uint64_t block, uint32_t blockBytes)
{
    const uint64_t offset = block * blockBytes;
    if (offset + blockBytes > data.size())
        return {};
    return data.subspan(static_cast<size_t>(offset), blockBytes);
}

// Amiga RDB checksum: the signed sum of `size` longwords (size read from offset 4) is zero.
bool ChecksumValid(std::span<const uint8_t> block)
{
    const uint32_t size = Be32(block, 4);
    // Compared as a count, not as `size * 4`: the product wraps for a corrupt size field of
    // 2^30 or more and the loop below then runs a billion times on the UI thread.
    if (size < 2 || size > block.size() / 4)
        return false;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < size; ++i)
        sum += Be32(block, i * 4u);
    return sum == 0;
}

std::string FixedString(std::span<const uint8_t> block, size_t offset, size_t length)
{
    std::string text;
    for (size_t i = 0; i < length && offset + i < block.size(); ++i) {
        const unsigned char ch = block[offset + i];
        if (ch == 0)
            break;
        text.push_back(std::isprint(ch) ? static_cast<char>(ch) : '?');
    }
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    return text;
}

std::string BcplString(std::span<const uint8_t> block, size_t offset, size_t capacity)
{
    if (offset >= block.size())
        return {};
    const size_t length = std::min<size_t>(block[offset], capacity - 1);
    return FixedString(block, offset + 1, length);
}

void GuessGeometry(HardfileInfo& info)
{
    // Mirrors retro_config_harddrives(): 32 sectors, 2 reserved, 512-byte blocks, one surface
    // below 1 GB and sixteen from there.
    info.guessSectors = 32;
    info.guessReserved = 2;
    info.guessBlockSize = 512;
    info.guessSurfaces = (info.sizeBytes / 1024) >= (1024ull * 1024ull) ? 16 : 1;
}

} // namespace

std::string HardfileInspector::DosTypeText(uint32_t dosType)
{
    std::string text;
    for (int shift = 24; shift >= 8; shift -= 8) {
        const unsigned char ch = static_cast<unsigned char>(dosType >> shift);
        if (!std::isprint(ch))
            return fmt::format("0x{:08X}", dosType);
        text.push_back(static_cast<char>(ch));
    }
    const unsigned char last = static_cast<unsigned char>(dosType);
    if (last < 10)
        text.push_back(static_cast<char>('0' + last));
    else if (std::isprint(last))
        text.push_back(static_cast<char>(last));
    else
        return fmt::format("0x{:08X}", dosType);
    return text;
}

std::string HardfileInspector::DosTypeDescription(uint32_t dosType)
{
    switch (dosType) {
        case 0x444F5300:
            return "OFS";
        case 0x444F5301:
            return "FFS";
        case 0x444F5302:
            return "OFS International";
        case 0x444F5303:
            return "FFS International";
        case 0x444F5304:
            return "OFS DirCache";
        case 0x444F5305:
            return "FFS DirCache";
        case 0x444F5306:
            return "OFS Long Names";
        case 0x444F5307:
            return "FFS Long Names";
        case 0x50465301:
        case 0x50465302:
        case 0x50465303:
            return "Professional File System";
        case 0x53465300:
        case 0x53465302:
            return "Smart File System";
        default:
            return {};
    }
}

HardfileInfo HardfileInspector::InspectBytes(std::span<const uint8_t> data, uint64_t sizeBytes)
{
    HardfileInfo info;
    info.sizeBytes = sizeBytes;
    GuessGeometry(info);

    std::span<const uint8_t> rdsk;
    for (uint32_t block = 0; block < kRdbLocationLimit; ++block) {
        auto candidate = Block(data, block, 512);
        if (candidate.empty())
            break;
        if (Be32(candidate, 0) == kIdRdsk) {
            rdsk = candidate;
            info.rdbBlock = block;
            break;
        }
    }

    if (rdsk.empty()) {
        info.rawDosType = Be32(data, 0);
        return info;
    }

    info.hasRdb = true;
    info.rdbChecksumValid = ChecksumValid(rdsk);
    if (!info.rdbChecksumValid)
        info.warnings.push_back(fmt::format("RDSK block {} has a bad checksum", info.rdbBlock));
    const uint32_t blockBytes = Be32(rdsk, 0x10);
    info.rdbBlockBytes = (blockBytes >= 256 && blockBytes <= 32768) ? blockBytes : 512;
    info.cylinders = Be32(rdsk, 0x40);
    info.sectors = Be32(rdsk, 0x44);
    info.heads = Be32(rdsk, 0x48);
    info.diskVendor = FixedString(rdsk, 0xA0, 8);
    info.diskProduct = FixedString(rdsk, 0xA8, 16);

    // Partition chain. Every link is bounds- and cycle-checked: a damaged table must report,
    // not hang or read past the buffer.
    std::set<uint32_t> visited;
    uint32_t next = Be32(rdsk, 0x1C);
    while (next != kEndOfList && next != 0 && visited.insert(next).second) {
        auto part = Block(data, next, info.rdbBlockBytes);
        if (part.empty()) {
            info.warnings.push_back(
                fmt::format("PART block {} lies outside the inspected area", next));
            break;
        }
        if (Be32(part, 0) != kIdPart) {
            info.warnings.push_back(
                fmt::format("block {} in the partition list is not PART", next));
            break;
        }
        RdbPartition partition;
        partition.block = next;
        partition.checksumValid = ChecksumValid(part);
        if (!partition.checksumValid)
            info.warnings.push_back(fmt::format("PART block {} has a bad checksum", next));
        const uint32_t flags = Be32(part, 0x14);
        partition.bootable = (flags & 1) != 0;
        partition.noMount = (flags & 2) != 0;
        partition.driveName = BcplString(part, 0x24, 32);
        constexpr size_t env = 0x80;
        partition.sizeBlock = Be32(part, env + 1 * 4);
        partition.surfaces = Be32(part, env + 3 * 4);
        partition.blocksPerTrack = Be32(part, env + 5 * 4);
        partition.reserved = Be32(part, env + 6 * 4);
        partition.lowCylinder = Be32(part, env + 9 * 4);
        partition.highCylinder = Be32(part, env + 10 * 4);
        partition.bootPriority = static_cast<int32_t>(Be32(part, env + 15 * 4));
        partition.dosType = Be32(part, env + 16 * 4);
        const uint64_t cylinderBlocks = uint64_t(partition.surfaces) * partition.blocksPerTrack;
        partition.firstBlock = partition.lowCylinder * cylinderBlocks;
        if (partition.highCylinder >= partition.lowCylinder)
            partition.blockCount =
                (uint64_t(partition.highCylinder) - partition.lowCylinder + 1) * cylinderBlocks;
        if (sizeBytes > 0 && (partition.firstBlock + partition.blockCount) * 512 > sizeBytes)
            info.warnings.push_back(
                fmt::format("partition {} extends past the end of the image", partition.driveName));
        info.partitions.push_back(std::move(partition));
        next = Be32(part, 0x10);
    }

    visited.clear();
    next = Be32(rdsk, 0x20);
    while (next != kEndOfList && next != 0 && visited.insert(next).second) {
        auto fshd = Block(data, next, info.rdbBlockBytes);
        if (fshd.empty() || Be32(fshd, 0) != kIdFshd)
            break;
        info.filesystems.push_back({next, Be32(fshd, 0x20), Be32(fshd, 0x24)});
        next = Be32(fshd, 0x10);
    }
    return info;
}

HardfileInspector::GunzipResult HardfileInspector::Gunzip(const std::filesystem::path& path,
                                                          uint64_t maxBytes,
                                                          std::vector<uint8_t>& out,
                                                          std::string& error)
{
    out.clear();
    error.clear();
#ifdef _WIN32
    gzFile file = gzopen_w(path.wstring().c_str(), "rb");
#else
    gzFile file = gzopen(path.string().c_str(), "rb");
#endif
    if (!file)
        return GunzipResult::CannotOpen;
    std::vector<uint8_t> chunk(1 << 20);
    GunzipResult result = GunzipResult::Ok;
    for (;;) {
        const int n = gzread(file, chunk.data(), static_cast<unsigned>(chunk.size()));
        if (n == 0)
            break;
        if (n < 0) {
            result = GunzipResult::Damaged;
            break;
        }
        if (out.size() + static_cast<size_t>(n) > maxBytes) {
            result = GunzipResult::TooLarge;
            break;
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + n);
    }
    int code = Z_OK;
    const char* message = gzerror(file, &code);
    if (result == GunzipResult::Damaged || (result == GunzipResult::Ok && code != Z_OK))
        error = message && *message ? message : "corrupt data";
    if (!error.empty())
        result = GunzipResult::Damaged;
    const bool gzip = !gzdirect(file); // not gzip at all: zlib read the file as it is
    gzclose(file);
    // A stream cut short reads to where it stops and then ends with no error from zlib. The
    // trailer says what the whole image was: its CRC-32 and its size (mod 2^32), in the file's
    // last eight bytes - which, in a cut file, are compressed data instead.
    if (result == GunzipResult::Ok && gzip) {
        uint8_t trailer[8] = {};
        std::ifstream in(path, std::ios::binary);
        in.seekg(-8, std::ios::end);
        in.read(reinterpret_cast<char*>(trailer), sizeof trailer);
        const auto le32 = [&](int at) {
            return uint32_t(trailer[at]) | uint32_t(trailer[at + 1]) << 8 |
                   uint32_t(trailer[at + 2]) << 16 | uint32_t(trailer[at + 3]) << 24;
        };
        uLong crc = crc32(0L, Z_NULL, 0);
        for (size_t at = 0; at < out.size(); at += 1u << 30)
            crc = crc32(crc, out.data() + at,
                        static_cast<uInt>(std::min<size_t>(out.size() - at, 1u << 30)));
        if (!in || le32(0) != static_cast<uint32_t>(crc) ||
            le32(4) != static_cast<uint32_t>(out.size())) {
            result = GunzipResult::Damaged;
            error = "the image is shorter than the file says (cut short, or corrupt)";
        }
    }
    if (result != GunzipResult::Ok)
        out.clear();
    return result;
}

std::optional<HardfileInfo> HardfileInspector::Inspect(const std::filesystem::path& path,
                                                       std::string& outError)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        outError = "not a regular file";
        return std::nullopt;
    }
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    std::vector<uint8_t> head(kInspectBytes);
    uint64_t sizeBytes = 0;
    size_t got = 0;
    const bool compressed = extension == ".hdz";
    if (compressed) {
#ifdef _WIN32
        gzFile file = gzopen_w(path.wstring().c_str(), "rb");
#else
        gzFile file = gzopen(path.string().c_str(), "rb");
#endif
        if (!file) {
            outError = "cannot open the compressed hardfile";
            return std::nullopt;
        }
        // Only the head is decompressed. The uncompressed size comes from the gzip trailer
        // (ISIZE, the last four bytes, little-endian), which is exact below 4 GiB.
        const int n = gzread(file, head.data(), static_cast<unsigned>(head.size()));
        gzclose(file);
        got = n > 0 ? static_cast<size_t>(n) : 0;
        std::ifstream raw(path, std::ios::binary | std::ios::ate);
        const auto compressedSize = static_cast<std::streamoff>(raw.tellg());
        if (raw && compressedSize >= 4) {
            std::array<uint8_t, 4> trailer{};
            raw.seekg(compressedSize - 4);
            raw.read(reinterpret_cast<char*>(trailer.data()), 4);
            sizeBytes = uint64_t(trailer[0]) | (uint64_t(trailer[1]) << 8) |
                        (uint64_t(trailer[2]) << 16) | (uint64_t(trailer[3]) << 24);
        }
        if (sizeBytes < got)
            sizeBytes = got;
    } else {
        sizeBytes = std::filesystem::file_size(path, ec);
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            outError = "cannot open the hardfile";
            return std::nullopt;
        }
        stream.read(reinterpret_cast<char*>(head.data()),
                    static_cast<std::streamsize>(head.size()));
        got = static_cast<size_t>(stream.gcount());
    }
    if (sizeBytes < 512) {
        outError = "the image is smaller than one block";
        return std::nullopt;
    }
    head.resize(got);
    HardfileInfo info = InspectBytes(head, sizeBytes);
    info.compressed = compressed;
    if (sizeBytes % 512 != 0)
        info.warnings.push_back("the image size is not a whole number of 512-byte blocks");
    return info;
}

} // namespace adm::core
