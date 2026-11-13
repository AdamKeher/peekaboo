#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace adm::core {

// Exocet Disk Map: a production disk's own layout - a disk
// with no filesystem, whose player finds its data by a directory of its own - read off the ADF by
// the map in the `.exocet`'s "disk" section. The map is data, so a production that changes its
// directory changes its `.exocet`, not Peekaboo. The crass repo's anim production writes it for
// the Sneezeman disk (productions/tvfyb/disk/disk.json, tools/disk/disk_layout.py).
//
//   "disk": {
//     "name", "text", "sector_bytes", "sectors", "sectors_per_track",
//     "regions": [{"name", "first", "count" | "end", "text"}],   numbers, or a header field's name
//     "directory": {"sector", "sectors", "magic", "version", "count": <header field>,
//                   "header": [field], "entry": [field]},
//     "slices": {"zx0_flag", "text"},
//     "kinds": [{"value", "name", "packer": zx0 | lz4 | none, "text"}]
//   }
//   field: {"name", "type": u8 | u16 | u32 | charN, "unit", "role", "text"}
//
// Big-endian, the fields in order from the directory's first byte; the entries follow the
// header. An entry's fields are found by their role: sector, packed, unpacked, kind, sectors.
//
// "slices", when there, is how a packed block is laid out - a run of slices, each a packed-size
// word (with zx0_flag set for ZX0, else LZ4) and an unpacked-size word, then the slice, padded to
// an even length - so each block's packer is read off the disk as well as the map's kind says.

struct ExocetDiskField
{
    std::string name;
    std::string type; // u8, u16, u32, charN
    std::string unit; // bytes, sector, sectors, count, or empty
    std::string role; // an entry's: sector, packed, unpacked, kind, sectors
    std::string text;
    size_t offset = 0;
    size_t size = 0;
    bool isText = false; // charN
};

struct ExocetDiskRegionSpec
{
    std::string name;
    std::string text;
    nlohmann::ordered_json first, count, end; // a number, or a header field's name
};

struct ExocetDiskKind
{
    int value = 0;
    std::string name;
    std::string packer; // zx0, lz4, none; empty: the map does not say
    std::string text;
};

struct ExocetDiskSpec
{
    std::string name;
    std::string text;
    uint32_t sectorBytes = 512;
    uint32_t sectors = 1760;
    uint32_t sectorsPerTrack = 11;
    std::vector<ExocetDiskRegionSpec> regions;
    uint32_t dirSector = 0;
    uint32_t dirSectors = 1;
    std::string magic; // empty: not checked
    int version = -1;  // -1: not checked
    std::string count; // the header field holding the entry count
    std::string magicField = "magic";
    std::string versionField = "version";
    std::vector<ExocetDiskField> header;
    std::vector<ExocetDiskField> entry;
    size_t headerBytes = 0;
    size_t entryBytes = 0;
    bool slices = false;       // blocks are runs of slices, their packer readable per block
    uint16_t zx0Flag = 0x8000; // a slice's packed size with this set: ZX0, else LZ4
    std::string slicesText;
    std::vector<ExocetDiskKind> kinds;

    const ExocetDiskKind* FindKind(int value) const;
    const ExocetDiskField* FindHeader(const std::string& name) const;
};

// The "disk" section parsed; false with `error` set when it is not a map this reads.
bool ParseExocetDiskSpec(const nlohmann::ordered_json& disk, ExocetDiskSpec* out,
                         std::string* error = nullptr);

struct ExocetDiskHeaderValue
{
    const ExocetDiskField* field = nullptr;
    uint32_t value = 0;
    std::string text; // charN, and what a number shows as
};

struct ExocetDiskEntry
{
    int index = 0;
    uint32_t sector = 0, packed = 0, unpacked = 0, sectors = 0;
    int kind = 0;
    std::string kindName;      // "kind N" when the map has no name for it
    uint32_t streamOffset = 0; // its place in its kind's stream: the unpacked bytes before it
    bool fits = true;          // inside the disk, and its sectors hold its packed bytes
    // read off the block: zx0, lz4, mixed (both kinds of slice), none (the stream itself), or ?
    // (not slices that add up to the entry); the map's kind's packer when there are no slices
    std::string packer;
    int slices = 0;       // its slices, when read
    bool packerOk = true; // what the map's kind says, or the map says nothing
};

// A block's packer, read off its bytes by the map's slice layout (ExocetDiskEntry::packer).
std::string ExocetDetectPacker(const ExocetDiskSpec& spec, const uint8_t* block, uint32_t packed,
                               uint32_t unpacked, int* slices = nullptr);

// Consecutive entries of one kind, sector after sector.
struct ExocetDiskRun
{
    int firstEntry = 0, lastEntry = 0;
    int kind = 0;
    std::string kindName;
    uint32_t firstSector = 0, endSector = 0; // end exclusive
    int blocks = 0;
    uint64_t packed = 0, unpacked = 0;
    std::string packer; // its blocks', or mixed when they differ
    bool packerOk = true;
};

struct ExocetDiskRegion
{
    std::string name;
    std::string text;
    uint32_t first = 0, count = 0;
    bool fromEntries = false; // the blocks and the unused tail, worked out, not in the map
};

struct ExocetDiskKindTotal
{
    int kind = 0;
    std::string name, text;
    std::string packer; // the map's
    std::string found;  // the blocks', or mixed when they differ
    int blocks = 0;
    uint32_t sectors = 0;
    uint64_t packed = 0, unpacked = 0;
};

struct ExocetDiskMap
{
    bool ok = false;
    std::string error;                 // why there is no map (ok false)
    std::vector<std::string> warnings; // read, but not what the map says
    std::vector<ExocetDiskRegion> regions;
    std::vector<ExocetDiskHeaderValue> header;
    std::vector<ExocetDiskEntry> entries;
    std::vector<ExocetDiskRun> runs;
    std::vector<ExocetDiskKindTotal> kinds; // by kind value, those present
    uint32_t usedEnd = 0;                   // the first sector past every block
};

// The ADF read by the map. `adf` is the whole image.
ExocetDiskMap ReadExocetDiskMap(const ExocetDiskSpec& spec, const std::vector<uint8_t>& adf);

class ExocetPack;

// The map as the Disk Map window and get_exocet_disk_map read it: the pack's "disk" section over
// the ADF at `diskPath`. `status` says why there is no map (`packError`, when there is no pack).
struct ExocetDiskMapLoad
{
    bool haveSpec = false;
    ExocetDiskSpec spec;
    ExocetDiskMap map;
    std::string status; // empty: `map` is the disk's
};
ExocetDiskMapLoad LoadExocetDiskMap(const ExocetPack* pack, const std::string& packError,
                                    const std::string& diskPath);

} // namespace adm::core
