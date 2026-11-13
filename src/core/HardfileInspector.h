#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adm::core {

// One PART block of a Rigid Disk Block partition table.
struct RdbPartition
{
    uint32_t block = 0; // where the PART block itself lives
    bool checksumValid = false;
    std::string driveName; // DH0, DH1, ... as the RDB names it
    bool bootable = false;
    bool noMount = false;
    uint32_t sizeBlock = 128; // longwords per filesystem block (128 = 512 bytes)
    uint32_t surfaces = 0;
    uint32_t blocksPerTrack = 0;
    uint32_t reserved = 0;
    uint32_t lowCylinder = 0;
    uint32_t highCylinder = 0;
    int32_t bootPriority = 0;
    uint32_t dosType = 0;
    uint64_t firstBlock = 0; // in RDB (512-byte) blocks from the start of the image
    uint64_t blockCount = 0;
};

// One FSHD block: a filesystem the RDB carries for its partitions.
struct RdbFilesystem
{
    uint32_t block = 0;
    uint32_t dosType = 0;
    uint32_t version = 0; // major << 16 | minor
};

struct HardfileInfo
{
    uint64_t sizeBytes = 0;
    bool compressed = false; // .hdz: inspected through gunzip
    bool hasRdb = false;
    uint32_t rdbBlock = 0;
    bool rdbChecksumValid = false;
    uint32_t rdbBlockBytes = 512;
    uint32_t cylinders = 0;
    uint32_t heads = 0;
    uint32_t sectors = 0;
    std::string diskVendor;
    std::string diskProduct;
    std::vector<RdbPartition> partitions;
    std::vector<RdbFilesystem> filesystems;
    // Without an RDB: what block 0 holds, and the geometry UAE would be given.
    uint32_t rawDosType = 0;
    int guessSectors = 32;
    int guessSurfaces = 1;
    int guessReserved = 2;
    int guessBlockSize = 512;
    std::vector<std::string> warnings;
};

class HardfileInspector
{
public:
    // Reads the first blocks of a .hdf (or gunzips them from a .hdz) and decodes the RDB.
    static std::optional<HardfileInfo> Inspect(const std::filesystem::path& path,
                                               std::string& outError);
    // Decodes from memory. `data` needs to hold at least the RDB blocks; `sizeBytes` is the size
    // of the whole image (it drives the no-RDB geometry guess).
    static HardfileInfo InspectBytes(std::span<const uint8_t> data, uint64_t sizeBytes);

    // A whole .hdz decompressed into `out`, up to `maxBytes`. A stream cut short or corrupt is
    // Damaged (with zlib's message in `error`), never a shorter image that looks whole.
    enum class GunzipResult { Ok, CannotOpen, TooLarge, Damaged };
    static GunzipResult Gunzip(const std::filesystem::path& path, uint64_t maxBytes,
                               std::vector<uint8_t>& out, std::string& error);

    // "DOS\3" -> "DOS3", 'PFS\3' -> "PFS3"; unprintable bytes are shown in hex.
    static std::string DosTypeText(uint32_t dosType);
    // "FFS International" for DOS3 and friends; empty when unknown.
    static std::string DosTypeDescription(uint32_t dosType);
};

} // namespace adm::core
